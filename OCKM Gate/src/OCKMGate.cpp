// OCKMGate.cpp - OCKM Gate, a tempo-locked trance gate / pump effect for the MPC.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// See ../SPEC.md for the design. Signal flow:
//
//   input L/R
//     -> StepClock: position in the bar (MPC song position while playing, own clock when
//        stopped), Rate, Swing, Steps (pattern length, restarts every bar)
//     -> PulseEnvelope: every On step TRIGGERS an envelope (Gate / Pump / Smooth / Ping-Pong)
//        shaped by Attack, Length, Release, Curve and scaled by the step's Level.
//        Triggered envelopes run on across Off steps, so a pump keeps recovering through them.
//     -> gain = 1 - Mix * Depth * (1 - envelope)  per channel (Ping-Pong sends alternate
//        triggers left / right; Width sets how far)
//     -> 0.3 ms de-click smoothing -> Output level -> L/R
//
// Tap Rec: while armed and the MPC is playing, each press of TAP switches on the step nearest
// to "now". The press is only flagged on the UI thread; process() applies it at the exact
// song position and tells the host which step parameters changed.
//
// Realtime: no allocation, locks or I/O in process(). Per sample: a few adds/multiplies, one
// floor, one table lookup. The curve table is rebuilt only when Curve changes.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/smoother.h"

#include <atomic>
#include <cmath>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kOutput = 0,   // output level, dB (param 0 is nudged by the host on load: harmless)
    kMode,         // Gate / Pump / Smooth / PingPong
    kDepth,        // how far the gate closes, %
    kMix,          // dry/wet, %
    kRate,         // step length: 1/8, 1/8T, 1/16, 1/16T, 1/32
    kAttack,       // opening (or ducking) time, ms
    kLength,       // gate open time / swell peak, % of a step
    kRelease,      // closing / recovery time, % of a step (up to 4 steps)
    kCurve,        // release shape: - slow-then-fast ... + snappy
    kSwing,        // 50..75 %, delays every second step
    kSteps,        // pattern length 1..16 (restarts every bar)
    kWidth,        // Ping-Pong stereo spread, %
    kRunStop,      // keep pulsing when the MPC is stopped
    kTapRec,       // arm tap-in recording
    kTap,          // any change = one tap
    kOn1,          // 16 step on/off switches (kOn1 .. kOn1 + 15)
    kLv1 = kOn1 + 16,   // 16 step levels, %
    kNumParams = kLv1 + 16
};

enum Mode { kGate = 0, kPump, kSmooth, kPingPong };

constexpr int kMaxSteps = 16;
constexpr int kCurveTable = 64;
const float kRateBeats[5] = {0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};

// ---------------------------------------------------------------------------
// StepClock: where are we in the bar, which step, how far into it.
// ---------------------------------------------------------------------------
struct StepPos
{
    int   stepInBar = 0;   // step counted from the bar start (before wrapping to Steps)
    float phase = 0.0f;    // 0..1 inside the current step
};

inline StepPos locate(double posInBar, double stepBeats, float swing)
{
    // Steps come in pairs; swing moves the boundary inside each pair.
    const double pairLen = 2.0 * stepBeats;
    const double pair = std::floor(posInBar / pairLen);
    const double inPair = posInBar - pair * pairLen;
    const double split = pairLen * swing;          // 0.5 = straight
    StepPos p;
    if (inPair < split) { p.stepInBar = (int)pair * 2;     p.phase = (float)(inPair / split); }
    else                { p.stepInBar = (int)pair * 2 + 1; p.phase = (float)((inPair - split) / (pairLen - split)); }
    return p;
}

// ---------------------------------------------------------------------------
// PulseEnvelope: one envelope, retriggered by each On step. Time is in steps.
// ---------------------------------------------------------------------------
class PulseEnvelope
{
public:
    void reset() { tau_ = 1.0e9f; level_ = 0.0f; start_ = idleValue(); value_ = start_; side_ = 1; }

    void setShape(int mode, float attackSteps, float length, float release, const float* curve)
    {
        mode_ = mode; att_ = attackSteps; len_ = length; rel_ = release; curve_ = curve;
        if (len_ < att_) len_ = att_;
    }

    void trigger(float level)
    {
        start_ = value_;      // continue from wherever we are: no jumps
        level_ = level;
        tau_ = 0.0f;
        side_ ^= 1;           // Ping-Pong alternates sides per trigger
    }

