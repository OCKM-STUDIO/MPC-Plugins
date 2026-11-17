// OCKM FM Kit - eight one-shot FM drums for the MPC, by OCKM.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// Signal flow
//   MIDI note -> drum = (note - 36) wrapped onto 0..7 -> (closed hat? choke the open hat) -> trigger
//   each drum: 2-3 sine "operators" (one shared sine table, phase modulation) + noise where noted,
//              shaped by exponential envelopes (one multiply per sample each)
//            -> fixed pan -> Main (outputs 1-2) or Aux (outputs 3-4) depending on Routing
//   buses -> Master (smoothed) -> soft safety limiter (output can never reach 0 dBFS)
//
// FM in one sentence: a "modulator" sine wobbles the read position of a "carrier" sine; how far it
// wobbles (the index, our Tone knob) sets brightness, and the speed ratio between them sets whether
// it sounds pitched (whole numbers) or metallic (odd ratios).
//
// Note map: 36 Kick, 37 Snare, 38 Clap, 39 Closed Hat, 40 Open Hat, 41 Low Tom, 42 High Tom, 43 Perc.
// Every other note wraps onto the same eight (44 = Kick, 35 = Perc). Note-offs are ignored.
//
// Parameters (ids and order are permanent): 0 Master, 1 Routing, then five per drum, added in a
// loop: Tune, Decay, Tone, Level and Morph (which blends each drum through three characters).
// v1.2 added three kit-wide knobs at the end (42-44): Variation, Vel>Morph and Drive. All three
// default to 0, where the kit sounds exactly as it did in 1.1.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/noise.h"
#include "dsp/smoother.h"

#include <cmath>
#include <string>
#include <vector>

using namespace mpc::dsp;

namespace fmkit {

enum Drum { kKick = 0, kSnare, kClap, kClosedHat, kOpenHat, kLowTom, kHighTom, kPerc, kNumDrums };
enum Knob { kTune = 0, kDecay, kTone, kLevel, kChar, kKnobsPerDrum };
enum GlobalParam { kMaster = 0, kRouting, kFirstDrumParam };
constexpr int kVariation = kFirstDrumParam + kNumDrums * kKnobsPerDrum;   // 42 (v1.2)
constexpr int kVelMorph  = kVariation + 1;                                 // 43 (v1.2)
constexpr int kDrive     = kVariation + 2;                                 // 44 (v1.2)
constexpr int kNumParams = kVariation + 3;                                 // 45
constexpr int kFirstNote = 36;

inline int paramIndex(int drum, int knob) { return kFirstDrumParam + drum * kKnobsPerDrum + knob; }

// ----- one shared sine table (2048 points + a guard point), built once ----------------------
constexpr int kTableSize = 2048;
struct SineTable
{
    float v[kTableSize + 1];
    SineTable() { for (int i = 0; i <= kTableSize; ++i) v[i] = (float)std::sin(6.283185307179586 * i / kTableSize); }
};
inline const SineTable& sineTable() { static const SineTable t; return t; }

// Sine of a phase given in cycles (any value; wrapped here). Linear interpolation.
inline float sine(const float* tab, float phase)
{
    phase -= std::floor(phase);
    const float x = phase * (float)kTableSize;
    int i = (int)x;
    if (i >= kTableSize) i = kTableSize - 1;
    const float f = x - (float)i;
    return tab[i] + f * (tab[i + 1] - tab[i]);
}

// Coefficient for an exponential decay that falls 60 dB in `ms`.
inline float decayCoef(float ms, float sr) { return std::exp(-6.9078f / (0.001f * std::fmax(ms, 0.1f) * sr)); }
// 0..1 knob -> lo..hi on a log scale (so the bottom of the knob isn't cramped).
inline float expMap(float t, float lo, float hi) { return lo * std::pow(hi / lo, clamp(t, 0.0f, 1.0f)); }
// Morph: blend from `a` (0%) through `b` (50%) to `c` (100%).
inline float m3(float a, float b, float c, float x) { return x < 0.5f ? lerp(a, b, 2.0f * x) : lerp(b, c, 2.0f * x - 1.0f); }
inline float hpCoef(float hz, float sr) { return 1.0f - std::exp(-6.2831853f * hz / sr); }

// Per-drum Decay ranges (ms to -60 dB) and pans.
static const float kDecayLo[kNumDrums] = {80, 60, 80, 15, 150, 100, 80, 20};
static const float kDecayHi[kNumDrums] = {1500, 500, 900, 300, 2000, 1200, 1000, 800};
static const float kPan[kNumDrums]     = {0.0f, 0.0f, 0.15f, -0.25f, 0.25f, -0.35f, 0.35f, 0.4f};
// How much of the Drive knob each drum takes: the kick most, the hats least (they turn harsh fast).
static const float kDriveAmt[kNumDrums] = {1.0f, 0.8f, 0.7f, 0.5f, 0.5f, 0.8f, 0.8f, 0.6f};

// Smooth saturation (a tanh look-alike): straight near zero, bends over, flat at +-1 beyond +-3.
inline float sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

constexpr float kOff = 3.0e-4f;   // -70 dB: a drum below this is finished and costs nothing

// ----- one drum voice -------------------------------------------------------------------------
struct Voice
{
    Drum type = kKick;
    float sr = 44100.0f;
    const float* tab = nullptr;
    bool active = false;
    float gainL = 0.7f, gainR = 0.7f;   // fixed pan

