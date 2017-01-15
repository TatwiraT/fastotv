#include "core/video_state.h"

extern "C" {
#include <SDL2/SDL.h>
}

#include <common/macros.h>
#include <common/utils.h>

#include "core/utils.h"
#include "core/types.h"

#define AV_NOSYNC_THRESHOLD 10.0
#define CURSOR_HIDE_DELAY 1000000
#define USE_ONEPASS_SUBTITLE_RENDER 1
/* Step size for volume control */
#define SDL_VOLUME_STEP (SDL_MIX_MAXVOLUME / 50)
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
/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

#define FF_ALLOC_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

namespace {
int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue) {
  return stream_id < 0 || queue->abort_request() ||
         (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         (queue->nb_packets() > MIN_FRAMES &&
          (!queue->duration() || av_q2d(st->time_base) * queue->duration() > 1.0));
}
}

VideoState::VideoState(AVInputFormat* ifo, AppOptions* opt, ComplexOptions* copt)
    : opt(opt),
      copt(copt),
      audio_callback_time(0),
      ic(NULL),
      auddec(NULL),
      viddec(NULL),
      subdec(NULL),
      vis_texture(NULL),
      sub_texture(NULL),
#if CONFIG_AVFILTER
      vfilter_idx(0),
#endif
      video_engine_(nullptr),
      audio_engine_(nullptr),
      subtitle_engine_(nullptr),
      paused_(false),
      last_paused_(false),
      cursor_hidden_(false),
      cursor_last_shown_(0),
      eof_(false),

      renderer(NULL),
      window(NULL) {
  iformat = ifo;
  ytop = 0;
  xleft = 0;

  video_engine_ = new StreamEngine(VIDEO_PICTURE_QUEUE_SIZE, true);
  audio_engine_ = new StreamEngine(SAMPLE_QUEUE_SIZE, true);
  subtitle_engine_ = new StreamEngine(SUBPICTURE_QUEUE_SIZE, false);

  if (!(continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return;
  }
  audio_clock_serial = -1;
  if (opt->startup_volume < 0) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", opt->startup_volume);
  }
  if (opt->startup_volume > 100) {
    av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", opt->startup_volume);
  }
  opt->startup_volume = av_clip(opt->startup_volume, 0, 100);
  opt->startup_volume =
      av_clip(SDL_MIX_MAXVOLUME * opt->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
  audio_volume = opt->startup_volume;
  muted = 0;
}

VideoState::~VideoState() {
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  abort_request = 1;
  SDL_WaitThread(read_tid, NULL);

  /* close each stream */
  if (audio_stream >= 0) {
    stream_component_close(audio_stream);
  }
  if (video_stream >= 0) {
    stream_component_close(video_stream);
  }
  if (subtitle_stream >= 0) {
    stream_component_close(subtitle_stream);
  }

  avformat_close_input(&ic);

  destroy(&video_engine_);
  destroy(&audio_engine_);
  destroy(&subtitle_engine_);

  SDL_DestroyCond(continue_read_thread);
  sws_freeContext(img_convert_ctx);
  sws_freeContext(sub_convert_ctx);
  if (vis_texture) {
    SDL_DestroyTexture(vis_texture);
    vis_texture = NULL;
  }
  if (sub_texture) {
    SDL_DestroyTexture(sub_texture);
    sub_texture = NULL;
  }

  if (renderer) {
    SDL_DestroyRenderer(renderer);
    renderer = NULL;
  }
  if (window) {
    SDL_DestroyWindow(window);
    window = NULL;
  }
}

int VideoState::stream_component_open(int stream_index) {
  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return AVERROR(EINVAL);
  }

  AVCodecContext* avctx = avcodec_alloc_context3(NULL);
  if (!avctx) {
    return AVERROR(ENOMEM);
  }
  const char* forced_codec_name = NULL;
  AVCodec* codec;
  AVDictionary* opts = NULL;
  AVDictionaryEntry* t = NULL;
  int sample_rate, nb_channels;
  int64_t channel_layout;
  int stream_lowres = opt->lowres;

  int ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
  if (ret < 0) {
    goto fail;
  }
  av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

  codec = avcodec_find_decoder(avctx->codec_id);

  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      last_audio_stream = stream_index;
      forced_codec_name = common::utils::c_strornull(opt->audio_codec_name);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      last_subtitle_stream = stream_index;
      forced_codec_name = common::utils::c_strornull(opt->subtitle_codec_name);
      break;
    case AVMEDIA_TYPE_VIDEO:
      last_video_stream = stream_index;
      forced_codec_name = common::utils::c_strornull(opt->video_codec_name);
      break;
  }
  if (forced_codec_name) {
    codec = avcodec_find_decoder_by_name(forced_codec_name);
  }
  if (!codec) {
    if (forced_codec_name) {
      av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
    } else {
      av_log(NULL, AV_LOG_WARNING, "No codec could be found with id %d\n", avctx->codec_id);
    }
    ret = AVERROR(EINVAL);
    goto fail;
  }

  avctx->codec_id = codec->id;
  if (stream_lowres > av_codec_get_max_lowres(codec)) {
    av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
           av_codec_get_max_lowres(codec));
    stream_lowres = av_codec_get_max_lowres(codec);
  }
  av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
  if (stream_lowres) {
    avctx->flags |= CODEC_FLAG_EMU_EDGE;
  }
#endif
  if (opt->fast) {
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;
  }
#if FF_API_EMU_EDGE
  if (codec->capabilities & AV_CODEC_CAP_DR1) {
    avctx->flags |= CODEC_FLAG_EMU_EDGE;
  }
#endif

  opts = filter_codec_opts(copt->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
  if (!av_dict_get(opts, "threads", NULL, 0)) {
    av_dict_set(&opts, "threads", "auto", 0);
  }
  if (stream_lowres) {
    av_dict_set_int(&opts, "lowres", stream_lowres, 0);
  }
  if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    av_dict_set(&opts, "refcounted_frames", "1", 0);
  }
  if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
    goto fail;
  }
  if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }

  eof_ = false;
  ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
    {
      AVFilterLink* link;

      audio_filter_src.freq = avctx->sample_rate;
      audio_filter_src.channels = avctx->channels;
      audio_filter_src.channel_layout =
          get_valid_channel_layout(avctx->channel_layout, avctx->channels);
      audio_filter_src.fmt = avctx->sample_fmt;
      if ((ret = configure_audio_filters(opt->afilters, 0)) < 0) {
        goto fail;
      }
      link = out_audio_filter->inputs[0];
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
      if ((ret = audio_open(this, channel_layout, nb_channels, sample_rate, &audio_tgt,
                            sdl_audio_callback)) < 0) {
        goto fail;
      }

      audio_hw_buf_size = ret;
      audio_src = audio_tgt;
      audio_buf_size = 0;
      audio_buf_index = 0;

      /* init averaging filter */
      audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
      audio_diff_avg_count = 0;
      /* since we do not have a precise anough audio FIFO fullness,
         we correct audio sync only if larger than this threshold */
      audio_diff_threshold =
          static_cast<double>(audio_hw_buf_size) / static_cast<double>(audio_tgt.bytes_per_sec);

      audio_stream = stream_index;
      audio_st = ic->streams[stream_index];

      auddec = new Decoder(avctx, audio_engine_->packet_queue_, continue_read_thread,
                           opt->decoder_reorder_pts);
      if ((ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
          !ic->iformat->read_seek) {
        auddec->start_pts = audio_st->start_time;
        auddec->start_pts_tb = audio_st->time_base;
      }
      if ((ret = auddec->start(audio_thread, this)) < 0) {
        destroy(&auddec);
        goto out;
      }
      SDL_PauseAudio(0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      video_stream = stream_index;
      video_st = ic->streams[stream_index];

      viddec = new VideoDecoder(avctx, video_engine_->packet_queue_, continue_read_thread,
                                opt->decoder_reorder_pts);
      if ((ret = viddec->start(video_thread, this)) < 0) {
        destroy(&viddec);
        goto out;
      }
      queue_attachments_req = 1;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      subtitle_stream = stream_index;
      subtitle_st = ic->streams[stream_index];

      subdec = new SubDecoder(avctx, subtitle_engine_->packet_queue_, continue_read_thread,
                              opt->decoder_reorder_pts);
      if ((ret = subdec->start(subtitle_thread, this)) < 0) {
        destroy(&subdec);
        goto out;
      }
      break;
    default:
      break;
  }
  goto out;

fail:
  avcodec_free_context(&avctx);
out:
  av_dict_free(&opts);

  return ret;
}

void VideoState::stream_component_close(int stream_index) {
  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return;
  }

  AVCodecParameters* codecpar = ic->streams[stream_index]->codecpar;
  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      auddec->abort(audio_engine_->frame_queue_);
      SDL_CloseAudio();
      destroy(&auddec);
      swr_free(&swr_ctx);
      av_freep(&audio_buf1);
      audio_buf1_size = 0;
      audio_buf = NULL;

      if (rdft) {
        av_rdft_end(rdft);
        av_freep(&rdft_data);
        rdft = NULL;
        rdft_bits_ = 0;
      }
      break;
    case AVMEDIA_TYPE_VIDEO:
      viddec->abort(video_engine_->frame_queue_);
      destroy(&viddec);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      subdec->abort(subtitle_engine_->frame_queue_);
      destroy(&subdec);
      break;
    default:
      break;
  }

  ic->streams[stream_index]->discard = AVDISCARD_ALL;
  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      audio_st = NULL;
      audio_stream = -1;
      break;
    case AVMEDIA_TYPE_VIDEO:
      video_st = NULL;
      video_stream = -1;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      subtitle_st = NULL;
      subtitle_stream = -1;
      break;
    default:
      break;
  }
}

