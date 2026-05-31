#include "dusk/audio/DuskAudioSystem.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_hints.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <span>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "JSystem/JAudio2/JASAiCtrl.h"
#include "JSystem/JAudio2/JASChannel.h"
#include "JSystem/JAudio2/JASCriticalSection.h"
#include "JSystem/JAudio2/JASDSPChannel.h"
#include "JSystem/JAudio2/JASHeapCtrl.h"

#include "DuskDsp.hpp"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JASDriverIF.h"
#include "tracy/Tracy.hpp"

using namespace dusk::audio;

static OutputSubframe OutBuffer;
static std::array<f32, DSP_SUBFRAME_SIZE * OutputSubframe::NUM_CHANNELS> OutInterleaveBuffer;

static SDL_AudioStream* PlaybackStream;

/**
 * SDL audiostream callback to trigger rendering of new audio data.
 */
static void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int);

/**
 * Render an entire new frame of audio and output it to SDL3.
 * Note: "audio frames" are unrelated to video frames.
 * @return Amount of audio samples rendered.
 */
static int RenderNewAudioFrame();

/**
 * Render an audio subframe and output it to SDL3.
 */
static void RenderAudioSubframe();

static void InitSDL3Output() {
#if defined(__APPLE__) && defined(TARGET_OS_VISION) && TARGET_OS_VISION
    // visionOS's default CoreAudio IO buffer is small, and the GameCube DSP
    // mixer runs on the SDL audio callback thread (rendering a full audio frame
    // per call) while competing with the Dawn/Metal render thread. Under that
    // contention the small buffer underruns periodically -> audible crackle /
    // dropouts. Request a larger device buffer to add scheduling slack. Must be
    // set before the audio device is opened. visionOS-only; other platforms keep
    // their existing (lower-latency) behavior.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
#endif
    SDL_Init(SDL_INIT_AUDIO);

    constexpr SDL_AudioSpec spec = {
        SDL_AUDIO_F32,
        2,
        SampleRate,
    };
    PlaybackStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        &GetNewAudio,
        nullptr);
}

void dusk::audio::Initialize() {
    InitSDL3Output();
    DspInit();

    JASDsp::initBuffer();
    JASDSPChannel::initAll();

    JASPoolAllocObject_MultiThreaded<JASChannel>::newMemPool(0x48);

    SDL_ResumeAudioStreamDevice(PlaybackStream);
}

void dusk::audio::SetMasterVolume(const f32 value) {
    JASCriticalSection section;

    MasterVolume = value;
}

void dusk::audio::SetPaused(const bool paused) {
    if (paused) {
        SDL_PauseAudioStreamDevice(PlaybackStream);
    } else {
        SDL_ResumeAudioStreamDevice(PlaybackStream);
    }
}

void dusk::audio::SetEnableReverb(const bool value) {
    JASCriticalSection section;

    EnableReverb = value;
}

#ifdef TRACY_ENABLE
static auto FrameName = "GetNewAudio";
#endif

void SDLCALL GetNewAudio(
    void*,
    SDL_AudioStream*,
    int needed,
    int) {
    FrameMarkStart(FrameName);
    while (needed > 0) {
        const int rendered = RenderNewAudioFrame();
        needed -= rendered;
    }
    FrameMarkEnd(FrameName);
}

int RenderNewAudioFrame() {
    ZoneScoped;
    JASCriticalSection section;
    const u32 countSubframes = JASDriver::getSubFrames();

    JASAudioThread::setDSPSyncCount(countSubframes);

    for (u32 i = 0; i < countSubframes; i++) {
        RenderAudioSubframe();

        JASAudioThread::snIntCount -= 1;
    }

    // Return the number of BYTES pushed to the stream this call. SDL reports
    // `needed` (in GetNewAudio) in bytes, and each subframe pushes
    // sizeof(OutInterleaveBuffer) bytes (DSP_SUBFRAME_SIZE * NUM_CHANNELS f32s).
    // This previously returned a sample-frame count (DSP_SUBFRAME_SIZE), i.e. 8x
    // too small, so GetNewAudio looped ~8x too long -- over-producing and, during
    // cutscenes, draining the movie player's decoded-audio queue ~8x faster than
    // it is decoded. That starved the queue (tens of thousands of underflow events
    // -> silence), which stuttered the cutscene audio and, because the THP video is
    // synced to the audio clock, stuttered the video with it.
    return static_cast<int>(countSubframes) * static_cast<int>(sizeof(OutInterleaveBuffer));
}

static void InterleaveOutputData(const OutputSubframe& data, std::span<f32> target) {
    assert(target.size() >= data.channels[0].size() * OutputSubframe::NUM_CHANNELS);

    size_t outPos = 0;
    for (size_t inPos = 0; inPos < data.channels[0].size(); inPos++) {
        for (size_t channelIdx = 0; channelIdx < OutputSubframe::NUM_CHANNELS; channelIdx++) {
            target[outPos++] = data.channels[channelIdx][inPos];
        }
    }
}

void RenderAudioSubframe() {
    ZoneScoped;
    OutBuffer = {};

    JASDriver::updateDSP();
    DspRender(OutBuffer);

    InterleaveOutputData(OutBuffer, OutInterleaveBuffer);

    if (JASDriver::extMixCallback != nullptr && JASDriver::sMixMode == MIX_MODE_INTERLEAVE) {
        static_assert(OutputSubframe::NUM_CHANNELS == 2); // This code only works with Stereo so far.
        // NOTE: In the real game, this gets called on the entire audio frame, rather than the subframe.
        // That's probably more efficient, but I didn't wanna change the code to calculate the
        // entire audio buffers at once.
        // This is only used for the movie player, and it seems to work fine with the smaller calls.
        const auto mixData = JASDriver::extMixCallback(DSP_SUBFRAME_SIZE);
        if (mixData) {
            for (int i = 0; i < OutInterleaveBuffer.size(); i++) {
                OutInterleaveBuffer[i] += static_cast<f32>(mixData[i]) / static_cast<f32>(0x7FFF);
            }
        }
    }

    // Saturate to [-1, 1] before handing the buffer to SDL's resampler / CoreAudio.
    // The hardware DSP mixer saturates at every s16 mix step (see JASDriver::
    // mixInterleaveTrack), but this f32 reimplementation sums voices, reverb, HRTF
    // and the movie-player track (added above) without clamping, so the result can
    // exceed unity. Out-of-range samples are hard-clipped downstream, producing
    // harsh high-frequency distortion (and audibly distorted cutscene audio, where
    // the unclamped movie track is mixed in). Clamping here matches the hardware's
    // saturating behavior and keeps the resampler input in range.
    for (f32& sample : OutInterleaveBuffer) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }

    SDL_PutAudioStreamData(PlaybackStream, &OutInterleaveBuffer, sizeof(OutInterleaveBuffer));
}

u32 dusk::audio::GetResetCount(int channelIdx) {
    return ChannelAux[channelIdx].resetCount;
}

f32 dusk::audio::VolumeFromU16(u16 value) {
    return static_cast<f32>(value) / static_cast<f32>(JASDriver::getChannelLevel_dsp());
}
