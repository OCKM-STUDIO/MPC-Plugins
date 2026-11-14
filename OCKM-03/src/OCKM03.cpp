// OCKM03.cpp - OCKM-03, a lean monophonic acid bass instrument for the MPC.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// One job: squelchy, slidey acid basslines played from the MPC's own sequencer.
// See ../SPEC.md for the design and the reasons behind it.
//
// Signal flow (one mono voice):
//
//   MIDI -> NoteInput (note stack, last-note priority, slide + accent decisions)
//        -> Mutator (v1.2: repeatable per-step octave jumps, slide-ins and rests)
//        -> AcidVoice:
//             Oscillator (PolyBLEP saw or slightly narrow square)
//          -> Filter: 4-pole zero-delay-feedback ladder low-pass, run at 2x the
//             sample rate, saturated (tanh) input and feedback
//             cutoff (octaves) = base + EnvMod*filterEnv + accent*(env push + accumulator)
//                                + 0.5 * (pitch - C1) / 12   (key tracking, v1.2)
//          -> Amp: gate with a long fixed decay, 1 ms declick attack, 8 ms release,
//             accent boost
//          -> Drive (soft-clip, gain compensated, 0 = clean)
//   -> Low cut (fixed 30 Hz, 12 dB/oct, v1.2) -> Volume
//   -> output ceiling (transparent below -3 dBFS) -> both outputs
//
// Code layout (kept separate so a step sequencer can be added later):
//   NoteInput  - knows MIDI, knows nothing about sound.
//   AcidVoice  - knows sound, knows nothing about MIDI. Only noteOn/noteOff/setTargets.
//   OCKM03     - the plugin: parameters, presets.
//
// v1.2: Mutate (repeatable per-step octave jumps, slide-ins and rests; parameters 26-29,
// see SPEC section 11), filter key tracking and a fixed 30 Hz low cut.
//
// v1.1: back to the classic panel. The four macros and their eleven recipe depths
// (parameters 11-25) are retired: they stay registered, because parameter ids and
// order are permanent, but they are hidden from the screen and Q-Links and have no
// effect on the sound, so an old project with a macro turned up still sounds right.
//
// Arp-friendly accents (v1.1): an arp often plays every note at full velocity, so
// every note is accented. Accented notes still decay shorter than normal ones, but
// Decay now scales them too, and the accent build-up drains with Decay and is capped,
// so a steady stream of accents doesn't pin the filter open and the knobs keep working.
//
// Realtime rules: no allocation, locks or I/O in process(); pow/tan only every
// 16 samples; the per-sample path is adds, multiplies and two rational tanh.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/smoother.h"

#include <cmath>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kVolume = 0,   // master volume, dB (param 0 is nudged by the host on load: harmless)
    kWave,         // Saw / Square
    kCutoff,       // filter cutoff, 0..100 % of knob travel (shown in Hz)
    kReso,         // resonance, 0..100 %
    kEnvMod,       // filter envelope depth, 0..100 %
    kDecay,        // filter envelope decay, 0..100 % (shown in ms)
    kAccent,       // how hard accented notes hit, 0..100 %
    kTune,         // tuning, -12..+12 semitones
    kSlide,        // slide (glide) time, 0..100 % (shown in ms)
    kDrive,        // output drive, 0..100 %
    kAccThr,       // velocity at or above which a note is accented, 1..127
    // Retired in v1.1 (kept only so saved projects keep their parameter order; no effect):
    kSquelch, kBite, kRubber, kGrit,          // the four macros
    kSqCut, kSqRes, kSqEnv,                   // their recipe depths
    kBtAcc, kBtDec, kBtDrv,
    kRbSld, kRbDec, kRbCut,
    kGrDrv, kGrRes,
    // v1.2 Mutate (appended): repeatable per-step changes to the notes played.
    kMuOct,        // chance per note of jumping up an octave, 0..100 %
    kMuSld,        // chance per note of sliding in from the previous note, 0..100 %
    kMuRst,        // chance per note of being left out (a rest), 0..100 %
    kMuVar,        // which repeatable set of mutations, 1..64
    kNumParams
};