void VideoState::stream_seek(int64_t pos, int64_t rel, int seek_by_bytes) {
  if (!seek_req) {
    seek_pos = pos;
    seek_rel = rel;
    seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (seek_by_bytes) {
      seek_flags |= AVSEEK_FLAG_BYTE;
    }
    seek_req = 1;
    SDL_CondSignal(continue_read_thread);
  }
}

void VideoState::step_to_next_frame() {
  /* if the stream is paused unpause it, then step */
  if (paused_) {
    stream_toggle_pause();
  }
  step = 1;
}

int VideoState::get_master_sync_type() const {
  if (opt->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    if (video_st) {
      return AV_SYNC_VIDEO_MASTER;
    } else {
      return AV_SYNC_AUDIO_MASTER;
    }
  } else if (opt->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    if (audio_st) {
      return AV_SYNC_AUDIO_MASTER;
    } else {
      return AV_SYNC_EXTERNAL_CLOCK;
    }
  }

  return AV_SYNC_EXTERNAL_CLOCK;
}

double VideoState::compute_target_delay(double delay) {
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (get_master_sync_type() != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
       duplicating or deleting a frame */
    diff = video_engine_->GetClock() - get_master_clock();

    /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know
       if it is the best guess */
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs(diff) < max_frame_duration) {
      if (diff <= -sync_threshold)
        delay = FFMAX(0, delay + diff);
      else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        delay = delay + diff;
      else if (diff >= sync_threshold)
        delay = 2 * delay;
    }
  }

  av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

  return delay;
}

/* get the current master clock value */
double VideoState::get_master_clock() {
  double val;

  switch (get_master_sync_type()) {
    case AV_SYNC_VIDEO_MASTER:
      val = video_engine_->GetClock();
      break;
    case AV_SYNC_AUDIO_MASTER:
      val = audio_engine_->GetClock();
      break;
    default:
      val = subtitle_engine_->GetClock();
      break;
  }
  return val;
}

void VideoState::refresh_loop_wait_event(SDL_Event* event) {
  double remaining_time = 0.0;
  SDL_PumpEvents();
  while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden_ && av_gettime_relative() - cursor_last_shown_ > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden_ = 1;
    }
    if (remaining_time > 0.0) {
      const unsigned sleep_time = static_cast<unsigned>(remaining_time * 1000000.0);
      av_usleep(sleep_time);
    }
    remaining_time = REFRESH_RATE;
    if (opt->show_mode != SHOW_MODE_NONE && (!paused_ || force_refresh)) {
      video_refresh(&remaining_time);
    }
    SDL_PumpEvents();
  }
}

void VideoState::video_refresh(double* remaining_time) {
  double time;
  if (!paused_ && get_master_sync_type() == AV_SYNC_EXTERNAL_CLOCK && realtime) {
    check_external_clock_speed();
  }

  if (!opt->display_disable && opt->show_mode != SHOW_MODE_VIDEO && audio_st) {
    time = av_gettime_relative() / 1000000.0;
    if (force_refresh || last_vis_time + opt->rdftspeed < time) {
      video_display();
      last_vis_time = time;
    }
    *remaining_time = FFMIN(*remaining_time, last_vis_time + opt->rdftspeed - time);
  }

  if (video_st) {
  retry:
    if (video_engine_->frame_queue_->nb_remaining() == 0) {
      // nothing to do, no picture to display in the queue
    } else {
      double last_duration, duration, delay;
      Frame *vp, *lastvp;

      /* dequeue the picture */
      lastvp = video_engine_->frame_queue_->peek_last();
      vp = video_engine_->frame_queue_->peek();

      if (vp->serial != video_engine_->packet_queue_->serial) {
        video_engine_->frame_queue_->next();
        goto retry;
      }

      if (lastvp->serial != vp->serial)
        frame_timer = av_gettime_relative() / 1000000.0;

      if (paused_) {
        goto display;
      }

      /* compute nominal last_duration */
      last_duration = vp_duration(lastvp, vp);
      delay = compute_target_delay(last_duration);

      time = av_gettime_relative() / 1000000.0;
      if (time < frame_timer + delay) {
        *remaining_time = FFMIN(frame_timer + delay - time, *remaining_time);
        goto display;
      }

      frame_timer += delay;
      if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX)
        frame_timer = time;

      SDL_LockMutex(video_engine_->frame_queue_->mutex);
      if (!isnan(vp->pts)) {
        update_video_pts(vp->pts, vp->pos, vp->serial);
      }
      SDL_UnlockMutex(video_engine_->frame_queue_->mutex);

      if (video_engine_->frame_queue_->nb_remaining() > 1) {
        Frame* nextvp = video_engine_->frame_queue_->peek_next();
        duration = vp_duration(vp, nextvp);
        if (!step && (opt->framedrop > 0 ||
                      (opt->framedrop && get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) &&
            time > frame_timer + duration) {
          frame_drops_late++;
          video_engine_->frame_queue_->next();
          goto retry;
        }
      }

      if (subtitle_st) {
        while (subtitle_engine_->frame_queue_->nb_remaining() > 0) {
          Frame* sp = subtitle_engine_->frame_queue_->peek();
          Frame* sp2 = NULL;
          if (subtitle_engine_->frame_queue_->nb_remaining() > 1) {
            sp2 = subtitle_engine_->frame_queue_->peek_next();
          }

          if (sp->serial != subtitle_engine_->packet_queue_->serial ||
              (video_engine_->GetPts() >
               (sp->pts + (static_cast<float>(sp->sub.end_display_time) / 1000))) ||
              (sp2 &&
               video_engine_->GetPts() >
                   (sp2->pts + (static_cast<float>(sp2->sub.start_display_time) / 1000)))) {
            if (sp->uploaded) {
              for (unsigned int i = 0; i < sp->sub.num_rects; i++) {
                AVSubtitleRect* sub_rect = sp->sub.rects[i];
                uint8_t* pixels;
                int pitch;
                if (!SDL_LockTexture(sub_texture, reinterpret_cast<SDL_Rect*>(sub_rect),
                                     reinterpret_cast<void**>(&pixels), &pitch)) {
                  for (int j = 0; j < sub_rect->h; j++, pixels += pitch) {
                    memset(pixels, 0, sub_rect->w << 2);
                  }
                  SDL_UnlockTexture(sub_texture);
                }
              }
            }
            subtitle_engine_->frame_queue_->next();
          } else {
            break;
          }
        }
      }

      video_engine_->frame_queue_->next();
      force_refresh = 1;

      if (step && !paused_) {
        stream_toggle_pause();
      }
    }
  display:
    /* display picture */
    if (!opt->display_disable && force_refresh && opt->show_mode == SHOW_MODE_VIDEO &&
        video_engine_->frame_queue_->rindexShown()) {
      video_display();
    }
  }
  force_refresh = 0;
  if (opt->show_status) {
    static int64_t last_time;
    int64_t cur_time;
    int aqsize, vqsize, sqsize;
    double av_diff;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
      aqsize = 0;
      vqsize = 0;
      sqsize = 0;
      if (audio_st) {
        aqsize = audio_engine_->packet_queue_->size();
      }
      if (video_st) {
        vqsize = video_engine_->packet_queue_->size();
      }
      if (subtitle_st) {
        sqsize = subtitle_engine_->packet_queue_->size();
      }
      av_diff = 0;
      if (audio_st && video_st) {
        av_diff = audio_engine_->GetClock() - video_engine_->GetClock();
      } else if (video_st) {
        av_diff = get_master_clock() - video_engine_->GetClock();
      } else if (audio_st) {
        av_diff = get_master_clock() - audio_engine_->GetClock();
      }
      av_log(NULL, AV_LOG_INFO,
             "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%" PRId64 "/%" PRId64 "   \r",
             get_master_clock(),
             (audio_st && video_st) ? "A-V" : (video_st ? "M-V" : (audio_st ? "M-A" : "   ")),
             av_diff, frame_drops_early + frame_drops_late, aqsize / 1024, vqsize / 1024, sqsize,
             video_st ? viddec->ptsCorrectionNumFaultyDts() : 0,
             video_st ? viddec->ptsCorrectionNumFaultyPts() : 0);
      fflush(stdout);
      last_time = cur_time;
    }
  }
}

