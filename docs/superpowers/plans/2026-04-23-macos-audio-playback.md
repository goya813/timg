# macOS Audio Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add opt-in audio playback to timg for video files on macOS, kept roughly in lip-sync with the terminal video rendering.

**Architecture:** A new `AudioPlayer` class wraps CoreAudio AudioQueue. `VideoSource` detects an audio stream, decodes audio packets in the existing demux loop, and feeds PCM to `AudioPlayer`. Video frame pacing is switched from "frame count × frame duration" to "wait until audio clock catches up". Opt-in via `--audio`; single-file playback only; macOS-only code paths guarded by `WITH_TIMG_AUDIO`.

**Tech Stack:** C++17, ffmpeg libavcodec / libavformat / libswresample, CoreAudio AudioToolbox (AudioQueue), CMake.

**Reference spec:** `docs/superpowers/specs/2026-04-23-audio-playback-design.md`

**Test strategy:** The project has no existing unit-test infrastructure (no `add_test` in CMake). This plan uses **compile gates** and **manual smoke tests** after each task, matching the spec's testing section. Each task ends with a commit.

**Working state assumed at start:**
- Branch: `feature/macos-audio-playback`
- Spec already committed (`4bdc012` or later)
- Existing build directory at `/Users/goya/projects/timg/build/` configured with `-DWITH_RSVG=OFF -DWITH_POPPLER=OFF`
- A local test video with audio, e.g. `~/sample.mp4`, is useful for manual smoke tests from Task 7 onward. If you don't have one, a quick way to synthesize one:
  ```bash
  ffmpeg -f lavfi -i "testsrc=duration=10:size=320x240:rate=30" \
         -f lavfi -i "sine=frequency=440:duration=10" \
         -c:v libx264 -c:a aac -shortest /tmp/timg-sample.mp4
  ```

---

## File Structure

