#include "sdl_renderer.h"

#include <cmath>
#include <csignal>

// WebRTC
#include <api/video/i420_buffer.h>
#include <rtc_base/logging.h>
#include <third_party/libyuv/include/libyuv/convert_from.h>
#include <third_party/libyuv/include/libyuv/video_common.h>

#define STD_ASPECT 1.34
#define WIDE_ASPECT 1.78
#define FRAME_INTERVAL (1000 / 30)

SDLRenderer::SDLRenderer(int width, int height, bool fullscreen)
    : running_(true),
      window_(nullptr),
      renderer_(nullptr),
      dispatch_(nullptr),
      width_(width),
      height_(height),
      rows_(1),
      cols_(1),
      show_title_bar_(false),
      mouse_in_title_area_(false),
      title_bar_texture_(nullptr),
      minimize_button_(nullptr),
      restore_button_(nullptr),
      close_button_(nullptr),
      last_mouse_move_time_(0) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": SDL_Init failed " << SDL_GetError();
    return;
  }

  window_ = SDL_CreateWindow(
      "Momo WebRTC Native Client", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, width_, height_,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);

  if (window_ == nullptr) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": SDL_CreateWindow failed "
                      << SDL_GetError();
    return;
  }

  // Start in fullscreen by default
  SetFullScreen(true);

  CreateTitleBarTextures();

#if defined(__APPLE__)
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
  if (renderer_ == nullptr) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": SDL_CreateRenderer failed "
                      << SDL_GetError();
    return;
  }
#endif

  thread_ = SDL_CreateThread(SDLRenderer::RenderThreadExec, "Render", this);
}

SDLRenderer::~SDLRenderer() {
  running_ = false;
  int ret = 0;
  SDL_WaitThread(thread_, &ret);
  if (ret != 0) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": SDL Thread error:" << ret;
  }

  // Clean up textures
  if (title_bar_texture_) {
    SDL_DestroyTexture(title_bar_texture_);
  }
  if (minimize_button_) {
    SDL_DestroyTexture(minimize_button_);
  }
  if (restore_button_) {
    SDL_DestroyTexture(restore_button_);
  }
  if (close_button_) {
    SDL_DestroyTexture(close_button_);
  }

  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

bool SDLRenderer::IsFullScreen() {
  return SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
}

