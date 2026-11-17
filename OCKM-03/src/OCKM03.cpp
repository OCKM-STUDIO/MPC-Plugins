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
//          -> Filter (v1.4): diode ladder (4 coupled stages, smaller bottom capacitor), run at
//             2x the sample rate, tanh input pair and tanh rungs (v1.6), 78 Hz (16 Hz with
//             Full Bass) + 13 Hz coupling high-passes inside the resonance loop; Overdrive
//             scales the oscillator into the input pair (x1 .. x66.6)
//             cutoff (octaves) = base + EnvMod*5.5*(filterEnv - 0.3) + 3*accentSweep
//                                + 0.5 * (pitch - C1) / 12   (key tracking, v1.2)
//             accentSweep = blend(accent voltage, its charge-only capacitor; by Resonance)
//          -> Amp: gate with a long fixed decay, 3 ms soft attack (0.5 ms accented), 8 ms
//             release, accent punch (follows the accent voltage)
//          -> Muffler (v1.6: soft clip, 0 = off)
//          -> Drive (soft-clip, gain compensated, 0 = clean)
//   -> Low cut (fixed 30 Hz, 12 dB/oct, v1.2)
//   -> Echo (v1.3: one knob, dotted-8th synced, bass-light repeats; 0 = bypassed) -> Volume
//   -> output ceiling (transparent below -3 dBFS) -> both outputs
//
// Code layout (kept separate so a step sequencer can be added later):
//   NoteInput  - knows MIDI, knows nothing about sound.
//   AcidVoice  - knows sound, knows nothing about MIDI. Only noteOn/noteOff/setTargets.
//   OCKM03     - the plugin: parameters, presets.
//
// v1.6: Grit (SPEC section 15): filter input-current fix, inner-stage saturation, Overdrive,
// Muffler, Full Bass (parameters 39-41) and a soft attack on normal notes.
//
// v1.5: Pattern generator (parameters 31-38, SPEC section 14): Pattern = Hold plays a repeatable
// generated 16th line in the key of the held note; Mutate works on it as on played notes.
//
// v1.4: closer to the classic circuit (SPEC section 13): diode-ladder filter, accent sweep
// tied to Resonance, accent volume as a punch, Env Mod lowers the resting cutoff. Behaviour
// taken from the circuit notes of the GPL-3 "DW-303" project; no code copied.
//
// v1.3: Echo, a one-knob dotted-8th delay (parameter 30).
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
// Decay now scales them too, and (v1.4) the accent sweep can never exceed the accent
// voltage, so a steady stream of accents doesn't pin the filter open.
//
// Realtime rules: no allocation, locks or I/O in process(); pow/tan only every
// 16 samples; the per-sample path is adds, multiplies and two rational tanh.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/delay_line.h"
#include "dsp/onepole.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/smoother.h"

#include <cmath>
#include <cstdint>

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
    // v1.3 (appended):
    kEcho,         // echo amount, 0..100 %: mix and repeats rise together; 0 = off
    // v1.5 Pattern generator (appended):
    kPtMode,       // Off / Hold: while a note is held, play a generated 16th line in its key
    kPtSeed,       // Pattern #, 1..999: which line ("Generate")
    kPtDen,        // Density, 0..100 %: share of steps that play
    kPtAcc,        // Accent, 0..100 %: share of steps accented
    kPtSld,        // Slide, 0..100 %: share of steps that tie into the next
    kPtRng,        // Range, 1..3 octaves
    kPtLen,        // Length, 1..32 steps
    kPtScl,        // Scale: Minor / Phrygian / Dorian / Minor Pent / Harm Minor
    // v1.6 Grit (appended):
    kOverdrive,    // oscillator level into the filter, x1 .. x66.6 (0..100 %)
    kMuffler,      // soft clip after the amp, 0..100 %
    kFullBass,     // Off / On: filter output coupling at 16 Hz instead of 78 Hz
    kNumParams
};

// Mappings from normalised knob travel (0..1) to the real quantity.
inline float cutoffHz(float n)   { return 20.0f * std::pow(400.0f, n); }      // 20 Hz .. 8 kHz (0 = nearly silent)
inline float decayMs(float n)    { return 50.0f * std::pow(50.0f, n); }       // 50 ms .. 2.5 s
inline float slideMs(float n)    { return 10.0f * std::pow(30.0f, n); }       // 10 .. 300 ms
inline float coeffForMs(float ms, float sr) { return 1.0f - std::exp(-1.0f / std::fmax(1.0f, ms * 0.001f * sr)); }
inline float decayPerSample(float ms, float sr) { return std::exp(-1.0f / std::fmax(1.0f, ms * 0.001f * sr)); }