    float gain = 0.0f;                 // level x velocity
    float amp = 0.0f, ampK = 0.0f;     // main amplitude envelope
    float pEnv = 0.0f, pK = 0.0f;      // pitch envelope
    float mEnv = 0.0f, mK = 0.0f;      // modulator (brightness) envelope
    float nEnv = 0.0f, nK = 0.0f;      // noise / clap-tail envelope
    float inc = 0.0f;                  // carrier base increment (cycles per sample)
    float sweep = 0.0f;                // pitch envelope depth (multiplier - 1)
    float ratio2 = 1.0f, ratio3 = 1.0f;
    float idxBase = 0.0f, idxEnv = 0.0f, idx3 = 0.0f;
    float noiseAmt = 0.0f, dev = 0.0f, body = 1.0f;
    float fbC = 0.0f, fbM = 0.0f;      // feedback (carrier / top modulator wobbling itself), in cycles
    float c1 = 0, c2 = 0, o1 = 0, o2 = 0;   // last two outputs of the carrier and of the top modulator
    float ph1 = 0, ph2 = 0, ph3 = 0;
    float hpA = 0.0f, lp1 = 0.0f, lp2 = 0.0f;   // one-pole high-pass states
    float lpA = 1.0f, lp3 = 0.0f;               // snare: low-pass on the wires (top of the band)
    float sEnv = 0.0f, sK = 0.0f, snapAmt = 0.0f;   // snare: extra burst of noise at the start (the crack)
    int burstsLeft = 0, burstTimer = 0, burstGap = 0;
    float tail = 0.0f, tailK = 0.0f;   // declick: the previous sound fading out over ~1.5 ms
    float lastOut = 0.0f;
    float drvG = 0.0f, drvN = 1.0f, drvMix = 0.0f;   // Drive: input gain, output scale, wet amount (0 = off)
    Xorshift32 rng;

    void prepare(Drum t, float sampleRate, uint32_t seed)
    {
        type = t; sr = sampleRate; tab = sineTable().v;
        rng.reseed(seed);
        equalPowerPan(kPan[t], gainL, gainR);
        tailK = decayCoef(1.5f, sr) ;
        kill(); tail = 0.0f; lastOut = 0.0f;
    }
    // Stop the sound; whatever was playing fades out over ~1.5 ms instead of jumping to zero.
    void kill() { if (active) tail = lastOut; active = false; amp = nEnv = 0.0f; burstsLeft = 0; }
    bool busy() const { return active || std::fabs(tail) > 1.0e-5f; }