void SDLRenderer::SetFullScreen(bool fullscreen) {
  SDL_SetWindowFullscreen(window_,
                          fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

void SDLRenderer::PollEvent() {
  SDL_Event e;
  while (SDL_PollEvent(&e) > 0) {
    if (e.type == SDL_MOUSEMOTION) {
      UpdateTitleBar();
    }

    if (e.type == SDL_MOUSEBUTTONDOWN && show_title_bar_) {
      int x = e.button.x;
      int y = e.button.y;

      if (y < TITLE_BAR_HEIGHT) {
        int window_width = width_;
        if (x > window_width - BUTTON_WIDTH) {
          // Close button
          std::raise(SIGTERM);
        } else if (x > window_width - 2 * BUTTON_WIDTH) {
          // Restore button
          SetFullScreen(false);
        } else if (x > window_width - 3 * BUTTON_WIDTH) {
          // Minimize button
          SDL_MinimizeWindow(window_);
        }
      }
    }

    if (e.type == SDL_WINDOWEVENT &&
        e.window.event == SDL_WINDOWEVENT_RESIZED &&
        e.window.windowID == SDL_GetWindowID(window_)) {
      webrtc::MutexLock lock(&sinks_lock_);
      width_ = e.window.data1;
      height_ = e.window.data2;
      SetOutlines();
    }

    if (e.type == SDL_KEYUP) {
      switch (e.key.keysym.sym) {
        case SDLK_f:
          SetFullScreen(!IsFullScreen());
          break;
        case SDLK_q:
          std::raise(SIGTERM);
          break;
      }
    }

    if (e.type == SDL_QUIT) {
      std::raise(SIGTERM);
    }
  }
}

void SDLRenderer::SetDispatchFunction(
    std::function<void(std::function<void()>)> dispatch) {
  webrtc::MutexLock lock(&sinks_lock_);
  dispatch_ = std::move(dispatch);
}

int SDLRenderer::RenderThreadExec(void* data) {
  return ((SDLRenderer*)data)->RenderThread();
}

int SDLRenderer::RenderThread() {
#if !defined(__APPLE__)
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
  if (renderer_ == nullptr) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << ": SDL_CreateRenderer failed "
                      << SDL_GetError();
    return 1;
  }
#endif

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);

  uint32_t start_time, duration;
  while (running_) {
    start_time = SDL_GetTicks();
    {
      webrtc::MutexLock lock(&sinks_lock_);
      SDL_RenderClear(renderer_);
      for (const VideoTrackSinkVector::value_type& sinks : sinks_) {
        Sink* sink = sinks.second.get();

        webrtc::MutexLock frame_lock(sink->GetMutex());

        if (!sink->GetOutlineChanged())
          continue;

        int width = sink->GetFrameWidth();
        int height = sink->GetFrameHeight();

        if (width == 0 || height == 0)
          continue;

        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
            sink->GetImage(), width, height, 32, width * 4, 0, 0, 0, 0);
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_FreeSurface(surface);

        SDL_Rect image_rect = {0, 0, width, height};
        SDL_Rect draw_rect = {sink->GetOffsetX(), sink->GetOffsetY(),
                              sink->GetWidth(), sink->GetHeight()};

        SDL_RenderCopy(renderer_, texture, &image_rect, &draw_rect);
        SDL_DestroyTexture(texture);
      }
      SDL_RenderPresent(renderer_);

      // Check if we need to show/hide title bar
      int x, y;
      SDL_GetMouseState(&x, &y);
      bool in_title_area = y < TITLE_BAR_HEIGHT;

      if (in_title_area) {
        last_mouse_move_time_ = SDL_GetTicks();
        if (!show_title_bar_) {
          show_title_bar_ = true;
        }
      } else if (show_title_bar_ &&
                 (SDL_GetTicks() - last_mouse_move_time_ > TITLE_SHOW_DELAY)) {
        show_title_bar_ = false;
      }

      if (show_title_bar_) {
        UpdateTitleBar();
      }

      if (dispatch_) {
        dispatch_(std::bind(&SDLRenderer::PollEvent, this));
      }
    }
    duration = SDL_GetTicks() - start_time;
    SDL_Delay(FRAME_INTERVAL - (duration % FRAME_INTERVAL));
  }

  SDL_DestroyRenderer(renderer_);
  renderer_ = nullptr;

  return 0;
}

SDLRenderer::Sink::Sink(SDLRenderer* renderer,
                        webrtc::VideoTrackInterface* track)
    : renderer_(renderer),
      track_(track),
      outline_offset_x_(0),
      outline_offset_y_(0),
      outline_width_(0),
      outline_height_(0),
      outline_changed_(false),
      input_width_(0),
      input_height_(0),
      scaled_(false),
      width_(0),
      height_(0) {
  track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
}

SDLRenderer::Sink::~Sink() {
  track_->RemoveSink(this);
}

