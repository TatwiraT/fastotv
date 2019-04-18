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

#include "commands_info/runtime_channel_info.h"

#define RUNTIME_CHANNEL_INFO_CHANNEL_ID_FIELD "channel_id"

#define RUNTIME_CHANNEL_INFO_CHANNEL_TYPE_FIELD "channel_type"
#define RUNTIME_CHANNEL_INFO_WATCHERS_FIELD "watchers"
#define RUNTIME_CHANNEL_INFO_CHAT_ENABLED_FIELD "chat_enabled"
#define RUNTIME_CHANNEL_INFO_CHAT_READONLY_FIELD "chat_readonly"
#define RUNTIME_CHANNEL_INFO_MESSAGES_FIELD "messages"

namespace fastotv {

RuntimeChannelLiteInfo::RuntimeChannelLiteInfo() : RuntimeChannelLiteInfo(invalid_stream_id) {}

RuntimeChannelLiteInfo::RuntimeChannelLiteInfo(stream_id channel_id) : channel_id_(channel_id) {}

RuntimeChannelLiteInfo::~RuntimeChannelLiteInfo() {}

bool RuntimeChannelLiteInfo::IsValid() const {
  return channel_id_ != invalid_stream_id;
}

void RuntimeChannelLiteInfo::SetChannelID(stream_id sid) {
  channel_id_ = sid;
}

stream_id RuntimeChannelLiteInfo::GetChannelID() const {
  return channel_id_;
}

bool RuntimeChannelLiteInfo::Equals(const RuntimeChannelLiteInfo& inf) const {
  return channel_id_ == inf.channel_id_;
}

common::Error RuntimeChannelLiteInfo::DoDeSerialize(json_object* serialized) {
  RuntimeChannelLiteInfo inf;
  json_object* jcid = nullptr;
  json_bool jcid_exists = json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_CHANNEL_ID_FIELD, &jcid);
  if (!jcid_exists) {
    return common::make_error_inval();
  }
  stream_id cid = json_object_get_string(jcid);
  if (cid == invalid_stream_id) {
    return common::make_error_inval();
  }
  inf.channel_id_ = cid;

  *this = inf;
  return common::Error();
}

common::Error RuntimeChannelLiteInfo::SerializeFields(json_object* deserialized) const {
  if (!IsValid()) {
    return common::make_error_inval();
  }

  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_CHANNEL_ID_FIELD,
                         json_object_new_string(channel_id_.c_str()));
  return common::Error();
}

RuntimeChannelInfo::RuntimeChannelInfo()
    : base_class(), watchers_(0), type_(UNKNOWN_CHANNEL), chat_enabled_(false), chat_read_only_(false), messages_() {}

RuntimeChannelInfo::RuntimeChannelInfo(stream_id channel_id,
                                       size_t watchers,
                                       ChannelType type,
                                       bool chat_enabled,
                                       bool read_only,
                                       const messages_t& msgs)
    : base_class(channel_id),
      watchers_(watchers),
      type_(type),
      chat_enabled_(chat_enabled),
      chat_read_only_(read_only),
      messages_(msgs) {}

RuntimeChannelInfo::~RuntimeChannelInfo() {}

void RuntimeChannelInfo::SetWatchersCount(size_t count) {
  watchers_ = count;
}

size_t RuntimeChannelInfo::GetWatchersCount() const {
  return watchers_;
}

void RuntimeChannelInfo::SetChatEnabled(bool en) {
  chat_enabled_ = en;
}

bool RuntimeChannelInfo::IsChatEnabled() const {
  return chat_enabled_;
}

void RuntimeChannelInfo::SetChatReadOnly(bool ro) {
  chat_read_only_ = ro;
}

bool RuntimeChannelInfo::IsChatReadOnly() const {
  return chat_read_only_;
}

void RuntimeChannelInfo::AddMessage(const ChatMessage& msg) {
  messages_.push_back(msg);
}

RuntimeChannelInfo::messages_t RuntimeChannelInfo::GetMessages() const {
  return messages_;
}

void RuntimeChannelInfo::SetChannelType(ChannelType ct) {
  type_ = ct;
}

ChannelType RuntimeChannelInfo::GetChannelType() const {
  return type_;
}

common::Error RuntimeChannelInfo::SerializeFields(json_object* deserialized) const {
  common::Error err = base_class::SerializeFields(deserialized);
  if (err) {
    return err;
  }

  json_object* jmsgs = json_object_new_array();
  for (size_t i = 0; i < messages_.size(); ++i) {
    serialize_type jmsg = nullptr;
    common::Error err = messages_[i].Serialize(&jmsg);
    if (err) {
      continue;
    }
    json_object_array_add(jmsgs, jmsg);
  }

  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_WATCHERS_FIELD, json_object_new_int(watchers_));
  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_CHANNEL_TYPE_FIELD, json_object_new_int(type_));
  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_CHAT_ENABLED_FIELD, json_object_new_boolean(chat_enabled_));
  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_CHAT_READONLY_FIELD,
                         json_object_new_boolean(chat_read_only_));
  json_object_object_add(deserialized, RUNTIME_CHANNEL_INFO_MESSAGES_FIELD, jmsgs);
  return common::Error();
}

common::Error RuntimeChannelInfo::DoDeSerialize(json_object* serialized) {
  RuntimeChannelInfo inf;
  common::Error err = inf.base_class::DoDeSerialize(serialized);
  if (err) {
    return err;
  }

  json_object* jwatchers = nullptr;
  json_bool jwatchers_exists = json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_WATCHERS_FIELD, &jwatchers);
  if (jwatchers_exists) {
    inf.watchers_ = json_object_get_int64(jwatchers);
  }

  json_object* jchat_type = nullptr;
  json_bool jchat_type_exists =
      json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_CHANNEL_TYPE_FIELD, &jchat_type);
  if (jchat_type_exists) {
    inf.type_ = static_cast<ChannelType>(json_object_get_int(jchat_type));
  }

  json_object* jchat_enabled = nullptr;
  json_bool jchat_enabled_exists =
      json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_CHAT_ENABLED_FIELD, &jchat_enabled);
  if (jchat_enabled_exists) {
    inf.chat_enabled_ = json_object_get_boolean(jchat_enabled);
  }

  json_object* jchat_readonly = nullptr;
  json_bool jchat_readonly_exists =
      json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_CHAT_READONLY_FIELD, &jchat_readonly);
  if (jchat_readonly_exists) {
    inf.chat_read_only_ = json_object_get_boolean(jchat_readonly);
  }

  json_object* jmsgs = nullptr;
  json_bool jmsgs_exists = json_object_object_get_ex(serialized, RUNTIME_CHANNEL_INFO_MESSAGES_FIELD, &jmsgs);
  if (jmsgs_exists) {
    messages_t msgs;
    size_t len = json_object_array_length(jmsgs);
    for (size_t i = 0; i < len; ++i) {
      json_object* jmess = json_object_array_get_idx(jmsgs, i);
      ChatMessage msg;
      common::Error err = msg.DeSerialize(jmess);
      if (err) {
        continue;
      }
      msgs.push_back(msg);
    }
    inf.messages_ = msgs;
  }

  *this = inf;
  return common::Error();
}

bool RuntimeChannelInfo::Equals(const RuntimeChannelInfo& inf) const {
  return base_class::Equals(inf) && watchers_ == inf.watchers_ && chat_enabled_ == inf.chat_enabled_ &&
         messages_ == inf.messages_;
}

}  // namespace fastotv