int VideoState::video_open(Frame* vp) {
  if (vp && vp->width) {
    set_default_window_size(vp->width, vp->height, vp->sar);
  }

  int w, h;
  if (opt->screen_width) {
    w = opt->screen_width;
    h = opt->screen_height;
  } else {
    w = opt->default_width;
    h = opt->default_height;
  }

  if (!window) {
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (opt->window_title.empty()) {
      opt->window_title = opt->input_filename;
    }
    if (opt->is_full_screen) {
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    window = SDL_CreateWindow(opt->window_title.c_str(), SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, w, h, flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      SDL_RendererInfo info;
      renderer =
          SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer) {
        av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer, &info))
          av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", info.name);
      }
    }
  } else {
    SDL_SetWindowSize(window, w, h);
  }

  if (!window || !renderer) {
    av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
    return ERROR_RESULT_VALUE;
  }

  width = w;
  height = h;

  return 0;
}

int VideoState::alloc_picture() {
  Frame* vp = &video_engine_->frame_queue_->queue[video_engine_->frame_queue_->windex()];

  int res = video_open(vp);
  if (res == ERROR_RESULT_VALUE) {
    return ERROR_RESULT_VALUE;
  }

  Uint32 sdl_format;
  if (vp->format == AV_PIX_FMT_YUV420P) {
    sdl_format = SDL_PIXELFORMAT_YV12;
  } else {
    sdl_format = SDL_PIXELFORMAT_ARGB8888;
  }

  if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
    /* SDL allocates a buffer smaller than requested if the video
     * overlay hardware is unable to support the requested size. */
    av_log(NULL, AV_LOG_FATAL,
           "Error: the video system does not support an image\n"
           "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
           "to reduce the image size.\n",
           vp->width, vp->height);
    return ERROR_RESULT_VALUE;
  }

  SDL_LockMutex(video_engine_->frame_queue_->mutex);
  vp->allocated = 1;
  SDL_CondSignal(video_engine_->frame_queue_->cond);
  SDL_UnlockMutex(video_engine_->frame_queue_->mutex);
  return SUCCESS_RESULT_VALUE;
}

/* display the current picture, if any */
void VideoState::video_display() {
  if (!window) {
    video_open(NULL);
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (audio_st && opt->show_mode != SHOW_MODE_VIDEO) {
    video_audio_display();
  } else if (video_st) {
    video_image_display();
  }
  SDL_RenderPresent(renderer);
}

void VideoState::toggle_full_screen() {
  opt->is_full_screen = !opt->is_full_screen;
  SDL_SetWindowFullscreen(window, opt->is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

int VideoState::realloc_texture(SDL_Texture** texture,
                                Uint32 new_format,
                                int new_width,
                                int new_height,
                                SDL_BlendMode blendmode,
                                int init_texture) {
  Uint32 format;
  int access, w, h;
  if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w ||
      new_height != h || new_format != format) {
    void* pixels;
    int pitch;
    SDL_DestroyTexture(*texture);
    if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width,
                                       new_height))) {
      return ERROR_RESULT_VALUE;
    }
    if (SDL_SetTextureBlendMode(*texture, blendmode) < 0) {
      return ERROR_RESULT_VALUE;
    }
    if (init_texture) {
      if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0) {
        return ERROR_RESULT_VALUE;
      }
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(*texture);
    }
  }
  return SUCCESS_RESULT_VALUE;
}

void VideoState::set_default_window_size(int width, int height, AVRational sar) {
  SDL_Rect rect;
  calculate_display_rect(&rect, 0, 0, INT_MAX, height, width, height, sar);
  opt->default_width = rect.w;
  opt->default_height = rect.h;
}

void VideoState::video_image_display() {
  Frame* sp = NULL;
  Frame* vp = video_engine_->frame_queue_->peek_last();
  if (vp->bmp) {
    if (subtitle_st) {
      if (subtitle_engine_->frame_queue_->nb_remaining() > 0) {
        sp = subtitle_engine_->frame_queue_->peek();

        if (vp->pts >= sp->pts + (static_cast<float>(sp->sub.start_display_time) / 1000)) {
          if (!sp->uploaded) {
            uint8_t* pixels[4];
            int pitch[4];
            if (!sp->width || !sp->height) {
              sp->width = vp->width;
              sp->height = vp->height;
            }
            if (realloc_texture(&sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height,
                                SDL_BLENDMODE_BLEND, 1) < 0) {
              return;
            }

            for (unsigned int i = 0; i < sp->sub.num_rects; i++) {
              AVSubtitleRect* sub_rect = sp->sub.rects[i];

              sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
              sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
              sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
              sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

              sub_convert_ctx = sws_getCachedContext(sub_convert_ctx, sub_rect->w, sub_rect->h,
                                                     AV_PIX_FMT_PAL8, sub_rect->w, sub_rect->h,
                                                     AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL);
              if (!sub_convert_ctx) {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                return;
              }
              if (!SDL_LockTexture(sub_texture, reinterpret_cast<SDL_Rect*>(sub_rect),
                                   reinterpret_cast<void**>(pixels), pitch)) {
                sws_scale(sub_convert_ctx, const_cast<const uint8_t* const*>(sub_rect->data),
                          sub_rect->linesize, 0, sub_rect->h, pixels, pitch);
                SDL_UnlockTexture(sub_texture);
              }
            }
            sp->uploaded = 1;
          }
        } else
          sp = NULL;
      }
    }

    SDL_Rect rect;
    calculate_display_rect(&rect, xleft, ytop, width, height, vp->width, vp->height, vp->sar);

    if (!vp->uploaded) {
      if (upload_texture(vp->bmp, vp->frame, &img_convert_ctx) < 0)
        return;
      vp->uploaded = 1;
      vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderCopyEx(renderer, vp->bmp, NULL, &rect, 0, NULL,
                     vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
      SDL_RenderCopy(renderer, sub_texture, NULL, &rect);
#else
      int i;
      double xratio = (double)rect.w / (double)sp->width;
      double yratio = (double)rect.h / (double)sp->height;
      for (i = 0; i < sp->sub.num_rects; i++) {
        SDL_Rect* sub_rect = (SDL_Rect*)sp->sub.rects[i];
        SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                           .y = rect.y + sub_rect->y * yratio,
                           .w = sub_rect->w * xratio,
                           .h = sub_rect->h * yratio};
        SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
      }
#endif
    }
  }
}

void VideoState::video_audio_display() {
  int rdft_bits;
  for (rdft_bits = 1; (1 << rdft_bits) < 2 * height; rdft_bits++) {
  }
  const int nb_freq = 1 << (rdft_bits - 1);

  int i, i_start, x, y1, y, ys, delay, n;
  int h, h2;
  /* compute display index : center on currently output samples */
  const int channels = audio_tgt.channels;
  if (!paused_) {
    int data_used = opt->show_mode == SHOW_MODE_WAVES ? width : (2 * nb_freq);
    n = 2 * channels;
    delay = audio_write_buf_size;
    delay /= n;

    /* to be more precise, we take into account the time spent since
       the last buffer computation */
    if (audio_callback_time) {
      int64_t time_diff = av_gettime_relative() - audio_callback_time;
      delay -= (time_diff * audio_tgt.freq) / 1000000;
    }

    delay += 2 * data_used;
    if (delay < data_used) {
      delay = data_used;
    }

    i_start = x = compute_mod(sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
    if (opt->show_mode == SHOW_MODE_WAVES) {
      h = INT_MIN;
      for (i = 0; i < 1000; i += channels) {
        int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
        int a = sample_array[idx];
        int b = sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
        int c = sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
        int d = sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
        int score = a - d;
        if (h < score && (b ^ c) < 0) {
          h = score;
          i_start = idx;
        }
      }
    }

    last_i_start = i_start;
  } else {
    i_start = last_i_start;
  }

  int nb_display_channels = channels;
  if (opt->show_mode == SHOW_MODE_WAVES) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    /* total height for one channel */
    h = height / nb_display_channels;
    /* graph height / 2 */
    h2 = (h * 9) / 20;
    for (int ch = 0; ch < nb_display_channels; ch++) {
      i = i_start + ch;
      y1 = ytop + ch * h + (h / 2); /* position of center line */
      for (x = 0; x < width; x++) {
        y = (sample_array[i] * h2) >> 15;
        if (y < 0) {
          y = -y;
          ys = y1 - y;
        } else {
          ys = y1;
        }
        fill_rectangle(renderer, xleft + x, ys, 1, y);
        i += channels;
        if (i >= SAMPLE_ARRAY_SIZE)
          i -= SAMPLE_ARRAY_SIZE;
      }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

    for (int ch = 1; ch < nb_display_channels; ch++) {
      y = ytop + ch * h;
      fill_rectangle(renderer, xleft, y, width, 1);
    }
  } else {
    if (realloc_texture(&vis_texture, SDL_PIXELFORMAT_ARGB8888, width, height, SDL_BLENDMODE_NONE,
                        1) < 0) {
      return;
    }

    nb_display_channels = FFMIN(nb_display_channels, 2);
    if (rdft_bits_ != rdft_bits) {
      av_rdft_end(rdft);
      av_free(rdft_data);
      rdft = av_rdft_init(rdft_bits, DFT_R2C);
      rdft_bits_ = rdft_bits;
      rdft_data = static_cast<FFTSample*>(av_malloc_array(nb_freq, 4 * sizeof(*rdft_data)));
    }
    if (!rdft || !rdft_data) {
      av_log(NULL, AV_LOG_ERROR,
             "Failed to allocate buffers for RDFT, switching to waves display\n");
      opt->show_mode = SHOW_MODE_WAVES;
    } else {
      FFTSample* data[2];
      SDL_Rect rect = {.x = xpos, .y = 0, .w = 1, .h = height};
      uint32_t* pixels;
      int pitch;
      for (int ch = 0; ch < nb_display_channels; ch++) {
        data[ch] = rdft_data + 2 * nb_freq * ch;
        i = i_start + ch;
        for (x = 0; x < 2 * nb_freq; x++) {
          double w = (x - nb_freq) * (1.0 / nb_freq);
          data[ch][x] = sample_array[i] * (1.0 - w * w);
          i += channels;
          if (i >= SAMPLE_ARRAY_SIZE)
            i -= SAMPLE_ARRAY_SIZE;
        }
        av_rdft_calc(rdft, data[ch]);
      }
      /* Least efficient way to do this, we should of course
       * directly access it but it is more than fast enough. */
      if (!SDL_LockTexture(vis_texture, &rect, reinterpret_cast<void**>(&pixels), &pitch)) {
        pitch >>= 2;
        pixels += pitch * height;
        for (y = 0; y < height; y++) {
          double w = 1 / sqrt(nb_freq);
          int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] +
                                data[0][2 * y + 1] * data[0][2 * y + 1]));
          int b = (nb_display_channels == 2)
                      ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                      : a;
          a = FFMIN(a, 255);
          b = FFMIN(b, 255);
          pixels -= pitch;
          *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
        }
        SDL_UnlockTexture(vis_texture);
      }
      SDL_RenderCopy(renderer, vis_texture, NULL, NULL);
    }
    if (!paused_) {
      xpos++;
    }
    if (xpos >= width) {
      xpos = xleft;
    }
  }
}