void SDLRenderer::Sink::OnFrame(const webrtc::VideoFrame& frame) {
  if (outline_width_ == 0 || outline_height_ == 0)
    return;
  if (frame.width() == 0 || frame.height() == 0)
    return;
  webrtc::MutexLock lock(GetMutex());
  if (outline_changed_ || frame.width() != input_width_ ||
      frame.height() != input_height_) {
    int width, height;
    float frame_aspect = (float)frame.width() / (float)frame.height();
    if (frame_aspect > outline_aspect_) {
      width = outline_width_;
      height = width / frame_aspect;
      offset_x_ = 0;
      offset_y_ = (outline_height_ - height) / 2;
    } else {
      height = outline_height_;
      width = height * frame_aspect;
      offset_x_ = (outline_width_ - width) / 2;
      offset_y_ = 0;
    }
    if (width_ != width || height_ != height) {
      width_ = width;
      height_ = height;
    }
    input_width_ = frame.width();
    input_height_ = frame.height();
    scaled_ = width_ < input_width_;
    if (scaled_) {
      image_.reset(new uint8_t[width_ * height_ * 4]);
    } else {
      image_.reset(new uint8_t[input_width_ * input_height_ * 4]);
    }
    RTC_LOG(LS_VERBOSE) << __FUNCTION__ << ": scaled_=" << scaled_;
    outline_changed_ = false;
  }
  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer_if;
  if (scaled_) {
    rtc::scoped_refptr<webrtc::I420Buffer> buffer =
        webrtc::I420Buffer::Create(width_, height_);
    buffer->ScaleFrom(*frame.video_frame_buffer()->ToI420());
    if (frame.rotation() != webrtc::kVideoRotation_0) {
      buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    }
    buffer_if = buffer;
  } else {
    buffer_if = frame.video_frame_buffer()->ToI420();
  }
  libyuv::ConvertFromI420(
      buffer_if->DataY(), buffer_if->StrideY(), buffer_if->DataU(),
      buffer_if->StrideU(), buffer_if->DataV(), buffer_if->StrideV(),
      image_.get(), (scaled_ ? width_ : input_width_) * 4, buffer_if->width(),
      buffer_if->height(), libyuv::FOURCC_ARGB);
}

void SDLRenderer::Sink::SetOutlineRect(int x, int y, int width, int height) {
  outline_offset_x_ = x;
  outline_offset_y_ = y;
  if (outline_width_ == width && outline_height_ == height) {
    return;
  }
  webrtc::MutexLock lock(GetMutex());
  offset_y_ = 0;
  offset_x_ = 0;
  outline_width_ = width;
  outline_height_ = height;
  outline_aspect_ = (float)outline_width_ / (float)outline_height_;
  outline_changed_ = true;
}

webrtc::Mutex* SDLRenderer::Sink::GetMutex() {
  return &frame_params_lock_;
}

bool SDLRenderer::Sink::GetOutlineChanged() {
  return !outline_changed_;
}

int SDLRenderer::Sink::GetOffsetX() {
  return outline_offset_x_ + offset_x_;
}

int SDLRenderer::Sink::GetOffsetY() {
  return outline_offset_y_ + offset_y_;
}

int SDLRenderer::Sink::GetFrameWidth() {
  return scaled_ ? width_ : input_width_;
}

int SDLRenderer::Sink::GetFrameHeight() {
  return scaled_ ? height_ : input_height_;
}

int SDLRenderer::Sink::GetWidth() {
  return width_;
}

int SDLRenderer::Sink::GetHeight() {
  return height_;
}

uint8_t* SDLRenderer::Sink::GetImage() {
  return image_.get();
}

void SDLRenderer::SetOutlines() {
  float window_aspect = (float)width_ / (float)height_;
  bool window_is_wide = window_aspect > ((STD_ASPECT + WIDE_ASPECT) / 2.0);
  float frame_aspect = window_is_wide ? WIDE_ASPECT : STD_ASPECT;
  int rows = 1;
  int cols = 1;
  if (window_aspect >= frame_aspect) {
    int times = std::floor(window_aspect / frame_aspect);
    if (times < 1)
      times = 1;
    while (rows * cols < sinks_.size()) {
      if (times < (cols / rows)) {
        rows++;
      } else {
        cols++;
      }
    }
  } else {
    int times = std::floor(frame_aspect / window_aspect);
    if (times < 1)
      times = 1;
    while (rows * cols < sinks_.size()) {
      if (times < (rows / cols)) {
        cols++;
      } else {
        rows++;
      }
    }
  }
  RTC_LOG(LS_VERBOSE) << __FUNCTION__ << " rows:" << rows << " cols:" << cols;
  int outline_width = std::floor(width_ / cols);
  int outline_height = std::floor(height_ / rows);
  int sinks_count = sinks_.size();
  for (int i = 0; i < sinks_count; i++) {
    Sink* sink = sinks_[i].second.get();
    int offset_x = outline_width * (i % cols);
    int offset_y = outline_height * std::floor(i / cols);
    sink->SetOutlineRect(offset_x, offset_y, outline_width, outline_height);
    RTC_LOG(LS_VERBOSE) << __FUNCTION__ << " offset_x:" << offset_x
                        << " offset_y:" << offset_y
                        << " outline_width:" << outline_width
                        << " outline_height:" << outline_height;
  }
  rows_ = rows;
  cols_ = cols;
}

