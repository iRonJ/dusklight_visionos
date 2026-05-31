# Audio notes (visionOS port)

Findings and fixes for audio glitches observed when running Dusklight on visionOS
(Apple Vision Pro hardware and the visionOS Simulator). Most of the fixes are
general audio-correctness improvements and are **not** visionOS-gated unless noted.

## How Dusklight audio works

- One SDL3 audio stream opened at **32 kHz / stereo / `SDL_AUDIO_F32`**
  (`DuskDsp.hpp: SampleRate = 32000`, the GameCube DSP rate) via
  `SDL_OpenAudioDeviceStream` with a **pull callback** (`GetNewAudio`).
  SDL3 transparently resamples 32 kHz → the device rate (48 kHz on Apple HW).
- The GameCube DSP mixer runs **on the SDL audio callback thread**
  (`RenderAudioSubframe` → `JASDriver::updateDSP()` + `DspRender()`), rendering
  GC-DSP subframes (`DSP_SUBFRAME_SIZE = 0x50 = 80` samples) and pushing them with
  `SDL_PutAudioStreamData`. Audio is therefore **decoupled from the video frame
  loop** — `getSubFrames()` is a constant, not driven by the game loop.
- Each voice is decoded (ADPCM, etc.) into `decodeBuf` and **resampled to the
  voice's pitch** (`RenderChannel` in `src/dusk/audio/DuskDsp.cpp`).
- Movie/cutscene audio is mixed in via `JASDriver::extMixCallback`
  (`daMP_audioCallbackWithMSound` → `daMP_MixAudio` in
  `src/d/actor/d_a_movie_player.cpp`), `MIX_MODE_INTERLEAVE`.

## Symptoms investigated

1. General audio glitches / crackle on visionOS.
2. Harsh high frequencies — most noticeable on bright/high-pitched SFX
   (cymbals, the lock-on / "focus" targeting sound).
3. Cutscene **sound** stutter (the picture is fine). **Still open** — see below.

## Root causes & fixes

| # | Symptom | Root cause | Fix | File | Gating |
|---|---------|-----------|-----|------|--------|
| 1 | Crackle / underrun | visionOS default CoreAudio IO buffer is small; the DSP mixer runs on the callback thread and competes with the Dawn/Metal render thread | Request a larger device buffer (`SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES = 2048`) before opening the device | `DuskAudioSystem.cpp` (`InitSDL3Output`) | visionOS only (`TARGET_OS_VISION`) |
| 2 | Harsh distortion (esp. cutscenes) | The f32 mix (voices + reverb + HRTF + master volume + the movie track) was summed with **no saturation**, unlike the hardware DSP which saturates at every s16 mix step (`JASDriver::mixInterleaveTrack`). Out-of-range samples were hard-clipped by SDL's resampler / CoreAudio | Saturate the interleaved output to `[-1, 1]` before `SDL_PutAudioStreamData` | `DuskAudioSystem.cpp` (`RenderAudioSubframe`) | all platforms |
| 3 | Harsh highs (broad) | The per-voice pitch resampler used **2-point linear interpolation**, whose piecewise-linear corners fold HF energy into audible distortion | Upgrade to **4-point cubic (Catmull-Rom)** interpolation (added `resamplePrev2` history) | `DuskDsp.cpp` (`RenderChannel`), `DuskDsp.hpp` | all platforms |
| 4 | Harsh high-**pitched** SFX (cymbals, focus/lock-on) | When a voice is pitched up (`step > 1`) the resampler **decimates** the source with no band-limiting → aliasing. Cubic interpolation improves accuracy but does **not** anti-alias decimation | **Anti-aliasing low-pass before decimation**: two cascaded one-poles (~12 dB/oct), cutoff `≈ 0.45/step`, applied only to freshly-decoded samples when `step > 1`, state carried across subframes | `DuskDsp.cpp` (`RenderChannel`), `DuskDsp.hpp` (`aaLp1`/`aaLp2`) | all platforms (engages only on pitch-up) |
| 5 | **Cutscene audio + video both stutter** (video stutters even at a steady, locked 120 fps) | **The bytes-vs-frames unit bug.** `GetNewAudio`'s `needed` is in **bytes**, but `RenderNewAudioFrame` returned a **sample-frame count** (`DSP_SUBFRAME_SIZE`, ~8× too small). Each audio callback therefore rendered ~8× too many subframes and called the movie mix callback ~8× too often, **draining the movie player's decoded-audio queue ~8× faster than the THP thread decodes it** → near-constant underflow → silence/stutter. Because THP video is **synced to the audio clock**, the video stuttered with it. (Confirmed by instrumentation: **19,396 underflow events / 1.53M samples silenced** in one cutscene → **0** after the fix.) | Return **bytes pushed** (`countSubframes * sizeof(OutInterleaveBuffer)`) so the callback consumes at the correct rate | `DuskAudioSystem.cpp` (`RenderNewAudioFrame`) | all platforms |
| 6 | Cutscene video interpolation judder | Frame interpolation kept running during movie playback (the `STBWAIT` cut action wasn't in the camera's presentation-sync list, `d_camera.cpp` ~10435), interpolating pre-rendered THP video and contending with the decode threads | Request presentation sync (which disables interpolation) **every frame the movie draws** | `d_a_movie_player.cpp` (`daMP_ActivePlayer_Draw`) | visionOS / PC (`TARGET_PC`) |

