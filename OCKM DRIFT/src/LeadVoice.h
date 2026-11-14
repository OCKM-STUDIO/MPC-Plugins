// LeadVoice.h - OCKM DRIFT's sound: one retro analogue lead voice (three drifting oscillators,
// a 24 dB ladder low-pass, drive, amp). Knows nothing about the sequencer.
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
#pragma once

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <cmath>
#include <cstdint>

namespace ockm_drift {

using mpc::dsp::clamp;
using mpc::dsp::fastTanh;
// Knob laws (normalised 0..1 -> real quantity).
inline float cutoffHz(float n)   { return 60.0f * std::pow(200.0f, n); }    // 60 Hz .. 12 kHz
inline float decayMs(float n)    { return 30.0f * std::pow(66.6667f, n); }  // 30 ms .. 2 s
inline float glideMs(float n)    { return 10.0f * std::pow(40.0f, n); }     // 10 .. 400 ms
inline float releaseMs(float n)  { return 5.0f * std::pow(300.0f, n); }     // 5 ms .. 1.5 s
inline float detuneCents(float n){ return 12.0f * n; }

// Envelope times are "time to fall to about 2 %" = 4 time constants.
inline float mulForMs(float ms, float sr, float steps = 1.0f)
{
    return std::exp(-4.0f * steps / std::fmax(1.0f, ms * 0.001f * sr));
}

constexpr int   kTick        = 8;        // control-rate interval in samples
constexpr float kEnvOctaves  = 5.0f;     // Env Amount 100 %: +5 octaves at the peak
constexpr float kAccentOct   = 1.5f;     // Accent 100 %: +1.5 octaves more
constexpr float kAccentGain  = 0.585f;   // Accent 100 %: +4 dB
constexpr float kKeyTrack    = 0.5f;     // filter follows half an octave per octave
constexpr float kDriftCents  = 3.0f;     // fixed oscillator drift
constexpr float kOutGain     = 2.0f;     // voice make-up gain (+6 dB): presets peak around -4 .. -6 dBFS

// ---------------------------------------------------------------------------
// The sound: one retro analogue lead voice. Knows nothing about the sequencer.
// ---------------------------------------------------------------------------

// PolyBLEP correction for a saw step at phase wrap.
inline float polyBlep(float t, float dt)
{
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}
// Band-limited saw at phase p.
inline float blepSaw(float p, float dt) { return 2.0f * p - 1.0f - polyBlep(p, dt); }

// A value set once per tick and ramped linearly over the next kTick samples (no zipper).
struct Ramp
{
    float v = 0.0f, d = 0.0f;
    void to(float target, bool snap) { if (snap) { v = target; d = 0.0f; } else d = (target - v) * (1.0f / kTick); }
    float next() { return v += d; }
};

// 24 dB/oct ladder: four zero-delay-feedback one-pole stages in a feedback loop. The loop
// is solved with a linear estimate of the last stage and its input is saturated, which keeps
// the resonance round and bounded at any setting.
class Ladder
{
public:
    void reset() { s1_ = s2_ = s3_ = s4_ = 0.0f; }
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

struct Knobs                       // normalised 0..1 unless noted
{
    float cutoff = 0.4f, reso = 0.35f, envAmt = 0.5f, decay = 0.35f;
    float wave = 0.25f, detune = 0.35f, sub = 0.3f, glide = 0.3f;
    float release = 0.15f, accent = 0.5f, drive = 0.2f;
    float driftOct = 0.0f;         // Cut Drift offset in octaves (already scaled)
};

class LeadVoice
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        smooth_ = 1.0f - std::exp(-(float)kTick / (0.015f * sr_));          // 15 ms knob smoothing
        fenvAttack_ = 1.0f - std::exp(-(float)kTick / (0.0007f * sr_));    // ~1 ms filter env attack
        attackStep_ = 1.0f / (0.002f * sr_);                                // 2 ms amp attack
        gainSmooth_ = 1.0f - std::exp(-1.0f / (0.002f * sr_));
        driftPick_ = (int)(0.4f * sr_);                                      // new drift target ~every 0.4 s
        driftCoef_ = 1.0f - std::exp(-32.0f / (0.35f * sr_));
        reset();
    }

