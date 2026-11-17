// OCKMWave.cpp - OCKM Wave, a lean wavetable synth for the MPC that doubles as a lesson in
// wavetable synthesis.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// See ../SPEC.md for the design and ../README.md for the plain-English lessons.
// Inspired by the feature list in the SPECS.md of "Panacea" by KRRSRR (MIT); no code taken.
//
// Signal flow (per voice):
//
//   note -> pitch (Oct, Porta, bend +-2 st, LFO vibrato)
//        -> WAVETABLE OSC: 8 tables x 16 frames. WT Pos picks / blends two neighbouring
//           frames; Warp bends how the frame is read (Sync, Bend, Mirror, Quantize);
//           Unison plays 1-5 detuned copies, spread left-right by Width.
//        +  SUB: a sine one octave below (read from the same band-limited tables)
//        -> DRIVE (soft saturation) -> FILTER: 12 dB state-variable LP / BP / HP, or at 24 dB a
//           4-pole ladder (LP) / two state-variable stages (BP, HP); one per side -> FAT (low boost)
//        -> x ENV 1 (amp) x velocity
//   Two STEP LANES (A, B), shared by all notes and locked to the MPC tempo, each move one target
//   (Cutoff, WT Pos, Warp, Res, Volume or Pitch) through a 16-step pattern of its own length.
//   sum of voices -> Master level -> L/R
//
// On-screen names follow common wavetable-synth wording (WT Pos, Env1 = amp envelope,
// Env2 = mod envelope, LFO1, Width, Porta). The ids below kept their original spelling.
//
// Wavetables are generated from formulas the first time the plugin loads, then stored at
// 10 brightness levels ("mipmaps") so high notes never alias. They are shared by all instances.
//
// Realtime: process() never allocates, locks or does I/O. Modulation (envelopes, LFO, pitch,
// mip level, filter coefficients) runs every 16 samples; per sample each oscillator copy does
// one phase step, one warp, 4 table reads and a few multiply-adds.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/adsr.h"
#include "dsp/svf.h"
#include "dsp/lfo.h"
#include "dsp/voice.h"
#include "dsp/tempo.h"
#include "dsp/noise.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

using mpc::floatParam; using mpc::logParam; using mpc::intParam; using mpc::choiceParam;
using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0,   // "Master": output level, dB (param 0 is nudged by the host on load: harmless)
    kTable,        // which wavetable (8 generated tables)
    kPosition,     // "WT Pos": which frame of the table, 0..100 % (blends between neighbours)
    kWarp,         // how the frame is read: Off / Sync / Bend / Mirror / Quantize
    kWarpAmt,      // how strongly, % (0 = identical to Off)
    kUnison,       // 1..5 oscillator copies per voice
    kDetune,       // copies' pitch spread, % of +-50 cents
    kSpread,       // "Width": copies' stereo spread, %
    kSub,          // sine one octave down, %
    kOctave,       // -3..+3
    kVoiceMode,    // Poly (6 voices) / Mono / Legato
    kGlide,        // "Porta": pitch slide time, ms
    kVelAmp,       // how much velocity changes loudness, %
    kFType,        // LP / BP / HP
    kCutoff,       // Hz
    kReso,         // %
    kKeyTrack,     // cutoff follows the keyboard, % (100 = 1 octave per octave)
    kAAtk, kADec, kASus, kARel,   // "Env1": amp envelope
    kMAtk, kMDec, kMSus, kMRel,   // "Env2": mod envelope
    kEnvPos,       // Env2 -> WT Pos, +-%
    kEnvWarp,      // Env2 -> Warp Amt, +-%
    kEnvCut,       // Env2 -> cutoff, +-semitones
    kLfoShape,     // Sine / Triangle / Saw / Square / S&H
    kLfoRate,      // Hz (Sync Off)
    kLfoSync,      // Off / On (locks to the MPC tempo and song position)
    kLfoDiv,       // note division (Sync On)
    kLfoPos,       // LFO1 -> WT Pos, % (swings +- half this around WT Pos)
    kLfoWarp,      // LFO -> Warp Amt, %
    kLfoCut,       // LFO -> cutoff, semitones
    kLfoPitch,     // LFO -> pitch (vibrato), cents
    kWheelPos,     // mod wheel -> WT Pos, +-%
    // ---- added in 1.2 (appended: never reorder) ----
    kSlope,        // 12 dB (state-variable) / 24 dB (LP: 4-pole ladder; BP, HP: two SVFs in a row)
    kDrive,        // saturation before the filter, %
    kFat,          // warm low-end boost after the filter, % (up to +6 dB below about 150 Hz)
    // Step lane A: 16 steps played in time with the MPC, moving one target.
    kADest, kAAmt, kALen, kARate, kASmooth, kAStep1,
    // Step lane B: same, own length and speed, so the two can drift against each other.
    kBDest = kAStep1 + 16, kBAmt, kBLen, kBRate, kBSmooth, kBStep1,
    kNumParams = kBStep1 + 16
};

// Step-lane targets, in the order of the "Target" menu.
enum LaneDest { kToCutoff = 0, kToPos, kToWarp, kToRes, kToVolume, kToPitch };
constexpr int kLaneSteps = 16;

enum Warp { kWarpOff = 0, kWarpSync, kWarpBend, kWarpMirror, kWarpQuantize };
enum VoiceMode { kPoly = 0, kMono, kLegato };

constexpr int kVoices = 6;
constexpr int kMaxUnison = 5;
constexpr int kCtrl = 16;            // control-rate interval, samples
constexpr float kVoiceGain = 0.22f;  // headroom for chords and unison stacks

// ---------------------------------------------------------------------------
// The wavetable bank: 8 tables x 16 frames x 10 mip levels, built once and shared.
// ---------------------------------------------------------------------------
constexpr int kTables = 8;
constexpr int kFrames = 16;
constexpr int kLevels = 10;          // level k keeps harmonics 1 .. 512 >> k
constexpr int kSrcSize = 2048;       // samples per cycle while generating

const char* const kTableNames[kTables] = {"Shapes", "Harmonics", "Pulse", "Vowels", "Sync", "FM", "Fold", "Steps"};

inline int levelHarmonics(int k) { return 512 >> k; }
inline int levelSize(int k)      { int n = 4 * levelHarmonics(k); return n < 256 ? 256 : (n > 2048 ? 2048 : n); }

// In-place radix-2 FFT (sign -1 forward, +1 inverse, no scaling). Load time only.
void fft(std::vector<std::complex<double>>& a, int sign)
{
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[(size_t)i], a[(size_t)j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = sign * 2.0 * 3.14159265358979323846 / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (int j = 0; j < len / 2; ++j)
            {
                const std::complex<double> u = a[(size_t)(i + j)], v = a[(size_t)(i + j + len / 2)] * w;
                a[(size_t)(i + j)] = u + v;
                a[(size_t)(i + j + len / 2)] = u - v;
                w *= wl;
            }
        }
    }
}

// One single cycle of table `t`, frame `f` (0..15), at phase x in [0,1). Plain formulas;
// the band-limiting happens afterwards, so these may be as sharp as they like.
double sourceSample(int t, int f, double x)
{
    const double twoPi = 2.0 * 3.14159265358979323846;
    const double m = f / 15.0;                                  // 0..1 across the table
    switch (t)
    {
        case 0: // Shapes: sine (frame 0) -> triangle (5) -> saw (10) -> square (15)
        {
            auto shape = [&](int s) {
                switch (s) {
                    case 0:  return std::sin(twoPi * x);
                    case 1:  return x < 0.25 ? 4 * x : (x < 0.75 ? 2 - 4 * x : 4 * x - 4);
                    case 2:  return 1.0 - 2.0 * x;
                    default: return x < 0.5 ? 1.0 : -1.0;
                }
            };
            const int seg = f >= 15 ? 2 : f / 5;
            const double u = (f - seg * 5) / 5.0;
            return shape(seg) * (1.0 - u) + shape(seg + 1) * u;
        }
        case 1: // Harmonics: frame f has harmonics 1..f+1 at 1/h
        {
            double y = 0.0;
            for (int h = 1; h <= f + 1; ++h) y += std::sin(twoPi * h * x) / h;
            return y;
        }
        case 2: // Pulse: width 50 % -> 5 %
            return x < 0.5 - 0.45 * m ? 1.0 : -1.0;
        case 3: // Vowels: three formants per vowel, morphing ah -> eh -> ee -> oh -> oo
        {
            static const double F[5][3] = {{800, 1150, 2900}, {550, 1770, 2490}, {270, 2290, 3010},
                                           {450, 800, 2830},  {325, 700, 2530}};
            static const double G[3] = {1.0, 0.5, 0.25}, BW[3] = {90, 110, 160};
            const double pos = m * 4.0;
            const int a = pos >= 4.0 ? 3 : (int)pos;
            const double u = pos - a;
            double y = 0.0;
            for (int h = 1; h <= 40; ++h)
            {
                const double hz = 130.0 * h;            // formants placed for a 130 Hz voice
                double amp = 0.0;
                for (int k = 0; k < 3; ++k)
                {
                    const double fk = F[a][k] * (1 - u) + F[a + 1][k] * u;
                    const double d = (hz - fk) / BW[k];
                    amp += G[k] / (1.0 + d * d);
                }
                y += amp / std::sqrt((double)h) * std::sin(twoPi * h * x);
            }
            return y;
        }
        case 4: // Sync: a saw restarted every cycle, running 1x -> 8x faster
        {
            const double r = 1.0 + 7.0 * m, p = r * x;
            return 1.0 - 2.0 * (p - std::floor(p));
        }
        case 5: // FM: carrier 1, modulator 3, index 0 -> 8
            return std::sin(twoPi * x + 8.0 * m * std::sin(twoPi * 3.0 * x));
        case 6: // Fold: a sine pushed through a wavefolder, gain 1 -> 8
        {
            double v = (1.0 + 7.0 * m) * std::sin(twoPi * x) + 1.0;
            v -= 4.0 * std::floor(v / 4.0);
            return v < 2.0 ? v - 1.0 : 3.0 - v;
        }
        default: // Steps: smooth saw, then a staircase of 64 -> 4 steps
        {
            if (f == 0) return 1.0 - 2.0 * x;
            const double n = std::round(64.0 * std::pow(4.0 / 64.0, (f - 1) / 14.0));
            return 1.0 - 2.0 * (std::floor(x * n) / n);
        }
    }
}

