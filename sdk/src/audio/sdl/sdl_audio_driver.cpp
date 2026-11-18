/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_INT32(audio_sdl_sample_frames, 0, "Audio",
                     "Host audio device buffer in sample frames asked of SDL (SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES); "
                     "0 = SDL's default. Smaller buffers mean smaller pulls from the frame queue")
    .range(0, 8192);

// [audio diag] (audio_system.cpp): what the host device takes per SDL callback.
namespace rex::audio::sdl {
std::atomic<uint64_t> g_sdl_pull_calls{0}, g_sdl_pull_frames{0}, g_sdl_pull_max{0}, g_sdl_pull_min{~0ull},
    g_sdl_silent_frames{0};
}  // namespace rex::audio::sdl
REXCVAR_DEFINE_INT32(audio_capture_s, 0, "Audio",
                     "Diagnostic: seconds of exactly what is handed to SDL (silence included) to write to "
                     "audio_capture.wav in the executable folder, once; 0 = off");
REXCVAR_DEFINE_INT32(audio_capture_skip_s, 0, "Audio",
                     "Diagnostic: seconds of output skipped before audio_capture_s starts");
REXCVAR_DEFINE_BOOL(audio_capture_guest, false, "Audio",
                    "Diagnostic: with audio_capture_s, also write the guest's 6 channels before the "
                    "stereo fold (fl fr fc lf bl br) to audio_capture_guest.wav (silent callbacks "
                    "are not included)");

namespace {
// DS2 port: output capture for checking clicks/static offline (tools/analysis/audio_clicks.py).
struct OutputCapture {
  const char* file_name = "audio_capture.wav";
  std::vector<float> samples;
  uint64_t seen = 0;  // interleaved samples handed to SDL so far
  bool done = false;
  void Add(const float* data, size_t count, int channels, int rate) {
    const int seconds = REXCVAR_GET(audio_capture_s);
    if (done || seconds <= 0 || channels <= 0) return;
    const uint64_t skip = uint64_t(std::max(0, REXCVAR_GET(audio_capture_skip_s))) * rate * channels;
    const uint64_t want = uint64_t(seconds) * rate * channels;
    for (size_t i = 0; i < count && samples.size() < want; ++i, ++seen) {
      if (seen >= skip) samples.push_back(data[i]);
    }
    if (samples.size() < want) return;
    done = true;
    const auto path = rex::filesystem::GetExecutableFolder() / file_name;
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return;
    const uint32_t bytes = uint32_t(samples.size() * sizeof(float));
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(3); u16(uint16_t(channels)); u32(uint32_t(rate)); u32(uint32_t(rate * channels * 4));
    u16(uint16_t(channels * 4)); u16(32); std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(samples.data(), sizeof(float), samples.size(), f);
    std::fclose(f);
    REXAPU_INFO("audio capture: {} s written to {}", seconds, path.string());
    samples.clear();
    samples.shrink_to_fit();
  }
};
OutputCapture g_output_capture;
OutputCapture g_guest_capture{"audio_capture_guest.wav"};
}  // namespace

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  if (const int frames = REXCVAR_GET(audio_sdl_sample_frames); frames > 0) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(frames).c_str());
  }

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  int device_sample_frames = 0;
  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, &device_sample_frames)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
#if REX_PLATFORM_ANDROID
  // Android reports the requested 6 channels for phone speakers and headsets
  // alike, and a 6-channel AAudio stream can stay silent there: always use
  // our stereo fold-down.
  obtained_spec.channels = 2;
#endif
  if (obtained_spec.channels <= 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}, device buffer {} sample frames; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), device_sample_frames,
              static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  {
    const uint64_t frames = uint64_t(std::max(additional_amount, 0)) /
                            (sizeof(float) * std::max<uint8_t>(driver->sdl_device_channels_, 1));
    g_sdl_pull_calls.fetch_add(1, std::memory_order_relaxed);
    g_sdl_pull_frames.fetch_add(frames, std::memory_order_relaxed);
    if (frames > g_sdl_pull_max.load(std::memory_order_relaxed)) g_sdl_pull_max.store(frames);
    if (frames < g_sdl_pull_min.load(std::memory_order_relaxed)) g_sdl_pull_min.store(frames);
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      g_sdl_silent_frames.fetch_add(1, std::memory_order_relaxed);
      g_output_capture.Add(data, size_t(sample_count), driver->sdl_device_channels_, 48000);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_capture_guest) && REXCVAR_GET(audio_capture_s) > 0) {
        // The guest frame as rendered: 6 big-endian channels one after another.
        std::array<float, channel_samples_ * 6> guest;
        for (uint32_t i = 0; i < channel_samples_; ++i) {
          for (uint32_t c = 0; c < 6; ++c) {
            guest[i * 6 + c] = rex::byte_swap(buffer[c * channel_samples_ + i]);
          }
        }
        g_guest_capture.Add(guest.data(), guest.size(), 6, 48000);
      }
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      g_output_capture.Add(data, size_t(sample_count), driver->sdl_device_channels_, 48000);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
