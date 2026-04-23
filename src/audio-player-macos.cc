// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// CoreAudio (AudioQueue) backed AudioPlayer. macOS only.

#include "audio-player.h"
#include "audio-ring-buffer.h"

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
