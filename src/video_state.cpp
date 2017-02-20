#include "video_state.h"

#include <limits.h>    // for INT_MAX, INT_MIN
#include <stdio.h>     // for snprintf, stdout
#include <stdlib.h>    // for EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>    // for memset, strlen, memcpy, etc
#include <errno.h>     // for ENOMEM, EINVAL, EAGAIN, etc
#include <inttypes.h>  // for PRIx64
#include <math.h>      // for fabs, isnan, NAN, exp, log

#include <ostream>  // for operator<<, basic_ostream, etc
#include <memory>

#include <SDL2/SDL_audio.h>     // for SDL_MIX_MAXVOLUME, etc
#include <SDL2/SDL_error.h>     // for SDL_GetError
#include <SDL2/SDL_events.h>    // for SDL_Event, SDL_PushEvent, etc
#include <SDL2/SDL_hints.h>     // for SDL_SetHint, etc
#include <SDL2/SDL_keyboard.h>  // for SDL_Keysym
#include <SDL2/SDL_keycode.h>   // for ::SDLK_0, ::SDLK_9, etc
#include <SDL2/SDL_mouse.h>     // for SDL_ShowCursor, etc
#include <SDL2/SDL_rect.h>      // for SDL_Rect

extern "C" {
#include <libavcodec/avcodec.h>  // for AVCodecContext, etc
#include <libavcodec/version.h>  // for FF_API_EMU_EDGE
#include <libavformat/avio.h>    // for AVIOContext, etc
#include <libavutil/avutil.h>    // for AV_NOPTS_VALUE, etc
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>       // for FFMAX, av_clip, FFMIN
#include <libavutil/dict.h>         // for av_dict_free, av_dict_get, etc
#include <libavutil/error.h>        // for AVERROR, AVERROR_EOF, etc
#include <libavutil/mathematics.h>  // for av_compare_ts, av_rescale_q
#include <libavutil/mem.h>          // for av_freep, av_fast_malloc, etc
#include <libavutil/pixfmt.h>       // for AVPixelFormat, etc
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>  // for AVSampleFormat, etc
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#if CONFIG_AVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
}

#include <common/logger.h>  // for LogMessage, etc
#include <common/macros.h>  // for destroy, ERROR_RESULT_VALUE, etc
#include <common/threads/thread_manager.h>
#include <common/utils.h>
#include <common/sprintf.h>

#include "video_state_handler.h"

#include "core/types.h"  // for get_valid_channel_layout, etc
#include "core/utils.h"  // for configure_filtergraph, etc
#include "core/stream.h"
#include "core/frame_queue.h"
#include "core/app_options.h"
#include "core/decoder.h"
#include "core/packet_queue.h"

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

#define AV_NOSYNC_THRESHOLD 10.0

#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10
/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10
/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001
/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

namespace {
int decode_interrupt_callback(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  return is->IsAborted();
}

SDL_Texture* CreateTexture(SDL_Renderer* renderer,
                           Uint32 new_format,
                           int new_width,
                           int new_height,
                           SDL_BlendMode blendmode,
                           bool init_texture) {
  SDL_Texture* ltexture =
      SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height);
  if (!ltexture) {
    NOTREACHED();
    return nullptr;
  }
  if (SDL_SetTextureBlendMode(ltexture, blendmode) < 0) {
    NOTREACHED();
    SDL_DestroyTexture(ltexture);
    return nullptr;
  }
  if (init_texture) {
    void* pixels;
    int pitch;
    if (SDL_LockTexture(ltexture, NULL, &pixels, &pitch) < 0) {
      NOTREACHED();
      SDL_DestroyTexture(ltexture);
      return nullptr;
    }
    memset(pixels, 0, pitch * new_height);
    SDL_UnlockTexture(ltexture);
  }

  return ltexture;
}
}

VideoState::VideoState(const common::uri::Uri& uri,
                       core::AppOptions* opt,
                       core::ComplexOptions* copt,
                       VideoStateHandler* handler)
    : uri_(uri),
      opt_(opt),
      copt_(copt),
      audio_callback_time_(0),
      read_tid_(THREAD_MANAGER()->createThread(&VideoState::ReadThread, this)),
      force_refresh_(false),
      queue_attachments_req_(false),
      seek_req_(false),
      seek_flags_(0),
      seek_pos_(0),
      seek_rel_(0),
      read_pause_return_(0),
      ic_(NULL),
      realtime_(false),
      vstream_(new core::VideoStream),
      astream_(new core::AudioStream),
      viddec_(nullptr),
      auddec_(nullptr),
      video_frame_queue_(nullptr),
      audio_frame_queue_(nullptr),
      audio_clock_(0),
      audio_clock_serial_(-1),
      audio_diff_cum_(0),
      audio_diff_avg_coef_(0),
      audio_diff_threshold_(0),
      audio_diff_avg_count_(0),
      audio_hw_buf_size_(0),
      audio_buf_(NULL),
      audio_buf1_(NULL),
      audio_buf_size_(0),
      audio_buf1_size_(0),
      audio_buf_index_(0),
      audio_write_buf_size_(0),
      audio_volume_(0),
      audio_src_(),
#if CONFIG_AVFILTER
      audio_filter_src_(),
#endif
      audio_tgt_(),
      swr_ctx_(NULL),
      sample_array_{0},
      sample_array_index_(0),
      last_i_start_(0),
      last_vis_time_(0),
      frame_timer_(0),
      frame_last_returned_time_(0),
      frame_last_filter_delay_(0),
      max_frame_duration_(0),
      img_convert_ctx_(NULL),
      sub_convert_ctx_(NULL),
      width_(0),
      height_(0),
      xleft_(0),
      ytop_(0),
      step_(false),
#if CONFIG_AVFILTER
      vfilter_idx_(0),
      in_video_filter_(NULL),
      out_video_filter_(NULL),
      in_audio_filter_(NULL),
      out_audio_filter_(NULL),
      agraph_(NULL),
#endif
      last_video_stream_(invalid_stream_index),
      last_audio_stream_(invalid_stream_index),
      vdecoder_tid_(THREAD_MANAGER()->createThread(&VideoState::VideoThread, this)),
      adecoder_tid_(THREAD_MANAGER()->createThread(&VideoState::AudioThread, this)),
      paused_(false),
      last_paused_(false),
      muted_(false),
      eof_(false),
      abort_request_(false),
      renderer_(NULL),
      window_(NULL),
      stats_(),
      handler_(handler) {
  CHECK(handler);
  audio_volume_ = opt->startup_volume;
}

VideoState::~VideoState() {
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  Abort();
  int res = read_tid_->joinAndGet();
  DCHECK_EQ(res, 0);

  /* close each stream */
  if (vstream_->IsOpened()) {
    StreamComponentClose(vstream_->Index());
    vstream_->Close();
  }
  if (astream_->IsOpened()) {
    StreamComponentClose(astream_->Index());
    astream_->Close();
  }

  destroy(&astream_);
  destroy(&vstream_);

  avformat_close_input(&ic_);

  sws_freeContext(img_convert_ctx_);
  sws_freeContext(sub_convert_ctx_);
}

int VideoState::StreamComponentOpen(int stream_index) {
  if (stream_index == invalid_stream_index ||
      static_cast<unsigned int>(stream_index) >= ic_->nb_streams) {
    return AVERROR(EINVAL);
  }

  AVCodecContext* avctx = avcodec_alloc_context3(NULL);
  if (!avctx) {
    return AVERROR(ENOMEM);
  }

  AVStream* stream = ic_->streams[stream_index];
  int ret = avcodec_parameters_to_context(avctx, stream->codecpar);
  if (ret < 0) {
    avcodec_free_context(&avctx);
    return ret;
  }

  const char* forced_codec_name = NULL;
  av_codec_set_pkt_timebase(avctx, stream->time_base);
  AVCodec* codec = avcodec_find_decoder(avctx->codec_id);

  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
      last_video_stream_ = stream_index;
      forced_codec_name = common::utils::c_strornull(opt_->video_codec_name);
      break;
    }
    case AVMEDIA_TYPE_AUDIO: {
      last_audio_stream_ = stream_index;
      forced_codec_name = common::utils::c_strornull(opt_->audio_codec_name);
      break;
    }
    case AVMEDIA_TYPE_SUBTITLE: {
      break;
    }
    case AVMEDIA_TYPE_UNKNOWN: {
      break;
    }
    case AVMEDIA_TYPE_ATTACHMENT: {
      break;
    }
    case AVMEDIA_TYPE_DATA: {
      break;
    }
    case AVMEDIA_TYPE_NB: {
      break;
    }
    default:
      break;
  }
  if (forced_codec_name) {
    codec = avcodec_find_decoder_by_name(forced_codec_name);
  }
  if (!codec) {
    if (forced_codec_name) {
      WARNING_LOG() << "No codec could be found with name '" << forced_codec_name << "'";
    } else {
      WARNING_LOG() << "No codec could be found with id " << avctx->codec_id;
    }
    ret = AVERROR(EINVAL);
    avcodec_free_context(&avctx);
    return ret;
  }

  int stream_lowres = opt_->lowres;
  avctx->codec_id = codec->id;
  if (stream_lowres > av_codec_get_max_lowres(codec)) {
    WARNING_LOG() << "The maximum value for lowres supported by the decoder is "
                  << av_codec_get_max_lowres(codec);
    stream_lowres = av_codec_get_max_lowres(codec);
  }
  av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
  if (stream_lowres) {
    avctx->flags |= CODEC_FLAG_EMU_EDGE;
  }