    void trigger(float tune, float decay, float tone, float level, float mo, float vel, float drive)
    {
        tail = active ? lastOut : tail;    // retrigger without a click
        active = true;
        const float r = semitonesToRatio(tune);
        const float bright = 0.8f + 0.2f * vel;   // harder hits are a little brighter
        tone *= bright;
        gain = level * (0.32f + 0.68f * vel);
        amp = 1.0f; pEnv = 1.0f; mEnv = 1.0f; nEnv = 0.0f;
        ph1 = ph2 = ph3 = 0.0f; lp1 = lp2 = lp3 = 0.0f; sEnv = 0.0f;
        ampK = decayCoef(expMap(decay, kDecayLo[type], kDecayHi[type]), sr);
        sweep = 0.0f; ratio2 = 1.0f; idxBase = idxEnv = idx3 = 0.0f; noiseAmt = 0.0f; body = 1.0f;
        fbC = fbM = 0.0f; c1 = c2 = o1 = o2 = 0.0f;
        // Drive (0..1, already scaled for this drum): blends in a saturated copy that is boosted by up to
        // 10x (+20 dB) and then scaled back so a full-scale signal stays at full scale. Quiet parts (the
        // body and tail) come up a lot, loud peaks get rounded off: fatter, grittier and louder on
        // average, while peaks move only a few dB. The safety limiter still guards the output.
        drvMix = clamp(2.0f * drive, 0.0f, 1.0f);
        drvG = 1.0f + 9.0f * drive * drive;
        drvN = 1.0f / sat(drvG);
        const float x = clamp(mo, 0.0f, 1.0f);   // Morph 0..1; each drum blends three characters (0 / 50 / 100%)
        switch (type)
        {
            case kKick:      // round sub -> punchy classic -> hard FM boom with a zap
                inc = 48.0f * r / sr;
                sweep = m3(0.3f, 4.0f, 10.0f, x);
                pK = decayCoef(m3(80.0f, 160.0f, 45.0f, x), sr);
                ratio2 = m3(1.0f, 1.0f, 1.5f, x);
                idxBase = tone * m3(0.0f, 0.25f, 0.8f, x); idxEnv = tone * m3(0.6f, 3.0f, 5.0f, x);
                mK = decayCoef(m3(20.0f, 60.0f, 150.0f, x), sr);
                fbC = m3(0.0f, 0.0f, 0.2f, x);
                break;
            case kSnare:     // tight tuned snare -> classic snare -> noisy crack with a metallic ring
                // A snare is a short, pitch-dropping drum "body" plus the rattle of the wires (band-passed
                // noise), which is louder and lasts longer than the body. Decay sets both.
                inc = 180.0f * r / sr;
                sweep = m3(0.6f, 0.8f, 0.5f, x); pK = decayCoef(m3(40.0f, 30.0f, 20.0f, x), sr);
                ratio2 = m3(1.5f, 1.6f, 2.76f, x);
                idxBase = tone * m3(0.2f, 0.4f, 1.2f, x); idxEnv = tone * m3(1.0f, 2.0f, 4.0f, x);
                mK = decayCoef(m3(60.0f, 80.0f, 200.0f, x), sr);
                fbM = m3(0.0f, 0.0f, 0.15f, x);
                ampK = decayCoef(expMap(decay, 40.0f, 250.0f) * m3(1.3f, 1.0f, 0.8f, x), sr);   // body
                body = m3(1.0f, 0.8f, 0.5f, x);
                noiseAmt = m3(1.6f, 2.0f, 2.8f, x); nEnv = 1.0f;                                  // wires
                nK = decayCoef(expMap(decay, 80.0f, 500.0f) * m3(0.8f, 1.0f, 1.3f, x), sr);
                hpA = hpCoef(m3(600.0f, 1200.0f, 2500.0f, x), sr);
                lpA = hpCoef(m3(5000.0f, 9000.0f, 14000.0f, x), sr);
                sEnv = 1.0f; sK = decayCoef(12.0f, sr); snapAmt = m3(0.5f, 1.0f, 1.5f, x);     // crack
                break;
            case kClap:      // one tight snap -> classic three-clap -> wide, lower five-clap
            {
                inc = 1200.0f * m3(1.4f, 1.0f, 0.7f, x) * r / sr;
                dev = inc * (0.6f + 3.4f * tone);                         // Tone: band width
                const int claps = 1 + (int)std::lround(4.0f * x);         // 1..5
                burstGap = (int)(0.001f * m3(5.0f, 11.0f, 22.0f, x) * sr);
                burstTimer = burstGap; burstsLeft = claps - 1;
                mK = decayCoef(m3(30.0f, 30.0f, 45.0f, x), sr);           // each clap
                nK = ampK;                                                 // the tail uses Decay
                noiseAmt = m3(0.3f, 0.8f, 1.0f, x);                        // tail level
                if (burstsLeft == 0) nEnv = 1.0f;
                amp = 0.0f;
                break;
            }
            case kClosedHat:
            case kOpenHat:   // soft glassy shimmer -> classic metal -> trashy, noisy cymbal
                inc = 2400.0f * r / sr;
                ratio2 = m3(2.0f, 2.56f, 3.53f, x);
                ratio3 = ratio2 * m3(1.5f, 1.73f, 2.31f, x);
                idxBase = m3(0.15f, 1.0f, 1.6f, x) * (1.0f + 4.0f * tone);   // 0%: few partials, a tonal ring
                idx3 = m3(0.0f, 1.0f, 1.8f, x) * 3.0f * tone;
                fbM = m3(0.0f, 0.02f, 0.3f, x);                               // 100%: top operator turns to noise
                hpA = hpCoef(m3(4000.0f, 5000.0f, 2500.0f, x), sr);
                ampK = decayCoef(expMap(decay, kDecayLo[type], kDecayHi[type]) * m3(0.8f, 1.0f, 1.6f, x), sr);   // trashy rings on
                break;
            case kLowTom:
            case kHighTom:   // soft round tom -> synth tom with a big bend -> metallic conga / ping
                inc = (type == kLowTom ? 95.0f : 160.0f) * r / sr;
                sweep = m3(0.3f, 2.5f, 0.6f, x); pK = decayCoef(m3(240.0f, 300.0f, 80.0f, x), sr);
                ratio2 = m3(1.0f, 1.0f, 3.5f, x);
                idxBase = tone * m3(0.1f, 0.2f, 0.6f, x); idxEnv = tone * m3(1.0f, 2.0f, 4.0f, x);
                mK = decayCoef(m3(60.0f, 80.0f, 40.0f, x), sr);
                break;
            case kPerc:
            default:         // rim / clave click -> woodblock / cowbell -> long bell / chime
                inc = 800.0f * m3(1.6f, 1.0f, 1.1f, x) * r / sr;
                ratio2 = m3(2.9f, 1.48f, 3.5f, x);
                idxBase = tone * m3(0.6f, 0.5f, 0.2f, x); idxEnv = tone * m3(4.0f, 3.0f, 0.8f, x);
                mK = decayCoef(m3(5.0f, 60.0f, 500.0f, x), sr);
                fbC = m3(0.0f, 0.12f, 0.0f, x);
                ampK = decayCoef(expMap(decay, kDecayLo[type], kDecayHi[type]) * m3(0.4f, 1.0f, 2.5f, x), sr);
                break;
        }
    }

