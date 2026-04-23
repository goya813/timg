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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

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

    // Clock: Now() = base_pts + played_frames / kDeviceSampleRate.
    // base_pts is the PTS of the first audio sample ever fed (seconds).
    // played_frames is the cumulative frame count delivered to CoreAudio.
    std::atomic<double> base_pts{-1.0};
    std::atomic<uint64_t> played_frames{0};
    AVRational time_base{0, 1};       // copied from audio stream

    // Per-buffer scratch for resampling. Interleaved float32.
    std::vector<float> resample_scratch;

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

void AudioPlayer::OutputCallback(void *user_data,
                                 void *queue_opaque,
                                 void *buffer_opaque) {
    auto *self   = static_cast<AudioPlayer *>(user_data);
    auto  queue  = static_cast<AudioQueueRef>(queue_opaque);
    auto  buffer = static_cast<AudioQueueBufferRef>(buffer_opaque);
    auto &imp    = *self->impl_;

    float *out = static_cast<float *>(buffer->mAudioData);
    const std::size_t want =
        buffer->mAudioDataBytesCapacity / (sizeof(float) * kDeviceChannels);
    const std::size_t got = imp.ring.Read(out, want);
    if (got < want) {
        std::memset(out + got * kDeviceChannels, 0,
                    (want - got) * sizeof(float) * kDeviceChannels);
    }
    buffer->mAudioDataByteSize =
        static_cast<UInt32>(want * sizeof(float) * kDeviceChannels);

    // Advance by full buffer size: CoreAudio plays `want` samples in real
    // time even when part is zero-filled underrun padding.
    imp.played_frames.fetch_add(want, std::memory_order_release);

    if (!imp.stopping.load(std::memory_order_relaxed)) {
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
}

std::unique_ptr<AudioPlayer> AudioPlayer::Create(
    const AVCodecParameters *codecpar, AVRational time_base) {
    if (!codecpar) return nullptr;

    if (!avcodec_find_decoder(codecpar->codec_id)) {
        WARN_ONCE("audio codec unsupported, playing silent");
        return nullptr;
    }

    auto player = std::unique_ptr<AudioPlayer>(new AudioPlayer());
    auto &imp = *player->impl_;
    imp.time_base = time_base;

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

    OSStatus os = AudioQueueNewOutput(
        &fmt,
        reinterpret_cast<AudioQueueOutputCallback>(&AudioPlayer::OutputCallback),
        player.get(),
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

void AudioPlayer::Feed(const AVFrame *frame) {
    if (!impl_->swr || !frame) return;

    // Seed base_pts from the first frame we see.
    if (impl_->base_pts.load(std::memory_order_relaxed) < 0.0 &&
        frame->pts != AV_NOPTS_VALUE) {
        const double pts_sec = frame->pts * av_q2d(impl_->time_base);
        impl_->base_pts.store(pts_sec, std::memory_order_release);
    }

    // Worst-case output frame count for this input.
    const int64_t delay = swr_get_delay(impl_->swr, frame->sample_rate);
    const int64_t max_out_64 =
        av_rescale_rnd(delay + frame->nb_samples,
                       static_cast<int64_t>(kDeviceSampleRate),
                       frame->sample_rate, AV_ROUND_UP);
    if (max_out_64 <= 0) return;
    const int max_out = static_cast<int>(max_out_64);

    auto &scratch = impl_->resample_scratch;
    const std::size_t need = static_cast<std::size_t>(max_out) * kDeviceChannels;
    if (scratch.size() < need) scratch.resize(need);

    uint8_t *out_ptrs[1] = {reinterpret_cast<uint8_t *>(scratch.data())};
    const int out_frames = swr_convert(
        impl_->swr, out_ptrs, max_out,
        const_cast<const uint8_t **>(frame->data), frame->nb_samples);
    if (out_frames <= 0) return;

    // Write to ring; spin-sleep while full. Back-pressure throttles the
    // demux loop so the ring never grows unboundedly.
    std::size_t written = 0;
    while (written < static_cast<std::size_t>(out_frames)) {
        const std::size_t w = impl_->ring.Write(
            scratch.data() + written * kDeviceChannels,
            static_cast<std::size_t>(out_frames) - written);
        written += w;
        if (written < static_cast<std::size_t>(out_frames)) {
            if (impl_->stopping.load(std::memory_order_relaxed)) return;
            struct timespec ts = {0, 20 * 1000 * 1000};  // 20 ms
            nanosleep(&ts, nullptr);
        }
    }
}

void AudioPlayer::Flush() {
    if (!impl_->queue) return;
    AudioQueueFlush(impl_->queue);
    impl_->ring.Clear();
    impl_->played_frames.store(0, std::memory_order_release);
    impl_->base_pts.store(-1.0, std::memory_order_release);
}

double AudioPlayer::Now() const {
    const double base = impl_->base_pts.load(std::memory_order_acquire);
    if (base < 0.0) return -1.0;
    const uint64_t frames = impl_->played_frames.load(std::memory_order_acquire);
    return base + static_cast<double>(frames) / kDeviceSampleRate;
}

void AudioPlayer::Start() {
    if (!impl_->queue || impl_->running.load()) return;
    for (int i = 0; i < kBufferCount; ++i) {
        AudioPlayer::OutputCallback(this, impl_->queue, impl_->buffers[i]);
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
