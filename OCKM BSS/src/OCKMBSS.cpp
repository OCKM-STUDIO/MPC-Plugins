// OCKMBSS.cpp - OCKM BSS, a warm, round, soft bass for the MPC.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// One job: a soft bass whose filter you can really open up. Two knobs do most of the work:
// Cutoff (how bright) and Decay (how long). Both down is almost a kick drum; both up is a
// full-on growl. See ../SPEC.md for the design and the reasons behind it.
//
// Signal flow (one mono voice, one note at a time):
//
//   MIDI -> NoteStack (last-note priority; legato glide when Glide > 0 and notes overlap)
//        -> BassVoice:
//             Core (rounded sine) + Sub (sine, one octave down, phase-locked)
//               + Saw (PolyBLEP, optionally detuned)
//          -> Filter: 24 dB/oct zero-delay-feedback ladder low-pass, saturated in its loop
//             cutoff (octaves above 40 Hz) = Cutoff knob + Env Amount * filter env
//                                + key tracking (0.5 oct/oct from C1) - velocity darkening
//          -> Drive (soft clip, level compensated, 0 = clean)
//          -> Amp envelope (attack, Decay-driven decay, sustain, release) * velocity
//   -> 20 Hz rumble cut -> + Echo (bass-safe ping-pong delay, lows cut from the repeats)
//   -> Volume -> safety ceiling (transparent below -3 dBFS) -> L / R
//
// The "zones" (SPEC 3a) make Cutoff and Decay cover kick-to-growl on their own:
//   Decay 0-25 %   thump: sustain fades out, a quick pitch drop fades in at each note start
//   Decay 75-100 % hold:  sustain pushed up toward full
//   Cutoff 60-100 % growl: extra saw, drive and a touch of resonance on top of the knobs
// Outside those ranges the knobs act exactly as set.
//
// Realtime rules: no allocation, locks or I/O in process(). exp2/tan/exp only once every
// 8 samples (the "tick"), interpolated in between; the per-sample path is adds, multiplies
// and a few rational tanh approximations.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/delay_line.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"

#include <cmath>
#include <cstring>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kVolume = 0,   // master volume, dB (param 0 is nudged by the host on load: harmless)
    kCutoff,       // filter cutoff, 0..100 % of knob travel (shown in Hz)
    kDecay,        // filter + amp decay, 0..100 % (shown in ms)
    kReso,         // resonance, 0..100 %
    kEnvAmt,       // filter envelope depth, 0..100 % (0..6 octaves)
    kSub,          // sub oscillator level, 0..100 %
    kSaw,          // saw level, 0..100 %
    kDetune,       // saw detune, 0..100 % (0..20 cents sharp)
    kGlide,        // glide time on overlapping notes, 0..100 % (0 = off)
    kAttack,       // amp attack, 0..100 % (shown in ms)
    kSustain,      // amp sustain level, 0..100 %
    kRelease,      // amp release, 0..100 % (shown in ms)
    kDrive,        // drive, 0..100 %
    kVelFilt,      // how much softer notes darken the filter, 0..100 %
    kEchoTime,     // echo time, tempo-synced choice
    kEchoFb,       // echo feedback, 0..100 %
    kEchoMix,      // echo level, 0..100 % (0 = echo off)
    kNumParams
};

// Mappings from normalised knob travel (0..1) to the real quantity.
inline float cutoffHz(float n)  { return 40.0f * std::pow(250.0f, n); }    // 40 Hz .. 10 kHz
inline float decayMs(float n)   { return 60.0f * std::pow(50.0f, n); }     // 60 ms .. 3 s
inline float glideMs(float n)   { return n <= 0.0f ? 0.0f : 10.0f * std::pow(50.0f, n); }   // 0 (off), 10 .. 500 ms
inline float attackMs(float n)  { return std::pow(1000.0f, n); }           // 1 ms .. 1 s
inline float releaseMs(float n) { return 5.0f * std::pow(600.0f, n); }     // 5 ms .. 3 s
inline float detuneCents(float n) { return 20.0f * n; }

