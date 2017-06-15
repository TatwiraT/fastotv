#include <gtest/gtest.h>

#include "server/responce_info.h"
#include "server/user_info.h"
#include "server/user_state_info.h"

typedef fasto::fastotv::ChannelInfo::serialize_type serialize_t;

TEST(UserInfo, serialize_deserialize) {
  const std::string login = "palecc";
  const std::string password = "faf";
  fasto::fastotv::AuthInfo auth_info(login, password);

  const std::string name = "alex";
  const fasto::fastotv::stream_id stream_id = "123";
  const common::uri::Uri url("http://localhost:8080/hls/69_avformat_test_alex_2/play.m3u8");
  const bool enable_video = false;
  const bool enable_audio = true;

  fasto::fastotv::EpgInfo epg_info(stream_id, url, name);
  fasto::fastotv::ChannelsInfo channel_info;
  channel_info.AddChannel(fasto::fastotv::ChannelInfo(epg_info, enable_audio, enable_video));

  fasto::fastotv::server::UserInfo uinf(auth_info, channel_info);
  ASSERT_EQ(uinf.GetAuthInfo(), auth_info);
  ASSERT_EQ(uinf.GetChannelInfo(), channel_info);

  serialize_t ser;
  common::Error err = uinf.Serialize(&ser);
  ASSERT_TRUE(!err);
  fasto::fastotv::server::UserInfo duinf;
  err = uinf.DeSerialize(ser, &duinf);
  ASSERT_TRUE(!err);

  ASSERT_EQ(uinf, duinf);

  const std::string json_channel =
      R"(
      {
        "login":"atopilski@gmail.com",
        "password":"1234",
        "channels":
        [
        {
          "epg":{
          "id":"59106ed9457cd9f4c3c0b78f",
          "url":"http://example.com:6969/127.ts",
          "display_name":"Alex TV",
          "icon":"/images/unknown_channel.png",
          "programs":[]},
          "video":true,
          "audio":true
        },
        {
          "epg":
          {
            "id":"592fa5778b385c798bd499fa",
            "url":"fiel://C:/msys64/home/Sasha/work/fastotv/tests/big_buck_bunny_1080p_h264.mov",
            "display_name":"Local",
            "icon":"/images/unknown_channel.png",
           "programs":[]
          },
          "video":true,
          "audio":true
        },
        {
          "epg":
          {
            "id":"592feb388b385c798bd499fb",
            "url":"file:///home/sasha/work/fastotv/tests/big_buck_bunny_1080p_h264.mov",
            "display_name":"Local2",
            "icon":"/images/unknown_channel.png",
            "programs":[]
          },
          "video":true,
          "audio":true
        }
        ]
      }
      )";

  err = uinf.SerializeFromString(json_channel, &ser);
  ASSERT_TRUE(!err);

  err = uinf.DeSerialize(ser, &duinf);
  ASSERT_TRUE(!err);
  fasto::fastotv::ChannelsInfo ch = duinf.GetChannelInfo();
  const fasto::fastotv::AuthInfo auth("atopilski@gmail.com", "1234");
  ASSERT_EQ(duinf.GetAuthInfo(), auth);
  ASSERT_EQ(ch.GetSize(), 3);
}

TEST(UserStateInfo, serialize_deserialize) {
  const fasto::fastotv::server::user_id_t user_id = "123fe";
  const bool connected = false;

  fasto::fastotv::server::UserStateInfo ust(user_id, connected);
  ASSERT_EQ(ust.GetUserId(), user_id);
  ASSERT_EQ(ust.IsConnected(), connected);

  serialize_t ser;
  common::Error err = ust.Serialize(&ser);
  ASSERT_TRUE(!err);
  fasto::fastotv::server::UserStateInfo dust;
  err = ust.DeSerialize(ser, &dust);
  ASSERT_TRUE(!err);

  ASSERT_EQ(ust, dust);
}

TEST(ResponceInfo, serialize_deserialize) {
  const std::string request_id = "req";
  const std::string state = "state";
  const std::string command = "comma";
  const std::string responce_json = "{}";

  fasto::fastotv::server::ResponceInfo ust(request_id, state, command, responce_json);
  ASSERT_EQ(ust.GetRequestId(), request_id);
  ASSERT_EQ(ust.GetState(), state);
  ASSERT_EQ(ust.GetCommand(), command);
  ASSERT_EQ(ust.GetResponceJson(), responce_json);

  serialize_t ser;
  common::Error err = ust.Serialize(&ser);
  ASSERT_TRUE(!err);
  fasto::fastotv::server::ResponceInfo dust;
  err = ust.DeSerialize(ser, &dust);
  ASSERT_TRUE(!err);

  ASSERT_EQ(ust, dust);
}
