# Audio Playback for Video Files (macOS only)

**Date:** 2026-04-23
**Status:** Approved design, ready for implementation planning
**Scope:** timg — add synchronised audio playback alongside existing video rendering, macOS only

## 1. Background

timg renders videos frame-by-frame to the terminal via ffmpeg/libav but discards all non-video streams. The codebase carries an explicit `// TODO: sound output` marker at `src/video-source.cc:16-17`. The user wants to play the audio track as well, accepting that the feature ships for macOS only to keep scope and code volume small.

## 2. Requirements

### Functional

- Audio playback works for local video files whose container has an audio stream ffmpeg can decode.
- Audio is opt-in via a new long CLI flag `--audio` (no short form — `-a` is already taken by antialias-off at `src/timg.cc:579`).
- Audio is kept approximately in sync with video via **audio-as-master** clock: the video rendering path queries the current audio playback position and waits/drops frames to match.
- Sync accuracy target: lip-sync within ±50 ms on a normal speech clip (not professional-grade). This matches selection **B** ("practical sync") from brainstorming.
- When audio cannot be played (missing stream, unsupported codec, device failure), playback degrades silently to video-only; an optional one-shot warning is printed to stderr.

### Non-functional / out of scope

- macOS only. No Linux/Windows/BSD audio backend. The flag does not exist on non-Apple builds.
- No volume control flag (system volume applies).
- No stall/underrun recovery for network streams (MVP accepts brief audio dropouts).
- Grid mode (`--grid=`) and multi-file batches are **not** supported: `--audio` is silently forced off with a warning. Only a single video at a time plays audio.
- Loop mode (`--loops=`) is supported — audio restarts at each loop.

## 3. Architecture

One new component, `AudioPlayer`, wraps CoreAudio's `AudioQueue` API. `VideoSource` is extended to demux and decode audio packets in the same loop it already runs for video, feeding decoded audio frames to `AudioPlayer`. The video rendering pacing in `VideoSource::SendFrames()` is switched from the current frame-count × frame-duration model to querying the audio clock (`AudioPlayer::Now()`) when audio is active.

```
┌─────────────────┐
│  timg.cc (CLI)  │  parse --audio; force off for grid / multi-file
└────────┬────────┘
         │ audio_enabled flag
┌────────▼────────────────────────────────────────┐
│  VideoSource (existing, extended)               │
│   LoadAndScale():                               │
│     - find_best_stream(AUDIO) if enabled        │
│     - open audio decoder                        │
│     - AudioPlayer::Create(...)                  │
│   SendFrames():                                 │
│     demux loop                                  │
│       audio packet → decode → AudioPlayer::Feed │
│       video packet → decode → wait for audio    │
│                       clock → emit frame        │
└────────┬────────────────────────────────────────┘
         │ decoded AVFrame (audio)
┌────────▼────────────────────────────┐
│  AudioPlayer (new, macOS only)      │
│   - SwrContext (ffmpeg → device fmt)│
│   - Ring buffer (SPSC atomic)       │
│   - AudioQueue callback → device    │
│   - Now(): current playback PTS     │
└────────┬────────────────────────────┘
         │
    CoreAudio / AudioToolbox.framework
```

### Build configuration

- macOS only. Build system detects Apple via `if(APPLE AND WITH_VIDEO_DECODING)` in CMake.
- Define `TIMG_AUDIO_SUPPORT` preprocessor macro on that branch.
- `audio-player-macos.cc` is compiled only on that branch.
- `AudioToolbox.framework` is linked only on that branch.
- The header `audio-player.h` itself can be included unconditionally; all public methods are compiled out to no-op when `TIMG_AUDIO_SUPPORT` is undefined, or the header is simply not included from non-macOS code.
- On non-macOS builds, the `--audio` flag is not added to the option parser — `getopt` will reject it as unknown.

## 4. Components

### 4.1 `AudioPlayer` (new)

File: `src/audio-player.h`, `src/audio-player-macos.cc`

```cpp
namespace timg {
class AudioPlayer {
 public:
  // Factory. Returns nullptr if the stream cannot be played (unsupported
  // codec, resampler init failure, AudioQueue creation failure). Never throws.
  static std::unique_ptr<AudioPlayer> Create(
      const AVCodecParameters *codecpar, AVRational time_base);
  ~AudioPlayer();

  // Feed a decoded audio AVFrame. Callee does not take ownership.
  // Internally resamples to the device format and writes to the ring buffer.
  // Blocks when the ring buffer is full (back-pressure mechanism).
  void Feed(const AVFrame *frame);

  // Drop all buffered audio. Called at loop boundaries.
  void Flush();

  // Playback PTS (seconds, same origin as video stream PTS).
  // Returns a negative value before Start() / after Stop().
  double Now() const;

  // Begin playback after pre-roll. Idempotent.
  void Start();

  // Stop playback and drain.
  void Stop();
};
}  // namespace timg
```

