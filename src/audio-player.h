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
    // Note: if Start() has never been called, the ring fills within
    // ~2 seconds and this call will spin until Stop() runs.
    void Feed(const AVFrame *frame);

    // Drop all buffered audio and reset the playback clock. Called at
    // loop boundaries in VideoSource.
    void Flush();

    // Current audio playback PTS in seconds (same origin as the
    // container's audio stream PTS). Returns a negative value before
    // Start(), after Stop(), or if the audio source has no PTS
    // metadata (callers should treat negative as "unknown" and fall
    // back to their non-audio timing path).
    double Now() const;

    // Begin playback. Idempotent. Call after enough samples have been
    // fed to prevent initial underrun.
    void Start();

    // Stop playback. Safe to call repeatedly.
    void Stop();

private:
    static void OutputCallback(void *user_data, void *queue, void *buffer);
    AudioPlayer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace timg

#endif  // AUDIO_PLAYER_H_