#endif
  if (opt_->fast) {
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;
  }
#if FF_API_EMU_EDGE
  if (codec->capabilities & AV_CODEC_CAP_DR1) {
    avctx->flags |= CODEC_FLAG_EMU_EDGE;
  }
#endif

  AVDictionary* opts =
      core::filter_codec_opts(copt_->codec_opts, avctx->codec_id, ic_, stream, codec);
  if (!av_dict_get(opts, "threads", NULL, 0)) {
    av_dict_set(&opts, "threads", "auto", 0);
  }
  if (stream_lowres) {
    av_dict_set_int(&opts, "lowres", stream_lowres, 0);
  }
  if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    av_dict_set(&opts, "refcounted_frames", "1", 0);
  }
  ret = avcodec_open2(avctx, codec, &opts);
  if (ret < 0) {
    avcodec_free_context(&avctx);
    av_dict_free(&opts);
    return ret;
  }

  AVDictionaryEntry* t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX);
  if (t) {
    ERROR_LOG() << "Option " << t->key << " not found.";
    avcodec_free_context(&avctx);
    av_dict_free(&opts);
    return AVERROR_OPTION_NOT_FOUND;
  }

  int sample_rate, nb_channels;
  int64_t channel_layout = 0;
  eof_ = false;
  stream->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
      bool opened = vstream_->Open(stream_index, stream);
      UNUSED(opened);
      core::PacketQueue* packet_queue = vstream_->Queue();
      video_frame_queue_ = new core::VideoFrameQueue<VIDEO_PICTURE_QUEUE_SIZE>(true);
      viddec_ = new core::VideoDecoder(avctx, packet_queue, opt_->decoder_reorder_pts);
      viddec_->Start();
      if (!vdecoder_tid_->start()) {
        destroy(&viddec_);
        goto out;
      }
      queue_attachments_req_ = true;
      break;
    }
    case AVMEDIA_TYPE_AUDIO: {
#if CONFIG_AVFILTER
      {
        audio_filter_src_.freq = avctx->sample_rate;
        audio_filter_src_.channels = avctx->channels;
        audio_filter_src_.channel_layout =
            core::get_valid_channel_layout(avctx->channel_layout, avctx->channels);
        audio_filter_src_.fmt = avctx->sample_fmt;
        ret = ConfigureAudioFilters(opt_->afilters, 0);
        if (ret < 0) {
          avcodec_free_context(&avctx);
          av_dict_free(&opts);
          return ret;
        }
        AVFilterLink* link = out_audio_filter_->inputs[0];
        sample_rate = link->sample_rate;
        nb_channels = avfilter_link_get_channels(link);
        channel_layout = link->channel_layout;
      }
#else
      sample_rate = avctx->sample_rate;
      nb_channels = avctx->channels;
      channel_layout = avctx->channel_layout;
#endif

      /* prepare audio output */
      ret = audio_open(this, channel_layout, nb_channels, sample_rate, &audio_tgt_,
                       sdl_audio_callback);
      if (ret < 0) {
        avcodec_free_context(&avctx);
        av_dict_free(&opts);
        return ret;
      }

      audio_hw_buf_size_ = ret;
      audio_src_ = audio_tgt_;
      audio_buf_size_ = 0;
      audio_buf_index_ = 0;

      /* init averaging filter */
      audio_diff_avg_coef_ = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
      audio_diff_avg_count_ = 0;
      /* since we do not have a precise anough audio FIFO fullness,
         we correct audio sync only if larger than this threshold */
      audio_diff_threshold_ =
          static_cast<double>(audio_hw_buf_size_) / static_cast<double>(audio_tgt_.bytes_per_sec);
      bool opened = astream_->Open(stream_index, stream);
      UNUSED(opened);
      core::PacketQueue* packet_queue = astream_->Queue();
      audio_frame_queue_ = new core::AudioFrameQueue<SAMPLE_QUEUE_SIZE>(true);
      auddec_ = new core::AudioDecoder(avctx, packet_queue);
      if ((ic_->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
          !ic_->iformat->read_seek) {
        auddec_->SetStartPts(stream->start_time, stream->time_base);
      }
      auddec_->Start();
      if (!adecoder_tid_->start()) {
        destroy(&auddec_);
        goto out;
      }
      SDL_PauseAudio(0);
      break;
    }
    case AVMEDIA_TYPE_SUBTITLE: {
      break;
    }
    case AVMEDIA_TYPE_UNKNOWN:
      break;
    case AVMEDIA_TYPE_ATTACHMENT:
      break;
    case AVMEDIA_TYPE_DATA:
      break;
    case AVMEDIA_TYPE_NB:
      break;
    default:
      break;
  }
out:
  av_dict_free(&opts);
  return ret;
}

void VideoState::StreamComponentClose(int stream_index) {
  if (stream_index < 0 || static_cast<unsigned int>(stream_index) >= ic_->nb_streams) {
    return;
  }

  AVStream* avs = ic_->streams[stream_index];
  AVCodecParameters* codecpar = avs->codecpar;
  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
      video_frame_queue_->Stop();
      viddec_->Abort();
      vdecoder_tid_->join();
      vdecoder_tid_ = NULL;
      destroy(&viddec_);
      destroy(&video_frame_queue_);
      break;
    }
    case AVMEDIA_TYPE_AUDIO: {
      audio_frame_queue_->Stop();
      auddec_->Abort();
      adecoder_tid_->join();
      adecoder_tid_ = NULL;
      destroy(&auddec_);
      destroy(&audio_frame_queue_);
      SDL_CloseAudio();
      swr_free(&swr_ctx_);
      av_freep(&audio_buf1_);
      audio_buf1_size_ = 0;
      audio_buf_ = NULL;
      break;
    }
    case AVMEDIA_TYPE_SUBTITLE: {
      break;
    }
    case AVMEDIA_TYPE_UNKNOWN:
      break;
    case AVMEDIA_TYPE_ATTACHMENT:
      break;
    case AVMEDIA_TYPE_DATA:
      break;
    case AVMEDIA_TYPE_NB:
      break;
    default:
      break;
  }
  avs->discard = AVDISCARD_ALL;
}

void VideoState::StreamSeek(int64_t pos, int64_t rel, int seek_by_bytes) {
  if (!seek_req_) {
    seek_pos_ = pos;
    seek_rel_ = rel;
    seek_flags_ &= ~AVSEEK_FLAG_BYTE;
    if (seek_by_bytes) {
      seek_flags_ |= AVSEEK_FLAG_BYTE;
    }
    seek_req_ = true;
  }
}

void VideoState::StepToNextFrame() {
  /* if the stream is paused unpause it, then step */
  if (paused_) {
    StreamTogglePause();
  }
  step_ = true;
}

int VideoState::GetMasterSyncType() const {
  return opt_->av_sync_type;
}

double VideoState::ComputeTargetDelay(double delay) const {
  double diff = 0;

  /* update delay to follow master synchronisation source */
  if (GetMasterSyncType() != core::AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
       duplicating or deleting a frame */
    diff = vstream_->GetClock() - GetMasterClock();

    /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know
       if it is the best guess */
    double sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!std::isnan(diff) && fabs(diff) < max_frame_duration_) {
      if (diff <= -sync_threshold) {
        delay = FFMAX(0, delay + diff);
      } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
        delay = delay + diff;
      } else if (diff >= sync_threshold) {
        delay = 2 * delay;
      }
    }
  }
  DEBUG_LOG() << "video: delay=" << delay << " A-V=" << -diff;
  return delay;
}

/* get the current master clock value */
double VideoState::GetMasterClock() const {
  if (GetMasterSyncType() == core::AV_SYNC_VIDEO_MASTER) {
    return vstream_->GetClock();
  }

  return astream_->GetClock();
}