Internal details (macOS):
- Device format: 48 kHz, stereo, float32 interleaved.
- `AudioQueueNewOutput` with 4 buffers × 1024 frames ≈ 21 ms per buffer.
- Each enqueued buffer carries the PTS of its first sample in `AudioQueueBufferRef::mUserData`.
- The AudioQueue callback (invoked on an OS-managed thread) publishes the PTS of the buffer about to play via `std::atomic<double> played_pts_`.
- Ring buffer: single-producer (`Feed` from demux thread) / single-consumer (AudioQueue callback); lock-free using two atomic indices.

### 4.2 `VideoSource` changes

File: `src/video-source.{h,cc}`

New members:
- `int audio_stream_index_ = -1;`
- `AVCodecContext *audio_codec_context_ = nullptr;`
- `std::unique_ptr<AudioPlayer> audio_player_;`
- `bool audio_enabled_ = false;` (set via existing options path)

`LoadAndScale()` — after the existing video-stream scan, if `audio_enabled_`:
- `av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, ...)` to locate the audio track
- Open the audio decoder with `avcodec_find_decoder` + `avcodec_open2`
- Construct `AudioPlayer::Create(codecpar, time_base)`. If null, clear `audio_enabled_` (video-only fallback)

`SendFrames()` — see §5 for the loop.

Destructor: `avcodec_free_context(&audio_codec_context_)` in addition to existing cleanup.

### 4.3 `timg.cc` (CLI) changes

Guarded by `#if defined(TIMG_AUDIO_SUPPORT)`:
- Add `--audio` (long option only) to the option table and usage text (one-line description). Do not attach a short form; `-a` is already used for `antialias=false`.
- Plumb the flag into the options struct passed to `VideoSource`.
- After option parsing, if (`grid cols > 1 OR grid rows > 1 OR input file count > 1`) AND `audio_enabled`: print `warning: --audio is ignored in grid/multi-file mode` to stderr and force `audio_enabled = false`.

## 5. Data flow and synchronisation

### Startup

```
1. VideoSource::LoadAndScale()
   ├─ detect video stream (existing)
   └─ if audio_enabled_: detect audio stream → open decoder → AudioPlayer::Create()

2. SendFrames()
   ├─ pre-roll: read packets until AudioPlayer has ≥ ~200 ms of PCM buffered
   ├─ AudioPlayer::Start() — audio clock begins here
   └─ enter steady-state loop
```

### Steady-state loop (pseudocode)

```cpp
while (av_read_frame(fmt_ctx, pkt) >= 0 && !interrupt_received) {
  if (pkt->stream_index == audio_stream_index_) {
    avcodec_send_packet(audio_codec_ctx_, pkt);
    while (avcodec_receive_frame(audio_codec_ctx_, aframe) == 0) {
      audio_player_->Feed(aframe);   // resample + enqueue; blocks if full
    }
  } else if (pkt->stream_index == video_stream_index_) {
    avcodec_send_packet(video_codec_ctx_, pkt);
    while (avcodec_receive_frame(video_codec_ctx_, vframe) == 0) {
      const double frame_pts =
          vframe->best_effort_timestamp * av_q2d(video_stream->time_base);

      if (audio_player_) {
        const double wait = frame_pts - audio_player_->Now();
        if (wait < -0.1) continue;     // ≥100 ms late → drop
        if (wait > 0.5)  Time::Sleep(0.5);  // cap
        else if (wait > 0) Time::Sleep(wait);
      } else {
        // existing frame-duration pacing (unchanged path)
      }
      SwsScale → SendFrameToRenderer(vframe);
    }
  }
  av_packet_unref(pkt);
}
```

### Clock publication

```
Feed():
  swr_convert(frame) → ring buffer with PTS tag per chunk

AudioQueue callback (OS thread):
  - take next output buffer
  - copy samples from ring; record the PTS of the first sample
  - publish: played_pts_.store(pts, memory_order_release)
  - AudioQueueEnqueueBuffer(...)

Now():
  return played_pts_.load(memory_order_acquire);
```

