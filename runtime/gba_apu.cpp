#include "gba_apu.h"
#include "gba_bus.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
#include <SDL2/SDL.h>
#endif

#include <algorithm>
#include <cstring>

namespace gba {

#ifdef _WIN32
struct GbaApu::WaveBuf {
    WAVEHDR hdr{};
    std::array<int16_t, kSamplesPerBuffer * 2> data{};
    bool prepared = false;
};
#else
struct GbaApu::WaveBuf {
    uint32_t sdlAudioDev = 0;
};
#endif

GbaApu::GbaApu() {
    staging_.reserve(kSamplesPerBuffer * 8);
}

GbaApu::~GbaApu() {
    ShutdownAudioOutput();
}

bool GbaApu::InitAudioOutput() {
#ifdef _WIN32
    if (hWaveOut_) return true;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = kSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        return false;
    }

    hWaveOut_ = hwo;
    waveBufs_ = new WaveBuf[kNumWaveBuffers];
    for (int i = 0; i < kNumWaveBuffers; ++i) {
        waveBufs_[i].hdr.lpData = reinterpret_cast<LPSTR>(waveBufs_[i].data.data());
        waveBufs_[i].hdr.dwBufferLength = static_cast<DWORD>(waveBufs_[i].data.size() * sizeof(int16_t));
        waveBufs_[i].hdr.dwFlags = WHDR_DONE;
    }
    nextWaveBuf_ = 0;

    // Pre-fill 2 silent buffers so playback starts smoothly without an initial underrun click
    for (int i = 0; i < 2; ++i) {
        WaveBuf& wb = waveBufs_[nextWaveBuf_];
        wb.data.fill(0);
        wb.hdr.dwFlags = 0;
        waveOutPrepareHeader(hwo, &wb.hdr, sizeof(WAVEHDR));
        wb.prepared = true;
        waveOutWrite(hwo, &wb.hdr, sizeof(WAVEHDR));
        nextWaveBuf_ = (nextWaveBuf_ + 1) % kNumWaveBuffers;
    }
    return true;
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (waveBufs_) return true;
    SDL_AudioSpec want{}, have{};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kSamplesPerBuffer;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) return false;
    waveBufs_ = new WaveBuf{};
    waveBufs_->sdlAudioDev = dev;
    SDL_PauseAudioDevice(dev, 0);
    return true;
#else
    return false;
#endif
}

void GbaApu::ShutdownAudioOutput() {
#ifdef _WIN32
    if (!hWaveOut_) return;
    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(hWaveOut_);
    waveOutReset(hwo);
    if (waveBufs_) {
        for (int i = 0; i < kNumWaveBuffers; ++i) {
            if (waveBufs_[i].prepared) {
                waveOutUnprepareHeader(hwo, &waveBufs_[i].hdr, sizeof(WAVEHDR));
            }
        }
        delete[] waveBufs_;
        waveBufs_ = nullptr;
    }
    waveOutClose(hwo);
    hWaveOut_ = nullptr;
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (waveBufs_) {
        if (waveBufs_->sdlAudioDev) SDL_CloseAudioDevice(waveBufs_->sdlAudioDev);
        delete waveBufs_;
        waveBufs_ = nullptr;
    }
#endif
}

void GbaApu::Reset() {
    fifoA_.HardReset();
    fifoB_.HardReset();
    lpLeft_ = lpRight_ = 0.0f;
    dcPrevInL_ = dcPrevOutL_ = 0.0f;
    dcPrevInR_ = dcPrevOutR_ = 0.0f;
    cycleAccum_ = 0.0;
    drcRateScale_ = 1.0;
    staging_.clear();
}

