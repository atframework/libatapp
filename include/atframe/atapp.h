﻿/**
 * atapp.h
 *
 *  Created on: 2016年04月23日
 *      Author: owent
 */
#ifndef LIBATAPP_ATAPP_H
#define LIBATAPP_ATAPP_H

#pragma once

#include <bitset>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "atframe/atapp_conf.h"

#include "cli/cmd_option.h"
#include "time/time_utility.h"

#include "config/ini_loader.h"

#include "network/http_request.h"

#include "atframe/atapp_log_sink_maker.h"
#include "atframe/atapp_module_impl.h"

#include "connectors/atapp_connector_atbus.h"

#include "etcdcli/etcd_cluster.h"

namespace atbus {
namespace protocol {
class msg;
}
class node;
class endpoint;
class connection;
}  // namespace atbus

namespace atapp {

class etcd_module;

class app {
 public:
  using app_id_t = LIBATAPP_MACRO_BUSID_TYPE;
  using module_ptr_t = std::shared_ptr<module_impl>;
  using yaml_conf_map_t = atbus::detail::auto_select_map<std::string, std::vector<YAML::Node> >::type;
  using endpoint_index_by_id_t = LIBATFRAME_UTILS_AUTO_SELETC_MAP(uint64_t, atapp_endpoint::ptr_t);
  using endpoint_index_by_name_t = LIBATFRAME_UTILS_AUTO_SELETC_MAP(std::string, atapp_endpoint::ptr_t);
  using connector_protocol_map_t = LIBATFRAME_UTILS_AUTO_SELETC_MAP(std::string, std::shared_ptr<atapp_connector_impl>);
  using address_type_t = atapp_connector_impl::address_type_t;
  using ev_loop_t = uv_loop_t;

  struct LIBATAPP_MACRO_API_HEAD_ONLY flag_t {
    enum type {
      RUNNING = 0,  //
      STOPING,      //
      TIMEOUT,
      IN_CALLBACK,
      RESET_TIMER,
      INITIALIZED,
      STOPPED,
      DISABLE_ATBUS_FALLBACK,
      FLAG_MAX
    };
  };

  struct message_t {
    int32_t type;
    uint64_t msg_sequence;
    const void *data;
    size_t data_size;
    const atapp::protocol::atapp_metadata *metadata;

    LIBATAPP_MACRO_API message_t();
    LIBATAPP_MACRO_API ~message_t();
    LIBATAPP_MACRO_API message_t(const message_t &);
    LIBATAPP_MACRO_API message_t &operator=(const message_t &);
  };

  struct message_sender_t {
    app_id_t id;
    const std::string *name;
    atapp_endpoint *remote;
    LIBATAPP_MACRO_API message_sender_t();
    LIBATAPP_MACRO_API ~message_sender_t();
    LIBATAPP_MACRO_API message_sender_t(const message_sender_t &);
    LIBATAPP_MACRO_API message_sender_t &operator=(const message_sender_t &);
  };

  class flag_guard_t {
   public:
    LIBATAPP_MACRO_API flag_guard_t(app &owner, flag_t::type f);
    LIBATAPP_MACRO_API ~flag_guard_t();

   private:
    flag_guard_t(const flag_guard_t &);

    app *owner_;
    flag_t::type flag_;
  };
  friend class flag_guard_t;

  struct LIBATAPP_MACRO_API_HEAD_ONLY mode_t {
    enum type {
      CUSTOM = 0,  // custom command
      START,       // start server
      STOP,        // send a stop command
      RELOAD,      // send a reload command
      INFO,        // show information and exit
      HELP,        // show help and exit
      MODE_MAX
    };
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY custom_command_sender_t {
    app *self;
    std::list<std::string> *response;
  };

  struct timer_info_t {
    uv_timer_t timer;
  };

  // return > 0 means busy and will enter tick again as soon as possiable
  using tick_handler_t = std::function<int()>;
  using timer_ptr_t = std::shared_ptr<timer_info_t>;

  struct tick_timer_t {
    util::time::time_utility::raw_time_t sec_update;
    time_t sec;
    time_t usec;
    util::time::time_utility::raw_time_t *inner_break;

    timer_ptr_t tick_timer;
    timer_ptr_t timeout_timer;
  };

