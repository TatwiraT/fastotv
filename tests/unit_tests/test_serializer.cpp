/*  Copyright (C) 2014-2019 FastoGT. All right reserved.

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

#include <gtest/gtest.h>

#include "commands_info/auth_info.h"
#include "commands_info/channel_info.h"
#include "commands_info/channels_info.h"
#include "commands_info/client_info.h"
#include "commands_info/ping_info.h"
#include "commands_info/runtime_channel_info.h"
#include "commands_info/server_info.h"

typedef fastotv::AuthInfo::serialize_type serialize_t;

TEST(ChannelInfo, serialize_deserialize) {
  const std::string name = "alex";
  const fastotv::stream_id stream_id = "123";
  const common::uri::Url url("http://localhost:8080/hls/69_avformat_test_alex_2/play.m3u8");
  const bool enable_video = false;
  const bool enable_audio = true;

  fastotv::EpgInfo epg_info(stream_id, url, name);
  ASSERT_EQ(epg_info.GetDisplayName(), name);
  ASSERT_EQ(epg_info.GetChannelID(), stream_id);
  ASSERT_EQ(epg_info.GetUrl(), url);

  serialize_t user;
  common::Error err = epg_info.Serialize(&user);
  ASSERT_TRUE(!err);
  fastotv::EpgInfo depg;
  err = depg.DeSerialize(user);
  ASSERT_TRUE(!err);

  ASSERT_EQ(epg_info, depg);

  fastotv::ChannelInfo http_uri(epg_info, enable_audio, enable_video);
  ASSERT_EQ(http_uri.GetName(), name);
  ASSERT_EQ(http_uri.GetID(), stream_id);
  ASSERT_EQ(http_uri.GetUrl(), url);
  ASSERT_EQ(http_uri.IsEnableAudio(), enable_audio);
  ASSERT_EQ(http_uri.IsEnableVideo(), enable_video);

  serialize_t ser;
  err = http_uri.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ChannelInfo dhttp_uri;
  err = dhttp_uri.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(http_uri, dhttp_uri);
}

TEST(ServerInfo, serialize_deserialize) {
  const common::net::HostAndPort hs = common::net::HostAndPort::CreateLocalHost(3554);

  fastotv::ServerInfo serv_info(hs);
  ASSERT_EQ(serv_info.GetBandwidthHost(), hs);

  serialize_t ser;
  common::Error err = serv_info.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ServerInfo dser;
  err = dser.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(serv_info.GetBandwidthHost(), dser.GetBandwidthHost());
}

TEST(ServerPingInfo, serialize_deserialize) {
  fastotv::ServerPingInfo ping_info;
  serialize_t ser;
  common::Error err = ping_info.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ServerPingInfo dser;
  err = dser.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(ping_info.GetTimeStamp(), dser.GetTimeStamp());
}

TEST(ClientPingInfo, serialize_deserialize) {
  fastotv::ClientPingInfo ping_info;
  serialize_t ser;
  common::Error err = ping_info.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ClientPingInfo dser;
  err = dser.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(ping_info.GetTimeStamp(), dser.GetTimeStamp());
}

TEST(ClientInfo, serialize_deserialize) {
  const fastotv::login_t login = "Alex";
  const std::string os = "Os";
  const std::string cpu_brand = "brand";
  const int64_t ram_total = 1;
  const int64_t ram_free = 2;
  const fastotv::bandwidth_t bandwidth = 5;

  fastotv::ClientInfo cinf(login, os, cpu_brand, ram_total, ram_free, bandwidth);
  ASSERT_EQ(cinf.GetLogin(), login);
  ASSERT_EQ(cinf.GetOs(), os);
  ASSERT_EQ(cinf.GetCpuBrand(), cpu_brand);
  ASSERT_EQ(cinf.GetRamTotal(), ram_total);
  ASSERT_EQ(cinf.GetRamFree(), ram_free);
  ASSERT_EQ(cinf.GetBandwidth(), bandwidth);

  serialize_t ser;
  common::Error err = cinf.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ClientInfo dcinf;
  err = dcinf.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(cinf.GetLogin(), dcinf.GetLogin());
  ASSERT_EQ(cinf.GetOs(), dcinf.GetOs());
  ASSERT_EQ(cinf.GetCpuBrand(), dcinf.GetCpuBrand());
  ASSERT_EQ(cinf.GetRamTotal(), dcinf.GetRamTotal());
  ASSERT_EQ(cinf.GetRamFree(), dcinf.GetRamFree());
  ASSERT_EQ(cinf.GetBandwidth(), dcinf.GetBandwidth());
}

TEST(channels_t, serialize_deserialize) {
  const std::string name = "alex";
  const fastotv::stream_id stream_id = "123";
  const common::uri::Url url("http://localhost:8080/hls/69_avformat_test_alex_2/play.m3u8");
  const bool enable_video = false;
  const bool enable_audio = true;

  fastotv::ChannelsInfo channels;
  fastotv::EpgInfo epg_info(stream_id, url, name);
  channels.AddChannel(fastotv::ChannelInfo(epg_info, enable_audio, enable_video));
  ASSERT_EQ(channels.GetSize(), 1);

  serialize_t ser;
  common::Error err = channels.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::ChannelsInfo dchannels;
  err = dchannels.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(channels, dchannels);
}

TEST(AuthInfo, serialize_deserialize) {
  const std::string login = "palec";
  const std::string password = "ff";
  const std::string device = "dev";
  fastotv::AuthInfo auth_info(login, password, device);
  ASSERT_EQ(auth_info.GetLogin(), login);
  ASSERT_EQ(auth_info.GetPassword(), password);
  ASSERT_EQ(auth_info.GetDeviceID(), device);
  serialize_t ser;
  common::Error err = auth_info.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::AuthInfo dser;
  err = dser.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(auth_info, dser);
}

TEST(RuntimeChannelInfo, serialize_deserialize) {
  const std::string channel_id = "1234";
  const size_t watchers = 7;
  const fastotv::ChannelType ct = fastotv::OFFICAL_CHANNEL;
  const bool chat_enabled = true;
  const bool chat_readonly = true;
  const std::vector<fastotv::ChatMessage> msgs = {
      fastotv::ChatMessage("1234", "alex", "test", fastotv::ChatMessage::MESSAGE)};
  fastotv::RuntimeChannelInfo rinf_info(channel_id, watchers, ct, chat_enabled, chat_readonly, msgs);
  ASSERT_EQ(rinf_info.GetChannelID(), channel_id);
  ASSERT_EQ(rinf_info.GetWatchersCount(), watchers);
  ASSERT_EQ(rinf_info.GetChannelType(), ct);
  ASSERT_EQ(rinf_info.IsChatEnabled(), chat_enabled);
  ASSERT_EQ(rinf_info.IsChatReadOnly(), chat_readonly);
  serialize_t ser;
  common::Error err = rinf_info.Serialize(&ser);
  ASSERT_TRUE(!err);
  fastotv::RuntimeChannelInfo dser;
  err = dser.DeSerialize(ser);
  ASSERT_TRUE(!err);

  ASSERT_EQ(rinf_info, dser);
}
