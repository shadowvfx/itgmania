#include "MovieTexture_Generic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "Actor.h"
#include "Preference.h"
#include "RageDisplay.h"
#include "RageLog.h"
#include "RageSurface.h"
#include "RageTextureID.h"
#include "RageTextureManager.h"
#include "RageTextureRenderTarget.h"
#include "RageThreads.h"
#include "RageTimer.h"
#include "RageTypes.h"
#include "RageUtil.h"
#include "Sprite.h"
#include "arch/MovieTexture/MovieTexture.h"
#include "global.h"

#if defined(WIN32)
#include <windows.h>

#include "archutils/Win32/ErrorStrings.h"
#endif

static Preference<bool> g_bMovieTextureDirectUpdates(
    "MovieTextureDirectUpdates", true);

MovieTexture_Generic::MovieTexture_Generic(
    RageTextureID ID, std::unique_ptr<MovieDecoder> pDecoder)
    : RageMovieTexture(ID) {
  LOG->Trace(
      "MovieTexture_Generic::MovieTexture_Generic(%s)", ID.filename.c_str());

  decoder_ = std::move(pDecoder);

  texture_handle_ = 0;
  render_target_ = nullptr;
  intermediate_texture_ = nullptr;
  loop_ = true;
  surface_ = nullptr;
  texture_lock_ = nullptr;
  rate_ = 1;
  clock_ = 0;
  sprite_ = std::make_unique<Sprite>();
}

std::string MovieTexture_Generic::Init() {
  std::string err = decoder_->Open(GetID().filename);
  if (err != "") {
    LOG->Warn(
        "MovieTexture_Generic::Init: failed to open decoder for file: %s, with "
        "error:\n%s",
        GetID().filename.c_str(), err.c_str());
    return err;
  }

  CreateTexture();
  CreateFrameRects();
  decoder_->SetLooping(loop_);

  decoding_thread_ = std::make_unique<std::thread>([this]() {
    LOG->Trace(
        "Beginning to decode video file \"%s\"", GetID().filename.c_str());
    auto timer = RageTimer();

    int ret = decoder_->DecodeMovie();
    if (ret == -1) {
      failure_ = true;
    }

    LOG->Trace(
        "Done decoding video file \"%s\", took %f seconds",
        GetID().filename.c_str(), timer.Ago());
  });

  LOG->Trace(
      "Resolution: %ix%i (%ix%i, %ix%i)", m_iSourceWidth, m_iSourceHeight,
      m_iImageWidth, m_iImageHeight, m_iTextureWidth, m_iTextureHeight);

  CHECKPOINT_M("Generic initialization completed. No errors found.");

  return std::string();
}

MovieTexture_Generic::~MovieTexture_Generic() {
  if (decoder_) {
    decoder_->Cancel();
    decoding_thread_->join();
    decoder_->Close();
  }

  /* sprite_ may reference the texture; delete it before DestroyTexture. */
  sprite_.reset();
  DestroyTexture();
}

/* Delete the surface and texture.  The decoding thread must be stopped, and
 * this is normally done after destroying the decoder. */
void MovieTexture_Generic::DestroyTexture() {
  delete surface_;
  surface_ = nullptr;

  delete texture_lock_;
  texture_lock_ = nullptr;

  if (texture_handle_) {
    DISPLAY->DeleteTexture(texture_handle_);
    texture_handle_ = 0;
  }

  intermediate_texture_ = nullptr;
}