void SDLRenderer::AddTrack(webrtc::VideoTrackInterface* track) {
  std::unique_ptr<Sink> sink(new Sink(this, track));
  webrtc::MutexLock lock(&sinks_lock_);
  sinks_.push_back(std::make_pair(track, std::move(sink)));
  SetOutlines();
}

void SDLRenderer::RemoveTrack(webrtc::VideoTrackInterface* track) {
  webrtc::MutexLock lock(&sinks_lock_);
  sinks_.erase(
      std::remove_if(sinks_.begin(), sinks_.end(),
                     [track](const VideoTrackSinkVector::value_type& sink) {
                       return sink.first == track;
                     }),
      sinks_.end());
  SetOutlines();
}

void SDLRenderer::CreateTitleBarTextures() {
  if (!renderer_)
    return;

  // Create semi-transparent black title bar
  SDL_Surface* title_surface = SDL_CreateRGBSurface(
      0, width_, SDLRenderer::TITLE_BAR_HEIGHT, 32, 0, 0, 0, 0);
  SDL_FillRect(title_surface, NULL,
               SDL_MapRGBA(title_surface->format, 0, 0, 0, 180));
  title_bar_texture_ = SDL_CreateTextureFromSurface(renderer_, title_surface);
  SDL_FreeSurface(title_surface);

  // Create button surfaces
  SDL_Surface* button_surface =
      SDL_CreateRGBSurface(0, SDLRenderer::BUTTON_WIDTH,
                           SDLRenderer::TITLE_BAR_HEIGHT, 32, 0, 0, 0, 0);

  // Minimize button
  SDL_FillRect(
      button_surface, NULL,
      SDL_MapRGBA(button_surface->format, SDLRenderer::BUTTON_NORMAL_COLOR.r,
                  SDLRenderer::BUTTON_NORMAL_COLOR.g,
                  SDLRenderer::BUTTON_NORMAL_COLOR.b,
                  SDLRenderer::BUTTON_NORMAL_COLOR.a));

  // Draw minimize icon (horizontal line)
  SDL_Rect minimize_icon = {SDLRenderer::BUTTON_WIDTH / 4,
                            SDLRenderer::TITLE_BAR_HEIGHT / 2,
                            SDLRenderer::BUTTON_WIDTH / 2, 2};
  SDL_FillRect(button_surface, &minimize_icon,
               SDL_MapRGBA(button_surface->format, 255, 255, 255, 255));
  minimize_button_ = SDL_CreateTextureFromSurface(renderer_, button_surface);

  // Restore button
  SDL_FillRect(
      button_surface, NULL,
      SDL_MapRGBA(button_surface->format, SDLRenderer::BUTTON_NORMAL_COLOR.r,
                  SDLRenderer::BUTTON_NORMAL_COLOR.g,
                  SDLRenderer::BUTTON_NORMAL_COLOR.b,
                  SDLRenderer::BUTTON_NORMAL_COLOR.a));

  // Draw restore icon (square)
  SDL_Rect restore_icon = {
      SDLRenderer::BUTTON_WIDTH / 4, SDLRenderer::TITLE_BAR_HEIGHT / 4,
      SDLRenderer::BUTTON_WIDTH / 2, SDLRenderer::TITLE_BAR_HEIGHT / 2};
  SDL_FillRect(button_surface, &restore_icon,
               SDL_MapRGBA(button_surface->format, 255, 255, 255, 255));
  restore_button_ = SDL_CreateTextureFromSurface(renderer_, button_surface);

  // Close button
  SDL_FillRect(
      button_surface, NULL,
      SDL_MapRGBA(button_surface->format, SDLRenderer::BUTTON_CLOSE_COLOR.r,
                  SDLRenderer::BUTTON_CLOSE_COLOR.g,
                  SDLRenderer::BUTTON_CLOSE_COLOR.b,
                  SDLRenderer::BUTTON_CLOSE_COLOR.a));

  DrawCloseIcon(button_surface, SDLRenderer::BUTTON_WIDTH);
  close_button_ = SDL_CreateTextureFromSurface(renderer_, button_surface);

  SDL_FreeSurface(button_surface);
}