int VideoState::exec() {
  read_tid = SDL_CreateThread(read_thread, "read_thread", this);
  if (!read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

  SDL_Event event;
  double incr, pos, frac;
  VideoState* cur_stream = this;
  while (true) {
    double x;
    refresh_loop_wait_event(&event);
    switch (event.type) {
      case SDL_KEYDOWN: {
        if (opt->exit_on_keydown) {
          return EXIT_SUCCESS;
        }
        switch (event.key.keysym.sym) {
          case SDLK_ESCAPE:
          case SDLK_q:
            return EXIT_SUCCESS;
          case SDLK_f:
            toggle_full_screen();
            cur_stream->force_refresh = 1;
            break;
          case SDLK_p:
          case SDLK_SPACE:
            toggle_pause();
            break;
          case SDLK_m:
            toggle_mute();
            break;
          case SDLK_KP_MULTIPLY:
          case SDLK_0:
            update_volume(1, SDL_VOLUME_STEP);
            break;
          case SDLK_KP_DIVIDE:
          case SDLK_9:
            update_volume(-1, SDL_VOLUME_STEP);
            break;
          case SDLK_s:  // S: Step to next frame
            step_to_next_frame();
            break;
          case SDLK_a:
            stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
            break;
          case SDLK_v:
            stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
            break;
          case SDLK_c:
            stream_cycle_channel(AVMEDIA_TYPE_VIDEO);
            stream_cycle_channel(AVMEDIA_TYPE_AUDIO);
            stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
            break;
          case SDLK_t:
            stream_cycle_channel(AVMEDIA_TYPE_SUBTITLE);
            break;
          case SDLK_w: {
#if CONFIG_AVFILTER
            const int nb_vfilters = opt->vfilters_list.size();
            if (cur_stream->opt->show_mode == SHOW_MODE_VIDEO &&
                cur_stream->vfilter_idx < nb_vfilters - 1) {
              if (++cur_stream->vfilter_idx >= nb_vfilters)
                cur_stream->vfilter_idx = 0;
            } else {
              cur_stream->vfilter_idx = 0;
              toggle_audio_display();
            }
#else
            toggle_audio_display(cur_stream);
#endif
            break;
          }
          case SDLK_PAGEUP:
            if (cur_stream->ic->nb_chapters <= 1) {
              incr = 600.0;
              goto do_seek;
            }
            seek_chapter(1);
            break;
          case SDLK_PAGEDOWN:
            if (cur_stream->ic->nb_chapters <= 1) {
              incr = -600.0;
              goto do_seek;
            }
            seek_chapter(-1);
            break;
          case SDLK_LEFT:
            incr = -10.0;
            goto do_seek;
          case SDLK_RIGHT:
            incr = 10.0;
            goto do_seek;
          case SDLK_UP:
            incr = 60.0;
            goto do_seek;
          case SDLK_DOWN:
            incr = -60.0;
          do_seek:
            if (opt->seek_by_bytes) {
              pos = -1;
              if (pos < 0 && cur_stream->video_stream >= 0) {
                pos = cur_stream->video_engine_->frame_queue_->last_pos();
              }
              if (pos < 0 && cur_stream->audio_stream >= 0) {
                pos = cur_stream->audio_engine_->frame_queue_->last_pos();
              }
              if (pos < 0) {
                pos = avio_tell(cur_stream->ic->pb);
              }
              if (cur_stream->ic->bit_rate) {
                incr *= cur_stream->ic->bit_rate / 8.0;
              } else {
                incr *= 180000.0;
              }
              pos += incr;
              cur_stream->stream_seek(pos, incr, 1);
            } else {
              pos = get_master_clock();
              if (isnan(pos)) {
                pos = static_cast<double>(cur_stream->seek_pos) / AV_TIME_BASE;
              }
              pos += incr;
              if (cur_stream->ic->start_time != AV_NOPTS_VALUE &&
                  pos < cur_stream->ic->start_time / static_cast<double>(AV_TIME_BASE))
                pos = cur_stream->ic->start_time / static_cast<double>(AV_TIME_BASE);
              cur_stream->stream_seek(static_cast<int64_t>(pos * AV_TIME_BASE),
                                      static_cast<int64_t>(incr * AV_TIME_BASE), 0);
            }
            break;
          default:
            break;
        }
        break;
      }
      case SDL_MOUSEBUTTONDOWN: {
        if (opt->exit_on_mousedown) {
          return EXIT_SUCCESS;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
          static int64_t last_mouse_left_click = 0;
          if (av_gettime_relative() - last_mouse_left_click <= 500000) {
            toggle_full_screen();
            cur_stream->force_refresh = 1;
            last_mouse_left_click = 0;
          } else {
            last_mouse_left_click = av_gettime_relative();
          }
        }
      }
      case SDL_MOUSEMOTION: {
        if (cursor_hidden_) {
          SDL_ShowCursor(1);
          cursor_hidden_ = false;
        }
        cursor_last_shown_ = av_gettime_relative();
        if (event.type == SDL_MOUSEBUTTONDOWN) {
          if (event.button.button != SDL_BUTTON_RIGHT)
            break;
          x = event.button.x;
        } else {
          if (!(event.motion.state & SDL_BUTTON_RMASK))
            break;
          x = event.motion.x;
        }
        if (opt->seek_by_bytes || cur_stream->ic->duration <= 0) {
          const int64_t size = avio_size(cur_stream->ic->pb);
          const int64_t pos = size * x / cur_stream->width;
          cur_stream->stream_seek(pos, 0, 1);
        } else {
          int ns, hh, mm, ss;
          int tns, thh, tmm, tss;
          tns = cur_stream->ic->duration / 1000000LL;
          thh = tns / 3600;
          tmm = (tns % 3600) / 60;
          tss = (tns % 60);
          frac = x / cur_stream->width;
          ns = frac * tns;
          hh = ns / 3600;
          mm = (ns % 3600) / 60;
          ss = (ns % 60);
          av_log(NULL, AV_LOG_INFO,
                 "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n",
                 frac * 100, hh, mm, ss, thh, tmm, tss);
          int64_t ts = frac * cur_stream->ic->duration;
          if (cur_stream->ic->start_time != AV_NOPTS_VALUE) {
            ts += cur_stream->ic->start_time;
          }
          cur_stream->stream_seek(ts, 0, 0);
        }
        break;
      }
      case SDL_WINDOWEVENT: {
        switch (event.window.event) {
          case SDL_WINDOWEVENT_RESIZED: {
            opt->screen_width = cur_stream->width = event.window.data1;
            opt->screen_height = cur_stream->height = event.window.data2;
            if (cur_stream->vis_texture) {
              SDL_DestroyTexture(cur_stream->vis_texture);
              cur_stream->vis_texture = NULL;
            }
            cur_stream->force_refresh = 1;
            break;
          }
          case SDL_WINDOWEVENT_EXPOSED: {
            cur_stream->force_refresh = 1;
            break;
          }
        }
        break;
      }
      case SDL_QUIT:
      case FF_QUIT_EVENT: {
        return EXIT_SUCCESS;
      }
      case FF_ALLOC_EVENT: {
        int res = static_cast<VideoState*>(event.user.data1)->alloc_picture();
        if (res == ERROR_RESULT_VALUE) {
          return EXIT_FAILURE;
        }
        break;
      }
      default:
        break;
    }
  }
  return EXIT_SUCCESS;
}

void VideoState::check_external_clock_speed() {
  if ((video_stream >= 0 &&
       video_engine_->packet_queue_->nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES) ||
      (audio_stream >= 0 &&
       audio_engine_->packet_queue_->nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES)) {
    subtitle_engine_->SetClockSpeed(
        FFMAX(EXTERNAL_CLOCK_SPEED_MIN, subtitle_engine_->GetSpeed() - EXTERNAL_CLOCK_SPEED_STEP));
  } else if ((video_stream < 0 ||
              video_engine_->packet_queue_->nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (audio_stream < 0 ||
              audio_engine_->packet_queue_->nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES)) {
    subtitle_engine_->SetClockSpeed(
        FFMIN(EXTERNAL_CLOCK_SPEED_MAX, subtitle_engine_->GetSpeed() + EXTERNAL_CLOCK_SPEED_STEP));
  } else {
    double speed = subtitle_engine_->GetSpeed();
    if (speed != 1.0) {
      subtitle_engine_->SetClockSpeed(
          speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
  }
}

double VideoState::vp_duration(Frame* vp, Frame* nextvp) {
  if (vp->serial == nextvp->serial) {
    double duration = nextvp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > max_frame_duration) {
      return vp->duration;
    } else {
      return duration;
    }
  } else {
    return 0.0;
  }
}

void VideoState::update_video_pts(double pts, int64_t pos, int serial) {
  UNUSED(pos);

  /* update current video pts */
  video_engine_->SetClock(pts, serial);
  subtitle_engine_->SyncClockWith(video_engine_, AV_NOSYNC_THRESHOLD);
}

void VideoState::stream_toggle_pause() {
  if (paused_) {
    frame_timer += av_gettime_relative() / 1000000.0 - video_engine_->LastUpdatedClock();
    if (read_pause_return != AVERROR(ENOSYS)) {
      video_engine_->SetPaused(false);
    }
    video_engine_->SyncSerialClock();
  }
  subtitle_engine_->SyncSerialClock();
  paused_ = !paused_;
  audio_engine_->SetPaused(paused_);
  video_engine_->SetPaused(paused_);
  subtitle_engine_->SetPaused(paused_);
}

void VideoState::toggle_pause() {
  stream_toggle_pause();
  step = 0;
}

void VideoState::toggle_mute() {
  muted = !muted;
}

void VideoState::update_volume(int sign, int step) {
  audio_volume = av_clip(audio_volume + sign * step, 0, SDL_MIX_MAXVOLUME);
}

void VideoState::toggle_audio_display() {
  int next = opt->show_mode;
  do {
    next = (next + 1) % SHOW_MODE_NB;
  } while (next != opt->show_mode &&
           ((next == SHOW_MODE_VIDEO && !video_st) || (next != SHOW_MODE_VIDEO && !audio_st)));
  if (opt->show_mode != next) {
    force_refresh = 1;
    opt->show_mode = static_cast<ShowMode>(next);
  }
}

void VideoState::seek_chapter(int incr) {
  if (!ic->nb_chapters) {
    return;
  }

  int i;
  int64_t pos = get_master_clock() * AV_TIME_BASE;
  /* find the current chapter */
  for (i = 0; i < ic->nb_chapters; i++) {
    AVChapter* ch = ic->chapters[i];
    if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
      i--;
      break;
    }
  }

  i += incr;
  i = FFMAX(i, 0);
  if (i >= ic->nb_chapters) {
    return;
  }

  av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
  stream_seek(av_rescale_q(ic->chapters[i]->start, ic->chapters[i]->time_base, AV_TIME_BASE_Q), 0,
              0);
}

void VideoState::stream_cycle_channel(int codec_type) {
  int start_index, stream_index;
  int old_index;
  AVStream* st;
  AVProgram* p = NULL;
  int lnb_streams = ic->nb_streams;

  if (codec_type == AVMEDIA_TYPE_VIDEO) {
    start_index = last_video_stream;
    old_index = video_stream;
  } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
    start_index = last_audio_stream;
    old_index = audio_stream;
  } else {
    start_index = last_subtitle_stream;
    old_index = subtitle_stream;
  }
  stream_index = start_index;

  if (codec_type != AVMEDIA_TYPE_VIDEO && video_stream != -1) {
    p = av_find_program_from_stream(ic, NULL, video_stream);
    if (p) {
      lnb_streams = p->nb_stream_indexes;
      for (start_index = 0; start_index < lnb_streams; start_index++)
        if (p->stream_index[start_index] == stream_index)
          break;
      if (start_index == lnb_streams)
        start_index = -1;
      stream_index = start_index;
    }
  }

  while (true) {
    if (++stream_index >= lnb_streams) {
      if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        stream_index = -1;
        last_subtitle_stream = -1;
        goto the_end;
      }
      if (start_index == -1)
        return;
      stream_index = 0;
    }
    if (stream_index == start_index)
      return;
    st = ic->streams[p ? p->stream_index[stream_index] : stream_index];
    if (st->codecpar->codec_type == codec_type) {
      /* check that parameters are OK */
      switch (codec_type) {
        case AVMEDIA_TYPE_AUDIO:
          if (st->codecpar->sample_rate != 0 && st->codecpar->channels != 0)
            goto the_end;
          break;
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_SUBTITLE:
          goto the_end;
        default:
          break;
      }
    }
  }
the_end:
  if (p && stream_index != -1)
    stream_index = p->stream_index[stream_index];
  av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
         av_get_media_type_string(static_cast<AVMediaType>(codec_type)), old_index, stream_index);

  stream_component_close(old_index);
  stream_component_open(stream_index);
}