// Envelope times are "time to fall to about 2 %" = 4 time constants.
inline float mulForMs(float ms, float sr, float steps = 1.0f)
{
    return std::exp(-4.0f * steps / std::fmax(1.0f, ms * 0.001f * sr));
}

constexpr float kEnvOctaves   = 6.0f;    // Env Amount 100 %: the envelope opens 6 octaves
constexpr float kVelOctaves   = 3.0f;    // Vel>Filter 100 %: velocity 1 is 3 octaves darker
constexpr float kKeyTrack     = 0.5f;    // filter follows 0.5 octave per octave played, from C1
constexpr float kThumpSemis   = 24.0f;   // pitch drop at Decay 0: two octaves
constexpr float kThumpMs      = 12.0f;   // pitch drop time constant (gone in about 40 ms)
constexpr float kGrowlSaw     = 0.7f;    // growl zone at Cutoff 100 %: +70 % saw,
constexpr float kGrowlDrive   = 0.6f;    //   +60 % drive,
constexpr float kGrowlReso    = 0.15f;   //   +15 % resonance
constexpr int   kTick         = 8;       // control-rate interval in samples
constexpr float kOutGain      = 2.2f;    // voice make-up gain (+7 dB): presets peak around -5 dBFS

// A value set once per tick and ramped linearly over the next kTick samples (no zipper steps).
struct Ramp
{
    float v = 0.0f, d = 0.0f;
    void to(float target, bool snap) { if (snap) { v = target; d = 0.0f; } else d = (target - v) * (1.0f / kTick); }
    float next() { return v += d; }
};

// Test hook: the harness turns the zones off to prove they only act where promised.
bool gZonesEnabled = true;

// Zone shapes (SPEC 3a). Each returns 0 outside its range.
inline float thumpAmount(float decay) { return gZonesEnabled ? clamp(1.0f - decay * 4.0f, 0.0f, 1.0f) : 0.0f; }
inline float holdAmount(float decay)  { return gZonesEnabled ? clamp((decay - 0.75f) * 4.0f, 0.0f, 1.0f) : 0.0f; }
inline float growlAmount(float cut)   { return gZonesEnabled ? clamp((cut - 0.6f) * 2.5f, 0.0f, 1.0f) : 0.0f; }

// Rounded sine: two parabolas. phase 0..1, output -1..1, starts at 0 going up.
inline float roundSine(float p)
{
    return p < 0.5f ? 16.0f * p * (0.5f - p) : -16.0f * (p - 0.5f) * (1.0f - p);
}