class RageMovieTexture_Generic_Intermediate : public RageTexture {
 public:
  RageMovieTexture_Generic_Intermediate(
      RageTextureID ID, int iWidth, int iHeight, int iImageWidth,
      int iImageHeight, int iTextureWidth, int iTextureHeight,
      RageSurfaceFormat SurfaceFormat, RagePixelFormat pixfmt)
      : RageTexture(ID), m_SurfaceFormat(SurfaceFormat) {
    m_PixFmt = pixfmt;
    m_iSourceWidth = iWidth;
    m_iSourceHeight = iHeight;

    m_iImageWidth = iImageWidth;
    m_iImageHeight = iImageHeight;
    m_iTextureWidth = iTextureWidth;
    m_iTextureHeight = iTextureHeight;

    CreateFrameRects();

    texture_handle_ = 0;
    CreateTexture();
  }
  virtual ~RageMovieTexture_Generic_Intermediate() {
    if (texture_handle_) {
      DISPLAY->DeleteTexture(texture_handle_);
      texture_handle_ = 0;
    }
  }

  virtual void Invalidate() { texture_handle_ = 0; }
  virtual void Reload() {}
  virtual uintptr_t GetTexHandle() const { return texture_handle_; }

  bool IsAMovie() const { return true; }

 private:
  void CreateTexture() {
    if (texture_handle_) {
      return;
    }

    RageSurface* pSurface = CreateSurfaceFrom(
        m_iImageWidth, m_iImageHeight, m_SurfaceFormat.BitsPerPixel,
        m_SurfaceFormat.Mask[0], m_SurfaceFormat.Mask[1],
        m_SurfaceFormat.Mask[2], m_SurfaceFormat.Mask[3], nullptr, 1);

    texture_handle_ = DISPLAY->CreateTexture(m_PixFmt, pSurface, false);
    delete pSurface;
  }

  uintptr_t texture_handle_;
  RageSurfaceFormat m_SurfaceFormat;
  RagePixelFormat m_PixFmt;
};

void MovieTexture_Generic::Invalidate() {
  texture_handle_ = 0;
  if (intermediate_texture_ != nullptr) {
    intermediate_texture_->Invalidate();
  }
}