void VideoState::update_sample_display(short* samples, int samples_size) {
  int size = samples_size / sizeof(short);
  while (size > 0) {
    int len = SAMPLE_ARRAY_SIZE - sample_array_index;
    if (len > size) {
      len = size;
    }
    memcpy(sample_array + sample_array_index, samples, len * sizeof(short));
    samples += len;
    sample_array_index += len;
    if (sample_array_index >= SAMPLE_ARRAY_SIZE) {
      sample_array_index = 0;
    }
    size -= len;
  }
}

int VideoState::synchronize_audio(int nb_samples) {
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (get_master_sync_type() != AV_SYNC_AUDIO_MASTER) {
    int min_nb_samples, max_nb_samples;
    double diff = audio_engine_->GetClock() - get_master_clock();
    if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      audio_diff_cum = diff + audio_diff_avg_coef * audio_diff_cum;
      if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        double avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coef);
        if (fabs(avg_diff) >= audio_diff_threshold) {
          wanted_nb_samples = nb_samples + static_cast<int>(diff * audio_src.freq);
          min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
               avg_diff, wanted_nb_samples - nb_samples, audio_clock, audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so
         reset A-V filter */
      audio_diff_avg_count = 0;
      audio_diff_cum = 0;
    }
  }

  return wanted_nb_samples;
}