// Mappings from normalised knob travel (0..1) to the real quantity.
inline float cutoffHz(float n)   { return 20.0f * std::pow(400.0f, n); }      // 20 Hz .. 8 kHz (0 = nearly silent)
inline float decayMs(float n)    { return 50.0f * std::pow(50.0f, n); }       // 50 ms .. 2.5 s
inline float slideMs(float n)    { return 10.0f * std::pow(30.0f, n); }       // 10 .. 300 ms
inline float coeffForMs(float ms, float sr) { return 1.0f - std::exp(-1.0f / std::fmax(1.0f, ms * 0.001f * sr)); }
inline float decayPerSample(float ms, float sr) { return std::exp(-1.0f / std::fmax(1.0f, ms * 0.001f * sr)); }

// ---------------------------------------------------------------------------
// Filter: 4-pole ladder, zero-delay-feedback (TPT) one-pole stages, with a
// saturating input stage. Runs at 2x the host rate (input linearly interpolated,
// output averaged) so resonance and saturation stay clean near the top.
// ---------------------------------------------------------------------------
class LadderFilter
{
public:
    void reset() { s1_ = s2_ = s3_ = s4_ = 0.0f; prevIn_ = 0.0f; }

    // fc in Hz, res 0..1, osRate = 2 * host rate. Call at control rate.
    void set(float fc, float res, float osRate)
    {
        fc = clamp(fc, 20.0f, 0.45f * osRate * 0.5f);
        const float g = std::tan(kPi * fc / osRate);
        G_ = g / (1.0f + g);
        k_ = 4.15f * res;                                    // top of the range sings at the edge of self-oscillation
        comp_ = 1.0f + 0.12f * k_;                           // only a little bass make-up: high resonance thins out
    }

    float process(float in)
    {
        const float mid = 0.5f * (prevIn_ + in);
        prevIn_ = in;
        const float a = tick(mid);
        const float b = tick(in);
        return 0.5f * (a + b);
    }

private:
    float tick(float x)
    {
        const float G = G_;
        // Linear estimate of the 4th stage output to resolve the feedback loop.
        const float sigma = G * G * G * s1_ + G * G * s2_ + G * s3_ + s4_;
        const float G4 = G * G * G * G;
        // Each stage is y = G*x + (1-G)*s, so y4 = G^4*u + (1-G)*sigma with u = x - k*y4.
        const float y4est = (G4 * x * comp_ + (1.0f - G) * sigma) / (1.0f + k_ * G4);
        // Saturated input: this is where the squelch and the rubbery resonance come from.
        const float u = fastTanh(x * comp_ - k_ * y4est);
        const float y1 = stage(u, s1_, G);
        const float y2 = stage(y1, s2_, G);
        const float y3 = stage(y2, s3_, G);
        return stage(y3, s4_, G);
    }
    static float stage(float x, float& s, float G)
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }

    float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f, prevIn_ = 0.0f;
    float G_ = 0.1f, k_ = 0.0f, comp_ = 1.0f;
};

// ---------------------------------------------------------------------------
// AcidVoice: the sound. Knows nothing about MIDI.
// ---------------------------------------------------------------------------
struct VoiceTargets            // knob values, normalised 0..1
{
    int   wave = 0;
    float cutoff = 0.3f, reso = 0.6f, envMod = 0.5f, decay = 0.35f, accent = 0.5f;
    float slide = 0.2f, drive = 0.0f, tuneSemis = 0.0f;
};