// ---------------------------------------------------------------------------
// Filter (v1.4): diode ladder, modelled on the classic acid bass circuit.
//
//   x -> tanh input pair -> 4 coupled capacitor stages -> x5 output stage -> 78 Hz coupling HP -> y
//          ^                                                                                      |
//          +---- k * (y through a 13 Hz coupling HP) <---------------------------------------------+
//
// Unlike a transistor ladder (where the four stages are buffered from each other), each stage loads its neighbours (a diode ladder), and the bottom stage's
// capacitor is smaller (18/33 of the others). The two coupling high-passes sit inside the
// resonance loop, as in the circuit: they thin the low end and soften resonance when the filter
// is nearly closed. All six states are solved together each sample (zero-delay feedback,
// trapezoidal), so there is no unit delay in the loop; only the input tanh is nonlinear.
// The 6x6 system matrix is inverted at control rate. Runs at 2x the host rate.
// The displayed cutoff is where resonance peaks: the stage conductance is fc / 1.855.
// ---------------------------------------------------------------------------
class DiodeLadder
{
public:
    static constexpr int N = 6;                          // v0..v3 ladder, 4 = output HP, 5 = feedback HP
    static constexpr float kPeakRatio = 1.39f;           // resonant peak / stage conductance (v1.6)
    static constexpr float kOscK = 17.2f;                // loop gain that would self-oscillate (v1.6)
    static constexpr float kMaxK = 0.9f * kOscK;         // Resonance 100: rings hard, never whistles
    static constexpr float kOutGain = 1.0f;              // top stage: unity DC gain (before coupling)

    void reset() { for (float& v : s_) v = 0.0f; prevIn_ = 0.0f; }

    // fc in Hz, res 0..1, osRate = 2 * host rate, drive = input gain (Overdrive, 1..66.6),
    // fullBass = output coupling at 16 Hz instead of 78 Hz. Call at control rate.
    void set(float fc, float res, float osRate, float drive = 1.0f, bool fullBass = false)
    {
        fc = clamp(fc, 20.0f, 0.45f * osRate * 0.5f);
        const float g  = std::tan(kPi * (fc / kPeakRatio) / osRate);      // h * conductance (prewarped)
        const float wo = kPi * (fullBass ? 16.0f : 78.0f) / osRate, wf = kPi * 13.0f / osRate;  // h * coupling corners
        const float c0 = 33.0f / 18.0f;                                    // bottom stage: smaller capacitor
        k_ = kMaxK * res;
        // Level make-up: high resonance thins the passband (as in the original); recover about
        // half of it before the tanh (more squelch) and the rest after the filter.
        pre_  = (1.0f + 0.35f * res) * drive;
        post_ = kPostBase * (1.0f + kPostRes * res);
        // Overdrive level compensation: an overdriven input swamps the resonance's level loss, so
        // the output would rise by about 9 dB (Resonance 0) to 20 dB (Resonance 100). Take most of it back.
        if (drive > 1.0f)
            post_ *= std::pow(10.0f, -(8.7f + 11.5f * std::sqrt(res)) * (1.0f - 1.0f / drive) * (1.0f / 20.0f));
        hg_ = g;
        // Stage-saturation correction: full strength except at the very top of the range, where
        // a single correction step would overshoot.
        corr_ = std::fmin(1.0f, 0.5f / (g * c0));

        // hM: the continuous system v' = M v + b u, scaled by h = T/2.
        float m[N][N] = {};
        m[0][0] = -g * c0; m[0][1] = g * c0;                    // v1.6: the input pair feeds a current
        m[1][0] = g; m[1][1] = -2.0f * g; m[1][2] = g;
        m[2][1] = g; m[2][2] = -2.0f * g; m[2][3] = g;
        m[3][2] = g; m[3][3] = -2.0f * g;
        const float cy[N] = {0, 0, 0, kOutGain, -1.0f, 0};                 // y = 5 v3 - h
        const float cr[N] = {0, 0, 0, kOutGain, -1.0f, -1.0f};             // r = y - f
        for (int j = 0; j < N; ++j) { m[4][j] = wo * cy[j]; m[5][j] = wf * cr[j]; }
        // A = I - hM; invert by Gauss-Jordan (diagonally dominant enough: no pivoting needed).
        float a[N][2 * N];
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) { a[i][j] = (i == j ? 1.0f : 0.0f) - m[i][j]; a[i][N + j] = (i == j) ? 1.0f : 0.0f; }
        for (int c = 0; c < N; ++c)
        {
            const float inv = 1.0f / a[c][c];
            for (int j = 0; j < 2 * N; ++j) a[c][j] *= inv;
            for (int r = 0; r < N; ++r)
            {
                if (r == c || a[r][c] == 0.0f) continue;
                const float f = a[r][c];
                for (int j = 0; j < 2 * N; ++j) a[r][j] -= f * a[c][j];
            }
        }
        for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) n_[i][j] = a[i][N + j];
        // d = N * (h b), with h b = (g * c0) e0.
        for (int i = 0; i < N; ++i) d_[i] = n_[i][0] * g * c0;
        crd_ = 0.0f;
        for (int j = 0; j < N; ++j) { crd_ += cr[j] * d_[j]; q_[j] = 0.0f; }
        for (int j = 0; j < N; ++j) for (int i = 0; i < N; ++i) q_[j] += cr[i] * n_[i][j];   // q = cr * N
        denom_ = 1.0f / (1.0f + k_ * crd_);
        // Flush tiny states here (control rate) so a decaying tail never reaches denormals.
        for (float& v : s_) if (std::fabs(v) < 1.0e-20f) v = 0.0f;
    }

    float process(float in)
    {
        const float mid = 0.5f * (prevIn_ + in);
        prevIn_ = in;
        const float a = tick(mid);
        const float b = tick(in);
        return 0.5f * (a + b) * post_;
    }