struct Bank
{
    std::vector<float> data;
    size_t tableStride = 0;
    size_t levelOffset[kLevels] = {};
    int stride[kLevels] = {};        // samples per frame at level k (size + 2 guard samples)

    Bank()
    {
        size_t perTable = 0;
        for (int k = 0; k < kLevels; ++k)
        {
            levelOffset[k] = perTable;
            stride[k] = levelSize(k) + 2;
            perTable += (size_t)kFrames * (size_t)stride[k];
        }
        tableStride = perTable;
        data.assign(perTable * kTables, 0.0f);

        std::vector<std::complex<double>> spec((size_t)kSrcSize), tmp;
        for (int t = 0; t < kTables; ++t)
            for (int f = 0; f < kFrames; ++f)
            {
                for (int i = 0; i < kSrcSize; ++i) spec[(size_t)i] = sourceSample(t, f, (double)i / kSrcSize);
                fft(spec, -1);
                float peak = 0.0f;
                for (int k = 0; k < kLevels; ++k)
                {
                    // Keep harmonics 1..H (DC removed), rebuild at this level's size.
                    const int n = levelSize(k), hMax = std::min(levelHarmonics(k), n / 2 - 1);
                    tmp.assign((size_t)n, std::complex<double>(0.0, 0.0));
                    for (int h = 1; h <= hMax; ++h)
                    {
                        tmp[(size_t)h] = spec[(size_t)h] / (double)kSrcSize;
                        tmp[(size_t)(n - h)] = std::conj(tmp[(size_t)h]);
                    }
                    fft(tmp, +1);
                    float* dst = frame(t, k, f);
                    for (int i = 0; i < n; ++i) dst[i] = (float)tmp[(size_t)i].real();
                    dst[n] = dst[0]; dst[n + 1] = dst[1];                 // guard samples for interpolation
                    if (k == 0) for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(dst[i]));
                }
                const float g = peak > 1e-6f ? 1.0f / peak : 0.0f;   // full-bandwidth peak = 1
                for (int k = 0; k < kLevels; ++k)
                {
                    float* dst = frame(t, k, f);
                    for (int i = 0; i < stride[k]; ++i) dst[i] *= g;
                }
            }
    }

    float* frame(int t, int k, int f) { return data.data() + tableStride * (size_t)t + levelOffset[k] + (size_t)f * (size_t)stride[k]; }
    const float* frame(int t, int k, int f) const { return data.data() + tableStride * (size_t)t + levelOffset[k] + (size_t)f * (size_t)stride[k]; }
};

const Bank& bank()
{
    static const Bank b;             // built on first use (thread-safe), shared by every instance
    return b;
}

// Highest mip level whose top harmonic stays under 0.45 x sample rate.
inline int pickLevel(float hz, float sr)
{
    const float hMax = 0.45f * sr / std::max(hz, 1.0f);
    int k = 0;
    while (k < kLevels - 1 && (float)levelHarmonics(k) > hMax) ++k;
    return k;
}

// ---------------------------------------------------------------------------
// Settings read once per block.
// ---------------------------------------------------------------------------
struct Settings
{
    int table = 0, warp = 0, unison = 1, octave = 0, mode = kPoly, ftype = 0;
    float position = 0.67f, warpAmt = 0, sub = 0, velAmp = 0.5f;
    float cutoff = 18000, reso = 0.1f, keyTrack = 0.5f;
    float envPos = 0, envWarp = 0, envCut = 0, lfoPos = 0, lfoWarp = 0, lfoCut = 0, lfoPitch = 0, wheelPos = 0;
    float glideCoef = 1.0f;
    bool ampSusZero = false;
    int slope = 0;
    bool monoVoice = true;                    // one copy or Width 0: left == right, so filter once
    float drive = 0, driveGain = 1, driveNorm = 1, fat = 0;
    float uniRatio[kMaxUnison] = {1, 1, 1, 1, 1}, uniL[kMaxUnison] = {}, uniR[kMaxUnison] = {};
    float uniMaxRatio = 1.0f;
};

// ---------------------------------------------------------------------------
// One voice.
// ---------------------------------------------------------------------------
struct Voice
{
    bool active = false, fresh = true;
    int note = 60;
    float velGain = 1.0f;
    float pitch = 60.0f, target = 60.0f;      // MIDI note, gliding
    ADSR amp, mod;
    SVF fL, fR, fL2, fR2;                     // fL2/fR2: second stage for 24 dB BP / HP
    float ladL[4] = {}, ladR[4] = {};         // 24 dB LP ladder states
    float ladG = 0, ladG2 = 0, ladG3 = 0, ladG4 = 0, ladA = 1, ladK = 0, ladInv = 1, ladComp = 1;
    float fatL = 0, fatR = 0;                 // Fat: low-pass state for the low boost
    float phase[kMaxUnison] = {};
    float subPhase = 0.0f;
    float pos = 0.0f;                         // position at the end of the last control step
    float fade = 1.0f, fadeStep = 0.0f;       // quick fade for "all sound off"
    bool peaked = false;                      // attack finished (for freeing silent plucks)

    // Worked out every control step
    const float* frameA = nullptr;
    const float* subTab = nullptr;
    int size = 256, subSize = 256, frameStride = 0;
    float inc[kMaxUnison] = {}, subInc = 0.0f, posStep = 0.0f;
    float warpA = 0, warpB = 0, warpC = 0;    // mode-specific warp constants
};