// PolyBLEP correction for a saw step at phase wrap.
inline float polyBlep(float t, float dt)
{
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Filter: four zero-delay-feedback one-pole stages in a feedback loop (a ladder low-pass).
// The loop is solved with a linear estimate of the last stage, and the input to the loop is
// saturated, which keeps the full-resonance peak round and bounded.
// ---------------------------------------------------------------------------
class Ladder
{
public:
    void reset() { s1_ = s2_ = s3_ = s4_ = 0.0f; }

    // G is the stage coefficient g/(1+g); k the feedback (0..3.9); comp the bass make-up.
    float process(float x, float G, float k, float comp)
    {
        const float G2 = G * G, G4 = G2 * G2;
        const float sigma = G2 * G * s1_ + G2 * s2_ + G * s3_ + s4_;
        const float xin = x * comp;
        const float y4est = (G4 * xin + (1.0f - G) * sigma) / (1.0f + k * G4);
        const float u = fastTanh(xin - k * y4est);
        const float y1 = stage(u, s1_, G);
        const float y2 = stage(y1, s2_, G);
        const float y3 = stage(y2, s3_, G);
        return stage(y3, s4_, G);
    }

private:
    static float stage(float x, float& s, float G)
    {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }
    float s1_ = 0.0f, s2_ = 0.0f, s3_ = 0.0f, s4_ = 0.0f;
};

// ---------------------------------------------------------------------------
// BassVoice: the sound. Knows nothing about MIDI.
// ---------------------------------------------------------------------------
struct Knobs                       // knob values, normalised 0..1
{
    float cutoff = 0.35f, decay = 0.4f, reso = 0.25f, envAmt = 0.5f;
    float sub = 0.5f, saw = 0.3f, detune = 0.1f, glide = 0.0f;
    float attack = 0.0f, sustain = 0.25f, release = 0.1f, drive = 0.0f, velFilt = 0.3f;
};

class BassVoice
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        smooth_ = 1.0f - std::exp(-(float)kTick / (0.015f * sr_));       // 15 ms knob smoothing
        fenvAttack_ = 1.0f - std::exp(-(float)kTick / (0.00068f * sr_)); // ~1 ms filter env attack
        thumpMul_ = std::exp(-(float)kTick / (kThumpMs * 0.001f * sr_));
        reset();
    }

    void reset()
    {
        filter_.reset();
        gate_ = false; stage_ = kIdle; amp_ = 0.0f;
        fenv_ = 0.0f; fenvRising_ = false; drop_ = 0.0f;
        pitch_ = pitchTarget_ = 36.0f;
        phase_ = sawPhase_ = 0.0f; subFlip_ = 0.0f;
        ctr_ = 0; first_ = true;
    }

    // Once per block: the knob targets (smoothed here at control rate).
    void setKnobs(const Knobs& k)
    {
        t_ = k;
        if (first_) { s_ = k; first_ = false; }
        const float gms = glideMs(k.glide);
        glideCoef_ = gms <= 0.0f ? 1.0f : 1.0f - mulForMs(gms, sr_, (float)kTick);
        attackCoef_ = 1.0f - std::exp(-1.47f / std::fmax(1.0f, attackMs(k.attack) * 0.001f * sr_));
        releaseMul_ = mulForMs(releaseMs(k.release), sr_);
    }

    bool glideOn() const { return t_.glide > 0.0f; }
    bool active() const { return stage_ != kIdle; }

    // A fresh note: jump to the pitch, re-pluck both envelopes, thump if Decay is short.
    void trigger(int note, int vel)
    {
        pitch_ = pitchTarget_ = (float)note;
        setVelocity(vel);
        if (amp_ < 0.02f)                                                     // from (near) silence:
        {
            phase_ = 0.0f; sawPhase_ = 0.0f; subFlip_ = 0.0f;                 // clean, repeatable start
            s_ = t_;                                                          // knobs moved while silent apply at once
            snap_ = true;                                                     // no pitch/cutoff ramp from the last note
        }
        drop_ = kThumpSemis * thumpAmount(s_.decay);
        gate_ = true; stage_ = kAttack;
        fenvRising_ = true;
        ctr_ = 0;                                                             // tick now: no stale coefficients
    }

    // Overlapping note with Glide on: glide there, keep the envelopes running.
    // The amp level keeps the first note's velocity (no jump mid-note); the filter follows the new one.
    void glideTo(int note, int vel) { pitchTarget_ = (float)note; vel01_ = clamp((float)(vel - 1) / 126.0f, 0.0f, 1.0f); }

    void release() { gate_ = false; if (stage_ != kIdle) stage_ = kRelease; }

    void render(float* out, int n)
    {
        if (stage_ == kIdle) return;
        for (int i = 0; i < n; ++i)
        {
            if (ctr_-- <= 0) { tick(); ctr_ = kTick - 1; }

            // Envelope (per sample, so attacks and releases stay click-free).
            switch (stage_)
            {
                case kAttack:
                    amp_ += attackCoef_ * (1.3f - amp_);
                    if (amp_ >= 1.0f) { amp_ = 1.0f; stage_ = kDecaySus; }
                    break;
                case kDecaySus:
                    amp_ = sus_ + (amp_ - sus_) * ampMul_;
                    break;
                case kRelease:
                    amp_ *= releaseMul_;
                    if (amp_ < 1.0e-4f) { amp_ = 0.0f; stage_ = kIdle; filter_.reset(); return; }   // -80 dB: done
                    break;
                default: break;
            }

            // Oscillators.
            const float inc = inc_.next();
            phase_ += inc;
            if (phase_ >= 1.0f) { phase_ -= 1.0f; subFlip_ = subFlip_ > 0.0f ? 0.0f : 0.5f; }
            const float sawInc = inc * detuneRatio_;
            sawPhase_ += sawInc;
            if (sawPhase_ >= 1.0f) sawPhase_ -= 1.0f;
            const float core = roundSine(phase_);
            const float sub = roundSine(0.5f * phase_ + subFlip_);
            const float saw = 2.0f * sawPhase_ - 1.0f - polyBlep(sawPhase_, sawInc);
            const float mix = core + subLvl_.next() * sub + sawLvl_.next() * saw;

            // Filter, drive, amp.
            Gs_ += 0.35f * (G_.next() - Gs_);                                 // rounds the ramp's corners
            float y = filter_.process(mix, Gs_, k_.next(), comp_.next());
            const float dMix = driveMix_.next(), dPre = drivePre_.next(), dNorm = driveNorm_.next();
            if (dMix > 0.0f) y += dMix * (fastTanh(y * dPre) * dNorm - y);
            out[i] += y * amp_ * velGain_ * kOutGain;
        }
    }

