// OCKMDRIFT.cpp - OCKM DRIFT, a generative lead for the MPC.
//
// SPDX-License-Identifier: MIT
// Copyright (c) OCKM
//
// One job: it plays its own 16-step line on a fat retro analogue mono lead, and the line
// slowly changes by itself while staying in key. See ../SPEC.md for the design and reasons.
//
// Signal flow:
//
//   MPC transport (tempo, song position) -> Clock: which bar, which step (with Swing)
//   Pattern (16 steps: a note as a scale step + four "dice" values)
//        -- every Evolve bars, Mutate re-rolls some steps (seeded hash, repeatable)
//        -- knobs applied live: Hits/Rotate (Euclidean rhythm), Density, Slide, Scale,
//           Root, Range, Octave, plus the key shift from MPC notes (Keys / Play)
//   -> LeadVoice (one voice):
//        Osc 1 + Osc 2 (detuned saw..square) + Osc 3 (one octave down, "Sub"), each with
//        a slow pitch drift
//     -> 24 dB/oct zero-delay-feedback ladder low-pass, saturated in its loop
//        cutoff = Cutoff + filter env (+ accent) + key tracking + Cut Drift
//     -> Drive (soft clip) -> Amp (2 ms attack, gate, release) * accent
//   -> 20 Hz DC cut -> Volume -> safety ceiling (transparent below -3 dBFS) -> L = R
//
// Realtime rules: no allocation, locks or I/O in process(). exp2/tan/pow only once every
// 8 samples (the "tick"), interpolated in between. The generator does a few hundred integer
// operations per bar, never more.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/onepole.h"
#include "dsp/smoother.h"
#include "Generator.h"
#include "LeadVoice.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace mpc::dsp;
using namespace ockm_drift;

namespace {

// ---------------------------------------------------------------------------
// Parameters. Order and ids are PERMANENT once anyone has a project: append only.
// ---------------------------------------------------------------------------
enum ParamIndex
{
    kVolume = 0,   // master volume, dB (param 0 is nudged by the host on load: harmless)
    kCutoff,       // filter cutoff, 0..100 % (shown in Hz)
    kReso,         // resonance, 0..100 %
    kEnvAmt,       // filter envelope depth, 0..100 % (0..5 octaves)
    kDecay,        // filter envelope decay, 0..100 % (shown in ms)
    kWave,         // saw (0) .. square (100)
    kDetune,       // osc 1/2 spread, 0..100 % (0..+-12 cents)
    kSub,          // osc 3 (one octave down) level
    kGlide,        // slide time, 0..100 % (shown in ms)
    kLength,       // note length, 10..100 % of a step
    kRelease,      // amp release, 0..100 % (shown in ms)
    kAccent,       // how much accented steps stand out
    kDrive,        // drive, 0..100 %
    kKeys,         // Off / In Key / Transpose: what MPC notes do
    kPlay,         // Always / Held
    kRunStop,      // run while the MPC is stopped
    kMutate,       // chance each step changes at each Evolve point
    kEvolve,       // 1 / 2 / 4 / 8 bars
    kSeed,         // 1..999
    kLock,         // freeze the pattern
    kScale,        // scale choice
    kRoot,         // C..B
    kRange,        // 1..3 octaves
    kOctave,       // -2..+2
    kHits,         // Euclidean hits 1..16
    kRotate,       // 0..15
    kDensity,      // 0..100 % (50 = just the hits)
    kRate,         // 1/16, 1/8, 1/4
    kSlide,        // chance a note slides into the next
    kCutDrift,     // slow cutoff wander depth
    kSwing,        // 50..75 %
    kNumParams
};

enum Keys { kKeysOff = 0, kKeysInKey, kKeysTranspose };
enum Play { kPlayAlways = 0, kPlayHeld };

constexpr float kDriftOct    = 2.0f;     // Cut Drift 100 %: +-2 octaves
constexpr float kLateMs      = 25.0f;    // a note this late still re-pitches the step

// Test hook: the harness logs every note the line plays. Null on the MPC (one compare per note).
struct NoteInfo { int step; int note; bool accent; bool legato; bool late; int offset; };
void (*gNoteHook)(void* user, const NoteInfo& info) = nullptr;
void* gNoteHookUser = nullptr;

} // namespace

