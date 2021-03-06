﻿
#include "atframe/atapp.h"

#include <std/static_assert.h>

#include <libatbus.h>
#include <libatbus_protocol.h>

#include <algorithm/crypto_cipher.h>
#include <algorithm/murmur_hash.h>

#if defined(CRYPTO_USE_OPENSSL) || defined(CRYPTO_USE_LIBRESSL) || defined(CRYPTO_USE_BORINGSSL)
#  include <openssl/ssl.h>
#endif

#include <algorithm/sha.h>
#include <common/file_system.h>
#include <common/string_oprs.h>

#include <cli/shell_font.h>
#include <log/log_sink_file_backend.h>

#include <atframe/atapp_conf_rapidjson.h>
#include <atframe/modules/etcd_module.h>

#include <assert.h>
#include <signal.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#define ATAPP_DEFAULT_STOP_TIMEOUT 30000
#define ATAPP_DEFAULT_TICK_INTERVAL 16

namespace atapp {
app *app::last_instance_ = nullptr;

static void _app_close_timer_handle(uv_handle_t *handle) {
  app::timer_ptr_t *ptr = reinterpret_cast<app::timer_ptr_t *>(handle->data);
  if (nullptr == ptr) {
    if (nullptr != handle->loop) {
      uv_stop(handle->loop);
    }
    return;
  }

  delete ptr;
}

LIBATAPP_MACRO_API app::message_t::message_t()
    : type(0), msg_sequence(0), data(nullptr), data_size(0), metadata(nullptr) {}
LIBATAPP_MACRO_API app::message_t::~message_t() {}

LIBATAPP_MACRO_API app::message_t::message_t(const message_t &other) { (*this) = other; }

LIBATAPP_MACRO_API app::message_t &app::message_t::operator=(const message_t &other) {
  type = other.type;
  msg_sequence = other.msg_sequence;
  data = other.data;
  data_size = other.data_size;
  metadata = other.metadata;
  return *this;
}

LIBATAPP_MACRO_API app::message_sender_t::message_sender_t() : id(0), name(nullptr), remote(nullptr) {}
LIBATAPP_MACRO_API app::message_sender_t::~message_sender_t() {}

LIBATAPP_MACRO_API app::message_sender_t::message_sender_t(const message_sender_t &other) { (*this) = other; }

LIBATAPP_MACRO_API app::message_sender_t &app::message_sender_t::operator=(const message_sender_t &other) {
  id = other.id;
  name = other.name;
  remote = other.remote;
  return *this;
}

LIBATAPP_MACRO_API app::flag_guard_t::flag_guard_t(app &owner, flag_t::type f) : owner_(&owner), flag_(f) {
  if (owner_->check_flag(flag_)) {
    owner_ = nullptr;
    return;
  }

  owner_->set_flag(flag_, true);
}

LIBATAPP_MACRO_API app::flag_guard_t::~flag_guard_t() {
  if (nullptr == owner_) {
    return;
  }

  owner_->set_flag(flag_, false);
}

static std::pair<uint64_t, const char *> make_size_showup(uint64_t sz) {
  const char *unit = "KB";
  if (sz > 102400) {
    sz /= 1024;
    unit = "MB";
  }

  if (sz > 102400) {
    sz /= 1024;
    unit = "GB";
  }

  if (sz > 102400) {
    sz /= 1024;
    unit = "TB";
  }

  return std::pair<uint64_t, const char *>(sz, unit);
}

static uint64_t chrono_to_libuv_duration(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &in,
                                         uint64_t default_value) {
  uint64_t ret = static_cast<uint64_t>(in.seconds() * 1000 + in.nanos() / 1000000);
  if (ret <= 0) {
    ret = default_value;
  }

  return ret;
}

LIBATAPP_MACRO_API app::app() : setup_result_(0), last_proc_event_count_(0), ev_loop_(nullptr), mode_(mode_t::CUSTOM) {
  if (nullptr == last_instance_) {
#if defined(OPENSSL_VERSION_NUMBER)
#  if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
#  else
    OPENSSL_init_ssl(0, nullptr);
#  endif
#endif
    util::crypto::cipher::init_global_algorithm();
  }

  last_instance_ = this;

  memset(pending_signals_, 0, sizeof(pending_signals_));
  conf_.id = 0;
  conf_.execute_path = nullptr;
  conf_.upgrade_mode = false;

  tick_timer_.sec_update = util::time::time_utility::raw_time_t::min();
  tick_timer_.sec = 0;
  tick_timer_.usec = 0;
  tick_timer_.inner_break = nullptr;

  stat_.last_checkpoint_min = 0;
  stat_.endpoint_wake_count = 0;
  stat_.inner_etcd.sum_error_requests = 0;
  stat_.inner_etcd.continue_error_requests = 0;
  stat_.inner_etcd.sum_success_requests = 0;
  stat_.inner_etcd.continue_success_requests = 0;
  stat_.inner_etcd.sum_create_requests = 0;

  atbus_connector_ = add_connector<atapp_connector_atbus>();

  // inner modules
  inner_module_etcd_ = std::make_shared<atapp::etcd_module>();
  add_module(inner_module_etcd_);
}

LIBATAPP_MACRO_API app::~app() {
  endpoint_index_by_id_.clear();
  endpoint_index_by_name_.clear();
  endpoint_waker_.clear();

  if (this == last_instance_) {
    last_instance_ = nullptr;
  }

  for (module_ptr_t &mod : modules_) {
    if (mod && mod->owner_ == this) {
      mod->on_unbind();
      mod->owner_ = nullptr;
    }
  }

  // reset atbus first, make sure atbus ref count is greater than 0 when reset it
  // some inner async deallocate action will add ref count and we should make sure
  // atbus is not destroying
  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }

  assert(!tick_timer_.tick_timer);
  assert(!tick_timer_.timeout_timer);
}

LIBATAPP_MACRO_API int app::run(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
  if (0 != setup_result_) {
    return setup_result_;
  }

  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  if (is_closed()) {
    return EN_ATAPP_ERR_ALREADY_CLOSED;
  }

  if (false == check_flag(flag_t::INITIALIZED)) {
    int res = init(ev_loop, argc, argv, priv_data);
    if (res < 0) {
      return res;
    }
  }

  if (mode_t::START != mode_) {
    return 0;
  }

  int ret = 0;
  while (!is_closed()) {
    ret = run_inner(UV_RUN_DEFAULT);
  }
  return ret;

}  // namespace atapp

LIBATAPP_MACRO_API int app::init(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
  if (check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_ALREADY_INITED;
  }
  setup_result_ = 0;

  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  // update time first
  util::time::time_utility::update();

  // step 1. bind default options
  // step 2. load options from cmd line
  setup_option(argc, argv, priv_data);
  setup_command();

  // step 3. if not in show mode, exit 0
  if (mode_t::INFO == mode_) {
    return 0;
  }
  if (mode_t::HELP == mode_) {
    print_help();
    return 0;
  }

  setup_startup_log();

  // step 4. load options from cmd line
  if (nullptr == ev_loop) {
    ev_loop = uv_default_loop();
  }

  ev_loop_ = ev_loop;
  conf_.bus_conf.ev_loop = ev_loop;
  int ret = reload();
  if (ret < 0) {
    FWLOGERROR("load configure failed");
    return setup_result_ = ret;
  }

  // step 5. if not in start mode, send cmd
  switch (mode_) {
    case mode_t::START: {
      break;
    }
    case mode_t::CUSTOM:
    case mode_t::STOP:
    case mode_t::RELOAD: {
      return send_last_command(ev_loop);
    }
    default: {
      return setup_result_ = 0;
    }
  }

  ret = setup_signal();
  if (ret < 0) {
    FWLOGERROR("setup signal failed");
    write_pidfile();
    return setup_result_ = ret;
  }

  // all modules setup
  for (module_ptr_t &mod : modules_) {
    if (mod->is_enabled()) {
      ret = mod->setup(conf_);
      if (ret < 0) {
        FWLOGERROR("setup module {} failed", mod->name());
        write_pidfile();
        return setup_result_ = ret;
      }
    }
  }

  WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT)->clear_sinks();
  ret = setup_log();
  if (ret < 0) {
    FWLOGERROR("setup log failed");
    write_pidfile();
    return setup_result_ = ret;
  }

  if (setup_timer() < 0) {
    FWLOGERROR("setup timer failed");
    bus_node_.reset();
    write_pidfile();
    return setup_result_ = EN_ATAPP_ERR_SETUP_TIMER;
  }

  ret = setup_atbus();
  if (ret < 0) {
    FWLOGERROR("setup atbus failed");
    bus_node_.reset();
    write_pidfile();
    return setup_result_ = ret;
  }

  // all modules reload
  for (module_ptr_t &mod : modules_) {
    if (mod->is_enabled()) {
      ret = mod->reload();
      if (ret < 0) {
        FWLOGERROR("load configure of {} failed", mod->name());
        write_pidfile();
        return setup_result_ = ret;
      }
    }
  }

  // all modules init
  size_t inited_mod_idx = 0;
  int mod_init_res = 0;
  for (; mod_init_res >= 0 && inited_mod_idx < modules_.size(); ++inited_mod_idx) {
    if (modules_[inited_mod_idx]->is_enabled()) {
      mod_init_res = modules_[inited_mod_idx]->init();
      if (mod_init_res < 0) {
        FWLOGERROR("initialze {} failed", modules_[inited_mod_idx]->name());
        break;
      }
      modules_[inited_mod_idx]->active();
      ++last_proc_event_count_;
    }
  }
  // cleanup all inited modules if failed
  if (mod_init_res < 0) {
    for (; inited_mod_idx < modules_.size(); --inited_mod_idx) {
      if (modules_[inited_mod_idx]) {
        modules_[inited_mod_idx]->cleanup();
      }

      if (0 == inited_mod_idx) {
        break;
      }
    }
    write_pidfile();
    return setup_result_ = mod_init_res;
  }

  // callback of all modules inited
  if (evt_on_all_module_inited_) {
    evt_on_all_module_inited_(*this);
  }

  // write pid file
  if (false == write_pidfile()) {
    return EN_ATAPP_ERR_WRITE_PID_FILE;
  }

  set_flag(flag_t::STOPPED, false);
  set_flag(flag_t::STOPING, false);
  set_flag(flag_t::INITIALIZED, true);
  set_flag(flag_t::RUNNING, true);

  // notify all module to get ready
  for (inited_mod_idx = 0; inited_mod_idx < modules_.size(); ++inited_mod_idx) {
    if (modules_[inited_mod_idx]->is_enabled()) {
      modules_[inited_mod_idx]->ready();
    }
  }

  return EN_ATAPP_ERR_SUCCESS;
}  // namespace atapp

LIBATAPP_MACRO_API int app::run_noblock(uint64_t max_event_count) {
  uint64_t evt_count = 0;
  int ret = 0;
  do {
    ret = run_inner(UV_RUN_NOWAIT);
    if (ret < 0) {
      break;
    }

    if (0 == last_proc_event_count_) {
      break;
    }

    evt_count += last_proc_event_count_;
  } while (0 == max_event_count || evt_count < max_event_count);

  return ret;
}

LIBATAPP_MACRO_API int app::run_once(uint64_t min_event_count, time_t timeout_miliseconds) {
  ev_loop_t *loop = get_evloop();
  if (nullptr == loop) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  uint64_t evt_count = 0;
  int ret = 0;

  util::time::time_utility::raw_time_t timeout;
  if (timeout_miliseconds > 0) {
    util::time::time_utility::update();
    timeout = util::time::time_utility::sys_now() + std::chrono::milliseconds(timeout_miliseconds);
  }

  do {
    if (timeout_miliseconds > 0) {
      if (nullptr == tick_timer_.inner_break) {
        tick_timer_.inner_break = &timeout;
      } else if (timeout < *tick_timer_.inner_break) {
        tick_timer_.inner_break = &timeout;
      }
    }

    ret = run_inner(UV_RUN_ONCE);
    if (ret < 0) {
      break;
    }

    evt_count += last_proc_event_count_;

    if (timeout_miliseconds > 0) {
      util::time::time_utility::update();
      if (timeout <= util::time::time_utility::sys_now()) {
        break;
      }
    }
  } while (evt_count < min_event_count);

  if (&timeout == tick_timer_.inner_break) {
    tick_timer_.inner_break = nullptr;
  }

  return ret;
}

LIBATAPP_MACRO_API bool app::is_inited() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::INITIALIZED); }

LIBATAPP_MACRO_API bool app::is_running() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::RUNNING); }

LIBATAPP_MACRO_API bool app::is_closing() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::STOPING); }

LIBATAPP_MACRO_API bool app::is_closed() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::STOPPED); }

static bool guess_configure_file_is_yaml(const std::string &file_path) {
  std::fstream file;
  file.open(file_path.c_str(), std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    return false;
  }

  std::string line;
  bool is_first_line = true;
  while (std::getline(file, line)) {
    if (is_first_line) {
      const unsigned char utf8_bom[3] = {0xef, 0xbb, 0xbf};
      // Skip UTF-8 BOM
      if (line.size() > 3 && 0 == memcmp(line.c_str(), utf8_bom, 3)) {
        line = line.substr(3);
      }
    }

    const char *begin = line.c_str();
    const char *end = line.c_str() + line.size();
    for (; *begin && begin < end; ++begin) {
      // YAML: Key: Value
      //   ini section and ini key can not contain ':'
      if (*begin == ':') {
        return true;
      }

      // ini section: [SECTION NAME...]
      //   YAML root node of configure file can not be sequence
      if (*begin == '[') {
        return false;
      }

      // ini: Key = Value
      //   YAML key can not contain '='
      if (*begin == '=') {
        return false;
      }
    }
  }

  return false;
}