private:
    float tick(float x)
    {
        x *= pre_;
        // Linear estimate of the feedback signal, then the saturated input with that feedback.
        float qs = 0.0f;
        for (int j = 0; j < N; ++j) qs += q_[j] * s_[j];
        const float rEst = (qs + crd_ * x) * denom_;
        const float u = fastTanh(x - k_ * rEst);
        float v[N];
        for (int i = 0; i < N; ++i)
        {
            float p = d_[i] * u;
            for (int j = 0; j < N; ++j) p += n_[i][j] * s_[j];
            v[i] = p;
        }
        // v1.6: inner-stage saturation. Each rung passes g*tanh(dv) instead of g*dv (and the top
        // termination g*tanh(v3)); one correction step from the linear prediction.
        const float e0 = v[0] - v[1], e1 = v[1] - v[2], e2 = v[2] - v[3], e3 = v[3];
        const float d0 = fastTanh(e0) - e0, d1 = fastTanh(e1) - e1, d2 = fastTanh(e2) - e2, d3 = fastTanh(e3) - e3;
        const float hc = hg_ * corr_;
        v[0] += hc * kC0 * (-d0);
        v[1] += hc * (d0 - d1);
        v[2] += hc * (d1 - d2);
        v[3] += hc * (d2 - d3);
        for (int i = 0; i < N; ++i) s_[i] = 2.0f * v[i] - s_[i];
        return kOutGain * v[3] - v[4];
    }

    float s_[N] = {}, n_[N][N] = {}, d_[N] = {}, q_[N] = {};
    float k_ = 0.0f, crd_ = 0.0f, denom_ = 1.0f, pre_ = 1.0f, post_ = 1.0f, prevIn_ = 0.0f, hg_ = 0.0f, corr_ = 1.0f;
    static constexpr float kC0 = 33.0f / 18.0f;           // bottom capacitor is 18/33 of the others
public:
    static constexpr float kPostBase = 2.0f, kPostRes = 1.2f;   // level make-up (matched to v1.5 loudness)
};

// ---------------------------------------------------------------------------
// AcidVoice: the sound. Knows nothing about MIDI.
// ---------------------------------------------------------------------------
struct VoiceTargets            // knob values, normalised 0..1
{
    int   wave = 0;
    float cutoff = 0.3f, reso = 0.6f, envMod = 0.5f, decay = 0.35f, accent = 0.5f;
    float slide = 0.2f, drive = 0.0f, tuneSemis = 0.0f;
    float overdrive = 0.0f, muffler = 0.0f; bool fullBass = false;   // v1.6
};