void MovieTexture_Generic::CreateTexture() {
  if (texture_handle_ || render_target_ != nullptr) {
    return;
  }

  CHECKPOINT;

  m_iSourceWidth = decoder_->GetWidth();
  m_iSourceHeight = decoder_->GetHeight();

  /* Adjust m_iSourceWidth to support different source aspect ratios. */
  float fSourceAspectRatio = decoder_->GetSourceAspectRatio();
  if (fSourceAspectRatio < 1) {
    m_iSourceHeight = std::lrint(m_iSourceHeight / fSourceAspectRatio);
  } else if (fSourceAspectRatio > 1) {
    m_iSourceWidth = std::lrint(m_iSourceWidth * fSourceAspectRatio);
  }

  /* HACK: Don't cap movie textures to the max texture size, since we
   * render them onto the texture at the source dimensions.  If we find a
   * fast way to resize movies, we can change this back. */
  m_iImageWidth = m_iSourceWidth;
  m_iImageHeight = m_iSourceHeight;

  /* Texture dimensions need to be a power of two; jump to the next. */
  m_iTextureWidth = power_of_two(m_iImageWidth);
  m_iTextureHeight = power_of_two(m_iImageHeight);
  MovieDecoderPixelFormatYCbCr fmt = PixelFormatYCbCr_Invalid;
  if (surface_ == nullptr) {
    ASSERT(texture_lock_ == nullptr);
    if (g_bMovieTextureDirectUpdates) {
      texture_lock_ = DISPLAY->CreateTextureLock();
    }

    surface_ = decoder_->CreateCompatibleSurface(
        m_iImageWidth, m_iImageHeight,
        TEXTUREMAN->GetPrefs().m_iMovieColorDepth == 32, fmt);
    if (texture_lock_ != nullptr) {
      delete[] surface_->pixels;
      surface_->pixels = nullptr;
    }
  }

  RagePixelFormat pixfmt = DISPLAY->FindPixelFormat(
      surface_->format->BitsPerPixel, surface_->format->Mask[0],
      surface_->format->Mask[1], surface_->format->Mask[2],
      surface_->format->Mask[3]);

  if (pixfmt == RagePixelFormat_Invalid) {
    /* We weren't given a natively-supported pixel format.  Pick a supported
     * one.  This is a fallback case, and implies a second conversion. */
    int depth = TEXTUREMAN->GetPrefs().m_iMovieColorDepth;
    switch (depth) {
      default:
        FAIL_M(ssprintf("Unsupported movie color depth: %i", depth));
      case 16:
        if (DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB5)) {
          pixfmt = RagePixelFormat_RGB5;
        } else {
          pixfmt = RagePixelFormat_RGBA4;
        }

        break;

      case 32:
        if (DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB8)) {
          pixfmt = RagePixelFormat_RGB8;
        } else if (DISPLAY->SupportsTextureFormat(RagePixelFormat_RGBA8)) {
          pixfmt = RagePixelFormat_RGBA8;
        } else if (DISPLAY->SupportsTextureFormat(RagePixelFormat_RGB5)) {
          pixfmt = RagePixelFormat_RGB5;
        } else {
          pixfmt = RagePixelFormat_RGBA4;
        }
        break;
    }
  }

  if (fmt != PixelFormatYCbCr_Invalid) {
    intermediate_texture_.reset();
    sprite_->UnloadTexture();

    /* Create the render target.  This will receive the final, converted
     * texture. */
    RenderTargetParam param;
    param.iWidth = m_iImageWidth;
    param.iHeight = m_iImageHeight;

    RageTextureID TargetID(GetID());
    TargetID.filename += " target";
    render_target_ = std::make_unique<RageTextureRenderTarget>(TargetID, param);

    /* Create the intermediate texture.  This receives the YUV image. */
    RageTextureID IntermedID(GetID());
    IntermedID.filename += " intermediate";

    intermediate_texture_ =
        std::make_unique<RageMovieTexture_Generic_Intermediate>(
            IntermedID, decoder_->GetWidth(), decoder_->GetHeight(),
            surface_->w, surface_->h, power_of_two(surface_->w),
            power_of_two(surface_->h), *surface_->format, pixfmt);

    /* Configure the sprite.  This blits the intermediate onto the ifnal render
     * target. */
    sprite_->SetHorizAlign(align_left);
    sprite_->SetVertAlign(align_top);

    /* Hack: Sprite wants to take ownership of the texture, and will decrement
     * the refcount when it unloads the texture.  Normally we'd make a "copy",
     * but we can't access RageTextureManager from here.  Just increment the
     * refcount. */
    ++intermediate_texture_->m_iRefCount;
    sprite_->SetTexture(intermediate_texture_.get());
    sprite_->SetEffectMode(GetEffectMode(fmt));

    return;
  }

  texture_handle_ = DISPLAY->CreateTexture(pixfmt, surface_, false);
}

/*
 * Returns:
 *  <= 0 if it's time for the next frame to display
 *   > 0 (seconds) if it's not yet time to display
 */
float MovieTexture_Generic::CheckFrameTime() {
  if (rate_ == 0) {
    return 1;  // "a long time until the next frame"
  }
  return (decoder_->GetTimestamp() - clock_) / rate_;
}

void MovieTexture_Generic::UpdateMovie(float seconds) {
  // Quick exit in case we failed to decode the movie.
  if (failure_) {
    return;
  }
  clock_ += seconds * rate_;

  // After a rewind, wait until the decoder has actually restarted before
  // reading frames; until then the cached frame 0 stays on screen.
  if (decoder_->IsResetPending()) {
    return;
  }

  // A looping movie has shown its last frame. Keep that frame up for its full
  // duration, then wrap around, keeping any time left over so loops don't
  // drift from the music.
  if (pending_rollover_) {
    if (clock_ < loop_length_) {
      return;
    }
    pending_rollover_ = false;
    decoder_->Rollover();
    clock_ = loop_length_ > 0 ? std::fmod(clock_, loop_length_) : 0.0f;
    next_frame_is_first_ = true;
  }

  // If the frame isn't ready, don't update. This does mean the video
  // will "speed up" to catch up when decoding does outpace display.
  //
  // In practice, display should rarely, if ever, outpace decoding.
  if (decoder_->IsCurrentFrameReady() && CheckFrameTime() <= 0) {
    UpdateFrame();
  }
}

