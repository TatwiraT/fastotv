#pragma once

#include <stdint.h>  // for int64_t

extern "C" {
#include <libavutil/frame.h>
}

#include <common/macros.h>

namespace core {

struct AudioFrame {
  AudioFrame();
  ~AudioFrame();

  AVFrame* frame;
  int serial;
  clock_t pts;      /* presentation timestamp for the frame */
  clock_t duration; /* estimated duration of the frame */
  int64_t pos;      /* byte position of the frame in the input file */

  void ClearFrame();

 private:
  DISALLOW_COPY_AND_ASSIGN(AudioFrame);
};

}  // namespace core