class AcidVoice
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        osc_.setSampleRate(sampleRate);
        attackNormal_ = coeffForMs(3.0f, sr_);                   // v1.6: soft attack on normal notes
        attackAccent_ = coeffForMs(0.5f, sr_);                   // accents start sharply
        releaseCoef_ = coeffForMs(8.0f, sr_);
        ampDecay_ = decayPerSample(3000.0f, sr_);
        capDrain_ = decayPerSample(100.0f, sr_);                 // accent sweep capacitor drains over ~100 ms
        capDrain16_ = std::pow(capDrain_, 16.0f);
        rampCoef_ = coeffForMs(10.0f, sr_);
        reset();
    }

    void reset()
    {
        filter_.reset();
        gate_ = false; gateLevel_ = 0.0f; amp_ = 0.0f; fenv_ = 0.0f;
        sweepCap_ = 0.0f; accented_ = false;
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
        if (first_) { cut_ = t.cutoff; res_ = t.reso; env_ = t.envMod; acc_ = t.accent; drv_ = t.drive; od_ = t.overdrive; mf_ = t.muffler; first_ = false; }
    }

    // pitch = MIDI note, accent = this note is accented, slide = glide without retrigger.
    void noteOn(int pitch, bool accent, bool slide)
    {
        pitchTarget_ = (float)pitch;
        accented_ = accent;
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
        gate_ = true;
        gateLevel_ = 1.0f;                                       // amp re-opens; fenv_ keeps going
    }

    void noteOff() { gate_ = false; }

    // Adds the voice into out[0..n).
    void render(float* out, int n)
    {
        if (!gate_ && amp_ == 0.0f)
        {
            // Silent: skip the DSP, but let the accent sweep capacitor keep draining.
            sweepCap_ *= std::pow(capDrain_, (float)n);
            if (sweepCap_ < 1.0e-4f) sweepCap_ = 0.0f;
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
                od_ += r * (t_.overdrive - od_); mf_ += r * (t_.muffler - mf_);

                osc_.setFrequency(midiToHz(pitch_ + t_.tuneSemis));
                // Accent voltage: follows the envelope while the note is accented (v1.4).
                const float accV = accented_ ? acc_ * fenv_ : 0.0f;
                // Accent sweep: the voltage charges a capacitor through a diode (charge only;
                // 32 ms at Resonance 0 .. 50 ms at 100), which drains over about 100 ms. Resonance
                // blends from the direct voltage (a quick blip) to the capacitor (a late swell that
                // builds over a run of accents). The capacitor can never exceed the direct voltage,
                // so a stream of accents (an arp) cannot pin the filter open.
                if (accV > sweepCap_) sweepCap_ += (1.0f - std::exp(-16.0f / ((0.032f + 0.018f * res_) * sr_))) * (accV - sweepCap_);
                else                  sweepCap_ *= capDrain16_;
                const float sweep = accV + res_ * (sweepCap_ - accV);
                // Env Mod (v1.4): the envelope rests below the cutoff reference, so more Env Mod
                // also lowers the resting cutoff (down 30 % of the sweep, up 70 %).
                const float octaves = env_ * kEnvOctaves * (fenv_ - kEnvRest) + kAccentOctaves * sweep;
                // Key tracking: the cutoff follows half the pitch distance from C1 (MIDI 36), so
                // higher and octave-jumped notes stay as bright as the rest.
                const float keyOct = kKeyTrack * (pitch_ + t_.tuneSemis - 36.0f) * (1.0f / 12.0f);
                filter_.set(cutoffHz(cut_) * std::pow(2.0f, clamp(octaves, -3.0f, 9.0f) + clamp(keyOct, -3.0f, 4.0f)), res_, osRate,
                            1.0f + 65.6f * od_ * od_, t_.fullBass);           // v1.6 Overdrive: x1 .. x66.6 into the filter
                // Muffler (v1.6): soft clip after the amp; the threshold falls 0.5 -> 0.1, fully mixed by 25 %.
                mfMix_ = clamp(4.0f * mf_, 0.0f, 1.0f);
                mfThr_ = 0.5f - 0.4f * mf_; mfInv_ = 1.0f / mfThr_;
                mfGain_ = 1.0f + 1.2f * mf_;
                // Progressive drive: gain into the clipper grows with the square of the knob,
                // so the whole travel is usable (it used to be fully saturated by 25 %).
                drivePre_ = 1.0f + 11.0f * drv_ * drv_;
                driveMix_ = clamp(drv_ * 2.0f, 0.0f, 1.0f);
                driveNorm_ = 0.5f / fastTanh(0.5f * drivePre_);
                // Accent punch (v1.4): extra level above a threshold, dying away with the accent
                // voltage, so it is a hit at the start (up to +8 dB), not a louder note.
                ampBoost_ = 1.0f + 1.6f * std::fmax(accV - 0.1f, 0.0f) * (1.0f / 0.9f);
            }

            pitch_ += slideCoef_ * (pitchTarget_ - pitch_);      // exponential glide in semitones
            fenv_ *= fdec;

            float target;
            if (gate_) { gateLevel_ *= ampDecay_; target = gateLevel_ * ampBoost_; amp_ += (accented_ ? attackAccent_ : attackNormal_) * (target - amp_); }
            else
            {
                amp_ -= releaseCoef_ * amp_;
                if (amp_ < 1.0e-5f) { amp_ = 0.0f; filter_.reset(); n = i + 1; }   // tail done: end cleanly
            }
            if (fenv_ < 1.0e-6f) fenv_ = 0.0f;

            // dcFix_ cancels the narrow square's DC offset, which would otherwise leak through a closed filter.
            float x = filter_.process((osc_.process() + dcFix_) * 0.8f) * amp_;
            if (mfMix_ > 0.0f) x = (x + mfMix_ * (mfThr_ * fastTanh(x * mfInv_) - x)) * mfGain_;
            float y = x;
            if (driveMix_ > 0.0f)
                y = x + driveMix_ * (fastTanh(x * drivePre_) * driveNorm_ - x);
            out[i] += y;
        }
    }

private:
    static constexpr float kEnvOctaves = 5.5f;   // Env Mod at 100 %: the envelope sweeps 5.5 octaves
    static constexpr float kEnvRest = 0.3f;      // envelope rest level below the cutoff reference (v1.4)
    static constexpr float kAccentOctaves = 3.0f; // accent sweep at Accent 100 (v1.4)
    static constexpr float kKeyTrack = 0.5f;     // filter key tracking: 0.5 octave per octave played
    PolyBlepOsc osc_;
    DiodeLadder filter_;
    VoiceTargets t_;
    float sr_ = 44100.0f;
    bool gate_ = false, accented_ = false, first_ = true;
    float gateLevel_ = 0.0f, amp_ = 0.0f, fenv_ = 0.0f, sweepCap_ = 0.0f;
    float pitch_ = 36.0f, pitchTarget_ = 36.0f;
    float attackNormal_ = 1.0f, attackAccent_ = 1.0f, releaseCoef_ = 1.0f, ampDecay_ = 1.0f, rampCoef_ = 1.0f;
    float decayNormal_ = 0.999f, decayAccent_ = 0.999f, slideCoef_ = 1.0f, capDrain_ = 0.999f, capDrain16_ = 0.99f;
    float cut_ = 0.3f, res_ = 0.6f, env_ = 0.5f, acc_ = 0.5f, drv_ = 0.0f;
    float drivePre_ = 1.0f, driveMix_ = 0.0f, driveNorm_ = 1.0f, ampBoost_ = 1.0f, dcFix_ = 0.0f;
    float od_ = 0.0f, mf_ = 0.0f, mfMix_ = 0.0f, mfThr_ = 0.5f, mfInv_ = 2.0f, mfGain_ = 1.0f;   // v1.6
    int ctrl_ = 0;
};