    // One sample (already scaled by level/velocity), including any declick tail.
    inline float process()
    {
        float y = 0.0f;
        if (active)
        {
            switch (type)
            {
                case kKick:
                case kLowTom:
                case kHighTom:
                case kPerc:
                case kSnare:
                {
                    const float i1 = inc * (1.0f + sweep * pEnv);
                    const float o = sine(tab, ph2 + fbM * (o1 + o2));          // modulator (with optional feedback)
                    o2 = o1; o1 = o * 0.5f;
                    const float m = o * (idxBase + idxEnv * mEnv);
                    const float c = sine(tab, ph1 + m + fbC * (c1 + c2));     // carrier (with optional feedback)
                    c2 = c1; c1 = c * 0.5f;
                    y = c * amp;
                    ph1 += i1; ph2 += i1 * ratio2;
                    if (ph1 >= 1.0f) ph1 -= 1.0f;
                    if (ph2 >= 1.0f) ph2 -= std::floor(ph2);
                    if (type == kSnare)
                    {
                        const float n = rng.nextBipolar();
                        lp1 += hpA * (n - lp1);                      // high-pass: take away the low rumble
                        lp3 += lpA * ((n - lp1) - lp3);              // low-pass: take the fizz off the top
                        y = y * body + lp3 * noiseAmt * (nEnv + snapAmt * sEnv) * 0.75f;
                        y *= 0.55f;                                  // noise peaks high: sit level with the kick
                        nEnv *= nK; sEnv *= sK;
                    }
                    else if (type == kKick) y *= 1.4f;    // the kick leads the mix
                    pEnv *= pK; mEnv *= mK; amp *= ampK;
                    if (amp < kOff && nEnv < kOff) active = false;
                    break;
                }
                case kClap:
                {
                    // Frequency wobbled by noise: instantaneous pitch = carrier +- dev.
                    ph1 += inc + dev * rng.nextBipolar();
                    ph1 -= std::floor(ph1);
                    const float s = sine(tab, ph1);
                    y = s * (mEnv + noiseAmt * nEnv) * 0.55f;
                    mEnv *= mK; nEnv *= nK;
                    if (burstsLeft > 0 && --burstTimer <= 0)
                    {
                        mEnv = 1.0f; burstTimer = burstGap;
                        if (--burstsLeft == 0) nEnv = 1.0f;   // the tail starts with the last clap
                    }
                    if (burstsLeft == 0 && mEnv < kOff && nEnv < kOff) active = false;
                    break;
                }
                case kClosedHat:
                case kOpenHat:
                default:
                {
                    const float o = sine(tab, ph3 + fbM * (o1 + o2));          // top operator, feeding back on itself
                    o2 = o1; o1 = o * 0.5f;
                    const float m2 = sine(tab, ph2 + o * idx3) * idxBase;
                    const float c = sine(tab, ph1 + m2);
                    ph1 += inc; ph2 += inc * ratio2; ph3 += inc * ratio3;
                    if (ph1 >= 1.0f) ph1 -= std::floor(ph1);
                    if (ph2 >= 1.0f) ph2 -= std::floor(ph2);
                    if (ph3 >= 1.0f) ph3 -= std::floor(ph3);
                    lp1 += hpA * (c - lp1); const float h1 = c - lp1;     // two gentle high-passes
                    lp2 += hpA * (h1 - lp2); const float h2 = h1 - lp2;
                    y = h2 * amp * 1.3f;
                    amp *= ampK;
                    if (amp < kOff) active = false;
                    break;
                }
            }
            if (drvMix > 0.0f) y += drvMix * (sat(y * drvG) * drvN - y);
            y *= 0.5f * gain;   // one drum peaks near -6 dBFS before panning
        }
        y += tail;
        tail *= tailK;
        if (std::fabs(tail) < 1.0e-6f) tail = 0.0f;
        lastOut = y;
        return y;
    }
};

// Safety limiter: untouched below 0.75 (-2.5 dBFS), then bends smoothly towards 1.0 and never gets there.
inline float limit(float x)
{
    const float T = 0.75f;
    const float a = std::fabs(x);
    if (a <= T) return x;
    const float e = (a - T) / (1.0f - T);
    const float y = T + (1.0f - T) * e / (1.0f + e);
    return x < 0.0f ? -y : y;
}

} // namespace fmkit