private:
    enum Stage { kIdle, kAttack, kDecaySus, kRelease };

    void setVelocity(int vel)
    {
        const float v = clamp((float)(vel - 1) / 126.0f, 0.0f, 1.0f);
        vel01_ = v;
        velGain_ = 0.5f + 0.5f * v;                                          // velocity 1 = -6 dB
    }

    // Control rate: smoothing, zones, coefficients. Sets per-sample ramps for the next kTick samples.
    void tick()
    {
        s_.cutoff  += smooth_ * (t_.cutoff - s_.cutoff);
        s_.decay   += smooth_ * (t_.decay - s_.decay);
        s_.reso    += smooth_ * (t_.reso - s_.reso);
        s_.envAmt  += smooth_ * (t_.envAmt - s_.envAmt);
        s_.sub     += smooth_ * (t_.sub - s_.sub);
        s_.saw     += smooth_ * (t_.saw - s_.saw);
        s_.detune  += smooth_ * (t_.detune - s_.detune);
        s_.sustain += smooth_ * (t_.sustain - s_.sustain);
        s_.drive   += smooth_ * (t_.drive - s_.drive);
        s_.velFilt += smooth_ * (t_.velFilt - s_.velFilt);

        // Zones (SPEC 3a): offsets on top of the knobs, never overwrites.
        const float thump = thumpAmount(s_.decay), hold = holdAmount(s_.decay), growl = growlAmount(s_.cutoff);
        float sus = s_.sustain * (1.0f - thump);
        sus += (1.0f - sus) * hold;
        sus_ = sus;
        const float sawLvl = clamp(s_.saw + kGrowlSaw * growl, 0.0f, 1.0f);
        const float drive = clamp(s_.drive + kGrowlDrive * growl, 0.0f, 1.0f);
        const float reso = clamp(s_.reso + kGrowlReso * growl, 0.0f, 1.0f);

        // Envelopes at control rate.
        const float dms = decayMs(s_.decay);
        ampMul_ = mulForMs(1.5f * dms, sr_);
        if (fenvRising_) { fenv_ += fenvAttack_ * (1.3f - fenv_); if (fenv_ >= 1.0f) { fenv_ = 1.0f; fenvRising_ = false; } }
        else fenv_ *= mulForMs(dms, sr_, (float)kTick);
        if (fenv_ < 1.0e-6f) fenv_ = 0.0f;

        // Pitch: glide and the thump's pitch drop.
        pitch_ += glideCoef_ * (pitchTarget_ - pitch_);
        drop_ *= thumpMul_;
        if (drop_ < 1.0e-3f) drop_ = 0.0f;
        const float incNext = clamp(440.0f * std::exp2((pitch_ + drop_ - 69.0f) * (1.0f / 12.0f)) / sr_, 0.0f, 0.45f);
        inc_.to(incNext, snap_);
        detuneRatio_ = std::exp2(detuneCents(s_.detune) * (1.0f / 1200.0f));

        // Filter cutoff in octaves above 40 Hz.
        const float keyOct = clamp(kKeyTrack * (pitch_ - 36.0f) * (1.0f / 12.0f), -3.0f, 4.0f);
        const float velOct = -kVelOctaves * s_.velFilt * (1.0f - vel01_);
        const float oct = std::log2(250.0f) * s_.cutoff + kEnvOctaves * s_.envAmt * fenv_ + keyOct + velOct;
        const float fc = clamp(40.0f * std::exp2(oct), 20.0f, 0.45f * sr_);
        const float g = std::tan(kPi * fc / sr_);
        const float Gnext = g / (1.0f + g);
        const bool snap = snap_;
        snap_ = false;
        G_.to(Gnext, snap);
        if (snap) Gs_ = Gnext;
        const float k = 3.9f * reso;
        k_.to(k, snap);

        // Mix (level-compensated, folded into the sub and saw levels and the core via comp) and drive.
        const float sawL = 0.8f * sawLvl;
        const float norm = 1.0f / (1.0f + 0.6f * s_.sub + 0.5f * sawL);
        subLvl_.to(s_.sub, snap);
        sawLvl_.to(sawL, snap);
        comp_.to(0.75f * std::pow(1.0f + k, 0.6f) * norm, snap);
        const float pre = 1.0f + 8.0f * drive * drive;
        drivePre_.to(pre, snap);
        driveMix_.to(clamp(2.0f * drive, 0.0f, 1.0f), snap);
        driveNorm_.to(0.6f / fastTanh(0.6f * pre), snap);
    }

    Ladder filter_;
    Knobs t_, s_;
    float sr_ = 44100.0f, smooth_ = 0.1f, fenvAttack_ = 0.5f, thumpMul_ = 0.9f;
    bool gate_ = false, first_ = true, snap_ = true, fenvRising_ = false;
    Stage stage_ = kIdle;
    float amp_ = 0.0f, sus_ = 0.25f, ampMul_ = 0.999f, attackCoef_ = 1.0f, releaseMul_ = 0.99f;
    float fenv_ = 0.0f, drop_ = 0.0f, glideCoef_ = 1.0f;
    float pitch_ = 36.0f, pitchTarget_ = 36.0f, vel01_ = 1.0f, velGain_ = 1.0f;
    float phase_ = 0.0f, sawPhase_ = 0.0f, subFlip_ = 0.0f, detuneRatio_ = 1.0f;
    Ramp inc_, G_, k_, comp_, subLvl_, sawLvl_, drivePre_, driveMix_, driveNorm_;
    float Gs_ = 0.0f;
    int ctr_ = 0;
};