int VideoState::audio_decode_frame() {
  int data_size, resampled_data_size;
  int64_t dec_channel_layout;
  av_unused double audio_clock0;
  int wanted_nb_samples;
  Frame* af;

  if (paused_) {
    return -1;
  }

  do {
#if defined(_WIN32)
    while (audio_engine_->frame_queue_->nb_remaining() == 0) {
      if ((av_gettime_relative() - audio_callback_time) >
          1000000LL * audio_hw_buf_size / audio_tgt.bytes_per_sec / 2)
        return -1;
      av_usleep(1000);
    }
#endif
    if (!(af = audio_engine_->frame_queue_->peek_readable())) {
      return -1;
    }
    audio_engine_->frame_queue_->next();
  } while (af->serial != audio_engine_->packet_queue_->serial);

  const AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(af->frame->format);
  data_size = av_samples_get_buffer_size(NULL, av_frame_get_channels(af->frame),
                                         af->frame->nb_samples, sample_fmt, 1);

  dec_channel_layout = (af->frame->channel_layout &&
                        av_frame_get_channels(af->frame) ==
                            av_get_channel_layout_nb_channels(af->frame->channel_layout))
                           ? af->frame->channel_layout
                           : av_get_default_channel_layout(av_frame_get_channels(af->frame));
  wanted_nb_samples = synchronize_audio(af->frame->nb_samples);

  if (af->frame->format != audio_src.fmt || dec_channel_layout != audio_src.channel_layout ||
      af->frame->sample_rate != audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !swr_ctx)) {
    swr_free(&swr_ctx);
    swr_ctx = swr_alloc_set_opts(NULL, audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
                                 dec_channel_layout, sample_fmt, af->frame->sample_rate, 0, NULL);
    if (!swr_ctx || swr_init(swr_ctx) < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz "
             "%s %d channels!\n",
             af->frame->sample_rate, av_get_sample_fmt_name(sample_fmt),
             av_frame_get_channels(af->frame), audio_tgt.freq,
             av_get_sample_fmt_name(audio_tgt.fmt), audio_tgt.channels);
      swr_free(&swr_ctx);
      return -1;
    }
    audio_src.channel_layout = dec_channel_layout;
    audio_src.channels = av_frame_get_channels(af->frame);
    audio_src.freq = af->frame->sample_rate;
    audio_src.fmt = sample_fmt;
  }

  if (swr_ctx) {
    const uint8_t** in = const_cast<const uint8_t**>(af->frame->extended_data);
    uint8_t** out = &audio_buf1;
    int out_count =
        static_cast<int64_t>(wanted_nb_samples) * audio_tgt.freq / af->frame->sample_rate + 256;
    int out_size =
        av_samples_get_buffer_size(NULL, audio_tgt.channels, out_count, audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation(swr_ctx, (wanted_nb_samples - af->frame->nb_samples) *
                                            audio_tgt.freq / af->frame->sample_rate,
                               wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate) < 0) {
        av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
        return -1;
      }
    }
    av_fast_malloc(&audio_buf1, &audio_buf1_size, out_size);
    if (!audio_buf1) {
      return AVERROR(ENOMEM);
    }
    len2 = swr_convert(swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
    }
    if (len2 == out_count) {
      av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init(swr_ctx) < 0)
        swr_free(&swr_ctx);
    }
    audio_buf = audio_buf1;
    resampled_data_size = len2 * audio_tgt.channels * av_get_bytes_per_sample(audio_tgt.fmt);
  } else {
    audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
  }

  audio_clock0 = audio_clock;
  /* update the audio clock with the pts */
  if (!isnan(af->pts)) {
    audio_clock = af->pts + static_cast<double>(af->frame->nb_samples) / af->frame->sample_rate;
  } else {
    audio_clock = NAN;
  }
  audio_clock_serial = af->serial;
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

  is->audio_callback_time = av_gettime_relative();

  while (len > 0) {
    if (is->audio_buf_index >= is->audio_buf_size) {
      int audio_size = is->audio_decode_frame();
      if (audio_size < 0) {
        /* if error, just output silence */
        is->audio_buf = NULL;
        is->audio_buf_size =
            SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
      } else {
        if (is->opt->show_mode != SHOW_MODE_VIDEO) {
          is->update_sample_display(reinterpret_cast<int16_t*>(is->audio_buf), audio_size);
        }
        is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0;
    }
    int len1 = is->audio_buf_size - is->audio_buf_index;
    if (len1 > len) {
      len1 = len;
    }
    if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, is->audio_buf + is->audio_buf_index, len1);
    } else {
      memset(stream, 0, len1);
      if (!is->muted && is->audio_buf) {
        SDL_MixAudio(stream, is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
      }
    }
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
  is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!isnan(is->audio_clock)) {
    is->audio_engine_->SetClockAt(
        is->audio_clock -
            static_cast<double>(2 * is->audio_hw_buf_size + is->audio_write_buf_size) /
                is->audio_tgt.bytes_per_sec,
        is->audio_clock_serial, is->audio_callback_time / 1000000.0);
    is->subtitle_engine_->SyncClockWith(is->audio_engine_, AV_NOSYNC_THRESHOLD);
  }
}

int VideoState::queue_picture(AVFrame* src_frame,
                              double pts,
                              double duration,
                              int64_t pos,
                              int serial) {
#if defined(DEBUG_SYNC)
  printf("frame_type=%c pts=%0.3f\n", av_get_picture_type_char(src_frame->pict_type), pts);
#endif

  Frame* vp = video_engine_->frame_queue_->peek_writable();
  if (!vp) {
    return ERROR_RESULT_VALUE;
  }

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = 0;

  /* alloc or resize hardware picture buffer */
  if (!vp->bmp || !vp->allocated || vp->width != src_frame->width ||
      vp->height != src_frame->height || vp->format != src_frame->format) {
    SDL_Event event;

    vp->allocated = 0;
    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    /* the allocation must be done in the main thread to avoid
       locking problems. */
    event.type = FF_ALLOC_EVENT;
    event.user.data1 = this;
    SDL_PushEvent(&event);

    /* wait until the picture is allocated */
    SDL_LockMutex(video_engine_->frame_queue_->mutex);
    while (!vp->allocated && !video_engine_->packet_queue_->abort_request()) {
      SDL_CondWait(video_engine_->frame_queue_->cond, video_engine_->frame_queue_->mutex);
    }
    /* if the queue is aborted, we have to pop the pending ALLOC event or wait for the allocation to
     * complete */
    if (video_engine_->packet_queue_->abort_request() &&
        SDL_PeepEvents(&event, 1, SDL_GETEVENT, FF_ALLOC_EVENT, FF_ALLOC_EVENT) != 1) {
      while (!vp->allocated && !abort_request) {
        SDL_CondWait(video_engine_->frame_queue_->cond, video_engine_->frame_queue_->mutex);
      }
    }
    SDL_UnlockMutex(video_engine_->frame_queue_->mutex);

    if (video_engine_->packet_queue_->abort_request()) {
      return -1;
    }
  }

  /* if the frame is not skipped, then display it */
  if (vp->bmp) {
    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    av_frame_move_ref(vp->frame, src_frame);
    video_engine_->frame_queue_->push();
  }
  return 0;
}

int VideoState::get_video_frame(AVFrame* frame) {
  int got_picture;

  if ((got_picture = viddec->decodeFrame(frame, NULL)) < 0) {
    return -1;
  }

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE) {
      dpts = av_q2d(video_st->time_base) * frame->pts;
    }

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(ic, video_st, frame);

    if (opt->framedrop > 0 || (opt->framedrop && get_master_sync_type() != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - get_master_clock();
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
            diff - frame_last_filter_delay < 0 && viddec->pktSerial() == video_engine_->Serial() &&
            video_engine_->packet_queue_->nb_packets()) {
          frame_drops_early++;
          av_frame_unref(frame);
          got_picture = 0;
        }
      }
    }
  }

  return got_picture;
}