**New files:**
- `src/audio-player.h` — public `AudioPlayer` API (platform-neutral header; methods compile to no-op on non-macOS, though we won't include it on non-macOS)
- `src/audio-player-macos.cc` — CoreAudio AudioQueue implementation (compiled only when `WITH_TIMG_AUDIO` is defined)

**Modified files:**
- `CMakeLists.txt` — detect macOS, enable audio feature
- `src/CMakeLists.txt` — conditionally compile `audio-player-macos.cc`, link `AudioToolbox` framework, link `libswresample`
- `src/display-options.h` — add `bool audio_enabled`
- `src/video-source.h` — add audio members
- `src/video-source.cc` — stream detection, demux branching, sync-via-audio-clock, cleanup
- `src/timg.cc` — add `--audio` CLI flag, grid/multi-file guard

**No changes required:**
- `src/image-source.{h,cc}` — factory already forwards DisplayOptions to `LoadAndScale`
- `src/renderer.{h,cc}` — pacing change is internal to VideoSource

---

## Task 1: CMake scaffolding and empty source files

**Goal:** Wire `WITH_TIMG_AUDIO`, link AudioToolbox and swresample, add empty `audio-player.h` + `audio-player-macos.cc` that compile cleanly on macOS and are invisible on other platforms. No behavior change yet.

**Files:**
- Create: `src/audio-player.h`
- Create: `src/audio-player-macos.cc`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

### Steps

- [ ] **Step 1.1: Create minimal `src/audio-player.h`**

```cpp
// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.

#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <memory>

struct AVCodecParameters;
struct AVFrame;
struct AVRational;

namespace timg {

// Plays a single audio stream (PCM) to the default macOS audio output.
// Created per video file; destroyed when the video is done.
//
// Threading: Feed() is called from the demux thread. Now() may be called
// from any thread. The AudioQueue callback (on a CoreAudio-managed thread)
// runs internally. Start/Stop/Flush are called from the demux thread only.
class AudioPlayer {
public:
    // Factory. Opens the audio device and resampler. Returns nullptr if
    // the stream cannot be played (unsupported codec params, resampler
    // init failure, AudioQueue creation failure). Prints a one-shot
    // warning to stderr on non-trivial failures.
    static std::unique_ptr<AudioPlayer> Create(
        const AVCodecParameters *codecpar, AVRational time_base);

    ~AudioPlayer();

    // Feed a decoded audio AVFrame. Callee does not take ownership.
    // Blocks when the internal ring buffer is full (back-pressure).
    void Feed(const AVFrame *frame);

    // Drop all buffered audio and reset the playback clock. Called at
    // loop boundaries in VideoSource.
    void Flush();

    // Current audio playback PTS in seconds (same origin as the
    // container's audio stream PTS). Returns a negative value before
    // Start() or after Stop().
    double Now() const;

    // Begin playback. Idempotent. Call after enough samples have been
    // fed to prevent initial underrun.
    void Start();

    // Stop playback. Safe to call repeatedly.
    void Stop();

private:
    AudioPlayer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace timg

#endif  // AUDIO_PLAYER_H_
```

- [ ] **Step 1.2: Create minimal `src/audio-player-macos.cc`**

```cpp
// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// CoreAudio (AudioQueue) backed AudioPlayer. macOS only.

#include "audio-player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace timg {

struct AudioPlayer::Impl {};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() = default;

std::unique_ptr<AudioPlayer> AudioPlayer::Create(
    const AVCodecParameters * /*codecpar*/, AVRational /*time_base*/) {
    // Stub for Task 1. Real implementation comes in later tasks.
    return nullptr;
}

void AudioPlayer::Feed(const AVFrame * /*frame*/) {}
void AudioPlayer::Flush() {}
double AudioPlayer::Now() const { return -1.0; }
void AudioPlayer::Start() {}
void AudioPlayer::Stop() {}

}  // namespace timg
```

- [ ] **Step 1.3: Edit top-level `CMakeLists.txt` to detect macOS audio prerequisites**

Insert this block immediately after the existing `WITH_VIDEO_DECODING` pkg_check_modules block (around line 78, right before `find_package(Threads)`):

```cmake
# macOS audio playback (CoreAudio AudioQueue). Enabled automatically when
# building on Apple with video decoding enabled.
if(APPLE AND WITH_VIDEO_DECODING)
  pkg_check_modules(SWRESAMPLE IMPORTED_TARGET REQUIRED libswresample)
  set(WITH_TIMG_AUDIO ON)
endif()
```

- [ ] **Step 1.4: Edit `src/CMakeLists.txt` to compile audio sources conditionally**

Add this block immediately after the `if(WITH_VIDEO_DECODING) ... endif()` block (ends around line 105, right before the STB_IMAGE block):

```cmake
if(WITH_TIMG_AUDIO)
  target_sources(timg PUBLIC audio-player.h audio-player-macos.cc)
  target_compile_definitions(timg PUBLIC WITH_TIMG_AUDIO)
  target_link_libraries(timg PkgConfig::SWRESAMPLE "-framework AudioToolbox")
endif()
```

- [ ] **Step 1.5: Clean reconfigure and build**

Run:
```bash
cd /Users/goya/projects/timg/build && cmake .. -DWITH_RSVG=OFF -DWITH_POPPLER=OFF && make -j$(sysctl -n hw.ncpu)
```

Expected: Configure shows no new errors. Build completes. `build/src/timg --version` still works.

- [ ] **Step 1.6: Verify symbol linkage**

Run:
```bash
nm -gU /Users/goya/projects/timg/build/src/timg 2>/dev/null | grep -i AudioQueue | head -3
```
Expected: No output (we haven't called any AudioQueue functions yet). Acceptable; proves the binary still links.

Also verify:
```bash
otool -L /Users/goya/projects/timg/build/src/timg | grep -i -E "AudioToolbox|swresample"
```
Expected: Both `AudioToolbox.framework` and `libswresample.*.dylib` are listed.

- [ ] **Step 1.7: Commit**

```bash
cd /Users/goya/projects/timg
git add src/audio-player.h src/audio-player-macos.cc CMakeLists.txt src/CMakeLists.txt
git commit -m "Scaffold macOS audio playback build wiring.

Add WITH_TIMG_AUDIO build option, link AudioToolbox framework and
libswresample on macOS, introduce stub AudioPlayer class. No behavior
change; all methods are no-ops for now."
```

---

## Task 2: SPSC ring buffer helper

**Goal:** Lock-free single-producer / single-consumer ring buffer holding float32 interleaved stereo samples. Used by `AudioPlayer` to hand data from the demux thread to the AudioQueue callback.

**Files:**
- Create: `src/audio-ring-buffer.h` (header-only, internal to audio player)

### Steps

- [ ] **Step 2.1: Create `src/audio-ring-buffer.h`**

```cpp
// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// Single-producer / single-consumer ring buffer for interleaved float32
// audio samples. Used by AudioPlayer to hand PCM from the decode thread
// to the CoreAudio callback thread. Header-only.

#ifndef AUDIO_RING_BUFFER_H_
#define AUDIO_RING_BUFFER_H_

#include <atomic>
#include <cstddef>
#include <vector>

namespace timg {

// Holds N * channels floats. Indices are in "frames" (one frame = channels
// samples). Capacity in frames is fixed at construction.
class AudioRingBuffer {
public:
    AudioRingBuffer(std::size_t capacity_frames, int channels)
        : channels_(channels),
          capacity_(capacity_frames + 1),  // +1 to disambiguate full/empty
          data_(capacity_ * channels) {}

    int channels() const { return channels_; }

    // Number of frames currently readable. Safe to call from either side.
    std::size_t ReadableFrames() const {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return (w + capacity_ - r) % capacity_;
    }

    // Number of frames that can be written right now.
    std::size_t WritableFrames() const {
        return capacity_ - 1 - ReadableFrames();
    }

    // Write up to `frames` frames from `src` (interleaved). Returns the
    // number of frames actually written (may be less if buffer was full).
    // Producer side only.
    std::size_t Write(const float *src, std::size_t frames) {
        const std::size_t writable = WritableFrames();
        const std::size_t to_write = (frames < writable) ? frames : writable;
        std::size_t w = write_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < to_write; ++i) {
            for (int c = 0; c < channels_; ++c) {
                data_[w * channels_ + c] = src[i * channels_ + c];
            }
            w = (w + 1) % capacity_;
        }
        write_.store(w, std::memory_order_release);
        return to_write;
    }

    // Read up to `frames` frames into `dst`. Returns number actually read.
    // Consumer side only.
    std::size_t Read(float *dst, std::size_t frames) {
        const std::size_t readable = ReadableFrames();
        const std::size_t to_read = (frames < readable) ? frames : readable;
        std::size_t r = read_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < to_read; ++i) {
            for (int c = 0; c < channels_; ++c) {
                dst[i * channels_ + c] = data_[r * channels_ + c];
            }
            r = (r + 1) % capacity_;
        }
        read_.store(r, std::memory_order_release);
        return to_read;
    }

    // Drop all readable data. Caller must ensure no concurrent Read().
    void Clear() {
        read_.store(write_.load(std::memory_order_acquire),
                    std::memory_order_release);
    }

private:
    const int channels_;
    const std::size_t capacity_;
    std::vector<float> data_;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

}  // namespace timg

#endif  // AUDIO_RING_BUFFER_H_
```

- [ ] **Step 2.2: Build to verify header compiles when included**

Temporarily add `#include "audio-ring-buffer.h"` at the top of `src/audio-player-macos.cc` (after the `#include "audio-player.h"` line). Then:
```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Build succeeds with no warnings.

- [ ] **Step 2.3: Commit**

```bash
cd /Users/goya/projects/timg
git add src/audio-ring-buffer.h src/audio-player-macos.cc
git commit -m "Add SPSC audio ring buffer helper.

Lock-free single-producer / single-consumer ring buffer for interleaved
float32 frames. Used by the forthcoming AudioPlayer implementation."
```

---

## Task 3: AudioPlayer — CoreAudio device open + format negotiation

**Goal:** `AudioPlayer::Create` opens an `AudioQueueRef` at 48 kHz / stereo / float32 and initializes an `SwrContext` that can convert the ffmpeg source format. `Start`, `Stop` control the queue. `Feed`, `Now` are still stubs. Calling `Create` on a valid audio stream now returns a non-null pointer but output is silent (no data is being enqueued).

**Files:**
- Modify: `src/audio-player-macos.cc`

### Steps

- [ ] **Step 3.1: Replace `src/audio-player-macos.cc` with the full device-open implementation**

```cpp
// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// CoreAudio (AudioQueue) backed AudioPlayer. macOS only.

#include "audio-player.h"

#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudioTypes.h>

#include <atomic>
#include <cstdio>

#include "audio-ring-buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

namespace timg {
namespace {

// Device output format.
constexpr double kDeviceSampleRate   = 48000.0;
constexpr int    kDeviceChannels     = 2;
constexpr UInt32 kFramesPerBuffer    = 1024;        // ≈ 21 ms at 48 kHz
constexpr int    kBufferCount        = 4;
constexpr std::size_t kRingCapacityFrames =
    static_cast<std::size_t>(kDeviceSampleRate * 2.0);  // 2 seconds

void WarnOnce(const char *msg) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        std::fprintf(stderr, "timg audio: %s\n", msg);
    }
}

}  // namespace

struct AudioPlayer::Impl {
    // Set only at construction; never modified afterwards.
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kBufferCount] = {};
    SwrContext *swr = nullptr;
    AudioRingBuffer ring{kRingCapacityFrames, kDeviceChannels};

    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};

    ~Impl() {
        if (queue) {
            AudioQueueStop(queue, true);
            for (AudioQueueBufferRef b : buffers) {
                if (b) AudioQueueFreeBuffer(queue, b);
            }
            AudioQueueDispose(queue, true);
        }
        if (swr) swr_free(&swr);
    }
};

