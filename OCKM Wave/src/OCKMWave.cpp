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
//   note -> pitch (octave, glide, bend +-2 st, LFO vibrato)
//        -> WAVETABLE OSC: 8 tables x 16 frames. Position picks / blends two neighbouring
//           frames; Warp bends how the frame is read (Sync, Bend, Mirror, Quantize);
//           Unison plays 1-5 detuned copies spread left-right.
//        +  SUB: a sine one octave below (read from the same band-limited tables)
//        -> FILTER: 12 dB state-variable LP / BP / HP, one per side
//        -> x AMP ENV x velocity
//   sum of voices -> Output level -> L/R
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
#include <vector>

using mpc::floatParam; using mpc::logParam; using mpc::intParam; using mpc::choiceParam;
using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0,   // output level, dB (param 0 is nudged by the host on load: harmless)
    kTable,        // which wavetable (8 generated tables)
    kPosition,     // which frame of the table, 0..100 % (blends between neighbours)
    kWarp,         // how the frame is read: Off / Sync / Bend / Mirror / Quantize
    kWarpAmt,      // how strongly, % (0 = identical to Off)
    kUnison,       // 1..5 oscillator copies per voice
    kDetune,       // copies' pitch spread, % of +-50 cents
    kSpread,       // copies' stereo spread, %
    kSub,          // sine one octave down, %
    kOctave,       // -3..+3
    kVoiceMode,    // Poly (6 voices) / Mono / Legato
    kGlide,        // pitch slide time, ms
    kVelAmp,       // how much velocity changes loudness, %
    kFType,        // LP / BP / HP
    kCutoff,       // Hz
    kReso,         // %
    kKeyTrack,     // cutoff follows the keyboard, % (100 = 1 octave per octave)
    kAAtk, kADec, kASus, kARel,   // amp envelope
    kMAtk, kMDec, kMSus, kMRel,   // mod envelope
    kEnvPos,       // mod env -> Position, +-%
    kEnvWarp,      // mod env -> Warp Amt, +-%
    kEnvCut,       // mod env -> cutoff, +-semitones
    kLfoShape,     // Sine / Triangle / Saw / Square / S&H
    kLfoRate,      // Hz (Sync Off)
    kLfoSync,      // Off / On (locks to the MPC tempo and song position)
    kLfoDiv,       // note division (Sync On)
    kLfoPos,       // LFO -> Position, % (swings +- half this around Position)
    kLfoWarp,      // LFO -> Warp Amt, %
    kLfoCut,       // LFO -> cutoff, semitones
    kLfoPitch,     // LFO -> pitch (vibrato), cents
    kWheelPos,     // mod wheel -> Position, +-%
    kNumParams
};

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
    SVF fL, fR;
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
        addParam(floatParam("output", "Output", -24.0f, 6.0f, -3.0f, "dB", 1));
        addParam(choiceParam("table", "Table", {kTableNames[0], kTableNames[1], kTableNames[2], kTableNames[3],
                                                kTableNames[4], kTableNames[5], kTableNames[6], kTableNames[7]}, 0));
        addParam(floatParam("position", "Position", 0.0f, 100.0f, 67.0f, "%", 0));
        addParam(choiceParam("warp", "Warp", {"Off", "Sync", "Bend", "Mirror", "Quantize"}, 0));
        addParam(floatParam("warpamt", "Warp Amt", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("unison", "Unison", 1, kMaxUnison, 1));
        addParam(floatParam("detune", "Detune", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("spread", "Spread", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("sub", "Sub Level", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("octave", "Octave", -3, 3, 0));
        addParam(choiceParam("voicemode", "Voice Mode", {"Poly", "Mono", "Legato"}, 0));
        addParam(floatParam("glide", "Glide", 0.0f, 1000.0f, 0.0f, "ms", 0, 0.4f));
        addParam(floatParam("velamp", "Vel>Amp", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("ftype", "Filter", {"LP", "BP", "HP"}, 0));
        addParam(logParam("cutoff", "Cutoff", 20.0f, 18000.0f, 18000.0f, "Hz", 0));
        addParam(floatParam("reso", "Resonance", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam("keytrack", "Key Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam("aatk", "Amp Attack", 1.0f, 5000.0f, 2.0f, "ms", 0));
        addParam(logParam("adec", "Amp Decay", 1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(floatParam("asus", "Amp Sustain", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam("arel", "Amp Release", 1.0f, 8000.0f, 250.0f, "ms", 0));
        addParam(logParam("matk", "Mod Attack", 1.0f, 5000.0f, 2.0f, "ms", 0));
        addParam(logParam("mdec", "Mod Decay", 1.0f, 5000.0f, 400.0f, "ms", 0));
        addParam(floatParam("msus", "Mod Sustain", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("mrel", "Mod Release", 1.0f, 8000.0f, 300.0f, "ms", 0));
        addParam(floatParam("envpos", "Env>Pos", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("envwarp", "Env>Warp", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("envcut", "Env>Cutoff", -48.0f, 48.0f, 0.0f, "st", 0));
        addParam(choiceParam("lfoshape", "LFO Shape", LFO::shapeNames(), 1));
        addParam(logParam("lforate", "LFO Rate", 0.05f, 20.0f, 1.0f, "Hz", 2));
        addParam(choiceParam("lfosync", "LFO Sync", {"Off", "On"}, 0));
        addParam(choiceParam("lfodiv", "LFO Div", divisionNames(), kDivisionQuarter));
        addParam(floatParam("lfopos", "LFO>Pos", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lfowarp", "LFO>Warp", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lfocut", "LFO>Cutoff", 0.0f, 48.0f, 0.0f, "st", 0));
        addParam(floatParam("lfopitch", "LFO>Pitch", 0.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("wheelpos", "Wheel>Pos", -100.0f, 100.0f, 0.0f, "%", 0));
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
        }
        lfo_.setSampleRate(sampleRate);
        output_.setTime(20.0f, sampleRate);
        output_.snap(dbToGain(get(kOutput)));
        reset();
    }

    void reset() override
    {
        for (Voice& v : voices_)
        {
            v.amp.reset(); v.mod.reset(); v.fL.reset(); v.fR.reset();
            v.active = false; v.fresh = true; v.fade = 1.0f; v.fadeStep = 0.0f;
        }
        alloc_.reset();
        stackN_ = 0;
        lfo_.reset();
        lastMode_ = -1;
        wheel_ = wheelTarget_ = 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
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

        // Unison: copies evenly spaced from -1 to +1, detuned up to +-50 cents, panned by Spread,
        // and turned down by 1/sqrt(copies) so the stack stays about as loud as one copy.
        const int n = s.unison;
        const float detune = get(kDetune) * 0.01f, spread = get(kSpread) * 0.01f;
        const float uniGain = 1.0f / std::sqrt((float)n);
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
            v.fL.reset(); v.fR.reset();
            v.fresh = true;
        }
        if (retrigger || wasSilent) { v.amp.noteOn(); v.mod.noteOn(); v.peaked = false; }
        v.active = true;
    }

    void noteOn(int note, int vel)
    {
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

        float mixL[kCtrl] = {}, mixR[kCtrl] = {};
        for (int vi = 0; vi < kVoices; ++vi)
        {
            Voice& v = voices_[vi];
            if (!v.active) continue;
            control(v, lfo, n);
            switch (s.warp)
            {
                case kWarpSync:     renderVoice<kWarpSync>(v, mixL, mixR, n); break;
                case kWarpBend:     renderVoice<kWarpBend>(v, mixL, mixR, n); break;
                case kWarpMirror:   renderVoice<kWarpMirror>(v, mixL, mixR, n); break;
                case kWarpQuantize: renderVoice<kWarpQuantize>(v, mixL, mixR, n); break;
                default:            renderVoice<kWarpOff>(v, mixL, mixR, n); break;
            }
            if (v.amp.level() > 0.5f) v.peaked = true;               // the attack has been through
            const bool silentPluck = v.peaked && s.ampSusZero && v.amp.level() < 0.0005f;   // decayed to nothing, key still down
            if (!v.amp.isActive() || v.fade <= 0.0f || silentPluck)
            {
                v.active = false; v.amp.reset(); v.mod.reset();
                if (s.mode == kPoly) alloc_.voiceFinished(vi);
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const float g = output_.process();
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
        const float semis = v.pitch + 12.0f * (float)s.octave + alloc_.bendSemitones() + s.lfoPitch * lfo;
        const float hz = 440.0f * std::exp2((semis - 69.0f) * (1.0f / 12.0f));
        for (int c = 0; c < s.unison; ++c) v.inc[c] = std::min(hz * s.uniRatio[c] / sr_, 0.49f);
        v.subInc = std::min(0.5f * hz / sr_, 0.49f);

        // Position: knob + mod env + LFO + mod wheel, ramped across the step.
        const float pos = clamp(s.position + s.envPos * menv + s.lfoPos * lfo + s.wheelPos * wheel_, 0.0f, 1.0f);
        if (v.fresh) { v.pos = pos; v.fresh = false; }
        v.posStep = (pos - v.pos) / (float)n;

        // Warp amount, and how much it raises the highest harmonic (to pick a duller mip level).
        const float w = clamp(s.warpAmt + s.envWarp * menv + s.lfoWarp * lfo, 0.0f, 1.0f);
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
        const float cutSemis = s.keyTrack * (v.pitch + 12.0f * (float)s.octave - 60.0f) + s.envCut * menv + s.lfoCut * lfo;
        const float cut = clamp(s.cutoff * std::exp2(cutSemis * (1.0f / 12.0f)), 20.0f, 0.45f * sr_);
        v.fL.set(cut, s.reso); v.fR.set(cut, s.reso);
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

    template <int W>
    void renderVoice(Voice& v, float* mixL, float* mixR, int n)
    {
        const Settings& s = set_;
        const int size = v.size, stride = v.frameStride, uni = s.unison;
        const float warpA = v.warpA, warpB = v.warpB, warpC = v.warpC;
        const float gain = kVoiceGain * v.velGain;
        float pos = v.pos;
        for (int i = 0; i < n; ++i)
        {
            // Position -> the two neighbouring frames and how far between them.
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
            const float l = v.fL.process(sumL, s.ftype), r = v.fR.process(sumR, s.ftype);
            float g = v.amp.process() * gain;
            if (v.fadeStep > 0.0f) { v.fade -= v.fadeStep; if (v.fade < 0.0f) v.fade = 0.0f; g *= v.fade; }
            mixL[i] += l * g;
            mixR[i] += r * g;
        }
        v.pos = clamp(pos, 0.0f, 1.0f);
    }

    // ----- presets ------------------------------------------------------------
    // Lessons 01-16 each show one idea (see README "Lessons"); 17-24 are track-ready sounds.
    // Each preset starts from the defaults (= "01 Init Saw") and changes only what it lists.
    void addPrograms()
    {
        struct KV { int i; float v; };
        auto program = [this](const char* name, std::initializer_list<KV> kv)
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
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM Wave";
        info.vendor       = "OCKM";    // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', '5', '7', 'W');   // must match plugin.json "uid"
        info.version      = 1000;      // 1.0.0.0
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
};

} // namespace

// Creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKMWave)