// ---------------------------------------------------------------------------
// NoteStack: last-note priority. Knows MIDI, knows nothing about sound.
// ---------------------------------------------------------------------------
class NoteStack
{
public:
    void reset() { count_ = 0; }

    void noteOn(int note, int vel, BassVoice& v)
    {
        const bool overlap = count_ > 0;
        remove(note);
        if (count_ == kMax) { for (int i = 1; i < kMax; ++i) stack_[i - 1] = stack_[i]; --count_; }
        stack_[count_++] = {note, vel};
        if (overlap && v.glideOn() && v.active()) v.glideTo(note, vel);
        else v.trigger(note, vel);
    }

    void noteOff(int note, BassVoice& v)
    {
        const bool wasTop = count_ > 0 && stack_[count_ - 1].note == note;
        remove(note);
        if (!wasTop) return;
        if (count_ > 0)
        {
            const Held& h = stack_[count_ - 1];                 // back to the older held note
            if (v.glideOn()) v.glideTo(h.note, h.vel); else v.trigger(h.note, h.vel);
        }
        else v.release();
    }

    void allNotesOff(BassVoice& v) { count_ = 0; v.release(); }

private:
    struct Held { int note; int vel; };
    static constexpr int kMax = 16;
    void remove(int note)
    {
        int w = 0;
        for (int r = 0; r < count_; ++r) if (stack_[r].note != note) stack_[w++] = stack_[r];
        count_ = w;
    }
    Held stack_[kMax] = {};
    int count_ = 0;
};