void VideoState::VideoRefresh(double* remaining_time) {
  AVStream* video_st = vstream_->IsOpened() ? vstream_->AvStream() : NULL;
  AVStream* audio_st = astream_->IsOpened() ? astream_->AvStream() : NULL;
  core::PacketQueue* video_packet_queue = vstream_->Queue();
  core::PacketQueue* audio_packet_queue = astream_->Queue();

  if (!opt_->display_disable && opt_->show_mode != core::SHOW_MODE_VIDEO && audio_st) {
    double time = av_gettime_relative() / 1000000.0;
    if (force_refresh_ || last_vis_time_ < time) {
      VideoDisplay();
      last_vis_time_ = time;
    }
    *remaining_time = FFMIN(*remaining_time, last_vis_time_ - time);
  }

  if (video_st) {
  retry:
    if (video_frame_queue_->IsEmpty()) {
      // nothing to do, no picture to display in the queue
    } else {
      /* dequeue the picture */
      core::VideoFrame* lastvp = video_frame_queue_->PeekLast();
      core::VideoFrame* vp = video_frame_queue_->Peek();

      if (vp->serial != video_packet_queue->Serial()) {
        video_frame_queue_->MoveToNext();
        goto retry;
      }

      if (lastvp->serial != vp->serial) {
        frame_timer_ = av_gettime_relative() / 1000000.0;
      }

      if (paused_) {
        goto display;
      }

      /* compute nominal last_duration */
      double last_duration = core::VideoFrame::VpDuration(lastvp, vp, max_frame_duration_);
      double delay = ComputeTargetDelay(last_duration);
      double time = av_gettime_relative() / 1000000.0;
      if (time < frame_timer_ + delay) {
        *remaining_time = FFMIN(frame_timer_ + delay - time, *remaining_time);
        goto display;
      }

      frame_timer_ += delay;
      if (delay > 0 && time - frame_timer_ > AV_SYNC_THRESHOLD_MAX) {
        frame_timer_ = time;
      }

      {
        const double pts = vp->pts;
        const int serial = vp->serial;
        if (!std::isnan(pts)) {
          /* update current video pts */
          vstream_->SetClock(pts, serial);
        }
      }

      core::VideoFrame* nextvp = video_frame_queue_->PeekNextOrNull();
      if (nextvp) {
        double duration = core::VideoFrame::VpDuration(vp, nextvp, max_frame_duration_);
        if (!step_ && (opt_->framedrop > 0 ||
                       (opt_->framedrop && GetMasterSyncType() != core::AV_SYNC_VIDEO_MASTER)) &&
            time > frame_timer_ + duration) {
          stats_.frame_drops_late++;
          video_frame_queue_->MoveToNext();
          goto retry;
        }
      }

      video_frame_queue_->MoveToNext();
      force_refresh_ = true;

      if (step_ && !paused_) {
        StreamTogglePause();
      }
    }
  display:
    /* display picture */
    if (!opt_->display_disable && force_refresh_ && opt_->show_mode == core::SHOW_MODE_VIDEO &&
        video_frame_queue_->RindexShown()) {
      VideoDisplay();
    }
  }
  force_refresh_ = false;
  if (opt_->show_status) {
    static int64_t last_time;
    int64_t cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
      int aqsize = 0, vqsize = 0;
      if (video_st) {
        vqsize = video_packet_queue->Size();
      }
      if (audio_st) {
        aqsize = audio_packet_queue->Size();
      }
      double av_diff = 0;
      if (audio_st && video_st) {
        av_diff = astream_->GetClock() - vstream_->GetClock();
      } else if (video_st) {
        av_diff = GetMasterClock() - vstream_->GetClock();
      } else if (audio_st) {
        av_diff = GetMasterClock() - astream_->GetClock();
      }
      int64_t fdts = video_st ? viddec_->PtsCorrectionNumFaultyDts() : 0;
      int64_t fpts = video_st ? viddec_->PtsCorrectionNumFaultyPts() : 0;
      const char* fmt =
          (audio_st && video_st) ? "A-V" : (video_st ? "M-V" : (audio_st ? "M-A" : "   "));
      common::logging::LogMessage(common::logging::L_INFO, false).stream()
          << GetMasterClock() << " " << fmt << ":" << av_diff << " fd=" << stats_.FrameDrops()
          << " aq=" << aqsize / 1024 << "KB vq=" << vqsize / 1024 << "KB f=" << fdts << "/" << fpts
          << "\r";
      fflush(stdout);
      last_time = cur_time;
    }
  }
}

int VideoState::VideoOpen(core::VideoFrame* vp) {
  if (vp && vp->width) {
    SetDefaultWindowSize(vp->width, vp->height, vp->sar);
  }

  int w, h;
  if (opt_->screen_width) {
    w = opt_->screen_width;
    h = opt_->screen_height;
  } else {
    w = opt_->default_width;
    h = opt_->default_height;
  }

  bool res = handler_->RequestWindow(this, w, h, &renderer_, &window_);
  if (!res) {
    return ERROR_RESULT_VALUE;
  }

  width_ = w;
  height_ = h;
  return 0;
}

int VideoState::AllocPicture() {
  core::VideoFrame* vp = video_frame_queue_->Windex();

  int res = VideoOpen(vp);
  if (res == ERROR_RESULT_VALUE) {
    return ERROR_RESULT_VALUE;
  }

  Uint32 sdl_format;
  if (vp->format == AV_PIX_FMT_YUV420P) {
    sdl_format = SDL_PIXELFORMAT_YV12;
  } else {
    sdl_format = SDL_PIXELFORMAT_ARGB8888;
  }

  if (ReallocTexture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, false) < 0) {
    /* SDL allocates a buffer smaller than requested if the video
     * overlay hardware is unable to support the requested size. */

    ERROR_LOG() << "Error: the video system does not support an image\n"
                   "size of "
                << vp->width << "x" << vp->height
                << " pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                   "to reduce the image size.";
    return ERROR_RESULT_VALUE;
  }

  video_frame_queue_->ChangeSafeAndNotify([](core::VideoFrame* fr) { fr->allocated = true; }, vp);
  return SUCCESS_RESULT_VALUE;
}

/* display the current picture, if any */
void VideoState::VideoDisplay() {
  if (!renderer_) {
    VideoOpen(NULL);
  }

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  if (astream_->IsOpened() && opt_->show_mode != core::SHOW_MODE_VIDEO) {
    VideoAudioDisplay();
  } else if (vstream_->IsOpened()) {
    VideoImageDisplay();
  }
  SDL_RenderPresent(renderer_);
}

int VideoState::ReallocTexture(SDL_Texture** texture,
                               Uint32 new_format,
                               int new_width,
                               int new_height,
                               SDL_BlendMode blendmode,
                               bool init_texture) {
  Uint32 format;
  int access, w, h;
  if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w ||
      new_height != h || new_format != format) {
    SDL_DestroyTexture(*texture);
    *texture = CreateTexture(renderer_, new_format, new_width, new_height, blendmode, init_texture);
    if (!*texture) {
      return ERROR_RESULT_VALUE;
    }
  }
  return SUCCESS_RESULT_VALUE;
}

void VideoState::SetDefaultWindowSize(int width, int height, AVRational sar) {
  SDL_Rect rect;
  core::calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
  opt_->default_width = rect.w;
  opt_->default_height = rect.h;
}