class OCKMFMKit : public mpc::Plugin
{
public:
    OCKMFMKit() : mpc::Plugin(makeInfo())
    {
        using namespace fmkit;
        // Parameter 0 is Master: the MPC nudges it on every load, and it does nothing else.
        addParam(mpc::floatParam("master", "Master", -40.0f, 6.0f, -3.0f, "dB", 1));
        addParam(mpc::choiceParam("routing", "Routing", {"All Main", "Kick>Aux", "Hats>Aux", "Split"}, 0));
        // (the kit-wide v1.2 knobs are added after the drums, so the older parameter numbers never move)

        static const char* const ids[kNumDrums]   = {"kick", "snare", "clap", "chat", "ohat", "ltom", "htom", "perc"};
        static const char* const names[kNumDrums] = {"Kick", "Snare", "Clap", "CHat", "OHat", "LTom", "HTom", "Perc"};
        static const char* const charId[kNumDrums]   = {"punch", "snap", "spread", "metal", "metal", "bend", "bend", "ratio"};
        // The ids below are the original v1 ids and are permanent; on screen every fifth knob is "Morph".
        static const char* const charName[kNumDrums] = {"Morph", "Morph", "Morph", "Morph", "Morph", "Morph", "Morph", "Morph"};
        for (int d = 0; d < kNumDrums; ++d)
        {
            const std::string id = ids[d], nm = names[d];
            const float* def = kInit[d];
            addParam(mpc::floatParam((id + "_tune").c_str(),  (nm + " Tune").c_str(),  -24.0f, 24.0f, def[kTune], "st", 1));
            addParam(mpc::floatParam((id + "_decay").c_str(), (nm + " Decay").c_str(), 0.0f, 100.0f, def[kDecay], "%", 0));
            addParam(mpc::floatParam((id + "_tone").c_str(),  (nm + " Tone").c_str(),  0.0f, 100.0f, def[kTone], "%", 0));
            addParam(mpc::floatParam((id + "_level").c_str(), (nm + " Level").c_str(), 0.0f, 100.0f, def[kLevel], "%", 0));
            addParam(mpc::floatParam((id + "_" + charId[d]).c_str(), (nm + " " + charName[d]).c_str(), 0.0f, 100.0f, def[kChar], "%", 0));
        }
        addParam(mpc::floatParam("variation", "Variation", 0.0f, 100.0f, 0.0f, "%", 0));   // 42
        addParam(mpc::floatParam("velmorph", "Vel>Morph", 0.0f, 100.0f, 0.0f, "%", 0));    // 43
        addParam(mpc::floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));           // 44