static bool reload_all_configure_files(app::yaml_conf_map_t &yaml_map, util::config::ini_loader &conf_loader,
                                       atbus::detail::auto_select_set<std::string>::type &loaded_files,
                                       std::list<std::string> &pending_load_files) {
  bool ret = true;
  size_t conf_external_loaded_index;

  while (!pending_load_files.empty()) {
    std::string file_rule = pending_load_files.front();
    pending_load_files.pop_front();

    if (file_rule.empty()) {
      continue;
    }

    std::string::size_type colon = std::string::npos;
    // Skip windows absolute drive
    if (file_rule.size() < 2 || file_rule[1] != ':') {
      colon = file_rule.find(':');
    }
    bool is_yaml = false;

    if (colon != std::string::npos) {
      is_yaml = (0 == UTIL_STRFUNC_STRNCASE_CMP("yml", file_rule.c_str(), colon)) ||
                (0 == UTIL_STRFUNC_STRNCASE_CMP("yaml", file_rule.c_str(), colon));
      file_rule = file_rule.substr(colon + 1);
    }

    if (loaded_files.end() != loaded_files.find(file_rule)) {
      continue;
    }
    loaded_files.insert(file_rule);

    if (colon == std::string::npos) {
      is_yaml = guess_configure_file_is_yaml(file_rule);
    }

    if (is_yaml) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      try {
#endif
        yaml_map[file_rule] = YAML::LoadAllFromFile(file_rule);
        std::vector<YAML::Node> &nodes = yaml_map[file_rule];
        if (nodes.empty()) {
          yaml_map.erase(file_rule);
          continue;
        }

        // external files
        for (size_t i = 0; i < nodes.size(); ++i) {
          const YAML::Node atapp_node = nodes[i]["atapp"];
          if (!atapp_node || !atapp_node.IsMap()) {
            continue;
          }
          const YAML::Node atapp_config = atapp_node["config"];
          if (!atapp_config || !atapp_config.IsMap()) {
            continue;
          }
          const YAML::Node atapp_external = atapp_node["external"];
          if (!atapp_external) {
            continue;
          }

          if (atapp_config.IsSequence()) {
            for (size_t j = 0; j < atapp_config.size(); ++j) {
              const YAML::Node atapp_external_ele = atapp_config[j];
              if (!atapp_external_ele) {
                continue;
              }
              if (!atapp_external_ele.IsScalar()) {
                continue;
              }
              if (atapp_external_ele.Scalar().empty()) {
                continue;
              }
              pending_load_files.push_back(atapp_external_ele.Scalar());
            }
          } else if (atapp_config.IsScalar()) {
            if (!atapp_config.Scalar().empty()) {
              pending_load_files.push_back(atapp_config.Scalar());
            }
          }
        }
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      } catch (YAML::ParserException &e) {
        FWLOGERROR("load configure file {} failed.{}", file_rule, e.what());
        ret = false;
      } catch (YAML::BadSubscript &e) {
        FWLOGERROR("load configure file {} failed.{}", file_rule, e.what());
        ret = false;
      } catch (...) {
        FWLOGERROR("load configure file {} failed.", file_rule);
        ret = false;
      }
#endif
    } else {
      conf_external_loaded_index = conf_loader.get_node("atapp.config.external").size();
      if (conf_loader.load_file(file_rule.c_str(), false) < 0) {
        FWLOGERROR("load configure file {} failed", file_rule);
        ret = false;
        continue;
      } else {
        FWLOGINFO("load configure file {} success", file_rule);
      }

      // external files
      util::config::ini_value &external_paths = conf_loader.get_node("atapp.config.external");
      for (size_t i = conf_external_loaded_index; i < external_paths.size(); ++i) {
        pending_load_files.push_back(external_paths.as_cpp_string(i));
      }
    }
  }

  return ret;
}

LIBATAPP_MACRO_API int app::reload() {
  atbus::adapter::loop_t *old_loop = conf_.bus_conf.ev_loop;
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration old_tick_interval = conf_.origin.timer().tick_interval();

  FWLOGWARNING("============ start to load configure ============");
  // step 1. reset configure
  cfg_loader_.clear();
  yaml_loader_.clear();

  // step 2. reload from program configure file
  if (conf_.conf_file.empty()) {
    FWLOGERROR("missing configure file");
    print_help();
    return EN_ATAPP_ERR_MISSING_CONFIGURE_FILE;
  }

  // load configures
  atbus::detail::auto_select_set<std::string>::type loaded_files;
  std::list<std::string> pending_load_files;
  pending_load_files.push_back(conf_.conf_file);
  if (!reload_all_configure_files(yaml_loader_, cfg_loader_, loaded_files, pending_load_files)) {
    print_help();
    return EN_ATAPP_ERR_LOAD_CONFIGURE_FILE;
  }

  // apply ini configure
  apply_configure();
  // reuse ev loop if not configued
  if (nullptr == conf_.bus_conf.ev_loop) {
    conf_.bus_conf.ev_loop = old_loop;
  }

  // step 5. if not in start mode, return
  if (mode_t::START != mode_) {
    return 0;
  }

  if (is_running()) {
    // step 6. reset log
    setup_log();

    // step 7. if inited, let all modules reload
    for (module_ptr_t &mod : modules_) {
      if (mod->is_enabled()) {
        mod->reload();
      }
    }

    if (inner_module_etcd_) {
      inner_module_etcd_->set_maybe_update_keepalive_value();
    }
  }

  // step 8. if running and tick interval changed, reset timer
  if (old_tick_interval.seconds() != conf_.origin.timer().tick_interval().seconds() ||
      old_tick_interval.nanos() != conf_.origin.timer().tick_interval().nanos()) {
    set_flag(flag_t::RESET_TIMER, true);

    ev_loop_t *loop = get_evloop();
    if (is_running() && nullptr != loop) {
      uv_stop(loop);
    }
  }

  FWLOGWARNING("------------ load configure done ------------");
  return 0;
}

LIBATAPP_MACRO_API int app::stop() {
  if (check_flag(flag_t::STOPING)) {
    FWLOGWARNING("============= recall stop after some event action(s) finished =============");
  } else {
    FWLOGWARNING("============ receive stop signal and ready to stop all modules ============");
  }
  // step 1. set stop flag.
  // bool is_stoping = set_flag(flag_t::STOPING, true);
  set_flag(flag_t::STOPING, true);

  // step 2. stop libuv and return from uv_run
  // if (!is_stoping) {
  ev_loop_t *loop = get_evloop();
  if (bus_node_ && nullptr != loop) {
    uv_stop(loop);
  }
  // }
  return 0;
}

LIBATAPP_MACRO_API int app::tick() {
  int active_count;
  util::time::time_utility::update();
  // record start time point
  util::time::time_utility::raw_time_t start_tp = util::time::time_utility::sys_now();
  util::time::time_utility::raw_time_t end_tp = start_tp;

  std::chrono::system_clock::duration conf_tick_interval =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::seconds(conf_.origin.timer().tick_interval().seconds()));
  conf_tick_interval += std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::nanoseconds(conf_.origin.timer().tick_interval().nanos()));

  do {
    if (tick_timer_.sec != util::time::time_utility::get_sys_now()) {
      tick_timer_.sec = util::time::time_utility::get_sys_now();
      tick_timer_.usec = 0;
      tick_timer_.sec_update = util::time::time_utility::sys_now();
    } else {
      tick_timer_.usec = static_cast<time_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 util::time::time_utility::sys_now() - tick_timer_.sec_update)
                                                 .count());
    }

    active_count = 0;
    int res;
    // step 1. proc available modules
    for (module_ptr_t &mod : modules_) {
      if (mod->is_enabled() && mod->is_actived()) {
        res = mod->tick();
        if (res < 0) {
          FWLOGERROR("module {} run tick and return {}", mod->name(), res);
        } else {
          active_count += res;
        }
      }
    }

    // step 2. proc atbus
    if (bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state()) {
      res = bus_node_->proc(tick_timer_.sec, tick_timer_.usec);
      if (res < 0) {
        FWLOGERROR("atbus run tick and return {}", res);
      } else {
        active_count += res;
      }
    }

    // step 3. proc for pending messages of endpoints
    while (!endpoint_waker_.empty() && endpoint_waker_.begin()->first <= tick_timer_.sec_update) {
      atapp_endpoint::ptr_t ep = endpoint_waker_.begin()->second.lock();
      endpoint_waker_.erase(endpoint_waker_.begin());
      ++stat_.endpoint_wake_count;

      if (ep) {
        res = ep->retry_pending_messages(tick_timer_.sec_update, conf_.origin.bus().loop_times());
        if (res > 0) {
          active_count += res;
        }

        // TODO Support for delay recycle?
        if (!ep->has_connection_handle()) {
          remove_endpoint(ep);
        }
      }
    }

    // only tick time less than tick interval will run loop again
    util::time::time_utility::update();
    end_tp = util::time::time_utility::sys_now();

    if (active_count > 0) {
      last_proc_event_count_ += static_cast<uint64_t>(active_count);
    }
  } while (active_count > 0 && (end_tp - start_tp) < conf_tick_interval);

  ev_loop_t *loop = get_evloop();
  // if is stoping, quit loop  every tick
  if (nullptr != loop) {
    if (check_flag(flag_t::STOPING)) {
      uv_stop(loop);
    } else if (nullptr != tick_timer_.inner_break && tick_timer_.sec_update >= *tick_timer_.inner_break) {
      tick_timer_.inner_break = nullptr;
      uv_stop(loop);
    }
  }

  // stat log
  do {
    time_t now_min = util::time::time_utility::get_sys_now() / util::time::time_utility::MINITE_SECONDS;
    if (now_min != stat_.last_checkpoint_min) {
      time_t last_min = stat_.last_checkpoint_min;
      stat_.last_checkpoint_min = now_min;
      if (last_min + 1 == now_min) {
        uv_rusage_t last_usage;
        memcpy(&last_usage, &stat_.last_checkpoint_usage, sizeof(uv_rusage_t));
        if (0 != uv_getrusage(&stat_.last_checkpoint_usage)) {
          break;
        }
        long offset_usr = stat_.last_checkpoint_usage.ru_utime.tv_sec - last_usage.ru_utime.tv_sec;
        long offset_sys = stat_.last_checkpoint_usage.ru_stime.tv_sec - last_usage.ru_stime.tv_sec;
        offset_usr *= 1000000;
        offset_sys *= 1000000;
        offset_usr += stat_.last_checkpoint_usage.ru_utime.tv_usec - last_usage.ru_utime.tv_usec;
        offset_sys += stat_.last_checkpoint_usage.ru_stime.tv_usec - last_usage.ru_stime.tv_usec;

        std::pair<uint64_t, const char *> max_rss = make_size_showup(last_usage.ru_maxrss);
#ifdef WIN32
        FWLOGINFO(
            "[STATISTICS]: {} CPU usage: user {:02.3}%, sys {:02.3}%, max rss: {}{}, page faults: {}", get_app_name(),
            offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
            offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
            static_cast<unsigned long long>(max_rss.first), max_rss.second,
            static_cast<unsigned long long>(last_usage.ru_majflt));
#else
        std::pair<uint64_t, const char *> ru_ixrss = make_size_showup(last_usage.ru_ixrss);
        std::pair<uint64_t, const char *> ru_idrss = make_size_showup(last_usage.ru_idrss);
        std::pair<uint64_t, const char *> ru_isrss = make_size_showup(last_usage.ru_isrss);
        FWLOGINFO(
            "[STATISTICS]: {} CPU usage: user {:02.3}%, sys {:02.3}%, max rss: {}{}, shared size: {}{}, unshared data "
            "size: "
            "{}{}, unshared "
            "stack size: {}{}, page faults: {}",
            get_app_name(),
            offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
            offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
            static_cast<unsigned long long>(max_rss.first), max_rss.second,
            static_cast<unsigned long long>(ru_ixrss.first), ru_ixrss.second,
            static_cast<unsigned long long>(ru_idrss.first), ru_idrss.second,
            static_cast<unsigned long long>(ru_isrss.first), ru_isrss.second,
            static_cast<unsigned long long>(last_usage.ru_majflt));
        if (inner_module_etcd_) {
          const ::atapp::etcd_cluster::stats_t &current = inner_module_etcd_->get_raw_etcd_ctx().get_stats();
          FWLOGINFO(
              "\tetcd module(last minite): request count: {}, failed request: {}, continue failed: {}, success "
              "request: "
              "{}, continue success request {}",
              current.sum_create_requests - stat_.inner_etcd.sum_create_requests,
              current.sum_error_requests - stat_.inner_etcd.sum_error_requests,
              current.continue_error_requests - stat_.inner_etcd.continue_error_requests,
              current.sum_success_requests - stat_.inner_etcd.sum_success_requests,
              current.continue_success_requests - stat_.inner_etcd.continue_success_requests);
          FWLOGINFO(
              "\tetcd module(sum): request count: {}, failed request: {}, continue failed: {}, success request: "
              "{}, continue success request {}",
              current.sum_create_requests, current.sum_error_requests, current.continue_error_requests,
              current.sum_success_requests, current.continue_success_requests);
          stat_.inner_etcd = current;
        }

        FWLOGINFO("\tendpoint wake count: {}, by_id index size: {}, by_name index size: {}, waker size: {}",
                  stat_.endpoint_wake_count, endpoint_index_by_id_.size(), endpoint_index_by_name_.size(),
                  endpoint_waker_.size());
        stat_.endpoint_wake_count = 0;
#endif
      } else {
        uv_getrusage(&stat_.last_checkpoint_usage);
        if (inner_module_etcd_) {
          stat_.inner_etcd = inner_module_etcd_->get_raw_etcd_ctx().get_stats();
        }
      }

      if (nullptr != loop) {
        uv_stop(loop);
      }
    }
  } while (false);
  return 0;
}

LIBATAPP_MACRO_API app::app_id_t app::get_id() const { return conf_.id; }

LIBATAPP_MACRO_API app::ev_loop_t *app::get_evloop() { return ev_loop_; }

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in) const {
  return convert_app_id_by_string(id_in, conf_.id_mask);
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, bool hex) const {
  return convert_app_id_to_string(id_in, conf_.id_mask, hex);
}

LIBATAPP_MACRO_API void app::add_module(module_ptr_t app_module) {
  if (this == app_module->owner_) {
    return;
  }

  assert(nullptr == app_module->owner_);
  if (nullptr == app_module->owner_) {
    modules_.push_back(app_module);
    app_module->owner_ = this;
    app_module->on_bind();
  }
}

LIBATAPP_MACRO_API util::cli::cmd_option_ci::ptr_type app::get_command_manager() {
  if (!cmd_handler_) {
    return cmd_handler_ = util::cli::cmd_option_ci::create();
  }

  return cmd_handler_;
}

LIBATAPP_MACRO_API util::cli::cmd_option::ptr_type app::get_option_manager() {
  if (!app_option_) {
    return app_option_ = util::cli::cmd_option::create();
  }

  return app_option_;
}

LIBATAPP_MACRO_API bool app::is_current_upgrade_mode() const { return conf_.upgrade_mode; }

LIBATAPP_MACRO_API util::network::http_request::curl_m_bind_ptr_t app::get_shared_curl_multi_context() const {
  if (likely(inner_module_etcd_)) {
    return inner_module_etcd_->get_shared_curl_multi_context();
  }

  return nullptr;
}

LIBATAPP_MACRO_API void app::set_app_version(const std::string &ver) { conf_.app_version = ver; }

LIBATAPP_MACRO_API const std::string &app::get_app_version() const { return conf_.app_version; }

LIBATAPP_MACRO_API void app::set_build_version(const std::string &ver) { build_version_ = ver; }

