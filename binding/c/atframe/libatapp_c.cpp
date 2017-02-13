
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>


#include "common/compiler_message.h"
#include "common/string_oprs.h"
#include "libatapp_c.h"

#include <atframe/atapp.h>

#include <atframe/libatapp_c.h>

#define ATAPP_CONTEXT(x) ((::atapp::app *)(x).pa)
#define ATAPP_CONTEXT_IS_NULL(x) (NULL == (x).pa)

#define ATAPP_MESSAGE(x) ((const ::atapp::app::msg_t *)(x).pa)
#define ATAPP_MESSAGE_IS_NULL(x) (NULL == (x).pa)

namespace detail {
    struct libatapp_c_on_msg_functor {
        libatapp_c_on_msg_functor(libatapp_c_on_msg_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

        int operator()(::atapp::app &self, const ::atapp::app::msg_t &msg, const void *buffer, size_t sz) {
            if (NULL == callee_) {
                return 0;
            }

            libatapp_c_context ctx;
            libatapp_c_message m;

            ctx.pa = &self;
            m.pa = &msg;
            return (*callee_)(ctx, m, buffer, static_cast<uint64_t>(sz), private_data_);
        }

        libatapp_c_on_msg_fn_t callee_;
        void *private_data_;
    };

    struct libatapp_c_on_send_fail_functor {
        libatapp_c_on_send_fail_functor(libatapp_c_on_send_fail_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

        int operator()(::atapp::app &self, ::atapp::app::app_id_t src_pd, ::atapp::app::app_id_t dst_pd, const ::atapp::app::msg_t &msg) {
            if (NULL == callee_) {
                return 0;
            }

            libatapp_c_context ctx;
            libatapp_c_message m;

            ctx.pa = &self;
            m.pa = &msg;
            return (*callee_)(ctx, static_cast<uint64_t>(src_pd), static_cast<uint64_t>(dst_pd), m, private_data_);
        }

        libatapp_c_on_send_fail_fn_t callee_;
        void *private_data_;
    };

    struct libatapp_c_on_connected_functor {
        libatapp_c_on_connected_functor(libatapp_c_on_connected_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

        int operator()(::atapp::app &self, ::atbus::endpoint &ep, int status) {
            if (NULL == callee_) {
                return 0;
            }

            libatapp_c_context ctx;
            ctx.pa = &self;
            return (*callee_)(ctx, static_cast<uint64_t>(ep.get_id()), static_cast<int32_t>(status), private_data_);
        }

        libatapp_c_on_connected_fn_t callee_;
        void *private_data_;
    };

    struct libatapp_c_on_disconnected_functor {
        libatapp_c_on_disconnected_functor(libatapp_c_on_disconnected_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

        int operator()(::atapp::app &self, ::atbus::endpoint &ep, int status) {
            if (NULL == callee_) {
                return 0;
            }

            libatapp_c_context ctx;
            ctx.pa = &self;
            return (*callee_)(ctx, static_cast<uint64_t>(ep.get_id()), static_cast<int32_t>(status), private_data_);
        }

        libatapp_c_on_disconnected_fn_t callee_;
        void *private_data_;
    };

    struct libatapp_c_on_all_module_inited_functor {
        libatapp_c_on_all_module_inited_functor(libatapp_c_on_all_module_inited_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

        int operator()(::atapp::app &self) {
            if (NULL == callee_) {
                return 0;
            }

            libatapp_c_context ctx;
            ctx.pa = &self;
            return (*callee_)(ctx, private_data_);
        }

        libatapp_c_on_all_module_inited_fn_t callee_;
        void *private_data_;
    };

    class libatapp_c_on_module : public ::atapp::module_impl {
    public:
        libatapp_c_on_module(const char *name) {
            name_ = name;

            on_init_ = NULL;
            on_reload_ = NULL;
            on_stop_ = NULL;
            on_timeout_ = NULL;
            on_tick_ = NULL;
            on_init_private_data_ = NULL;
            on_reload_private_data_ = NULL;
            on_stop_private_data_ = NULL;
            on_timeout_private_data_ = NULL;
            on_tick_private_data_ = NULL;
        }