    void reset()
    {
        filter_.reset();
        stage_ = kIdle; amp_ = 0.0f; gain_ = gainT_ = 1.0f;
        fenv_ = 0.0f; fenvRising_ = false;
        pitch_ = pitchTarget_ = 60.0f; gliding_ = false;
        ph_[0] = 0.0f; ph_[1] = 0.37f; ph_[2] = 0.71f;
        for (int k = 0; k < 3; ++k) { drift_[k] = driftT_[k] = 0.0f; ratio_[k] = 1.0f; }
        rng_ = 0x2545F491U; driftCtr_ = 0; driftTick_ = 0;
        ctr_ = 0; first_ = true; snap_ = true;
    }

    void setKnobs(const Knobs& k)
    {
        t_ = k;
        if (first_) { s_ = k; first_ = false; }
        glideCoef_ = 1.0f - mulForMs(glideMs(k.glide), sr_, (float)kTick);
        releaseMul_ = mulForMs(releaseMs(k.release), sr_);
    }

    bool active() const { return stage_ != kIdle; }

    // A fresh note: jump to the pitch, re-pluck both envelopes (amp from where it is: no click).
    void trigger(int note, bool accent)
    {
        pitch_ = pitchTarget_ = (float)note;
        gliding_ = false;
        if (stage_ == kIdle) { s_ = t_; snap_ = true; }          // knobs moved while silent apply at once
        accent_ = accent ? s_.accent : 0.0f;
        gainT_ = 1.0f + kAccentGain * accent_;
        stage_ = kAttack;
        fenvRising_ = true;
        ctr_ = 0;                                                // tick now: fresh pitch and cutoff
    }

    // A slide: glide to the new pitch, keep the envelopes running (legato).
    void glideTo(int note, bool accent)
    {
        pitchTarget_ = (float)note;
        gliding_ = true;
        accent_ = accent ? s_.accent : 0.0f;
        gainT_ = 1.0f + kAccentGain * accent_;
    }

    // A late key change: re-pitch the note that is playing, no glide, no re-pluck.
    void retune(int note) { pitch_ = pitchTarget_ = (float)note; gliding_ = false; ctr_ = 0; }

    void release() { if (stage_ != kIdle) stage_ = kRelease; }

    float next()
    {
        if (stage_ == kIdle) return 0.0f;
        if (ctr_-- <= 0) { tick(); ctr_ = kTick - 1; }

        switch (stage_)
        {
            case kAttack:  amp_ += attackStep_; if (amp_ >= 1.0f) { amp_ = 1.0f; stage_ = kHold; } break;
            case kHold:    break;
            case kRelease:
                amp_ *= releaseMul_;
                if (amp_ < 1.0e-4f) { amp_ = 0.0f; stage_ = kIdle; filter_.reset(); return 0.0f; }   // -80 dB: done
                break;
            default: break;
        }
        gain_ += gainSmooth_ * (gainT_ - gain_);

        // Oscillators: three phases; each is saw - w * (saw half a cycle on) = saw .. square.
        const float base = inc_.next();
        const float w = wave_.next();
        float o = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float dt = base * ratio_[k];
            float p = ph_[k] + dt;
            if (p >= 1.0f) p -= 1.0f;
            ph_[k] = p;
            float q = p + 0.5f;
            if (q >= 1.0f) q -= 1.0f;
            const float v = blepSaw(p, dt) - w * blepSaw(q, dt);
            o += (k == 2 ? subLvl_.next() : 1.0f) * v;
        }

        Gs_ += 0.35f * (G_.next() - Gs_);                       // rounds the ramp's corners
        float y = filter_.process(o * norm_.next(), Gs_, k_.next(), comp_.next());
        const float dMix = driveMix_.next(), dPre = drivePre_.next(), dNorm = driveNorm_.next();
        if (dMix > 0.0f) y += dMix * (fastTanh(y * dPre) * dNorm - y);
        return y * amp_ * gain_ * kOutGain;
    }