class AcidVoice
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        osc_.setSampleRate(sampleRate);
        attackCoef_ = coeffForMs(1.0f, sr_);
        releaseCoef_ = coeffForMs(8.0f, sr_);
        ampDecay_ = decayPerSample(3000.0f, sr_);
        accSmoothCoef_ = coeffForMs(8.0f, sr_);
        rampCoef_ = coeffForMs(10.0f, sr_);
        reset();
    }

    void reset()
    {
        filter_.reset();
        gate_ = false; gateLevel_ = 0.0f; amp_ = 0.0f; fenv_ = 0.0f;
        accum_ = accumS_ = 0.0f; accented_ = false;
        pitch_ = pitchTarget_ = 36.0f;
        ctrl_ = 0;
        first_ = true;
    }

    // Called once per block with the knob values.
    void setTargets(const VoiceTargets& t)
    {
        t_ = t;
        osc_.setWave(t.wave == 0 ? Wave::Saw : Wave::Square);
        osc_.setPulseWidth(0.46f);                               // slightly narrow: hollow, reedy square
        dcFix_ = t.wave == 0 ? 0.0f : -(2.0f * 0.46f - 1.0f);    // square averages 2*pw-1; cancel it
        decayNormal_ = decayPerSample(decayMs(t.decay), sr_);
        // Accented notes decay shorter (snappier), but still follow Decay: 50 ms .. about 0.9 s.
        const float dms = decayMs(t.decay);
        decayAccent_ = decayPerSample(std::fmin(dms, 0.3f * dms + 120.0f), sr_);
        slideCoef_ = coeffForMs(slideMs(t.slide), sr_);
        // Accent build-up drains with a time tied to Decay (about 70 ms .. 0.7 s).
        accumDecay_ = decayPerSample(0.25f * dms + 60.0f, sr_);
        if (first_) { cut_ = t.cutoff; res_ = t.reso; env_ = t.envMod; acc_ = t.accent; drv_ = t.drive; first_ = false; }
    }

    // pitch = MIDI note, accent = this note is accented, slide = glide without retrigger.
    void noteOn(int pitch, bool accent, bool slide)
    {
        pitchTarget_ = (float)pitch;
        accented_ = accent;
        if (accent) accum_ = std::fmin(accum_ + 1.0f, kAccumMax); // consecutive accents build up
        if (slide && gate_) return;                              // legato: glide, keep envelopes
        pitch_ = pitchTarget_;                                   // fresh note: jump, retrigger
        gate_ = true;
        gateLevel_ = 1.0f;
        fenv_ = 1.0f;
        ctrl_ = 0;
    }

    // v1.2 Mutate "slide in": glide from the previous pitch WITHOUT re-plucking the filter,
    // even if the previous note has already been released (the amp re-opens).
    void slideIn(int pitch, bool accent)
    {
        if (gate_) { noteOn(pitch, accent, true); return; }      // still held: a normal slide
        pitchTarget_ = (float)pitch;
        accented_ = accent;
        if (accent) accum_ = std::fmin(accum_ + 1.0f, kAccumMax);
        gate_ = true;
        gateLevel_ = 1.0f;                                       // amp re-opens; fenv_ keeps going
    }

    void noteOff() { gate_ = false; }

    // Adds the voice into out[0..n).
    void render(float* out, int n)
    {
        if (!gate_ && amp_ == 0.0f)
        {
            // Silent: skip the DSP, but let the accent accumulator keep discharging.
            accum_ *= std::pow(accumDecay_, (float)n);
            if (accum_ < 1.0e-4f) accum_ = 0.0f;
            accumS_ = accum_;
            return;
        }
        const float fdec = accented_ ? decayAccent_ : decayNormal_;
        const float osRate = 2.0f * sr_;
        for (int i = 0; i < n; ++i)
        {
            if ((ctrl_++ & 15) == 0)
            {
                // Control rate (every 16 samples): smooth the knobs, then pitch and cutoff.
                const float r = rampCoef_ * 16.0f > 1.0f ? 1.0f : rampCoef_ * 16.0f;
                cut_ += r * (t_.cutoff - cut_); res_ += r * (t_.reso - res_);
                env_ += r * (t_.envMod - env_); acc_ += r * (t_.accent - acc_);
                drv_ += r * (t_.drive - drv_);

                osc_.setFrequency(midiToHz(pitch_ + t_.tuneSemis));
                const float accAmt = accented_ ? acc_ : 0.0f;
                const float octaves = env_ * kEnvOctaves * fenv_
                                    + accAmt * (1.2f * fenv_ + 1.0f * accumS_);
                // Key tracking: the cutoff follows half the pitch distance from C1 (MIDI 36), so
                // higher and octave-jumped notes stay as bright as the rest.
                const float keyOct = kKeyTrack * (pitch_ + t_.tuneSemis - 36.0f) * (1.0f / 12.0f);
                filter_.set(cutoffHz(cut_) * std::pow(2.0f, clamp(octaves, 0.0f, 9.0f) + clamp(keyOct, -3.0f, 4.0f)), res_, osRate);
                // Progressive drive: gain into the clipper grows with the square of the knob,
                // so the whole travel is usable (it used to be fully saturated by 25 %).
                drivePre_ = 1.0f + 11.0f * drv_ * drv_;
                driveMix_ = clamp(drv_ * 2.0f, 0.0f, 1.0f);
                driveNorm_ = 0.5f / fastTanh(0.5f * drivePre_);
                ampBoost_ = 1.0f + 1.6f * (accented_ ? acc_ : 0.0f);   // accent: up to +8 dB
            }

            pitch_ += slideCoef_ * (pitchTarget_ - pitch_);      // exponential glide in semitones
            fenv_ *= fdec;
            accum_ *= accumDecay_;
            accumS_ += accSmoothCoef_ * (accum_ - accumS_);

            float target;
            if (gate_) { gateLevel_ *= ampDecay_; target = gateLevel_ * ampBoost_; amp_ += attackCoef_ * (target - amp_); }
            else
            {
                amp_ -= releaseCoef_ * amp_;
                if (amp_ < 1.0e-5f) { amp_ = 0.0f; filter_.reset(); n = i + 1; }   // tail done: end cleanly
            }
            if (fenv_ < 1.0e-6f) fenv_ = 0.0f;

            // dcFix_ cancels the narrow square's DC offset, which would otherwise leak through a closed filter.
            const float x = filter_.process((osc_.process() + dcFix_) * 0.8f) * amp_;
            float y = x;
            if (driveMix_ > 0.0f)
                y = x + driveMix_ * (fastTanh(x * drivePre_) * driveNorm_ - x);
            out[i] += y;
        }
    }