// ---- ctor / dtor ----
AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() = default;

// Forward-declared callback; full implementation comes in Task 4.
static void OutputCallback(void * /*user_data*/,
                           AudioQueueRef queue,
                           AudioQueueBufferRef buffer) {
    // Task 3: emit silence.
    std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

std::unique_ptr<AudioPlayer> AudioPlayer::Create(
    const AVCodecParameters *codecpar, AVRational /*time_base*/) {
    if (!codecpar) return nullptr;

    const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        WarnOnce("audio codec unsupported, playing silent");
        return nullptr;
    }

    auto player = std::unique_ptr<AudioPlayer>(new AudioPlayer());
    auto &imp = *player->impl_;

    // --- swresample setup ---
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout;
    if (codecpar->ch_layout.nb_channels > 0 &&
        codecpar->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_copy(&in_layout, &codecpar->ch_layout);
    } else {
        av_channel_layout_default(&in_layout,
                                  codecpar->ch_layout.nb_channels > 0
                                      ? codecpar->ch_layout.nb_channels
                                      : 2);
    }

    int rc = swr_alloc_set_opts2(
        &imp.swr,
        &out_layout, AV_SAMPLE_FMT_FLT,
        static_cast<int>(kDeviceSampleRate),
        &in_layout, static_cast<AVSampleFormat>(codecpar->format),
        codecpar->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !imp.swr || swr_init(imp.swr) < 0) {
        WarnOnce("audio resampler init failed, playing silent");
        return nullptr;
    }

    // --- AudioQueue setup ---
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = kDeviceSampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket   = sizeof(float) * kDeviceChannels;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = sizeof(float) * kDeviceChannels;
    fmt.mChannelsPerFrame = kDeviceChannels;
    fmt.mBitsPerChannel   = 32;

    OSStatus os = AudioQueueNewOutput(&fmt, &OutputCallback, player.get(),
                                      nullptr, nullptr, 0, &imp.queue);
    if (os != noErr) {
        WarnOnce("AudioQueueNewOutput failed, playing silent");
        return nullptr;
    }

    const UInt32 buf_bytes = kFramesPerBuffer * fmt.mBytesPerFrame;
    for (int i = 0; i < kBufferCount; ++i) {
        os = AudioQueueAllocateBuffer(imp.queue, buf_bytes, &imp.buffers[i]);
        if (os != noErr) {
            WarnOnce("AudioQueueAllocateBuffer failed, playing silent");
            return nullptr;
        }
    }

    return player;
}

void AudioPlayer::Feed(const AVFrame * /*frame*/) {
    // Task 4.
}

void AudioPlayer::Flush() {
    if (!impl_->queue) return;
    AudioQueueFlush(impl_->queue);
    impl_->ring.Clear();
}

double AudioPlayer::Now() const { return -1.0; }

void AudioPlayer::Start() {
    if (!impl_->queue || impl_->running.load()) return;
    // Pre-fill all buffers with silence so playback starts immediately.
    for (int i = 0; i < kBufferCount; ++i) {
        OutputCallback(this, impl_->queue, impl_->buffers[i]);
    }
    if (AudioQueueStart(impl_->queue, nullptr) == noErr) {
        impl_->running.store(true);
    } else {
        WarnOnce("AudioQueueStart failed, playing silent");
    }
}

void AudioPlayer::Stop() {
    if (!impl_->queue) return;
    impl_->stopping.store(true);
    AudioQueueStop(impl_->queue, true);
    impl_->running.store(false);
}

}  // namespace timg
```

- [ ] **Step 3.2: Remove the temporary include added in Task 2 if it was not removed already**

The file above already includes `audio-ring-buffer.h`, so this is fine — nothing to do.

- [ ] **Step 3.3: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build. Warnings about unused `user_data` parameter in `OutputCallback` are acceptable (the static `OutputCallback` uses `this`-like semantics via parameter, but currently the param is unused — that is fine).

- [ ] **Step 3.4: Smoke test — ensure `timg --version` still succeeds**

```bash
/Users/goya/projects/timg/build/src/timg --version | head -3
```
Expected: Version banner. No crash.

- [ ] **Step 3.5: Commit**

```bash
cd /Users/goya/projects/timg
git add src/audio-player-macos.cc
git commit -m "Open CoreAudio AudioQueue and set up swresample in AudioPlayer.

AudioPlayer::Create now successfully opens a 48kHz stereo float32
AudioQueue and initializes an SwrContext matching the input stream.
Start() begins the queue (emitting silence for now); Stop() tears it
down. No audible output yet; that comes once the ring buffer is wired
into the callback."
```

---

## Task 4: AudioPlayer — data flow and audio clock

**Goal:** Feed decoded ffmpeg frames through the resampler into the ring buffer. The AudioQueue callback pulls from the ring into output buffers and advances a sample counter. `Now()` returns `base_pts + played_frames / sample_rate`.

**Files:**
- Modify: `src/audio-player-macos.cc`

### Steps

- [ ] **Step 4.1: Extend `AudioPlayer::Impl` with playback-clock fields**

In `struct AudioPlayer::Impl`, add these members after the `stopping` atomic:

```cpp
    // Clock: Now() = base_pts + played_frames / kDeviceSampleRate.
    // base_pts is the PTS of the first audio sample ever fed (seconds).
    // played_frames is the cumulative frame count handed to CoreAudio.
    std::atomic<double> base_pts{-1.0};
    std::atomic<uint64_t> played_frames{0};
    AVRational time_base{0, 1};       // copied from audio stream
    int64_t next_pts_frames = 0;       // cumulative frame count Fed so far

    // Per-buffer scratch for resampling. Interleaved float32.
    std::vector<float> resample_scratch;