void VideoState::VideoImageDisplay() {
  core::VideoFrame* vp = video_frame_queue_->PeekLast();
  if (vp->bmp) {
    SDL_Rect rect;
    core::calculate_display_rect(&rect, xleft_, ytop_, width_, height_, vp->width, vp->height,
                                 vp->sar);

    if (!vp->uploaded) {
      if (core::upload_texture(vp->bmp, vp->frame, &img_convert_ctx_) < 0) {
        return;
      }
      vp->uploaded = true;
      vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderCopyEx(renderer_, vp->bmp, NULL, &rect, 0, NULL,
                     vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
  }
}

void VideoState::VideoAudioDisplay() {
  int i_start, x;
  int h;
  /* compute display index : center on currently output samples */
  const int channels = audio_tgt_.channels;
  if (!paused_) {
    int data_used = width_;
    int n = 2 * channels;
    int delay = audio_write_buf_size_;
    delay /= n;

    /* to be more precise, we take into account the time spent since
       the last buffer computation */
    if (audio_callback_time_) {
      int64_t time_diff = av_gettime_relative() - audio_callback_time_;
      delay -= (time_diff * audio_tgt_.freq) / 1000000;
    }

    delay += 2 * data_used;
    if (delay < data_used) {
      delay = data_used;
    }

    i_start = x = core::compute_mod(sample_array_index_ - delay * channels, SAMPLE_ARRAY_SIZE);
    if (opt_->show_mode == core::SHOW_MODE_WAVES) {
      h = INT_MIN;
      for (int i = 0; i < 1000; i += channels) {
        int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
        int a = sample_array_[idx];
        int b = sample_array_[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
        int c = sample_array_[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
        int d = sample_array_[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
        int score = a - d;
        if (h < score && (b ^ c) < 0) {
          h = score;
          i_start = idx;
        }
      }
    }

    last_i_start_ = i_start;
  } else {
    i_start = last_i_start_;
  }

  int nb_display_channels = channels;
  if (opt_->show_mode == core::SHOW_MODE_WAVES) {
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);

    /* total height for one channel */
    h = height_ / nb_display_channels;
    /* graph height / 2 */
    int h2 = (h * 9) / 20;
    for (int ch = 0; ch < nb_display_channels; ch++) {
      int i = i_start + ch;
      int ys;
      int y1 = ytop_ + ch * h + (h / 2); /* position of center line */
      for (x = 0; x < width_; x++) {
        int y = (sample_array_[i] * h2) >> 15;
        if (y < 0) {
          y = -y;
          ys = y1 - y;
        } else {
          ys = y1;
        }
        core::fill_rectangle(renderer_, xleft_ + x, ys, 1, y);
        i += channels;
        if (i >= SAMPLE_ARRAY_SIZE) {
          i -= SAMPLE_ARRAY_SIZE;
        }
      }
    }

    SDL_SetRenderDrawColor(renderer_, 0, 0, 255, 255);

    for (int ch = 1; ch < nb_display_channels; ch++) {
      int y = ytop_ + ch * h;
      core::fill_rectangle(renderer_, xleft_, y, width_, 1);
    }
  }
}

int VideoState::Exec() {
  bool started = read_tid_->start();
  if (!started) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

void VideoState::Abort() {
  abort_request_ = true;
}

bool VideoState::IsAborted() const {
  return abort_request_;
}

const common::uri::Uri& VideoState::uri() const {
  return uri_;
}

void VideoState::StreamTogglePause() {
  if (paused_) {
    frame_timer_ += av_gettime_relative() / 1000000.0 - vstream_->LastUpdatedClock();
    if (read_pause_return_ != AVERROR(ENOSYS)) {
      vstream_->SetPaused(false);
    }
    vstream_->SyncSerialClock();
  }
  paused_ = !paused_;
  vstream_->SetPaused(paused_);
  astream_->SetPaused(paused_);
}

void VideoState::SetFullScreen(bool full_screen) {
  SDL_SetWindowFullscreen(window_, full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  force_refresh_ = true;
}

void VideoState::TogglePause() {
  StreamTogglePause();
  step_ = false;
}

void VideoState::ToggleMute() {
  muted_ = !muted_;
}

void VideoState::UpdateVolume(int sign, int step) {
  audio_volume_ = av_clip(audio_volume_ + sign * step, 0, SDL_MIX_MAXVOLUME);
}

void VideoState::ToggleWaveDisplay() {
#if CONFIG_AVFILTER
  const size_t nb_vfilters = opt_->vfilters_list.size();
  if (opt_->show_mode == core::SHOW_MODE_VIDEO && vfilter_idx_ < nb_vfilters - 1) {
    if (++vfilter_idx_ >= nb_vfilters) {
      vfilter_idx_ = 0;
    }
  } else {
    vfilter_idx_ = 0;
    ToggleAudioDisplay();
  }
#else
  ToggleAudioDisplay(cur_stream);
#endif
}

void VideoState::TryRefreshVideo(double* remaining_time) {
  if (opt_->show_mode != core::SHOW_MODE_NONE && (!paused_ || force_refresh_)) {
    VideoRefresh(remaining_time);
  }
}

void VideoState::ToggleAudioDisplay() {
  int next = opt_->show_mode;
  do {
    next = (next + 1) % core::SHOW_MODE_NB;
  } while (next != opt_->show_mode && ((next == core::SHOW_MODE_VIDEO && !vstream_->IsOpened()) ||
                                       (next != core::SHOW_MODE_VIDEO && !astream_->IsOpened())));
  if (opt_->show_mode != next) {
    force_refresh_ = true;
    opt_->show_mode = static_cast<core::ShowMode>(next);
  }
}

void VideoState::SeekChapter(int incr) {
  if (!ic_->nb_chapters) {
    return;
  }

  unsigned int i;
  int64_t pos = GetMasterClock() * AV_TIME_BASE;
  /* find the current chapter */
  for (i = 0; i < ic_->nb_chapters; i++) {
    AVChapter* ch = ic_->chapters[i];
    if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
      i--;
      break;
    }
  }

  unsigned int ii = FFMAX(i + incr, 0);
  if (ii >= ic_->nb_chapters) {
    return;
  }

  DEBUG_LOG() << "Seeking to chapter " << ii;
  int64_t rq = av_rescale_q(ic_->chapters[ii]->start, ic_->chapters[ii]->time_base, AV_TIME_BASE_Q);
  StreamSeek(rq, 0, 0);
}

void VideoState::StreamCycleChannel(AVMediaType codec_type) {
  int start_index = invalid_stream_index;
  int old_index = invalid_stream_index;
  if (codec_type == AVMEDIA_TYPE_VIDEO) {
    start_index = last_video_stream_;
    old_index = vstream_->Index();
  } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
    start_index = last_audio_stream_;
    old_index = astream_->Index();
  } else {
    DNOTREACHED();
  }
  int stream_index = start_index;

  AVProgram* p = NULL;
  int lnb_streams = ic_->nb_streams;
  if (codec_type != AVMEDIA_TYPE_VIDEO && vstream_->IsOpened()) {
    p = av_find_program_from_stream(ic_, NULL, old_index);
    if (p) {
      lnb_streams = p->nb_stream_indexes;
      for (start_index = 0; start_index < lnb_streams; start_index++) {
        if (p->stream_index[start_index] == stream_index) {
          break;
        }
      }
      if (start_index == lnb_streams) {
        start_index = invalid_stream_index;
      }
      stream_index = start_index;
    }
  }

  while (true) {
    if (++stream_index >= lnb_streams) {
      if (start_index == invalid_stream_index) {
        return;
      }
      stream_index = 0;
    }
    if (stream_index == start_index) {
      return;
    }
    AVStream* st = ic_->streams[p ? p->stream_index[stream_index] : stream_index];
    if (st->codecpar->codec_type == codec_type) {
      /* check that parameters are OK */
      switch (codec_type) {
        case AVMEDIA_TYPE_AUDIO: {
          if (st->codecpar->sample_rate != 0 && st->codecpar->channels != 0) {
            goto the_end;
          }
          break;
        }
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_SUBTITLE:
          goto the_end;
        default:
          break;
      }
    }
  }
the_end:
  if (p && stream_index != invalid_stream_index) {
    stream_index = p->stream_index[stream_index];
  }
  INFO_LOG() << "Switch " << av_get_media_type_string(static_cast<AVMediaType>(codec_type))
             << "  stream from #" << old_index << " to #" << stream_index;
  StreamComponentClose(old_index);
  StreamComponentOpen(stream_index);
}

void VideoState::StreamSeekPos(double x, int seek_by_bytes) {
  if (seek_by_bytes || ic_->duration <= 0) {
    const int64_t size = avio_size(ic_->pb);
    const int64_t pos = size * x / width_;
    StreamSeek(pos, 0, 1);
  } else {
    int ns, hh, mm, ss;
    int tns, thh, tmm, tss;
    tns = ic_->duration / 1000000LL;
    thh = tns / 3600;
    tmm = (tns % 3600) / 60;
    tss = (tns % 60);
    float frac = x / width_;
    ns = frac * tns;
    hh = ns / 3600;
    mm = (ns % 3600) / 60;
    ss = (ns % 60);
    INFO_LOG() << "Seek to " << frac * 100
               << common::MemSPrintf(" (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)", hh, mm,
                                     ss, thh, tmm, tss);
    int64_t ts = frac * ic_->duration;
    if (ic_->start_time != AV_NOPTS_VALUE) {
      ts += ic_->start_time;
    }
    StreamSeek(ts, 0, 0);
  }
}

void VideoState::StreemSeek(double incr, int seek_by_bytes) {
  if (seek_by_bytes) {
    int pos = -1;
    if (pos < 0 && vstream_->IsOpened()) {
      int64_t lpos = 0;
      core::PacketQueue* vqueue = vstream_->Queue();
      if (video_frame_queue_->GetLastUsedPos(&lpos, vqueue->Serial())) {
        pos = lpos;
      } else {
        pos = -1;
      }
    }
    if (pos < 0 && astream_->IsOpened()) {
      int64_t lpos = 0;
      core::PacketQueue* aqueue = astream_->Queue();
      if (audio_frame_queue_->GetLastUsedPos(&lpos, aqueue->Serial())) {
        pos = lpos;
      } else {
        pos = -1;
      }
    }
    if (pos < 0) {
      pos = avio_tell(ic_->pb);
    }
    if (ic_->bit_rate) {
      incr *= ic_->bit_rate / 8.0;
    } else {
      incr *= 180000.0;
    }
    pos += incr;
    StreamSeek(pos, incr, 1);
  } else {
    int pos = GetMasterClock();
    if (std::isnan(pos)) {
      pos = static_cast<double>(seek_pos_) / AV_TIME_BASE;
    }
    pos += incr;
    if (ic_->start_time != AV_NOPTS_VALUE &&
        pos < ic_->start_time / static_cast<double>(AV_TIME_BASE)) {
      pos = ic_->start_time / static_cast<double>(AV_TIME_BASE);
    }
    StreamSeek(static_cast<int64_t>(pos * AV_TIME_BASE), static_cast<int64_t>(incr * AV_TIME_BASE),
               0);
  }
}

void VideoState::MoveToNextFragment(double incr, int seek_by_bytes) {
  if (ic_->nb_chapters <= 1) {
    incr = 600.0;
    StreemSeek(incr, seek_by_bytes);
  }
  SeekChapter(1);
}

void VideoState::MoveToPreviousFragment(double incr, int seek_by_bytes) {
  if (ic_->nb_chapters <= 1) {
    incr = -600.0;
    StreemSeek(incr, seek_by_bytes);
  }
  SeekChapter(-1);
}

void VideoState::HandleWindowEvent(SDL_WindowEvent* event) {
  if (!event) {
    return;
  }

  switch (event->event) {
    case SDL_WINDOWEVENT_RESIZED: {
      opt_->screen_width = width_ = event->data1;
      opt_->screen_height = height_ = event->data2;
      force_refresh_ = true;
      break;
    }
    case SDL_WINDOWEVENT_EXPOSED: {
      force_refresh_ = true;
      break;
    }
  }
}

int VideoState::HandleAllocPictureEvent() {
  return AllocPicture();
}

void VideoState::UpdateSampleDisplay(short* samples, int samples_size) {
  int size = samples_size / sizeof(short);
  while (size > 0) {
    int len = SAMPLE_ARRAY_SIZE - sample_array_index_;
    if (len > size) {
      len = size;
    }
    memcpy(sample_array_ + sample_array_index_, samples, len * sizeof(short));
    samples += len;
    sample_array_index_ += len;
    if (sample_array_index_ >= SAMPLE_ARRAY_SIZE) {
      sample_array_index_ = 0;
    }
    size -= len;
  }
}

int VideoState::SynchronizeAudio(int nb_samples) {
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (GetMasterSyncType() != core::AV_SYNC_AUDIO_MASTER) {
    double diff = astream_->GetClock() - GetMasterClock();
    if (!std::isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      audio_diff_cum_ = diff + audio_diff_avg_coef_ * audio_diff_cum_;
      if (audio_diff_avg_count_ < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        audio_diff_avg_count_++;
      } else {
        /* estimate the A-V difference */
        double avg_diff = audio_diff_cum_ * (1.0 - audio_diff_avg_coef_);
        if (fabs(avg_diff) >= audio_diff_threshold_) {
          wanted_nb_samples = nb_samples + static_cast<int>(diff * audio_src_.freq);
          int min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          int max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        DEBUG_LOG() << "diff=" << diff << " adiff=" << avg_diff
                    << " sample_diff=" << wanted_nb_samples - nb_samples << " apts=" << audio_clock_
                    << " " << audio_diff_threshold_;
      }
    } else {
      /* too big difference : may be initial PTS errors, so
         reset A-V filter */
      audio_diff_avg_count_ = 0;
      audio_diff_cum_ = 0;
    }
  }

  return wanted_nb_samples;
}

int VideoState::AudioDecodeFrame() {
  int data_size, resampled_data_size;
  int64_t dec_channel_layout;
  int wanted_nb_samples;

  if (paused_) {
    return -1;
  }

  core::PacketQueue* audio_packet_queue = astream_->IsOpened() ? astream_->Queue() : NULL;
  if (!audio_packet_queue) {
    return -1;
  }

  core::AudioFrame* af = nullptr;
  do {
    af = audio_frame_queue_->GetPeekReadable();
    if (!af) {
      return -1;
    }
    audio_frame_queue_->MoveToNext();
  } while (af->serial != audio_packet_queue->Serial());

  const AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(af->frame->format);
  data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame),
                                         af->frame->nb_samples, sample_fmt, 1);

  dec_channel_layout = (af->frame->channel_layout &&
                        av_frame_get_channels(af->frame) ==
                            av_get_channel_layout_nb_channels(af->frame->channel_layout))
                           ? af->frame->channel_layout
                           : av_get_default_channel_layout(av_frame_get_channels(af->frame));
  wanted_nb_samples = SynchronizeAudio(af->frame->nb_samples);

  if (af->frame->format != audio_src_.fmt || dec_channel_layout != audio_src_.channel_layout ||
      af->frame->sample_rate != audio_src_.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !swr_ctx_)) {
    swr_free(&swr_ctx_);
    swr_ctx_ = swr_alloc_set_opts(NULL, audio_tgt_.channel_layout, audio_tgt_.fmt, audio_tgt_.freq,
                                  dec_channel_layout, sample_fmt, af->frame->sample_rate, 0, NULL);
    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
      ERROR_LOG() << "Cannot create sample rate converter for conversion of "
                  << af->frame->sample_rate << " Hz " << av_get_sample_fmt_name(sample_fmt) << " "
                  << av_frame_get_channels(af->frame) << " channels to " << audio_tgt_.freq
                  << " Hz " << av_get_sample_fmt_name(audio_tgt_.fmt) << " " << audio_tgt_.channels
                  << " channels!";
      swr_free(&swr_ctx_);
      return -1;
    }
    audio_src_.channel_layout = dec_channel_layout;
    audio_src_.channels = av_frame_get_channels(af->frame);
    audio_src_.freq = af->frame->sample_rate;
    audio_src_.fmt = sample_fmt;
  }

  if (swr_ctx_) {
    const uint8_t** in = const_cast<const uint8_t**>(af->frame->extended_data);
    uint8_t** out = &audio_buf1_;
    int out_count =
        static_cast<int64_t>(wanted_nb_samples) * audio_tgt_.freq / af->frame->sample_rate + 256;
    int out_size =
        av_samples_get_buffer_size(NULL, audio_tgt_.channels, out_count, audio_tgt_.fmt, 0);
    if (out_size < 0) {
      ERROR_LOG() << "av_samples_get_buffer_size() failed";
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation(swr_ctx_, (wanted_nb_samples - af->frame->nb_samples) *
                                             audio_tgt_.freq / af->frame->sample_rate,
                               wanted_nb_samples * audio_tgt_.freq / af->frame->sample_rate) < 0) {
        ERROR_LOG() << "swr_set_compensation() failed";
        return -1;
      }
    }
    av_fast_malloc(&audio_buf1_, &audio_buf1_size_, out_size);
    if (!audio_buf1_) {
      return AVERROR(ENOMEM);
    }
    int len2 = swr_convert(swr_ctx_, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      ERROR_LOG() << "swr_convert() failed";
      return -1;
    }
    if (len2 == out_count) {
      WARNING_LOG() << "audio buffer is probably too small";
      if (swr_init(swr_ctx_) < 0) {
        swr_free(&swr_ctx_);
      }
    }
    audio_buf_ = audio_buf1_;
    resampled_data_size = len2 * audio_tgt_.channels * av_get_bytes_per_sample(audio_tgt_.fmt);
  } else {
    audio_buf_ = af->frame->data[0];
    resampled_data_size = data_size;
  }

#ifdef DEBUG
  double audio_clock0 = audio_clock;
#endif
  /* update the audio clock with the pts */
  if (!std::isnan(af->pts)) {
    audio_clock_ = af->pts + static_cast<double>(af->frame->nb_samples) / af->frame->sample_rate;
  } else {
    audio_clock_ = NAN;
  }
  audio_clock_serial_ = af->serial;
#ifdef DEBUG
  {
    static double last_clock;
    printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n", audio_clock - last_clock, audio_clock,
           audio_clock0);
    last_clock = audio_clock;
  }