class OCKMWave : public mpc::Plugin
{
public:
    OCKMWave() : mpc::Plugin(makeInfo())
    {
        addParam(floatParam("output", "Master", -24.0f, 6.0f, -3.0f, "dB", 1));
        addParam(choiceParam("table", "Wavetable", {kTableNames[0], kTableNames[1], kTableNames[2], kTableNames[3],
                                                kTableNames[4], kTableNames[5], kTableNames[6], kTableNames[7]}, 0));
        addParam(floatParam("position", "WT Pos", 0.0f, 100.0f, 67.0f, "%", 0));
        addParam(choiceParam("warp", "Warp", {"Off", "Sync", "Bend", "Mirror", "Quantize"}, 0));
        addParam(floatParam("warpamt", "Warp Amt", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("unison", "Unison", 1, kMaxUnison, 1));
        addParam(floatParam("detune", "Detune", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("spread", "Width", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("sub", "Sub", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("octave", "Oct", -3, 3, 0));
        addParam(choiceParam("voicemode", "Voicing", {"Poly", "Mono", "Legato"}, 0));
        addParam(floatParam("glide", "Porta", 0.0f, 1000.0f, 0.0f, "ms", 0, 0.4f));
        addParam(floatParam("velamp", "Velo>Amp", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("ftype", "Filter Type", {"LP", "BP", "HP"}, 0));
        addParam(logParam("cutoff", "Cutoff", 20.0f, 18000.0f, 18000.0f, "Hz", 0));
        addParam(floatParam("reso", "Res", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam("keytrack", "Key Trk", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam("aatk", "Env1 Attack", 1.0f, 5000.0f, 2.0f, "ms", 0));
        addParam(logParam("adec", "Env1 Decay", 1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(floatParam("asus", "Env1 Sustain", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam("arel", "Env1 Release", 1.0f, 8000.0f, 250.0f, "ms", 0));
        addParam(logParam("matk", "Env2 Attack", 1.0f, 5000.0f, 2.0f, "ms", 0));
        addParam(logParam("mdec", "Env2 Decay", 1.0f, 5000.0f, 400.0f, "ms", 0));
        addParam(floatParam("msus", "Env2 Sustain", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("mrel", "Env2 Release", 1.0f, 8000.0f, 300.0f, "ms", 0));
        addParam(floatParam("envpos", "Env2>WT Pos", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("envwarp", "Env2>Warp", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("envcut", "Env2>Cutoff", -48.0f, 48.0f, 0.0f, "st", 0));
        addParam(choiceParam("lfoshape", "LFO1 Shape", LFO::shapeNames(), 1));
        addParam(logParam("lforate", "LFO1 Rate", 0.05f, 20.0f, 1.0f, "Hz", 2));
        addParam(choiceParam("lfosync", "LFO1 BPM", {"Off", "On"}, 0));
        addParam(choiceParam("lfodiv", "LFO1 Div", divisionNames(), kDivisionQuarter));
        addParam(floatParam("lfopos", "LFO1>WT Pos", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lfowarp", "LFO1>Warp", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lfocut", "LFO1>Cutoff", 0.0f, 48.0f, 0.0f, "st", 0));
        addParam(floatParam("lfopitch", "LFO1>Pitch", 0.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("wheelpos", "Wheel>WT Pos", -100.0f, 100.0f, 0.0f, "%", 0));
        // ---- 1.2 ----
        addParam(choiceParam("slope", "Slope", {"12 dB", "24 dB"}, 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("fat", "Fat", 0.0f, 100.0f, 0.0f, "%", 0));
        // Default patterns: lane A a busy rhythm, lane B a rising ramp. Both do nothing until
        // their Amount is turned away from 0.
        static const float kDefA[kLaneSteps] = {100, 25, 60, 10, 80, 20, 45, 0, 100, 35, 70, 15, 90, 5, 55, 30};
        static const float kDefB[kLaneSteps] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 100};
        for (int lane = 0; lane < 2; ++lane)
        {
            const std::string L = lane == 0 ? "A" : "B", id = lane == 0 ? "seqa" : "seqb";
            addParam(choiceParam((id + "dest").c_str(), (L + " Target").c_str(),
                                 {"Cutoff", "WT Pos", "Warp", "Res", "Volume", "Pitch"}, lane == 0 ? kToCutoff : kToPos));
            addParam(floatParam((id + "amt").c_str(), (L + " Amount").c_str(), -100.0f, 100.0f, 0.0f, "%", 0));
            addParam(intParam((id + "len").c_str(), (L + " Length").c_str(), 1, kLaneSteps, kLaneSteps));
            addParam(choiceParam((id + "rate").c_str(), (L + " Rate").c_str(), divisionNames(), kDivisionSixteenth));
            addParam(floatParam((id + "smooth").c_str(), (L + " Smooth").c_str(), 0.0f, 100.0f, lane == 0 ? 20.0f : 60.0f, "%", 0));
            for (int i = 0; i < kLaneSteps; ++i)
                addParam(floatParam((id + std::to_string(i + 1)).c_str(), (L + std::to_string(i + 1)).c_str(),
                                    0.0f, 100.0f, lane == 0 ? kDefA[i] : kDefB[i], "%", 0));
        }
        addPrograms();
        (void)bank();                 // build the tables now, never on the audio thread
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = (float)sampleRate;
        for (Voice& v : voices_)
        {
            v.amp.setSampleRate(sampleRate); v.mod.setSampleRate(sampleRate);
            v.fL.setSampleRate(sampleRate); v.fR.setSampleRate(sampleRate);
            v.fL2.setSampleRate(sampleRate); v.fR2.setSampleRate(sampleRate);
        }
        fatCoef_ = 1.0f - std::exp(-2.0f * kPi * 150.0f / sr_);
        lfo_.setSampleRate(sampleRate);
        output_.setTime(20.0f, sampleRate);
        output_.snap(dbToGain(get(kOutput)));
        reset();
    }

    void reset() override
    {
        for (Voice& v : voices_)
        {
            v.amp.reset(); v.mod.reset(); v.fL.reset(); v.fR.reset(); v.fL2.reset(); v.fR2.reset();
            clearLadder(v);
            v.active = false; v.fresh = true; v.fade = 1.0f; v.fadeStep = 0.0f;
        }
        alloc_.reset();
        stackN_ = 0;
        lfo_.reset();
        lastMode_ = -1;
        wheel_ = wheelTarget_ = 0.0f;
        laneBeats_ = 0.0;
        for (int l = 0; l < 2; ++l) { laneOut_[l] = 0.0f; laneDestPrev_[l] = -1; }
        laneGain_ = laneGainPrev_ = 1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        // Step-lane clock, in beats: follows the MPC's song position while it plays; otherwise it
        // runs from the first note after silence (see noteOn).
        bpm_ = ctx.transport.tempoValid ? ctx.transport.bpm : 120.0;
        playing_ = ctx.transport.playing && ctx.transport.ppqValid;
        if (playing_) laneBeats_ = ctx.transport.ppqPos;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int pos = 0, ev = 0;
        while (pos < ctx.numFrames)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            int end = ctx.numFrames;
            if (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset < end) end = ctx.midi[ev].sampleOffset;
            while (pos < end)
            {
                const int n = std::min(kCtrl, end - pos);
                renderStep(outL + pos, outR ? outR + pos : nullptr, n);
                pos += n;
            }
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
    }

    // For the test harness.
    int activeVoices() const { int n = 0; for (const Voice& v : voices_) n += v.active ? 1 : 0; return n; }
    float voicePitch(int i) const { return voices_[i].pitch; }

private:
    // ----- block settings -------------------------------------------------
    void readSettings(const mpc::ProcessContext& ctx)
    {
        Settings& s = set_;
        s.table = clamp(getInt(kTable), 0, kTables - 1);
        s.position = get(kPosition) * 0.01f;
        s.warp = clamp(getInt(kWarp), 0, 4);
        s.warpAmt = get(kWarpAmt) * 0.01f;
        s.unison = clamp(getInt(kUnison), 1, kMaxUnison);
        s.sub = get(kSub) * 0.01f;
        s.octave = getInt(kOctave);
        s.mode = clamp(getInt(kVoiceMode), 0, 2);
        s.velAmp = get(kVelAmp) * 0.01f;
        s.ftype = clamp(getInt(kFType), 0, 2);
        s.cutoff = get(kCutoff);
        s.reso = get(kReso) * 0.01f * 0.95f;
        s.keyTrack = get(kKeyTrack) * 0.01f;
        s.envPos = get(kEnvPos) * 0.01f; s.envWarp = get(kEnvWarp) * 0.01f; s.envCut = get(kEnvCut);
        s.lfoPos = get(kLfoPos) * 0.005f; s.lfoWarp = get(kLfoWarp) * 0.005f;
        s.lfoCut = get(kLfoCut); s.lfoPitch = get(kLfoPitch) * 0.01f;
        s.wheelPos = get(kWheelPos) * 0.01f;
        s.ampSusZero = get(kASus) < 0.01f;
        const float glideMs = get(kGlide);
        s.glideCoef = glideMs < 1.0f ? 1.0f : 1.0f - std::exp(-(float)kCtrl / (glideMs * 0.001f * sr_ / 3.0f));

        // Unison: copies evenly spaced from -1 to +1, detuned up to +-50 cents, panned by Width,
        // and turned down by 1/sqrt(copies) so the stack stays about as loud as one copy.
        const int n = s.unison;
        const float detune = get(kDetune) * 0.01f, spread = get(kSpread) * 0.01f;
        const float uniGain = 1.0f / std::sqrt((float)n);
        s.monoVoice = n == 1 || spread <= 0.0f;
        s.uniMaxRatio = 1.0f;
        for (int c = 0; c < n; ++c)
        {
            const float x = n == 1 ? 0.0f : -1.0f + 2.0f * (float)c / (float)(n - 1);
            s.uniRatio[c] = centsToRatio(x * detune * 50.0f);
            s.uniMaxRatio = std::max(s.uniMaxRatio, s.uniRatio[c]);
            equalPowerPan(x * spread, s.uniL[c], s.uniR[c]);
            s.uniL[c] *= 1.41421356f * uniGain; s.uniR[c] *= 1.41421356f * uniGain;
        }

        for (Voice& v : voices_)
        {
            v.amp.set(get(kAAtk), get(kADec), get(kASus) * 0.01f, get(kARel));
            v.mod.set(get(kMAtk), get(kMDec), get(kMSus) * 0.01f, get(kMRel));
        }

        // One LFO for the whole instance, so chords move together.
        lfo_.setShape(getInt(kLfoShape));
        if (getInt(kLfoSync) == 1)
        {
            lfo_.setTempoSync(ctx.transport.tempoValid ? ctx.transport.bpm : 120.0, divisionBeats(getInt(kLfoDiv)));
            if (ctx.transport.playing && ctx.transport.ppqValid) lfo_.syncToPpq(ctx.transport.ppqPos);
        }
        else lfo_.setFrequency(get(kLfoRate));

        output_.setTarget(dbToGain(get(kOutput)));

        // Filter character.
        s.slope = clamp(getInt(kSlope), 0, 1);
        s.drive = get(kDrive) * 0.01f;
        s.driveGain = 1.0f + 9.0f * s.drive;                         // 1x .. 10x into the saturator
        s.driveNorm = (1.0f - 0.35f * s.drive) / softClip(s.driveGain);    // peaks stay put, a bit quieter
        s.fat = get(kFat) * 0.01f;

        // Step lanes.
        for (int l = 0; l < 2; ++l)
        {
            const int base = l == 0 ? kADest : kBDest;
            Lane& ln = lanes_[l];
            ln.dest = clamp(getInt(base), 0, 5);
            ln.amt = get(base + 1) * 0.01f;
            ln.len = clamp(getInt(base + 2), 1, kLaneSteps);
            ln.beats = divisionBeats(getInt(base + 3));
            ln.smooth = get(base + 4) * 0.01f;
            for (int i = 0; i < kLaneSteps; ++i) ln.step[i] = get(base + 5 + i) * 0.01f;
        }

        if (s.mode != lastMode_)      // switching Poly / Mono / Legato: let everything ring out
        {
            if (lastMode_ >= 0) releaseAll();
            lastMode_ = s.mode;
        }
    }

    // ----- MIDI -------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) noteOn(e.note(), e.velocity());
        else if (e.isNoteOff()) noteOff(e.note());
        else if (e.isPitchBend()) alloc_.setPitchBend(e.pitchBend());
        else if (e.isAllNotesOff())
        {
            if (e.controller() == 120)        // all sound off: fade out in 5 ms
                for (Voice& v : voices_) if (v.active) v.fadeStep = 1.0f / (0.005f * sr_);
            releaseAll();
        }
        else if (e.isController() && e.controller() == 1) wheelTarget_ = (float)e.controllerValue() / 127.0f;
        else if (e.isController() && e.controller() == 64 && set_.mode == kPoly)
            alloc_.setSustain(e.controllerValue() >= 64, [this](int v) { voices_[v].amp.noteOff(); voices_[v].mod.noteOff(); });
    }

    // fromPitch >= 0: glide from that pitch.
    void startVoice(Voice& v, int note, int vel, bool retrigger, float fromPitch)
    {
        const bool wasSilent = !v.active;
        v.note = note;
        v.target = (float)note;
        if (fromPitch >= 0.0f) v.pitch = fromPitch;
        else if (set_.glideCoef >= 1.0f || wasSilent) v.pitch = (float)note;
        v.velGain = 1.0f - set_.velAmp * (1.0f - (float)vel / 127.0f);
        v.fade = 1.0f; v.fadeStep = 0.0f;
        if (wasSilent)
        {
            // One copy starts at phase 0 (punchy, repeatable); stacks start at random phases.
            for (int c = 0; c < kMaxUnison; ++c) v.phase[c] = set_.unison == 1 ? 0.0f : rng_.nextFloat();
            v.subPhase = 0.0f;
            v.fL.reset(); v.fR.reset(); v.fL2.reset(); v.fR2.reset();
            clearLadder(v);
            v.fresh = true;
        }
        if (retrigger || wasSilent) { v.amp.noteOn(); v.mod.noteOn(); v.peaked = false; }
        v.active = true;
    }

    void noteOn(int note, int vel)
    {
        if (!playing_ && activeVoices() == 0) laneBeats_ = 0.0;     // free-running lanes start on the first note
        if (set_.mode == kPoly)
        {
            const int prev = alloc_.lastNote();
            const int vi = alloc_.noteOn(note, vel);
            const bool glide = set_.glideCoef < 1.0f && prev >= 0;
            startVoice(voices_[vi], note, vel, true, glide ? (float)prev : -1.0f);
            return;
        }
        // Mono / Legato: last-note priority, all on voice 0.
        for (int i = 0; i < stackN_; ++i)
            if (stack_[i] == note) { for (int j = i; j < stackN_ - 1; ++j) stack_[j] = stack_[j + 1]; --stackN_; break; }
        if (stackN_ == kStack) { for (int j = 0; j < kStack - 1; ++j) stack_[j] = stack_[j + 1]; --stackN_; }
        stack_[stackN_++] = note;
        Voice& v = voices_[0];
        const bool held = v.active && v.amp.isHeld();
        const bool retrigger = !(set_.mode == kLegato && held);
        startVoice(v, note, vel, retrigger, -1.0f);   // glides from wherever it is when already sounding
    }

    void noteOff(int note)
    {
        if (set_.mode == kPoly)
        {
            const int vi = alloc_.noteOff(note);
            if (vi >= 0) { voices_[vi].amp.noteOff(); voices_[vi].mod.noteOff(); }
            return;
        }
        int idx = -1;
        for (int i = 0; i < stackN_; ++i) if (stack_[i] == note) idx = i;
        if (idx < 0) return;
        const bool wasTop = idx == stackN_ - 1;
        for (int j = idx; j < stackN_ - 1; ++j) stack_[j] = stack_[j + 1];
        --stackN_;
        if (!wasTop) return;
        Voice& v = voices_[0];
        if (stackN_ > 0)              // fall back to the previous held note
        {
            v.note = stack_[stackN_ - 1]; v.target = (float)v.note;
            if (set_.glideCoef >= 1.0f) v.pitch = v.target;
            if (set_.mode == kMono) { v.amp.noteOn(); v.mod.noteOn(); }
        }
        else { v.amp.noteOff(); v.mod.noteOff(); }
    }

    void releaseAll()
    {
        alloc_.reset();
        stackN_ = 0;
        for (Voice& v : voices_) { v.amp.noteOff(); v.mod.noteOff(); }
    }

    // ----- rendering --------------------------------------------------------
    void renderStep(float* outL, float* outR, int n)
    {
        const Settings& s = set_;
        const float lfo = lfo_.last();
        for (int i = 0; i < n; ++i) lfo_.process();
        wheel_ += (wheelTarget_ - wheel_) * 0.2f;
        updateLanes(n);

        float mixL[kCtrl] = {}, mixR[kCtrl] = {};
        for (int vi = 0; vi < kVoices; ++vi)
        {
            Voice& v = voices_[vi];
            if (!v.active) continue;
            control(v, lfo, n);
            const int fp = s.slope == 0 ? kF12 : (s.ftype == 0 ? kFLadder : kF24);
            switch (s.warp)
            {
                case kWarpSync:     renderFilter<kWarpSync>(fp, v, mixL, mixR, n); break;
                case kWarpBend:     renderFilter<kWarpBend>(fp, v, mixL, mixR, n); break;
                case kWarpMirror:   renderFilter<kWarpMirror>(fp, v, mixL, mixR, n); break;
                case kWarpQuantize: renderFilter<kWarpQuantize>(fp, v, mixL, mixR, n); break;
                default:            renderFilter<kWarpOff>(fp, v, mixL, mixR, n); break;
            }
            if (v.amp.level() > 0.5f) v.peaked = true;               // the attack has been through
            const bool silentPluck = v.peaked && s.ampSusZero && v.amp.level() < 0.0005f;   // decayed to nothing, key still down
            if (!v.amp.isActive() || v.fade <= 0.0f || silentPluck)
            {
                v.active = false; v.amp.reset(); v.mod.reset();
                if (s.mode == kPoly) alloc_.voiceFinished(vi);
            }
        }
        const float gStep = (laneGain_ - laneGainPrev_) / (float)n;
        float lg = laneGainPrev_;
        laneGainPrev_ = laneGain_;
        for (int i = 0; i < n; ++i)
        {
            lg += gStep;
            const float g = output_.process() * lg;
            const float l = sanitize(mixL[i] * g), r = sanitize(mixR[i] * g);
            if (outR) { outL[i] += l; outR[i] += r; } else outL[i] += 0.5f * (l + r);
        }
    }

    // Everything that only needs updating every 16 samples.
    void control(Voice& v, float lfo, int n)
    {
        const Settings& s = set_;
        const Bank& b = bank();
        const float menv = v.mod.level();
        for (int i = 0; i < n; ++i) v.mod.process();

        v.pitch += (v.target - v.pitch) * s.glideCoef;
        const float semis = v.pitch + 12.0f * (float)s.octave + alloc_.bendSemitones() + s.lfoPitch * lfo + mod_.pitch;
        const float hz = 440.0f * std::exp2((semis - 69.0f) * (1.0f / 12.0f));
        for (int c = 0; c < s.unison; ++c) v.inc[c] = std::min(hz * s.uniRatio[c] / sr_, 0.49f);
        v.subInc = std::min(0.5f * hz / sr_, 0.49f);

        // WT Pos: knob + Env2 + LFO + mod wheel, ramped across the step.
        const float pos = clamp(s.position + s.envPos * menv + s.lfoPos * lfo + s.wheelPos * wheel_ + mod_.pos, 0.0f, 1.0f);
        if (v.fresh) { v.pos = pos; v.fresh = false; }
        v.posStep = (pos - v.pos) / (float)n;

        // Warp amount, and how much it raises the highest harmonic (to pick a duller mip level).
        const float w = clamp(s.warpAmt + s.envWarp * menv + s.lfoWarp * lfo + mod_.warp, 0.0f, 1.0f);
        float harmonicFactor = 1.0f;
        switch (s.warp)
        {
            case kWarpSync:   v.warpA = 1.0f + 3.0f * w; harmonicFactor = v.warpA; break;     // read 1x..4x faster
            case kWarpBend:
            {
                const float d = 0.5f - 0.45f * w;           // first half of the frame squeezed into d
                v.warpA = d; v.warpB = 0.5f / d; v.warpC = 0.5f / (1.0f - d);
                harmonicFactor = v.warpB;
                break;
            }
            case kWarpMirror: v.warpA = w; harmonicFactor = w > 0.0f ? 2.0f : 1.0f; break;     // blend in a mirrored read
            case kWarpQuantize:
                v.warpA = w < 0.005f ? 0.0f : std::round(128.0f * std::exp2(-5.0f * w));     // 128 -> 4 steps (0 = off)
                v.warpB = v.warpA > 0.0f ? 1.0f / v.warpA : 0.0f;
                break;
            default: break;
        }

        const int level = pickLevel(hz * s.uniMaxRatio * harmonicFactor, sr_);
        v.size = levelSize(level);
        v.frameStride = b.stride[level];
        v.frameA = b.frame(s.table, level, 0);
        const int subLevel = pickLevel(0.5f * hz, sr_);
        v.subSize = levelSize(subLevel);
        v.subTab = b.frame(0, subLevel, 0);             // Shapes frame 0 = a pure sine

        // Filter: cutoff + key tracking (around middle C) + mod env + LFO, in semitones.
        const float cutSemis = s.keyTrack * (v.pitch + 12.0f * (float)s.octave - 60.0f) + s.envCut * menv + s.lfoCut * lfo
                             + mod_.cut;
        const float cut = clamp(s.cutoff * std::exp2(cutSemis * (1.0f / 12.0f)), 20.0f, 0.45f * sr_);
        const float reso = clamp(s.reso + mod_.res, 0.0f, 0.95f);
        if (s.slope == 1 && s.ftype == 0)
        {
            // 4-pole ladder (zero-delay feedback, solved per sample; one soft clip in the loop).
            const float g = std::tan(kPi * cut / sr_);
            v.ladG = g / (1.0f + g); v.ladA = 1.0f - v.ladG;
            v.ladG2 = v.ladG * v.ladG; v.ladG3 = v.ladG2 * v.ladG; v.ladG4 = v.ladG3 * v.ladG;
            v.ladK = reso * 4.0f;                                  // Res 100% -> k 3.8, just short of whistling
            v.ladInv = 1.0f / (1.0f + v.ladK * v.ladG4);
            v.ladComp = 1.0f + 0.5f * v.ladK;                      // gives back part of the bass that resonance takes
            for (int i = 0; i < 4; ++i) { v.ladL[i] = flushDenormal(v.ladL[i]); v.ladR[i] = flushDenormal(v.ladR[i]); }
        }
        else
        {
            v.fL.set(cut, reso); v.fR.set(cut, reso);
            if (s.slope == 1) { v.fL2.set(cut, 0.0f); v.fR2.set(cut, 0.0f); }
        }
        v.fatL = flushDenormal(v.fatL); v.fatR = flushDenormal(v.fatR);
    }

    static void clearLadder(Voice& v)
    {
        for (int i = 0; i < 4; ++i) v.ladL[i] = v.ladR[i] = 0.0f;
        v.fatL = v.fatR = 0.0f;
    }

    static inline float ladder(float* st, float x, const Voice& v)
    {
        const float S = v.ladA * (v.ladG3 * st[0] + v.ladG2 * st[1] + v.ladG * st[2] + st[3]);
        const float y4 = (v.ladG4 * x + S) * v.ladInv;            // the loop's output, solved without delay
        float u = x - v.ladK * y4;
        u = 1.5f * softClip(u * (1.0f / 1.5f));                    // the ladder's input clip: keeps it round and bounded
        for (int i = 0; i < 4; ++i)
        {
            const float t = (u - st[i]) * v.ladG;
            const float y = t + st[i];
            st[i] = y + t;
            u = y;
        }
        return u;
    }

    // Step lanes: once per control step, work out where each lane is and what it adds.
    void updateLanes(int n)
    {
        mod_ = LaneMod();
        float gain = 1.0f;
        const double beatsPerSample = bpm_ / 60.0 / (double)sr_;
        for (int l = 0; l < 2; ++l)
        {
            const Lane& ln = lanes_[l];
            const double pos = laneBeats_ / (double)ln.beats;
            long long idx = (long long)std::floor(pos) % ln.len;
            if (idx < 0) idx += ln.len;
            const float st = ln.step[idx], a = ln.amt;
            float target = 0.0f;
            switch (ln.dest)
            {
                case kToCutoff: target = st * a * 48.0f; break;                         // up to 4 octaves
                case kToPos:    target = st * a; break;
                case kToWarp:   target = st * a; break;
                case kToRes:    target = st * a * 0.95f; break;
                case kToVolume: target = a >= 0.0f ? 1.0f - a * (1.0f - st) : 1.0f + a * st; break;   // a gate
                case kToPitch:  target = std::round(st * a * 12.0f); break;             // whole semitones, up to an octave
                default: break;
            }
            // Smooth: 0% = a quick 1 ms ramp (no clicks), 100% = half a step to settle.
            const float stepSec = (float)(ln.beats * 60.0 / bpm_);
            const float tau = std::max(0.001f, ln.smooth * 0.5f * stepSec);
            const float coef = 1.0f - std::exp(-(float)n / (tau * sr_));
            if (laneDestPrev_[l] != ln.dest) { laneOut_[l] = ln.dest == kToVolume ? 1.0f : 0.0f; laneDestPrev_[l] = ln.dest; }
            laneOut_[l] += (target - laneOut_[l]) * coef;
            const float o = laneOut_[l];
            switch (ln.dest)
            {
                case kToCutoff: mod_.cut += o; break;
                case kToPos:    mod_.pos += o; break;
                case kToWarp:   mod_.warp += o; break;
                case kToRes:    mod_.res += o; break;
                case kToVolume: gain *= clamp(o, 0.0f, 1.0f); break;
                case kToPitch:  mod_.pitch += o; break;
                default: break;
            }
        }
        laneGain_ = gain;
        laneBeats_ += beatsPerSample * (double)n;
    }

    static inline float readAt(const float* t, int size, float ph)
    {
        const float x = ph * (float)size;
        const int i = (int)x;
        const float f = x - (float)i;
        return t[i] + f * (t[i + 1] - t[i]);
    }

    // Read two neighbouring frames at the same spot and blend them by ff.
    static inline float readPair(const float* A, const float* B, int size, float ph, float ff)
    {
        const float x = ph * (float)size;
        const int i = (int)x;
        const float f = x - (float)i;
        const float a = A[i] + f * (A[i + 1] - A[i]), b = B[i] + f * (B[i + 1] - B[i]);
        return a + ff * (b - a);
    }

    // Filter paths, chosen once per control step so the per-sample loop has no filter branches.
    enum { kF12 = 0, kF24, kFLadder };
    template <int W>
    void renderFilter(int fp, Voice& v, float* mixL, float* mixR, int n)
    {
        if (fp == kFLadder) renderVoice<W, kFLadder>(v, mixL, mixR, n);
        else if (fp == kF24) renderVoice<W, kF24>(v, mixL, mixR, n);
        else renderVoice<W, kF12>(v, mixL, mixR, n);
    }

    template <int W, int F>
    void renderVoice(Voice& v, float* mixL, float* mixR, int n)
    {
        const Settings& s = set_;
        const int size = v.size, stride = v.frameStride, uni = s.unison;
        const float warpA = v.warpA, warpB = v.warpB, warpC = v.warpC;
        const float gain = kVoiceGain * v.velGain;
        const bool drive = s.drive > 0.0f, fat = s.fat > 0.0f, mono = s.monoVoice;
        const float driveGain = s.driveGain, driveNorm = s.driveNorm, fatAmt = s.fat, fatCoef = fatCoef_;
        float pos = v.pos;
        for (int i = 0; i < n; ++i)
        {
            // WT Pos -> the two neighbouring frames and how far between them.
            pos += v.posStep;
            const float fp = clamp(pos, 0.0f, 1.0f) * (float)(kFrames - 1);
            int fi = (int)fp;
            float ff = fp - (float)fi;
            if (fi >= kFrames - 1) { fi = kFrames - 2; ff = 1.0f; }
            const float* A = v.frameA + (size_t)fi * (size_t)stride;
            const float* B = A + stride;

            float sumL = 0.0f, sumR = 0.0f;
            for (int c = 0; c < uni; ++c)
            {
                float ph = v.phase[c] + v.inc[c];
                if (ph >= 1.0f) ph -= 1.0f;
                v.phase[c] = ph;
                float rp = ph;                                   // where in the frame to read
                if (W == kWarpSync) { rp = ph * warpA; rp -= (float)(int)rp; }
                else if (W == kWarpBend) rp = ph < warpA ? ph * warpB : 0.5f + (ph - warpA) * warpC;
                else if (W == kWarpQuantize && warpA > 0.0f) rp = (float)(int)(ph * warpA) * warpB;
                if (rp >= 1.0f) rp = 0.0f;
                float y = readPair(A, B, size, rp, ff);
                if (W == kWarpMirror && warpA > 0.0f)
                {
                    float mp = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph;   // forwards, then backwards
                    if (mp >= 1.0f) mp = 0.0f;
                    y += warpA * (readPair(A, B, size, mp, ff) - y);
                }
                sumL += y * s.uniL[c];
                sumR += y * s.uniR[c];
            }
            if (s.sub > 0.0f)
            {
                v.subPhase += v.subInc;
                if (v.subPhase >= 1.0f) v.subPhase -= 1.0f;
                const float sb = s.sub * readAt(v.subTab, v.subSize, v.subPhase);
                sumL += sb; sumR += sb;
            }
            // Drive -> filter -> Fat.
            if (drive) { sumL = softClip(sumL * driveGain) * driveNorm; if (!mono) sumR = softClip(sumR * driveGain) * driveNorm; }
            float l, r;
            if (F == kFLadder) l = ladder(v.ladL, sumL * v.ladComp, v);
            else { l = v.fL.process(sumL, s.ftype); if (F == kF24) l = v.fL2.process(l, s.ftype); }
            if (fat) { v.fatL += (l - v.fatL) * fatCoef; l += fatAmt * v.fatL; }
            if (mono) r = l;
            else
            {
                if (F == kFLadder) r = ladder(v.ladR, sumR * v.ladComp, v);
                else { r = v.fR.process(sumR, s.ftype); if (F == kF24) r = v.fR2.process(r, s.ftype); }
                if (fat) { v.fatR += (r - v.fatR) * fatCoef; r += fatAmt * v.fatR; }
            }
            float g = v.amp.process() * gain;
            if (v.fadeStep > 0.0f) { v.fade -= v.fadeStep; if (v.fade < 0.0f) v.fade = 0.0f; g *= v.fade; }
            mixL[i] += l * g;
            mixR[i] += r * g;
        }
        v.pos = clamp(pos, 0.0f, 1.0f);
        if (mono)          // keep the right side's filter in step, so turning Width up later doesn't click
        {
            v.fR = v.fL; v.fR2 = v.fL2; v.fatR = v.fatL;
            for (int i = 0; i < 4; ++i) v.ladR[i] = v.ladL[i];
        }
    }

    // ----- presets ------------------------------------------------------------
    // Lessons 01-16 each show one idea (see README "Lessons"); 17-24 are track-ready sounds.
    // Each preset starts from the defaults (= "01 Init Saw") and changes only what it lists.
    void addPrograms()
    {
        struct KV { int i; float v; };
        auto program = [this](const char* name, std::vector<KV> kv)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const KV& p : kv) v[(size_t)p.i] = p.v;
            addProgram(name, std::move(v));
        };
        enum { SHAPES, HARM, PULSE, VOWELS, SYNC, FM, FOLD, STEPS };
        program("01 Init Saw", {});
        program("02 Shape Morph", {{kPosition, 50}, {kLfoPos, 100}, {kLfoShape, 1}, {kLfoRate, 0.2f}, {kARel, 400}});
        program("03 Wheel Scan", {{kTable, HARM}, {kPosition, 0}, {kWheelPos, 100}, {kASus, 90}});
        program("04 Pluck by Table", {{kTable, HARM}, {kPosition, 0}, {kEnvPos, 100}, {kMAtk, 1}, {kMDec, 350}, {kMSus, 0},
                                      {kAAtk, 1}, {kADec, 700}, {kASus, 0}, {kARel, 300}});
        program("05 Pluck by Filter", {{kCutoff, 250}, {kReso, 20}, {kEnvCut, 48}, {kMAtk, 1}, {kMDec, 350}, {kMSus, 0},
                                       {kAAtk, 1}, {kADec, 700}, {kASus, 0}, {kARel, 300}});
        program("06 Table PWM", {{kTable, PULSE}, {kPosition, 45}, {kLfoPos, 80}, {kLfoShape, 0}, {kLfoRate, 0.7f}, {kSub, 30}});
        program("07 Talking Pad", {{kTable, VOWELS}, {kPosition, 50}, {kLfoSync, 1}, {kLfoDiv, kDivisionHalf}, {kLfoPos, 100},
                                   {kUnison, 3}, {kDetune, 20}, {kSpread, 70}, {kAAtk, 300}, {kASus, 90}, {kARel, 1200}});
        program("08 Sync Sweep", {{kTable, SYNC}, {kPosition, 0}, {kEnvPos, 100}, {kMAtk, 1500}, {kMSus, 100}, {kMRel, 800},
                                  {kCutoff, 9000}});
        program("09 FM Bell", {{kTable, FM}, {kPosition, 0}, {kEnvPos, 80}, {kMAtk, 1}, {kMDec, 1200}, {kMSus, 0},
                               {kAAtk, 1}, {kADec, 2500}, {kASus, 0}, {kARel, 1500}});
        program("10 Fold Growl", {{kTable, FOLD}, {kPosition, 20}, {kVoiceMode, kMono}, {kOctave, -1}, {kGlide, 40},
                                  {kLfoSync, 1}, {kLfoDiv, kDivisionEighth}, {kLfoShape, 0}, {kLfoPos, 60}, {kSub, 40},
                                  {kCutoff, 2500}, {kReso, 30}});
        program("11 Step Grit", {{kTable, STEPS}, {kPosition, 60}, {kAAtk, 1}, {kADec, 200}, {kASus, 60}, {kARel, 100}});
        program("12 Warp Sync", {{kPosition, 0}, {kWarp, kWarpSync}, {kEnvWarp, 100}, {kMAtk, 1}, {kMDec, 500}, {kMSus, 30}});
        program("13 Warp Bend", {{kWarp, kWarpBend}, {kWarpAmt, 40}, {kLfoWarp, 60}, {kLfoRate, 0.5f}});
        program("14 Warp Mirror", {{kTable, HARM}, {kPosition, 50}, {kWarp, kWarpMirror}, {kWarpAmt, 100},
                                   {kAAtk, 5}, {kASus, 100}, {kARel, 80}});
        program("15 Unison Stack", {{kUnison, 5}, {kDetune, 40}, {kSpread, 100}});
        program("16 Aliasing Free", {{kTable, SYNC}, {kPosition, 100}, {kOctave, 2}, {kADec, 400}, {kASus, 50}});
        program("17 House Stab", {{kTable, HARM}, {kPosition, 30}, {kEnvPos, 60}, {kMAtk, 1}, {kMDec, 180}, {kMSus, 0},
                                  {kCutoff, 2500}, {kEnvCut, 24}, {kReso, 25}, {kUnison, 3}, {kDetune, 15}, {kSpread, 50},
                                  {kAAtk, 1}, {kADec, 250}, {kASus, 0}, {kARel, 150}});
        program("18 Deep Chord", {{kTable, VOWELS}, {kPosition, 30}, {kCutoff, 1200}, {kReso, 15}, {kEnvCut, 12}, {kMDec, 600},
                                  {kUnison, 2}, {kDetune, 10}, {kSpread, 60}, {kAAtk, 20}, {kADec, 800}, {kASus, 60}, {kARel, 500}});
        program("19 Sub Bass", {{kPosition, 20}, {kSub, 60}, {kVoiceMode, kMono}, {kOctave, -1}, {kCutoff, 800},
                                {kADec, 300}, {kASus, 90}, {kARel, 80}});
        program("20 Techno Rumble", {{kTable, FOLD}, {kPosition, 10}, {kOctave, -2}, {kVoiceMode, kMono}, {kLfoPos, 40},
                                     {kLfoRate, 0.3f}, {kCutoff, 400}, {kReso, 40}, {kSub, 30}, {kARel, 600}});
        program("21 Acid-ish Lead", {{kTable, SYNC}, {kPosition, 30}, {kVoiceMode, kLegato}, {kGlide, 80}, {kOctave, -1},
                                     {kCutoff, 400}, {kReso, 70}, {kEnvCut, 36}, {kMAtk, 1}, {kMDec, 250}, {kMSus, 0}});
        program("22 Wide Pad", {{kUnison, 5}, {kDetune, 25}, {kSpread, 100}, {kAAtk, 800}, {kADec, 1000}, {kASus, 80},
                                {kARel, 2000}, {kLfoPos, 30}, {kLfoRate, 0.15f}, {kCutoff, 5000}});
        program("23 Digital Arp", {{kTable, STEPS}, {kPosition, 40}, {kWarp, kWarpQuantize}, {kWarpAmt, 50},
                                   {kAAtk, 1}, {kADec, 180}, {kASus, 0}, {kARel, 120}, {kCutoff, 7000}});
        program("24 Vowel Bass", {{kTable, VOWELS}, {kPosition, 80}, {kVoiceMode, kMono}, {kOctave, -1}, {kEnvPos, -80},
                                  {kMAtk, 1}, {kMDec, 400}, {kMSus, 0}, {kSub, 40}, {kCutoff, 3000}});

        // 25-48: show-off sounds, grouped bass / lead / keys and plucks / pads / FX and builds.
        // Between them every parameter is used at least once (the test harness checks this).
        enum { SINE = 0, TRI = 1, SAW = 2, SQR = 3, SH = 4 };     // LFO shapes, in shapeNames() order
        const float kDiv16 = 4, kDiv8 = kDivisionEighth, kDiv2D = 14, kDiv1 = kDivisionBar;
        // --- bass
        program("25 Reese Bass", {{kPosition, 67}, {kUnison, 3}, {kDetune, 30}, {kSpread, 20}, {kVoiceMode, kMono},
                                  {kOctave, -1}, {kSub, 30}, {kCutoff, 900}, {kReso, 20}, {kKeyTrack, 30},
                                  {kLfoShape, SINE}, {kLfoRate, 0.25f}, {kLfoCut, 12}, {kASus, 100}, {kARel, 120}});
        program("26 Wobble Bass", {{kPosition, 80}, {kVoiceMode, kMono}, {kOctave, -1}, {kSub, 40}, {kCutoff, 300},
                                   {kReso, 55}, {kLfoShape, SINE}, {kLfoSync, 1}, {kLfoDiv, kDiv8}, {kLfoCut, 40},
                                   {kLfoPos, 30}, {kASus, 100}, {kARel, 100}, {kOutput, -5}});
        program("27 Organ Bass", {{kTable, HARM}, {kPosition, 20}, {kVoiceMode, kMono}, {kOctave, -1}, {kVelAmp, 80},
                                  {kCutoff, 600}, {kEnvCut, 24}, {kMAtk, 1}, {kMDec, 150}, {kAAtk, 1}, {kADec, 250},
                                  {kASus, 30}, {kARel, 80}});
        program("28 Acid Squelch", {{kPosition, 67}, {kVoiceMode, kLegato}, {kGlide, 60}, {kOctave, -1}, {kCutoff, 250},
                                    {kReso, 85}, {kEnvCut, 40}, {kMAtk, 1}, {kMDec, 200}, {kKeyTrack, 20}, {kVelAmp, 70},
                                    {kASus, 100}, {kARel, 60}, {kOutput, -6}});
        program("29 Hollow Bass", {{kTable, PULSE}, {kPosition, 30}, {kFType, 1}, {kCutoff, 700}, {kReso, 30},
                                   {kVoiceMode, kMono}, {kOctave, -1}, {kSub, 50}, {kEnvCut, 24}, {kMAtk, 1}, {kMDec, 300},
                                   {kASus, 90}, {kARel, 90}, {kOutput, 2}});
        // --- lead
        program("30 Supersaw Lead", {{kPosition, 67}, {kUnison, 5}, {kDetune, 45}, {kSpread, 90}, {kVoiceMode, kLegato},
                                     {kGlide, 30}, {kLfoShape, SINE}, {kLfoRate, 5}, {kLfoPitch, 15}, {kCutoff, 12000},
                                     {kAAtk, 5}, {kASus, 90}, {kARel, 300}});
        program("31 Sync Scream", {{kTable, SYNC}, {kPosition, 10}, {kWarp, kWarpSync}, {kWarpAmt, 40}, {kEnvPos, 70},
                                   {kMAtk, 1}, {kMDec, 800}, {kMSus, 20}, {kVoiceMode, kMono}, {kGlide, 50},
                                   {kLfoShape, SINE}, {kLfoRate, 5.5f}, {kLfoPitch, 10}, {kCutoff, 6000}, {kReso, 30},
                                   {kASus, 100}, {kARel, 200}, {kOutput, 1}});
        program("32 Vowel Lead", {{kTable, VOWELS}, {kPosition, 20}, {kWheelPos, 80}, {kVoiceMode, kLegato}, {kGlide, 40},
                                  {kLfoShape, TRI}, {kLfoRate, 4.5f}, {kLfoPitch, 12}, {kUnison, 2}, {kDetune, 10},
                                  {kASus, 100}, {kARel, 250}});
        // --- keys and plucks
        program("33 House Organ", {{kTable, HARM}, {kPosition, 45}, {kWarp, kWarpMirror}, {kWarpAmt, 50}, {kVelAmp, 60},
                                   {kFType, 2}, {kCutoff, 150}, {kReso, 0}, {kAAtk, 1}, {kADec, 200}, {kASus, 80}, {kARel, 60}});
        program("34 FM Marimba", {{kTable, FM}, {kPosition, 20}, {kEnvPos, 50}, {kMAtk, 1}, {kMDec, 200}, {kVelAmp, 90},
                                  {kCutoff, 8000}, {kKeyTrack, 100}, {kAAtk, 1}, {kADec, 600}, {kASus, 0}, {kARel, 400}});
        program("35 Glass Keys", {{kTable, FM}, {kPosition, 40}, {kWarp, kWarpMirror}, {kWarpAmt, 30}, {kUnison, 2},
                                  {kDetune, 8}, {kEnvPos, 40}, {kMAtk, 1}, {kMDec, 900}, {kLfoShape, SINE}, {kLfoRate, 4},
                                  {kLfoPitch, 5}, {kAAtk, 1}, {kADec, 1500}, {kASus, 20}, {kARel, 800}});
        program("36 Dub Chord", {{kPosition, 67}, {kFType, 1}, {kCutoff, 900}, {kReso, 40}, {kEnvCut, 24}, {kMAtk, 1},
                                 {kMDec, 250}, {kUnison, 2}, {kDetune, 12}, {kVelAmp, 70}, {kLfoShape, TRI}, {kLfoSync, 1},
                                 {kLfoDiv, kDiv2D}, {kLfoCut, 12}, {kAAtk, 1}, {kADec, 350}, {kASus, 0}, {kARel, 400}});
        program("37 Trance Pluck", {{kPosition, 67}, {kUnison, 4}, {kDetune, 30}, {kSpread, 80}, {kCutoff, 500},
                                    {kReso, 30}, {kEnvCut, 40}, {kMAtk, 1}, {kMDec, 220}, {kAAtk, 1}, {kADec, 400},
                                    {kASus, 0}, {kARel, 300}});
        program("38 Random Steps", {{kTable, STEPS}, {kPosition, 30}, {kLfoShape, SH}, {kLfoSync, 1}, {kLfoDiv, kDiv16},
                                    {kLfoPos, 70}, {kLfoCut, 18}, {kCutoff, 3000}, {kReso, 35}, {kAAtk, 1}, {kADec, 200},
                                    {kASus, 0}, {kARel, 150}});
        // --- pads
        program("39 Saw Strings", {{kPosition, 67}, {kUnison, 4}, {kDetune, 20}, {kSpread, 100}, {kCutoff, 3500},
                                   {kKeyTrack, 80}, {kLfoShape, SINE}, {kLfoRate, 4.5f}, {kLfoPitch, 6}, {kAAtk, 400},
                                   {kADec, 1000}, {kASus, 90}, {kARel, 1500}});
        program("40 Choir Pad", {{kTable, VOWELS}, {kPosition, 60}, {kUnison, 3}, {kDetune, 15}, {kSpread, 90},
                                 {kLfoShape, SINE}, {kLfoRate, 0.12f}, {kLfoPos, 40}, {kCutoff, 6000}, {kAAtk, 600},
                                 {kADec, 1000}, {kASus, 90}, {kARel, 2000}});
        program("41 Evolving Pad", {{kTable, HARM}, {kPosition, 10}, {kEnvPos, 80}, {kMAtk, 4000}, {kMSus, 100},
                                    {kMRel, 3000}, {kWarp, kWarpBend}, {kWarpAmt, 20}, {kLfoShape, TRI}, {kLfoRate, 0.2f},
                                    {kLfoWarp, 30}, {kUnison, 3}, {kDetune, 20}, {kSpread, 100}, {kAAtk, 1500},
                                    {kADec, 1000}, {kASus, 90}, {kARel, 3000}});
        program("42 Dark Drone", {{kTable, FOLD}, {kPosition, 30}, {kOctave, -1}, {kUnison, 3}, {kDetune, 15},
                                  {kLfoShape, TRI}, {kLfoRate, 0.07f}, {kLfoPos, 60}, {kLfoCut, 12}, {kCutoff, 700},
                                  {kReso, 40}, {kAAtk, 2000}, {kASus, 100}, {kARel, 4000}});
        program("43 Air Pad", {{kPosition, 67}, {kFType, 2}, {kCutoff, 1500}, {kReso, 20}, {kUnison, 5}, {kDetune, 35},
                               {kSpread, 100}, {kLfoShape, SINE}, {kLfoSync, 1}, {kLfoDiv, kDiv1}, {kLfoCut, 12},
                               {kAAtk, 800}, {kADec, 1000}, {kASus, 80}, {kARel, 2500}});
        // --- FX and builds
        program("44 Riser", {{kTable, SYNC}, {kPosition, 0}, {kEnvPos, 100}, {kWarp, kWarpSync}, {kEnvWarp, 60},
                             {kEnvCut, 36}, {kCutoff, 300}, {kMAtk, 5000}, {kMSus, 100}, {kMRel, 1500}, {kUnison, 3},
                             {kDetune, 30}, {kSpread, 100}, {kLfoShape, SINE}, {kLfoRate, 6}, {kLfoPitch, 40},
                             {kAAtk, 3000}, {kASus, 100}, {kARel, 1500}, {kOutput, 0}});
        program("45 Laser Zap", {{kTable, FM}, {kPosition, 100}, {kEnvPos, -100}, {kMAtk, 1}, {kMDec, 150},
                                 {kVoiceMode, kMono}, {kCutoff, 400}, {kReso, 80}, {kEnvCut, 48}, {kAAtk, 1}, {kADec, 250},
                                 {kASus, 0}, {kARel, 100}, {kOutput, -6}});
        program("46 Rave Stab", {{kTable, HARM}, {kPosition, 70}, {kWarp, kWarpMirror}, {kWarpAmt, 60}, {kUnison, 3},
                                 {kDetune, 35}, {kSpread, 70}, {kCutoff, 2000}, {kEnvCut, 24}, {kMAtk, 1}, {kMDec, 200},
                                 {kVelAmp, 80}, {kAAtk, 1}, {kADec, 300}, {kASus, 0}, {kARel, 250}});
        program("47 Bleep", {{kPosition, 0}, {kOctave, 1}, {kWarp, kWarpSync}, {kEnvWarp, 60}, {kMAtk, 1}, {kMDec, 80},
                             {kAAtk, 1}, {kADec, 120}, {kASus, 0}, {kARel, 60}});
        program("48 Trance Gate", {{kPosition, 67}, {kUnison, 2}, {kDetune, 15}, {kCutoff, 400}, {kReso, 20},
                                   {kLfoShape, SQR}, {kLfoSync, 1}, {kLfoDiv, kDiv16}, {kLfoCut, 36}, {kAAtk, 10},
                                   {kASus, 100}, {kARel, 300}});

        // 49-56 (1.2): the 24 dB ladder, Drive and Fat, and the two step lanes. Hold one key (or a
        // chord) and the lanes play the movement in time with the MPC.
        // with(kv, first step, {16 values}) adds a lane's step pattern to a preset.
        auto with = [](std::vector<KV> kv, int first, std::initializer_list<float> st)
        {
            int i = 0;
            for (float x : st) kv.push_back({first + i++, x});
            return kv;
        };
        const float kDiv8D = 9;
        // Semitones for a Pitch lane at Amount 100%: a step of n/12 plays n semitones up.
        auto st = [](float semis) { return semis * 100.0f / 12.0f; };
        program("49 Ladder Bass", {{kPosition, 67}, {kSlope, 1}, {kVoiceMode, kMono}, {kOctave, -1}, {kCutoff, 300},
                                   {kReso, 60}, {kDrive, 40}, {kFat, 60}, {kEnvCut, 36}, {kMAtk, 1}, {kMDec, 220},
                                   {kSub, 20}, {kASus, 100}, {kARel, 80}});
        program("50 Seq Filter Funk", with({{kPosition, 67}, {kSlope, 1}, {kVoiceMode, kMono}, {kOctave, -1}, {kGlide, 65},
                                            {kCutoff, 350}, {kReso, 55}, {kDrive, 30}, {kFat, 40}, {kASus, 100}, {kARel, 80},
                                            {kADest, kToCutoff}, {kAAmt, 80}, {kASmooth, 10}},
                                           kAStep1, {100, 10, 60, 0, 90, 20, 40, 0, 100, 0, 70, 30, 50, 0, 80, 20}));
        program("51 Polymeter Pad", with(with({{kTable, VOWELS}, {kPosition, 40}, {kUnison, 3}, {kDetune, 18}, {kSpread, 90},
                                                {kCutoff, 1500}, {kReso, 25}, {kAAtk, 300}, {kADec, 1000}, {kASus, 90}, {kARel, 1500},
                                                {kADest, kToCutoff}, {kAAmt, 50}, {kALen, 12}, {kASmooth, 40},
                                                {kBDest, kToPos}, {kBAmt, 60}, {kBLen, 7}, {kBSmooth, 50}},
                                               kAStep1, {80, 20, 50, 0, 100, 30, 60, 10, 90, 40, 70, 0, 50, 50, 50, 50}),
                                          kBStep1, {0, 100, 30, 70, 10, 90, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
        program("52 Pitch Lane Riff", with(with({{kTable, HARM}, {kPosition, 55}, {kSlope, 1}, {kCutoff, 1800}, {kReso, 45},
                                                  {kDrive, 25}, {kLfoShape, SINE}, {kLfoRate, 0.1f}, {kLfoCut, 18},
                                                  {kASus, 100}, {kARel, 60},
                                                  {kADest, kToVolume}, {kAAmt, 100}, {kASmooth, 5},
                                                  {kBDest, kToPitch}, {kBAmt, 100}, {kBSmooth, 0}},
                                                 kAStep1, {100, 0, 100, 100, 0, 100, 0, 100, 100, 0, 100, 0, 100, 100, 0, 100}),
                                            kBStep1, {0, 0, st(7), 0, st(10), 0, st(7), st(12), 0, st(3), st(5), 0, st(7), st(10), st(12), st(5)}));
        program("53 Gate Build", with(with({{kPosition, 67}, {kUnison, 5}, {kDetune, 35}, {kSpread, 100}, {kCutoff, 600},
                                            {kReso, 20}, {kAAtk, 10}, {kASus, 100}, {kARel, 300},
                                            {kADest, kToVolume}, {kAAmt, 100}, {kASmooth, 5},
                                            {kBDest, kToCutoff}, {kBAmt, 60}, {kBRate, kDiv1}, {kBSmooth, 100}},
                                           kAStep1, {100, 0, 100, 100, 0, 100, 0, 100, 100, 0, 100, 0, 100, 100, 0, 100}),
                                      kBStep1, {0, 6, 12, 18, 25, 31, 37, 44, 50, 56, 62, 69, 75, 81, 88, 100}));
        program("54 Dub Techno Seq", with(with({{kPosition, 67}, {kFType, 1}, {kCutoff, 700}, {kReso, 35}, {kUnison, 2},
                                                {kDetune, 12}, {kVelAmp, 70}, {kAAtk, 1}, {kADec, 400}, {kASus, 0}, {kARel, 500},
                                                {kFat, 30},
                                                {kADest, kToCutoff}, {kAAmt, 50}, {kALen, 7}, {kARate, kDiv8D}, {kASmooth, 30},
                                                {kBDest, kToRes}, {kBAmt, 40}, {kBLen, 5}, {kBSmooth, 20}},
                                               kAStep1, {20, 90, 40, 100, 0, 60, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
                                          kBStep1, {100, 20, 60, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
        program("55 Acid Seq", with(with({{kPosition, 67}, {kSlope, 1}, {kVoiceMode, kMono}, {kOctave, -1}, {kCutoff, 220},
                                          {kReso, 80}, {kDrive, 60}, {kFat, 30}, {kASus, 100}, {kARel, 60}, {kOutput, -5},
                                          {kADest, kToCutoff}, {kAAmt, 70}, {kASmooth, 0},
                                          {kBDest, kToPitch}, {kBAmt, 100}, {kBSmooth, 15}},
                                         kAStep1, {60, 10, 100, 20, 40, 0, 80, 30, 50, 10, 90, 0, 70, 20, 100, 40}),
                                    kBStep1, {0, 0, st(12), 0, st(3), 0, st(10), st(12), 0, st(7), 0, st(3), st(5), 0, st(12), st(10)}));
        program("56 Wavetable Walk", with(with({{kTable, FM}, {kPosition, 10}, {kWarp, kWarpBend}, {kUnison, 2}, {kDetune, 10},
                                                {kSpread, 80}, {kCutoff, 9000}, {kAAtk, 5}, {kASus, 90}, {kARel, 600},
                                                {kADest, kToPos}, {kAAmt, 90}, {kALen, 8}, {kARate, kDiv8}, {kASmooth, 70},
                                                {kBDest, kToWarp}, {kBAmt, 60}, {kBLen, 6}, {kBSmooth, 30}},
                                               kAStep1, {0, 40, 15, 80, 30, 100, 60, 20, 0, 0, 0, 0, 0, 0, 0, 0}),
                                          kBStep1, {0, 50, 100, 20, 70, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM Wave";
        info.vendor       = "OCKM";    // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', '5', '7', 'W');   // must match plugin.json "uid"
        info.version      = 1200;      // 1.2.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        return info;
    }

    static constexpr int kStack = 16;
    Voice voices_[kVoices];
    VoiceAllocator<kVoices> alloc_;
    Settings set_;
    LFO lfo_;
    Smoother output_;
    Xorshift32 rng_{0x51A7E5u};
    float sr_ = 44100.0f, wheel_ = 0.0f, wheelTarget_ = 0.0f;
    int stack_[kStack] = {}, stackN_ = 0, lastMode_ = -1;

    // Step lanes (shared by every note, like the LFO).
    struct Lane { int dest = 0, len = 16; float amt = 0, smooth = 0, beats = 0.25f; float step[kLaneSteps] = {}; };
    struct LaneMod { float cut = 0, pos = 0, warp = 0, res = 0, pitch = 0; };
    Lane lanes_[2];
    LaneMod mod_;
    float laneOut_[2] = {}, laneGain_ = 1.0f, laneGainPrev_ = 1.0f, fatCoef_ = 0.02f;
    int laneDestPrev_[2] = {-1, -1};
    double laneBeats_ = 0.0, bpm_ = 120.0;
    bool playing_ = false;
};

} // namespace

// Creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKMWave)
