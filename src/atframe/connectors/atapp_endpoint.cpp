#include <limits>

#include <detail/libatbus_error.h>

#include <atframe/atapp.h>

#include <atframe/connectors/atapp_connector_impl.h>
#include <atframe/connectors/atapp_endpoint.h>

#ifdef max
#  undef max
#endif

namespace atapp {
LIBATAPP_MACRO_API atapp_endpoint::atapp_endpoint(app &owner, construct_helper_t &)
    : closing_(false),
      owner_(&owner),
      pending_message_size_(0)
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
      ,
      pending_message_count_(0)
#endif
{
  nearest_waker_ = std::chrono::system_clock::from_time_t(0);
}

LIBATAPP_MACRO_API atapp_endpoint::ptr_t atapp_endpoint::create(app &owner) {
  construct_helper_t helper;
  ptr_t ret = std::make_shared<atapp_endpoint>(owner, helper);
  if (ret) {
    ret->watcher_ = ret;

    FWLOGINFO("create atapp endpoint {}", reinterpret_cast<const void *>(ret.get()));
  }
  return ret;
}

LIBATAPP_MACRO_API atapp_endpoint::~atapp_endpoint() {
  reset();
  FWLOGINFO("destroy atapp endpoint {}", reinterpret_cast<const void *>(this));
}

void atapp_endpoint::reset() {
  if (closing_) {
    return;
  }
  closing_ = true;

  cancel_pending_messages();

  handle_set_t handles;
  handles.swap(refer_connections_);

  for (handle_set_t::const_iterator iter = handles.begin(); iter != handles.end(); ++iter) {
    if (*iter) {
      atapp_endpoint_bind_helper::unbind(**iter, *this);
    }
  }

  closing_ = false;
}

LIBATAPP_MACRO_API void atapp_endpoint::add_connection_handle(atapp_connection_handle &handle) {
  if (closing_) {
    return;
  }

  atapp_endpoint_bind_helper::bind(handle, *this);
}

LIBATAPP_MACRO_API void atapp_endpoint::remove_connection_handle(atapp_connection_handle &handle) {
  if (closing_) {
    return;
  }

  atapp_endpoint_bind_helper::unbind(handle, *this);
}

LIBATAPP_MACRO_API atapp_connection_handle *atapp_endpoint::get_ready_connection_handle() const UTIL_CONFIG_NOEXCEPT {
  for (handle_set_t::const_iterator iter = refer_connections_.begin(); iter != refer_connections_.end(); ++iter) {
    if (*iter && (*iter)->is_ready()) {
      return *iter;
    }
  }

  return NULL;
}

LIBATAPP_MACRO_API uint64_t atapp_endpoint::get_id() const UTIL_CONFIG_NOEXCEPT {
  if (!discovery_) {
    return 0;
  }

  return discovery_->get_discovery_info().id();
}

LIBATAPP_MACRO_API const std::string &atapp_endpoint::get_name() const UTIL_CONFIG_NOEXCEPT {
  if (!discovery_) {
    static std::string empty;
    return empty;
  }

  return discovery_->get_discovery_info().name();
}

LIBATAPP_MACRO_API const etcd_discovery_node::ptr_t &atapp_endpoint::get_discovery() const UTIL_CONFIG_NOEXCEPT {
  return discovery_;
}

LIBATAPP_MACRO_API void atapp_endpoint::update_discovery(const etcd_discovery_node::ptr_t &discovery)
    UTIL_CONFIG_NOEXCEPT {
  if (discovery_ == discovery) {
    return;
  }

  discovery_ = discovery;

  if (discovery) {
    FWLOGINFO("update atapp endpoint {} with {}({})", reinterpret_cast<const void *>(this),
              discovery->get_discovery_info().id(), discovery->get_discovery_info().name());
  }
}

LIBATAPP_MACRO_API int32_t atapp_endpoint::push_forward_message(int32_t type, uint64_t &msg_sequence, const void *data,
                                                                size_t data_size,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  // Closing
  if (closing_ || NULL == owner_) {
    do {
      atapp_connection_handle *handle = get_ready_connection_handle();
      if (NULL == handle) {
        break;
      }

      atapp_connector_impl *connector = handle->get_connector();
      if (NULL == connector) {
        break;
      }

      connector->on_receive_forward_response(handle, type, msg_sequence, EN_ATBUS_ERR_CLOSING, data, data_size,
                                             metadata);
    } while (false);
    return EN_ATBUS_ERR_CLOSING;
  }

  if (NULL == data || 0 == data_size) {
    return EN_ATBUS_ERR_SUCCESS;
  }

  // Has handle
  do {
    if (!pending_message_.empty()) {
      break;
    }

    atapp_connection_handle *handle = get_ready_connection_handle();
    if (NULL == handle) {
      break;
    }

    atapp_connector_impl *connector = handle->get_connector();
    if (NULL == connector) {
      break;
    }

    int32_t ret = connector->on_send_forward_request(handle, type, &msg_sequence, data, data_size, metadata);
    if (0 != ret) {
      connector->on_receive_forward_response(handle, type, msg_sequence, ret, data, data_size, metadata);
    }

    return ret;
  } while (false);

  // Failed to add to pending
  int32_t failed_error_code = 0;
  if (NULL != owner_) {
    uint64_t send_buffer_number = owner_->get_origin_configure().bus().send_buffer_number();
    uint64_t send_buffer_size = owner_->get_origin_configure().bus().send_buffer_size();
    if (send_buffer_number > 0 &&
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
        pending_message_count_ + 1 > send_buffer_number
#else
        pending_message_.size() + 1 > send_buffer_number
#endif
    ) {
      failed_error_code = EN_ATBUS_ERR_BUFF_LIMIT;
    }

    if (send_buffer_size > 0 && pending_message_size_ + data_size > send_buffer_size) {
      failed_error_code = EN_ATBUS_ERR_BUFF_LIMIT;
    }
  }

  if (failed_error_code != 0) {
    atapp_connection_handle *handle = get_ready_connection_handle();
    if (NULL == handle) {
      return failed_error_code;
    }

    atapp_connector_impl *connector = handle->get_connector();
    if (NULL == connector) {
      return failed_error_code;
    }

    connector->on_receive_forward_response(handle, type, msg_sequence, failed_error_code, data, data_size, metadata);
    return failed_error_code;
  }

  // Success to add to pending
  pending_message_.push_back(pending_message_t());
  pending_message_t &msg = pending_message_.back();
  msg.type = type;
  msg.msg_sequence = msg_sequence;
  msg.data.resize(data_size);
  msg.expired_timepoint = owner_->get_last_tick_time();
  msg.expired_timepoint += owner_->get_configure_message_timeout();
  memcpy(&msg.data[0], data, data_size);
  if (NULL != metadata) {
    msg.metadata.reset(new atapp::protocol::atapp_metadata());
    if (msg.metadata) {
      msg.metadata->CopyFrom(*metadata);
    }
  }

  pending_message_size_ += data_size;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  ++pending_message_count_;
#endif

  add_waker(msg.expired_timepoint);
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_endpoint::retry_pending_messages(const util::time::time_utility::raw_time_t &tick_time,
                                                                  int32_t max_count) {
  // Including equal
  if (nearest_waker_ <= tick_time) {
    nearest_waker_ = std::chrono::system_clock::from_time_t(0);
  }

  int ret = 0;
  if (pending_message_.empty()) {
    return ret;
  }

  if (max_count <= 0) {
    max_count = std::numeric_limits<int32_t>::max();
  }

  atapp_connection_handle *handle = get_ready_connection_handle();
  atapp_connector_impl *connector = NULL;
  if (NULL != handle) {
    connector = handle->get_connector();
  }

  while (!pending_message_.empty()) {
    pending_message_t &msg = pending_message_.front();

    int res = EN_ATBUS_ERR_NODE_TIMEOUT;
    // Support to send data after reconnected
    if (max_count > 0 && NULL != handle && NULL != connector) {
      --max_count;
      res = connector->on_send_forward_request(handle, msg.type, &msg.msg_sequence,
                                               reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                               msg.metadata.get());
    } else if (msg.expired_timepoint > tick_time) {
      break;
    }

    if (0 != res) {
      if (NULL != handle && NULL != connector) {
        connector->on_receive_forward_response(handle, msg.type, msg.msg_sequence, res,
                                               reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                               msg.metadata.get());
      }
    }

    ++ret;

    if (likely(pending_message_size_ >= msg.data.size())) {
      pending_message_size_ -= msg.data.size();
    } else {
      pending_message_size_ = 0;
    }
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    if (pending_message_count_ > 0) {
      --pending_message_count_;
    }
#endif
    pending_message_.pop_front();
  }

  if (pending_message_.empty()) {
    pending_message_size_ = 0;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    pending_message_count_ = 0;
#endif
  } else if (NULL != owner_) {
    add_waker(pending_message_.front().expired_timepoint);
  }

  return ret;
}

LIBATAPP_MACRO_API void atapp_endpoint::add_waker(util::time::time_utility::raw_time_t wakeup_time) {
  if (wakeup_time < nearest_waker_ || std::chrono::system_clock::to_time_t(nearest_waker_) == 0) {
    if (NULL != owner_) {
      if (owner_->add_endpoint_waker(wakeup_time, watcher_)) {
        nearest_waker_ = wakeup_time;
      }
    }
  }
}

void atapp_endpoint::cancel_pending_messages() {
  atapp_connection_handle *handle = get_ready_connection_handle();
  if (NULL == handle) {
    return;
  }

  atapp_connector_impl *connector = handle->get_connector();
  if (NULL == connector) {
    return;
  }

  while (!pending_message_.empty()) {
    const pending_message_t &msg = pending_message_.front();
    connector->on_receive_forward_response(handle, msg.type, msg.msg_sequence, EN_ATBUS_ERR_CLOSING,
                                           reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                           msg.metadata.get());

    if (likely(pending_message_size_ >= msg.data.size())) {
      pending_message_size_ -= msg.data.size();
    } else {
      pending_message_size_ = 0;
    }
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    if (pending_message_count_ > 0) {
      --pending_message_count_;
    }
#endif
    pending_message_.pop_front();
  }

  pending_message_size_ = 0;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  pending_message_count_ = 0;
#endif
}
}  // namespace atapp