// ---------------------------------------------------------------------------
// Echo (v1.3): one knob. A dotted-8th delay synced to the MPC tempo. The knob raises the
// wet level (up to 45 %) and the repeats (feedback 15..65 %) together. Repeats are
// band-limited (about 250 Hz .. 3.5 kHz) so the bass stays solid and the echoes sit
// behind it. At 0, and once the tail has died away, it is bypassed completely.
// ---------------------------------------------------------------------------
class Echo
{
public:
    void prepare(double sampleRate)
    {
        sr_ = (float)sampleRate;
        line_.prepare((int)(sampleRate * kMaxSeconds) + 4);    // allocation: prepare() only
        hp_.setCutoff(250.0f, sampleRate);
        lp_.setCutoff(3500.0f, sampleRate);
        smoothCoef_ = 1.0f - std::exp(-1.0f / (0.05f * sr_)); // 50 ms glide for mix and tempo changes
        reset();
    }

    void reset() { line_.reset(); hp_.reset(); lp_.reset(); mix_ = fb_ = 0.0f; delay_ = target_; tail_ = 0; active_ = false; }

    // amount 0..1, bpm from the MPC (120 if it gives none). Call once per block.
    void setAmount(float amount, double bpm)
    {
        amount_ = amount;
        const double b = bpm < 40.0 ? 40.0 : (bpm > 300.0 ? 300.0 : bpm);
        target_ = (float)(0.75 * 60.0 / b * sr_);              // dotted 8th = 3/4 of a beat
        if (!active_ && amount > 0.0f) { active_ = true; delay_ = target_; }
    }

    float process(float x)
    {
        if (!active_) return x;
        const float mixT = 0.45f * amount_, fbT = amount_ > 0.0f ? 0.15f + 0.5f * amount_ : 0.0f;
        mix_ += smoothCoef_ * (mixT - mix_);
        fb_ += smoothCoef_ * (fbT - fb_);
        delay_ += smoothCoef_ * (target_ - delay_);
        const float wet = line_.readLinear(delay_);
        // Everything entering the line is band-limited, so even the first repeat is bass-light,
        // and each later repeat a little thinner and darker.
        float in = lp_.process(hp_.process(x + wet * fb_));
        if (std::fabs(in) < 1.0e-15f) in = 0.0f;               // no denormal tails
        line_.write(in);
        // Bypass again once the knob is at 0 and the tail has gone quiet for a whole delay time.
        if (amount_ == 0.0f && mix_ < 1.0e-4f && std::fabs(wet) < 1.0e-6f) { if (++tail_ > (int)target_) reset(); }
        else tail_ = 0;
        return x + mix_ * wet;
    }

private:
    static constexpr double kMaxSeconds = 1.2;                 // dotted 8th at 40 BPM is 1.125 s
    DelayLine line_;
    OnePoleHP hp_;
    OnePoleLP lp_;
    float sr_ = 44100.0f, amount_ = 0.0f, mix_ = 0.0f, fb_ = 0.0f, delay_ = 1.0f, target_ = 1.0f, smoothCoef_ = 1.0f;
    int tail_ = 0;
    bool active_ = false;
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
// Pattern (v1.5): a repeatable acid-line generator (our own design; see SPEC section 14).
// For each step and lane, a fixed number comes from hash(Pattern #, step, lane). The knobs only
// move thresholds against those numbers, so the line reshapes live and returns exactly when a
// knob returns. Steps are ranked within the line's Length, so amounts are exact and nested
// (Density 50 % of 16 steps plays exactly 8; turning a knob up only adds steps).
// ---------------------------------------------------------------------------
struct PatternStep { bool plays = false, accent = false, slide = false; int semis = 0; };

class Pattern
{
public:
    enum Lane : uint32_t { kPlay = 11, kAccent = 12, kSlide = 13, kPitch = 14, kOct = 15 };
    static constexpr int kMaxLen = 32;

    // Rank of this step's number among steps 0..len-1, as (rank + 0.5) / len.
    static float ranked(int step, int len, int seed, uint32_t lane)
    {
        const uint32_t h = Mutator::hash(step, seed, lane);
        int rank = 0;
        for (int o = 0; o < len; ++o)
        {
            const uint32_t ho = Mutator::hash(o, seed, lane);
            if (ho < h || (ho == h && o < step)) ++rank;
        }
        return ((float)rank + 0.5f) / (float)len;
    }

    static bool plays(int step, int len, int seed, float density) { return ranked(step, len, seed, kPlay) < density; }

