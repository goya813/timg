// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// CoreAudio (AudioQueue) backed AudioPlayer. macOS only.

#include "audio-player.h"

#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudioTypes.h>

#include <atomic>
#include <cstdio>
#include <cstring>

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
constexpr UInt32 kFramesPerBuffer    = 1024;        // ~21 ms at 48 kHz
constexpr int    kBufferCount        = 4;
constexpr std::size_t kRingCapacityFrames =
    static_cast<std::size_t>(kDeviceSampleRate * 2.0);  // 2 seconds

// Print a message to stderr at most once per call site. The static
// guard is scoped to the enclosing block, so each WARN_ONCE site has
// its own flag and each distinct failure class prints once.
#define WARN_ONCE(msg) do {                                     \
    static std::atomic<bool> timg_audio_warned{false};          \
    if (!timg_audio_warned.exchange(true)) {                    \
        std::fprintf(stderr, "timg audio: %s\n", msg);          \
    }                                                           \
} while (0)

}  // namespace

struct AudioPlayer::Impl {
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

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() = default;

// Stub callback for Task 3: emit silence. Real implementation in Task 4.
static void OutputCallbackStub(void * /*user_data*/,
                               AudioQueueRef queue,
                               AudioQueueBufferRef buffer) {
    std::memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

std::unique_ptr<AudioPlayer> AudioPlayer::Create(
    const AVCodecParameters *codecpar, AVRational /*time_base*/) {
    if (!codecpar) return nullptr;

    if (!avcodec_find_decoder(codecpar->codec_id)) {
        WARN_ONCE("audio codec unsupported, playing silent");
        return nullptr;
    }

    auto player = std::unique_ptr<AudioPlayer>(new AudioPlayer());
    auto &imp = *player->impl_;

    // --- swresample setup ---
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout{};
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
        WARN_ONCE("audio resampler init failed, playing silent");
        return nullptr;
    }

    // --- AudioQueue setup ---
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = kDeviceSampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket   = static_cast<UInt32>(sizeof(float) * kDeviceChannels);
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = static_cast<UInt32>(sizeof(float) * kDeviceChannels);
    fmt.mChannelsPerFrame = static_cast<UInt32>(kDeviceChannels);
    fmt.mBitsPerChannel   = 32;

    OSStatus os = AudioQueueNewOutput(&fmt, &OutputCallbackStub, player.get(),
                                      nullptr, nullptr, 0, &imp.queue);
    if (os != noErr) {
        WARN_ONCE("AudioQueueNewOutput failed, playing silent");
        return nullptr;
    }

    const UInt32 buf_bytes = kFramesPerBuffer * fmt.mBytesPerFrame;
    for (int i = 0; i < kBufferCount; ++i) {
        os = AudioQueueAllocateBuffer(imp.queue, buf_bytes, &imp.buffers[i]);
        if (os != noErr) {
            WARN_ONCE("AudioQueueAllocateBuffer failed, playing silent");
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
    for (int i = 0; i < kBufferCount; ++i) {
        OutputCallbackStub(this, impl_->queue, impl_->buffers[i]);
    }
    if (AudioQueueStart(impl_->queue, nullptr) == noErr) {
        impl_->running.store(true);
    } else {
        WARN_ONCE("AudioQueueStart failed, playing silent");
    }
}

void AudioPlayer::Stop() {
    if (!impl_->queue) return;
    impl_->stopping.store(true);
    AudioQueueStop(impl_->queue, true);
    impl_->running.store(false);
}

}  // namespace timg