        virtual int init() {
            if (NULL != on_init_) {
                libatapp_c_module mod;
                mod.pa = this;
                (*on_init_)(mod, on_init_private_data_);
            }
            return 0;
        };

        virtual int reload() {
            if (NULL != on_reload_) {
                libatapp_c_module mod;
                mod.pa = this;
                (*on_reload_)(mod, on_reload_private_data_);
            }
            return 0;
        }

        virtual int stop() {
            if (NULL != on_stop_) {
                libatapp_c_module mod;
                mod.pa = this;
                (*on_stop_)(mod, on_stop_private_data_);
            }
            return 0;
        }

        virtual int timeout() {
            if (NULL != on_timeout_) {
                libatapp_c_module mod;
                mod.pa = this;
                (*on_timeout_)(mod, on_timeout_private_data_);
            }
            return 0;
        }

        virtual const char *name() const { return name_.c_str(); }

        virtual int tick() {
            if (NULL != on_tick_) {
                libatapp_c_module mod;
                mod.pa = this;
                (*on_tick_)(mod, on_tick_private_data_);
            }

            return 0;
        }

        using ::atapp::module_impl::get_app;

    public:
        libatapp_c_module_on_init_fn_t on_init_;
        void *on_init_private_data_;
        libatapp_c_module_on_reload_fn_t on_reload_;
        void *on_reload_private_data_;
        libatapp_c_module_on_stop_fn_t on_stop_;
        void *on_stop_private_data_;
        libatapp_c_module_on_timeout_fn_t on_timeout_;
        void *on_timeout_private_data_;
        libatapp_c_module_on_tick_fn_t on_tick_;
        void *on_tick_private_data_;