private:
    static constexpr float kEnvOctaves = 5.5f;   // Env Mod at 100 %: the envelope sweeps 5.5 octaves
    static constexpr float kAccumMax = 1.6f;     // cap on the accent build-up (in accent steps)
    static constexpr float kKeyTrack = 0.5f;     // filter key tracking: 0.5 octave per octave played
    PolyBlepOsc osc_;
    LadderFilter filter_;
    VoiceTargets t_;
    float sr_ = 44100.0f;
    bool gate_ = false, accented_ = false, first_ = true;
    float gateLevel_ = 0.0f, amp_ = 0.0f, fenv_ = 0.0f, accum_ = 0.0f, accumS_ = 0.0f;
    float pitch_ = 36.0f, pitchTarget_ = 36.0f;
    float attackCoef_ = 1.0f, releaseCoef_ = 1.0f, ampDecay_ = 1.0f, accSmoothCoef_ = 1.0f, rampCoef_ = 1.0f;
    float decayNormal_ = 0.999f, decayAccent_ = 0.999f, slideCoef_ = 1.0f, accumDecay_ = 0.999f;
    float cut_ = 0.3f, res_ = 0.6f, env_ = 0.5f, acc_ = 0.5f, drv_ = 0.0f;
    float drivePre_ = 1.0f, driveMix_ = 0.0f, driveNorm_ = 1.0f, ampBoost_ = 1.0f, dcFix_ = 0.0f;
    int ctrl_ = 0;
};

// ---------------------------------------------------------------------------
// Mutator (v1.2): repeatable, per-step decisions. Knows nothing about MIDI or sound.
// Each decision is a pure function of (step in the bar, Variation, lane): the same step
// gets the same answer on every pass of the loop. Each Variation shuffles the 16 steps
// into a fixed order (per lane); an amount of N/16 mutates the first N steps of that
// order. So 25 % is always exactly 4 of 16 steps, 50 % exactly 8, and the amounts are
// nested: every step mutated at 30 % is still mutated at 60 %.
// ---------------------------------------------------------------------------
struct Mutation { bool rest = false; bool octave = false; bool slide = false; };

class Mutator
{
public:
    enum Lane : uint32_t { kLaneOctave = 1, kLaneSlide = 2, kLaneRest = 3 };

    static uint32_t hash(int step, int variation, uint32_t lane)
    {
        uint32_t h = (uint32_t)step * 0x9E3779B1u ^ (uint32_t)variation * 0x85EBCA77u ^ lane * 0xC2B2AE3Du;
        h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;   // integer mix
        return h;
    }