    // Advance by dt steps and return the envelope (0..1, 1 = fully open).
    float next(float dt)
    {
        tau_ += dt;
        const float t = tau_;
        float g;
        switch (mode_)
        {
            case kPump:
            {
                const float low = 1.0f - level_;
                if (t < att_) g = start_ + (low - start_) * (t / att_);
                else
                {
                    const float x = (t - att_) / rel_;
                    g = x >= 1.0f ? 1.0f : low + level_ * (1.0f - fall(x));
                }
                break;
            }
            case kSmooth:
            {
                // Half-cosine rise to the peak at Length, half-cosine fall over Release.
                if (t < len_)
                {
                    const float x = t / len_;
                    g = start_ + (level_ - start_) * (0.5f - 0.5f * std::cos(kPi * x));
                }
                else
                {
                    const float x = (t - len_) / rel_;
                    g = x >= 1.0f ? 0.0f : level_ * (0.5f + 0.5f * std::cos(kPi * x));
                }
                break;
            }
            case kGate:
            case kPingPong:
            default:
            {
                if (t < att_) g = start_ + (level_ - start_) * (t / att_);
                else if (t < len_) g = level_;
                else
                {
                    const float x = (t - len_) / rel_;
                    g = x >= 1.0f ? 0.0f : level_ * fall(x);
                }
                break;
            }
        }
        value_ = g;
        return g;
    }

    float value() const { return value_; }
    int side() const { return side_; }
    float idleValue() const { return mode_ == kPump ? 1.0f : 0.0f; }

private:
    // Release curve, 1 -> 0 over x = 0..1, from the shared table.
    float fall(float x) const
    {
        const float f = x * (float)(kCurveTable - 1);
        const int i = (int)f;
        if (i >= kCurveTable - 1) return curve_[kCurveTable - 1];
        return curve_[i] + (curve_[i + 1] - curve_[i]) * (f - (float)i);
    }