float MovieTexture_Generic::FrameDuration() const {
  if (frame_duration_ > 0 && frame_duration_ < 1.0f) {
    return frame_duration_;
  }
  return 1.0f / 30.0f;  // Fallback if we haven't seen two frames yet.
}

void MovieTexture_Generic::UpdateFrame() {
  if (finished_ || pending_rollover_) {
    return;
  }

  /* Just in case we were invalidated: */
  CreateTexture();

  if (texture_lock_ != nullptr) {
    uintptr_t iHandle = intermediate_texture_ != nullptr
                            ? intermediate_texture_->GetTexHandle()
                            : this->GetTexHandle();
    texture_lock_->Lock(iHandle, surface_);
  }

  // How many frames we may drop in one update to catch up when the movie
  // plays faster than the screen refreshes (high rate and/or music rate).
  static const int kMaxCatchUpFrames = 8;

  int frame_ret = -1;
  int frames_skipped = 0;
  // Our frame buffer should (almost) always be 50 ahead of our current frame to
  // display, which is when IsCurrentFrameReady will evaluate to false. This
  // loop will continue attempting to find a displayable frame until we hit that
  // limit. If we really can't find a frame after that, then the currently
  // displayed frame will appear stuck.
  while (decoder_->IsCurrentFrameReady()) {
    const float frame_ts = decoder_->GetTimestamp();
    const bool is_first_frame = next_frame_is_first_;
    if (is_first_frame) {
      frame_ret = GetFirstFrame();
    } else {
      frame_ret = decoder_->GetFrame(surface_);
    }

    if (frame_ret >= 0) {
      if (is_first_frame) {
        next_frame_is_first_ = false;
      } else if (frame_ts > last_frame_ts_) {
        const float gap = frame_ts - last_frame_ts_;
        if (frame_duration_ <= 0 || gap < frame_duration_) {
          frame_duration_ = gap;
        }
      }
      last_frame_ts_ = frame_ts;
    }

    if (decoder_->EndOfMovie()) {
      if (loop_) {
        // Don't wrap yet: the last frame still has to be shown for its full
        // duration. UpdateMovie wraps once the clock passes its end.
        LOG->Info("File \"%s\" looping", GetID().filename.c_str());
        pending_rollover_ = true;
        loop_length_ = last_frame_ts_ + FrameDuration();
      } else {
        // At the end of the movie, and not looping.
        finished_ = true;
      }
      break;
    }

    // If we failed to display the frame, it's getting skipped, advance the
    // clock.
    if (frame_ret < 0) {
      clock_ += (decoder_->GetTimestamp() - clock_);
      continue;
    }

    // Catch up: if the next frame is already due as well, show that one
    // instead of falling further behind.
    if (frames_skipped < kMaxCatchUpFrames && decoder_->IsCurrentFrameReady() &&
        CheckFrameTime() <= 0) {
      ++frames_skipped;
      continue;
    }
    break;
  }

  if (texture_lock_ != nullptr) {
    texture_lock_->Unlock(surface_, true);
  }

  // There's an issue with the frame, make sure it does not get
  // uploaded.
  if (frame_ret < 0) {
    return;
  }

  UploadSurface();
}

void MovieTexture_Generic::UploadSurface() {
  if (render_target_ != nullptr) {
    CHECKPOINT_M("About to upload the texture.");

    /* If we have no texture_lock_, we still have to upload the texture. */
    if (texture_lock_ == nullptr) {
      DISPLAY->UpdateTexture(
          intermediate_texture_->GetTexHandle(), surface_, 0, 0, surface_->w,
          surface_->h);
    }
    render_target_->BeginRenderingTo(false);
    sprite_->Draw();
    render_target_->FinishRenderingTo();
  } else {
    if (texture_lock_ == nullptr) {
      DISPLAY->UpdateTexture(
          texture_handle_, surface_, 0, 0, m_iImageWidth, m_iImageHeight);
    }
  }
}