    // The step's place in this Variation's shuffled order, as (rank + 0.5) / 16 in (0, 1).
    static float uniform(int step, int variation, uint32_t lane)
    {
        const int s = ((step % 16) + 16) % 16;
        const uint32_t h = hash(s, variation, lane);
        int rank = 0;
        for (int o = 0; o < 16; ++o)
        {
            const uint32_t ho = hash(o, variation, lane);
            if (ho < h || (ho == h && o < s)) ++rank;
        }
        return ((float)rank + 0.5f) * (1.0f / 16.0f);
    }

    static Mutation decide(int step, int variation, float octave, float slide, float rest)
    {
        Mutation m;
        m.rest = uniform(step, variation, kLaneRest) < rest;
        if (m.rest) return m;
        m.octave = uniform(step, variation, kLaneOctave) < octave;
        m.slide  = uniform(step, variation, kLaneSlide)  < slide;
        return m;
    }
};

// ---------------------------------------------------------------------------
// NoteInput: MIDI -> musical decisions. Knows nothing about sound.
// Mono, last-note priority. Overlapping notes slide; velocity >= threshold accents.
// Each held key remembers the pitch it actually PLAYS (after an octave jump), so
// note-offs and the slide back to an older held note use the right pitch.
// ---------------------------------------------------------------------------
class NoteInput
{
public:
    void reset() { count_ = 0; }
    void setAccentThreshold(int t) { threshold_ = t; }

    // canSlideIn: false after a long silence (nothing sensible to slide from).
    void noteOn(int note, int vel, const Mutation& m, bool canSlideIn, AcidVoice& v)
    {
        if (m.rest) { remove(note); return; }                    // left out: no sound, nothing held
        const bool legato = count_ > 0;
        const int played = m.octave ? (note + 12 > 127 ? note : note + 12) : note;
        remove(note);
        if (count_ == kMax) { for (int i = 1; i < kMax; ++i) stack_[i - 1] = stack_[i]; --count_; }
        stack_[count_++] = {note, vel, played};
        const bool accent = vel >= threshold_;
        if (m.slide && !legato && canSlideIn) v.slideIn(played, accent);
        else v.noteOn(played, accent, legato);
    }

    void noteOff(int note, AcidVoice& v)
    {
        const bool wasTop = count_ > 0 && stack_[count_ - 1].note == note;
        remove(note);
        if (!wasTop) return;
        if (count_ > 0) { const Held& h = stack_[count_ - 1]; v.noteOn(h.played, h.vel >= threshold_, true); }
        else v.noteOff();
    }