void GbaApu::OnPaused() {
#ifdef _WIN32
    // Keep waveOut fed with gentle silence while paused so resuming never clicks
    if (!hWaveOut_ || !waveBufs_) return;
    int queued = CountQueuedBuffers();
    if (queued < 2) {
        HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(hWaveOut_);
        WaveBuf& wb = waveBufs_[nextWaveBuf_];
        if (!wb.prepared || (wb.hdr.dwFlags & WHDR_DONE)) {
            if (wb.prepared) {
                waveOutUnprepareHeader(hwo, &wb.hdr, sizeof(WAVEHDR));
                wb.prepared = false;
            }
            wb.data.fill(0);
            wb.hdr.dwFlags = 0;
            waveOutPrepareHeader(hwo, &wb.hdr, sizeof(WAVEHDR));
            wb.prepared = true;
            waveOutWrite(hwo, &wb.hdr, sizeof(WAVEHDR));
            nextWaveBuf_ = (nextWaveBuf_ + 1) % kNumWaveBuffers;
        }
    }
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (waveBufs_ && waveBufs_->sdlAudioDev) {
        if (SDL_GetQueuedAudioSize(waveBufs_->sdlAudioDev) < kSamplesPerBuffer * 4) {
            std::array<int16_t, kSamplesPerBuffer * 2> silence{};
            SDL_QueueAudio(waveBufs_->sdlAudioDev, silence.data(), static_cast<Uint32>(silence.size() * sizeof(int16_t)));
        }
    }
#endif
}

void GbaApu::SetVolume(float volume, bool muted) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    muted_ = muted;
}

void GbaApu::PushFifoA(uint32_t word) {
    fifoA_.PushByte(static_cast<int8_t>(word & 0xFF));
    fifoA_.PushByte(static_cast<int8_t>((word >> 8) & 0xFF));
    fifoA_.PushByte(static_cast<int8_t>((word >> 16) & 0xFF));
    fifoA_.PushByte(static_cast<int8_t>((word >> 24) & 0xFF));
}

void GbaApu::PushFifoB(uint32_t word) {
    fifoB_.PushByte(static_cast<int8_t>(word & 0xFF));
    fifoB_.PushByte(static_cast<int8_t>((word >> 8) & 0xFF));
    fifoB_.PushByte(static_cast<int8_t>((word >> 16) & 0xFF));
    fifoB_.PushByte(static_cast<int8_t>((word >> 24) & 0xFF));
}

void GbaApu::ResetFifoA() { fifoA_.Reset(); }
void GbaApu::ResetFifoB() { fifoB_.Reset(); }

void GbaApu::OnTimerOverflow(GbaBus& bus, int timerId) {
    uint16_t soundcntH = bus.ReadIo16Direct(0x082);
    int timerA = (soundcntH >> 10) & 1;
    int timerB = (soundcntH >> 14) & 1;

    if (timerId == timerA) {
        fifoA_.Pop();
        if (fifoA_.count <= 16) {
            bus.TriggerDmaFifo(0x040000A0u);
        }
    }
    if (timerId == timerB) {
        fifoB_.Pop();
        if (fifoB_.count <= 16) {
            bus.TriggerDmaFifo(0x040000A4u);
        }
    }
}

int GbaApu::CountQueuedBuffers() const {
#ifdef _WIN32
    if (!waveBufs_) return 0;
    int q = 0;
    for (int i = 0; i < kNumWaveBuffers; ++i) {
        if (waveBufs_[i].prepared && !(waveBufs_[i].hdr.dwFlags & WHDR_DONE)) {
            q++;
        }
    }
    return q;
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (!waveBufs_ || !waveBufs_->sdlAudioDev) return 0;
    Uint32 bytes = SDL_GetQueuedAudioSize(waveBufs_->sdlAudioDev);
    return static_cast<int>(bytes / (kSamplesPerBuffer * 2 * sizeof(int16_t)));
#else
    return 0;
#endif
}