```

- [ ] **Step 4.2: Store `time_base` during `Create` so Feed can convert PTS**

Right after the factory creates `player`, copy the time base:

```cpp
    auto player = std::unique_ptr<AudioPlayer>(new AudioPlayer());
    auto &imp = *player->impl_;
    imp.time_base = time_base;
```

(Requires changing `AVRational /*time_base*/` to `AVRational time_base` in the `Create` signature body — it's already declared that way in the header, so just remove the `/* */` comment-out in the definition.)

- [ ] **Step 4.3: Replace `AudioPlayer::Feed`**

```cpp
void AudioPlayer::Feed(const AVFrame *frame) {
    if (!impl_->swr || !frame) return;

    // Seed base_pts from the first frame we see.
    if (impl_->base_pts.load(std::memory_order_relaxed) < 0.0 &&
        frame->pts != AV_NOPTS_VALUE) {
        const double pts_sec =
            frame->pts * av_q2d(impl_->time_base);
        impl_->base_pts.store(pts_sec, std::memory_order_release);
    }

    // Worst-case output frame count for this input.
    const int64_t delay = swr_get_delay(impl_->swr, frame->sample_rate);
    const int max_out =
        av_rescale_rnd(delay + frame->nb_samples,
                       static_cast<int64_t>(kDeviceSampleRate),
                       frame->sample_rate, AV_ROUND_UP);
    if (max_out <= 0) return;

    auto &scratch = impl_->resample_scratch;
    if (static_cast<int>(scratch.size()) < max_out * kDeviceChannels) {
        scratch.resize(max_out * kDeviceChannels);
    }

    uint8_t *out_ptrs[1] = {reinterpret_cast<uint8_t *>(scratch.data())};
    const int out_frames = swr_convert(
        impl_->swr, out_ptrs, max_out,
        const_cast<const uint8_t **>(frame->data), frame->nb_samples);
    if (out_frames <= 0) return;

    // Write to ring, blocking (spin) while full. Back-pressure stops the
    // demux loop from running too far ahead.
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(out_frames)) {
        const std::size_t w = impl_->ring.Write(
            scratch.data() + written * kDeviceChannels,
            out_frames - written);
        written += w;
        if (written < static_cast<std::size_t>(out_frames)) {
            // Ring full: sleep ≈1 buffer duration before retrying.
            struct timespec ts = {0, 20 * 1000 * 1000};  // 20 ms
            nanosleep(&ts, nullptr);
            if (impl_->stopping.load(std::memory_order_relaxed)) return;
        }
    }
}
```

- [ ] **Step 4.4: Replace the `OutputCallback` body**

```cpp
static void OutputCallback(void *user_data,
                           AudioQueueRef queue,
                           AudioQueueBufferRef buffer) {
    auto *self = static_cast<AudioPlayer *>(user_data);
    auto &imp = *self->impl_;  // NOTE: Impl is private; see Step 4.5.

    float *out = static_cast<float *>(buffer->mAudioData);
    const std::size_t want =
        buffer->mAudioDataBytesCapacity / (sizeof(float) * kDeviceChannels);
    const std::size_t got = imp.ring.Read(out, want);
    if (got < want) {
        std::memset(out + got * kDeviceChannels, 0,
                    (want - got) * sizeof(float) * kDeviceChannels);
    }
    buffer->mAudioDataByteSize = want * sizeof(float) * kDeviceChannels;

    imp.played_frames.fetch_add(want, std::memory_order_release);

    if (!imp.stopping.load(std::memory_order_relaxed)) {
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
}
```

- [ ] **Step 4.5: Expose `Impl` to the callback**

The callback is a free function and cannot access the private `impl_`. Add a `friend` declaration inside `AudioPlayer`'s class body in the header, OR (simpler) declare the callback as a static member function.

Preferred: replace the forward-declared `static void OutputCallback(...)` with a `static void AudioPlayer::OutputCallback(...)` member. To do so:

1. In `src/audio-player.h`, inside the `class AudioPlayer` body, add to the private section (just above the existing `AudioPlayer();`):
    ```cpp
    private:
        static void OutputCallback(void *user_data, void *queue, void *buffer);
    ```
    Use `void *` for the queue/buffer types to avoid forcing CoreAudio headers on every translation unit.

2. In `src/audio-player-macos.cc`, change the definition to:
    ```cpp
    void AudioPlayer::OutputCallback(void *user_data,
                                     void *queue_opaque,
                                     void *buffer_opaque) {
        auto *self = static_cast<AudioPlayer *>(user_data);
        auto *queue = static_cast<AudioQueueRef>(queue_opaque);
        auto *buffer = static_cast<AudioQueueBufferRef>(buffer_opaque);
        // ...body from Step 4.4...
    }
    ```

3. Update `AudioQueueNewOutput` to pass the member via `reinterpret_cast`:
    ```cpp
    os = AudioQueueNewOutput(
        &fmt,
        reinterpret_cast<AudioQueueOutputCallback>(&AudioPlayer::OutputCallback),
        player.get(),
        nullptr, nullptr, 0, &imp.queue);
    ```

4. Update the pre-fill loop in `Start()` similarly:
    ```cpp
    for (int i = 0; i < kBufferCount; ++i) {
        AudioPlayer::OutputCallback(this, impl_->queue, impl_->buffers[i]);
    }
    ```

- [ ] **Step 4.6: Replace `AudioPlayer::Now`**

```cpp
double AudioPlayer::Now() const {
    const double base = impl_->base_pts.load(std::memory_order_acquire);
    if (base < 0.0) return -1.0;
    const uint64_t frames = impl_->played_frames.load(std::memory_order_acquire);
    return base + static_cast<double>(frames) / kDeviceSampleRate;
}
```

- [ ] **Step 4.7: Extend `Flush` to reset the clock**

```cpp
void AudioPlayer::Flush() {
    if (!impl_->queue) return;
    AudioQueueFlush(impl_->queue);
    impl_->ring.Clear();
    impl_->played_frames.store(0, std::memory_order_release);
    impl_->base_pts.store(-1.0, std::memory_order_release);
}
```

- [ ] **Step 4.8: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 4.9: Smoke test — run on a silent file**

```bash
/Users/goya/projects/timg/build/src/timg /Users/goya/projects/timg/img/logo.svg
```
Expected: Still renders a static image (no audio path exercised). No crash.

- [ ] **Step 4.10: Commit**

```bash
cd /Users/goya/projects/timg
git add src/audio-player.h src/audio-player-macos.cc
git commit -m "Implement AudioPlayer data flow and playback clock.

Feed() resamples AVFrames via swresample into the ring buffer with
back-pressure. The AudioQueue output callback pulls from the ring and
advances a played-frames counter. Now() reports base_pts + played /
48kHz. Flush() resets both. Not wired into VideoSource yet, so still
no audible output."
```

---

## Task 5: VideoSource — detect audio stream and construct AudioPlayer

**Goal:** When `DisplayOptions::audio_enabled` is true, `VideoSource::LoadAndScale` finds the best audio stream, opens its decoder, and constructs an `AudioPlayer`. No audio data is fed yet; video rendering still uses the existing frame-duration pacing.

**Files:**
- Modify: `src/display-options.h`
- Modify: `src/video-source.h`
- Modify: `src/video-source.cc`

### Steps

- [ ] **Step 5.1: Add `audio_enabled` to `DisplayOptions`**

In `src/display-options.h`, add this field at the end of the struct (after `int pattern_size = 1;`, around line 104):

```cpp
    // Play audio track alongside video (macOS only; guarded by
    // WITH_TIMG_AUDIO at the CLI level).
    bool audio_enabled = false;
```

- [ ] **Step 5.2: Extend `VideoSource`**

In `src/video-source.h`:

1. Forward-declare:
    ```cpp
    struct AVCodecParameters;
    ```
    (after the existing `struct AVFrame;` line, around line 31)

2. Add an include guard block for `audio-player.h` near the top (after `#include "timg-time.h"`):
    ```cpp
    #ifdef WITH_TIMG_AUDIO
    #include "audio-player.h"
    #endif
    ```

3. Add these members to the private section (after `int center_indentation_ = 0;`, around line 78):
    ```cpp
        int audio_stream_index_               = -1;
        AVCodecContext *audio_codec_context_  = nullptr;
    #ifdef WITH_TIMG_AUDIO
        std::unique_ptr<AudioPlayer> audio_player_;
    #endif
    ```

4. Add `<memory>` to the top-level includes if not already present. (It is not; add it.)

- [ ] **Step 5.3: Open the audio stream in `LoadAndScale`**

In `src/video-source.cc`, add `#include <libavutil/rational.h>` inside the existing `extern "C"` block (around line 47), after the `avformat.h` line.

Then, at the end of `LoadAndScale` — right before the final `return true;` (around line 257) — insert:

```cpp
#ifdef WITH_TIMG_AUDIO
    if (display_options.audio_enabled) {
        for (int i = 0; i < (int)format_context_->nb_streams; ++i) {
            AVStream *s = format_context_->streams[i];
            if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            const AVCodec *ac = avcodec_find_decoder(s->codecpar->codec_id);
            if (!ac) break;
            audio_codec_context_ = avcodec_alloc_context3(ac);
            if (!audio_codec_context_) break;
            if (avcodec_parameters_to_context(audio_codec_context_,
                                              s->codecpar) < 0 ||
                avcodec_open2(audio_codec_context_, ac, nullptr) < 0) {
                avcodec_free_context(&audio_codec_context_);
                break;
            }
            audio_stream_index_ = i;
            audio_player_ = AudioPlayer::Create(s->codecpar, s->time_base);
            if (!audio_player_) {
                avcodec_free_context(&audio_codec_context_);
                audio_stream_index_ = -1;
            }
            break;
        }
    }
#endif
```

- [ ] **Step 5.4: Clean up in destructor**

Find the `VideoSource::~VideoSource()` in `video-source.cc` (search for `VideoSource::~`). Add before its existing cleanup:

```cpp
#ifdef WITH_TIMG_AUDIO
    if (audio_player_) audio_player_->Stop();
    audio_player_.reset();
#endif
    if (audio_codec_context_) avcodec_free_context(&audio_codec_context_);
```

If the destructor doesn't yet exist or uses default, you'll need to peek at how the existing fields are freed. Inspect with:

```bash
grep -n "VideoSource::~\|~VideoSource\|avcodec_free_context\|sws_freeContext" /Users/goya/projects/timg/src/video-source.cc
```

Place the new code in whatever cleanup path you find (likely alongside the existing `avcodec_free_context(&codec_context_);`).

- [ ] **Step 5.5: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 5.6: Smoke test — video without --audio still works**

```bash
/Users/goya/projects/timg/build/src/timg /tmp/timg-sample.mp4 --frames=30
```
Expected: Plays 30 frames of video silently. No crash.

- [ ] **Step 5.7: Commit**

```bash
cd /Users/goya/projects/timg
git add src/display-options.h src/video-source.h src/video-source.cc
git commit -m "Detect and open audio stream in VideoSource when enabled.

When DisplayOptions::audio_enabled is true, VideoSource::LoadAndScale
finds the first audio stream, opens its decoder, and constructs an
AudioPlayer. The player is torn down in the destructor. No audio
packets are fed yet; video pacing is unchanged."
```

---

## Task 6: VideoSource — demux and feed audio packets

**Goal:** Wire the existing demux loop in `SendFrames` to route audio packets to the audio decoder, collect decoded frames, and call `AudioPlayer::Feed`. Call `AudioPlayer::Start` once some audio has been buffered. Video pacing still uses the existing frame-counter path; sync correction comes in Task 7. After this task, running `timg --audio video.mp4` should produce **sound** (though A/V may drift).

**Files:**
- Modify: `src/video-source.cc`

### Steps

- [ ] **Step 6.1: Extend the inner loop in `SendFrames`**

Locate the block around line 324:
```cpp
if (state_reading && packet->stream_index != video_stream_index_) {
    av_packet_unref(packet);
    continue;  // Not a packet we're interested in
}
```

Replace it with:

```cpp
#ifdef WITH_TIMG_AUDIO
            if (state_reading && packet->stream_index == audio_stream_index_ &&
                audio_player_ && audio_codec_context_) {
                if (avcodec_send_packet(audio_codec_context_, packet) == 0) {
                    AVFrame *af = av_frame_alloc();
                    while (avcodec_receive_frame(audio_codec_context_, af) == 0) {
                        audio_player_->Feed(af);
                        av_frame_unref(af);
                    }
                    av_frame_free(&af);
                }
                av_packet_unref(packet);
                continue;
            }
#endif
            if (state_reading && packet->stream_index != video_stream_index_) {
                av_packet_unref(packet);
                continue;  // Not a packet we're interested in
            }
```

- [ ] **Step 6.2: Call `AudioPlayer::Start` once, just after entering the outer loop**

Find the `for (int k = 0; ...)` outer loop (around line 298). Immediately after the `if (k > 0)` rewind block, add:

```cpp
#ifdef WITH_TIMG_AUDIO
        // Pre-roll: pump packets until the audio player reports that
        // enough samples have queued, then Start(). Time out at ~0.5s
        // in real time so we don't hang on video-only files.
        if (audio_player_) {
            const auto pre_roll_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(500);
            while (audio_player_->Now() < 0.0 &&
                   std::chrono::steady_clock::now() < pre_roll_deadline) {
                if (av_read_frame(format_context_, packet) != 0) break;
                if (packet->stream_index == audio_stream_index_ &&
                    audio_codec_context_) {
                    if (avcodec_send_packet(audio_codec_context_, packet) == 0) {
                        AVFrame *af = av_frame_alloc();
                        while (avcodec_receive_frame(audio_codec_context_, af)
                               == 0) {
                            audio_player_->Feed(af);
                            av_frame_unref(af);
                        }
                        av_frame_free(&af);
                    }
                } else {
                    // Push video packet back into the decoder immediately so
                    // we don't lose it.
                    if (packet->stream_index == video_stream_index_) {
                        if (avcodec_send_packet(codec_context_, packet) == 0) {
                            ++decode_in_flight;
                        }
                    }
                }
                av_packet_unref(packet);
            }
            audio_player_->Start();
        }
#endif
```

Add `#include <chrono>` to the top of the file (after `<cstring>` around line 28) if not present.

- [ ] **Step 6.3: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 6.4: Smoke test — audio should now be audible**

```bash
/Users/goya/projects/timg/build/src/timg --audio /tmp/timg-sample.mp4
```
Expected: Video plays AND a 440 Hz tone plays through your default audio output. Lip sync may drift (that's fine — Task 7 fixes it). The tone ending roughly matches the video ending.

Note: if you hear nothing, check system volume and that your terminal is not muted. Also check for stderr warnings (`timg audio: ...`).

- [ ] **Step 6.5: Commit**

```bash
cd /Users/goya/projects/timg
git add src/video-source.cc
git commit -m "Route decoded audio packets to AudioPlayer in SendFrames.

Audio packets are decoded in the demux loop and fed to the player; a
short pre-roll before AudioPlayer::Start() prevents initial underrun.
Video pacing still uses the legacy frame-counter path, so A/V drift is
possible — fixed in the next commit."
```

---

## Task 7: VideoSource — switch video pacing to audio clock

**Goal:** Replace the frame-duration-based pacing with "wait until `audio_player_->Now() >= frame_pts`" when audio is active. Drop video frames that are ≥100 ms late. Keep the legacy path when `audio_player_` is null.

**Files:**
- Modify: `src/video-source.cc`

### Steps

- [ ] **Step 7.1: Capture the video stream time base near the top of `SendFrames`**

At the top of `SendFrames` (right after the `AVPacket *packet = av_packet_alloc();` line, around line 287), add:

```cpp
#ifdef WITH_TIMG_AUDIO
    const AVRational video_time_base =
        format_context_->streams[video_stream_index_]->time_base;
#endif
```

- [ ] **Step 7.2: Replace the pacing logic inside the decode-loop**

Locate the block that currently advances `time_from_first_frame` and emits the frame (lines ~349-364):

```cpp
time_from_first_frame.Add(frame_duration_);
// TODO: when frame skipping enabled, avoid this step if we're
// falling behind.
sws_scale(sws_context_, decode_frame->data,
          decode_frame->linesize, 0, codec_context_->height,
          terminal_fb_->row_data(), terminal_fb_->stride());
AlphaBlendFramebuffer();
const int dy = is_first ? 0 : -terminal_fb_->height();
sink(center_indentation_, dy, *terminal_fb_,
     is_first ? SeqType::StartOfAnimation
              : SeqType::AnimationFrame,
     time_from_first_frame);
```

Replace it with:

```cpp
#ifdef WITH_TIMG_AUDIO
                if (audio_player_) {
                    const double frame_pts =
                        decode_frame->best_effort_timestamp *
                        av_q2d(video_time_base);
                    const double audio_now = audio_player_->Now();
                    if (audio_now >= 0.0) {
                        const double wait = frame_pts - audio_now;
                        if (wait < -0.1) {
                            // ≥100 ms late — drop this video frame.
                            ++observed_frame_count;
                            if (frame_limit) --remaining_frames;
                            continue;
                        }
                        double capped_wait = wait > 0.5 ? 0.5 : wait;
                        if (capped_wait > 0.0) {
                            Duration::Nanos(static_cast<int64_t>(
                                capped_wait * 1e9)).WaitUntil(
                                Time::Now() + Duration::Nanos(
                                    static_cast<int64_t>(capped_wait * 1e9)));
                        }
                    }
                    time_from_first_frame = Duration::Nanos(
                        static_cast<int64_t>(frame_pts * 1e9));
                } else
#endif
                {
                    time_from_first_frame.Add(frame_duration_);
                }
                sws_scale(sws_context_, decode_frame->data,
                          decode_frame->linesize, 0, codec_context_->height,
                          terminal_fb_->row_data(), terminal_fb_->stride());
                AlphaBlendFramebuffer();
                const int dy = is_first ? 0 : -terminal_fb_->height();
                sink(center_indentation_, dy, *terminal_fb_,
                     is_first ? SeqType::StartOfAnimation
                              : SeqType::AnimationFrame,
                     time_from_first_frame);
```

Note: the existing code uses `timg::Duration::WaitUntil(absolute_time)` style — confirm the available API by inspecting `src/timg-time.h`:

```bash
grep -nE "Sleep|WaitUntil|Nanos\(" /Users/goya/projects/timg/src/timg-time.h
```

If a simpler `Duration::Nanos(n).Sleep()` or similar exists, prefer that. Adjust the wait implementation to match the real API — the intent is "sleep for `capped_wait` seconds, but wake up if interrupted".

If in doubt, use `nanosleep`:

```cpp
if (capped_wait > 0.0) {
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(capped_wait);
    ts.tv_nsec = static_cast<long>((capped_wait - ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
}
```

and `#include <ctime>` at the top of `video-source.cc` if not already included.

- [ ] **Step 7.3: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 7.4: Smoke test — lip sync**

```bash
/Users/goya/projects/timg/build/src/timg --audio /tmp/timg-sample.mp4
```

Expected: Playback runs at roughly real-time. If your sample has a visible counter / flashing clapper, the audible tone onset lines up with the visual event within ≈50 ms.

If you used the ffmpeg synthesis from the intro, the visual is a simple test pattern with a counter; the audio is a continuous 440 Hz tone, so you'll mostly verify that:
1. The whole clip ends at roughly the same time audibly and visually.
2. The terminal rendering doesn't run ahead / trail the audio.

For a better test, use a video you know (speech / music video).

- [ ] **Step 7.5: Commit**

```bash
cd /Users/goya/projects/timg
git add src/video-source.cc
git commit -m "Drive video pacing from the audio clock.

When AudioPlayer is active, each video frame is held back until its PTS
catches up with AudioPlayer::Now(). Frames ≥100 ms late are dropped.
Falls back to the legacy frame-duration pacing when audio is disabled
or unavailable."
```

---

## Task 8: Loop and interrupt handling for audio

**Goal:** At each loop boundary, flush the audio codec and player so the new iteration starts cleanly. On SIGINT, ensure the audio stops promptly.

**Files:**
- Modify: `src/video-source.cc`

### Steps

- [ ] **Step 8.1: Flush audio codec and player on loop rewind**

Find the rewind block (around line 302):

```cpp
if (k > 0) {
    // Rewind unless we're just starting.
    av_seek_frame(format_context_, video_stream_index_, 0,
                  AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(codec_context_);
}
```

Expand to:

```cpp
if (k > 0) {
    // Rewind unless we're just starting.
    av_seek_frame(format_context_, video_stream_index_, 0,
                  AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(codec_context_);
#ifdef WITH_TIMG_AUDIO
    if (audio_codec_context_) avcodec_flush_buffers(audio_codec_context_);
    if (audio_player_) audio_player_->Flush();
#endif
}
```

- [ ] **Step 8.2: Stop the audio player on interrupt / end of playback**

At the very end of `SendFrames`, right after `av_packet_free(&packet);` (around line 369), add:

```cpp
#ifdef WITH_TIMG_AUDIO
    if (audio_player_) audio_player_->Stop();
#endif
```

- [ ] **Step 8.3: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 8.4: Smoke test — loops**

```bash
/Users/goya/projects/timg/build/src/timg --audio --loops=3 /tmp/timg-sample.mp4
```
Expected: Video plays 3 times. Audio restarts at each loop without stuck / repeated samples. The clap of a click-noise at the boundary is acceptable.

- [ ] **Step 8.5: Smoke test — Ctrl-C**

Start the sample and press Ctrl-C mid-playback:
```bash
/Users/goya/projects/timg/build/src/timg --audio /tmp/timg-sample.mp4
# press Ctrl-C
```
Expected: Audio stops immediately (not continuing for ~0.5 s). Process exits.

- [ ] **Step 8.6: Commit**

```bash
cd /Users/goya/projects/timg
git add src/video-source.cc
git commit -m "Flush audio on loop and stop on interrupt.

Looped playback now flushes the audio codec and AudioPlayer at each
rewind, and SendFrames stops the player on exit so SIGINT terminates
audio promptly."
```

---

## Task 9: CLI — `--audio` flag and grid/multi-file guard

**Goal:** Add `--audio` to the option parser. After option parsing, if the user asked for `--audio` but grid mode is active or multiple input files were provided, print a warning and force `audio_enabled = false`. On non-macOS builds the option doesn't exist (guarded by `WITH_TIMG_AUDIO`).

**Files:**
- Modify: `src/timg.cc`

### Steps

- [ ] **Step 9.1: Add the new long option ID**

In `src/timg.cc`, find `enum LongOptionIds` (around line 474). Add a new member:

```cpp
        OPT_SCROLL,
        OPT_AUDIO,
    };
```

- [ ] **Step 9.2: Add the `--audio` long option entry**

In the `long_options` array (starts around line 495), in alphabetical order between `"auto-crop"` and `"center"`, insert:

```cpp
#ifdef WITH_TIMG_AUDIO
        {"audio",                no_argument,       NULL, OPT_AUDIO         },
#endif
```

- [ ] **Step 9.3: Handle the option in the switch**

In the option-parsing switch (starts around line 526), find a logical spot — e.g., after `case OPT_FRAME_COUNT: max_frames = atoi(optarg); break;` (around line 578) — and insert:

```cpp
#ifdef WITH_TIMG_AUDIO
        case OPT_AUDIO: display_opts.audio_enabled = true; break;
#endif
```

- [ ] **Step 9.4: Add the grid / multi-file guard**

Find the block (around line 757) that sets `cell_size_warning_needed = (present.grid_cols > 1);` / forces `present.grid_cols = 1;`. Right after the grid decisions have settled and before `loaded_sources` is filled (around line 947), insert:

```cpp
#ifdef WITH_TIMG_AUDIO
    if (display_opts.audio_enabled &&
        (present.grid_cols > 1 || present.grid_rows > 1 ||
         filelist.size() > 1)) {
        fprintf(stderr,
                "warning: --audio is ignored in grid/multi-file mode\n");
        display_opts.audio_enabled = false;
    }
#endif
```

If you're unsure of the exact location, search for `filelist.size()` to find the right spot:

```bash
grep -n "filelist.size\|loaded_sources" /Users/goya/projects/timg/src/timg.cc | head -10
```

Place the guard immediately after all grid adjustments and before the sources are loaded.

- [ ] **Step 9.5: Add `--audio` to the usage/help text**

Find the `usage` function or help printer (search for `Synopsis` or `usage:` in the file):

```bash
grep -n "usage:\|Synopsis\|fprintf.*stderr.*-p<pix" /Users/goya/projects/timg/src/timg.cc | head -10
```

In the usage text, add a one-line description near the other video-related flags. If the usage is in `timg-help.cc` / generated, follow the same pattern:

```cpp
#ifdef WITH_TIMG_AUDIO
    "\t--audio        : (macOS) Play the audio track of a video. Ignored "
    "in grid / multi-file mode.\n"
#endif
```

If you cannot find a clear spot, add it in the usage function next to an unambiguous neighboring option (e.g. near the `--frames` description).

- [ ] **Step 9.6: Build**

```bash
cd /Users/goya/projects/timg/build && make -j$(sysctl -n hw.ncpu)
```
Expected: Clean build.

- [ ] **Step 9.7: Smoke test — help text**

```bash
/Users/goya/projects/timg/build/src/timg --help 2>&1 | grep -i audio
```
Expected: Line describing `--audio`.

- [ ] **Step 9.8: Smoke test — unknown flag rejected when guard fires**

```bash
/Users/goya/projects/timg/build/src/timg --audio --grid=2 /tmp/timg-sample.mp4 /tmp/timg-sample.mp4 2>&1 | head -3
```
Expected: `warning: --audio is ignored in grid/multi-file mode` appears.

- [ ] **Step 9.9: Smoke test — audio plays for single-file form**

```bash
/Users/goya/projects/timg/build/src/timg --audio /tmp/timg-sample.mp4
```
Expected: Works as in Task 7.

- [ ] **Step 9.10: Commit**

```bash
cd /Users/goya/projects/timg
git add src/timg.cc
git commit -m "Add --audio CLI flag with grid / multi-file guard.

Exposes the audio feature via --audio (macOS only; guarded by
WITH_TIMG_AUDIO). Forces audio off with a warning when combined
with grid mode or multi-file input."
```

---

## Task 10: Final manual verification pass

**Goal:** Walk the full spec checklist. Record results. No code changes unless a defect is found.

**Files:** None (unless defects surface).

### Steps

- [ ] **Step 10.1: Check that all spec checklist items behave as specified**

Run each case from spec §7 and record the result. Short checklist:

1. `timg --audio sample.mp4` — video + audible 440 Hz tone, lip sync ≤ ±50 ms
2. `timg sample.mp4` — video only, no change from current behavior
3. Silent file: `timg --audio silent.gif` — video only, no warning, clean exit
4. Multi-file: `timg --audio a.mp4 b.mp4` — warning, both files play video only
5. Grid: `timg --audio --grid=2 a.mp4 b.mp4` — warning, grid plays video only
6. Loop: `timg --audio --loops=3 clip.mp4` — audio restarts cleanly each time
7. Ctrl-C — audio stops immediately, process exits
8. Unsupported audio codec — one warning, video continues (use an obscure codec file or ffmpeg-synthesize one)
9. `--frames=50` — video and audio end together (audio may cut very slightly early, ≤21 ms)
10. AirPods/device swap between runs — next run plays correctly

- [ ] **Step 10.2: Make sure non-macOS build is unaffected (best-effort check)**

If a Linux VM / remote is available:
```bash
git clone <this feature branch> && cd timg && mkdir build && cd build && cmake .. && make -j
./src/timg --help 2>&1 | grep -i audio   # expect: no output
./src/timg --audio foo.mp4 2>&1 | head -1  # expect: unknown option
```
Skip if no Linux environment is handy — the build wiring already uses `if(APPLE AND WITH_VIDEO_DECODING)` so no audio code is compiled off-Apple, and the CLI flag is `#ifdef`'d out.

- [ ] **Step 10.3: Fix any defect found, committing each fix separately**

For any failure, commit with a message like:

```bash
git commit -m "Fix <specific issue from checklist item N>"
```

- [ ] **Step 10.4: Final commit / tag (optional)**

If everything passes, optionally note completion:

```bash
cd /Users/goya/projects/timg
git log --oneline feature/macos-audio-playback ^main
```
Expected: ~9 feature commits + the spec commit.

---

## Self-Review (plan author's check)

**Spec coverage:**

| Spec section | Implemented by task(s) |
|---|---|
| §2 Opt-in via `--audio` | Task 9 |
| §2 Audio-as-master sync (±50 ms) | Tasks 4, 7 |
| §2 Grid/multi-file forced off with warning | Task 9 |
| §2 Loop supported | Task 8 |
| §2 macOS-only, no flag on other platforms | Tasks 1, 9 (guard by `WITH_TIMG_AUDIO`) |
| §3 Architecture (AudioPlayer + VideoSource extension) | Tasks 1-8 |
| §3 CMake build branch on `APPLE AND WITH_VIDEO_DECODING` | Task 1 |
| §4.1 AudioPlayer public API | Tasks 1 (decl), 3-4 (impl) |
| §4.2 VideoSource new members and lifecycle | Tasks 5, 8 |
| §4.3 CLI flag + grid guard | Task 9 |
| §5 Pre-roll + steady-state loop | Tasks 6, 7 |
| §5 Flush on loop + SIGINT | Task 8 |
| §5 Back-pressure via Feed blocking | Task 4 (Feed spin-sleep on full ring) |
| §6 Silent degradation on errors | Tasks 3, 5 (null AudioPlayer path) |
| §6 One-shot warnings | Task 3 (WarnOnce helper) |
| §6 RAII cleanup | Tasks 3 (Impl dtor), 5 (VideoSource dtor) |
| §7 Manual checklist | Task 10 |

All spec sections have corresponding tasks.

**Placeholder scan:** No "TBD" / "implement later" / "handle edge cases" — all steps have concrete code or exact commands.

**Type consistency:**
- `AudioPlayer::Create(const AVCodecParameters *, AVRational)` — used identically in Tasks 1, 3, 5.
- `AudioPlayer::Feed(const AVFrame *)` — Tasks 1, 4, 6 agree.
- `AudioPlayer::Now()` / `Flush()` / `Start()` / `Stop()` — consistent across Tasks 1, 4, 5, 6, 8.
- `AudioRingBuffer::Write/Read` — one definition (Task 2), one set of callers (Task 4).
- `DisplayOptions::audio_enabled` — defined Task 5, used Task 9.
- `WITH_TIMG_AUDIO` macro — defined Task 1, used Tasks 1, 5, 6, 7, 8, 9.
- `OutputCallback` signature changed between Tasks 3 and 4 (from free function to member). Task 4 calls this out explicitly with step-by-step migration.

**Scope check:** One subsystem (macOS audio playback for video). ≈10 commits, all in a single feature branch. Reasonable for one plan.

No gaps to fix.

---

**Plan ready. Saved to `docs/superpowers/plans/2026-04-23-macos-audio-playback.md`.**
