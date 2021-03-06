/*  Copyright (C) 2014-2019 FastoGT. All right reserved.
    This file is part of iptv_cloud.
    iptv_cloud is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    iptv_cloud is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with iptv_cloud.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <map>
#include <string>
#include <utility>

#include <common/libev/io_client.h>

#include "protocol/types.h"

namespace fastotv {
namespace protocol {

typedef uint32_t protocoled_size_t;  // sizeof 4 byte
enum { MAX_COMMAND_SIZE = 1024 * 32 };

namespace detail {
common::ErrnoError WriteRequest(common::libev::IoClient* client, const request_t& request) WARN_UNUSED_RESULT;
common::ErrnoError WriteResponce(common::libev::IoClient* client, const response_t& responce) WARN_UNUSED_RESULT;
common::ErrnoError ReadCommand(common::libev::IoClient* client, std::string* out) WARN_UNUSED_RESULT;
}  // namespace detail

template <typename Client>
class ProtocolClient : public Client {
 public:
  typedef Client base_class;

  typedef std::function<void(const response_t* responce)> callback_t;
  typedef std::pair<request_t, callback_t> request_save_entry_t;

  template <typename... Args>
  explicit ProtocolClient(Args... args) : base_class(args...) {}

  common::ErrnoError WriteRequest(const request_t& request, callback_t cb = callback_t()) WARN_UNUSED_RESULT {
    common::ErrnoError err = detail::WriteRequest(this, request);
    if (!err && !request.IsNotification()) {
      requests_queue_[request.id] = std::make_pair(request, cb);
    }
    return err;
  }

  common::ErrnoError WriteResponce(const response_t& responce) WARN_UNUSED_RESULT {
    return detail::WriteResponce(this, responce);
  }

  common::ErrnoError ReadCommand(std::string* out) WARN_UNUSED_RESULT { return detail::ReadCommand(this, out); }

  bool PopRequestByID(sequance_id_t sid, request_t* req, callback_t* cb = nullptr) {
    if (!req || !sid) {
      return false;
    }

    auto found_it = requests_queue_.find(sid);
    if (found_it == requests_queue_.end()) {
      return false;
    }

    request_save_entry_t it = found_it->second;
    *req = it.first;
    if (cb) {
      *cb = it.second;
    }
    requests_queue_.erase(found_it);
    return true;
  }

 private:
  std::map<sequance_id_t, request_save_entry_t> requests_queue_;
  using Client::Read;
  using Client::Write;
};

typedef ProtocolClient<common::libev::IoClient> protocol_client_t;

}  // namespace protocol
}  // namespace fastotv