void SDLRenderer::DrawCloseIcon(SDL_Surface* surface, int button_width) {
  // Draw X using SDL_gfx or manual pixel plotting
  const int margin = button_width / 4;
  const int size = button_width / 2;

  for (int i = 0; i < size; i++) {
    // Draw main diagonal
    SDL_Rect pixel1 = {margin + i, margin + i, 2, 2};
    // Draw counter diagonal
    SDL_Rect pixel2 = {margin + i, margin + size - i, 2, 2};

    SDL_FillRect(surface, &pixel1,
                 SDL_MapRGBA(surface->format, 255, 255, 255, 255));
    SDL_FillRect(surface, &pixel2,
                 SDL_MapRGBA(surface->format, 255, 255, 255, 255));
  }
}

void SDLRenderer::UpdateTitleBar() {
  if (!IsFullScreen())
    return;

  int x, y;
  SDL_GetMouseState(&x, &y);

  bool in_title_area = y < TITLE_BAR_HEIGHT;

  if (in_title_area) {
    last_mouse_move_time_ = SDL_GetTicks();
    if (!show_title_bar_) {
      show_title_bar_ = true;
    }
  } else if (show_title_bar_ &&
             (SDL_GetTicks() - last_mouse_move_time_ > TITLE_SHOW_DELAY)) {
    show_title_bar_ = false;
  }

  // Render title bar if needed
  if (show_title_bar_ && IsFullScreen()) {
    SDL_Rect title_rect = {0, 0, width_, TITLE_BAR_HEIGHT};
    SDL_RenderCopy(renderer_, title_bar_texture_, NULL, &title_rect);

    // Get mouse position for hover effects
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    // Render buttons
    int x = width_ - BUTTON_WIDTH;
    SDL_Rect button_rect = {x, 0, BUTTON_WIDTH, TITLE_BAR_HEIGHT};

    // Close button
    if (mouse_y < TITLE_BAR_HEIGHT && mouse_x > x) {
      SDL_SetTextureColorMod(close_button_, 255, 255, 255);
    } else {
      SDL_SetTextureColorMod(close_button_, 220, 220, 220);
    }
    SDL_RenderCopy(renderer_, close_button_, NULL, &button_rect);

    // Restore button
    button_rect.x -= BUTTON_WIDTH;
    if (mouse_y < TITLE_BAR_HEIGHT && mouse_x > button_rect.x &&
        mouse_x < button_rect.x + BUTTON_WIDTH) {
      SDL_SetTextureColorMod(restore_button_, 255, 255, 255);
    } else {
      SDL_SetTextureColorMod(restore_button_, 220, 220, 220);
    }
    SDL_RenderCopy(renderer_, restore_button_, NULL, &button_rect);

    // Minimize button
    button_rect.x -= BUTTON_WIDTH;
    if (mouse_y < TITLE_BAR_HEIGHT && mouse_x > button_rect.x &&
        mouse_x < button_rect.x + BUTTON_WIDTH) {
      SDL_SetTextureColorMod(minimize_button_, 255, 255, 255);
    } else {
      SDL_SetTextureColorMod(minimize_button_, 220, 220, 220);
    }
    SDL_RenderCopy(renderer_, minimize_button_, NULL, &button_rect);

    SDL_RenderPresent(renderer_);
  }
}