#endif
  return resampled_data_size;
}

void VideoState::sdl_audio_callback(void* opaque, Uint8* stream, int len) {
  VideoState* is = static_cast<VideoState*>(opaque);

  is->audio_callback_time_ = av_gettime_relative();

  while (len > 0) {
    if (is->audio_buf_index_ >= is->audio_buf_size_) {
      int audio_size = is->AudioDecodeFrame();
      if (audio_size < 0) {
        /* if error, just output silence */
        is->audio_buf_ = NULL;
        is->audio_buf_size_ =
            SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt_.frame_size * is->audio_tgt_.frame_size;
      } else {
        if (is->opt_->show_mode != core::SHOW_MODE_VIDEO) {
          is->UpdateSampleDisplay(reinterpret_cast<int16_t*>(is->audio_buf_), audio_size);
        }
        is->audio_buf_size_ = audio_size;
      }
      is->audio_buf_index_ = 0;
    }
    int len1 = is->audio_buf_size_ - is->audio_buf_index_;
    if (len1 > len) {
      len1 = len;
    }
    if (!is->muted_ && is->audio_buf_ && is->audio_volume_ == SDL_MIX_MAXVOLUME) {
      memcpy(stream, is->audio_buf_ + is->audio_buf_index_, len1);
    } else {
      memset(stream, 0, len1);
      if (!is->muted_ && is->audio_buf_) {
        SDL_MixAudio(stream, is->audio_buf_ + is->audio_buf_index_, len1, is->audio_volume_);
      }
    }
    len -= len1;
    stream += len1;
    is->audio_buf_index_ += len1;
  }
  is->audio_write_buf_size_ = is->audio_buf_size_ - is->audio_buf_index_;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!std::isnan(is->audio_clock_)) {
    const double pts = is->audio_clock_ -
                       static_cast<double>(2 * is->audio_hw_buf_size_ + is->audio_write_buf_size_) /
                           is->audio_tgt_.bytes_per_sec;
    is->astream_->SetClockAt(pts, is->audio_clock_serial_, is->audio_callback_time_ / 1000000.0);
  }
}