  // void on_forward_response(atapp_connection_handle* handle, int32_t type, uint64_t msg_sequence, int32_t error_code,
  //    const void* data, size_t data_size, const atapp::protocol::atapp_metadata* metadata)
  using callback_fn_on_forward_request_t = std::function<int(app &, const message_sender_t &, const message_t &m)>;
  using callback_fn_on_forward_response_t =
      std::function<int(app &, const message_sender_t &, const message_t &m, int32_t)>;
  using callback_fn_on_connected_t = std::function<int(app &, atbus::endpoint &, int)>;
  using callback_fn_on_disconnected_t = std::function<int(app &, atbus::endpoint &, int)>;
  using callback_fn_on_all_module_inited_t = std::function<int(app &)>;

  UTIL_DESIGN_PATTERN_NOCOPYABLE(app)
  UTIL_DESIGN_PATTERN_NOMOVABLE(app)
 public:
  LIBATAPP_MACRO_API app();
  LIBATAPP_MACRO_API ~app();

  /**
   * @brief run atapp loop until stop
   * @param ev_loop pointer to event loop
   * @param argc argument count for command line(include exec)
   * @param argv arguments for command line(include exec)
   * @param priv_data private data for custom option callbacks
   * @note you can call init(ev_loop, argc, argv, priv_data), and then call run(NULL, 0, NULL).
   * @return 0 or error code
   */
  LIBATAPP_MACRO_API int run(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data = NULL);

  /**
   * @brief initialize atapp
   * @param ev_loop pointer to event loop
   * @param argc argument count for command line(include exec)
   * @param argv arguments for command line(include exec)
   * @param priv_data private data for custom option callbacks
   * @return 0 or error code
   */
  LIBATAPP_MACRO_API int init(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data = NULL);

  /**
   * @brief run atapp loop but noblock if there is no event
   * @note you must call init(ev_loop, argc, argv, priv_data), before call run_noblock().
   * @param max_event_count max event in once call
   * @return 0 for no more actions or error code < 0 or 1 for there is pending actions
   */
  LIBATAPP_MACRO_API int run_noblock(uint64_t max_event_count = 20000);

  /**
   * @brief run atapp loop once
   * @note you must call init(ev_loop, argc, argv, priv_data), before call run_noblock().
   * @param min_event_count min event in once call, 0 for any inner action in event loop, not just the logic event
   * @param timeout_miliseconds timeout in miliseconds if no action happen
   * @return error code < 0 or logic event count, 0 for inner action event or timeout
   */
  LIBATAPP_MACRO_API int run_once(uint64_t min_event_count = 0, time_t timeout_miliseconds = 10000);

  LIBATAPP_MACRO_API bool is_inited() const UTIL_CONFIG_NOEXCEPT;

  LIBATAPP_MACRO_API bool is_running() const UTIL_CONFIG_NOEXCEPT;

  LIBATAPP_MACRO_API bool is_closing() const UTIL_CONFIG_NOEXCEPT;

  LIBATAPP_MACRO_API bool is_closed() const UTIL_CONFIG_NOEXCEPT;

  LIBATAPP_MACRO_API int reload();

  LIBATAPP_MACRO_API int stop();

  LIBATAPP_MACRO_API int tick();

  LIBATAPP_MACRO_API app_id_t get_id() const;

  LIBATAPP_MACRO_API ev_loop_t *get_evloop();

  LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char *id_in) const;
  LIBATAPP_MACRO_API std::string convert_app_id_to_string(app_id_t id_in, bool hex = false) const;

  LIBATAPP_MACRO_API bool check_flag(flag_t::type f) const;

  /**
   * @brief add a new module
   */
  LIBATAPP_MACRO_API void add_module(module_ptr_t module);

  /**
   * @brief convert module type and add a new module
   */
  template <typename TModPtr>
  LIBATAPP_MACRO_API_HEAD_ONLY void add_module(TModPtr module) {
    add_module(std::dynamic_pointer_cast<module_impl>(module));
  }

  /**
   * @brief api: add custom command callback
   * @return the command manager
   */
  LIBATAPP_MACRO_API util::cli::cmd_option_ci::ptr_type get_command_manager();

