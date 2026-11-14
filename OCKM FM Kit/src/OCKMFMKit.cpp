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
// loop: Tune, Decay, Tone, Level and a fifth "character" knob (Punch, Snap, Spread, Metal, Bend, Ratio).

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
constexpr int kNumParams = kFirstDrumParam + kNumDrums * kKnobsPerDrum;   // 42
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

// Per-drum Decay ranges (ms to -60 dB) and pans.
static const float kDecayLo[kNumDrums] = {80, 60, 80, 15, 150, 100, 80, 20};
static const float kDecayHi[kNumDrums] = {1500, 500, 900, 300, 2000, 1200, 1000, 800};
static const float kPan[kNumDrums]     = {0.0f, 0.0f, 0.15f, -0.25f, 0.25f, -0.35f, 0.35f, 0.4f};

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
    float noiseAmt = 0.0f, dev = 0.0f;
    float ph1 = 0, ph2 = 0, ph3 = 0;
    float hpA = 0.0f, lp1 = 0.0f, lp2 = 0.0f;   // one-pole high-pass states
    int burstsLeft = 0, burstTimer = 0, burstGap = 0;
    float tail = 0.0f, tailK = 0.0f;   // declick: the previous sound fading out over ~1.5 ms
    float lastOut = 0.0f;
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

    void trigger(float tune, float decay, float tone, float level, float ch, float vel)
    {
        tail = active ? lastOut : tail;    // retrigger without a click
        active = true;
        const float r = semitonesToRatio(tune);
        const float bright = 0.8f + 0.2f * vel;   // harder hits are a little brighter
        tone *= bright;
        gain = level * (0.32f + 0.68f * vel);
        amp = 1.0f; pEnv = 1.0f; mEnv = 1.0f; nEnv = 0.0f;
        ph1 = ph2 = ph3 = 0.0f; lp1 = lp2 = 0.0f;
        ampK = decayCoef(expMap(decay, kDecayLo[type], kDecayHi[type]), sr);
        sweep = 0.0f; ratio2 = 1.0f; idxBase = idxEnv = idx3 = 0.0f; noiseAmt = 0.0f;
        switch (type)
        {
            case kKick:      // carrier ~48 Hz, pitch drop (Punch), modulator at ratio 1 for the knock (Tone)
                inc = 48.0f * r / sr;
                sweep = 7.0f * ch;
                pK = decayCoef(lerp(120.0f, 220.0f, ch), sr);
                ratio2 = 1.0f; idxBase = 0.25f * tone; idxEnv = 3.0f * tone;
                mK = decayCoef(60.0f, sr);
                break;
            case kSnare:     // carrier ~185 Hz, modulator at 1.6 (shell ring), high-passed noise (Snap)
                inc = 185.0f * r / sr;
                sweep = 0.5f; pK = decayCoef(90.0f, sr);
                ratio2 = 1.6f; idxBase = 0.5f * tone; idxEnv = 2.5f * tone;
                mK = decayCoef(150.0f, sr);
                noiseAmt = ch; nEnv = 1.0f;
                nK = decayCoef(expMap(decay, 80.0f, 500.0f) * (0.5f + 0.8f * ch), sr);
                hpA = 1.0f - std::exp(-6.2831853f * 1500.0f / sr);
                break;
            case kClap:      // carrier ~1.2 kHz whose frequency is wobbled by noise = a band of noise
                inc = 1200.0f * r / sr;
                dev = inc * (0.6f + 3.4f * tone);           // Tone: band width
                burstGap = (int)(0.001f * (6.0f + 18.0f * ch) * sr);   // Spread: gap between claps
                burstTimer = burstGap; burstsLeft = 2;
                mK = decayCoef(lerp(25.0f, 45.0f, ch), sr);   // each clap burst
                nK = ampK;                                     // the tail uses Decay
                amp = 0.0f;
                break;
            case kClosedHat:
            case kOpenHat:   // three operators in a chain (3 -> 2 -> 1) at odd ratios, then high-pass
                inc = 2400.0f * r / sr;
                ratio2 = 1.41f + 2.3f * ch;                  // Metal: glassy to clangy
                ratio3 = ratio2 * 1.73f;
                idxBase = 1.0f + 4.0f * tone; idx3 = 3.0f * tone;
                hpA = 1.0f - std::exp(-6.2831853f * 5000.0f / sr);
                break;
            case kLowTom:
            case kHighTom:   // like the kick, higher, with a gentler pitch bend
                inc = (type == kLowTom ? 95.0f : 160.0f) * r / sr;
                sweep = 1.5f * ch; pK = decayCoef(240.0f, sr);
                ratio2 = 1.0f; idxBase = 0.2f * tone; idxEnv = 2.0f * tone;
                mK = decayCoef(80.0f, sr);
                break;
            case kPerc:
            default:         // carrier ~800 Hz, inharmonic modulator: rim / block -> bell -> clang
                inc = 800.0f * r / sr;
                ratio2 = 1.41f + 5.0f * ch;
                idxBase = 0.3f * tone; idxEnv = 3.0f * tone;
                mK = decayCoef(lerp(40.0f, 200.0f, decay), sr);
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
                    const float m = sine(tab, ph2) * (idxBase + idxEnv * mEnv);
                    y = sine(tab, ph1 + m) * amp;
                    ph1 += i1; ph2 += i1 * ratio2;
                    if (ph1 >= 1.0f) ph1 -= 1.0f;
                    if (ph2 >= 1.0f) ph2 -= std::floor(ph2);
                    if (type == kSnare)
                    {
                        const float n = rng.nextBipolar();
                        lp1 += hpA * (n - lp1);
                        y = y * 0.85f + (n - lp1) * noiseAmt * nEnv * 0.9f;
                        nEnv *= nK;
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
                    y = s * (mEnv + 0.8f * nEnv) * 0.55f;
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
                    const float m3 = sine(tab, ph3) * idx3;
                    const float m2 = sine(tab, ph2 + m3) * idxBase;
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

        static const char* const ids[kNumDrums]   = {"kick", "snare", "clap", "chat", "ohat", "ltom", "htom", "perc"};
        static const char* const names[kNumDrums] = {"Kick", "Snare", "Clap", "CHat", "OHat", "LTom", "HTom", "Perc"};
        static const char* const charId[kNumDrums]   = {"punch", "snap", "spread", "metal", "metal", "bend", "bend", "ratio"};
        static const char* const charName[kNumDrums] = {"Punch", "Snap", "Spread", "Metal", "Metal", "Bend", "Bend", "Ratio"};
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

        // Kits for house and techno (master -3 dB, everything on Main).
        addKit("Init Kit", kInit);
        addKit("Deep House Kit", kDeepHouse);
        addKit("Warehouse Techno", kWarehouse);
        addKit("Minimal Clicks", kMinimal);
        addKit("Industrial Clang", kIndustrial);
        addKit("Rumble Techno", kRumble);
        addKit("Lo-Fi House", kLoFi);
        addKit("Acid Partner Kit", kAcidPartner);
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        using namespace fmkit;
        sr_ = (float)sampleRate;
        for (int d = 0; d < kNumDrums; ++d) voices_[d].prepare((Drum)d, sr_, 0x9E3779B9u * (uint32_t)(d + 1));
        master_.setTime(10.0f, sampleRate);
        master_.snap(dbToGain(get(kMaster)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.kill(); v.tail = 0.0f; v.lastOut = 0.0f; }
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
    // Preset data: per drum {Tune, Decay, Tone, Level, 5th knob}.
    // Order: Kick, Snare, Clap, CHat, OHat, LTom, HTom, Perc.
    using Kit = float[fmkit::kNumDrums][fmkit::kKnobsPerDrum];
    static constexpr Kit kInit = {
        {0, 50, 30, 80, 50}, {0, 45, 35, 80, 55}, {0, 45, 50, 80, 40}, {0, 30, 60, 70, 40},
        {0, 50, 60, 65, 40}, {0, 50, 25, 75, 40}, {0, 50, 25, 75, 40}, {0, 30, 45, 70, 30}};
    static constexpr Kit kDeepHouse = {
        {-2, 55, 20, 85, 40}, {-2, 40, 20, 70, 35}, {-1, 60, 35, 75, 70}, {-3, 25, 40, 60, 25},
        {-3, 45, 40, 55, 25}, {-2, 55, 15, 70, 30}, {-2, 55, 15, 70, 30}, {-5, 25, 25, 65, 10}};
    static constexpr Kit kWarehouse = {
        {-4, 75, 35, 90, 70}, {0, 35, 50, 70, 60}, {0, 40, 60, 75, 30}, {2, 20, 80, 70, 70},
        {0, 60, 85, 65, 75}, {-3, 35, 30, 70, 50}, {-1, 30, 30, 70, 50}, {5, 10, 60, 65, 45}};
    static constexpr Kit kMinimal = {
        {0, 20, 60, 85, 60}, {3, 15, 40, 70, 70}, {2, 15, 40, 70, 15}, {5, 8, 50, 65, 50},
        {5, 25, 50, 55, 50}, {2, 20, 40, 65, 30}, {4, 15, 40, 65, 30}, {12, 5, 50, 70, 20}};
    static constexpr Kit kIndustrial = {
        {-5, 60, 75, 90, 80}, {-3, 50, 90, 75, 80}, {-2, 50, 85, 75, 55}, {0, 30, 100, 70, 95},
        {-2, 70, 100, 65, 100}, {-6, 50, 70, 70, 70}, {-4, 50, 70, 70, 70}, {0, 45, 90, 70, 85}};
    static constexpr Kit kRumble = {
        {-6, 90, 25, 90, 55}, {0, 40, 40, 65, 50}, {0, 50, 50, 70, 40}, {4, 15, 60, 65, 55},
        {3, 35, 60, 55, 55}, {-14, 85, 30, 70, 15}, {-5, 50, 30, 65, 40}, {0, 25, 45, 60, 30}};
    static constexpr Kit kLoFi = {
        {-3, 50, 15, 85, 35}, {-4, 55, 25, 80, 45}, {-3, 70, 20, 75, 85}, {-7, 30, 20, 60, 20},
        {-7, 50, 20, 55, 20}, {-5, 55, 15, 70, 30}, {-5, 55, 15, 70, 30}, {-7, 30, 20, 60, 15}};
    static constexpr Kit kAcidPartner = {
        {0, 35, 40, 85, 65}, {2, 30, 45, 70, 60}, {1, 35, 55, 80, 25}, {3, 20, 70, 70, 45},
        {2, 65, 70, 65, 45}, {0, 40, 30, 70, 40}, {2, 35, 30, 70, 40}, {7, 40, 60, 65, 60}};

    void addKit(const char* name, const Kit& kit)
    {
        using namespace fmkit;
        std::vector<float> v((size_t)kNumParams);
        v[kMaster] = -3.0f; v[kRouting] = 0.0f;
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
        info.version      = 1000;
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
            voices_[d].trigger(get(paramIndex(d, kTune)), get(paramIndex(d, kDecay)) * 0.01f,
                               get(paramIndex(d, kTone)) * 0.01f, get(paramIndex(d, kLevel)) * 0.01f,
                               get(paramIndex(d, kChar)) * 0.01f, (float)ev.velocity() / 127.0f);
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
    Smoother master_;
};

MPC_PLUGIN_ENTRY(OCKMFMKit)