int VideoState::QueuePicture(AVFrame* src_frame,
                             double pts,
                             double duration,
                             int64_t pos,
                             int serial) {
#if defined(DEBUG_SYNC)
  printf("frame_type=%c pts=%0.3f\n", av_get_picture_type_char(src_frame->pict_type), pts);
#endif

  core::PacketQueue* video_packet_queue = vstream_->Queue();
  core::VideoFrame* vp = video_frame_queue_->GetPeekWritable();
  if (!vp) {
    return ERROR_RESULT_VALUE;
  }

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = false;

  /* alloc or resize hardware picture buffer */
  if (!vp->bmp || !vp->allocated || vp->width != src_frame->width ||
      vp->height != src_frame->height || vp->format != src_frame->format) {
    SDL_Event event;

    vp->allocated = false;
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    /* the allocation must be done in the main thread to avoid
       locking problems. */
    event.type = FF_ALLOC_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);

    video_frame_queue_->WaitSafeAndNotify([video_packet_queue, vp]() -> bool {
      return !vp->allocated && !video_packet_queue->AbortRequest();
    });

    if (video_packet_queue->AbortRequest()) {
      return ERROR_RESULT_VALUE;
    }
  }

  /* if the frame is not skipped, then display it */
  if (vp->bmp) {
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    av_frame_move_ref(vp->frame, src_frame);
    video_frame_queue_->Push();
  }
  return SUCCESS_RESULT_VALUE;
}

int VideoState::GetVideoFrame(AVFrame* frame) {
  int got_picture = viddec_->DecodeFrame(frame);
  if (got_picture < 0) {
    return -1;
  }

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE) {
      dpts = vstream_->q2d() * frame->pts;
    }

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(ic_, vstream_->AvStream(), frame);

    if (opt_->framedrop > 0 ||
        (opt_->framedrop && GetMasterSyncType() != core::AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - GetMasterClock();
        core::PacketQueue* video_packet_queue = vstream_->Queue();
        if (!std::isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
            diff - frame_last_filter_delay_ < 0 && viddec_->GetPktSerial() == vstream_->Serial() &&
            video_packet_queue->NbPackets()) {
          stats_.frame_drops_early++;
          av_frame_unref(frame);
          got_picture = 0;
        }
      }
    }
  }

  return got_picture;
}