// ---------------------------------------------------------------------------
// Echo: tempo-synced ping-pong delay for bass. What goes into the echo has its lows cut
// (24 dB/oct at 250 Hz), and every pass loses a little more low end (200 Hz) and top
// (4.5 kHz), so the bass itself stays one tight, centred note and the repeats always fade.
// ---------------------------------------------------------------------------
class Echo
{
public:
    static constexpr int kNumTimes = 5;
    static constexpr float kBeats[kNumTimes] = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f};   // 1/16 1/8T 1/8 1/8. 1/4

    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        const int maxSamples = (int)(sampleRate * kMaxSeconds) + 4;
        lineL_.prepare(maxSamples);                       // allocation: prepare() only
        lineR_.prepare(maxSamples);
        hp1_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 250.0f));
        hp2_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 250.0f));
        lpL_.setCutoff(4500.0f, sampleRate); lpR_.setCutoff(4500.0f, sampleRate);
        loopHpL_.setCutoff(200.0f, sampleRate); loopHpR_.setCutoff(200.0f, sampleRate);
        xfStep_ = 1.0f / (0.02f * sr_);                   // 20 ms crossfade on time changes
        smooth_ = 1.0f - std::exp(-1.0f / (0.02f * sr_)); // 20 ms on mix and feedback
        reset();
    }

    void reset()
    {
        lineL_.reset(); lineR_.reset();
        clearFilters();
        mix_ = fb_ = 0.0f; active_ = false; dirty_ = false; xf_ = -1.0f;
        cur_ = next_ = target_;
    }

    // Once per block.
    void set(int timeIndex, double bpm, float fb01, float mix01)
    {
        const double b = bpm < 40.0 ? 40.0 : (bpm > 300.0 ? 300.0 : bpm);
        target_ = (float)(kBeats[clamp(timeIndex, 0, kNumTimes - 1)] * 60.0 / b * sr_);
        fbT_ = 0.8f * fb01;
        mixT_ = 0.9f * mix01;
        if (!active_ && mixT_ > 0.0f)
        {
            if (dirty_) { lineL_.reset(); lineR_.reset(); clearFilters(); dirty_ = false; }   // rare: Mix back up from 0
            active_ = true; cur_ = next_ = target_; xf_ = -1.0f; fb_ = fbT_;
        }
    }

    bool active() const { return active_; }

    // dry in, wet left/right out (to be added to the dry signal).
    void process(float x, float& wl, float& wr)
    {
        mix_ += smooth_ * (mixT_ - mix_);
        fb_ += smooth_ * (fbT_ - fb_);
        if (xf_ < 0.0f && std::fabs(target_ - next_) > 0.5f) { next_ = target_; xf_ = 0.0f; }
        float l = lineL_.readLinear(cur_), r = lineR_.readLinear(cur_);
        if (xf_ >= 0.0f)
        {
            const float l2 = lineL_.readLinear(next_), r2 = lineR_.readLinear(next_);
            l += xf_ * (l2 - l); r += xf_ * (r2 - r);
            xf_ += xfStep_;
            if (xf_ >= 1.0f) { cur_ = next_; xf_ = -1.0f; }
        }
        const float send = hp2_.process(hp1_.process(x));
        float inL = send + fb_ * loopHpR_.process(lpR_.process(r));   // ping-pong: right feeds left,
        float inR = fb_ * loopHpL_.process(lpL_.process(l));          //            left feeds right
        inL = std::fabs(inL) < 1.0e-15f ? 0.0f : fastTanh(inL);   // soft limit in the loop
        inR = std::fabs(inR) < 1.0e-15f ? 0.0f : fastTanh(inR);
        lineL_.write(inL);
        lineR_.write(inR);
        wl = mix_ * l; wr = mix_ * r;
        if (mixT_ == 0.0f && mix_ < 1.0e-5f) { active_ = false; dirty_ = true; mix_ = 0.0f; }
    }