Listening results: #1 reduced general crackle; #2 helped harshness "a tad"
(confirming clipping was a minor contributor); #3 made the highs "better"; #4
removed the residual pitched-up-SFX aliasing (the focus/lock-on sound); **#5 (with
#6) eliminated the cutscene audio *and* video stutter entirely** — user-confirmed
"no audio issues and video anymore".

### Notes / interactions

- Catmull-Rom (#3) can overshoot slightly past the source range; the output
  saturation clamp (#2) bounds that, so it introduces no new clipping.
- #4 is a **quality enhancement over the hardware DSP** (the real GameCube uses
  linear interpolation), traded deliberately for cleaner highs. It is cheap
  (~2 mults per pitched-up source sample) and only runs on pitch-up, so it should
  not pressure the audio-callback deadline.

## Further options to address residual harshness

If bright/high-pitched SFX are still harsh after fix #4, in rough order of
effort/impact (all in `RenderChannel`, `DuskDsp.cpp`):

1. **Lower the anti-alias cutoff** — change `0.45f / step` toward `0.40f / step`
   (or lower). Trades a little brightness for less sizzle. One-number change.
2. **Steeper roll-off** — add a 3rd/4th cascaded one-pole, or replace the cascade
   with a true **2nd-order Butterworth biquad** (maximally flat passband, 12 dB/oct
   without the cascade's passband droop). More aliasing rejection per octave.
3. **Windowed-sinc / Lanczos resampler** with a cutoff that scales by `1/step` — the
   textbook anti-aliased sampler. Best quality; higher CPU (N taps per output
   sample). Watch the audio-callback deadline (see cutscene stutter below) — gate
   the expensive path to `step > 1` and keep taps modest (e.g. Lanczos-3 = 6 taps).
4. **Band-limited oscillators (PolyBLEP)** — *if* any harsh sound turns out to be a
   synthesized square/saw voice (`RenderOscChannel`, `OscType::SQUARE_*`/`SAW_WAVE`)
   rather than a sample, the naive oscillator formulas alias badly and need
   band-limiting. (Confirm first: cubic helping implies the sample path, but some
   SFX use oscillator channels.)
5. **Dynamic rate control** — read the actual device output rate and nudge the
   32 kHz→device resample ratio to hold the SDL buffer ~half full, eliminating slow
   drift underruns (see the deep-research notes: byuu/Near, libretro DRC).
6. **Real-time audio scheduling** — configure `AVAudioSession` explicitly (category,
   preferred 48 kHz, IO buffer duration before activation) and join the CoreAudio
   **audio workgroup** (`os_workgroup` / `AudioWorkInterval`) from the mixer thread so
   the callback reliably meets its deadline under render load.

## Cutscene audio + video stutter — RESOLVED

Both the cutscene **sound** and the **picture** stuttered (the picture juddered even
at a steady, locked 120 fps). Resolved by fixes **#5** and **#6** above.

How it was diagnosed (the symptoms were misleading — steady fps but stuttering A/V):

1. Ruled out call granularity: `daMP_MixAudio` consumes exactly the requested sample
   count, so per-subframe vs per-frame calls were not the bug.
2. Added a passive **underflow counter** on the movie audio silence path
   (`d_a_movie_player.cpp`, `daMP_MixAudio` → the `memset(dst, 0, …)` path), incremented
   cheaply on the audio thread and summarized on the game thread at movie audio
   start/stop (`daMP_audioInitWithMSound` / `daMP_audioQuitWithMSound`):
   `[movie-audio] mixing ended: N underflow events, M samples silenced`.
   That instrumentation is still in the code (TARGET_PC only) for future diagnosis.
3. A device run reported **19,396 underflow events / 1,528,394 samples silenced** in one
   cutscene → the movie audio queue was starved almost continuously.
4. Root cause: the **bytes-vs-frames unit bug** (fix #5) caused ~8× over-consumption of
   the movie audio queue. Key realization: **THP video is synced to the audio clock**, so
   the starved audio dragged the video into stutter too — which is why a frame-pacing
   fix alone (#6) didn't resolve the picture; the audio fix did.
5. After fix #5 (+#6): user-confirmed **no audio or video stutter**. (Re-run the build to
   see the underflow count fall to ~0.)

If cutscene underflows ever return (e.g. on slower hardware), the next levers are larger
THP audio-decode buffering and/or raising the THP decoder-thread priority.

## Verification & build

These fixes cannot be auto-verified (no audio capture in the build/test loop);
they are verified by listening on device/simulator. For build, signing, and
deployment of the visionOS port, see the
[visionOS section of CLAUDE.md](CLAUDE.md#apple-vision-visionos-port).

Background research that informed the diagnosis (SDL3/CoreAudio buffer & sample-rate
negotiation, audio workgroups, resampling/underrun mechanics): see the cited sources
in the project's deep-research output.