/* this thread gets the stream from the disk or the network */
int VideoState::ReadThread() {
  bool scan_all_pmts_set = false;
  const std::string uri_str = uri_.url();
  const char* in_filename = common::utils::c_strornull(uri_str);
  AVFormatContext* ic = avformat_alloc_context();
  if (!ic) {
    ERROR_LOG() << "Could not allocate context.";
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);
    return AVERROR(ENOMEM);
  }
  ic->interrupt_callback.callback = decode_interrupt_callback;
  ic->interrupt_callback.opaque = this;
  if (!av_dict_get(copt_->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
    av_dict_set(&copt_->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    scan_all_pmts_set = true;
  }
  int err = avformat_open_input(&ic, in_filename, NULL, &copt_->format_opts);  // autodetect format
  if (err < 0) {
    char errbuf[128];
    const char* errbuf_ptr = errbuf;
    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0) {
      errbuf_ptr = strerror(AVUNERROR(err));
    }
    ERROR_LOG() << in_filename << ": " << errbuf_ptr;

    avformat_close_input(&ic);
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);
    return -1;
  }
  if (scan_all_pmts_set) {
    av_dict_set(&copt_->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
  }

  AVDictionaryEntry* t = av_dict_get(copt_->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX);
  if (t) {
    ERROR_LOG() << "Option " << t->key << " not found.";
    avformat_close_input(&ic);
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);
    return AVERROR_OPTION_NOT_FOUND;
  }
  ic_ = ic;

  core::VideoStream* video_stream = vstream_;
  core::AudioStream* audio_stream = astream_;
  core::PacketQueue* video_packet_queue = video_stream->Queue();
  core::PacketQueue* audio_packet_queue = audio_stream->Queue();
  int st_index[AVMEDIA_TYPE_NB];
  memset(st_index, -1, sizeof(st_index));

  if (opt_->genpts) {
    ic->flags |= AVFMT_FLAG_GENPTS;
  }

  av_format_inject_global_side_data(ic);

  AVDictionary** opts = core::setup_find_stream_info_opts(ic, copt_->codec_opts);
  unsigned int orig_nb_streams = ic->nb_streams;

  err = avformat_find_stream_info(ic, opts);

  for (unsigned int i = 0; i < orig_nb_streams; i++) {
    av_dict_free(&opts[i]);
  }
  av_freep(&opts);

  AVPacket pkt1, *pkt = &pkt1;
  int ret;
  if (err < 0) {
    WARNING_LOG() << in_filename << ": could not find codec parameters";
    ret = -1;
    goto fail;
  }

  if (ic->pb) {
    ic->pb->eof_reached =
        0;  // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
  }

  max_frame_duration_ = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
  handler_->OnDiscoveryStream(this, ic);

  /* if seeking requested, we execute it */
  if (opt_->start_time != AV_NOPTS_VALUE) {
    int64_t timestamp = opt_->start_time;
    /* add the stream start time */
    if (ic->start_time != AV_NOPTS_VALUE) {
      timestamp += ic->start_time;
    }
    ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      WARNING_LOG() << in_filename << ": could not seek to position "
                    << static_cast<double>(timestamp) / AV_TIME_BASE;
    }
  }

  realtime_ = core::is_realtime(ic);

  if (opt_->show_status) {
    av_dump_format(ic, 0, in_filename, 0);
  }

  for (int i = 0; i < static_cast<int>(ic->nb_streams); i++) {
    AVStream* st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    const char* want_spec = common::utils::c_strornull(opt_->wanted_stream_spec[type]);
    if (type >= 0 && want_spec && st_index[type] == -1) {
      if (avformat_match_stream_specifier(ic, st, want_spec) > 0) {
        st_index[type] = i;
      }
    }
  }
  for (int i = 0; i < static_cast<int>(AVMEDIA_TYPE_NB); i++) {
    const char* want_spec = common::utils::c_strornull(opt_->wanted_stream_spec[i]);
    if (want_spec && st_index[i] == -1) {
      ERROR_LOG() << "Stream specifier " << want_spec << " does not match any "
                  << av_get_media_type_string(static_cast<AVMediaType>(i)) << " stream";
      st_index[i] = INT_MAX;
    }
  }

  if (!opt_->video_disable) {
    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  }
  if (!opt_->audio_disable) {
    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  }

  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters* codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
    if (codecpar->width) {
      SetDefaultWindowSize(codecpar->width, codecpar->height, sar);
    }
  }

  /* open the streams */
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    StreamComponentOpen(st_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = StreamComponentOpen(st_index[AVMEDIA_TYPE_VIDEO]);
  }
  if (opt_->show_mode == core::SHOW_MODE_NONE) {
    opt_->show_mode = ret >= 0 ? core::SHOW_MODE_VIDEO : core::SHOW_MODE_WAVES;
  }

  if (!video_stream->IsOpened() && !audio_stream->IsOpened()) {
    ERROR_LOG() << "Failed to open file '" << in_filename << "' or configure filtergraph";
    ret = -1;
    goto fail;
  }

  if (opt_->infinite_buffer < 0 && realtime_) {
    opt_->infinite_buffer = 1;
  }

  while (!IsAborted()) {
    if (paused_ != last_paused_) {
      last_paused_ = paused_;
      if (paused_) {
        read_pause_return_ = av_read_pause(ic);
      } else {
        av_read_play(ic);
      }
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (paused_ &&
        (!strcmp(ic->iformat->name, "rtsp") || (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
      /* wait 10 ms to avoid trying to get another packet */
      /* XXX: horrible */
      SDL_Delay(10);
      continue;
    }
#endif
    if (seek_req_) {
      int64_t seek_target = seek_pos_;
      int64_t seek_min = seek_rel_ > 0 ? seek_target - seek_rel_ + 2 : INT64_MIN;
      int64_t seek_max = seek_rel_ < 0 ? seek_target - seek_rel_ - 2 : INT64_MAX;
      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables

      ret = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, seek_flags_);
      if (ret < 0) {
        ERROR_LOG() << ic->filename << ": error while seeking";
      } else {
        if (video_stream->IsOpened()) {
          video_packet_queue->Flush();
          video_packet_queue->Put(core::PacketQueue::FlushPkt());
        }
        if (audio_stream->IsOpened()) {
          audio_packet_queue->Flush();
          audio_packet_queue->Put(core::PacketQueue::FlushPkt());
        }
      }
      seek_req_ = false;
      queue_attachments_req_ = true;
      eof_ = false;
      if (paused_) {
        StepToNextFrame();
      }
    }
    AVStream* video_st = video_stream->IsOpened() ? video_stream->AvStream() : NULL;
    AVStream* audio_st = audio_stream->IsOpened() ? audio_stream->AvStream() : NULL;
    if (queue_attachments_req_) {
      if (video_st && video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        AVPacket copy;
        if ((ret = av_copy_packet(&copy, &video_st->attached_pic)) < 0) {
          goto fail;
        }
        video_packet_queue->Put(&copy);
        video_packet_queue->PutNullpacket(video_stream->Index());
      }
      queue_attachments_req_ = false;
    }

    /* if the queue are full, no need to read more */
    if (opt_->infinite_buffer < 1 &&
        (video_packet_queue->Size() + audio_packet_queue->Size() > MAX_QUEUE_SIZE ||
         (astream_->HasEnoughPackets() && vstream_->HasEnoughPackets()))) {
      continue;
    }
    if (!paused_ && (!audio_st || (auddec_->Finished() == audio_packet_queue->Serial() &&
                                   audio_frame_queue_->IsEmpty())) &&
        (!video_st ||
         (viddec_->Finished() == video_packet_queue->Serial() && video_frame_queue_->IsEmpty()))) {
      if (opt_->loop != 1 && (!opt_->loop || --opt_->loop)) {
        StreamSeek(opt_->start_time != AV_NOPTS_VALUE ? opt_->start_time : 0, 0, 0);
      } else if (opt_->autoexit) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }
    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !eof_) {
        if (video_stream->IsOpened()) {
          video_packet_queue->PutNullpacket(video_stream->Index());
        }
        if (audio_stream->IsOpened()) {
          audio_packet_queue->PutNullpacket(audio_stream->Index());
        }
        eof_ = true;
      }
      if (ic->pb && ic->pb->error) {
        break;
      }
      continue;
    } else {
      eof_ = false;
    }
    /* check if packet is in play range specified by user, then queue, otherwise discard */
    int64_t stream_start_time = ic->streams[pkt->stream_index]->start_time;
    int64_t pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    bool pkt_in_play_range =
        opt_->duration == AV_NOPTS_VALUE ||
        (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                    av_q2d(ic->streams[pkt->stream_index]->time_base) -
                static_cast<double>(opt_->start_time != AV_NOPTS_VALUE ? opt_->start_time : 0) /
                    1000000 <=
            (static_cast<double>(opt_->duration) / 1000000);
    if (pkt->stream_index == audio_stream->Index() && pkt_in_play_range) {
      audio_packet_queue->Put(pkt);
    } else if (pkt->stream_index == video_stream->Index() && pkt_in_play_range &&
               !(video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      video_packet_queue->Put(pkt);
    } else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  if (ret != 0) {
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);
  }
  return 0;
}

int VideoState::AudioThread() {
  core::AudioFrame* af = nullptr;
#if CONFIG_AVFILTER
  int last_serial = -1;
  int64_t dec_channel_layout;
  int reconfigure;
#endif
  int ret = 0;

  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return AVERROR(ENOMEM);
  }

  do {
    int got_frame = auddec_->DecodeFrame(frame);
    if (got_frame < 0) {
      break;
    }

    if (got_frame) {
      AVRational tb = {1, frame->sample_rate};

#if CONFIG_AVFILTER
      dec_channel_layout =
          core::get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));

      reconfigure = core::cmp_audio_fmts(audio_filter_src_.fmt, audio_filter_src_.channels,
                                         static_cast<AVSampleFormat>(frame->format),
                                         av_frame_get_channels(frame)) ||
                    audio_filter_src_.channel_layout != dec_channel_layout ||
                    audio_filter_src_.freq != frame->sample_rate ||
                    auddec_->GetPktSerial() != last_serial;

      if (reconfigure) {
        char buf1[1024], buf2[1024];
        av_get_channel_layout_string(buf1, sizeof(buf1), -1, audio_filter_src_.channel_layout);
        av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
        const std::string mess = common::MemSPrintf(
            "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d "
            "fmt:%s layout:%s serial:%d\n",
            audio_filter_src_.freq, audio_filter_src_.channels,
            av_get_sample_fmt_name(audio_filter_src_.fmt), buf1, last_serial, frame->sample_rate,
            av_frame_get_channels(frame),
            av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)), buf2,
            auddec_->GetPktSerial());
        DEBUG_LOG() << mess;

        audio_filter_src_.fmt = static_cast<AVSampleFormat>(frame->format);
        audio_filter_src_.channels = av_frame_get_channels(frame);
        audio_filter_src_.channel_layout = dec_channel_layout;
        audio_filter_src_.freq = frame->sample_rate;
        last_serial = auddec_->GetPktSerial();

        if ((ret = ConfigureAudioFilters(opt_->afilters, 1)) < 0) {
          break;
        }
      }

      ret = av_buffersrc_add_frame(in_audio_filter_, frame);
      if (ret < 0) {
        break;
      }

      while ((ret = av_buffersink_get_frame_flags(out_audio_filter_, frame, 0)) >= 0) {
        tb = out_audio_filter_->inputs[0]->time_base;
#endif
        af = audio_frame_queue_->GetPeekWritable();
        if (!af) {  // if stoped
#if CONFIG_AVFILTER
          avfilter_graph_free(&agraph_);
#endif
          av_frame_free(&frame);
          return ret;
        }

        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->pos = av_frame_get_pkt_pos(frame);
        af->serial = auddec_->GetPktSerial();
        AVRational tmp = {frame->nb_samples, frame->sample_rate};
        af->duration = av_q2d(tmp);

        av_frame_move_ref(af->frame, frame);
        audio_frame_queue_->Push();

#if CONFIG_AVFILTER
        core::PacketQueue* audio_packet_queue = astream_->Queue();
        if (audio_packet_queue->Serial() != auddec_->GetPktSerial()) {
          break;
        }
      }
      if (ret == AVERROR_EOF) {
        auddec_->SetFinished(true);
      }
#endif
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

#if CONFIG_AVFILTER
  avfilter_graph_free(&agraph_);
#endif
  av_frame_free(&frame);
  return ret;
}

int VideoState::VideoThread() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return AVERROR(ENOMEM);
  }

  double pts;
  double duration;
  int ret;
  AVStream* video_st = vstream_->AvStream();
  AVRational tb = video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(ic_, video_st, NULL);

#if CONFIG_AVFILTER
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterContext *filt_out = NULL, *filt_in = NULL;
  int last_w = 0;
  int last_h = 0;
  enum AVPixelFormat last_format = AV_PIX_FMT_NONE;  //-2
  int last_serial = -1;
  size_t last_vfilter_idx = 0;
  if (!graph) {
    av_frame_free(&frame);
    return AVERROR(ENOMEM);
  }