void GbaApu::StepCycles(GbaBus& bus, uint32_t cycles) {
    cycleAccum_ += static_cast<double>(cycles);
    uint16_t soundcntX = bus.ReadIo16Direct(0x084);
    bool masterEnable = (soundcntX & 0x80) != 0;
    uint16_t soundcntH = bus.ReadIo16Direct(0x082);

    // 1-pole low-pass filter (~5.2 kHz cutoff at 44.1 kHz, removes 10.5 kHz Direct Sound carrier buzz)
    constexpr float kLpAlpha = 0.35f;
    // DC-blocking high-pass filter coefficient
    constexpr float kDcR = 0.996f;

    double effectiveCyclesPerSample = kBaseCyclesPerSample * drcRateScale_;

    while (cycleAccum_ >= effectiveCyclesPerSample) {
        cycleAccum_ -= effectiveCyclesPerSample;

        float rawL = 0.0f;
        float rawR = 0.0f;

        if (masterEnable && !muted_) {
            float sA = static_cast<float>(fifoA_.currentSample) * ((soundcntH & (1u << 2)) ? 1.0f : 0.5f);
            float sB = static_cast<float>(fifoB_.currentSample) * ((soundcntH & (1u << 3)) ? 1.0f : 0.5f);

            if (soundcntH & (1u << 8))  rawR += sA;
            if (soundcntH & (1u << 9))  rawL += sA;
            if (soundcntH & (1u << 12)) rawR += sB;
            if (soundcntH & (1u << 13)) rawL += sB;

            rawL *= 110.0f * volume_;
            rawR *= 110.0f * volume_;
        }

        // Low-pass filter
        lpLeft_  += kLpAlpha * (rawL - lpLeft_);
        lpRight_ += kLpAlpha * (rawR - lpRight_);

        // DC blocker
        float outL = lpLeft_  - dcPrevInL_ + kDcR * dcPrevOutL_;
        float outR = lpRight_ - dcPrevInR_ + kDcR * dcPrevOutR_;
        dcPrevInL_ = lpLeft_;   dcPrevOutL_ = outL;
        dcPrevInR_ = lpRight_;  dcPrevOutR_ = outR;

        int32_t iL = std::clamp(static_cast<int32_t>(outL), -32767, 32767);
        int32_t iR = std::clamp(static_cast<int32_t>(outR), -32767, 32767);

        staging_.push_back(static_cast<int16_t>(iL));
        staging_.push_back(static_cast<int16_t>(iR));

        if (staging_.size() >= static_cast<size_t>(kSamplesPerBuffer * 2)) {
            PumpAudio();
            effectiveCyclesPerSample = kBaseCyclesPerSample * drcRateScale_;
        }
    }
}

void GbaApu::PumpAudio() {
#ifdef _WIN32
    if (!hWaveOut_ || !waveBufs_) {
        if (staging_.size() > kSamplesPerBuffer * 8) staging_.clear();
        return;
    }

    int queued = CountQueuedBuffers();
    double error = static_cast<double>(queued - kTargetQueuedBuffers);
    double targetScale = 1.0 + std::clamp(error * 0.004, -0.015, 0.015);
    drcRateScale_ = drcRateScale_ * 0.85 + targetScale * 0.15;

    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(hWaveOut_);
    while (staging_.size() >= static_cast<size_t>(kSamplesPerBuffer * 2)) {
        WaveBuf& wb = waveBufs_[nextWaveBuf_];
        if (wb.prepared && !(wb.hdr.dwFlags & WHDR_DONE)) {
            if (staging_.size() > static_cast<size_t>(kSamplesPerBuffer * 6)) {
                staging_.erase(staging_.begin(), staging_.begin() + kSamplesPerBuffer * 2);
            }
            break;
        }
        if (wb.prepared) {
            waveOutUnprepareHeader(hwo, &wb.hdr, sizeof(WAVEHDR));
            wb.prepared = false;
        }

        std::memcpy(wb.data.data(), staging_.data(), kSamplesPerBuffer * 2 * sizeof(int16_t));
        staging_.erase(staging_.begin(), staging_.begin() + kSamplesPerBuffer * 2);

        wb.hdr.dwFlags = 0;
        waveOutPrepareHeader(hwo, &wb.hdr, sizeof(WAVEHDR));
        wb.prepared = true;
        waveOutWrite(hwo, &wb.hdr, sizeof(WAVEHDR));
        nextWaveBuf_ = (nextWaveBuf_ + 1) % kNumWaveBuffers;
    }
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    if (!waveBufs_ || !waveBufs_->sdlAudioDev) {
        if (staging_.size() > kSamplesPerBuffer * 8) staging_.clear();
        return;
    }

    int queued = CountQueuedBuffers();
    double error = static_cast<double>(queued - kTargetQueuedBuffers);
    double targetScale = 1.0 + std::clamp(error * 0.004, -0.015, 0.015);
    drcRateScale_ = drcRateScale_ * 0.85 + targetScale * 0.15;

    if (queued < kNumWaveBuffers && !staging_.empty()) {
        SDL_QueueAudio(waveBufs_->sdlAudioDev, staging_.data(),
                       static_cast<Uint32>(staging_.size() * sizeof(int16_t)));
        staging_.clear();
    } else if (staging_.size() > static_cast<size_t>(kSamplesPerBuffer * 6)) {
        staging_.clear();
    }
#else
    if (staging_.size() > kSamplesPerBuffer * 8) staging_.clear();
#endif
}

} // namespace gba