private:
    static constexpr double kMaxSeconds = 1.5;            // 1/4 at 40 BPM
    void clearFilters() { hp1_.reset(); hp2_.reset(); lpL_.reset(); lpR_.reset(); loopHpL_.reset(); loopHpR_.reset(); }
    DelayLine lineL_, lineR_;
    Biquad hp1_, hp2_;
    OnePoleLP lpL_, lpR_;
    OnePoleHP loopHpL_, loopHpR_;
    float sr_ = 44100.0f, target_ = 1000.0f, cur_ = 1000.0f, next_ = 1000.0f, xf_ = -1.0f, xfStep_ = 0.001f;
    float mix_ = 0.0f, mixT_ = 0.0f, fb_ = 0.0f, fbT_ = 0.0f, smooth_ = 0.001f;
    bool active_ = false, dirty_ = false;
};

} // namespace

class OCKMBSS : public mpc::Plugin
{
public:
    OCKMBSS() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("volume",  "Volume",     -60.0f, 6.0f, -6.0f, "dB", 1));
        addParam(floatParam("cutoff",  "Cutoff",       0.0f, 100.0f, 35.0f, "Hz", 0));
        addParam(floatParam("decay",   "Decay",        0.0f, 100.0f, 40.0f, "ms", 0));
        addParam(floatParam("reso",    "Resonance",    0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("envamt",  "Env Amount",   0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("sub",     "Sub",          0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("saw",     "Saw",          0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("detune",  "Detune",       0.0f, 100.0f, 10.0f, "ct", 0));
        addParam(floatParam("glide",   "Glide",        0.0f, 100.0f, 0.0f, "ms", 0));
        addParam(floatParam("attack",  "Attack",       0.0f, 100.0f, 0.0f, "ms", 0));
        addParam(floatParam("sustain", "Sustain",      0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("release", "Release",      0.0f, 100.0f, 10.0f, "ms", 0));
        addParam(floatParam("drive",   "Drive",        0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("velfilt", "Vel>Filter",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(choiceParam("echotime", "Echo Time", {"1/16", "1/8T", "1/8", "1/8.", "1/4"}, 3));
        addParam(floatParam("echofb",  "Feedback",     0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam("echomix", "Echo Mix",     0.0f, 100.0f, 0.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("OCKM BSS: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Presets are mostly spots on the Cutoff x Decay map (SPEC 7).
        program("Init Soft Bass",   {});
        program("Kick Thud",        {{kCutoff, 8}, {kDecay, 3}, {kSub, 70}});
        program("Deep Round Sub",   {{kCutoff, 15}, {kDecay, 55}, {kSaw, 10}, {kSub, 70}});
        program("Warm House Pluck", {{kCutoff, 40}, {kDecay, 20}, {kEchoTime, 3}, {kEchoMix, 15}});
        program("Rubber Glide",     {{kCutoff, 30}, {kDecay, 60}, {kGlide, 35}, {kReso, 40}});
        program("Buzzy Techno",     {{kCutoff, 65}, {kDecay, 50}, {kDrive, 20}});
        program("Muffled Dub",      {{kCutoff, 12}, {kDecay, 85}, {kRelease, 35}, {kDetune, 30},
                                     {kEchoTime, 4}, {kEchoFb, 55}, {kEchoMix, 30}});
        program("Full Growl",       {{kCutoff, 95}, {kDecay, 92}, {kReso, 35}, {kDetune, 25}});
        program("Open Sweep",       {{kCutoff, 20}, {kDecay, 30}, {kReso, 55}});
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        voice_.prepare(sampleRate);
        echo_.prepare(sampleRate);
        rumble_.setCutoff(20.0f, sampleRate);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        voice_.reset();
        notes_.reset();
        echo_.reset();
        rumble_.reset();
        volume_.snap(dbToGain(get(kVolume)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        Knobs k;
        k.cutoff = getNormalised(kCutoff); k.decay = getNormalised(kDecay);
        k.reso = getNormalised(kReso);     k.envAmt = getNormalised(kEnvAmt);
        k.sub = getNormalised(kSub);       k.saw = getNormalised(kSaw);
        k.detune = getNormalised(kDetune); k.glide = getNormalised(kGlide);
        k.attack = getNormalised(kAttack); k.sustain = getNormalised(kSustain);
        k.release = getNormalised(kRelease); k.drive = getNormalised(kDrive);
        k.velFilt = getNormalised(kVelFilt);
        voice_.setKnobs(k);
        volume_.setTarget(dbToGain(get(kVolume)));
        const mpc::Transport& tr = ctx.transport;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        echo_.set(getInt(kEchoTime), bpm, getNormalised(kEchoFb), getNormalised(kEchoMix));

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            const int at = mpc::clamp(ev.sampleOffset, 0, ctx.numFrames);
            if (at > pos) { voice_.render(L + pos, at - pos); pos = at; }
            if (ev.isNoteOn())            notes_.noteOn(ev.note(), ev.velocity(), voice_);
            else if (ev.isNoteOff())      notes_.noteOff(ev.note(), voice_);
            else if (ev.isAllNotesOff())  notes_.allNotesOff(voice_);
            // Pitch bend, mod wheel and other controllers are ignored on purpose (lean).
        }
        if (ctx.numFrames > pos) voice_.render(L + pos, ctx.numFrames - pos);

        bool silentIn = true;
        float rumblePeak = 0.0f;
        const bool echoOn = echo_.active();
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            if (L[i] != 0.0f) silentIn = false;
            const float dry = rumble_.process(L[i]);
            rumblePeak = std::fmax(rumblePeak, std::fabs(dry));
            const float g = volume_.process();
            float l = dry, r = dry;
            if (echoOn) { float wl, wr; echo_.process(dry, wl, wr); l += wl; r += wr; }
            L[i] = ceiling(l * g);
            if (R) R[i] = ceiling(r * g);
        }
        // Voice silent and the rumble cut's tail below -140 dB: clear it so it never decays
        // into denormals.
        if (silentIn && rumblePeak < 1.0e-7f) rumble_.reset();
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[32];
        const float n = plain * 0.01f;
        switch (index)
        {
            case kCutoff:  std::snprintf(buf, sizeof(buf), "%.0f", (double)cutoffHz(n)); break;
            case kDecay:   std::snprintf(buf, sizeof(buf), "%.0f", (double)decayMs(n)); break;
            case kGlide:   if (n <= 0.0f) { out = "Off"; return true; }
                           std::snprintf(buf, sizeof(buf), "%.0f", (double)glideMs(n)); break;
            case kAttack:  std::snprintf(buf, sizeof(buf), "%.0f", (double)attackMs(n)); break;
            case kRelease: std::snprintf(buf, sizeof(buf), "%.0f", (double)releaseMs(n)); break;
            case kDetune:  std::snprintf(buf, sizeof(buf), "%.1f", (double)detuneCents(n)); break;
            default: return false;
        }
        out = buf;
        return true;
    }

private:
    // Output ceiling: untouched below -3 dBFS (0.7), then rounds off smoothly toward 0.99,
    // so no setting can clip the MPC's mixer.
    static float ceiling(float x)
    {
        const float a = std::fabs(x);
        if (a <= 0.7f) return x;
        const float y = 0.7f + 0.29f * fastTanh((a - 0.7f) / 0.29f);
        return x < 0.0f ? -y : y;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM BSS";
        info.vendor       = "OCKM";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;     // 1.0.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "OgyT";

    BassVoice voice_;
    NoteStack notes_;
    Echo echo_;
    OnePoleHP rumble_;
    Smoother volume_;
    double sr_ = 44100.0;
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKMBSS)