  /**
   * @brief api: add custem program options
   * @return the program options manager
   */
  LIBATAPP_MACRO_API util::cli::cmd_option::ptr_type get_option_manager();

  /**
   * @brief api: if last command or action run with upgrade mode
   * @return true if last command or action run with upgrade mode
   */
  LIBATAPP_MACRO_API bool is_current_upgrade_mode() const;

  /**
   * @brief api: get shared httr_request context, this context can be reused to create util::network::http_request
   * @note All the http_requests created from this context should be cleaned before atapp is destroyed.
   * @note this function only return a valid context after initialized.
   * @return the shared http_request context
   */
  LIBATAPP_MACRO_API util::network::http_request::curl_m_bind_ptr_t get_shared_curl_multi_context() const;

  LIBATAPP_MACRO_API void set_app_version(const std::string &ver);

  LIBATAPP_MACRO_API const std::string &get_app_version() const;

  LIBATAPP_MACRO_API void set_build_version(const std::string &ver);

  LIBATAPP_MACRO_API const std::string &get_build_version() const;

  LIBATAPP_MACRO_API const std::string &get_app_name() const;

  LIBATAPP_MACRO_API const std::string &get_app_identity() const;

  LIBATAPP_MACRO_API const std::string &get_type_name() const;

  LIBATAPP_MACRO_API app_id_t get_type_id() const;

  LIBATAPP_MACRO_API const std::string &get_hash_code() const;

  LIBATAPP_MACRO_API std::shared_ptr<atbus::node> get_bus_node();
  LIBATAPP_MACRO_API const std::shared_ptr<atbus::node> get_bus_node() const;

  LIBATAPP_MACRO_API void enable_fallback_to_atbus_connector();
  LIBATAPP_MACRO_API void disable_fallback_to_atbus_connector();
  LIBATAPP_MACRO_API bool is_fallback_to_atbus_connector_enabled() const;

  LIBATAPP_MACRO_API util::time::time_utility::raw_time_t get_last_tick_time() const;

  LIBATAPP_MACRO_API util::config::ini_loader &get_configure_loader();
  LIBATAPP_MACRO_API const util::config::ini_loader &get_configure_loader() const;

  /**
   * @brief get yaml configure loaders
   * @note Be careful yaml API may throw a exception
   * @return yaml configure loaders
   */
  LIBATAPP_MACRO_API yaml_conf_map_t &get_yaml_loaders();

  /**
   * @brief get yaml configure loaders
   * @note Be careful yaml API may throw a exception
   * @return yaml configure loaders
   */
  LIBATAPP_MACRO_API const yaml_conf_map_t &get_yaml_loaders() const;

  LIBATAPP_MACRO_API void parse_configures_into(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                const std::string &path) const;

  LIBATAPP_MACRO_API const atapp::protocol::atapp_configure &get_origin_configure() const;
  LIBATAPP_MACRO_API const atapp::protocol::atapp_metadata &get_metadata() const;
  LIBATAPP_MACRO_API atapp::protocol::atapp_metadata &mutable_metadata();
  LIBATAPP_MACRO_API const atapp::protocol::atapp_area &get_area() const;
  LIBATAPP_MACRO_API atapp::protocol::atapp_area &mutable_area();
  LIBATAPP_MACRO_API util::time::time_utility::raw_duration_t get_configure_message_timeout() const;

  LIBATAPP_MACRO_API void pack(atapp::protocol::atapp_discovery &out) const;

  LIBATAPP_MACRO_API std::shared_ptr< ::atapp::etcd_module> get_etcd_module() const;

  LIBATAPP_MACRO_API uint32_t get_address_type(const std::string &addr) const;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_discovery_node_by_id(uint64_t id) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_discovery_node_by_name(const std::string &name) const;

