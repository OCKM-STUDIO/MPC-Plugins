// Generator.h - OCKM DRIFT's pattern: scales, the seeded hash, the first pattern of a Seed,
// Mutate, Range folding and Euclidean hits. Plain arithmetic, no allocation.
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
constexpr int kSteps = 16;
constexpr int kNumScales = 8;
constexpr int kScaleLen[kNumScales] = {7, 7, 7, 7, 7, 7, 5, 5};
constexpr int kScaleNotes[kNumScales][7] = {
    {0, 2, 3, 5, 7, 8, 10},   // Minor
    {0, 2, 3, 5, 7, 9, 10},   // Dorian
    {0, 1, 3, 5, 7, 8, 10},   // Phrygian
    {0, 2, 4, 5, 7, 9, 11},   // Major
    {0, 2, 4, 5, 7, 9, 10},   // Mixolydian
    {0, 2, 3, 5, 7, 8, 11},   // Harmonic minor
    {0, 3, 5, 7, 10, 0, 0},   // Minor pentatonic
    {0, 2, 4, 7, 9, 0, 0},    // Major pentatonic
};
// Chord tones (1st, 3rd, 5th) as scale-step indices, per scale.
constexpr int kChordTones[kNumScales][3] = {
    {0, 2, 4}, {0, 2, 4}, {0, 2, 4}, {0, 2, 4}, {0, 2, 4}, {0, 2, 4}, {0, 1, 3}, {0, 2, 3}};
constexpr double kRateBeats[3] = {0.25, 0.5, 1.0};   // 1/16, 1/8, 1/4
constexpr int kEvolveBars[4] = {1, 2, 4, 8};

// ---------------------------------------------------------------------------
// Repeatable randomness: a fixed integer hash of (seed, cycle, step, salt). Same on
// every CPU, so the desktop harness and the MPC play the same line.
// ---------------------------------------------------------------------------
inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash4(uint32_t seed, uint32_t cycle, uint32_t step, uint32_t salt)
{
    return mix32(seed * 0x9E3779B1U ^ mix32(cycle * 0x85EBCA77U ^ mix32(step * 0xC2B2AE3DU ^ mix32(salt + 0x27D4EB2FU))));
}
inline uint16_t dice16(uint32_t s, uint32_t c, uint32_t i, uint32_t salt) { return (uint16_t)(hash4(s, c, i, salt) >> 16); }
inline float rnd01(uint32_t s, uint32_t c, uint32_t i, uint32_t salt) { return (float)(hash4(s, c, i, salt) >> 8) * (1.0f / 16777216.0f); }
inline float dice01(uint16_t d) { return (float)d * (1.0f / 65536.0f); }

// ---------------------------------------------------------------------------
// The pattern: 16 steps, each a note (scale steps above the root, before folding into
// Range) and four dice that the knobs are compared with live.
// ---------------------------------------------------------------------------
struct Step
{
    int8_t   deg = 0;          // raw random-walk position in scale steps
    uint16_t play = 0;         // Density: does the step play?
    uint16_t acc = 0;          // accent
    uint16_t slide = 0;        // Slide
    uint16_t tone = 0;         // beat steps: snap to a chord tone?
};
struct Pattern
{
    Step s[kSteps];
    bool operator==(const Pattern& o) const
    {
        for (int i = 0; i < kSteps; ++i)
            if (s[i].deg != o.s[i].deg || s[i].play != o.s[i].play || s[i].acc != o.s[i].acc ||
                s[i].slide != o.s[i].slide || s[i].tone != o.s[i].tone) return false;
        return true;
    }
    bool operator!=(const Pattern& o) const { return !(*this == o); }
};

constexpr int kDegLimit = 60;

// The first pattern of a Seed: a random walk starting on the root, mostly one scale step.
inline Pattern firstPattern(uint32_t seed)
{
    Pattern p;
    int d = 0;
    for (int i = 0; i < kSteps; ++i)
    {
        if (i > 0)
        {
            const float r = rnd01(seed, 0, (uint32_t)i, 1);
            const int size = r < 0.15f ? 0 : (r < 0.62f ? 1 : (r < 0.92f ? 2 : 3));
            const int sign = rnd01(seed, 0, (uint32_t)i, 2) < 0.5f ? -1 : 1;
            d = clamp(d + sign * size, -kDegLimit, kDegLimit);
        }
        Step& st = p.s[i];
        st.deg = (int8_t)d;
        st.play = dice16(seed, 0, (uint32_t)i, 3);
        st.acc = dice16(seed, 0, (uint32_t)i, 4);
        st.slide = dice16(seed, 0, (uint32_t)i, 5);
        st.tone = dice16(seed, 0, (uint32_t)i, 6);
    }
    return p;
}

// One Evolve point (cycle >= 1): each step changes with chance `mutate`, in one small way.
inline void mutatePattern(Pattern& p, uint32_t seed, uint32_t cycle, float mutate)
{
    if (mutate <= 0.0f) return;
    for (int i = 0; i < kSteps; ++i)
    {
        if (rnd01(seed, cycle, (uint32_t)i, 10) >= mutate) continue;
        Step& st = p.s[i];
        const float kind = rnd01(seed, cycle, (uint32_t)i, 11);
        if (kind < 0.55f)                                    // the note walks one or two scale steps
        {
            const int size = rnd01(seed, cycle, (uint32_t)i, 12) < 0.7f ? 1 : 2;
            const int sign = rnd01(seed, cycle, (uint32_t)i, 13) < 0.5f ? -1 : 1;
            st.deg = (int8_t)clamp(st.deg + sign * size, -kDegLimit, kDegLimit);
        }
        else if (kind < 0.75f) st.play = dice16(seed, cycle, (uint32_t)i, 14);
        else if (kind < 0.88f) st.acc = dice16(seed, cycle, (uint32_t)i, 15);
        else { st.slide = dice16(seed, cycle, (uint32_t)i, 16); st.tone = dice16(seed, cycle, (uint32_t)i, 17); }
    }
}

// Bounce a walk position into 0..top (a triangle fold: never leaps from one end to the other).
inline int foldDeg(int d, int top)
{
    if (top <= 0) return 0;
    const int period = 2 * top;
    int m = d % period;
    if (m < 0) m += period;
    return m > top ? period - m : m;
}

// Euclidean rhythm: `hits` spread evenly over 16 steps (hit on step 0), shifted by `rotate`.
inline bool euclidHit(int step, int hits, int rotate)
{
    const int i = ((step - rotate) % kSteps + kSteps) % kSteps;
    return (i * hits) % kSteps < hits;
}

} // namespace ockm_drift