    private:
        std::string name_;
    };
}

#define ATAPP_MODULE(x) ((::detail::libatapp_c_on_module *)(x).pa)
#define ATAPP_MODULE_IS_NULL(x) (NULL == (x).pa)

#ifdef __cplusplus
extern "C" {
#endif

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_set_on_msg_fn(libatapp_c_context context, libatapp_c_on_msg_fn_t fn, void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return;
    }

    return ATAPP_CONTEXT(context)->set_evt_on_recv_msg(::detail::libatapp_c_on_msg_functor(fn, priv_data));
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_set_on_send_fail_fn(libatapp_c_context context, libatapp_c_on_send_fail_fn_t fn, void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return;
    }

    return ATAPP_CONTEXT(context)->set_evt_on_send_fail(::detail::libatapp_c_on_send_fail_functor(fn, priv_data));
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_set_on_connected_fn(libatapp_c_context context, libatapp_c_on_connected_fn_t fn, void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return;
    }

    return ATAPP_CONTEXT(context)->set_evt_on_app_connected(::detail::libatapp_c_on_connected_functor(fn, priv_data));
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_set_on_disconnected_fn(libatapp_c_context context, libatapp_c_on_disconnected_fn_t fn, void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return;
    }

    return ATAPP_CONTEXT(context)->set_evt_on_app_disconnected(::detail::libatapp_c_on_disconnected_functor(fn, priv_data));
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_set_on_all_module_inited_fn(libatapp_c_context context, libatapp_c_on_all_module_inited_fn_t fn,
                                                                          void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return;
    }

    return ATAPP_CONTEXT(context)->set_evt_on_all_module_inited(::detail::libatapp_c_on_all_module_inited_functor(fn, priv_data));
}

ATFRAME_SYMBOL_EXPORT libatapp_c_context __cdecl libatapp_c_create() {
    libatapp_c_context ret;
    assert(sizeof(void *) == sizeof(libatapp_c_context));

    atapp::app *res = new (std::nothrow) atapp::app();
    ret.pa = res;
    return ret;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_destroy(libatapp_c_context context) { delete ATAPP_CONTEXT(context); }

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_run(libatapp_c_context context, int32_t argc, const char **argv, void *priv_data) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->run(uv_default_loop(), argc, argv, priv_data);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_reload(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->reload();
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_stop(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->stop();
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_tick(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->tick();
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_get_id(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    return ATAPP_CONTEXT(context)->get_id();
}

ATFRAME_SYMBOL_EXPORT const char *__cdecl libatapp_c_get_app_version(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return "";
    }

    return ATAPP_CONTEXT(context)->get_app_version().c_str();
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_get_configure_size(libatapp_c_context context, const char *path) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    return static_cast<uint64_t>(ATAPP_CONTEXT(context)->get_configure().get_node(path).size());
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_get_configure(libatapp_c_context context, const char *path, const char *out_buf[], uint64_t out_len[],
                                                                uint64_t arr_sz) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    util::config::ini_value &val = ATAPP_CONTEXT(context)->get_configure().get_node(path);
    uint64_t ret = 0;
    for (ret = 0; ret < val.size() && ret < arr_sz; ++ret) {
        out_buf[ret] = val.as_cpp_string(static_cast<size_t>(ret)).c_str();
        out_len[ret] = static_cast<uint64_t>(val.as_cpp_string(static_cast<size_t>(ret)).size());
    }

    return ret;
}


ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_is_running(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    return ATAPP_CONTEXT(context)->check(atapp::app::flag_t::RUNNING);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_is_stoping(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    return ATAPP_CONTEXT(context)->check(atapp::app::flag_t::STOPING);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_is_timeout(libatapp_c_context context) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return 0;
    }

    return ATAPP_CONTEXT(context)->check(atapp::app::flag_t::TIMEOUT);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_listen(libatapp_c_context context, const char *address) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->get_bus_node()->listen(address);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_connect(libatapp_c_context context, const char *address) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->get_bus_node()->connect(address);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_disconnect(libatapp_c_context context, uint64_t app_id) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->get_bus_node()->disconnect(app_id);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_send_data_msg(libatapp_c_context context, uint64_t app_id, int32_t type, const void *buffer, uint64_t sz,
                                                               int32_t require_rsp) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    return ATAPP_CONTEXT(context)->get_bus_node()->send_data(app_id, type, buffer, sz, 0 != require_rsp);
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_send_custom_msg(libatapp_c_context context, uint64_t app_id, const void *arr_buf[], uint64_t arr_size[],
                                                                 uint64_t arr_count) {
    if (ATAPP_CONTEXT_IS_NULL(context)) {
        return EN_ATBUS_ERR_PARAMS;
    }

    std::vector<size_t> szs;
    szs.resize(arr_count);
    for (uint64_t i = 0; i < arr_count; ++i) {
        szs[i] = arr_size[i];
    }

    return ATAPP_CONTEXT(context)->get_bus_node()->send_custom_cmd(app_id, arr_buf, &szs[0], arr_count);
}


ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_msg_get_cmd(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->head.cmd;
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_msg_get_type(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->head.type;
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_msg_get_ret(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->head.ret;
}

ATFRAME_SYMBOL_EXPORT uint32_t __cdecl libatapp_c_msg_get_sequence(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->head.sequence;
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_msg_get_src_bus_id(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->head.src_bus_id;
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_msg_get_forward_from(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    if (NULL == ATAPP_MESSAGE(msg)->body.forward) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->body.forward->from;
}

ATFRAME_SYMBOL_EXPORT uint64_t __cdecl libatapp_c_msg_get_forward_to(libatapp_c_message msg) {
    if (ATAPP_MESSAGE_IS_NULL(msg)) {
        return 0;
    }

    if (NULL == ATAPP_MESSAGE(msg)->body.forward) {
        return 0;
    }

    return ATAPP_MESSAGE(msg)->body.forward->to;
}


ATFRAME_SYMBOL_EXPORT libatapp_c_module __cdecl libatapp_c_module_create(libatapp_c_context context, const char *mod_name) {
    libatapp_c_module ret;
    ret.pa = NULL;
    if (ATAPP_CONTEXT_IS_NULL(context) || NULL == mod_name) {
        return ret;
    }

    std::shared_ptr< ::detail::libatapp_c_on_module> res = std::make_shared< ::detail::libatapp_c_on_module>(mod_name);
    if (!res) {
        return ret;
    }
    ret.pa = res.get();
    ATAPP_CONTEXT(context)->add_module(res);

    return ret;
}

ATFRAME_SYMBOL_EXPORT const char *__cdecl libatapp_c_module_get_name(libatapp_c_module mod) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return "";
    }

    return ATAPP_MODULE(mod)->name();
}

ATFRAME_SYMBOL_EXPORT libatapp_c_context __cdecl libatapp_c_module_get_context(libatapp_c_module mod) {
    libatapp_c_context ret;
    ret.pa = NULL;

    if (ATAPP_MODULE_IS_NULL(mod)) {
        return ret;
    }

    ret.pa = ATAPP_MODULE(mod)->get_app();
    return ret;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_module_set_on_init(libatapp_c_module mod, libatapp_c_module_on_init_fn_t fn, void *priv_data) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return;
    }

    ATAPP_MODULE(mod)->on_init_ = fn;
    ATAPP_MODULE(mod)->on_init_private_data_ = priv_data;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_module_set_on_reload(libatapp_c_module mod, libatapp_c_module_on_reload_fn_t fn, void *priv_data) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return;
    }

    ATAPP_MODULE(mod)->on_reload_ = fn;
    ATAPP_MODULE(mod)->on_reload_private_data_ = priv_data;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_module_set_on_stop(libatapp_c_module mod, libatapp_c_module_on_stop_fn_t fn, void *priv_data) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return;
    }

    ATAPP_MODULE(mod)->on_stop_ = fn;
    ATAPP_MODULE(mod)->on_stop_private_data_ = priv_data;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_module_set_on_timeout(libatapp_c_module mod, libatapp_c_module_on_timeout_fn_t fn, void *priv_data) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return;
    }

    ATAPP_MODULE(mod)->on_timeout_ = fn;
    ATAPP_MODULE(mod)->on_timeout_private_data_ = priv_data;
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_module_set_on_tick(libatapp_c_module mod, libatapp_c_module_on_tick_fn_t fn, void *priv_data) {
    if (ATAPP_MODULE_IS_NULL(mod)) {
        return;
    }

    ATAPP_MODULE(mod)->on_tick_ = fn;
    ATAPP_MODULE(mod)->on_tick_private_data_ = priv_data;
}

ATFRAME_SYMBOL_EXPORT int64_t __cdecl libatapp_c_get_unix_timestamp() { return util::time::time_utility::get_now(); }

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_log_write(uint32_t tag, uint32_t level, const char *level_name, const char *file_path, const char *func_name,
                                                        uint32_t line_number, const char *content) {

    util::log::log_wrapper *log_cat = util::log::log_wrapper::mutable_log_cat(tag);
    if (NULL == log_cat) {
        return;
    }

    if (!log_cat->check(static_cast<util::log::log_wrapper::level_t::type>(level))) {
        return;
    }

    log_cat->log(
        util::log::log_formatter::caller_info_t(static_cast<util::log::log_wrapper::level_t::type>(level), level_name, file_path, line_number, func_name), "%s",
        content);
}

ATFRAME_SYMBOL_EXPORT void __cdecl libatapp_c_log_update() { util::log::log_wrapper::update(); }

ATFRAME_SYMBOL_EXPORT uint32_t __cdecl libatapp_c_log_get_level(uint32_t tag) {
    util::log::log_wrapper *log_cat = util::log::log_wrapper::mutable_log_cat(tag);
    if (NULL == log_cat) {
        return 0;
    }

    return log_cat->get_level();
}

ATFRAME_SYMBOL_EXPORT int32_t __cdecl libatapp_c_log_check_level(uint32_t tag, uint32_t level) {
    util::log::log_wrapper *log_cat = util::log::log_wrapper::mutable_log_cat(tag);
    if (NULL == log_cat) {
        return 0;
    }

    return log_cat->check(static_cast<util::log::log_wrapper::level_t::type>(level)) ? 1 : 0;
}


#ifdef __cplusplus
}
#endif