        // Kits for house and techno (master -3 dB, everything on Main), then Variation, Vel>Morph, Drive.
        addKit("Init Kit", kInit, 0, 0, 0);
        addKit("Deep House Kit", kDeepHouse, 25, 30, 10);
        addKit("Warehouse Techno", kWarehouse, 15, 40, 45);
        addKit("Minimal Clicks", kMinimal, 10, 50, 0);
        addKit("Industrial Clang", kIndustrial, 20, 30, 80);
        addKit("Rumble Techno", kRumble, 15, 25, 35);
        addKit("Lo-Fi House", kLoFi, 40, 20, 25);
        addKit("Acid Partner Kit", kAcidPartner, 15, 35, 30);
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        using namespace fmkit;
        sr_ = (float)sampleRate;
        for (int d = 0; d < kNumDrums; ++d) voices_[d].prepare((Drum)d, sr_, 0x9E3779B9u * (uint32_t)(d + 1));
        varRng_.reseed(0x0C4B7A11u);
        master_.setTime(10.0f, sampleRate);
        master_.snap(dbToGain(get(kMaster)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.kill(); v.tail = 0.0f; v.lastOut = 0.0f; }
        varRng_.reseed(0x0C4B7A11u);
        master_.snap(dbToGain(get(fmkit::kMaster)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        routing_ = getInt(fmkit::kRouting);
        master_.setTarget(dbToGain(get(fmkit::kMaster)));

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)   // sample-accurate triggering
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);

        // Master then the safety limiter, on every output.
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float g = master_.process();
            for (int c = 0; c < ctx.numOutputs; ++c) ctx.outputs[c][i] = fmkit::limit(ctx.outputs[c][i] * g);
        }
    }

    // Which stereo pair (0 = Main, 1 = Aux) a drum goes to.
    int busFor(int drum) const
    {
        using namespace fmkit;
        switch (routing_)
        {
            case 1: return drum == kKick ? 1 : 0;
            case 2: return (drum == kClosedHat || drum == kOpenHat) ? 1 : 0;
            case 3: return drum >= kClosedHat ? 1 : 0;
            default: return 0;
        }
    }

    static int drumForNote(int note)
    {
        const int n = fmkit::kNumDrums;
        return (((note - fmkit::kFirstNote) % n) + n) % n;
    }