    int mode_ = kGate, side_ = 1;
    float tau_ = 1.0e9f, level_ = 0.0f, start_ = 0.0f, value_ = 0.0f;
    float att_ = 0.01f, len_ = 0.5f, rel_ = 0.15f;
    const float* curve_ = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class OCKMGate : public mpc::Plugin
{
public:
    OCKMGate() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("output", "Output", -24.0f, 6.0f, 0.0f, "dB", 1));
        addParam(choiceParam("mode", "Mode", {"Gate", "Pump", "Smooth", "PingPong"}, 0));
        addParam(floatParam("depth", "Depth", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(choiceParam("rate", "Rate", {"1/8", "1/8T", "1/16", "1/16T", "1/32"}, 2));
        addParam(logParam("attack", "Attack", 0.5f, 50.0f, 2.0f, "ms", 1));
        addParam(floatParam("length", "Length", 5.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("release", "Release", 1.0f, 400.0f, 15.0f, "%", 0, 0.5f));
        addParam(floatParam("curve", "Curve", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("swing", "Swing", 50.0f, 75.0f, 50.0f, "%", 0));
        addParam(intParam("steps", "Steps", 1, 16, 16));
        addParam(floatParam("width", "Width", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(boolParam("runstop", "Run Stopped", true));
        addParam(boolParam("taprec", "Tap Rec", false));
        addParam(boolParam("tap", "TAP", false));
        for (int i = 0; i < kMaxSteps; ++i)
        {
            std::snprintf(idBuf_, sizeof(idBuf_), "on%d", i + 1);
            std::snprintf(nameBuf_, sizeof(nameBuf_), "Step %d", i + 1);
            addParam(boolParam(idBuf_, nameBuf_, true));
        }
        for (int i = 0; i < kMaxSteps; ++i)
        {
            std::snprintf(idBuf_, sizeof(idBuf_), "lv%d", i + 1);
            std::snprintf(nameBuf_, sizeof(nameBuf_), "Lvl %d", i + 1);
            addParam(floatParam(idBuf_, nameBuf_, 0.0f, 100.0f, 100.0f, "%", 0));
        }
        if (numParams() != kNumParams) mpc::log("OCKM Gate: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        addPrograms();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        output_.setTime(10.0f, sampleRate);
        declick_ = onePoleCoeff(0.3f, sampleRate);
        reset();
    }

    void reset() override
    {
        env_.reset();
        gainL_ = gainR_ = 1.0f;
        freePos_ = 0.0;
        lastStep_ = -1;
        wasPlaying_ = false;
        curveValue_ = 1.0e9f;
        output_.snap(dbToGain(get(kOutput)));
    }

    void parameterChanged(int index, float plainValue) override
    {
        // UI thread. Only flag things here; process() acts on them.
        // A tap is a real change of the TAP value. Loading a preset or project rewrites every
        // parameter, usually with the same value, and must never count as a tap.
        if (index == kTap)
        {
            const bool v = plainValue >= 0.5f;
            if (v != lastTapValue_.exchange(v, std::memory_order_relaxed)) tapPending_.store(true, std::memory_order_relaxed);
        }
        else if (index == kTapRec)
        {
            const bool v = plainValue >= 0.5f;
            if (v && !lastTapRec_.exchange(v, std::memory_order_relaxed)) clearOnFirstTap_.store(true, std::memory_order_relaxed);
            else lastTapRec_.store(v, std::memory_order_relaxed);
        }
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        const float* inL = ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // ----- block settings -----
        const mpc::Transport& tr = ctx.transport;
        const double bpm = (tr.tempoValid && tr.bpm > 1.0) ? tr.bpm : 120.0;
        const double barBeats = barLength(tr);
        const int rate = getInt(kRate);
        const double stepBeats = kRateBeats[clamp(rate, 0, 4)];
        const float swing = get(kSwing) * 0.01f;
        const int steps = clamp(getInt(kSteps), 1, kMaxSteps);
        const int mode = getInt(kMode);
        const float depth = get(kDepth) * 0.01f, mix = get(kMix) * 0.01f, width = get(kWidth) * 0.01f;
        const double beatsPerSample = bpm / 60.0 / sr_;
        const float stepsPerSample = (float)(beatsPerSample / stepBeats);
        const float stepMs = (float)(stepBeats * 60000.0 / bpm);
        updateCurve(get(kCurve) * 0.01f);
        env_.setShape(mode, get(kAttack) / stepMs, get(kLength) * 0.01f,
                      std::fmax(0.01f, get(kRelease) * 0.01f), curveTable_);
        bool on[kMaxSteps]; float lv[kMaxSteps];
        for (int i = 0; i < kMaxSteps; ++i) { on[i] = getBool(kOn1 + i); lv[i] = get(kLv1 + i) * 0.01f; }
        output_.setTarget(dbToGain(get(kOutput)));

        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const bool running = playing || getBool(kRunStop);
        double pos;
        if (playing)
        {
            double inBar = tr.ppqPos - tr.barStartPpq;
            if (!(inBar >= 0.0 && inBar < barBeats + 1.0e-6)) inBar = std::fmod(tr.ppqPos, barBeats);
            if (inBar < 0.0) inBar += barBeats;
            pos = inBar;
            if (!wasPlaying_) lastStep_ = -1;          // MPC just started: retrigger cleanly
        }
        else pos = freePos_;
        wasPlaying_ = playing;

        // ----- Tap Rec -----
        if (tapPending_.exchange(false, std::memory_order_relaxed) && getBool(kTapRec) && playing)
            applyTap(pos, stepBeats, swing, steps);

        // ----- audio -----
        for (int i = 0; i < n; ++i)
        {
            float targetL = 1.0f, targetR = 1.0f;
            if (running)
            {
                const StepPos sp = locate(pos, stepBeats, swing);
                if (sp.stepInBar != lastStep_)
                {
                    lastStep_ = sp.stepInBar;
                    const int idx = sp.stepInBar % steps;
                    if (on[idx] && lv[idx] > 0.0f) env_.trigger(lv[idx]);
                }
                const float g = env_.next(stepsPerSample);
                float gl = g, gr = g;
                if (mode == kPingPong)
                {
                    if (env_.side() == 0) gr = g * (1.0f - width);
                    else                  gl = g * (1.0f - width);
                }
                targetL = 1.0f - mix * depth * (1.0f - gl);
                targetR = 1.0f - mix * depth * (1.0f - gr);
                pos += beatsPerSample;
                if (pos >= barBeats) { pos -= barBeats; lastStep_ = -1; }
            }
            gainL_ += declick_ * (targetL - gainL_);
            gainR_ += declick_ * (targetR - gainR_);
            if (std::fabs(targetL - gainL_) < 1.0e-6f) gainL_ = targetL;   // settle exactly: no denormal tails
            if (std::fabs(targetR - gainR_) < 1.0e-6f) gainR_ = targetR;
            const float out = output_.process();
            const float l = inL ? inL[i] : 0.0f;          // read both inputs before writing (in-place buffers)
            const float r = inR ? inR[i] : l;
            outL[i] = l * gainL_ * out;
            if (outR) outR[i] = r * gainR_ * out;
        }
        if (!playing) freePos_ = pos;
    }

private:
    static double barLength(const mpc::Transport& tr)
    {
        if (tr.timeSigNumerator > 0 && tr.timeSigDenominator > 0)
            return 4.0 * tr.timeSigNumerator / tr.timeSigDenominator;
        return 4.0;
    }

    void updateCurve(float c)
    {
        if (c == curveValue_) return;
        curveValue_ = c;
        // fall(x) = (1 - x)^p: p > 1 drops fast (snappy), p < 1 holds then drops.
        const float p = std::pow(2.0f, c * 2.0f);
        for (int i = 0; i < kCurveTable; ++i)
        {
            const float x = (float)i / (float)(kCurveTable - 1);
            curveTable_[i] = std::pow(1.0f - x, p);
        }
    }

    void applyTap(double pos, double stepBeats, float swing, int steps)
    {
        // Nearest step: look at where we are in the current step.
        const StepPos sp = locate(pos, stepBeats, swing);
        int step = sp.stepInBar + (sp.phase >= 0.5f ? 1 : 0);
        step %= steps;
        if (clearOnFirstTap_.exchange(false, std::memory_order_relaxed))
            for (int i = 0; i < kMaxSteps; ++i)
                if (i != step && getBool(kOn1 + i)) { set(kOn1 + i, 0.0f); notifyHostParameterChanged(kOn1 + i); }
        if (!getBool(kOn1 + step)) { set(kOn1 + step, 1.0f); notifyHostParameterChanged(kOn1 + step); }
    }

    // Built-in programs: a rhythm plus a matching pulse shape.
    // Pattern letters: X = 100 %, x = 65 %, g = 30 % (ghost), . = off.
    void addPrograms()
    {
        struct Shape { int mode; float attack, length, release, curve, depth, swing; int rate; float width; };
        auto program = [this](const char* name, const char* pattern, Shape s)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            v[kMode] = (float)s.mode; v[kAttack] = s.attack; v[kLength] = s.length; v[kRelease] = s.release;
            v[kCurve] = s.curve; v[kDepth] = s.depth; v[kSwing] = s.swing; v[kRate] = (float)s.rate; v[kWidth] = s.width;
            for (int i = 0; i < kMaxSteps; ++i)
            {
                const char c = pattern[i];
                v[(size_t)(kOn1 + i)] = c == '.' ? 0.0f : 1.0f;
                v[(size_t)(kLv1 + i)] = c == 'x' ? 65.0f : (c == 'g' ? 30.0f : 100.0f);
            }
            addProgram(name, std::move(v));
        };
        //                                          mode       att  len  rel   curve depth swing rate width
        program("Init",              "XXXXXXXXXXXXXXXX", {kGate,     2.0f, 50, 15,    0, 100, 50, 2, 100});
        program("Classic 16ths",     "X.XX.XX.X.XX.XX.", {kGate,     1.0f, 60, 10,   30, 100, 50, 2, 100});
        program("Offbeat Pump",      "X...X...X...X...", {kPump,     1.0f, 50, 150,  40, 100, 50, 2, 100});
        program("Quarter Pump",      "X...X...X...X...", {kPump,     3.0f, 50, 360, -40,  80, 50, 2, 100});
        program("3-3-2",             "X..X..X.X..X..X.", {kGate,     1.0f, 70, 15,   20, 100, 50, 2, 100});
        program("Ghost Chop",        "XggXgXggXggXgXgg", {kGate,     1.0f, 55, 12,   30, 100, 54, 2, 100});
        program("Smooth Pulse",      "X.X.X.X.X.X.X.X.", {kSmooth,   5.0f, 50, 100,   0,  85, 50, 2, 100});
        program("Ping-Pong Stutter", "XXXXXXXXXXXXXXXX", {kPingPong, 1.0f, 45, 15,   20, 100, 50, 2, 100});
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM Gate";
        info.vendor       = "OCKM";    // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', 'y', 'S', 'Y');   // must match plugin.json "uid"
        info.version      = 1000;      // 1.0.0.0
        info.isInstrument = false;
        info.numInputs    = 2;
        info.numOutputs   = 2;
        info.tailSamples  = 1;         // no tail
        return info;
    }

    PulseEnvelope env_;
    Smoother output_;
    double sr_ = 44100.0, freePos_ = 0.0;
    float gainL_ = 1.0f, gainR_ = 1.0f, declick_ = 1.0f;
    float curveTable_[kCurveTable] = {};
    float curveValue_ = 1.0e9f;
    int lastStep_ = -1;
    bool wasPlaying_ = false;
    std::atomic<bool> tapPending_{false}, clearOnFirstTap_{false}, lastTapValue_{false}, lastTapRec_{false};
    char idBuf_[16] = {}, nameBuf_[16] = {};
};

// Creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKMGate)