class OCKMDRIFT : public mpc::Plugin
{
public:
    OCKMDRIFT() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        auto setting = [](Param p) { p.automatable = false; return p; };
        addParam(floatParam("volume",  "Volume",     -60.0f, 6.0f, -6.0f, "dB", 1));
        addParam(floatParam("cutoff",  "Cutoff",       0.0f, 100.0f, 40.0f, "Hz", 0));
        addParam(floatParam("reso",    "Resonance",    0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam("envamt",  "Env Amount",   0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("decay",   "Decay",        0.0f, 100.0f, 35.0f, "ms", 0));
        addParam(floatParam("wave",    "Wave",         0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("detune",  "Detune",       0.0f, 100.0f, 35.0f, "ct", 0));
        addParam(floatParam("sub",     "Sub",          0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("glide",   "Glide",        0.0f, 100.0f, 30.0f, "ms", 0));
        addParam(floatParam("length",  "Length",      10.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("release", "Release",      0.0f, 100.0f, 15.0f, "ms", 0));
        addParam(floatParam("accent",  "Accent",       0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("drive",   "Drive",        0.0f, 100.0f, 20.0f, "%", 0));
        addParam(setting(choiceParam("keys", "Keys", {"Off", "In Key", "Transpose"}, 1)));
        addParam(setting(choiceParam("play", "Play", {"Always", "Held"}, 0)));
        addParam(setting(boolParam("runstop", "Run Stopped", false)));
        addParam(floatParam("mutate",  "Mutate",       0.0f, 100.0f, 25.0f, "%", 0));
        addParam(choiceParam("evolve", "Evolve", {"1 Bar", "2 Bars", "4 Bars", "8 Bars"}, 1));
        addParam(setting(intParam("seed", "Seed", 1, 999, 1)));
        addParam(setting(boolParam("lock", "Lock", false)));
        addParam(choiceParam("scale", "Scale", {"Minor", "Dorian", "Phrygian", "Major", "Mixolydian",
                                                 "Harm Minor", "Minor Pent", "Major Pent"}, 0));
        addParam(choiceParam("root", "Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 9));
        addParam(intParam("range",  "Range",  1, 3, 1, "oct"));
        addParam(intParam("octave", "Octave", -2, 2, 0, "oct"));
        addParam(intParam("hits",   "Hits",   1, 16, 7));
        addParam(intParam("rotate", "Rotate", 0, 15, 0));
        addParam(floatParam("density", "Density",      0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("rate", "Rate", {"1/16", "1/8", "1/4"}, 0));
        addParam(floatParam("slide",   "Slide",        0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("cutdrift", "Cut Drift",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("swing",   "Swing",       50.0f, 75.0f, 50.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("OCKM DRIFT: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Root index: C=0 .. B=11. Scale index as in the Scale choice list.
        program("Init Drift",     {});
        program("Slow Burn",      {{kScale, 1}, {kRoot, 2}, {kHits, 5}, {kMutate, 10}, {kEvolve, 3},
                                   {kCutDrift, 45}, {kDecay, 55}, {kCutoff, 35}, {kSeed, 7}});
        program("Rolling Techno", {{kScale, 2}, {kRoot, 5}, {kHits, 11}, {kDensity, 60}, {kOctave, -1},
                                   {kMutate, 30}, {kEvolve, 0}, {kDrive, 45}, {kReso, 50}, {kCutoff, 30},
                                   {kDecay, 25}, {kSlide, 10}, {kSeed, 23}});
        program("Peak Arp",       {{kScale, 6}, {kRoot, 9}, {kHits, 16}, {kRange, 2}, {kMutate, 40},
                                   {kEvolve, 0}, {kDecay, 15}, {kLength, 40}, {kSlide, 5}, {kGlide, 15},
                                   {kCutoff, 45}, {kSeed, 101}});
        program("Sparse Dub",     {{kScale, 1}, {kRoot, 7}, {kHits, 3}, {kRotate, 2}, {kRate, 1},
                                   {kRelease, 55}, {kCutDrift, 60}, {kMutate, 20}, {kEvolve, 2},
                                   {kLength, 70}, {kCutoff, 30}, {kSeed, 64}});
        program("Singing Lead",   {{kScale, 4}, {kRoot, 0}, {kRate, 1}, {kHits, 6}, {kSlide, 60},
                                   {kGlide, 50}, {kRange, 2}, {kLength, 90}, {kReso, 45}, {kDecay, 60},
                                   {kWave, 50}, {kSeed, 12}});
        program("Deep Walker",    {{kScale, 0}, {kRoot, 4}, {kOctave, -2}, {kHits, 6}, {kSub, 60},
                                   {kCutoff, 25}, {kMutate, 15}, {kWave, 60}, {kDecay, 30}, {kSeed, 5}});
        program("Chaos Garden",   {{kScale, 5}, {kRoot, 11}, {kHits, 9}, {kDensity, 75}, {kRange, 3},
                                   {kMutate, 70}, {kEvolve, 0}, {kCutDrift, 50}, {kSeed, 333}});
        program("Frozen Hook",    {{kScale, 0}, {kRoot, 0}, {kHits, 7}, {kRotate, 3}, {kLock, 1},
                                   {kSlide, 30}, {kMutate, 0}, {kSeed, 42}});

        pattern_ = firstPattern(1);
        patternSeed_ = 1;
        publish();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        voice_.prepare(sampleRate);
        dc_.setCutoff(20.0f, sampleRate);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        voice_.reset();
        dc_.reset();
        volume_.snap(dbToGain(get(kVolume)));
        running_ = false; curStep_ = -1; tie_ = false; gateLeft_ = -1;
        heldCount_ = 0;
        if (staged_.exchange(false, std::memory_order_acq_rel))       // a project's locked pattern
        {
            pattern_ = stagedPattern_;
            patternSeed_ = stagedSeed_;
            publish();
        }
    }

    // ----- state: the pattern, so a locked line comes back with the project -----
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        Pattern p; uint32_t seed = 1;
        if (staged_.load(std::memory_order_acquire)) { p = stagedPattern_; seed = stagedSeed_; }   // loaded, not yet swapped in
        else readPublished(p, seed);
        const uint32_t want = (uint32_t)clamp(getInt(kSeed), 1, 999);
        if (seed != want) { p = firstPattern(want); seed = want; }   // a Seed change not yet applied: save what will play
        out.push_back(1);                                  // version
        out.push_back((uint8_t)(seed & 0xFF)); out.push_back((uint8_t)(seed >> 8));
        for (int i = 0; i < kSteps; ++i)
        {
            const Step& s = p.s[i];
            out.push_back((uint8_t)s.deg);
            for (uint16_t v : {s.play, s.acc, s.slide, s.tone}) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); }
        }
    }

    void loadExtraState(const uint8_t* d, size_t n) override
    {
        if (n < 3 + kSteps * 9 || d[0] != 1) return;       // unknown: keep what we have
        Pattern p;
        size_t k = 3;
        for (int i = 0; i < kSteps; ++i)
        {
            Step& s = p.s[i];
            s.deg = (int8_t)clamp((int)(int8_t)d[k++], -kDegLimit, kDegLimit);
            uint16_t* f[4] = {&s.play, &s.acc, &s.slide, &s.tone};
            for (uint16_t* v : f) { *v = (uint16_t)(d[k] | (d[k + 1] << 8)); k += 2; }
        }
        stagedPattern_ = p;
        stagedSeed_ = (uint32_t)(d[1] | (d[2] << 8));
        staged_.store(true, std::memory_order_release);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        // ----- block settings -----
        readSettings();
        const mpc::Transport& tr = ctx.transport;
        const double bpm = (tr.tempoValid && tr.bpm > 1.0) ? tr.bpm : 120.0;
        const double barBeats = barLength(tr);
        bps_ = bpm / 60.0 / sr_;
        stepBeats_ = kRateBeats[rate_];
        loopBars_ = clamp((int)std::ceil(kSteps * stepBeats_ / barBeats - 1.0e-6), 1, 8);
        stepSamples_ = stepBeats_ / bps_;
        barSamples_ = barBeats / bps_;
        lateSamples_ = (int)(kLateMs * 0.001 * sr_);

        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const bool running = playing || runStop_;

        // Position at the block start: song bar number and beats into the bar.
        long long bar0; double inBar0;
        if (playing)
        {
            double barStart = tr.barStartPpq;
            double inBar = tr.ppqPos - barStart;
            if (!(inBar >= -1.0e-9 && inBar < barBeats + 1.0e-6))
            {
                barStart = std::floor(tr.ppqPos / barBeats) * barBeats;
                inBar = tr.ppqPos - barStart;
            }
            if (inBar < 0.0) inBar = 0.0;
            bar0 = std::llround(barStart / barBeats);
            inBar0 = inBar;
        }
        else
        {
            if (!running_ || wasPlaying_) freePos_ = 0.0;   // free run starts on a bar line
            bar0 = (long long)std::floor(freePos_ / barBeats);
            inBar0 = freePos_ - (double)bar0 * barBeats;
        }

        if (!running && running_) stopLine();
        if (running && (!running_ || playing != wasPlaying_)) startLine(bar0, inBar0, barBeats);
        running_ = running;
        wasPlaying_ = playing;
        if (!running && patternSeed_ != (uint32_t)seed_) newSeedPattern();   // stopped: a new Seed applies at once

        // Cut Drift: a smooth wander following the bar count (once per block; the voice smooths it).
        knobs_.driftOct = cutDrift_ * kDriftOct * driftValue(barsPlayed_ + inBar0 / barBeats);
        voice_.setKnobs(knobs_);
        volume_.setTarget(dbToGain(get(kVolume)));

        // ----- samples, with MIDI at its sample offset -----
        int ev = 0;
        long long bar = bar0;
        double inBar = inBar0;
        bool silentIn = true;
        float dcPeak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i) handleMidi(ctx.midi[ev++], i);

            if (running)
            {
                if (bar != lastBar_) onBar(bar);
                ++sinceBar_;
                const int step = locate((double)(((bar % loopBars_) + loopBars_) % loopBars_) * barBeats + inBar);
                if (step != curStep_)
                {
                    curStep_ = step;
                    sinceStep_ = 0;
                    if (step >= 0) playStep(step, i, false);
                }
                else ++sinceStep_;
                if (gateLeft_ > 0 && --gateLeft_ == 0) { voice_.release(); gateLeft_ = -1; }
                inBar += beatsPerSample();
                if (inBar >= barBeats) { inBar -= barBeats; ++bar; }
            }

            const float v = voice_.next();
            if (v != 0.0f) silentIn = false;
            const float dry = dc_.process(v);
            dcPeak = std::fmax(dcPeak, std::fabs(dry));
            const float y = ceiling(dry * volume_.process());
            L[i] = y;
            if (R) R[i] = y;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++], n);
        if (running && !playing) freePos_ = (double)bar * barBeats + inBar;
        if (silentIn && dcPeak < 1.0e-7f) dc_.reset();     // no denormal tail
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char buf[32];
        const float nrm = plain * 0.01f;
        switch (index)
        {
            case kCutoff:  std::snprintf(buf, sizeof(buf), "%.0f", (double)cutoffHz(nrm)); break;
            case kDecay:   std::snprintf(buf, sizeof(buf), "%.0f", (double)decayMs(nrm)); break;
            case kGlide:   std::snprintf(buf, sizeof(buf), "%.0f", (double)glideMs(nrm)); break;
            case kRelease: std::snprintf(buf, sizeof(buf), "%.0f", (double)releaseMs(nrm)); break;
            case kDetune:  std::snprintf(buf, sizeof(buf), "%.1f", (double)detuneCents(nrm)); break;
            case kOctave:  std::snprintf(buf, sizeof(buf), "%+d", (int)std::lround(plain)); break;
            case kCutDrift: if (plain <= 0.0f) { out = "Off"; return true; } return false;
            case kDensity: if (std::fabs(plain - 50.0f) < 0.5f) { out = "Hits"; return true; } return false;
            default: return false;
        }
        out = buf;
        return true;
    }

    // ----- for the test harness -----
    const Pattern& pattern() const { return pattern_; }
    uint32_t cycle() const { return cycle_; }
    // The MIDI note a step plays right now (or -1 if it rests), with the current knobs and key.
    int stepNote(int step) { readSettings(); return stepPlays(step) ? pitchOf(step) : -1; }

private:
    // ----- settings read once per block -----
    void readSettings()
    {
        knobs_.cutoff = getNormalised(kCutoff); knobs_.reso = getNormalised(kReso);
        knobs_.envAmt = getNormalised(kEnvAmt); knobs_.decay = getNormalised(kDecay);
        knobs_.wave = getNormalised(kWave);     knobs_.detune = getNormalised(kDetune);
        knobs_.sub = getNormalised(kSub);       knobs_.glide = getNormalised(kGlide);
        knobs_.release = getNormalised(kRelease); knobs_.accent = getNormalised(kAccent);
        knobs_.drive = getNormalised(kDrive);
        length_ = get(kLength) * 0.01f;
        keys_ = clamp(getInt(kKeys), 0, 2);
        play_ = clamp(getInt(kPlay), 0, 1);
        runStop_ = getBool(kRunStop);
        mutate_ = get(kMutate) * 0.01f;
        evolve_ = kEvolveBars[clamp(getInt(kEvolve), 0, 3)];
        seed_ = clamp(getInt(kSeed), 1, 999);
        lock_ = getBool(kLock);
        scale_ = clamp(getInt(kScale), 0, kNumScales - 1);
        root_ = clamp(getInt(kRoot), 0, 11);
        range_ = clamp(getInt(kRange), 1, 3);
        octave_ = clamp(getInt(kOctave), -2, 2);
        hits_ = clamp(getInt(kHits), 1, 16);
        rotate_ = clamp(getInt(kRotate), 0, 15);
        density_ = get(kDensity) * 0.01f;
        rate_ = clamp(getInt(kRate), 0, 2);
        slide_ = get(kSlide) * 0.01f;
        cutDrift_ = get(kCutDrift) * 0.01f;
        swing_ = get(kSwing) * 0.01f;
    }

    double beatsPerSample() const { return bps_; }

    static double barLength(const mpc::Transport& tr)
    {
        if (tr.timeSigNumerator > 0 && tr.timeSigDenominator > 0)
            return 4.0 * tr.timeSigNumerator / tr.timeSigDenominator;
        return 4.0;
    }

    // Which step (0..15) a position in the pattern loop (in beats) falls in; -1 past step 16
    // (only in odd time signatures). Swing delays every second step.
    int locate(double pos) const
    {
        const double pair = 2.0 * stepBeats_;
        const double pairIdx = std::floor(pos / pair + 1.0e-9);
        const double inPair = pos - pairIdx * pair;
        const int step = 2 * (int)pairIdx + (inPair >= swing_ * pair - 1.0e-9 ? 1 : 0);
        return step < kSteps ? step : -1;
    }

    // ----- transport -----
    void startLine(long long bar, double inBar, double barBeats)
    {
        if (!lock_ || patternSeed_ != (uint32_t)seed_) { pattern_ = firstPattern((uint32_t)seed_); patternSeed_ = (uint32_t)seed_; publish(); }
        cycle_ = 0; barsSinceChange_ = 0; barsPlayed_ = 0;
        lastBar_ = bar;
        sinceBar_ = 0;
        tie_ = false; gateLeft_ = -1;
        // Starting inside a step: play it only if we're within the "late" window of its start.
        const double pos = (double)(((bar % loopBars_) + loopBars_) % loopBars_) * barBeats + inBar;
        const int step = locate(pos);
        curStep_ = step;
        sinceStep_ = lateSamples_;
        if (step >= 0)
        {
            const double start = stepStart(step);
            const double late = (pos - start) / bps_;
            if (late < (double)lateSamples_) { curStep_ = -1; }      // triggers on the first sample
        }
    }

    double stepStart(int step) const
    {
        const double pair = 2.0 * stepBeats_;
        return (double)(step / 2) * pair + ((step & 1) ? swing_ * pair : 0.0);
    }

    void stopLine()
    {
        voice_.release();
        curStep_ = -1; tie_ = false; gateLeft_ = -1;
    }

    // A new bar: count it, then apply a new Seed or an Evolve point.
    // A bar line that comes within a quarter bar of the last one is the MPC's loop jump landing
    // just after the bar line it already reported (a block straddled the loop point): it is
    // not counted again, but can still be the pattern start an Evolve point waits for.
    void onBar(long long bar)
    {
        lastBar_ = bar;
        if ((double)sinceBar_ >= 0.25 * barSamples_ || barsPlayed_ == 0)
        {
            ++barsSinceChange_;
            ++barsPlayed_;
        }
        sinceBar_ = 0;
        if (patternSeed_ != (uint32_t)seed_) { newSeedPattern(); return; }
        if (lock_) return;
        const int every = ((evolve_ + loopBars_ - 1) / loopBars_) * loopBars_;   // whole patterns
        if (barsSinceChange_ >= every && ((bar % loopBars_) + loopBars_) % loopBars_ == 0)
        {
            ++cycle_;
            mutatePattern(pattern_, patternSeed_, cycle_, mutate_);
            barsSinceChange_ = 0;
            publish();
        }
    }

    void newSeedPattern()
    {
        pattern_ = firstPattern((uint32_t)seed_);
        patternSeed_ = (uint32_t)seed_;
        cycle_ = 0; barsSinceChange_ = 0;
        publish();
    }

    // ----- the line -----
    bool stepPlays(int i) const
    {
        const Step& s = pattern_.s[i];
        const bool hit = euclidHit(i, hits_, rotate_);
        if (density_ <= 0.5f)
        {
            if (!hit) return false;
            const float t = density_ * 2.0f;
            const float d = (i == firstHit()) ? 0.0f : dice01(s.play);   // the first hit drops out last
            return t > 0.0f && d < t;
        }
        return hit || dice01(s.play) < (density_ - 0.5f) * 2.0f;
    }

    int firstHit() const
    {
        for (int i = 0; i < kSteps; ++i) if (euclidHit(i, hits_, rotate_)) return i;
        return 0;
    }

    // Scale step (folded into Range, beats snapped to chord tones, step 1 = root) -> MIDI note.
    int pitchOf(int i) const
    {
        const int len = kScaleLen[scale_];
        const int top = len * range_;
        int d = (i == 0) ? 0 : foldDeg(pattern_.s[i].deg, top);
        if (i != 0 && (i & 3) == 0 && dice01(pattern_.s[i].tone) < 0.75f) d = nearestChordTone(d, len, top);
        if (keys_ == kKeysInKey) d += keyShiftDeg_;
        const int oct = (d >= 0) ? d / len : -((-d + len - 1) / len);
        const int idx = d - oct * len;
        int note = 60 + root_ + 12 * octave_ + 12 * oct + kScaleNotes[scale_][idx];
        if (keys_ == kKeysTranspose) note += keyShiftSemi_;
        return clamp(note, 0, 127);
    }

    int nearestChordTone(int d, int len, int top) const
    {
        int best = d, bestDist = 1000;
        for (int o = -1; o <= top / len + 1; ++o)
            for (int c = 0; c < 3; ++c)
            {
                const int t = o * len + kChordTones[scale_][c];
                if (t < 0 || t > top) continue;
                const int dist = std::abs(t - d);
                if (dist < bestDist || (dist == bestDist && t < best)) { best = t; bestDist = dist; }
            }
        return best;
    }

    bool keysAllow() const { return play_ == kPlayAlways || heldCount_ > 0; }

    void playStep(int i, int offset, bool late)
    {
        const bool plays = keysAllow() && stepPlays(i);
        if (!plays)
        {
            if (tie_) { voice_.release(); tie_ = false; gateLeft_ = -1; }
            return;
        }
        const Step& s = pattern_.s[i];
        const int note = pitchOf(i);
        const bool accent = (((i & 3) == 0) && euclidHit(i, hits_, rotate_)) || dice01(s.acc) < (1.0f / 6.0f);
        const bool legato = tie_ && voice_.active();
        if (legato) voice_.glideTo(note, accent); else voice_.trigger(note, accent);
        // A slide holds this note into the next step if that one plays.
        const int nx = (i + 1) % kSteps;
        tie_ = dice01(s.slide) < slide_ && stepPlays(nx);
        gateLeft_ = tie_ ? -1 : std::max(1, (int)(length_ * stepSamples_) - (late ? sinceStep_ : 0));
        playingStep_ = i;
        if (gNoteHook) gNoteHook(gNoteHookUser, {i, note, accent, legato, late, offset});
    }

    // ----- notes from the MPC -----
    void handleMidi(const mpc::MidiEvent& e, int offset)
    {
        if (e.isNoteOn())
        {
            addHeld(e.note());
            applyKey(offset);
        }
        else if (e.isNoteOff())
        {
            removeHeld(e.note());
            if (heldCount_ > 0) applyKey(offset);
            else if (play_ == kPlayHeld) { voice_.release(); tie_ = false; gateLeft_ = -1; }
        }
        else if (e.isAllNotesOff())
        {
            heldCount_ = 0;
            keyShiftDeg_ = 0; keyShiftSemi_ = 0;
            if (play_ == kPlayHeld) { voice_.release(); tie_ = false; gateLeft_ = -1; }
        }
    }

    void addHeld(int note)
    {
        removeHeld(note);
        if (heldCount_ < 16) held_[heldCount_++] = note;
    }
    void removeHeld(int note)
    {
        int w = 0;
        for (int r = 0; r < heldCount_; ++r) if (held_[r] != note) held_[w++] = held_[r];
        heldCount_ = w;
    }

    // The lowest held note sets the key shift (latched). If the current step began less than
    // kLateMs ago, it is re-pitched (or, in Held mode, started late).
    void applyKey(int /*offset*/)
    {
        if (heldCount_ == 0) return;
        int low = held_[0];
        for (int k = 1; k < heldCount_; ++k) low = std::min(low, held_[k]);
        const int rel = ((low % 12) - root_ + 12) % 12;
        const int len = kScaleLen[scale_];
        int idx = 0;
        for (int k = 0; k < len; ++k) if (kScaleNotes[scale_][k] <= rel) idx = k;
        const int deg = (idx * 2 > len) ? idx - len : idx;           // nearest: up to half the scale
        const int semi = rel >= 6 ? rel - 12 : rel;
        const bool changed = deg != keyShiftDeg_ || semi != keyShiftSemi_;
        keyShiftDeg_ = deg; keyShiftSemi_ = semi;
        if (keys_ == kKeysOff || !running_ || curStep_ < 0 || sinceStep_ >= lateSamples_) return;
        if (playingStep_ == curStep_ && voice_.active() && (gateLeft_ > 0 || tie_))
        {
            if (changed) voice_.retune(pitchOf(curStep_));
        }
        else if (play_ == kPlayHeld) playStep(curStep_, 0, true);
    }

    // ----- Cut Drift: smooth value noise over the bar count, a new point every 4 bars -----
    float driftValue(double bars) const
    {
        if (cutDrift_ <= 0.0f) return 0.0f;
        const double x = bars * 0.25;
        const double k = std::floor(x);
        const float t = (float)(x - k);
        const float a = rnd01(patternSeed_, (uint32_t)(int64_t)k, 0, 77) * 2.0f - 1.0f;
        const float b = rnd01(patternSeed_, (uint32_t)(int64_t)(k + 1), 0, 77) * 2.0f - 1.0f;
        const float s = t * t * (3.0f - 2.0f * t);
        return a + (b - a) * s;
    }

    // ----- pattern snapshot for saveExtraState (UI thread) via a sequence lock -----
    void publish()
    {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        pubPattern_ = pattern_;
        pubSeed_ = patternSeed_;
        seq_.fetch_add(1, std::memory_order_acq_rel);
    }
    void readPublished(Pattern& p, uint32_t& seed) const
    {
        for (int tries = 0; tries < 1000; ++tries)
        {
            const unsigned a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;
            p = pubPattern_;
            seed = pubSeed_;
            if (seq_.load(std::memory_order_acquire) == a) return;
        }
    }

    // Output ceiling: untouched below -3 dBFS (0.7), then rounds off smoothly toward 0.99.
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
        info.name         = "OCKM DRIFT";
        info.vendor       = "OCKM";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;     // 1.0.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "O8z7";

    LeadVoice voice_;
    OnePoleHP dc_;
    Smoother volume_;
    Knobs knobs_;
    double sr_ = 44100.0;

    // settings (block)
    float length_ = 0.5f, mutate_ = 0.25f, density_ = 0.5f, slide_ = 0.2f, cutDrift_ = 0.0f, swing_ = 0.5f;
    int keys_ = kKeysInKey, play_ = kPlayAlways, evolve_ = 2, seed_ = 1, scale_ = 0, root_ = 9;
    int range_ = 1, octave_ = 0, hits_ = 7, rotate_ = 0, rate_ = 0;
    bool runStop_ = false, lock_ = false;

    // clock
    double bps_ = 2.0 / 44100.0, stepBeats_ = 0.25, stepSamples_ = 5512.5, freePos_ = 0.0;
    int loopBars_ = 1, lateSamples_ = 1102;
    bool running_ = false, wasPlaying_ = false;
    long long lastBar_ = 0, barsPlayed_ = 0;
    int barsSinceChange_ = 0;
    long long sinceBar_ = 0;
    double barSamples_ = 88200.0;
    int curStep_ = -1, sinceStep_ = 0, playingStep_ = -1, gateLeft_ = -1;
    bool tie_ = false;

    // pattern
    Pattern pattern_;
    uint32_t patternSeed_ = 1, cycle_ = 0;

    // keys
    int held_[16] = {};
    int heldCount_ = 0, keyShiftDeg_ = 0, keyShiftSemi_ = 0;

    // state handover
    std::atomic<unsigned> seq_{0};
    Pattern pubPattern_;
    uint32_t pubSeed_ = 1;
    Pattern stagedPattern_;
    uint32_t stagedSeed_ = 1;
    std::atomic<bool> staged_{false};
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(OCKMDRIFT)