// Decodes frame 0 and keeps a copy of it, so a rewind can show it right away.
// When a texture lock is used, surface_->pixels points at write-only driver
// memory that can't be read back, so decode into our own buffer first and
// copy that into the locked memory.
int MovieTexture_Generic::GetFirstFrame() {
  if (surface_ == nullptr) {
    return decoder_->GetFrame(surface_);
  }
  const std::size_t size =
      static_cast<std::size_t>(surface_->pitch) * surface_->h;
  first_frame_pixels_.resize(size);

  uint8_t* target = surface_->pixels;
  surface_->pixels = first_frame_pixels_.data();
  const int ret = decoder_->GetFrame(surface_);
  surface_->pixels = target;

  if (ret < 0) {
    first_frame_pixels_.clear();
    return ret;
  }
  if (target != nullptr) {
    std::memcpy(target, first_frame_pixels_.data(), size);
  }
  return ret;
}

void MovieTexture_Generic::UploadCachedFirstFrame() {
  if (first_frame_pixels_.empty() || surface_ == nullptr) {
    return;
  }

  /* Just in case we were invalidated: */
  CreateTexture();

  if (texture_lock_ != nullptr) {
    uintptr_t iHandle = intermediate_texture_ != nullptr
                            ? intermediate_texture_->GetTexHandle()
                            : this->GetTexHandle();
    texture_lock_->Lock(iHandle, surface_);
  }

  const std::size_t size =
      static_cast<std::size_t>(surface_->pitch) * surface_->h;
  const bool ok =
      surface_->pixels != nullptr && size == first_frame_pixels_.size();
  if (ok) {
    std::memcpy(surface_->pixels, first_frame_pixels_.data(), size);
  }

  if (texture_lock_ != nullptr) {
    texture_lock_->Unlock(surface_, true);
  }

  if (ok) {
    UploadSurface();
  }
}

static EffectMode EffectModes[] = {
    EffectMode_YUYV422,
};
static_assert(ARRAYLEN(EffectModes) == NUM_PixelFormatYCbCr);

EffectMode MovieTexture_Generic::GetEffectMode(
    MovieDecoderPixelFormatYCbCr fmt) {
  ASSERT(fmt != PixelFormatYCbCr_Invalid);
  return EffectModes[fmt];
}

void MovieTexture_Generic::Reload() {}

void MovieTexture_Generic::SetPosition(float seconds) {
  // TODO: The only non-zero use case of this function would be practice mode.
  // Implement this by mathing out fSeconds and frame counts to seek the
  // video.
  if (seconds != 0) {
    LOG->Warn(
        "MovieTexture_Generic::SetPosition(%f): non-0 seeking unsupported; "
        "ignored",
        seconds);
    return;
  }

  // Already at the start (nothing shown since the movie was loaded, rewound
  // or wrapped around): frame 0 is next in line, so restarting the decoder
  // would only throw it away and leave the screen blank until it's decoded
  // again.
  if (next_frame_is_first_ && !finished_ && !pending_rollover_ &&
      !decoder_->IsResetPending()) {
    clock_ = 0;
    return;
  }

  LOG->Trace("Seek to %f", seconds);
  clock_ = 0;
  // A non-looping movie that reached its end must be able to play again.
  finished_ = false;
  pending_rollover_ = false;
  next_frame_is_first_ = true;
  last_frame_ts_ = 0;
  decoder_->Rewind();
  // Put frame 0 on screen now, so nothing stale or blank shows while the
  // decoder restarts in the background.
  UploadCachedFirstFrame();
}

uintptr_t MovieTexture_Generic::GetTexHandle() const {
  if (render_target_ != nullptr) {
    return render_target_->GetTexHandle();
  }

  return texture_handle_;
}

/*
 * (c) 2003-2005 Glenn Maynard
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
