// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2026 timg authors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
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

    // Number of frames currently readable. Safe to call from either side;
    // both atomic loads use acquire order so the producer side also gets
    // an up-to-date view of the consumer's progress.
    std::size_t ReadableFrames() const {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
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