  LIBATAPP_MACRO_API int32_t listen(const std::string &address);
  LIBATAPP_MACRO_API int32_t send_message(uint64_t target_node_id, int32_t type, const void *data, size_t data_size,
                                          uint64_t *msg_sequence = NULL,
                                          const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message(const std::string &target_node_name, int32_t type, const void *data,
                                          size_t data_size, uint64_t *msg_sequence = NULL,
                                          const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message(const etcd_discovery_node::ptr_t &target_node_discovery, int32_t type,
                                          const void *data, size_t data_size, uint64_t *msg_sequence = NULL,
                                          const atapp::protocol::atapp_metadata *metadata = NULL);

  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                             const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(uint64_t hash_key, int32_t type, const void *data,
                                                             size_t data_size, uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(int64_t hash_key, int32_t type, const void *data,
                                                             size_t data_size, uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const std::string &hash_key, int32_t type,
                                                             const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);

  LIBATAPP_MACRO_API int32_t send_message_by_random(int32_t type, const void *data, size_t data_size,
                                                    uint64_t *msg_sequence = NULL,
                                                    const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_round_robin(int32_t type, const void *data, size_t data_size,
                                                         uint64_t *msg_sequence = NULL,
                                                         const atapp::protocol::atapp_metadata *metadata = NULL);

  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                             const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                             const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const etcd_discovery_set &discovery_set, uint64_t hash_key,
                                                             int32_t type, const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const etcd_discovery_set &discovery_set, int64_t hash_key,
                                                             int32_t type, const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                             const std::string &hash_key, int32_t type,
                                                             const void *data, size_t data_size,
                                                             uint64_t *msg_sequence = NULL,
                                                             const atapp::protocol::atapp_metadata *metadata = NULL);

  LIBATAPP_MACRO_API int32_t send_message_by_random(const etcd_discovery_set &discovery_set, int32_t type,
                                                    const void *data, size_t data_size, uint64_t *msg_sequence = NULL,
                                                    const atapp::protocol::atapp_metadata *metadata = NULL);
  LIBATAPP_MACRO_API int32_t send_message_by_round_robin(const etcd_discovery_set &discovery_set, int32_t type,
                                                         const void *data, size_t data_size,
                                                         uint64_t *msg_sequence = NULL,
                                                         const atapp::protocol::atapp_metadata *metadata = NULL);

  /**
   * @brief add log sink maker, this function allow user to add custom log sink from the configure of atapp
   */
  LIBATAPP_MACRO_API bool add_log_sink_maker(const std::string &name, log_sink_maker::log_reg_t fn);

  LIBATAPP_MACRO_API void set_evt_on_forward_request(callback_fn_on_forward_request_t fn);
  LIBATAPP_MACRO_API void set_evt_on_forward_response(callback_fn_on_forward_response_t fn);
  LIBATAPP_MACRO_API void set_evt_on_app_connected(callback_fn_on_connected_t fn);
  LIBATAPP_MACRO_API void set_evt_on_app_disconnected(callback_fn_on_disconnected_t fn);
  LIBATAPP_MACRO_API void set_evt_on_all_module_inited(callback_fn_on_all_module_inited_t fn);

  LIBATAPP_MACRO_API const callback_fn_on_forward_request_t &get_evt_on_forward_request() const;
  LIBATAPP_MACRO_API const callback_fn_on_forward_response_t &get_evt_on_forward_response() const;
  LIBATAPP_MACRO_API const callback_fn_on_connected_t &get_evt_on_app_connected() const;
  LIBATAPP_MACRO_API const callback_fn_on_disconnected_t &get_evt_on_app_disconnected() const;
  LIBATAPP_MACRO_API const callback_fn_on_all_module_inited_t &get_evt_on_all_module_inited() const;

  LIBATAPP_MACRO_API bool add_endpoint_waker(util::time::time_utility::raw_time_t wakeup_time,
                                             const atapp_endpoint::weak_ptr_t &ep_watcher);
  LIBATAPP_MACRO_API void remove_endpoint(uint64_t by_id);
  LIBATAPP_MACRO_API void remove_endpoint(const std::string &by_name);
  LIBATAPP_MACRO_API void remove_endpoint(const atapp_endpoint::ptr_t &enpoint);
  LIBATAPP_MACRO_API atapp_endpoint::ptr_t mutable_endpoint(const etcd_discovery_node::ptr_t &discovery);
  LIBATAPP_MACRO_API atapp_endpoint *get_endpoint(uint64_t by_id);
  LIBATAPP_MACRO_API const atapp_endpoint *get_endpoint(uint64_t by_id) const;
  LIBATAPP_MACRO_API atapp_endpoint *get_endpoint(const std::string &by_name);
  LIBATAPP_MACRO_API const atapp_endpoint *get_endpoint(const std::string &by_name) const;

  template <class TCONNECTOR, class... TARGS>
  LIBATAPP_MACRO_API_HEAD_ONLY std::shared_ptr<TCONNECTOR> add_connector(TARGS &&...args) {
    std::shared_ptr<TCONNECTOR> ret = std::make_shared<TCONNECTOR>(*this, std::forward<TARGS>(args)...);
    if (ret) {
      add_connector_inner(std::static_pointer_cast<atapp_connector_impl>(ret));
    }

    return ret;
  }

  LIBATAPP_MACRO_API bool match_gateway(const atapp::protocol::atapp_gateway &checked) const;

 private:
  static void ev_stop_timeout(uv_timer_t *handle);

  bool set_flag(flag_t::type f, bool v);

  int apply_configure();

  void run_ev_loop(int run_mode);

  int run_inner(int run_mode);

  int setup_signal();

  void setup_option(int argc, const char *argv[], void *priv_data);

  void setup_command();

  void setup_startup_log();

  int setup_log();

  int setup_atbus();

  void close_timer(timer_ptr_t &t);

  int setup_timer();

  int send_last_command(ev_loop_t *ev_loop);

  bool write_pidfile();
  bool cleanup_pidfile();
  void print_help();

  bool match_gateway_hosts(const atapp::protocol::atapp_gateway &checked) const;
  bool match_gateway_namespace(const atapp::protocol::atapp_gateway &checked) const;
  bool match_gateway_labels(const atapp::protocol::atapp_gateway &checked) const;

  // ============ inner functional handlers ============

 public:
  static LIBATAPP_MACRO_API custom_command_sender_t get_custom_command_sender(util::cli::callback_param);
  static LIBATAPP_MACRO_API bool add_custom_command_rsp(util::cli::callback_param, const std::string &rsp_text);
  static LIBATAPP_MACRO_API void split_ids_by_string(const char *in, std::vector<app_id_t> &out);
  static LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char *id_in, const std::vector<app_id_t> &mask_in);
  static LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char *id_in, const char *mask_in);
  static LIBATAPP_MACRO_API std::string convert_app_id_to_string(app_id_t id_in, const std::vector<app_id_t> &mask_in,
                                                                 bool hex = false);
  static LIBATAPP_MACRO_API std::string convert_app_id_to_string(app_id_t id_in, const char *mask_in, bool hex = false);

  /**
   * @brief get last instance
   * @note This API is not thread-safety and only usageful when there is only one app instance.
   *       If libatapp is built as static library, this API should only be used when only linked into one executable or
   * dynamic library. You should build libatapp as dynamic library when linked into more than one target.
   */
  static LIBATAPP_MACRO_API app *get_last_instance();

 private:
  int prog_option_handler_help(util::cli::callback_param params, util::cli::cmd_option *opt_mgr,
                               util::cli::cmd_option_ci *cmd_mgr);
  int prog_option_handler_version(util::cli::callback_param params);
  int prog_option_handler_set_id(util::cli::callback_param params);
  int prog_option_handler_set_id_mask(util::cli::callback_param params);
  int prog_option_handler_set_conf_file(util::cli::callback_param params);
  int prog_option_handler_set_pid(util::cli::callback_param params);
  int prog_option_handler_upgrade_mode(util::cli::callback_param params);
  int prog_option_handler_set_startup_log(util::cli::callback_param params);
  int prog_option_handler_start(util::cli::callback_param params);
  int prog_option_handler_stop(util::cli::callback_param params);
  int prog_option_handler_reload(util::cli::callback_param params);
  int prog_option_handler_run(util::cli::callback_param params);

  int command_handler_start(util::cli::callback_param params);
  int command_handler_stop(util::cli::callback_param params);
  int command_handler_reload(util::cli::callback_param params);
  int command_handler_invalid(util::cli::callback_param params);
  int command_handler_disable_etcd(util::cli::callback_param params);
  int command_handler_enable_etcd(util::cli::callback_param params);
  int command_handler_list_discovery(util::cli::callback_param params);

 private:
  int bus_evt_callback_on_recv_msg(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                   const atbus::protocol::msg &, const void *, size_t);
  int bus_evt_callback_on_forward_response(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                           const atbus::protocol::msg *m);
  int bus_evt_callback_on_error(const atbus::node &, const atbus::endpoint *, const atbus::connection *, int, int);
  int bus_evt_callback_on_info_log(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                   const char *);
  int bus_evt_callback_on_reg(const atbus::node &, const atbus::endpoint *, const atbus::connection *, int);
  int bus_evt_callback_on_shutdown(const atbus::node &, int);
  int bus_evt_callback_on_available(const atbus::node &, int);
  int bus_evt_callback_on_invalid_connection(const atbus::node &, const atbus::connection *, int);
  int bus_evt_callback_on_custom_cmd(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t,
                                     const std::vector<std::pair<const void *, size_t> > &, std::list<std::string> &);
  int bus_evt_callback_on_add_endpoint(const atbus::node &, atbus::endpoint *, int);
  int bus_evt_callback_on_remove_endpoint(const atbus::node &, atbus::endpoint *, int);
  int bus_evt_callback_on_custom_rsp(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t,
                                     const std::vector<std::pair<const void *, size_t> > &, uint64_t);

  LIBATAPP_MACRO_API void add_connector_inner(std::shared_ptr<atapp_connector_impl> connector);

  /** this function should always not be used outside atapp.cpp **/
  static void _app_setup_signal_handle(int signo);
  void process_signals();
  void process_signal(int signo);

 public:
  LIBATAPP_MACRO_API int trigger_event_on_forward_request(const message_sender_t &source, const message_t &msg);
  LIBATAPP_MACRO_API int trigger_event_on_forward_response(const message_sender_t &source, const message_t &msg,
                                                           int32_t error_code);
  LIBATAPP_MACRO_API void trigger_event_on_discovery_event(etcd_discovery_action_t::type,
                                                           const etcd_discovery_node::ptr_t &);

 private:
  static app *last_instance_;
  util::config::ini_loader cfg_loader_;
  yaml_conf_map_t yaml_loader_;
  util::cli::cmd_option::ptr_type app_option_;
  util::cli::cmd_option_ci::ptr_type cmd_handler_;
  std::vector<std::string> last_command_;
  int setup_result_;

  enum inner_options_t {
    // POSIX: _POSIX_SIGQUEUE_MAX on most platform is 32
    // RLIMIT_SIGPENDING on most linux distributions is 11
    // We use 32 here
    MAX_SIGNAL_COUNT = 32,
  };
  int pending_signals_[MAX_SIGNAL_COUNT];
  uint64_t last_proc_event_count_;

  app_conf conf_;
  mutable std::string build_version_;

  ev_loop_t *ev_loop_;
  std::shared_ptr<atbus::node> bus_node_;
  std::bitset<flag_t::FLAG_MAX> flags_;
  mode_t::type mode_;
  tick_timer_t tick_timer_;

  std::vector<module_ptr_t> modules_;
  std::map<std::string, log_sink_maker::log_reg_t>
      log_reg_;  // log reg will not changed or be checked outside the init, so std::map is enough

  // callbacks
  callback_fn_on_forward_request_t evt_on_forward_request_;
  callback_fn_on_forward_response_t evt_on_forward_response_;
  callback_fn_on_connected_t evt_on_app_connected_;
  callback_fn_on_disconnected_t evt_on_app_disconnected_;
  callback_fn_on_all_module_inited_t evt_on_all_module_inited_;

  // stat
  struct stat_data_t {
    uv_rusage_t last_checkpoint_usage;
    time_t last_checkpoint_min;

    size_t endpoint_wake_count;
    ::atapp::etcd_cluster::stats_t inner_etcd;
  };
  stat_data_t stat_;

  // inner modules
  std::shared_ptr< ::atapp::etcd_module> inner_module_etcd_;

  // inner endpoints
  endpoint_index_by_id_t endpoint_index_by_id_;
  endpoint_index_by_name_t endpoint_index_by_name_;
  std::multimap<util::time::time_utility::raw_time_t, atapp_endpoint::weak_ptr_t> endpoint_waker_;

  // inner connectors
  std::list<std::shared_ptr<atapp_connector_impl> > connectors_;
  connector_protocol_map_t connector_protocols_;
  std::shared_ptr<atapp_connector_atbus> atbus_connector_;
};
}  // namespace atapp

#endif