LIBATAPP_MACRO_API const std::string &app::get_build_version() const {
  if (build_version_.empty()) {
    std::stringstream ss;
    if (get_app_version().empty()) {
      ss << "1.0.0.0 - based on libatapp " << LIBATAPP_VERSION << std::endl;
    } else {
      ss << get_app_version() << " - based on libatapp " << LIBATAPP_VERSION << std::endl;
    }

    const size_t key_padding = 20;

#ifdef __DATE__
    ss << std::setw(key_padding) << "Build Time: " << __DATE__;
#  ifdef __TIME__
    ss << " " << __TIME__;
#  endif
    ss << std::endl;
#endif

#if defined(PROJECT_SCM_VERSION) || defined(PROJECT_SCM_NAME) || defined(PROJECT_SCM_BRANCH)
    ss << std::setw(key_padding) << "Build SCM:";
#  ifdef PROJECT_SCM_NAME
    ss << " " << PROJECT_SCM_NAME;
#  endif
#  ifdef PROJECT_SCM_BRANCH
    ss << " branch " << PROJECT_SCM_BRANCH;
#  endif
#  ifdef PROJECT_SCM_VERSION
    ss << " commit " << PROJECT_SCM_VERSION;
#  endif
#endif

#if defined(_MSC_VER)
    ss << std::setw(key_padding) << "Build Compiler: MSVC ";
#  ifdef _MSC_FULL_VER
    ss << _MSC_FULL_VER;
#  else
    ss << _MSC_VER;
#  endif

#  ifdef _MSVC_LANG
    ss << " with standard " << _MSVC_LANG;
#  endif
    ss << std::endl;

#elif defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
    ss << std::setw(key_padding) << "Build Compiler: ";
#  if defined(__clang__)
    ss << "clang ";
#  else
    ss << "gcc ";
#  endif

#  if defined(__clang_version__)
    ss << __clang_version__;
#  elif defined(__VERSION__)
    ss << __VERSION__;
#  endif
#  if defined(__OPTIMIZE__)
    ss << " optimize level " << __OPTIMIZE__;
#  endif
#  if defined(__STDC_VERSION__)
    ss << " C standard " << __STDC_VERSION__;
#  endif
#  if defined(__cplusplus) && __cplusplus > 1
    ss << " C++ standard " << __cplusplus;
#  endif
    ss << std::endl;
#endif

    build_version_ = ss.str();
  }

  return build_version_;
}

LIBATAPP_MACRO_API const std::string &app::get_app_name() const { return conf_.origin.name(); }

LIBATAPP_MACRO_API const std::string &app::get_app_identity() const { return conf_.origin.identity(); }

LIBATAPP_MACRO_API const std::string &app::get_type_name() const { return conf_.origin.type_name(); }

LIBATAPP_MACRO_API app::app_id_t app::get_type_id() const { return static_cast<app::app_id_t>(conf_.origin.type_id()); }

LIBATAPP_MACRO_API const std::string &app::get_hash_code() const { return conf_.hash_code; }

LIBATAPP_MACRO_API std::shared_ptr<atbus::node> app::get_bus_node() { return bus_node_; }
LIBATAPP_MACRO_API const std::shared_ptr<atbus::node> app::get_bus_node() const { return bus_node_; }

LIBATAPP_MACRO_API void app::enable_fallback_to_atbus_connector() { set_flag(flag_t::DISABLE_ATBUS_FALLBACK, false); }

LIBATAPP_MACRO_API void app::disable_fallback_to_atbus_connector() { set_flag(flag_t::DISABLE_ATBUS_FALLBACK, true); }

LIBATAPP_MACRO_API bool app::is_fallback_to_atbus_connector_enabled() const {
  return !check_flag(flag_t::DISABLE_ATBUS_FALLBACK);
}

LIBATAPP_MACRO_API util::time::time_utility::raw_time_t app::get_last_tick_time() const {
  return tick_timer_.sec_update;
}

LIBATAPP_MACRO_API util::config::ini_loader &app::get_configure_loader() { return cfg_loader_; }
LIBATAPP_MACRO_API const util::config::ini_loader &app::get_configure_loader() const { return cfg_loader_; }

LIBATAPP_MACRO_API app::yaml_conf_map_t &app::get_yaml_loaders() { return yaml_loader_; }
LIBATAPP_MACRO_API const app::yaml_conf_map_t &app::get_yaml_loaders() const { return yaml_loader_; }

LIBATAPP_MACRO_API void app::parse_configures_into(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                   const std::string &path) const {
  if (!path.empty()) {
    util::config::ini_value::ptr_t cfg_value = cfg_loader_.get_root_node().get_child_by_path(path);
    if (cfg_value) {
      ini_loader_dump_to(*cfg_value, dst);
    }
  } else {
    ini_loader_dump_to(cfg_loader_.get_root_node(), dst);
  }

  for (yaml_conf_map_t::const_iterator iter = yaml_loader_.begin(); iter != yaml_loader_.end(); ++iter) {
    for (size_t i = 0; i < iter->second.size(); ++i) {
      yaml_loader_dump_to(yaml_loader_get_child_by_path(iter->second[i], path), dst);
    }
  }
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_configure &app::get_origin_configure() const { return conf_.origin; }

LIBATAPP_MACRO_API const atapp::protocol::atapp_metadata &app::get_metadata() const { return conf_.metadata; }

LIBATAPP_MACRO_API atapp::protocol::atapp_metadata &app::mutable_metadata() {
  if (inner_module_etcd_) {
    inner_module_etcd_->set_maybe_update_keepalive_value();
  }
  return conf_.metadata;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_area &app::get_area() const { return conf_.origin.area(); }

LIBATAPP_MACRO_API atapp::protocol::atapp_area &app::mutable_area() {
  if (inner_module_etcd_) {
    inner_module_etcd_->set_maybe_update_keepalive_value();
  }
  return *conf_.origin.mutable_area();
}

LIBATAPP_MACRO_API util::time::time_utility::raw_time_t::duration app::get_configure_message_timeout() const {
  const google::protobuf::Duration &dur = conf_.origin.timer().message_timeout();
  if (dur.seconds() <= 0 && dur.nanos() <= 0) {
    return std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(5));
  }

  util::time::time_utility::raw_time_t::duration ret =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(dur.seconds()));
  ret += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(dur.nanos()));
  return ret;
}

LIBATAPP_MACRO_API void app::pack(atapp::protocol::atapp_discovery &out) const {
  out.set_id(get_id());
  out.set_name(get_app_name());
  out.set_hostname(atbus::node::get_hostname());
  out.set_pid(atbus::node::get_pid());
  out.set_identity(get_app_identity());

  out.set_hash_code(get_hash_code());
  out.set_type_id(get_type_id());
  out.set_type_name(get_type_name());
  if (conf_.origin.has_area()) {
    out.mutable_area()->CopyFrom(conf_.origin.area());
  }
  out.set_version(get_app_version());
  // out.set_custom_data(get_conf_custom_data());
  out.mutable_gateways()->Reserve(conf_.origin.bus().gateways().size());
  for (int i = 0; i < conf_.origin.bus().gateways().size(); ++i) {
    atapp::protocol::atapp_gateway *gateway = out.add_gateways();
    if (nullptr != gateway) {
      gateway->CopyFrom(conf_.origin.bus().gateways(i));
    }
  }

  out.mutable_metadata()->CopyFrom(get_metadata());

  if (bus_node_) {
    const std::vector<atbus::endpoint_subnet_conf> &subnets = bus_node_->get_conf().subnets;
    for (size_t i = 0; i < subnets.size(); ++i) {
      atapp::protocol::atbus_subnet_range *subset = out.add_atbus_subnets();
      if (nullptr == subset) {
        FWLOGERROR("pack configures for {}(0x{:x}) but malloc atbus_subnet_range failed", get_app_name(), get_id());
        break;
      }

      subset->set_id_prefix(subnets[i].id_prefix);
      subset->set_mask_bits(subnets[i].mask_bits);
    }

    out.mutable_listen()->Reserve(static_cast<int>(bus_node_->get_listen_list().size()));
    // std::list<std::string>
    for (std::list<std::string>::const_iterator iter = bus_node_->get_listen_list().begin();
         iter != bus_node_->get_listen_list().end(); ++iter) {
      out.add_listen(*iter);
    }
    out.set_atbus_protocol_version(bus_node_->get_protocol_version());
    out.set_atbus_protocol_min_version(bus_node_->get_protocol_minimal_version());
  } else {
    out.mutable_listen()->Reserve(conf_.origin.bus().listen().size());
    for (int i = 0; i < conf_.origin.bus().listen().size(); ++i) {
      out.add_listen(conf_.origin.bus().listen(i));
    }
    out.set_atbus_protocol_version(atbus::protocol::ATBUS_PROTOCOL_VERSION);
    out.set_atbus_protocol_min_version(atbus::protocol::ATBUS_PROTOCOL_MINIMAL_VERSION);
  }
}

LIBATAPP_MACRO_API std::shared_ptr< ::atapp::etcd_module> app::get_etcd_module() const { return inner_module_etcd_; }

