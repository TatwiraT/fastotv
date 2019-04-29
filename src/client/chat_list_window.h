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

#pragma once

#include <string>
#include <vector>

#include <player/gui/widgets/list_box.h>

#include "commands_info/chat_message.h"

namespace fastotv {
namespace client {

class ChatListWindow : public fastoplayer::gui::IListBox {
 public:
  enum { login_field_width = 240, space_width = 10 };
  typedef fastoplayer::gui::IListBox base_class;
  typedef std::vector<ChatMessage> messages_t;
  explicit ChatListWindow(const SDL_Color& back_ground_color);

  size_t GetRowCount() const override;

  void SetMessages(const messages_t& msgs);
  messages_t GetMessages() const;

 protected:
  void DrawRow(SDL_Renderer* render, size_t pos, bool active, bool hover, const SDL_Rect& row_rect) override;

 private:
  messages_t msgs_;
};

}  // namespace client
}  // namespace fastotv