Resolution: `Now()` is accurate to ≈1 audio buffer (≈21 ms) — sufficient for the ±50 ms lip-sync target.

### Flush / loop / SIGINT

- **Loop** (existing `av_seek_frame` + `avcodec_flush_buffers`): additionally call `avcodec_flush_buffers(audio_codec_ctx_)` and `AudioPlayer::Flush()`; `AudioPlayer::Start()` is idempotent and stays as-is.
- **SIGINT** (`interrupt_received`): main loop exits, destructor of `VideoSource` runs, which destroys `AudioPlayer` — its destructor calls `AudioQueueStop(queue, true)` then `AudioQueueDispose`.

### Back-pressure

`Feed()` blocks when the ring buffer is full. The demux loop therefore pauses naturally when the consumer (AudioQueue) is behind, which also throttles video decoding — preventing unbounded memory growth if a video stream has sparse audio or vice versa.

## 6. Error handling

All audio failures degrade to **silent video playback**. The video path is never broken by audio errors.

| Where | Condition | Behaviour |
|---|---|---|
| `AudioPlayer::Create` | no audio stream in container | return `nullptr` (normal, no warning) |
| `AudioPlayer::Create` | codec not supported by ffmpeg | return `nullptr` + one-shot `warning: audio codec unsupported, playing silent` to stderr |
| `AudioPlayer::Create` | `AudioQueueNewOutput` fails | return `nullptr` + warning |
| `AudioPlayer::Create` | `swr_init` fails | return `nullptr` + warning |
| `Feed` | transient decode / resample error | skip that packet/frame, continue |
| `Start` | `AudioQueueStart` fails | release the player; `VideoSource` clears `audio_player_` and continues video-only |

CLI-level validation:
- `--audio` + grid or multi-file → warn + force off (§4.3)
- `--audio` on non-macOS build → unknown option (getopt default behaviour)

Logging rules:
- Default quiet: audio-less files produce no warning even with `--audio` on.
- Each distinct warning class prints at most once per process (reset on new file is not required for MVP).
- Inherit existing `av_log_set_level(AV_LOG_ERROR)` for ffmpeg-internal messages.

Resource cleanup is strictly RAII: `AudioPlayer` destructor stops & disposes the queue, frees `SwrContext`, releases the ring buffer.

## 7. Testing

No existing unit test infrastructure — CMake has no `add_test` wiring. Strategy matches that: **build gates + manual checklist.**

### Automated (CI / local)

- **macOS (Apple Silicon) — `WITH_VIDEO_DECODING=ON`**: builds, `audio-player-macos.cc` compiles, `AudioToolbox.framework` linked, binary has `--audio` in `--help`.
- **macOS — `WITH_VIDEO_DECODING=OFF`**: builds, no audio code compiled.
- **Ubuntu CI (`.github/workflows/ubuntu.yml`)**: builds unchanged, `timg --help` has no `--audio` entry, running with `--audio` prints unknown-option error.

### Manual checklist (run before declaring done)

| # | Scenario | Expected |
|---|---|---|
| 1 | `timg --audio speech.mp4` | video + audio, lip-sync within ≈50 ms |
| 2 | `timg speech.mp4` (no flag) | video only, no change from current behaviour |
| 3 | `timg --audio silent.gif` (no audio stream) | video only, no warning, clean exit |
| 4 | `timg --audio a.mp4 b.mp4` | warning printed; both files play video only |
| 5 | `timg --audio --grid=2 a.mp4 b.mp4` | warning printed; grid plays video only |
| 6 | `timg --audio --loops=3 clip.mp4` | audio restarts cleanly at each loop, minimal click |
| 7 | Ctrl-C during playback | audio stops immediately, process exits cleanly |
| 8 | video with codec ffmpeg cannot decode for audio | one warning, video continues |
| 9 | `timg --audio --frames=100 clip.mp4` | video and audio end together |
| 10 | Switch output device (e.g. AirPods) between runs | next run plays correctly |

### Out-of-scope for verification

- Intel Mac (target is Apple Silicon dev machine)
- Network streams (stall/underrun)
- Sample rates < 8 kHz or > 96 kHz
- Output device switching *during* a single playback

## 8. Open questions

None.

## 9. References

- Existing TODO: `src/video-source.cc:16-17`
- Current video demux loop: `src/video-source.cc` `SendFrames()` (lines ~268-370)
- ffmpeg build wiring: `CMakeLists.txt:93-105`
- Frame timing: `src/renderer.cc:63`, `src/timg-time.h:146`