    // step: 0..len-1. density/accent/slide: 0..1. range: 1..3 octaves. scale: 0..4.
    static PatternStep at(int step, int len, int seed, float density, float accent, float slide, int range, int scale)
    {
        PatternStep p;
        len = clamp(len, 1, kMaxLen);
        step = ((step % len) + len) % len;
        p.plays = plays(step, len, seed, density);
        if (!p.plays) return p;
        p.accent = ranked(step, len, seed, kAccent) < accent;
        // A slide into a rest is just a plain note.
        p.slide = ranked(step, len, seed, kSlide) < slide && plays((step + 1) % len, len, seed, density);
        p.semis = degree(Mutator::hash(step, seed, kPitch), scale);
        const float o = ranked(step, len, seed, kOct);           // top of the order goes up first
        if (range >= 3 && o > 0.88f) p.semis += 24;
        else if (range >= 2 && o > 0.70f) p.semis += 12;
        return p;
    }

private:
    // Scale degree in semitones above the root, weighted towards acid-friendly notes:
    // root 40 %, 5th 15 %, minor 3rd 12 %, 7th 10 %, the rest share 23 %.
    static int degree(uint32_t h, int scale)
    {
        static const int kScales[5][8] = {
            {0, 2, 3, 5, 7, 8, 10, -1},     // Minor
            {0, 1, 3, 5, 7, 8, 10, -1},     // Phrygian
            {0, 2, 3, 5, 7, 9, 10, -1},     // Dorian
            {0, 3, 5, 7, 10, -1, -1, -1},   // Minor Pent
            {0, 2, 3, 5, 7, 8, 11, -1}};    // Harm Minor
        const int* sc = kScales[clamp(scale, 0, 4)];
        int n = 0; while (n < 8 && sc[n] >= 0) ++n;
        float w[8]; int others = 0;
        for (int i = 0; i < n; ++i)
        {
            const int s = sc[i];
            w[i] = s == 0 ? 40.0f : s == 7 ? 15.0f : s == 3 ? 12.0f : (s == 10 || s == 11) ? 10.0f : -1.0f;
            if (w[i] < 0.0f) ++others;
        }
        float total = 0.0f;
        for (int i = 0; i < n; ++i) { if (w[i] < 0.0f) w[i] = others > 0 ? 23.0f / others : 0.0f; total += w[i]; }
        float u = (float)(h >> 8) * (1.0f / 16777216.0f) * total;
        for (int i = 0; i < n; ++i) { if (u < w[i]) return sc[i]; u -= w[i]; }
        return 0;
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
        // v1.3 Echo (appended).
        addParam(floatParam("echo", "Echo", 0.0f, 100.0f, 0.0f, "%", 0));
        // v1.5 Pattern generator (appended).
        addParam(choiceParam("pt_mode", "Pattern", {"Off", "Hold"}, 0));
        addParam(intParam("pt_seed", "Pattern #", 1, 999, 1));
        addParam(floatParam("pt_den", "Density", 0.0f, 100.0f, 75.0f, "%", 0));
        addParam(floatParam("pt_acc", "Pat Accent", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("pt_sld", "Pat Slide", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(intParam("pt_rng", "Range", 1, 3, 1, "oct"));
        addParam(intParam("pt_len", "Length", 1, 32, 16, "st"));
        addParam(choiceParam("pt_scl", "Scale", {"Minor", "Phrygian", "Dorian", "Min Pent", "Harm Min"}, 0));
        // v1.6 Grit (appended).
        addParam(floatParam("odrive", "Overdrive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("muffle", "Muffler", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("fullbs", "Full Bass", {"Off", "On"}, 0));
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
        program("Classic Squelch", {{kCutoff, 52}, {kReso, 80}, {kEnvMod, 60}, {kDecay, 55}, {kAccent, 60}});
        program("Rubber Sub", {{kWave, 1}, {kCutoff, 36}, {kReso, 40}, {kEnvMod, 30}, {kDecay, 68}, {kSlide, 50},
                               {kVolume, -7}});
        program("Acid Scream", {{kCutoff, 56}, {kReso, 90}, {kEnvMod, 70}, {kDecay, 48}, {kAccent, 80}, {kDrive, 35},
                                {kVolume, -5}});
        program("Mutant Squelch", {{kCutoff, 49}, {kReso, 82}, {kEnvMod, 62}, {kDecay, 50}, {kAccent, 65},
                                   {kMuOct, 25}, {kMuSld, 30}, {kMuRst, 20}, {kMuVar, 7}, {kEcho, 30}});
        // v1.5: Pattern presets. Hold a note (a pad, or a long note on the grid) to play the line.
        program("Acid Machine", {{kCutoff, 52}, {kReso, 80}, {kEnvMod, 60}, {kDecay, 55}, {kAccent, 60},
                                 {kPtMode, 1}, {kPtSeed, 7}, {kPtDen, 75}, {kPtAcc, 30}, {kPtSld, 20}});
        program("Mutant Machine", {{kCutoff, 49}, {kReso, 82}, {kEnvMod, 62}, {kDecay, 50}, {kAccent, 65},
                                   {kMuOct, 20}, {kMuSld, 20}, {kMuRst, 15}, {kMuVar, 7}, {kEcho, 25},
                                   {kPtMode, 1}, {kPtSeed, 42}, {kPtDen, 80}, {kPtAcc, 35}, {kPtSld, 25}, {kPtRng, 2}});
        // v1.6: Grit preset.
        program("Grit Squelch", {{kCutoff, 46}, {kReso, 78}, {kEnvMod, 65}, {kDecay, 45}, {kAccent, 70},
                                 {kOverdrive, 45}, {kMuffler, 20}, {kFullBass, 1}, {kVolume, -4}});
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        voice_.prepare(sampleRate);
        sr_ = sampleRate;
        lowCut_.setCoeffs(BiquadCoeffs::highpass(sampleRate, 30.0f));   // fixed 30 Hz, 12 dB/oct
        lowCut_.reset();
        echo_.prepare(sampleRate);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        voice_.reset();
        notes_.reset();
        lowCut_.reset();
        echo_.reset();
        noteCounter_ = 0; wasPlaying_ = false; sinceNote_ = 1.0e9;
        heldCount_ = 0; stopPattern(); lastK_ = INT64_MIN; freeToNext_ = 0.0; freeCount_ = 0;
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
        echo_.setAmount(getNormalised(kEcho), bpm);
        const double beatsPerBar = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        const int    stepsPerBar = mpc::clamp((int)std::lround(beatsPerBar * 4.0), 1, 64);
        // Slide-in only within one bar of the previous note (nothing to slide from after a long gap).
        const double slideWindow = beatsPerBar * 60.0 / bpm * sr;

        float* L = ctx.outputs[0];
        // v1.5: switching Pattern on or off releases everything cleanly.
        const bool hold = getInt(kPtMode) == 1;
        if (hold != patternWasOn_) { notes_.allNotesOff(voice_); stopPattern(); heldCount_ = 0; patternWasOn_ = hold; }
        if (hold)
            renderPattern(ctx, L, onGrid, bpm, sr, beatsPerBar, stepsPerBar, muOct, muSld, muRst, muVar);
        else
        {
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
        }
        sinceNote_ += ctx.numFrames;

        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        bool silentIn = true;
        float outPeak = 0.0f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            if (L[i] != 0.0f) silentIn = false;
            const float y = echo_.process(lowCut_.process(L[i]));
            outPeak = std::fmax(outPeak, std::fabs(y));
            L[i] = ceiling(y * volume_.process());
            if (R) R[i] = L[i];
        }
        // The voice and the echo have stopped and the low cut's tail is below -140 dB: clear it,
        // so it never decays into denormals.
        if (silentIn && outPeak < 1.0e-7f) lowCut_.reset();
    }

    // -----------------------------------------------------------------------
    // v1.5 Pattern (Hold): while a key is held, play the generated line in its key.
    // Steps sit on the song's 16th grid while the MPC plays (step k of the song plays line step
    // k mod Length), or run from the key press at the MPC tempo when it is stopped. Held keys
    // make no sound themselves; the newest held key is the root.
    // -----------------------------------------------------------------------
    void stopPattern()
    {
        if (genNoteOn_) voice_.noteOff();
        genNoteOn_ = genTied_ = genStarted_ = false;
        gateAt_ = -1.0;
    }

    void keyDown(int note)
    {
        keyUp(note);
        if (heldCount_ == kMaxHeld) { for (int i = 1; i < kMaxHeld; ++i) held_[i - 1] = held_[i]; --heldCount_; }
        held_[heldCount_++] = note;
    }
    void keyUp(int note)
    {
        int w = 0;
        for (int r = 0; r < heldCount_; ++r) if (held_[r] != note) held_[w++] = held_[r];
        heldCount_ = w;
    }

    // Play one step of the line at sample t. lineStep: position in the line; barStep: 16th in
    // the bar (for Mutate).
    void fireStep(double t, int lineStep, int barStep, double sps, float muOct, float muSld, float muRst, int muVar)
    {
        const int len = getInt(kPtLen);
        const PatternStep ps = Pattern::at(lineStep, len, getInt(kPtSeed), getNormalised(kPtDen), getNormalised(kPtAcc),
                                           getNormalised(kPtSld), getInt(kPtRng), getInt(kPtScl));
        const Mutation m = Mutator::decide(barStep, muVar, muOct, muSld, muRst);
        if (!ps.plays || m.rest)
        {
            if (genNoteOn_) { voice_.noteOff(); genNoteOn_ = false; }
            genTied_ = false; gateAt_ = -1.0;
            return;
        }
        int pitch = held_[heldCount_ - 1] + ps.semis;
        if (m.octave && pitch + 12 <= 127) pitch += 12;
        pitch = clamp(pitch, 0, 127);
        const bool accent = (ps.accent ? 127 : 80) >= getInt(kAccThr);
        if (genNoteOn_ && genTied_) voice_.noteOn(pitch, accent, true);   // tied from the last step: slide
        else if (m.slide && genStarted_) voice_.slideIn(pitch, accent);    // Mutate's Add Slide
        else voice_.noteOn(pitch, accent, false);
        genNoteOn_ = genStarted_ = true;
        genTied_ = ps.slide;
        gateAt_ = ps.slide ? -1.0 : t + 0.5 * sps;                        // plain notes last half a step
    }

    void renderPattern(mpc::ProcessContext& ctx, float* L, bool onGrid, double bpm, double sr, double beatsPerBar,
                       int stepsPerBar, float muOct, float muSld, float muRst, int muVar)
    {
        const mpc::Transport& tr = ctx.transport;
        const int n = ctx.numFrames;
        const double sps = sr * 60.0 / bpm / 4.0;                  // samples per 16th
        const double dp = bpm / 60.0 / sr;                         // quarter notes per sample
        const double p0 = tr.ppqPos;
        if (onGrid != genOnGrid_) { freeToNext_ = 0.0; freeCount_ = 0; genOnGrid_ = onGrid; }
        if (onGrid && p0 * 4.0 < (double)lastK_ - 0.5) lastK_ = INT64_MIN;   // the playhead jumped back (loop)
        auto barStepAt = [&](double ppq)
        {
            double rel = std::fmod(ppq - tr.barStartPpq, beatsPerBar);
            if (rel < 0.0) rel += beatsPerBar;
            return (int)std::lround(rel * 4.0) % stepsPerBar;
        };
        auto lineOf = [&](int64_t k) { const int len = clamp(getInt(kPtLen), 1, Pattern::kMaxLen); return (int)(((k % len) + len) % len); };
        auto gridStep = [&](int64_t k, double t)
        {
            lastK_ = k;
            fireStep(t, lineOf(k), barStepAt(k * 0.25), sps, muOct, muSld, muRst, muVar);
        };
        auto freeStep = [&](double t)
        {
            fireStep(t, lineOf(freeCount_), freeCount_ % 16, sps, muOct, muSld, muRst, muVar);
            ++freeCount_;
            freeToNext_ = t + sps;
        };
        // The next grid step at or after sample `from`.
        auto nextGrid = [&](double from, int64_t& k)
        {
            k = (int64_t)std::ceil((p0 + from * dp) * 4.0 - 1.0e-3);
            if (k <= lastK_) k = lastK_ + 1;
            return std::fmax(from, (k * 0.25 - p0) / dp);
        };

        int e = 0, pos = 0;
        for (;;)
        {
            const double tMidi = e < ctx.numMidiEvents ? (double)clamp(ctx.midi[e].sampleOffset, 0, n) : 1.0e18;
            const double tGate = gateAt_ >= 0.0 ? std::fmax(gateAt_, (double)pos) : 1.0e18;
            int64_t k = 0;
            double tStep = 1.0e18;
            if (heldCount_ > 0) tStep = onGrid ? nextGrid(pos, k) : std::fmax(freeToNext_, (double)pos);
            const double tNext = std::fmin(tMidi, std::fmin(tGate, tStep));
            const int ti = (int)std::ceil(tNext - 1.0e-6);
            if (ti >= n && tMidi > n) break;                       // nothing more in this block
            const int at = clamp(ti, pos, n);
            if (at > pos) { voice_.render(L + pos, at - pos); pos = at; }
            if (tMidi <= tNext)                                    // keys first, so a key on a step plays it
            {
                const mpc::MidiEvent& ev = ctx.midi[e++];
                if (ev.isNoteOn())
                {
                    const bool first = heldCount_ == 0;
                    keyDown(ev.note());
                    if (first)
                    {
                        genStarted_ = false;
                        if (onGrid)
                        {
                            // Pressed just after a step (up to a quarter step late): play that step now.
                            const double q = (p0 + at * dp) * 4.0;
                            const int64_t kp = (int64_t)std::floor(q + 1.0e-6);
                            if (q - (double)kp < 0.25 && kp > lastK_) gridStep(kp, at);
                        }
                        else { freeCount_ = 0; freeStep(at); }
                    }
                }
                else if (ev.isNoteOff()) { keyUp(ev.note()); if (heldCount_ == 0) stopPattern(); }
                else if (ev.isAllNotesOff()) { heldCount_ = 0; stopPattern(); }
            }
            else if (tGate <= tNext) { voice_.noteOff(); genNoteOn_ = false; gateAt_ = -1.0; }
            else if (onGrid) gridStep(k, at);
            else freeStep(at);
        }
        if (n > pos) voice_.render(L + pos, n - pos);
        if (gateAt_ >= 0.0) gateAt_ -= n;
        if (!onGrid && heldCount_ > 0) freeToNext_ -= n;
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
        t.overdrive = getNormalised(kOverdrive);
        t.muffler = getNormalised(kMuffler);
        t.fullBass = getInt(kFullBass) == 1;
        return t;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM-03";
        info.vendor       = "OCKM";     // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', '5', 'u', 'Q');   // must match plugin.json "uid"
        info.version      = 1600;       // 1.6.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        return info;
    }

    AcidVoice voice_;
    NoteInput notes_;
    Echo echo_;
    Biquad lowCut_;                // fixed 30 Hz high-pass: trims sub rumble so the kick has room
    Smoother volume_;
    double sr_ = 44100.0;
    double sinceNote_ = 1.0e9;     // samples since the last played note-on
    int noteCounter_ = 0;          // step source when the MPC gives no playhead
    bool wasPlaying_ = false;
    // v1.5 Pattern state.
    static constexpr int kMaxHeld = 16;
    int held_[kMaxHeld] = {};
    int heldCount_ = 0;
    bool patternWasOn_ = false, genNoteOn_ = false, genTied_ = false, genStarted_ = false, genOnGrid_ = false;
    double gateAt_ = -1.0;         // sample (in this block) where the current plain note ends; -1 = none
    double freeToNext_ = 0.0;      // transport stopped: samples until the next step
    int64_t freeCount_ = 0;        // transport stopped: steps since the key went down
    int64_t lastK_ = INT64_MIN;    // last song 16th played (so a step never plays twice)
};

// Creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKM03)