    void allNotesOff(AcidVoice& v) { count_ = 0; v.noteOff(); }

private:
    struct Held { int note; int vel; int played; };
    static constexpr int kMax = 16;
    void remove(int note)
    {
        int w = 0;
        for (int r = 0; r < count_; ++r) if (stack_[r].note != note) stack_[w++] = stack_[r];
        count_ = w;
    }
    Held stack_[kMax] = {};
    int count_ = 0;
    int threshold_ = 100;
};

} // namespace

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class OCKM03 : public mpc::Plugin
{
public:
    OCKM03() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("volume", "Volume", -60.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("wave", "Wave", {"Saw", "Square"}, 0));
        addParam(floatParam("cutoff", "Cutoff", 0.0f, 100.0f, 38.0f, "Hz", 0));
        addParam(floatParam("reso", "Resonance", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("envmod", "Env Mod", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("decay", "Decay", 0.0f, 100.0f, 53.0f, "ms", 0));
        addParam(floatParam("accent", "Accent", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("tune", "Tuning", -12.0f, 12.0f, 0.0f, "st", 2));
        addParam(floatParam("slide", "Slide Time", 0.0f, 100.0f, 20.0f, "ms", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("accthr", "Accent Thr", 1, 127, 100));
        // Retired macro parameters (v1.0). Never remove, rename or reorder: saved projects
        // store values by position. Hidden from automation/Q-Links and ignored by the DSP.
        auto retired = [this](mpc::Param p) { p.automatable = false; addParam(std::move(p)); };
        retired(floatParam("m_squelch", "Squelch", 0.0f, 100.0f, 0.0f, "%", 0));
        retired(floatParam("m_bite", "Bite", 0.0f, 100.0f, 0.0f, "%", 0));
        retired(floatParam("m_rubber", "Rubber", 0.0f, 100.0f, 0.0f, "%", 0));
        retired(floatParam("m_grit", "Grit", 0.0f, 100.0f, 0.0f, "%", 0));
        retired(floatParam("sq_cut", "Sq>Cutoff", -100.0f, 100.0f, 40.0f, "%", 0));
        retired(floatParam("sq_res", "Sq>Reso", -100.0f, 100.0f, 60.0f, "%", 0));
        retired(floatParam("sq_env", "Sq>EnvMod", -100.0f, 100.0f, 50.0f, "%", 0));
        retired(floatParam("bt_acc", "Bt>Accent", -100.0f, 100.0f, 50.0f, "%", 0));
        retired(floatParam("bt_dec", "Bt>Decay", -100.0f, 100.0f, -40.0f, "%", 0));
        retired(floatParam("bt_drv", "Bt>Drive", -100.0f, 100.0f, 20.0f, "%", 0));
        retired(floatParam("rb_sld", "Rb>Slide", -100.0f, 100.0f, 50.0f, "%", 0));
        retired(floatParam("rb_dec", "Rb>Decay", -100.0f, 100.0f, 40.0f, "%", 0));
        retired(floatParam("rb_cut", "Rb>Cutoff", -100.0f, 100.0f, -15.0f, "%", 0));
        retired(floatParam("gr_drv", "Gr>Drive", -100.0f, 100.0f, 60.0f, "%", 0));
        retired(floatParam("gr_res", "Gr>Reso", -100.0f, 100.0f, -20.0f, "%", 0));
        // v1.2 Mutate (appended at the end; ids and order permanent from here on).
        addParam(floatParam("mu_oct", "Oct Jump", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("mu_sld", "Add Slide", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("mu_rst", "Add Rest", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("mu_var", "Variation", 1, 64, 1));
        if (numParams() != kNumParams) mpc::log("OCKM-03: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        // Factory presets: classic-panel settings only (retired parameters stay at defaults).
        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Init Acid", {});
        program("Classic Squelch", {{kCutoff, 43}, {kReso, 80}, {kEnvMod, 60}, {kDecay, 55}, {kAccent, 60}});
        program("Rubber Sub", {{kWave, 1}, {kCutoff, 29}, {kReso, 40}, {kEnvMod, 30}, {kDecay, 68}, {kSlide, 50},
                               {kVolume, -8}});
        program("Acid Scream", {{kCutoff, 47}, {kReso, 90}, {kEnvMod, 70}, {kDecay, 48}, {kAccent, 80}, {kDrive, 35},
                                {kVolume, -5}});
        program("Mutant Squelch", {{kCutoff, 40}, {kReso, 82}, {kEnvMod, 62}, {kDecay, 50}, {kAccent, 65},
                                   {kMuOct, 25}, {kMuSld, 30}, {kMuRst, 20}, {kMuVar, 7}});
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        voice_.prepare(sampleRate);
        sr_ = sampleRate;
        lowCut_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 30.0f));   // fixed 30 Hz, 12 dB/oct
        lowCut_.reset();
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        voice_.reset();
        notes_.reset();
        lowCut_.reset();
        noteCounter_ = 0; wasPlaying_ = false; sinceNote_ = 1.0e9;
        volume_.snap(dbToGain(get(kVolume)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        voice_.setTargets(knobTargets());
        notes_.setAccentThreshold(getInt(kAccThr));
        volume_.setTarget(dbToGain(get(kVolume)));

        // Mutate amounts for this block (0..1) and the repeatable set.
        const float muOct = getNormalised(kMuOct), muSld = getNormalised(kMuSld), muRst = getNormalised(kMuRst);
        const int   muVar = getInt(kMuVar);
        const mpc::Transport& tr = ctx.transport;
        const bool  onGrid = tr.playing && tr.ppqValid;
        if (onGrid && !wasPlaying_) noteCounter_ = 0;            // fresh start of the sequence
        wasPlaying_ = onGrid;
        const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : sr_;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double beatsPerBar = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        const int    stepsPerBar = mpc::clamp((int)std::lround(beatsPerBar * 4.0), 1, 64);
        // Slide-in only within one bar of the previous note (nothing to slide from after a long gap).
        const double slideWindow = beatsPerBar * 60.0 / bpm * sr;

        float* L = ctx.outputs[0];
        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            const int at = mpc::clamp(ev.sampleOffset, 0, ctx.numFrames);
            if (at > pos) { voice_.render(L + pos, at - pos); pos = at; }
            if (ev.isNoteOn())
            {
                // Step in the bar (16ths), from the MPC's playhead; a note counter without one.
                int step;
                if (onGrid)
                {
                    const double ppq = tr.ppqPos + at * bpm / 60.0 / sr;
                    double rel = std::fmod(ppq - tr.barStartPpq, beatsPerBar);
                    if (rel < 0.0) rel += beatsPerBar;
                    step = (int)std::lround(rel * 4.0) % stepsPerBar;
                }
                else step = noteCounter_ % 16;
                ++noteCounter_;
                const Mutation m = Mutator::decide(step, muVar, muOct, muSld, muRst);
                const bool canSlideIn = sinceNote_ + at <= slideWindow;
                notes_.noteOn(ev.note(), ev.velocity(), m, canSlideIn, voice_);
                if (!m.rest) sinceNote_ = -at;                        // counted from this note's sample
            }
            else if (ev.isNoteOff())      notes_.noteOff(ev.note(), voice_);
            else if (ev.isAllNotesOff())  { notes_.allNotesOff(voice_); noteCounter_ = 0; }
            // Pitch bend, mod wheel and other controllers are ignored on purpose (lean).
        }
        if (ctx.numFrames > pos) voice_.render(L + pos, ctx.numFrames - pos);
        sinceNote_ += ctx.numFrames;

        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        bool silentIn = true;
        float outPeak = 0.0f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            if (L[i] != 0.0f) silentIn = false;
            const float y = lowCut_.process(L[i]);
            outPeak = std::fmax(outPeak, std::fabs(y));
            L[i] = ceiling(y * volume_.process());
            if (R) R[i] = L[i];
        }
        // The voice has stopped and the low cut's tail is below -140 dB: clear it, so it never
        // decays into denormals.
        if (silentIn && outPeak < 1.0e-7f) lowCut_.reset();
    }

    // Show cutoff, decay and slide in real units (the knob itself is 0..100 % travel).
    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[32];
        const float n = plain * 0.01f;
        switch (index)
        {
            case kCutoff: std::snprintf(buf, sizeof(buf), "%.0f", (double)cutoffHz(n)); break;
            case kDecay:  std::snprintf(buf, sizeof(buf), "%.0f", (double)decayMs(n)); break;
            case kSlide:  std::snprintf(buf, sizeof(buf), "%.0f", (double)slideMs(n)); break;
            default: return false;
        }
        out = buf;
        return true;
    }

private:
    // Output ceiling: untouched below -3 dBFS, then rounds off smoothly so the hardest
    // resonance + accent + drive settings never clip the MPC's mixer.
    static float ceiling(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0.0f ? -y : y;
    }

    // The voice reads the classic-panel knobs directly. Retired macro parameters are ignored.
    VoiceTargets knobTargets() const
    {
        VoiceTargets t;
        t.wave   = getInt(kWave);
        t.cutoff = getNormalised(kCutoff);
        t.reso   = getNormalised(kReso);
        t.envMod = getNormalised(kEnvMod);
        t.decay  = getNormalised(kDecay);
        t.accent = getNormalised(kAccent);
        t.slide  = getNormalised(kSlide);
        t.drive  = getNormalised(kDrive);
        t.tuneSemis = get(kTune);
        return t;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM-03";
        info.vendor       = "OCKM";     // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', '5', 'u', 'Q');   // must match plugin.json "uid"
        info.version      = 1200;       // 1.2.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        return info;
    }

    AcidVoice voice_;
    NoteInput notes_;
    Biquad lowCut_;                // fixed 30 Hz high-pass: trims sub rumble so the kick has room
    Smoother volume_;
    double sr_ = 44100.0;
    double sinceNote_ = 1.0e9;     // samples since the last played note-on
    int noteCounter_ = 0;          // step source when the MPC gives no playhead
    bool wasPlaying_ = false;
};

// Creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKM03)