/* this thread gets the stream from the disk or the network */
int VideoState::read_thread(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  AVFormatContext* ic = NULL;
  int err, i, ret;
  int st_index[AVMEDIA_TYPE_NB];
  AVPacket pkt1, *pkt = &pkt1;
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  AVDictionaryEntry* t;
  AVDictionary** opts;
  int orig_nb_streams;
  SDL_mutex* wait_mutex = SDL_CreateMutex();
  int scan_all_pmts_set = 0;
  int64_t pkt_ts;

  const char* in_filename = common::utils::c_strornull(is->opt->input_filename);
  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(st_index, -1, sizeof(st_index));
  is->last_video_stream = is->video_stream = -1;
  is->last_audio_stream = is->audio_stream = -1;
  is->last_subtitle_stream = is->subtitle_stream = -1;
  is->eof_ = false;

  ic = avformat_alloc_context();
  if (!ic) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  ic->interrupt_callback.callback = decode_interrupt_cb;
  ic->interrupt_callback.opaque = is;
  if (!av_dict_get(is->copt->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
    av_dict_set(&is->copt->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    scan_all_pmts_set = 1;
  }
  err = avformat_open_input(&ic, in_filename, is->iformat, &is->copt->format_opts);
  if (err < 0) {
    print_error(in_filename, err);
    ret = -1;
    goto fail;
  }
  if (scan_all_pmts_set) {
    av_dict_set(&is->copt->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
  }

  if ((t = av_dict_get(is->copt->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }
  is->ic = ic;

  if (is->opt->genpts) {
    ic->flags |= AVFMT_FLAG_GENPTS;
  }

  av_format_inject_global_side_data(ic);

  opts = setup_find_stream_info_opts(ic, is->copt->codec_opts);
  orig_nb_streams = ic->nb_streams;

  err = avformat_find_stream_info(ic, opts);

  for (i = 0; i < orig_nb_streams; i++) {
    av_dict_free(&opts[i]);
  }
  av_freep(&opts);

  if (err < 0) {
    av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", in_filename);
    ret = -1;
    goto fail;
  }

  if (ic->pb)
    ic->pb->eof_reached =
        0;  // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

  if (is->opt->seek_by_bytes < 0) {
    is->opt->seek_by_bytes =
        !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
  }

  is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  if (is->opt->window_title.empty() && (t = av_dict_get(ic->metadata, "title", NULL, 0))) {
    is->opt->window_title = av_asprintf("%s - %s", t->value, in_filename);
  }

  /* if seeking requested, we execute it */
  if (is->opt->start_time != AV_NOPTS_VALUE) {
    int64_t timestamp;

    timestamp = is->opt->start_time;
    /* add the stream start time */
    if (ic->start_time != AV_NOPTS_VALUE)
      timestamp += ic->start_time;
    ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", in_filename,
             (double)timestamp / AV_TIME_BASE);
    }
  }

  is->realtime = is_realtime(ic);

  if (is->opt->show_status) {
    av_dump_format(ic, 0, in_filename, 0);
  }

  for (i = 0; i < ic->nb_streams; i++) {
    AVStream* st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    const char* want_spec = common::utils::c_strornull(is->opt->wanted_stream_spec[type]);
    if (type >= 0 && want_spec && st_index[type] == -1) {
      if (avformat_match_stream_specifier(ic, st, want_spec) > 0) {
        st_index[type] = i;
      }
    }
  }
  for (i = 0; i < static_cast<int>(AVMEDIA_TYPE_NB); i++) {
    const char* want_spec = common::utils::c_strornull(is->opt->wanted_stream_spec[i]);
    if (want_spec && st_index[i] == -1) {
      av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", want_spec,
             av_get_media_type_string(static_cast<AVMediaType>(i)));
      st_index[i] = INT_MAX;
    }
  }

  if (!is->opt->video_disable) {
    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  }
  if (!is->opt->audio_disable) {
    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  }
  if (!is->opt->video_disable && !is->opt->subtitle_disable) {
    st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
                            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO]
                                                               : st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);
  }

  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters* codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
    if (codecpar->width) {
      is->set_default_window_size(codecpar->width, codecpar->height, sar);
    }
  }

  /* open the streams */
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    is->stream_component_open(st_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = is->stream_component_open(st_index[AVMEDIA_TYPE_VIDEO]);
  }
  if (is->opt->show_mode == SHOW_MODE_NONE) {
    is->opt->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
  }

  if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
    is->stream_component_open(st_index[AVMEDIA_TYPE_SUBTITLE]);
  }

  if (is->video_stream < 0 && is->audio_stream < 0) {
    av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", in_filename);
    ret = -1;
    goto fail;
  }

  if (is->opt->infinite_buffer < 0 && is->realtime) {
    is->opt->infinite_buffer = 1;
  }

  while (true) {
    if (is->abort_request)
      break;
    if (is->paused_ != is->last_paused_) {
      is->last_paused_ = is->paused_;
      if (is->paused_) {
        is->read_pause_return = av_read_pause(ic);
      } else {
        av_read_play(ic);
      }
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (is->paused_ &&
        (!strcmp(ic->iformat->name, "rtsp") || (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
      /* wait 10 ms to avoid trying to get another packet */
      /* XXX: horrible */
      SDL_Delay(10);
      continue;
    }
#endif
    if (is->seek_req) {
      int64_t seek_target = is->seek_pos;
      int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
      int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables

      ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->filename);
      } else {
        if (is->audio_stream >= 0) {
          is->audio_engine_->packet_queue_->flush();
          is->audio_engine_->packet_queue_->put(PacketQueue::flush_pkt());
        }
        if (is->subtitle_stream >= 0) {
          is->subtitle_engine_->packet_queue_->flush();
          is->subtitle_engine_->packet_queue_->put(PacketQueue::flush_pkt());
        }
        if (is->video_stream >= 0) {
          is->video_engine_->packet_queue_->flush();
          is->video_engine_->packet_queue_->put(PacketQueue::flush_pkt());
        }
        if (is->seek_flags & AVSEEK_FLAG_BYTE) {
          is->subtitle_engine_->SetClock(NAN, 0);
        } else {
          is->subtitle_engine_->SetClock(seek_target / (double)AV_TIME_BASE, 0);
        }
      }
      is->seek_req = 0;
      is->queue_attachments_req = 1;
      is->eof_ = false;
      if (is->paused_) {
        is->step_to_next_frame();
      }
    }
    if (is->queue_attachments_req) {
      if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        AVPacket copy;
        if ((ret = av_copy_packet(&copy, &is->video_st->attached_pic)) < 0) {
          goto fail;
        }
        is->video_engine_->packet_queue_->put(&copy);
        is->video_engine_->packet_queue_->put_nullpacket(is->video_stream);
      }
      is->queue_attachments_req = 0;
    }

    /* if the queue are full, no need to read more */
    if (is->opt->infinite_buffer < 1 &&
        (is->audio_engine_->packet_queue_->size() + is->video_engine_->packet_queue_->size() +
                 is->subtitle_engine_->packet_queue_->size() >
             MAX_QUEUE_SIZE ||
         (stream_has_enough_packets(is->audio_st, is->audio_stream,
                                    is->audio_engine_->packet_queue_) &&
          stream_has_enough_packets(is->video_st, is->video_stream,
                                    is->video_engine_->packet_queue_) &&
          stream_has_enough_packets(is->subtitle_st, is->subtitle_stream,
                                    is->subtitle_engine_->packet_queue_)))) {
      /* wait 10 ms */
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    }
    if (!is->paused_ &&
        (!is->audio_st || (is->auddec->finished == is->audio_engine_->packet_queue_->serial &&
                           is->audio_engine_->frame_queue_->nb_remaining() == 0)) &&
        (!is->video_st || (is->viddec->finished == is->video_engine_->packet_queue_->serial &&
                           is->video_engine_->frame_queue_->nb_remaining() == 0))) {
      if (is->opt->loop != 1 && (!is->opt->loop || --is->opt->loop)) {
        is->stream_seek(is->opt->start_time != AV_NOPTS_VALUE ? is->opt->start_time : 0, 0, 0);
      } else if (is->opt->autoexit) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }
    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof_) {
        if (is->video_stream >= 0) {
          is->video_engine_->packet_queue_->put_nullpacket(is->video_stream);
        }
        if (is->audio_stream >= 0) {
          is->audio_engine_->packet_queue_->put_nullpacket(is->audio_stream);
        }
        if (is->subtitle_stream >= 0) {
          is->subtitle_engine_->packet_queue_->put_nullpacket(is->subtitle_stream);
        }
        is->eof_ = true;
      }
      if (ic->pb && ic->pb->error)
        break;
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    } else {
      is->eof_ = false;
    }
    /* check if packet is in play range specified by user, then queue, otherwise discard */
    stream_start_time = ic->streams[pkt->stream_index]->start_time;
    pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    pkt_in_play_range =
        is->opt->duration == AV_NOPTS_VALUE ||
        (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                    av_q2d(ic->streams[pkt->stream_index]->time_base) -
                static_cast<double>(is->opt->start_time != AV_NOPTS_VALUE ? is->opt->start_time
                                                                          : 0) /
                    1000000 <=
            (static_cast<double>(is->opt->duration) / 1000000);
    if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
      is->audio_engine_->packet_queue_->put(pkt);
    } else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
               !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      is->video_engine_->packet_queue_->put(pkt);
    } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
      is->subtitle_engine_->packet_queue_->put(pkt);
    } else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  if (ic && !is->ic)
    avformat_close_input(&ic);

  if (ret != 0) {
    SDL_Event event;

    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }
  SDL_DestroyMutex(wait_mutex);
  return 0;
}

int VideoState::audio_thread(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  Frame* af;
#if CONFIG_AVFILTER
  int last_serial = -1;
  int64_t dec_channel_layout;
  int reconfigure;
#endif
  int got_frame = 0;
  int ret = 0;

  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return AVERROR(ENOMEM);
  }

  do {
    if ((got_frame = is->auddec->decodeFrame(frame, NULL)) < 0) {
      goto the_end;
    }

    if (got_frame) {
      AVRational tb = {1, frame->sample_rate};

#if CONFIG_AVFILTER
      dec_channel_layout =
          get_valid_channel_layout(frame->channel_layout, av_frame_get_channels(frame));

      reconfigure = cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   static_cast<AVSampleFormat>(frame->format),
                                   av_frame_get_channels(frame)) ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq != frame->sample_rate ||
                    is->auddec->pktSerial() != last_serial;

      if (reconfigure) {
        char buf1[1024], buf2[1024];
        av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
        av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
        av_log(NULL, AV_LOG_DEBUG,
               "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d "
               "fmt:%s layout:%s serial:%d\n",
               is->audio_filter_src.freq, is->audio_filter_src.channels,
               av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
               frame->sample_rate, av_frame_get_channels(frame),
               av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)), buf2,
               is->auddec->pktSerial());

        is->audio_filter_src.fmt = static_cast<AVSampleFormat>(frame->format);
        is->audio_filter_src.channels = av_frame_get_channels(frame);
        is->audio_filter_src.channel_layout = dec_channel_layout;
        is->audio_filter_src.freq = frame->sample_rate;
        last_serial = is->auddec->pktSerial();

        if ((ret = is->configure_audio_filters(is->opt->afilters, 1)) < 0) {
          goto the_end;
        }
      }

      if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0) {
        goto the_end;
      }

      while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
        tb = is->out_audio_filter->inputs[0]->time_base;
#endif
        if (!(af = is->audio_engine_->frame_queue_->peek_writable())) {
          goto the_end;
        }

        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->pos = av_frame_get_pkt_pos(frame);
        af->serial = is->auddec->pktSerial();
        AVRational tmp = {frame->nb_samples, frame->sample_rate};
        af->duration = av_q2d(tmp);

        av_frame_move_ref(af->frame, frame);
        is->audio_engine_->frame_queue_->push();

#if CONFIG_AVFILTER
        if (is->audio_engine_->packet_queue_->serial != is->auddec->pktSerial()) {
          break;
        }
      }
      if (ret == AVERROR_EOF) {
        is->auddec->finished = is->auddec->pktSerial();
      }
