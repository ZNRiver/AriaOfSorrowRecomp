#pragma once

#include "gba_types.h"
#include <array>
#include <vector>

namespace gba {

class GbaBus;

class GbaApu {
public:
    GbaApu();
    ~GbaApu();

    bool InitAudioOutput();
    void ShutdownAudioOutput();
    void Reset();

    void SetVolume(float volume, bool muted);

    // Direct Sound FIFO A/B writes (0x040000A0 / 0x040000A4)
    void PushFifoA(uint32_t word);
    void PushFifoB(uint32_t word);
    void ResetFifoA();
    void ResetFifoB();

    // Called when Timer 0 or Timer 1 overflows
    void OnTimerOverflow(GbaBus& bus, int timerId);

    // Advance audio by N CPU cycles and emit samples to output ring buffer
    void StepCycles(GbaBus& bus, uint32_t cycles);

    // Flush queued audio buffers to Windows waveOut
    void PumpAudio();

    // Call when emulation is paused (e.g., ImGui menu open) to avoid stale buffer pops
    void OnPaused();

private:
    struct Fifo {
        std::array<int8_t, 32> buf{};
        int readPos = 0;
        int writePos = 0;
        int count = 0;
        int8_t currentSample = 0;

        void Reset() {
            readPos = writePos = count = 0;
            // Keep currentSample decaying smoothly instead of hard-snapping to 0 (prevents clicks!)
        }
        void HardReset() {
            readPos = writePos = count = 0;
            currentSample = 0;
        }
        void PushByte(int8_t b) {
            if (count < 32) {
                buf[writePos] = b;
                writePos = (writePos + 1) & 31;
                count++;
            }
        }
        void Pop() {
            if (count > 0) {
                currentSample = buf[readPos];
                readPos = (readPos + 1) & 31;
                count--;
            }
        }
    };

    Fifo fifoA_;
    Fifo fifoB_;

    // Smooth 1-pole low-pass filter state (eliminates staircase aliasing from 13-18kHz Direct Sound)
    float lpLeft_ = 0.0f;
    float lpRight_ = 0.0f;
    // DC-blocker high-pass state (removes pops/clicks)
    float dcPrevInL_ = 0.0f, dcPrevOutL_ = 0.0f;
    float dcPrevInR_ = 0.0f, dcPrevOutR_ = 0.0f;

    // Dynamic Rate Control (DRC) for glitch-free audio with zero drops/underruns
    double cycleAccum_ = 0.0;
    double drcRateScale_ = 1.0;
    static constexpr int kSampleRate = 44100;
    static constexpr double kBaseCyclesPerSample = static_cast<double>(CPU_FREQ_HZ) / kSampleRate;

    // Staging stereo samples (L, R)
    std::vector<int16_t> staging_;

    float volume_ = 0.8f;
    bool muted_ = false;

    // WinMM waveOut handle (opaque pointer to avoid windows.h in header)
    void* hWaveOut_ = nullptr;
    static constexpr int kNumWaveBuffers = 10;
    static constexpr int kTargetQueuedBuffers = 4; // ~46ms sweet spot: immune to OS jitter & low latency
    static constexpr int kSamplesPerBuffer = 512;  // ~11.6ms per buffer
    struct WaveBuf;
    WaveBuf* waveBufs_ = nullptr;
    int nextWaveBuf_ = 0;

    int CountQueuedBuffers() const;
};

} // namespace gba
