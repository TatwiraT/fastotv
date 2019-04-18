/*  Copyright (C) 2014-2018 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <common/libev/tcp/tcp_client.h>  // for TcpClient
#include <common/libev/tcp/tcp_server.h>  // for TcpServer

namespace common {
namespace libev {
class IoLoopObserver;
}
namespace net {
class socket_info;
struct HostAndPort;
}  // namespace net
}  // namespace common

namespace fastotv {
namespace server {
namespace inner {

class InnerTcpServer : public common::libev::tcp::TcpServer {
 public:
  InnerTcpServer(const common::net::HostAndPort& host, bool is_default, common::libev::IoLoopObserver* observer);
  const char* ClassName() const override;

 private:
  common::libev::tcp::TcpClient* CreateClient(const common::net::socket_info& info) override;
};

}  // namespace inner
}  // namespace server
}  // namespace fastotv