private:
    // Preset data: per drum {Tune, Decay, Tone, Level, Morph}.
    // Order: Kick, Snare, Clap, CHat, OHat, LTom, HTom, Perc.
    using Kit = float[fmkit::kNumDrums][fmkit::kKnobsPerDrum];
    static constexpr Kit kInit = {
        {0, 50, 30, 80, 50}, {0, 45, 35, 80, 50}, {0, 45, 50, 80, 50}, {0, 30, 60, 70, 50},
        {0, 50, 60, 65, 50}, {0, 50, 25, 75, 50}, {0, 50, 25, 75, 50}, {0, 30, 45, 70, 50}};
    static constexpr Kit kDeepHouse = {
        {-2, 55, 20, 85, 35}, {-2, 40, 20, 70, 40}, {-1, 60, 35, 75, 60}, {-3, 25, 40, 60, 30},
        {-3, 45, 40, 55, 30}, {-2, 55, 15, 70, 40}, {-2, 55, 15, 70, 40}, {-5, 25, 25, 65, 45}};
    static constexpr Kit kWarehouse = {
        {-4, 75, 35, 90, 70}, {0, 35, 50, 70, 70}, {0, 40, 60, 75, 55}, {2, 20, 80, 70, 75},
        {0, 60, 85, 65, 80}, {-3, 35, 30, 70, 60}, {-1, 30, 30, 70, 60}, {5, 10, 60, 65, 20}};
    static constexpr Kit kMinimal = {
        {0, 20, 60, 85, 55}, {3, 15, 40, 70, 20}, {2, 15, 40, 70, 0}, {5, 8, 50, 65, 20},
        {5, 25, 50, 55, 25}, {2, 20, 40, 65, 85}, {4, 15, 40, 65, 85}, {12, 5, 50, 70, 0}};
    static constexpr Kit kIndustrial = {
        {-5, 60, 75, 90, 95}, {-3, 50, 90, 75, 100}, {-2, 50, 85, 75, 80}, {0, 30, 100, 70, 100},
        {-2, 70, 100, 65, 100}, {-6, 50, 70, 70, 100}, {-4, 50, 70, 70, 100}, {0, 45, 90, 70, 75}};
    static constexpr Kit kRumble = {
        {-6, 90, 25, 90, 30}, {0, 40, 40, 65, 50}, {0, 50, 50, 70, 50}, {4, 15, 60, 65, 60},
        {3, 35, 60, 55, 60}, {-14, 85, 30, 70, 10}, {-5, 50, 30, 65, 45}, {0, 25, 45, 60, 25}};
    static constexpr Kit kLoFi = {
        {-3, 50, 15, 85, 25}, {-4, 55, 25, 80, 35}, {-3, 70, 20, 75, 70}, {-7, 30, 20, 60, 15},
        {-7, 50, 20, 55, 15}, {-5, 55, 15, 70, 30}, {-5, 55, 15, 70, 30}, {-7, 30, 20, 60, 50}};
    static constexpr Kit kAcidPartner = {
        {0, 35, 40, 85, 60}, {2, 30, 45, 70, 55}, {1, 35, 55, 80, 45}, {3, 20, 70, 70, 55},
        {2, 65, 70, 65, 60}, {0, 40, 30, 70, 50}, {2, 35, 30, 70, 50}, {7, 40, 60, 65, 90}};

    void addKit(const char* name, const Kit& kit, float variation, float velMorph, float drive)
    {
        using namespace fmkit;
        std::vector<float> v((size_t)kNumParams);
        v[kMaster] = -3.0f; v[kRouting] = 0.0f;
        v[kVariation] = variation; v[kVelMorph] = velMorph; v[kDrive] = drive;
        for (int d = 0; d < kNumDrums; ++d)
            for (int k = 0; k < kKnobsPerDrum; ++k) v[(size_t)paramIndex(d, k)] = kit[d][k];
        addProgram(name, v);
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "OCKM FM Kit";
        info.vendor       = "OCKM";                    // == plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC('O', '0', 'O', 'H');   // == plugin.json "uid" (permanent)
        info.version      = 1201;
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 4;                         // Main 1-2, Aux 3-4
        info.tailSamples  = 0;
        return info;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        using namespace fmkit;
        if (ev.isNoteOn())
        {
            const int d = drumForNote(ev.note());
            if (d == kClosedHat) voices_[kOpenHat].kill();     // choke
            const float vel = (float)ev.velocity() / 127.0f;
            float tune = get(paramIndex(d, kTune)), decay = get(paramIndex(d, kDecay)) * 0.01f;
            float tone = get(paramIndex(d, kTone)) * 0.01f, level = get(paramIndex(d, kLevel)) * 0.01f;
            float morph = get(paramIndex(d, kChar)) * 0.01f;
            // Vel>Morph: velocity 95 leaves Morph alone; harder hits push it up, softer pull it down,
            // by up to 40% at full amount (127 -> +40%, 64 and below -> -40%).
            morph += get(kVelMorph) * 0.01f * clamp((vel - 0.75f) * 1.6f, -0.4f, 0.4f);
            // Variation: every hit a little different (at 100%: +-35 cents, Tone +-25%, Decay +-12%,
            // Level +-1.3 dB, Morph +-12%). At 0% no random numbers are drawn at all.
            const float var = get(kVariation) * 0.01f;
            if (var > 0.0f)
            {
                tune  += var * 0.35f * varRng_.nextBipolar();
                tone  *= 1.0f + var * 0.25f * varRng_.nextBipolar();
                decay += var * 0.12f * varRng_.nextBipolar();
                level *= 1.0f + var * 0.15f * varRng_.nextBipolar();
                morph += var * 0.12f * varRng_.nextBipolar();
            }
            voices_[d].trigger(tune, clamp(decay, 0.0f, 1.0f), clamp(tone, 0.0f, 1.2f), level,
                               clamp(morph, 0.0f, 1.0f), vel, get(kDrive) * 0.01f * kDriveAmt[d]);
        }
        else if (ev.isAllNotesOff() || (ev.isController() && ev.controller() == 120))
        {
            for (auto& v : voices_) v.kill();
        }
    }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        const bool haveAux = ctx.numOutputs >= 4;
        for (int d = 0; d < fmkit::kNumDrums; ++d)
        {
            fmkit::Voice& v = voices_[d];
            if (!v.busy()) continue;
            const int bus = haveAux ? busFor(d) : 0;   // fall back to Main with only 2 outputs
            float* L = ctx.outputs[bus * 2];
            float* R = ctx.outputs[bus * 2 + 1];
            for (int i = from; i < to; ++i)
            {
                const float y = v.process();
                L[i] += y * v.gainL;
                R[i] += y * v.gainR;
            }
        }
    }

    float sr_ = 44100.0f;
    int routing_ = 0;
    fmkit::Voice voices_[fmkit::kNumDrums];
    Xorshift32 varRng_;   // Variation's random numbers (separate from the drums' own noise)
    Smoother master_;
};

MPC_PLUGIN_ENTRY(OCKMFMKit)