#endif
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
#if CONFIG_AVFILTER
  avfilter_graph_free(&is->agraph);
#endif
  av_frame_free(&frame);
  return ret;
}

int VideoState::video_thread(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  AVFrame* frame = av_frame_alloc();
  double pts;
  double duration;
  int ret;
  AVRational tb = is->video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
  AVFilterGraph* graph = avfilter_graph_alloc();
  AVFilterContext *filt_out = NULL, *filt_in = NULL;
  int last_w = 0;
  int last_h = 0;
  enum AVPixelFormat last_format = AV_PIX_FMT_NONE;  //-2
  int last_serial = -1;
  int last_vfilter_idx = 0;
  if (!graph) {
    av_frame_free(&frame);
    return AVERROR(ENOMEM);
  }

#endif

  if (!frame) {
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    return AVERROR(ENOMEM);
  }

  while (true) {
    ret = is->get_video_frame(frame);
    if (ret < 0) {
      goto the_end;
    }
    if (!ret) {
      continue;
    }

#if CONFIG_AVFILTER
    if (last_w != frame->width || last_h != frame->height || last_format != frame->format ||
        last_serial != is->viddec->pktSerial() || last_vfilter_idx != is->vfilter_idx) {
      av_log(NULL, AV_LOG_DEBUG,
             "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s "
             "serial:%d\n",
             last_w, last_h,
             static_cast<const char*>(av_x_if_null(av_get_pix_fmt_name(last_format), "none")),
             last_serial, frame->width, frame->height,
             static_cast<const char*>(
                 av_x_if_null(av_get_pix_fmt_name((AVPixelFormat)frame->format), "none")),
             is->viddec->pktSerial());
      avfilter_graph_free(&graph);
      graph = avfilter_graph_alloc();
      const char* vfilters = NULL;
      if (!is->opt->vfilters_list.empty()) {
        vfilters = is->opt->vfilters_list[is->vfilter_idx].c_str();
      }
      if ((ret = is->configure_video_filters(graph, vfilters, frame)) < 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
        goto the_end;
      }
      filt_in = is->in_video_filter;
      filt_out = is->out_video_filter;
      last_w = frame->width;
      last_h = frame->height;
      last_format = static_cast<AVPixelFormat>(frame->format);
      last_serial = is->viddec->pktSerial();
      last_vfilter_idx = is->vfilter_idx;
      frame_rate = filt_out->inputs[0]->frame_rate;
    }

    ret = av_buffersrc_add_frame(filt_in, frame);
    if (ret < 0)
      goto the_end;

    while (ret >= 0) {
      is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

      ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
      if (ret < 0) {
        if (ret == AVERROR_EOF) {
          is->viddec->finished = is->viddec->pktSerial();
        }
        ret = 0;
        break;
      }

      is->frame_last_filter_delay =
          av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
      if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
        is->frame_last_filter_delay = 0;
      tb = filt_out->inputs[0]->time_base;
#endif
      duration =
          (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num})
                                            : 0);
      pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      ret = is->queue_picture(frame, pts, duration, av_frame_get_pkt_pos(frame),
                              is->viddec->pktSerial());
      av_frame_unref(frame);
#if CONFIG_AVFILTER
    }
#endif

    if (ret < 0)
      goto the_end;
  }
the_end:
#if CONFIG_AVFILTER
  avfilter_graph_free(&graph);
#endif
  av_frame_free(&frame);
  return 0;
}

int VideoState::subtitle_thread(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  FrameQueue* subpq = is->subtitle_engine_->frame_queue_;
  SubDecoder* subdec = is->subdec;
  while (true) {
    Frame* sp = subpq->peek_writable();
    if (!sp) {
      return 0;
    }

    int got_subtitle = subdec->decodeFrame(NULL, &sp->sub);
    if (got_subtitle < 0) {
      break;
    }

    double pts = 0;
    if (got_subtitle && sp->sub.format == 0) {
      if (sp->sub.pts != AV_NOPTS_VALUE) {
        pts = static_cast<double>(sp->sub.pts) / static_cast<double>(AV_TIME_BASE);
      }
      sp->pts = pts;
      sp->serial = subdec->pktSerial();
      sp->width = subdec->width();
      sp->height = subdec->height();
      sp->uploaded = 0;

      /* now we can update the picture count */
      subpq->push();
    } else if (got_subtitle) {
      avsubtitle_free(&sp->sub);
    }
  }
  return 0;
}

int VideoState::decode_interrupt_cb(void* user_data) {
  VideoState* is = static_cast<VideoState*>(user_data);
  return is->abort_request;
}

#if CONFIG_AVFILTER
int VideoState::configure_video_filters(AVFilterGraph* graph,
                                        const char* vfilters,
                                        AVFrame* frame) {
  AVDictionary* sws_dict = copt->sws_dict;
  static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA,
                                                AV_PIX_FMT_NONE};
  char sws_flags_str[512] = "";
  char buffersrc_args[256];
  int ret;
  AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
  AVCodecParameters* codecpar = video_st->codecpar;
  AVRational fr = av_guess_frame_rate(ic, video_st, NULL);
  AVDictionaryEntry* e = NULL;

  while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
    if (!strcmp(e->key, "sws_flags")) {
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
    } else
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
  }
  if (strlen(sws_flags_str))
    sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

  graph->scale_sws_opts = av_strdup(sws_flags_str);

  snprintf(buffersrc_args, sizeof(buffersrc_args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", frame->width,
           frame->height, frame->format, video_st->time_base.num, video_st->time_base.den,
           codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
  if (fr.num && fr.den)
    av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

  if ((ret = avfilter_graph_create_filter(&filt_src, avfilter_get_by_name("buffer"),
                                          "ffplay_buffer", buffersrc_args, NULL, graph)) < 0) {
    goto fail;
  }

  ret = avfilter_graph_create_filter(&filt_out, avfilter_get_by_name("buffersink"),
                                     "ffplay_buffersink", NULL, NULL, graph);
  if (ret < 0) {
    goto fail;
  }

  if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                                 AV_OPT_SEARCH_CHILDREN)) < 0) {
    goto fail;
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
      goto fail;                                                                                   \
                                                                                                   \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                                              \
    if (ret < 0)                                                                                   \
      goto fail;                                                                                   \
                                                                                                   \
    last_filter = filt_ctx;                                                                        \
  } while (0)

  if (opt->autorotate) {
    double theta = get_rotation(video_st);

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

  if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

  in_video_filter = filt_src;
  out_video_filter = filt_out;

fail:
  return ret;
}

int VideoState::configure_audio_filters(const char* afilters, int force_output_format) {
  AVDictionary* swr_opts = copt->swr_opts;
  static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
  int sample_rates[2] = {0, -1};
  int64_t channel_layouts[2] = {0, -1};
  int channels[2] = {0, -1};
  AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
  char aresample_swr_opts[512] = "";
  AVDictionaryEntry* e = NULL;
  char asrc_args[256];

  avfilter_graph_free(&agraph);
  if (!(agraph = avfilter_graph_alloc())) {
    return AVERROR(ENOMEM);
  }

  while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
    av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
  }
  if (strlen(aresample_swr_opts)) {
    aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
  }
  av_opt_set(agraph, "aresample_swr_opts", aresample_swr_opts, 0);

  int ret = snprintf(asrc_args, sizeof(asrc_args),
                     "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                     audio_filter_src.freq, av_get_sample_fmt_name(audio_filter_src.fmt),
                     audio_filter_src.channels, 1, audio_filter_src.freq);
  if (audio_filter_src.channel_layout) {
    snprintf(asrc_args + ret, sizeof(asrc_args) - ret, ":channel_layout=0x%" PRIx64,
             audio_filter_src.channel_layout);
  }

  ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                     asrc_args, NULL, agraph);
  if (ret < 0)
    goto end;

  ret = avfilter_graph_create_filter(&filt_asink, avfilter_get_by_name("abuffersink"),
                                     "ffplay_abuffersink", NULL, NULL, agraph);
  if (ret < 0)
    goto end;

  if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE,
                                 AV_OPT_SEARCH_CHILDREN)) < 0) {
    goto end;
  }
  if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0) {
    goto end;
  }

  if (force_output_format) {
    channel_layouts[0] = audio_tgt.channel_layout;
    channels[0] = audio_tgt.channels;
    sample_rates[0] = audio_tgt.freq;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0) {
      goto end;
    }
    if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1,
                                   AV_OPT_SEARCH_CHILDREN)) < 0) {
      goto end;
    }
    if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1,
                                   AV_OPT_SEARCH_CHILDREN)) < 0) {
      goto end;
    }
    if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1,
                                   AV_OPT_SEARCH_CHILDREN)) < 0) {
      goto end;
    }
  }

  if ((ret = configure_filtergraph(agraph, afilters, filt_asrc, filt_asink)) < 0)
    goto end;

  in_audio_filter = filt_asrc;
  out_audio_filter = filt_asink;

end:
  if (ret < 0)
    avfilter_graph_free(&agraph);
  return ret;
}
#endif /* CONFIG_AVFILTER */