private:
    enum Stage { kIdle, kAttack, kHold, kRelease };

    void tick()
    {
        s_.cutoff  += smooth_ * (t_.cutoff - s_.cutoff);
        s_.reso    += smooth_ * (t_.reso - s_.reso);
        s_.envAmt  += smooth_ * (t_.envAmt - s_.envAmt);
        s_.decay   += smooth_ * (t_.decay - s_.decay);
        s_.wave    += smooth_ * (t_.wave - s_.wave);
        s_.detune  += smooth_ * (t_.detune - s_.detune);
        s_.sub     += smooth_ * (t_.sub - s_.sub);
        s_.drive   += smooth_ * (t_.drive - s_.drive);
        s_.driftOct += smooth_ * (t_.driftOct - s_.driftOct);
        s_.accent = t_.accent;

        // Filter envelope (accented notes open further and close faster).
        if (fenvRising_) { fenv_ += fenvAttack_ * (1.3f - fenv_); if (fenv_ >= 1.0f) { fenv_ = 1.0f; fenvRising_ = false; } }
        else fenv_ *= mulForMs(decayMs(s_.decay) * (1.0f - 0.5f * accent_), sr_, (float)kTick);
        if (fenv_ < 1.0e-6f) fenv_ = 0.0f;

        // Pitch and drift.
        if (gliding_) { pitch_ += glideCoef_ * (pitchTarget_ - pitch_); if (std::fabs(pitchTarget_ - pitch_) < 1.0e-3f) { pitch_ = pitchTarget_; gliding_ = false; } }
        if (driftTick_-- <= 0 || snap_) { updateDrift(); driftTick_ = 3; }  // every 32 samples
        const bool snap = snap_;
        snap_ = false;
        inc_.to(clamp(440.0f * std::exp2((pitch_ - 69.0f) * (1.0f / 12.0f)) / sr_, 0.0f, 0.2f), snap);

        // Cutoff in octaves above 60 Hz.
        const float keyOct = clamp(kKeyTrack * (pitch_ - 60.0f) * (1.0f / 12.0f), -3.0f, 3.0f);
        const float oct = std::log2(200.0f) * s_.cutoff + (kEnvOctaves * s_.envAmt + kAccentOct * accent_) * fenv_
                        + keyOct + s_.driftOct;
        const float fc = clamp(60.0f * std::exp2(oct), 20.0f, 0.45f * sr_);
        const float g = std::tan(3.14159265f * fc / sr_);
        const float Gn = g / (1.0f + g);
        G_.to(Gn, snap);
        if (snap) Gs_ = Gn;
        const float k = 3.8f * s_.reso;
        k_.to(k, snap);
        comp_.to(0.8f * std::pow(1.0f + k, 0.55f), snap);

        // Oscillator mix, level-compensated for Wave and Sub.
        wave_.to(s_.wave, snap);
        subLvl_.to(s_.sub, snap);
        norm_.to(0.55f / ((2.0f + 0.8f * s_.sub) * (1.0f + 0.35f * s_.wave)), snap);

        const float pre = 1.0f + 7.0f * s_.drive * s_.drive;
        drivePre_.to(pre, snap);
        driveMix_.to(clamp(2.0f * s_.drive, 0.0f, 1.0f), snap);
        driveNorm_.to(0.6f / fastTanh(0.6f * pre), snap);
    }

    // Each oscillator wanders slowly and independently by up to kDriftCents.
    void updateDrift()
    {
        driftCtr_ -= 32;
        if (driftCtr_ <= 0)
        {
            driftCtr_ = driftPick_;
            for (int k = 0; k < 3; ++k)
            {
                rng_ = rng_ * 1664525U + 1013904223U;
                driftT_[k] = ((float)(rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f) * kDriftCents;
            }
        }
        const float det = detuneCents(s_.detune);
        const float spread[3] = {det, -det, 0.0f};
        for (int k = 0; k < 3; ++k)
        {
            drift_[k] += driftCoef_ * (driftT_[k] - drift_[k]);
            ratio_[k] = std::exp2((spread[k] + drift_[k]) * (1.0f / 1200.0f)) * (k == 2 ? 0.5f : 1.0f);
        }
    }

    Ladder filter_;
    Knobs t_, s_;
    float sr_ = 44100.0f, smooth_ = 0.1f, fenvAttack_ = 0.5f, attackStep_ = 0.01f, gainSmooth_ = 0.01f;
    Stage stage_ = kIdle;
    float amp_ = 0.0f, gain_ = 1.0f, gainT_ = 1.0f, accent_ = 0.0f, releaseMul_ = 0.99f;
    float fenv_ = 0.0f, glideCoef_ = 1.0f;
    bool fenvRising_ = false, gliding_ = false, first_ = true, snap_ = true;
    float pitch_ = 60.0f, pitchTarget_ = 60.0f;
    float ph_[3] = {0.0f, 0.37f, 0.71f}, ratio_[3] = {1.0f, 1.0f, 0.5f};
    float drift_[3] = {}, driftT_[3] = {}, driftCoef_ = 0.01f;
    uint32_t rng_ = 0x2545F491U;
    int driftCtr_ = 0, driftPick_ = 17640, driftTick_ = 0;
    Ramp inc_, G_, k_, comp_, wave_, subLvl_, norm_, drivePre_, driveMix_, driveNorm_;
    float Gs_ = 0.0f;
    int ctr_ = 0;
};

} // namespace ockm_drift