#endif

  while (true) {
    ret = GetVideoFrame(frame);
    if (ret < 0) {
      goto the_end;
    }
    if (!ret) {
      continue;
    }

#if CONFIG_AVFILTER
    if (last_w != frame->width || last_h != frame->height || last_format != frame->format ||
        last_serial != viddec_->GetPktSerial() || last_vfilter_idx != vfilter_idx_) {
      const std::string mess = common::MemSPrintf(
          "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s "
          "serial:%d\n",
          last_w, last_h,
          static_cast<const char*>(av_x_if_null(av_get_pix_fmt_name(last_format), "none")),
          last_serial, frame->width, frame->height,
          static_cast<const char*>(
              av_x_if_null(av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)), "none")),
          viddec_->GetPktSerial());
      DEBUG_LOG() << mess;
      avfilter_graph_free(&graph);
      graph = avfilter_graph_alloc();
      std::string vfilters;
      if (!opt_->vfilters_list.empty()) {
        vfilters = opt_->vfilters_list[vfilter_idx_];
      }
      if ((ret = ConfigureVideoFilters(graph, vfilters, frame)) < 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = this;
        SDL_PushEvent(&event);
        goto the_end;
      }
      filt_in = in_video_filter_;
      filt_out = out_video_filter_;
      last_w = frame->width;
      last_h = frame->height;
      last_format = static_cast<AVPixelFormat>(frame->format);
      last_serial = viddec_->GetPktSerial();
      last_vfilter_idx = vfilter_idx_;
      frame_rate = filt_out->inputs[0]->frame_rate;
    }

    ret = av_buffersrc_add_frame(filt_in, frame);
    if (ret < 0) {
      goto the_end;
    }

    while (ret >= 0) {
      frame_last_returned_time_ = av_gettime_relative() / 1000000.0;

      ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
      if (ret < 0) {
        if (ret == AVERROR_EOF) {
          viddec_->SetFinished(true);
        }
        ret = 0;
        break;
      }

      frame_last_filter_delay_ = av_gettime_relative() / 1000000.0 - frame_last_returned_time_;
      if (fabs(frame_last_filter_delay_) > AV_NOSYNC_THRESHOLD / 10.0) {
        frame_last_filter_delay_ = 0;
      }
      tb = filt_out->inputs[0]->time_base;
#endif
      AVRational fr = {frame_rate.den, frame_rate.num};
      duration = (frame_rate.num && frame_rate.den ? av_q2d(fr) : 0);
      pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      ret =
          QueuePicture(frame, pts, duration, av_frame_get_pkt_pos(frame), viddec_->GetPktSerial());
      av_frame_unref(frame);
#if CONFIG_AVFILTER
    }
#endif

    if (ret < 0) {
      goto the_end;
    }
  }
the_end:
#if CONFIG_AVFILTER
  avfilter_graph_free(&graph);
#endif
  av_frame_free(&frame);
  return 0;
}

#if CONFIG_AVFILTER
int VideoState::ConfigureVideoFilters(AVFilterGraph* graph,
                                      const std::string& vfilters,
                                      AVFrame* frame) {
  static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA,
                                                AV_PIX_FMT_NONE};
  AVDictionary* sws_dict = copt_->sws_dict;
  AVDictionaryEntry* e = NULL;
  char sws_flags_str[512] = {0};
  while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
    if (strcmp(e->key, "sws_flags") == 0) {
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
    } else {
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
  }
  const size_t len_sws_flags = strlen(sws_flags_str);
  if (len_sws_flags) {
    sws_flags_str[len_sws_flags - 1] = 0;
  }

  AVStream* video_st = vstream_->AvStream();
  if (!video_st) {
    DNOTREACHED();
    return ERROR_RESULT_VALUE;
  }
  AVCodecParameters* codecpar = video_st->codecpar;

  char buffersrc_args[256];
  AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
  graph->scale_sws_opts = av_strdup(sws_flags_str);
  snprintf(buffersrc_args, sizeof(buffersrc_args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", frame->width,
           frame->height, frame->format, video_st->time_base.num, video_st->time_base.den,
           codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
  AVRational fr = av_guess_frame_rate(ic_, video_st, NULL);
  if (fr.num && fr.den) {
    av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);
  }

  int ret = avfilter_graph_create_filter(&filt_src, avfilter_get_by_name("buffer"), "ffplay_buffer",
                                         buffersrc_args, NULL, graph);
  if (ret < 0) {
    return ret;
  }

  ret = avfilter_graph_create_filter(&filt_out, avfilter_get_by_name("buffersink"),
                                     "ffplay_buffersink", NULL, NULL, graph);
  if (ret < 0) {
    return ret;
  }
  ret =
      av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    return ret;
  }

  last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                                                     \
  do {                                                                                             \
    AVFilterContext* filt_ctx;                                                                     \
                                                                                                   \
    ret = avfilter_graph_create_filter(&filt_ctx, avfilter_get_by_name(name), "ffplay_" name, arg, \
                                       NULL, graph);                                               \
    if (ret < 0)                                                                                   \
      return ret;                                                                                  \
                                                                                                   \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                                              \
    if (ret < 0)                                                                                   \
      return ret;                                                                                  \
                                                                                                   \
    last_filter = filt_ctx;                                                                        \
  } while (0)

  if (opt_->autorotate) {
    double theta = core::get_rotation(video_st);

    if (fabs(theta - 90) < 1.0) {
      INSERT_FILT("transpose", "clock");
    } else if (fabs(theta - 180) < 1.0) {
      INSERT_FILT("hflip", NULL);
      INSERT_FILT("vflip", NULL);
    } else if (fabs(theta - 270) < 1.0) {
      INSERT_FILT("transpose", "cclock");
    } else if (fabs(theta) > 1.0) {
      char rotate_buf[64];
      snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
      INSERT_FILT("rotate", rotate_buf);
    }
  }

  if ((ret = core::configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0) {
    return ret;
  }

  in_video_filter_ = filt_src;
  out_video_filter_ = filt_out;
  return ret;
}

int VideoState::ConfigureAudioFilters(const std::string& afilters, int force_output_format) {
  static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
  avfilter_graph_free(&agraph_);
  agraph_ = avfilter_graph_alloc();
  if (!agraph_) {
    return AVERROR(ENOMEM);
  }

  AVDictionaryEntry* e = NULL;
  char aresample_swr_opts[512] = "";
  AVDictionary* swr_opts = copt_->swr_opts;
  while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
    av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
  }
  if (strlen(aresample_swr_opts)) {
    aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
  }
  av_opt_set(agraph_, "aresample_swr_opts", aresample_swr_opts, 0);

  char asrc_args[256];
  int ret = snprintf(asrc_args, sizeof(asrc_args),
                     "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                     audio_filter_src_.freq, av_get_sample_fmt_name(audio_filter_src_.fmt),
                     audio_filter_src_.channels, 1, audio_filter_src_.freq);
  if (audio_filter_src_.channel_layout) {
    snprintf(asrc_args + ret, sizeof(asrc_args) - ret, ":channel_layout=0x%" PRIx64,
             audio_filter_src_.channel_layout);
  }

  AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
  ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                     asrc_args, NULL, agraph_);
  if (ret < 0) {
    avfilter_graph_free(&agraph_);
    return ret;
  }

  ret = avfilter_graph_create_filter(&filt_asink, avfilter_get_by_name("abuffersink"),
                                     "ffplay_abuffersink", NULL, NULL, agraph_);
  if (ret < 0) {
    avfilter_graph_free(&agraph_);
    return ret;
  }

  if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE,
                                 AV_OPT_SEARCH_CHILDREN)) < 0) {
    avfilter_graph_free(&agraph_);
    return ret;
  }
  if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) {
    avfilter_graph_free(&agraph_);
    return ret;
  }

  if (force_output_format) {
    int channels[2] = {0, -1};
    int64_t channel_layouts[2] = {0, -1};
    int sample_rates[2] = {0, -1};
    channel_layouts[0] = audio_tgt_.channel_layout;
    channels[0] = audio_tgt_.channels;
    sample_rates[0] = audio_tgt_.freq;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0) {
      avfilter_graph_free(&agraph_);
      return ret;
    }
    ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
      avfilter_graph_free(&agraph_);
      return ret;
    }
    ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
      avfilter_graph_free(&agraph_);
      return ret;
    }
    ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
      avfilter_graph_free(&agraph_);
      return ret;
    }
  }

  ret = core::configure_filtergraph(agraph_, afilters, filt_asrc, filt_asink);
  if (ret < 0) {
    avfilter_graph_free(&agraph_);
    return ret;
  }

  in_audio_filter_ = filt_asrc;
  out_audio_filter_ = filt_asink;
  return ret;
}
#endif /* CONFIG_AVFILTER */