LIBATAPP_MACRO_API uint32_t app::get_address_type(const std::string &address) const {
  uint32_t ret = address_type_t::EN_ACAT_NONE;

  atbus::channel::channel_address_t addr;
  atbus::channel::make_address(address.c_str(), addr);
  std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

  connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
  if (iter == connector_protocols_.end()) {
    return ret;
  }

  if (!iter->second) {
    return ret;
  }

  return iter->second->get_address_type(addr);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t app::get_discovery_node_by_id(uint64_t id) const {
  if (!inner_module_etcd_) {
    return nullptr;
  }

  return inner_module_etcd_->get_global_discovery().get_node_by_id(id);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t app::get_discovery_node_by_name(const std::string &name) const {
  if (!inner_module_etcd_) {
    return nullptr;
  }

  return inner_module_etcd_->get_global_discovery().get_node_by_name(name);
}

LIBATAPP_MACRO_API int32_t app::listen(const std::string &address) {
  atbus::channel::channel_address_t addr;
  atbus::channel::make_address(address.c_str(), addr);
  std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

  connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
  if (iter == connector_protocols_.end()) {
    return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
  }

  if (!iter->second) {
    return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
  }

  return iter->second->on_start_listen(addr);
}

LIBATAPP_MACRO_API int32_t app::send_message(uint64_t target_node_id, int32_t type, const void *data, size_t data_size,
                                             uint64_t *msg_sequence, const atapp::protocol::atapp_metadata *metadata) {
  // Find from cache
  do {
    atapp_endpoint *cache = get_endpoint(target_node_id);
    if (nullptr == cache) {
      break;
    }

    int32_t ret;
    if (nullptr != msg_sequence) {
      ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
    } else {
      uint64_t msg_seq = 0;
      ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
    }
    return ret;
  } while (false);

  // Try to create endpoint from discovery
  do {
    if (!inner_module_etcd_) {
      break;
    }

    etcd_discovery_node::ptr_t node = inner_module_etcd_->get_global_discovery().get_node_by_id(target_node_id);
    if (!node) {
      break;
    }

    return send_message(node, type, data, data_size, msg_sequence, metadata);
  } while (false);

  // Fallback to old atbus connector
  if (check_flag(flag_t::DISABLE_ATBUS_FALLBACK)) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }
  if (!bus_node_) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  return bus_node_->send_data(target_node_id, type, data, data_size, msg_sequence);
}

LIBATAPP_MACRO_API int32_t app::send_message(const std::string &target_node_name, int32_t type, const void *data,
                                             size_t data_size, uint64_t *msg_sequence,
                                             const atapp::protocol::atapp_metadata *metadata) {
  do {
    atapp_endpoint *cache = get_endpoint(target_node_name);
    if (nullptr == cache) {
      break;
    }

    int32_t ret;
    if (nullptr != msg_sequence) {
      ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
    } else {
      uint64_t msg_seq = 0;
      ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
    }
    return ret;
  } while (false);

  // Try to create endpoint from discovery
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  etcd_discovery_node::ptr_t node = inner_module_etcd_->get_global_discovery().get_node_by_name(target_node_name);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message(const etcd_discovery_node::ptr_t &target_node_discovery, int32_t type,
                                             const void *data, size_t data_size, uint64_t *msg_sequence,
                                             const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATBUS_ERR_PARAMS;
  }

  atapp_endpoint::ptr_t cache = mutable_endpoint(target_node_discovery);
  if (!cache) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  int32_t ret;
  if (nullptr != msg_sequence) {
    ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
  } else {
    uint64_t msg_seq = 0;
    ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
  }
  return ret;
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(inner_module_etcd_->get_global_discovery(), hash_buf, hash_bufsz, type, data,
                                         data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(uint64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(inner_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(int64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(inner_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const std::string &hash_key, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(inner_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_random(int32_t type, const void *data, size_t data_size,
                                                       uint64_t *msg_sequence,
                                                       const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_random(inner_module_etcd_->get_global_discovery(), type, data, data_size, msg_sequence,
                                metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_round_robin(int32_t type, const void *data, size_t data_size,
                                                            uint64_t *msg_sequence,
                                                            const atapp::protocol::atapp_metadata *metadata) {
  if (!inner_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_round_robin(inner_module_etcd_->get_global_discovery(), type, data, data_size, msg_sequence,
                                     metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_buf, hash_bufsz);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                uint64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                int64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                const std::string &hash_key, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_random(const etcd_discovery_set &discovery_set, int32_t type,
                                                       const void *data, size_t data_size, uint64_t *msg_sequence,
                                                       const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_random();
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_round_robin(const etcd_discovery_set &discovery_set, int32_t type,
                                                            const void *data, size_t data_size, uint64_t *msg_sequence,
                                                            const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_round_robin();
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API bool app::add_log_sink_maker(const std::string &name, log_sink_maker::log_reg_t fn) {
  if (log_reg_.end() != log_reg_.find(name)) {
    return false;
  }

  log_reg_[name] = fn;
  return true;
}

LIBATAPP_MACRO_API void app::set_evt_on_forward_request(callback_fn_on_forward_request_t fn) {
  evt_on_forward_request_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_forward_response(callback_fn_on_forward_response_t fn) {
  evt_on_forward_response_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_app_connected(callback_fn_on_connected_t fn) { evt_on_app_connected_ = fn; }
LIBATAPP_MACRO_API void app::set_evt_on_app_disconnected(callback_fn_on_disconnected_t fn) {
  evt_on_app_disconnected_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_all_module_inited(callback_fn_on_all_module_inited_t fn) {
  evt_on_all_module_inited_ = fn;
}

LIBATAPP_MACRO_API const app::callback_fn_on_forward_request_t &app::get_evt_on_forward_request() const {
  return evt_on_forward_request_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_forward_response_t &app::get_evt_on_forward_response() const {
  return evt_on_forward_response_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_connected_t &app::get_evt_on_app_connected() const {
  return evt_on_app_connected_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_disconnected_t &app::get_evt_on_app_disconnected() const {
  return evt_on_app_disconnected_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_all_module_inited_t &app::get_evt_on_all_module_inited() const {
  return evt_on_all_module_inited_;
}

LIBATAPP_MACRO_API bool app::add_endpoint_waker(util::time::time_utility::raw_time_t wakeup_time,
                                                const atapp_endpoint::weak_ptr_t &ep_watcher) {
  if (is_closing()) {
    return false;
  }

  endpoint_waker_.insert(
      std::pair<const util::time::time_utility::raw_time_t, atapp_endpoint::weak_ptr_t>(wakeup_time, ep_watcher));
  return true;
}

LIBATAPP_MACRO_API void app::remove_endpoint(uint64_t by_id) {
  endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id == endpoint_index_by_id_.end()) {
    return;
  }

  atapp_endpoint::ptr_t res = iter_id->second;
  endpoint_index_by_id_.erase(iter_id);

  std::string name;
  if (res) {
    name = res->get_name();
  }
  if (!name.empty()) {
    endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(name);
    if (iter_name != endpoint_index_by_name_.end() && iter_name->second == res) {
      endpoint_index_by_name_.erase(iter_name);
    }
  }

  // RAII destruction of res
}

LIBATAPP_MACRO_API void app::remove_endpoint(const std::string &by_name) {
  endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name == endpoint_index_by_name_.end()) {
    return;
  }

  atapp_endpoint::ptr_t res = iter_name->second;
  endpoint_index_by_name_.erase(iter_name);

  uint64_t id = 0;
  if (res) {
    id = res->get_id();
  }
  if (id != 0) {
    endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
    if (iter_id != endpoint_index_by_id_.end() && iter_id->second == res) {
      endpoint_index_by_id_.erase(iter_id);
    }
  }

  // RAII destruction of res
}

LIBATAPP_MACRO_API void app::remove_endpoint(const atapp_endpoint::ptr_t &enpoint) {
  if (!enpoint) {
    return;
  }

  {
    uint64_t id = enpoint->get_id();
    if (id != 0) {
      endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
      if (iter_id != endpoint_index_by_id_.end() && iter_id->second == enpoint) {
        endpoint_index_by_id_.erase(iter_id);
      }
    }
  }

  {
    std::string name = enpoint->get_name();
    if (!name.empty()) {
      endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(name);
      if (iter_name != endpoint_index_by_name_.end() && iter_name->second == enpoint) {
        endpoint_index_by_name_.erase(iter_name);
      }
    }
  }
}

LIBATAPP_MACRO_API atapp_endpoint::ptr_t app::mutable_endpoint(const etcd_discovery_node::ptr_t &discovery) {
  if (is_closing()) {
    return nullptr;
  }

  if (!discovery) {
    return nullptr;
  }

  uint64_t id = discovery->get_discovery_info().id();
  const std::string &name = discovery->get_discovery_info().name();
  atapp_endpoint::ptr_t ret;
  bool is_created = false;
  bool need_update_id_index = false;
  bool need_update_name_index = false;
  do {
    if (0 == id) {
      break;
    }

    endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
    if (iter_id != endpoint_index_by_id_.end()) {
      ret = iter_id->second;
    }

    need_update_id_index = !ret;
  } while (false);

  do {
    if (name.empty()) {
      break;
    }

    endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(name);
    if (iter_name != endpoint_index_by_name_.end()) {
      if (ret && iter_name->second == ret) {
        break;
      }
      if (ret) {
        remove_endpoint(id);
        need_update_id_index = true;
      }
      ret = iter_name->second;
      need_update_name_index = !ret;
    } else {
      need_update_name_index = true;
    }
  } while (false);

  if (!ret) {
    ret = atapp_endpoint::create(*this);
    is_created = !!ret;
  }
  if (ret) {
    if (need_update_id_index) {
      endpoint_index_by_id_[id] = ret;
    }
    if (need_update_name_index) {
      endpoint_index_by_name_[name] = ret;
    }
    ret->update_discovery(discovery);
  }

  // Wake and maybe it's should be cleanup if it's a new endpoint
  if (is_created && ret) {
    ret->add_waker(get_last_tick_time());
    atapp_connection_handle::ptr_t handle = std::make_shared<atapp_connection_handle>();

    int32_t gateway_size = discovery->get_ingress_size();
    for (int32_t i = 0; handle && i < gateway_size; ++i) {
      atbus::channel::channel_address_t addr;
      const atapp::protocol::atapp_gateway &gateway = discovery->next_ingress_gateway();
      if (!match_gateway(gateway)) {
        FWLOGDEBUG("atapp endpoint {}({}) skip unmatched gateway {}", ret->get_id(), ret->get_name(),
                   gateway.address());
        continue;
      }
      atbus::channel::make_address(gateway.address().c_str(), addr);
      std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

      connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
      if (iter == connector_protocols_.end()) {
        FWLOGDEBUG("atapp endpoint {}({}) skip unsupported address {}", ret->get_id(), ret->get_name(), addr.address);
        continue;
      }

      if (!iter->second) {
        FWLOGDEBUG("atapp endpoint {}({}) skip unsupported address {}", ret->get_id(), ret->get_name(), addr.address);
        continue;
      }

      int res = iter->second->on_start_connect(discovery.get(), addr, handle);
      if (0 == res && handle.use_count() > 1) {
        atapp_connector_bind_helper::bind(*handle, *iter->second);
        atapp_endpoint_bind_helper::bind(*handle, *ret);

        FWLOGINFO("atapp endpoint {}({}) connect address {} success and use handle {}", ret->get_id(), ret->get_name(),
                  addr.address, reinterpret_cast<const void *>(handle.get()));
        break;
      } else {
        FWLOGINFO("atapp endpoint {}({}) skip address {} with handle {}", ret->get_id(), ret->get_name(), addr.address,
                  reinterpret_cast<const void *>(handle.get()));
      }
    }
  }

  return ret;
}

LIBATAPP_MACRO_API atapp_endpoint *app::get_endpoint(uint64_t by_id) {
  endpoint_index_by_id_t::iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id != endpoint_index_by_id_.end()) {
    return iter_id->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API const atapp_endpoint *app::get_endpoint(uint64_t by_id) const {
  endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id != endpoint_index_by_id_.end()) {
    return iter_id->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API atapp_endpoint *app::get_endpoint(const std::string &by_name) {
  endpoint_index_by_name_t::iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name != endpoint_index_by_name_.end()) {
    return iter_name->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API const atapp_endpoint *app::get_endpoint(const std::string &by_name) const {
  endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name != endpoint_index_by_name_.end()) {
    return iter_name->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API bool app::match_gateway(const atapp::protocol::atapp_gateway &checked) const {
  if (checked.address().empty()) {
    return false;
  }

  if (checked.match_hosts_size() > 0 && !match_gateway_hosts(checked)) {
    return false;
  }

  if (checked.match_namespaces_size() && !match_gateway_namespace(checked)) {
    return false;
  }

  if (checked.match_labels_size() > 0 && !match_gateway_labels(checked)) {
    return false;
  }

  return true;
}

void app::ev_stop_timeout(uv_timer_t *handle) {
  assert(handle && handle->data);

  if (nullptr != handle && nullptr != handle->data) {
    app *self = reinterpret_cast<app *>(handle->data);
    self->set_flag(flag_t::TIMEOUT, true);
  }

  if (nullptr != handle) {
    uv_stop(handle->loop);
  }
}

bool app::set_flag(flag_t::type f, bool v) {
  if (f < 0 || f >= flag_t::FLAG_MAX) {
    return false;
  }

  bool ret = flags_.test(f);
  flags_.set(f, v);
  return ret;
}

LIBATAPP_MACRO_API bool app::check_flag(flag_t::type f) const {
  if (f < 0 || f >= flag_t::FLAG_MAX) {
    return false;
  }

  return flags_.test(f);
}

int app::apply_configure() {
  std::string old_name = conf_.origin.name();
  std::string old_hostname = conf_.origin.hostname();
  std::string old_identity = conf_.origin.identity();
  parse_configures_into(conf_.origin, "atapp");

  // id and id mask
  if (conf_.id_mask.empty()) {
    split_ids_by_string(conf_.origin.id_mask().c_str(), conf_.id_mask);
  }

  if (!conf_.id_cmd.empty()) {
    conf_.id = convert_app_id_by_string(conf_.id_cmd.c_str());
  }

  if (0 == conf_.id) {
    conf_.id = convert_app_id_by_string(conf_.origin.id().c_str());
  }

  // Changing name is not allowed
  if (!old_name.empty()) {
    conf_.origin.set_name(old_name);
  } else if (conf_.origin.name().empty()) {
    conf_.origin.set_name(LOG_WRAPPER_FWAPI_FORMAT("{}-0x{:x}", conf_.origin.type_name(), conf_.id));
  }

  {
    uint64_t hash_out[2];
    ::util::hash::murmur_hash3_x64_128(conf_.origin.name().c_str(), static_cast<int>(conf_.origin.name().size()),
                                       LIBATAPP_MACRO_HASH_MAGIC_NUMBER, hash_out);
    conf_.hash_code = LOG_WRAPPER_FWAPI_FORMAT("{:016X}{:016X}", hash_out[0], hash_out[1]);
  }

  // Changing hostname is not allowed
  if (!old_hostname.empty()) {
    conf_.origin.set_hostname(old_hostname);
  }

  if (!conf_.origin.hostname().empty()) {
    atbus::node::set_hostname(conf_.origin.hostname());
  }

  // Changing identity is not allowed
  if (!old_identity.empty()) {
    conf_.origin.set_identity(old_identity);
  }
  if (conf_.origin.identity().empty()) {
    std::stringstream identity_stream;
    identity_stream << util::file_system::get_abs_path(conf_.execute_path) << std::endl;
    identity_stream << util::file_system::get_abs_path(conf_.conf_file.c_str()) << std::endl;
    identity_stream << "id: " << conf_.id << std::endl;
    identity_stream << "name: " << conf_.origin.name() << std::endl;
    identity_stream << "hostname: " << conf_.origin.hostname() << std::endl;
    std::string identity_buffer = identity_stream.str();
    conf_.origin.set_identity(util::hash::sha::hash_to_hex(util::hash::sha::EN_ALGORITHM_SHA256,
                                                           identity_buffer.c_str(), identity_buffer.size()));
  }

  // atbus configure
  atbus::node::default_conf(&conf_.bus_conf);

  {
    conf_.bus_conf.subnets.reserve(static_cast<size_t>(conf_.origin.bus().subnets_size()));
    for (int i = 0; i < conf_.origin.bus().subnets_size(); ++i) {
      const std::string &subset = conf_.origin.bus().subnets(i);
      std::string::size_type sep_pos = subset.find('/');
      if (std::string::npos == sep_pos) {
        conf_.bus_conf.subnets.push_back(
            atbus::endpoint_subnet_conf(0, util::string::to_int<uint32_t>(subset.c_str())));
      } else {
        conf_.bus_conf.subnets.push_back(atbus::endpoint_subnet_conf(
            convert_app_id_by_string(subset.c_str()), util::string::to_int<uint32_t>(subset.c_str() + sep_pos + 1)));
      }
    }
  }

  conf_.bus_conf.parent_address = conf_.origin.bus().proxy();
  conf_.bus_conf.loop_times = conf_.origin.bus().loop_times();
  conf_.bus_conf.ttl = conf_.origin.bus().ttl();
  conf_.bus_conf.backlog = conf_.origin.bus().backlog();
  conf_.bus_conf.access_token_max_number = static_cast<size_t>(conf_.origin.bus().access_token_max_number());
  {
    conf_.bus_conf.access_tokens.reserve(static_cast<size_t>(conf_.origin.bus().access_tokens_size()));
    for (int i = 0; i < conf_.origin.bus().access_tokens_size(); ++i) {
      const std::string &access_token = conf_.origin.bus().access_tokens(i);
      conf_.bus_conf.access_tokens.push_back(std::vector<unsigned char>());
      conf_.bus_conf.access_tokens.back().assign(
          reinterpret_cast<const unsigned char *>(access_token.data()),
          reinterpret_cast<const unsigned char *>(access_token.data()) + access_token.size());
    }
  }

  conf_.bus_conf.first_idle_timeout = conf_.origin.bus().first_idle_timeout().seconds();
  conf_.bus_conf.ping_interval = conf_.origin.bus().ping_interval().seconds();
  conf_.bus_conf.retry_interval = conf_.origin.bus().retry_interval().seconds();

  conf_.bus_conf.fault_tolerant = static_cast<size_t>(conf_.origin.bus().fault_tolerant());
  conf_.bus_conf.msg_size = static_cast<size_t>(conf_.origin.bus().msg_size());
  conf_.bus_conf.recv_buffer_size = static_cast<size_t>(conf_.origin.bus().recv_buffer_size());
  conf_.bus_conf.send_buffer_size = static_cast<size_t>(conf_.origin.bus().send_buffer_size());
  conf_.bus_conf.send_buffer_number = static_cast<size_t>(conf_.origin.bus().send_buffer_number());

  return 0;
}  // namespace atapp

void app::run_ev_loop(int run_mode) {
  util::time::time_utility::update();

  ev_loop_t *loop = get_evloop();
  if (bus_node_) {
    assert(loop);
    // step X. loop uv_run util stop flag is set
    if (nullptr != loop) {
      flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
      uv_run(loop, static_cast<uv_run_mode>(run_mode));
    }

    if (check_flag(flag_t::RESET_TIMER)) {
      setup_timer();
    }

    if (0 != pending_signals_[0]) {
      process_signals();
    }

    if (check_flag(flag_t::STOPING)) {
      set_flag(flag_t::STOPPED, true);

      if (check_flag(flag_t::TIMEOUT)) {
        // step X. notify all modules timeout
        for (module_ptr_t &mod : modules_) {
          if (mod->is_enabled()) {
            FWLOGERROR("try to stop module {} but timeout", mod->name());
            mod->timeout();
            mod->disable();
          }
        }
      } else {
        // step X. notify all modules to finish and wait for all modules stop
        for (module_ptr_t &mod : modules_) {
          if (mod->is_enabled()) {
            int res = mod->stop();
            if (0 == res) {
              mod->disable();
            } else if (res < 0) {
              mod->disable();
              FWLOGERROR("try to stop module {} but failed and return {}", mod->name(), res);
            } else {
              // any module stop running will make app wait
              set_flag(flag_t::STOPPED, false);
            }
          }
        }

        // step X. if stop is blocked and timeout not triggered, setup stop timeout and waiting for all modules finished
        if (!tick_timer_.timeout_timer && nullptr != loop) {
          timer_ptr_t timer = std::make_shared<timer_info_t>();
          uv_timer_init(loop, &timer->timer);
          timer->timer.data = this;

          int res = uv_timer_start(
              &timer->timer, ev_stop_timeout,
              chrono_to_libuv_duration(conf_.origin.timer().stop_timeout(), ATAPP_DEFAULT_STOP_TIMEOUT), 0);
          if (0 == res) {
            tick_timer_.timeout_timer = timer;
          } else {
            FWLOGERROR("setup stop timeout failed, res: {}", res);
            set_flag(flag_t::TIMEOUT, false);

            timer->timer.data = new timer_ptr_t(timer);
            uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);
          }
        }
      }

      // stop atbus after all module closed
      if (check_flag(flag_t::STOPPED) && bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state() &&
          !bus_node_->check_flag(::atbus::node::flag_t::EN_FT_SHUTDOWN)) {
        bus_node_->shutdown(0);
      }
    }

    // if atbus is at shutdown state, loop event dispatcher using next while iterator
  }
}

int app::run_inner(int run_mode) {
  if (false == check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  ev_loop_t *loop = get_evloop();
  if (nullptr == loop) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  last_proc_event_count_ = 0;
  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  if (mode_t::START != mode_) {
    return 0;
  }

  run_ev_loop(run_mode);

  if (is_closed() && is_inited()) {
    // close timer
    close_timer(tick_timer_.tick_timer);
    close_timer(tick_timer_.timeout_timer);

    // cleanup modules
    for (std::vector<module_ptr_t>::reverse_iterator rit = modules_.rbegin(); rit != modules_.rend(); ++rit) {
      if (*rit) {
        (*rit)->cleanup();
      }
    }

    // cleanup pid file
    cleanup_pidfile();

    set_flag(flag_t::INITIALIZED, false);
    set_flag(flag_t::RUNNING, false);
  }

  if (last_proc_event_count_ > 0) {
    return 1;
  }

  return 0;
}

void app::_app_setup_signal_handle(int signo) {
  // Signal handle has a limited stack can process signal on next proc to keep signal safety
  app *current = app::last_instance_;
  if (nullptr == current) {
    return;
  }

  for (int i = 0; i < MAX_SIGNAL_COUNT; ++i) {
    if (0 == current->pending_signals_[i]) {
      current->pending_signals_[i] = signo;
      break;
    }
  }

  ev_loop_t *loop = current->get_evloop();
  if (nullptr != loop) {
    uv_stop(loop);
  }
}

void app::process_signals() {
  if (0 == pending_signals_[0]) {
    return;
  }

  int signals[MAX_SIGNAL_COUNT] = {0};
  memcpy(signals, pending_signals_, sizeof(pending_signals_));
  memset(pending_signals_, 0, sizeof(pending_signals_));

  for (int i = 0; i < MAX_SIGNAL_COUNT; ++i) {
    if (0 == signals[i]) {
      break;
    }

    process_signal(signals[i]);
  }
}

void app::process_signal(int signo) {
  switch (signo) {
#ifndef WIN32
    case SIGSTOP:
#endif
    case SIGTERM: {
      conf_.upgrade_mode = false;
      stop();
      break;
    }

    default: {
      break;
    }
  }
}

LIBATAPP_MACRO_API int app::trigger_event_on_forward_request(const message_sender_t &source, const message_t &msg) {
  if (evt_on_forward_request_) {
    return evt_on_forward_request_(std::ref(*this), source, msg);
  }

  return 0;
}

LIBATAPP_MACRO_API int app::trigger_event_on_forward_response(const message_sender_t &source, const message_t &msg,
                                                              int32_t error_code) {
  if (evt_on_forward_response_) {
    return evt_on_forward_response_(std::ref(*this), source, msg, error_code);
  }

  return 0;
}

LIBATAPP_MACRO_API void app::trigger_event_on_discovery_event(etcd_discovery_action_t::type action,
                                                              const etcd_discovery_node::ptr_t &node) {
  if (node) {
    const atapp::protocol::atapp_discovery &discovery_info = node->get_discovery_info();
    if (action == etcd_discovery_action_t::EN_NAT_PUT) {
      FWLOGINFO("app {}({}, type={}:{}) got a PUT discovery event({}({}, type={}:{}))", get_app_name(), get_id(),
                get_type_id(), get_type_name(), discovery_info.name(), discovery_info.id(), discovery_info.type_id(),
                discovery_info.type_name());
    } else {
      FWLOGINFO("app {}({}, type={}:{}) got a DELETE discovery event({}({}, type={}:{})", get_app_name(), get_id(),
                get_type_id(), get_type_name(), discovery_info.name(), discovery_info.id(), discovery_info.type_id(),
                discovery_info.type_name());
    }
  }

  for (std::list<std::shared_ptr<atapp_connector_impl> >::const_iterator iter = connectors_.begin();
       iter != connectors_.end(); ++iter) {
    if (*iter) {
      (*iter)->on_discovery_event(action, node);
    }
  }
}

int app::setup_signal() {
  // block signals
  app::last_instance_ = this;
  signal(SIGTERM, _app_setup_signal_handle);
  signal(SIGINT, SIG_IGN);

#ifndef WIN32
  signal(SIGSTOP, _app_setup_signal_handle);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGHUP, SIG_IGN);   // lost parent process
  signal(SIGPIPE, SIG_IGN);  // close stdin, stdout or stderr
  signal(SIGTSTP, SIG_IGN);  // close tty
  signal(SIGTTIN, SIG_IGN);  // tty input
  signal(SIGTTOU, SIG_IGN);  // tty output
#endif

  return 0;
}

static void setup_load_sink(const YAML::Node &log_sink_yaml_src, atapp::protocol::atapp_log_sink &out) {
  // yaml_loader_dump_to(src, out); // already dumped before in setup_load_category(...)

  if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_file_sink_name().c_str())) {
    // Inner file sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_file());
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stdout_sink_name().c_str())) {
    // Inner stdout sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_stdout());
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stderr_sink_name().c_str())) {
    // Inner stderr sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_stderr());
  } else {
    // Dump all configures into unresolved_key_values
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_unresolved_key_values(), std::string());
  }
}

static void setup_load_category(
    const YAML::Node &src,
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_log_category> &out) {
  if (!src || !src.IsMap()) {
    return;
  }

  const YAML::Node name_node = src["name"];
  if (!name_node || !name_node.IsScalar()) {
    return;
  }

  if (name_node.Scalar().empty()) {
    return;
  }

  atapp::protocol::atapp_log_category *log_cat = nullptr;
  for (int i = 0; i < out.size() && nullptr == log_cat; ++i) {
    if (out.Get(i).name() == name_node.Scalar()) {
      log_cat = out.Mutable(i);
    }
  }

  if (nullptr == log_cat) {
    log_cat = out.Add();
  }

  if (nullptr == log_cat) {
    FWLOGERROR("log {} malloc category failed, skipped.", name_node.Scalar());
    return;
  }

  int old_sink_count = log_cat->sink_size();
  yaml_loader_dump_to(src, *log_cat);

  const YAML::Node sink_node = src["sink"];
  if (!sink_node) {
    return;
  }

  if (sink_node.IsMap() && log_cat->sink_size() > old_sink_count) {
    setup_load_sink(sink_node, *log_cat->mutable_sink(old_sink_count));
  } else if (sink_node.IsSequence()) {
    for (int i = 0; i + old_sink_count < log_cat->sink_size(); ++i) {
      if (static_cast<size_t>(i) >= sink_node.size()) {
        break;
      }

      setup_load_sink(sink_node[i], *log_cat->mutable_sink(i + old_sink_count));
    }
  }
}

void app::setup_startup_log() {
  util::log::log_wrapper &wrapper = *WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT);

  ::atapp::protocol::atapp_log std_log_cfg;
  ::atapp::protocol::atapp_log_category std_cat_cfg;
  ::atapp::protocol::atapp_log_sink std_sink_cfg;

  for (std::list<std::string>::iterator iter = conf_.startup_log.begin(); iter != conf_.startup_log.end(); ++iter) {
    if ((*iter).empty() || 0 == UTIL_STRFUNC_STRNCASE_CMP("stdout", (*iter).c_str(), (*iter).size())) {
      wrapper.add_sink(log_sink_maker::get_stdout_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                             std_log_cfg, std_cat_cfg, std_sink_cfg));
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("stderr", (*iter).c_str(), (*iter).size())) {
      wrapper.add_sink(log_sink_maker::get_stderr_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                             std_log_cfg, std_cat_cfg, std_sink_cfg));
    } else {
      util::log::log_sink_file_backend file_sink(*iter);
      file_sink.set_rotate_size(100 * 1024 * 1024);  // Max 100 MB
      file_sink.set_flush_interval(1);
      wrapper.add_sink(file_sink);
    }
  }

  if (wrapper.sink_size() == 0) {
    wrapper.add_sink(log_sink_maker::get_stdout_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                           std_log_cfg, std_cat_cfg, std_sink_cfg));
  }

  wrapper.init();
}

int app::setup_log() {
  util::cli::shell_stream ss(std::cerr);

  // register inner log module
  if (log_reg_.find(log_sink_maker::get_file_sink_name()) == log_reg_.end()) {
    log_reg_[log_sink_maker::get_file_sink_name()] = log_sink_maker::get_file_sink_reg();
  }

  if (log_reg_.find(log_sink_maker::get_stdout_sink_name()) == log_reg_.end()) {
    log_reg_[log_sink_maker::get_stdout_sink_name()] = log_sink_maker::get_stdout_sink_reg();
  }

  if (log_reg_.find(log_sink_maker::get_stderr_sink_name()) == log_reg_.end()) {
    log_reg_[log_sink_maker::get_stderr_sink_name()] = log_sink_maker::get_stderr_sink_reg();
  }

  if (false == is_running()) {
    // if inited, let all modules setup custom logger
    for (module_ptr_t &mod : modules_) {
      if (mod && mod->is_enabled()) {
        int res = mod->setup_log();
        if (0 != res) {
          ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup log for module " << mod->name()
               << " failed, result: " << res << "." << std::endl;
          return res;
        }
      }
    }
  }

  typedef ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField< ::atapp::protocol::atapp_log_category> log_cat_array_t;
  log_cat_array_t categories;
  // load log configure - ini/conf
  {
    cfg_loader_.dump_to("atapp.log.level", *conf_.log.mutable_level());
    uint32_t log_cat_number = LOG_WRAPPER_CATEGORIZE_SIZE;
    cfg_loader_.dump_to("atapp.log.cat.number", log_cat_number);
    if (log_cat_number > LOG_WRAPPER_CATEGORIZE_SIZE) {
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log categorize should not be greater than "
           << LOG_WRAPPER_CATEGORIZE_SIZE
           << ". you can define LOG_WRAPPER_CATEGORIZE_SIZE to a greater number and rebuild atapp." << std::endl;
      log_cat_number = LOG_WRAPPER_CATEGORIZE_SIZE;
    }

    char log_path[256] = {0};

    for (uint32_t i = 0; i < log_cat_number; ++i) {
      UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.cat.%u", i);

      util::config::ini_value &log_cat_conf_src = cfg_loader_.get_node(log_path);
      std::string log_name = log_cat_conf_src["name"].as_cpp_string();
      if (log_name.empty()) {
        continue;
      }

      ::atapp::protocol::atapp_log_category *log_cat_conf = categories.Add();
      if (nullptr == log_cat_conf) {
        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log malloc " << log_name << "(" << i
             << ") failed, skipped." << std::endl;
        continue;
      }
      ini_loader_dump_to(log_cat_conf_src, *log_cat_conf);

      // register log handles
      for (uint32_t j = 0;; ++j) {
        UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.%s.%u", log_name.c_str(), j);

        util::config::ini_value &log_sink_conf_src = cfg_loader_.get_node(log_path);
        std::string sink_type = log_sink_conf_src["type"].as_cpp_string();

        if (sink_type.empty()) {
          break;
        }

        ::atapp::protocol::atapp_log_sink *log_sink = log_cat_conf->add_sink();
        if (nullptr == log_sink) {
          ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log " << log_name << "(" << i
               << ") malloc sink " << sink_type << "failed, skipped." << std::endl;
          continue;
        }

        ini_loader_dump_to(log_sink_conf_src, *log_sink);

        if (0 == UTIL_STRFUNC_STRCASE_CMP(sink_type.c_str(), log_sink_maker::get_file_sink_name().c_str())) {
          // Inner file sink
          ini_loader_dump_to(log_sink_conf_src, *log_sink->mutable_log_backend_file());
        } else if (0 == UTIL_STRFUNC_STRCASE_CMP(sink_type.c_str(), log_sink_maker::get_stdout_sink_name().c_str())) {
          // Inner stdout sink
          ini_loader_dump_to(log_sink_conf_src, *log_sink->mutable_log_backend_stdout());
        } else if (0 == UTIL_STRFUNC_STRCASE_CMP(sink_type.c_str(), log_sink_maker::get_stderr_sink_name().c_str())) {
          // Inner stderr sink
          ini_loader_dump_to(log_sink_conf_src, *log_sink->mutable_log_backend_stderr());
        } else {
          // Dump all configures into unresolved_key_values
          ini_loader_dump_to(log_sink_conf_src, *log_sink->mutable_unresolved_key_values(), std::string());
        }
      }
    }
  }
  conf_.log.mutable_category()->Swap(&categories);

  // load log configure - yaml
  for (yaml_conf_map_t::const_iterator iter = yaml_loader_.begin(); iter != yaml_loader_.end(); ++iter) {
    for (size_t i = 0; i < iter->second.size(); ++i) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      try {
#endif
        const YAML::Node atapp_node = yaml_loader_get_child_by_path(iter->second[i], "atapp");
        if (!atapp_node || !atapp_node.IsMap()) {
          continue;
        }
        const YAML::Node log_node = atapp_node["log"];
        if (!log_node || !log_node.IsMap()) {
          continue;
        }
        const YAML::Node log_level_node = log_node["level"];
        if (log_level_node && log_level_node.IsScalar() && !log_level_node.Scalar().empty()) {
          conf_.log.set_level(log_level_node.Scalar());
        }

        const YAML::Node log_category_node = log_node["category"];
        if (!log_category_node) {
          continue;
        }
        if (log_category_node.IsMap()) {
          setup_load_category(log_category_node, *conf_.log.mutable_category());
        } else if (log_category_node.IsSequence()) {
          size_t sz = log_category_node.size();
          for (size_t j = 0; j < sz; ++j) {
            const YAML::Node cat_node = log_category_node[j];
            if (!cat_node || !cat_node.IsMap()) {
              continue;
            }
            setup_load_category(cat_node, *conf_.log.mutable_category());
          }
        }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      } catch (...) {
        // Ignore error
      }
#endif
    }
  }

  // copy to atapp
  int log_level_id = util::log::log_wrapper::level_t::LOG_LW_INFO;
  log_level_id = util::log::log_formatter::get_level_by_name(conf_.log.level().c_str());

  for (int i = 0; i < conf_.log.category_size() && i < util::log::log_wrapper::categorize_t::MAX; ++i) {
    // init and set prefix
    if (0 != (WLOG_INIT(i, WLOG_LEVELID(log_level_id)))) {
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log initialize " << conf_.log.category(i).name()
           << "(" << i << ") failed, skipped." << std::endl;
      continue;
    }

    const ::atapp::protocol::atapp_log_category &log_cat = conf_.log.category(i);
    if (!log_cat.prefix().empty()) {
      WLOG_GETCAT(i)->set_prefix_format(log_cat.prefix());
    }

    // load stacktrace configure
    if (!log_cat.stacktrace().min().empty() || !log_cat.stacktrace().max().empty()) {
      util::log::log_formatter::level_t::type stacktrace_level_min = util::log::log_formatter::level_t::LOG_LW_DISABLED;
      util::log::log_formatter::level_t::type stacktrace_level_max = util::log::log_formatter::level_t::LOG_LW_DISABLED;
      if (!log_cat.stacktrace().min().empty()) {
        stacktrace_level_min = util::log::log_formatter::get_level_by_name(log_cat.stacktrace().min().c_str());
      }

      if (!log_cat.stacktrace().max().empty()) {
        stacktrace_level_max = util::log::log_formatter::get_level_by_name(log_cat.stacktrace().max().c_str());
      }

      WLOG_GETCAT(i)->set_stacktrace_level(stacktrace_level_max, stacktrace_level_min);
    }

    // For now, only log level can be reload
    size_t old_sink_number = WLOG_GETCAT(i)->sink_size();
    size_t new_sink_number = 0;

    // register log handles
    for (int j = 0; j < log_cat.sink_size(); ++j) {
      const ::atapp::protocol::atapp_log_sink &log_sink = log_cat.sink(j);
      int log_handle_min = util::log::log_wrapper::level_t::LOG_LW_FATAL,
          log_handle_max = util::log::log_wrapper::level_t::LOG_LW_DEBUG;
      if (!log_sink.level().min().empty()) {
        log_handle_min = util::log::log_formatter::get_level_by_name(log_sink.level().min().c_str());
      }
      if (!log_sink.level().max().empty()) {
        log_handle_max = util::log::log_formatter::get_level_by_name(log_sink.level().max().c_str());
      }

      // register log sink
      if (new_sink_number >= old_sink_number) {
        std::map<std::string, log_sink_maker::log_reg_t>::iterator iter = log_reg_.find(log_sink.type());
        if (iter != log_reg_.end()) {
          util::log::log_wrapper::log_handler_t log_handler =
              iter->second(*WLOG_GETCAT(i), j, conf_.log, log_cat, log_sink);
          WLOG_GETCAT(i)->add_sink(log_handler, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                                   static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
          ++new_sink_number;
        } else {
          ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "unavailable log type " << log_sink.type()
               << ", you can add log type register handle before init." << std::endl;
        }
      } else {
        WLOG_GETCAT(i)->set_sink(new_sink_number, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                                 static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
        ++new_sink_number;
      }
    }

    while (WLOG_GETCAT(i)->sink_size() > new_sink_number) {
      WLOG_GETCAT(i)->pop_sink();
    }
  }

  return 0;
}

int app::setup_atbus() {
  int ret = 0, res = 0;
  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }

  bus_node_ = atbus::node::create();
  if (!bus_node_) {
    FWLOGERROR("create bus node failed.");
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  ev_loop_t *loop = get_evloop();
  conf_.bus_conf.ev_loop = loop;
  ret = bus_node_->init(conf_.id, &conf_.bus_conf);
  if (ret < 0) {
    FWLOGERROR("init bus node failed. ret: {}", ret);
    bus_node_.reset();
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  // setup all callbacks
  bus_node_->set_on_recv_handle(std::bind(&app::bus_evt_callback_on_recv_msg, this, std::placeholders::_1,
                                          std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                          std::placeholders::_5, std::placeholders::_6));

  bus_node_->set_on_forward_response_handle(std::bind(&app::bus_evt_callback_on_forward_response, this,
                                                      std::placeholders::_1, std::placeholders::_2,
                                                      std::placeholders::_3, std::placeholders::_4));

  bus_node_->set_on_error_handle(std::bind(&app::bus_evt_callback_on_error, this, std::placeholders::_1,
                                           std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                           std::placeholders::_5));

  bus_node_->set_on_info_log_handle(std::bind(&app::bus_evt_callback_on_info_log, this, std::placeholders::_1,
                                              std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));

  bus_node_->set_on_register_handle(std::bind(&app::bus_evt_callback_on_reg, this, std::placeholders::_1,
                                              std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));

  bus_node_->set_on_shutdown_handle(
      std::bind(&app::bus_evt_callback_on_shutdown, this, std::placeholders::_1, std::placeholders::_2));

  bus_node_->set_on_available_handle(
      std::bind(&app::bus_evt_callback_on_available, this, std::placeholders::_1, std::placeholders::_2));

  bus_node_->set_on_invalid_connection_handle(std::bind(&app::bus_evt_callback_on_invalid_connection, this,
                                                        std::placeholders::_1, std::placeholders::_2,
                                                        std::placeholders::_3));

  bus_node_->set_on_custom_cmd_handle(std::bind(&app::bus_evt_callback_on_custom_cmd, this, std::placeholders::_1,
                                                std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                                std::placeholders::_5, std::placeholders::_6));
  bus_node_->set_on_add_endpoint_handle(std::bind(&app::bus_evt_callback_on_add_endpoint, this, std::placeholders::_1,
                                                  std::placeholders::_2, std::placeholders::_3));

  bus_node_->set_on_remove_endpoint_handle(std::bind(&app::bus_evt_callback_on_remove_endpoint, this,
                                                     std::placeholders::_1, std::placeholders::_2,
                                                     std::placeholders::_3));

  // TODO if not in resume mode, destroy shm
  // if (false == conf_.resume_mode) {}

  // init listen
  for (int i = 0; i < conf_.origin.bus().listen_size(); ++i) {
    res = listen(conf_.origin.bus().listen(i));
    if (res < 0) {
#ifdef _WIN32
      if (EN_ATBUS_ERR_SHM_GET_FAILED == res) {
        FWLOGERROR(
            "Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator.\nWe will ignore "
            "{} this time.",
            conf_.origin.bus().listen(i));
        util::cli::shell_stream ss(std::cerr);
        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED
             << "Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator." << std::endl
             << "We will ignore " << conf_.origin.bus().listen(i) << " this time." << std::endl;

        // res = 0; // Value stored to 'res' is never read
      } else {
#endif
        FWLOGERROR("bus node listen {} failed. res: {}", conf_.origin.bus().listen(i), res);
        if (EN_ATBUS_ERR_PIPE_ADDR_TOO_LONG == res) {
          atbus::channel::channel_address_t address;
          atbus::channel::make_address(conf_.origin.bus().listen(i).c_str(), address);
          std::string abs_path = util::file_system::get_abs_path(address.host.c_str());
          FWLOGERROR("listen pipe socket {}, but the length ({}) exceed the limit {}", abs_path,
                     static_cast<unsigned long long>(abs_path.size()),
                     static_cast<unsigned long long>(atbus::channel::io_stream_get_max_unix_socket_length()));
        }
        ret = res;
#ifdef _WIN32
      }
#endif
    }
  }

  if (ret < 0) {
    FWLOGERROR("bus node listen failed");
    bus_node_.reset();
    return ret;
  }

  // start
  ret = bus_node_->start();
  if (ret < 0) {
    FWLOGERROR("bus node start failed, ret: {}", ret);
    bus_node_.reset();
    return ret;
  }

  // if has father node, block and connect to father node
  if (atbus::node::state_t::CONNECTING_PARENT == bus_node_->get_state() ||
      atbus::node::state_t::LOST_PARENT == bus_node_->get_state()) {
    // setup timeout and waiting for parent connected
    if (!tick_timer_.timeout_timer) {
      timer_ptr_t timer = std::make_shared<timer_info_t>();

      uv_timer_init(loop, &timer->timer);
      timer->timer.data = this;

      res =
          uv_timer_start(&timer->timer, ev_stop_timeout,
                         chrono_to_libuv_duration(conf_.origin.timer().stop_timeout(), ATAPP_DEFAULT_STOP_TIMEOUT), 0);
      if (0 == res) {
        tick_timer_.timeout_timer = timer;
      } else {
        FWLOGERROR("setup stop timeout failed, res: {}", res);
        set_flag(flag_t::TIMEOUT, false);

        timer->timer.data = new timer_ptr_t(timer);
        uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);
      }

      while (nullptr == bus_node_->get_parent_endpoint()) {
        if (check_flag(flag_t::TIMEOUT)) {
          FWLOGERROR("connection to parent node {} timeout", conf_.bus_conf.parent_address);
          ret = -1;
          break;
        }

        {
          flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
          uv_run(loop, UV_RUN_ONCE);
        }
      }

      // if connected, do not trigger timeout
      close_timer(tick_timer_.timeout_timer);

      if (ret < 0) {
        FWLOGERROR("connect to parent node failed");
        bus_node_.reset();
        return ret;
      }
    }
  }

  return 0;
}

void app::close_timer(timer_ptr_t &t) {
  if (t) {
    uv_timer_stop(&t->timer);
    t->timer.data = new timer_ptr_t(t);
    uv_close(reinterpret_cast<uv_handle_t *>(&t->timer), _app_close_timer_handle);

    t.reset();
  }
}

static void _app_tick_timer_handle(uv_timer_t *handle) {
  if (nullptr != handle && nullptr != handle->data) {
    app *self = reinterpret_cast<app *>(handle->data);
    self->tick();
  }
}

int app::setup_timer() {
  set_flag(flag_t::RESET_TIMER, false);

  close_timer(tick_timer_.tick_timer);

  if (chrono_to_libuv_duration(conf_.origin.timer().tick_interval(), 0) < 1) {
    FWLOGWARNING("tick interval can not smaller than 1ms, we use default {}ms now.", ATAPP_DEFAULT_TICK_INTERVAL);
  } else {
    FWLOGINFO("setup tick interval to {}ms.", static_cast<unsigned long long>(chrono_to_libuv_duration(
                                                  conf_.origin.timer().tick_interval(), ATAPP_DEFAULT_TICK_INTERVAL)));
  }

  timer_ptr_t timer = std::make_shared<timer_info_t>();

  ev_loop_t *loop = get_evloop();
  assert(loop);
  uv_timer_init(loop, &timer->timer);
  timer->timer.data = this;

  int res = uv_timer_start(&timer->timer, _app_tick_timer_handle,
                           chrono_to_libuv_duration(conf_.origin.timer().tick_interval(), ATAPP_DEFAULT_TICK_INTERVAL),
                           chrono_to_libuv_duration(conf_.origin.timer().tick_interval(), ATAPP_DEFAULT_TICK_INTERVAL));
  if (0 == res) {
    tick_timer_.tick_timer = timer;
  } else {
    FWLOGERROR("setup tick timer failed, res: {}", res);

    timer->timer.data = new timer_ptr_t(timer);
    uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);

    return EN_ATAPP_ERR_SETUP_TIMER;
  }

  return 0;
}

bool app::write_pidfile() {
  if (!conf_.pid_file.empty()) {
    std::fstream pid_file;
    pid_file.open(conf_.pid_file.c_str(), std::ios::out | std::ios::trunc);
    if (!pid_file.is_open()) {
      util::cli::shell_stream ss(std::cerr);
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "open and write pid file " << conf_.pid_file
           << " failed" << std::endl;
      FWLOGERROR("open and write pid file {} failed", conf_.pid_file);
      // failed and skip running
      return false;
    } else {
      pid_file << atbus::node::get_pid();
      pid_file.close();
    }
  }

  return true;
}

bool app::cleanup_pidfile() {
  if (conf_.origin.remove_pidfile_after_exit() && !conf_.pid_file.empty()) {
    std::fstream pid_file;

    pid_file.open(conf_.pid_file.c_str(), std::ios::in);
    if (!pid_file.is_open()) {
      util::cli::shell_stream ss(std::cerr);
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "try to remove pid file " << conf_.pid_file
           << " failed" << std::endl;

      // failed and skip running
      return false;
    } else {
      int pid = 0;
      pid_file >> pid;
      pid_file.close();

      if (pid != atbus::node::get_pid()) {
        util::cli::shell_stream ss(std::cerr);
        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << "skip remove pid file " << conf_.pid_file
             << ". because it has pid " << pid << ", but our pid is " << atbus::node::get_pid() << std::endl;

        return false;
      } else {
        return util::file_system::remove(conf_.pid_file.c_str());
      }
    }
  }

  return true;
}

void app::print_help() {
  util::cli::shell_stream shls(std::cout);

  shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
         << "Usage: " << conf_.execute_path << " <options> <command> [command paraters...]" << std::endl;
  shls() << get_option_manager()->get_help_msg() << std::endl << std::endl;

  if (!(get_command_manager()->empty() && get_command_manager()->children_empty())) {
    shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
           << "Custom command help:" << std::endl;
    shls() << get_command_manager()->get_help_msg() << std::endl;
  }
}

bool app::match_gateway_hosts(const atapp::protocol::atapp_gateway &checked) const {
  bool has_matched_value = false;
  bool has_valid_conf = false;
  for (int i = 0; !has_matched_value && i < checked.match_hosts_size(); ++i) {
    if (checked.match_hosts(i).empty()) {
      continue;
    }
    has_valid_conf = true;
    has_matched_value = checked.match_hosts(i) == atbus::node::get_hostname();
  }

  return !has_valid_conf || has_matched_value;
}

bool app::match_gateway_namespace(const atapp::protocol::atapp_gateway &checked) const {
  bool has_matched_value = false;
  bool has_valid_conf = false;
  for (int i = 0; !has_matched_value && i < checked.match_namespaces_size(); ++i) {
    if (checked.match_namespaces(i).empty()) {
      continue;
    }
    has_valid_conf = true;
    has_matched_value = checked.match_namespaces(i) == get_metadata().namespace_name();
  }

  return !has_valid_conf || has_matched_value;
}

bool app::match_gateway_labels(const atapp::protocol::atapp_gateway &checked) const {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>::const_iterator iter =
      checked.match_labels().begin();
  for (; iter != checked.match_labels().end(); ++iter) {
    if (iter->first.empty() || iter->second.empty()) {
      continue;
    }

    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>::const_iterator self_label_iter =
        get_metadata().labels().find(iter->first);
    if (self_label_iter == get_metadata().labels().end()) {
      return false;
    }
    if (self_label_iter->second != iter->second) {
      return false;
    }
  }

  return true;
}

LIBATAPP_MACRO_API app::custom_command_sender_t app::get_custom_command_sender(util::cli::callback_param params) {
  custom_command_sender_t ret;
  ret.self = nullptr;
  ret.response = nullptr;
  if (nullptr != params.get_ext_param()) {
    ret = *reinterpret_cast<custom_command_sender_t *>(params.get_ext_param());
  }

  return ret;
}

LIBATAPP_MACRO_API bool app::add_custom_command_rsp(util::cli::callback_param params, const std::string &rsp_text) {
  custom_command_sender_t sender = get_custom_command_sender(params);
  if (nullptr == sender.response) {
    return false;
  }

  sender.response->push_back(rsp_text);
  return true;
}

LIBATAPP_MACRO_API void app::split_ids_by_string(const char *in, std::vector<app_id_t> &out) {
  if (nullptr == in) {
    return;
  }

  out.reserve(8);

  while (nullptr != in && *in) {
    // skip spaces
    if (' ' == *in || '\t' == *in || '\r' == *in || '\n' == *in) {
      ++in;
      continue;
    }

    out.push_back(::util::string::to_int<app_id_t>(in));

    for (; nullptr != in && *in && '.' != *in; ++in)
      ;
    // skip dot and ready to next segment
    if (nullptr != in && *in && '.' == *in) {
      ++in;
    }
  }
}

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in,
                                                               const std::vector<app_id_t> &mask_in) {
  if (nullptr == id_in || 0 == *id_in) {
    return 0;
  }

  bool id_in_is_number = true;
  if (!mask_in.empty()) {
    for (const char *check_char = id_in; *check_char && id_in_is_number; ++check_char) {
      if ('.' == *check_char) {
        id_in_is_number = false;
      }
    }
  }

  if (id_in_is_number) {
    return ::util::string::to_int<app_id_t>(id_in);
  }

  std::vector<app_id_t> ids;
  split_ids_by_string(id_in, ids);
  app_id_t ret = 0;
  for (size_t i = 0; i < ids.size() && i < mask_in.size(); ++i) {
    ret <<= mask_in[i];
    ret |= (ids[i] & ((static_cast<app_id_t>(1) << mask_in[i]) - 1));
  }

  return ret;
}

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in, const char *mask_in) {
  if (nullptr == id_in || 0 == *id_in) {
    return 0;
  }

  std::vector<app_id_t> mask;
  split_ids_by_string(mask_in, mask);
  return convert_app_id_by_string(id_in, mask);
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, const std::vector<app_id_t> &mask_in,
                                                             bool hex) {
  std::vector<app_id_t> ids;
  ids.resize(mask_in.size(), 0);

  for (size_t i = 0; i < mask_in.size(); ++i) {
    size_t idx = mask_in.size() - i - 1;
    ids[idx] = id_in & ((static_cast<app_id_t>(1) << mask_in[idx]) - 1);
    id_in >>= mask_in[idx];
  }

  std::stringstream ss;
  if (hex) {
    ss << std::hex;
  }

  for (size_t i = 0; i < ids.size(); ++i) {
    if (0 != i) {
      ss << ".";
    }

    if (hex) {
      ss << "0x";
    }
    ss << ids[i];
  }

  return ss.str();
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, const char *mask_in, bool hex) {
  std::vector<app_id_t> mask;
  split_ids_by_string(mask_in, mask);
  return convert_app_id_to_string(id_in, mask, hex);
}

LIBATAPP_MACRO_API app *app::get_last_instance() { return last_instance_; }

int app::prog_option_handler_help(util::cli::callback_param /*params*/, util::cli::cmd_option * /*opt_mgr*/,
                                  util::cli::cmd_option_ci * /*cmd_mgr*/) {
  mode_ = mode_t::HELP;
  return 0;
}

int app::prog_option_handler_version(util::cli::callback_param /*params*/) {
  mode_ = mode_t::INFO;
  printf("%s", get_build_version().c_str());
  return 0;
}

int app::prog_option_handler_set_id(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.id_cmd = params[0]->to_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-id require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_id_mask(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.id_mask.clear();
    split_ids_by_string(params[0]->to_string(), conf_.id_mask);
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-id-mask require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_conf_file(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.conf_file = params[0]->to_cpp_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-c, --conf, --config require 1 parameter"
         << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_pid(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.pid_file = params[0]->to_cpp_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-p, --pid require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_upgrade_mode(util::cli::callback_param /*params*/) {
  conf_.upgrade_mode = true;
  return 0;
}

int app::prog_option_handler_set_startup_log(util::cli::callback_param params) {
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    conf_.startup_log.push_back(params[i]->to_cpp_string());
  }
  return 0;
}

int app::prog_option_handler_start(util::cli::callback_param /*params*/) {
  mode_ = mode_t::START;
  return 0;
}

int app::prog_option_handler_stop(util::cli::callback_param /*params*/) {
  mode_ = mode_t::STOP;
  last_command_.clear();
  last_command_.push_back("stop");
  if (conf_.upgrade_mode) {
    last_command_.push_back("--upgrade");
  }
  return 0;
}

int app::prog_option_handler_reload(util::cli::callback_param /*params*/) {
  mode_ = mode_t::RELOAD;
  last_command_.clear();
  last_command_.push_back("reload");
  if (conf_.upgrade_mode) {
    last_command_.push_back("--upgrade");
  }
  return 0;
}

int app::prog_option_handler_run(util::cli::callback_param params) {
  mode_ = mode_t::CUSTOM;
  for (size_t i = 0; i < params.get_params_number(); ++i) {
    last_command_.push_back(params[i]->to_cpp_string());
  }

  if (0 == params.get_params_number()) {
    mode_ = mode_t::INFO;
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "run must follow a command" << std::endl;
  }
  return 0;
}

void app::setup_option(int argc, const char *argv[], void *priv_data) {
  assert(argc > 0);

  util::cli::cmd_option::ptr_type opt_mgr = get_option_manager();
  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
  // show help and exit
  opt_mgr->bind_cmd("-h, --help, help", &app::prog_option_handler_help, this, opt_mgr.get(), cmd_mgr.get())
      ->set_help_msg("-h. --help, help                       show this help message.");

  // show version and exit
  opt_mgr->bind_cmd("-v, --version", &app::prog_option_handler_version, this)
      ->set_help_msg("-v, --version                          show version and exit.");

  // set app bus id
  opt_mgr->bind_cmd("-id", &app::prog_option_handler_set_id, this)
      ->set_help_msg("-id <bus id>                           set app bus id.");
  // set app bus id
  opt_mgr->bind_cmd("-id-mask", &app::prog_option_handler_set_id_mask, this)
      ->set_help_msg(
          "-id-mask <bit number of bus id mask>   set app bus id mask(example: 8.8.8.8, and then -id 1.2.3.4 is just "
          "like "
          "-id 0x01020304).");

  // set configure file path
  opt_mgr->bind_cmd("-c, --conf, --config", &app::prog_option_handler_set_conf_file, this)
      ->set_help_msg("-c, --conf, --config <file path>       set configure file path.");

  // set app pid file
  opt_mgr->bind_cmd("-p, --pid", &app::prog_option_handler_set_pid, this)
      ->set_help_msg("-p, --pid <pid file>                   set where to store pid.");

  // set configure file path
  opt_mgr->bind_cmd("--upgrade", &app::prog_option_handler_upgrade_mode, this)
      ->set_help_msg("--upgrade                              set upgrade mode.");

  opt_mgr->bind_cmd("--startup-log", &app::prog_option_handler_set_startup_log, this)
      ->set_help_msg("--startup-log                          where to write start up log(file name or stdout/stderr).");

  // start server
  opt_mgr->bind_cmd("start", &app::prog_option_handler_start, this)
      ->set_help_msg("start                                  start mode.");

  // stop server
  opt_mgr->bind_cmd("stop", &app::prog_option_handler_stop, this)
      ->set_help_msg("stop                                   send stop command to server.");

  // reload all configures
  opt_mgr->bind_cmd("reload", &app::prog_option_handler_reload, this)
      ->set_help_msg("reload                                 send reload command to server.");

  // run custom command
  opt_mgr->bind_cmd("run", &app::prog_option_handler_run, this)
      ->set_help_msg("run <command> [parameters...]          send custom command and parameters to server.");

  conf_.execute_path = argv[0];
  // fill app version data
  if (conf_.app_version.empty()) {
    std::vector<std::string> out;
    util::file_system::split_path(out, conf_.execute_path);
    std::stringstream ss;
    if (out.empty()) {
      ss << conf_.execute_path;
    } else {
      ss << out[out.size() - 1];
    }
    ss << " with libatapp " << LIBATAPP_VERSION;
    conf_.app_version = ss.str();
  }
  opt_mgr->start(argc - 1, &argv[1], false, priv_data);
}

int app::app::command_handler_start(util::cli::callback_param /*params*/) {
  // add_custom_command_rsp(params, "success");
  // do nothing
  return 0;
}

int app::command_handler_stop(util::cli::callback_param params) {
  char msg[256] = {0};
  LOG_WRAPPER_FWAPI_FORMAT_TO_N(msg, sizeof(msg), "app node {:#x} run stop command success", get_id());
  FWLOGINFO("{}", msg);
  add_custom_command_rsp(params, msg);

  bool enable_upgrade_mode = false;
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    if (params[i]->to_cpp_string() == "--upgrade") {
      enable_upgrade_mode = true;
    }
  }
  conf_.upgrade_mode = enable_upgrade_mode;
  return stop();
}

int app::command_handler_reload(util::cli::callback_param params) {
  char msg[256] = {0};
  LOG_WRAPPER_FWAPI_FORMAT_TO_N(msg, sizeof(msg), "app node {:#x} run reload command success", get_id());
  FWLOGINFO("{}", msg);
  add_custom_command_rsp(params, msg);

  bool enable_upgrade_mode = false;
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    if (params[i]->to_cpp_string() == "--upgrade") {
      enable_upgrade_mode = true;
    }
  }
  conf_.upgrade_mode = enable_upgrade_mode;
  return reload();
}

int app::command_handler_invalid(util::cli::callback_param params) {
  char msg[256] = {0};
  std::stringstream args;
  for (util::cli::cmd_option_list::cmd_array_type::const_iterator iter = params.get_cmd_array().begin();
       iter != params.get_cmd_array().end(); ++iter) {
    args << " \"" << iter->first << '"';
  }
  for (size_t i = 0; i < params.get_params_number(); ++i) {
    if (params[i]) {
      args << " \"" << params[i]->to_cpp_string() << '"';
    }
  }
  LOG_WRAPPER_FWAPI_FORMAT_TO_N(msg, sizeof(msg), "receive invalid command :{}", args.str());
  FWLOGERROR("{}", msg);
  add_custom_command_rsp(params, msg);
  return 0;
}

int app::command_handler_disable_etcd(util::cli::callback_param params) {
  if (!inner_module_etcd_) {
    const char *msg = "Etcd module is not initialized, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else if (inner_module_etcd_->is_etcd_enabled()) {
    inner_module_etcd_->disable_etcd();
    const char *msg = "Etcd context is disabled now.";
    FWLOGINFO("{}", msg);
    add_custom_command_rsp(params, msg);
  } else {
    const char *msg = "Etcd context is already disabled, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  }
  return 0;
}

int app::command_handler_enable_etcd(util::cli::callback_param params) {
  if (!inner_module_etcd_) {
    const char *msg = "Etcd module not initialized, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else if (inner_module_etcd_->is_etcd_enabled()) {
    const char *msg = "Etcd context is already enabled, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else {
    inner_module_etcd_->enable_etcd();
    if (inner_module_etcd_->is_etcd_enabled()) {
      const char *msg = "Etcd context is enabled now.";
      FWLOGINFO("{}", msg);
      add_custom_command_rsp(params, msg);
    } else {
      const char *msg = "Etcd context can not be enabled, maybe need configure etcd.hosts.";
      FWLOGERROR("{}", msg);
      add_custom_command_rsp(params, msg);
    }
  }

  return 0;
}

int app::command_handler_list_discovery(util::cli::callback_param params) {
  if (!inner_module_etcd_) {
    const char *msg = "Etcd module not initialized.";
    add_custom_command_rsp(params, msg);
  } else {
    size_t start_idx = 0;
    size_t end_idx = 0;
    if (params.get_params_number() > 0) {
      start_idx = static_cast<size_t>(params[0]->to_uint64());
    }
    if (params.get_params_number() > 1) {
      end_idx = static_cast<size_t>(params[1]->to_uint64());
    }

    const std::vector<etcd_discovery_node::ptr_t> &nodes =
        inner_module_etcd_->get_global_discovery().get_sorted_nodes();
    for (size_t i = start_idx; i < nodes.size() && (0 == end_idx || i < end_idx); ++i) {
      if (!nodes[i]) {
        continue;
      }
      const atapp::protocol::atapp_discovery &node_info = nodes[i]->get_discovery_info();
      add_custom_command_rsp(
          params, LOG_WRAPPER_FWAPI_FORMAT("node -> private data: {}, destroy event: {}, hash: {:016x}{:016x}, {}",
                                           reinterpret_cast<const void *>(nodes[i]->get_private_data_ptr()),
                                           (nodes[i]->get_on_destroy() ? "ON" : "OFF"), nodes[i]->get_name_hash().first,
                                           nodes[i]->get_name_hash().second, rapidsjon_loader_stringify(node_info)));
    }
  }

  return 0;
}

int app::bus_evt_callback_on_recv_msg(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                      const atbus::protocol::msg &msg, const void *buffer, size_t len) {
  if (atbus::protocol::msg::kDataTransformReq != msg.msg_body_case() || 0 == msg.head().src_bus_id()) {
    FWLOGERROR("receive a message from unknown source {} or invalid body case", msg.head().src_bus_id());
    return EN_ATBUS_ERR_BAD_DATA;
  }

  app_id_t from_id = msg.data_transform_req().from();
  app::message_t message;
  message.data = buffer;
  message.data_size = len;
  message.metadata = nullptr;
  message.msg_sequence = msg.head().sequence();
  message.type = msg.head().type();

  app::message_sender_t sender;
  sender.id = from_id;
  sender.remote = get_endpoint(from_id);
  if (nullptr != sender.remote) {
    sender.name = &sender.remote->get_name();
  }

  trigger_event_on_forward_request(sender, message);

  ++last_proc_event_count_;
  return 0;
}

int app::bus_evt_callback_on_forward_response(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                              const atbus::protocol::msg *m) {
  ++last_proc_event_count_;

  // call failed callback if it's message transfer
  if (nullptr == m) {
    FWLOGERROR("app {:#x} receive a send failure without message", get_id());
    return EN_ATAPP_ERR_SEND_FAILED;
  }

  if (m->head().ret() < 0) {
    FWLOGERROR("app {:#x} receive a send failure from {:#x}, message cmd: {}, type: {}, ret: {}, sequence: {}",
               get_id(), m->head().src_bus_id(), atbus::msg_handler::get_body_name(m->msg_body_case()),
               m->head().type(), m->head().ret(), m->head().sequence());
  }

  if (atbus::protocol::msg::kDataTransformRsp != m->msg_body_case() || 0 == m->head().src_bus_id()) {
    FWLOGERROR("receive a message from unknown source {} or invalid body case", m->head().src_bus_id());
    return EN_ATBUS_ERR_BAD_DATA;
  }

  if (atbus_connector_) {
    atbus_connector_->on_receive_forward_response(
        m->data_transform_rsp().from(), m->head().type(), m->head().sequence(), m->head().ret(),
        reinterpret_cast<const void *>(m->data_transform_rsp().content().c_str()),
        m->data_transform_rsp().content().size(), nullptr);
    return 0;
  }

  app_id_t from_id = m->data_transform_rsp().from();
  app::message_t message;
  message.data = reinterpret_cast<const void *>(m->data_transform_rsp().content().c_str());
  message.data_size = m->data_transform_rsp().content().size();
  message.metadata = nullptr;
  message.msg_sequence = m->head().sequence();
  message.type = m->head().type();

  app::message_sender_t sender;
  sender.id = from_id;
  sender.remote = get_endpoint(from_id);
  if (nullptr != sender.remote) {
    sender.name = &sender.remote->get_name();
  }

  trigger_event_on_forward_response(sender, message, m->head().ret());
  return 0;
}

int app::bus_evt_callback_on_error(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                   int status, int errcode) {
  // meet eof or reset by peer is not a error
  if (UV_EOF == errcode || UV_ECONNRESET == errcode) {
    const char *msg = UV_EOF == errcode ? "got EOF" : "reset by peer";
    if (nullptr != conn) {
      if (nullptr != ep) {
        FWLOGINFO("bus node {:#x} endpoint {:#x} connection {}({}) closed: {}", n.get_id(), ep->get_id(),
                  reinterpret_cast<const void *>(conn), conn->get_address().address, msg);
      } else {
        FWLOGINFO("bus node {:#x} connection {}({}) closed: {}", n.get_id(), reinterpret_cast<const void *>(conn),
                  conn->get_address().address, msg);
      }

    } else {
      if (nullptr != ep) {
        FWLOGINFO("bus node {:#x} endpoint {:#x} closed: {}", n.get_id(), ep->get_id(), msg);
      } else {
        FWLOGINFO("bus node {:#x} closed: {}", n.get_id(), msg);
      }
    }
    return 0;
  }

  if (nullptr != conn) {
    if (nullptr != ep) {
      FWLOGERROR("bus node {:#x} endpoint {:#x} connection {}({}) error, status: {}, error code: {}", n.get_id(),
                 ep->get_id(), reinterpret_cast<const void *>(conn), conn->get_address().address, status, errcode);
    } else {
      FWLOGERROR("bus node {:#x} connection {}({}) error, status: {}, error code: {}", n.get_id(),
                 reinterpret_cast<const void *>(conn), conn->get_address().address, status, errcode);
    }

  } else {
    if (nullptr != ep) {
      FWLOGERROR("bus node {:#x} endpoint {:#x} error, status: {}, error code: {}", n.get_id(), ep->get_id(), status,
                 errcode);
    } else {
      FWLOGERROR("bus node {:#x} error, status: {}, error code: {}", n.get_id(), status, errcode);
    }
  }

  return 0;
}

int app::bus_evt_callback_on_info_log(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                      const char *msg) {
  FWLOGINFO("bus node {:#x} endpoint {:#x}({}) connection {}({}) message: {}", n.get_id(),
            (nullptr == ep ? 0 : ep->get_id()), reinterpret_cast<const void *>(ep),
            (nullptr == conn ? "" : conn->get_address().address.c_str()), reinterpret_cast<const void *>(conn),
            (nullptr == msg ? "" : msg));
  return 0;
}

int app::bus_evt_callback_on_reg(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                 int res) {
  ++last_proc_event_count_;

  if (nullptr != conn) {
    if (nullptr != ep) {
      WLOGINFO("bus node 0x%llx endpoint 0x%llx connection %s registered, res: %d",
               static_cast<unsigned long long>(n.get_id()), static_cast<unsigned long long>(ep->get_id()),
               conn->get_address().address.c_str(), res);
    } else {
      WLOGINFO("bus node 0x%llx connection %s registered, res: %d", static_cast<unsigned long long>(n.get_id()),
               conn->get_address().address.c_str(), res);
    }

  } else {
    if (nullptr != ep) {
      WLOGINFO("bus node 0x%llx endpoint 0x%llx registered, res: %d", static_cast<unsigned long long>(n.get_id()),
               static_cast<unsigned long long>(ep->get_id()), res);
    } else {
      WLOGINFO("bus node 0x%llx registered, res: %d", static_cast<unsigned long long>(n.get_id()), res);
    }
  }

  return 0;
}

int app::bus_evt_callback_on_shutdown(const atbus::node &n, int reason) {
  FWLOGINFO("bus node {:#x} shutdown, reason: {}", n.get_id(), reason);
  return stop();
}

int app::bus_evt_callback_on_available(const atbus::node &n, int res) {
  FWLOGINFO("bus node {:#x} initialze done, res: {}", n.get_id(), res);
  return res;
}

int app::bus_evt_callback_on_invalid_connection(const atbus::node &n, const atbus::connection *conn, int res) {
  ++last_proc_event_count_;

  if (nullptr == conn) {
    FWLOGERROR("bus node {:#x} recv a invalid nullptr connection , res: {}", n.get_id(), res);
  } else {
    // already disconncted finished.
    if (atbus::connection::state_t::DISCONNECTED != conn->get_status()) {
      if (is_closing()) {
        FWLOGINFO(
            "bus node {:#x} make a invalid connection {}({}) when closing, all unfinished connection will be aborted. "
            "res: {}",
            n.get_id(), reinterpret_cast<const void *>(conn), conn->get_address().address, res);
      } else {
        if (conn->check_flag(atbus::connection::flag_t::TEMPORARY)) {
          FWLOGWARNING("bus node {:#x} temporary connection {}({}) expired. res: {}", n.get_id(),
                       reinterpret_cast<const void *>(conn), conn->get_address().address, res);
        } else {
          FWLOGERROR("bus node {:#x} make a invalid connection {}({}). res: {}", n.get_id(),
                     reinterpret_cast<const void *>(conn), conn->get_address().address, res);
        }
      }
    }
  }
  return 0;
}

int app::bus_evt_callback_on_custom_cmd(const atbus::node &n, const atbus::endpoint *, const atbus::connection *,
                                        app_id_t /*src_id*/, const std::vector<std::pair<const void *, size_t> > &args,
                                        std::list<std::string> &rsp) {
  ++last_proc_event_count_;
  if (args.empty()) {
    return 0;
  }

  std::vector<std::string> args_str;
  args_str.resize(args.size());

  for (size_t i = 0; i < args_str.size(); ++i) {
    args_str[i].assign(reinterpret_cast<const char *>(args[i].first), args[i].second);
  }

  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
  custom_command_sender_t sender;
  sender.self = this;
  sender.response = &rsp;
  cmd_mgr->start(args_str, true, &sender);

  size_t max_size = n.get_conf().msg_size;
  size_t use_size = 0;
  size_t sum_size = 0;
  bool is_truncated = false;
  for (std::list<std::string>::iterator iter = rsp.begin(); iter != rsp.end();) {
    std::list<std::string>::iterator cur = iter++;
    sum_size += (*cur).size();
    if (is_truncated) {
      rsp.erase(cur);
      continue;
    }
    if (use_size + cur->size() > max_size) {
      cur->resize(max_size - use_size);
      use_size = max_size;
      is_truncated = true;
    } else {
      use_size += (*cur).size();
    }
  }

  if (is_truncated) {
    std::stringstream ss;
    ss << "Response message size " << sum_size << " is greater than size limit " << max_size
       << ", some data will be truncated.";
    rsp.push_back(ss.str());
  }
  return 0;
}

int app::bus_evt_callback_on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
  ++last_proc_event_count_;

  if (nullptr == ep) {
    FWLOGERROR("bus node {:#x} make connection to nullptr, res: {}", n.get_id(), res);
  } else {
    FWLOGINFO("bus node {:#x} make connection to {:#x} done, res: {}", n.get_id(), ep->get_id(), res);

    if (evt_on_app_connected_) {
      evt_on_app_connected_(std::ref(*this), std::ref(*ep), res);
    }
  }
  return 0;
}

int app::bus_evt_callback_on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
  ++last_proc_event_count_;

  if (nullptr == ep) {
    FWLOGERROR("bus node {:#x} release connection to nullptr, res: {}", n.get_id(), res);
  } else {
    FWLOGINFO("bus node {:#x} release connection to {:#x} done, res: {}", n.get_id(), ep->get_id(), res);

    if (evt_on_app_disconnected_) {
      evt_on_app_disconnected_(std::ref(*this), std::ref(*ep), res);
    }
  }
  return 0;
}

static size_t __g_atapp_custom_cmd_rsp_recv_times = 0;
int app::bus_evt_callback_on_custom_rsp(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                        app_id_t src_id, const std::vector<std::pair<const void *, size_t> > &args,
                                        uint64_t /*seq*/) {
  ++last_proc_event_count_;
  ++__g_atapp_custom_cmd_rsp_recv_times;
  if (args.empty()) {
    return 0;
  }

  util::cli::shell_stream ss(std::cout);
  char bus_addr[64] = {0};
  UTIL_STRFUNC_SNPRINTF(bus_addr, sizeof(bus_addr), "0x%llx", static_cast<unsigned long long>(src_id));
  for (size_t i = 0; i < args.size(); ++i) {
    std::string text(static_cast<const char *>(args[i].first), args[i].second);
    ss() << "Custom Command: (" << bus_addr << "): " << text << std::endl;
  }

  return 0;
}

LIBATAPP_MACRO_API void app::add_connector_inner(std::shared_ptr<atapp_connector_impl> connector) {
  if (!connector) {
    return;
  }

  connectors_.push_back(connector);

  // record all protocols
  for (atapp_connector_impl::protocol_set_t::const_iterator iter = connector->get_support_protocols().begin();
       iter != connector->get_support_protocols().end(); ++iter) {
    connector_protocol_map_t::const_iterator find_iter = connector_protocols_.find(*iter);
    if (find_iter != connector_protocols_.end()) {
      FWLOGWARNING("protocol {} is already registered by {}, we will overwrite it with {}", *iter,
                   find_iter->second->name(), connector->name());
    }

    connector_protocols_[*iter] = connector;
  }
}

void app::setup_command() {
  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();

  // start server
  cmd_mgr->bind_cmd("start", &app::app::command_handler_start, this);
  // stop server
  cmd_mgr->bind_cmd("stop", &app::app::command_handler_stop, this);
  // reload all configures
  cmd_mgr->bind_cmd("reload", &app::app::command_handler_reload, this);
  // enable etcd
  cmd_mgr->bind_cmd("enable-etcd", &app::command_handler_enable_etcd, this)
      ->set_help_msg("enable-etcd                            enable etcd discovery module.");

  // disable etcd
  cmd_mgr->bind_cmd("disable-etcd", &app::command_handler_disable_etcd, this)
      ->set_help_msg("disable-etcd                           disable etcd discovery module.");

  cmd_mgr->bind_cmd("list-discovery", &app::command_handler_list_discovery, this)
      ->set_help_msg("list-discovery [start:0] [end]         list all discovery node.");

  // invalid command
  cmd_mgr->bind_cmd("@OnError", &app::command_handler_invalid, this);
}

int app::send_last_command(ev_loop_t *ev_loop) {
  if (last_command_.empty()) {
    FWLOGERROR("command is empty.");
    return EN_ATAPP_ERR_COMMAND_IS_NULL;
  }

  // step 1. using the fastest way to connect to server
  int use_level = 0;
  bool is_sync_channel = false;
  atbus::channel::channel_address_t use_addr;

  for (int i = 0; i < conf_.origin.bus().listen_size(); ++i) {
    atbus::channel::channel_address_t parsed_addr;
    make_address(conf_.origin.bus().listen(i).c_str(), parsed_addr);
    int parsed_level = 0;
    if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm", parsed_addr.scheme.c_str(), 3)) {
      parsed_level = 5;
      is_sync_channel = true;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("unix", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 4;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("ipv6", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 3;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("ipv4", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 2;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("dns", parsed_addr.scheme.c_str(), 3)) {
      parsed_level = 1;
    }

    if (parsed_level > use_level) {
#ifdef _WIN32
      // On windows, shared memory must be load in the same directory, so use IOS first
      // Use Global\\ prefix requires the SeCreateGlobalPrivilege privilege
      if (5 == parsed_level && 0 != use_level) {
        continue;
      }
#endif
      use_addr = parsed_addr;
      use_level = parsed_level;
    }
  }

  if (0 == use_level) {
    FWLOGERROR("there is no available listener address to send command.");
    return EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS;
  }

  if (!ev_loop_) {
    ev_loop_ = uv_default_loop();
  }
  conf_.bus_conf.ev_loop = ev_loop_;

  if (!bus_node_) {
    bus_node_ = atbus::node::create();
  }

  // command mode , must no concurrence
  if (!bus_node_) {
    FWLOGERROR("create bus node failed");
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  // no need to connect to parent node
  conf_.bus_conf.parent_address.clear();

  // using 0 for command sender
  int ret = bus_node_->init(0, &conf_.bus_conf);
  if (ret < 0) {
    FWLOGERROR("init bus node failed. ret: {}", ret);
    return ret;
  }

  ret = bus_node_->start();
  if (ret < 0) {
    FWLOGERROR("start bus node failed. ret: {}", ret);
    return ret;
  }

  // step 2. connect failed return error code
  atbus::endpoint *ep = nullptr;
  if (is_sync_channel) {
    // preallocate endpoint when using shared memory channel, because this channel can not be connected without endpoint
    std::vector<atbus::endpoint_subnet_conf> subnets;
    atbus::endpoint::ptr_t new_ep =
        atbus::endpoint::create(bus_node_.get(), conf_.id, subnets, bus_node_->get_pid(), bus_node_->get_hostname());
    ret = bus_node_->add_endpoint(new_ep);
    if (ret < 0) {
      FWLOGERROR("connect to {} failed. ret: {}", use_addr.address, ret);
      return ret;
    }

    ret = bus_node_->connect(use_addr.address.c_str(), new_ep.get());
    if (ret >= 0) {
      ep = new_ep.get();
    }
  } else {
    ret = bus_node_->connect(use_addr.address.c_str());
  }

  if (ret < 0) {
    FWLOGERROR("connect to {} failed. ret: {}", use_addr.address, ret);
    return ret;
  }

  // step 3. setup timeout timer
  if (!tick_timer_.timeout_timer) {
    timer_ptr_t timer = std::make_shared<timer_info_t>();
    uv_timer_init(ev_loop, &timer->timer);
    timer->timer.data = this;

    int res =
        uv_timer_start(&timer->timer, ev_stop_timeout,
                       chrono_to_libuv_duration(conf_.origin.timer().stop_timeout(), ATAPP_DEFAULT_STOP_TIMEOUT), 0);
    if (0 == res) {
      tick_timer_.timeout_timer = timer;
    } else {
      FWLOGERROR("setup timeout timer failed, res: {}", res);
      set_flag(flag_t::TIMEOUT, false);

      timer->timer.data = new timer_ptr_t(timer);
      uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);
    }
  }

  // step 4. waiting for connect success
  while (nullptr == ep) {
    // If there is no connection timer, it means connecting is arleady failed.
    if (0 == bus_node_->get_connection_timer_size()) {
      break;
    }

    flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
    uv_run(ev_loop, UV_RUN_ONCE);

    if (check_flag(flag_t::TIMEOUT)) {
      break;
    }
    ep = bus_node_->get_endpoint(conf_.id);
  }

  if (nullptr == ep) {
    close_timer(tick_timer_.timeout_timer);
    FWLOGERROR("connect to {} failed or timeout.", use_addr.address);
    return EN_ATAPP_ERR_CONNECT_ATAPP_FAILED;
  }

  flag_guard_t running_guard(*this, flag_t::RUNNING);

  // step 5. send data
  std::vector<const void *> arr_buff;
  std::vector<size_t> arr_size;
  arr_buff.resize(last_command_.size());
  arr_size.resize(last_command_.size());
  for (size_t i = 0; i < last_command_.size(); ++i) {
    arr_buff[i] = last_command_[i].data();
    arr_size[i] = last_command_[i].size();
  }

  bus_node_->set_on_custom_rsp_handle(std::bind(&app::bus_evt_callback_on_custom_rsp, this, std::placeholders::_1,
                                                std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                                std::placeholders::_5, std::placeholders::_6));

  ret = bus_node_->send_custom_cmd(ep->get_id(), &arr_buff[0], &arr_size[0], last_command_.size());
  if (ret < 0) {
    close_timer(tick_timer_.timeout_timer);
    FWLOGERROR("send command failed. ret: {}", ret);
    return ret;
  }

  // step 6. waiting for send done(for shm, no need to wait, for io_stream fd, waiting write callback)
  if (!is_sync_channel) {
    do {
      if (__g_atapp_custom_cmd_rsp_recv_times) {
        break;
      }

      flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
      uv_run(ev_loop, UV_RUN_ONCE);
      if (check_flag(flag_t::TIMEOUT)) {
        FWLOGERROR("send command or receive response timeout");
        ret = -1;
        break;
      }
    } while (true);
  }

  close_timer(tick_timer_.timeout_timer);

  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }
  return ret;
}
}  // namespace atapp
