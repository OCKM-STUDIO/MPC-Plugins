// Test harness for OCKM Gate: includes the plugin source directly.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM Gate/src" "OCKM Gate/tests/harness.cpp" -o "OCKM Gate/build/harness"
//   "OCKM Gate/build/harness" "OCKM Gate/build/demo-presets.wav"
#include "OCKMGate.cpp"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static const double SR = 44100.0;

struct Render { std::vector<float> L, R; };

// Feed `in(i)` (same on both channels) through the plugin. playing: transport runs from ppq 0.
static Render run(OCKMGate& p, int frames, double bpm, bool playing, std::function<float(int)> in,
                  int block = 128, std::function<void(int)> beforeBlock = nullptr, double startPpq = 0.0)
{
    p.internalSetSampleRate(SR); p.internalSetBlockSize(block); p.internalResume();
    Render r; r.L.resize(frames); r.R.resize(frames);
    for (int pos = 0; pos < frames; pos += block)
    {
        if (beforeBlock) beforeBlock(pos);
        const int n = std::min(block, frames - pos);
        for (int i = 0; i < n; ++i) r.L[pos + i] = r.R[pos + i] = in(pos + i);
        float* io[2] = {r.L.data() + pos, r.R.data() + pos};
        mpc::ProcessContext c;
        c.inputs = io; c.outputs = io; c.numInputs = 2; c.numOutputs = 2; c.numFrames = n; c.sampleRate = SR;
        c.transport.valid = true; c.transport.tempoValid = true; c.transport.bpm = bpm;
        c.transport.playing = playing; c.transport.ppqValid = playing;
        const double ppq = startPpq + pos / SR * bpm / 60.0;
        c.transport.ppqPos = ppq; c.transport.barStartPpq = std::floor(ppq / 4.0) * 4.0;
        p.process(c);
    }
    return r;
}

static float avg(const std::vector<float>& x, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += x[i]; return (float)(s / (b - a)); }

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = fopen(path, "wb"); int n = (int)L.size(); int sr = 44100;
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 4); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(sr); w32(sr * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(n * 4);
    for (int i = 0; i < n; ++i) { w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, L[i])) * 32767)); w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, R[i])) * 32767)); }
    fclose(f);
}

int main(int argc, char** argv)
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    const double bpm = 120.0;
    const int step = (int)(SR * 60.0 / bpm / 4.0);      // 1/16 at 120 BPM = 5512 samples
    auto dc = [](int) { return 0.5f; };                 // constant input: output = 0.5 * gain

    // 1. Classic 16ths locks to the song position: on steps open, off steps closed.
    {
        OCKMGate p; p.setProgram(1);   // X.XX.XX.X.XX.XX.
        auto r = run(p, 16 * step, bpm, true, dc);
        const char* pat = "X.XX.XX.X.XX.XX.";
        bool ok = true;
        for (int k = 0; k < 16; ++k)
        {
            const float g = avg(r.L, k * step + step / 10, k * step + step / 2) / 0.5f;   // early part of each step
            if ((pat[k] == 'X') != (g > 0.8f)) ok = false;
            if (pat[k] == '.' && g > 0.05f) ok = false;
        }
        check(ok, "Classic 16ths: on steps open, off steps closed, locked to the beat");
    }
    // 2. Pump: ducked just after each beat, recovered before the next one.
    {
        OCKMGate p; p.setProgram(3);   // Quarter Pump
        auto r = run(p, 16 * step, bpm, true, dc);
        const float lowAfterBeat = avg(r.L, 4 * step + 100, 4 * step + 400) / 0.5f;
        const float beforeNext = avg(r.L, 8 * step - 600, 8 * step - 100) / 0.5f;
        printf("      Quarter Pump gain: just after beat %.2f, just before next beat %.2f\n", lowAfterBeat, beforeNext);
        check(lowAfterBeat < 0.35f && beforeNext > 0.85f, "Pump ducks on the beat and recovers through the off steps");
    }
    // 3. Ping-Pong: consecutive steps alternate between left and right.
    {
        OCKMGate p; p.setProgram(7);
        auto r = run(p, 4 * step, bpm, true, dc);
        float l0 = avg(r.L, step / 10, step / 3), r0 = avg(r.R, step / 10, step / 3);
        float l1 = avg(r.L, step + step / 10, step + step / 3), r1 = avg(r.R, step + step / 10, step + step / 3);
        printf("      step 1 L/R %.2f/%.2f, step 2 L/R %.2f/%.2f\n", l0 * 2, r0 * 2, l1 * 2, r1 * 2);
        check((l0 > r0 + 0.3f && r1 > l1 + 0.3f) || (r0 > l0 + 0.3f && l1 > r1 + 0.3f), "Ping-Pong alternates left and right");
    }
    // 4. Swing delays every second step.
    {
        OCKMGate p; p.set(kSwing, 66.f); p.set(kMode, (float)kGate);
        for (int i = 0; i < 16; ++i) p.set(kOn1 + i, i == 1 ? 1.f : 0.f);
        auto r = run(p, 2 * step, bpm, true, dc);
        int onset = -1; for (int i = 500; i < 2 * step; ++i) if (r.L[i] > 0.25f) { onset = i; break; }
        const int expect = (int)(0.66 * 2 * step);
        printf("      step 2 opens at sample %d (straight %d, expected with 66%% swing %d)\n", onset, step, expect);
        check(std::abs(onset - expect) < 120, "Swing moves the second step late");
    }
    // 5. No clicks: the hardest gate never jumps more than a smooth edge allows.
    {
        OCKMGate p; p.setProgram(1); p.set(kAttack, 0.5f); p.set(kRelease, 1.f); p.set(kCurve, 100.f);
        auto r = run(p, 16 * step, bpm, true, [](int) { return 1.0f; }, 128);
        float maxJump = 0; for (size_t i = 1; i < r.L.size(); ++i) maxJump = std::fmax(maxJump, std::fabs(r.L[i] - r.L[i - 1]));
        printf("      hardest settings: biggest gain change per sample %.3f\n", maxJump);
        check(maxJump < 0.1f, "edges are smoothed (no instant jumps = no clicks)");
    }
    // 6. Tap Rec: arm, tap during step 6 (index 5): only step 6 is left on.
    {
        OCKMGate p; p.setProgram(0);
        p.set(kTapRec, 1.f);
        auto r = run(p, 8 * step, bpm, true, dc, 128, [&](int pos) {
            if (pos <= 5 * step + 1000 && pos + 128 > 5 * step + 1000) p.set(kTap, 1.f);   // tap 23 ms into step 6
        });
        printf("      pattern after tap: "); for (int i = 0; i < 16; ++i) printf("%c", p.getBool(kOn1 + i) ? 'X' : '.'); printf("\n");
        bool ok = p.getBool(kOn1 + 5);
        for (int i = 0; i < 16; ++i) if (i != 5 && p.getBool(kOn1 + i)) ok = false;
        check(ok, "Tap Rec: first tap clears the pattern and sets the nearest step");
        OCKMGate q; q.setProgram(0); q.set(kTapRec, 1.f);
        run(q, 8 * step, bpm, false, dc, 128, [&](int pos) { if (pos == 5 * step / 128 * 128) q.set(kTap, 1.f); });
        bool untouched = true; for (int i = 0; i < 16; ++i) if (!q.getBool(kOn1 + i)) untouched = false;
        check(untouched, "Tap Rec ignores taps while the MPC is stopped");
        OCKMGate w; w.set(kTapRec, 1.f);
        run(w, 4 * step, bpm, true, dc, 128, [&](int pos) { if (pos == 128 * 10) w.setProgram(1); });
        const char* pat = "X.XX.XX.X.XX.XX.";
        bool presetIntact = true; for (int i = 0; i < 16; ++i) if (w.getBool(kOn1 + i) != (pat[i] == 'X')) presetIntact = false;
        check(presetIntact, "choosing a preset while Tap Rec is armed and playing does not count as a tap");
    }
    // 7. Pass-through when it should do nothing.
    {
        OCKMGate p; p.setProgram(1); p.set(kRunStop, 0.f);
        auto r = run(p, 4 * step, bpm, false, [](int i) { return 0.3f * std::sin(i * 0.05f); });
        bool same = true; for (int i = 0; i < 4 * step; ++i) if (std::fabs(r.L[i] - 0.3f * std::sin(i * 0.05f)) > 1e-5f) { same = false; break; }
        check(same, "MPC stopped + Run Stopped off: audio passes untouched");
        OCKMGate q; q.setProgram(1); q.set(kMix, 0.f);
        auto s = run(q, 4 * step, bpm, true, [](int i) { return 0.3f * std::sin(i * 0.05f); });
        same = true; for (int i = 0; i < 4 * step; ++i) if (std::fabs(s.L[i] - 0.3f * std::sin(i * 0.05f)) > 1e-5f) { same = false; break; }
        check(same, "Mix 0: audio passes untouched");
        OCKMGate f; f.setProgram(1);
        auto fr = run(f, 8 * step, bpm, false, dc);
        bool gated = avg(fr.L, step + step / 10, step + step / 2) < 0.05f && avg(fr.L, step / 10, step / 2) > 0.4f;
        check(gated, "MPC stopped + Run Stopped on: keeps gating at the MPC tempo");
    }
    // 8. Rate 1/4 (added after the hardware test) and patterns longer than a bar.
    auto onlyStep = [](OCKMGate& p, int s) { for (int i = 0; i < 16; ++i) p.set(kOn1 + i, i == s ? 1.f : 0.f); };
    auto firstOpen = [](const Render& r, int from) { for (size_t i = (size_t)from; i < r.L.size(); ++i) if (r.L[i] > 0.25f) return (int)i; return -1; };
    const int beat = (int)(SR * 60.0 / bpm);
    {
        OCKMGate p; check(std::string(p.param(kRate).choices.back()) == "1/4" && p.param(kRate).choices.size() == 6
                          && p.param(kRate).choices[2] == "1/16", "Rate: 1/4 appended at the end, old choices keep their places");
        OCKMGate a; a.set(kRate, 3.f); auto st = a.internalSaveState();
        OCKMGate b; b.internalLoadState(st.data(), st.size());
        check(b.getInt(kRate) == 3, "a saved Rate (1/16T) loads back unchanged");
    }
    {
        OCKMGate p; p.set(kRate, 5.f); onlyStep(p, 1);
        auto r = run(p, 2 * beat, bpm, true, dc);
        const int on = firstOpen(r, 500);
        printf("      1/4, step 2 only: opens at sample %d (expected %d)\n", on, beat);
        check(std::abs(on - beat) < 200, "Rate 1/4: one step per beat");
    }
    {
        OCKMGate p; p.set(kRate, 5.f); onlyStep(p, 4);
        auto r = run(p, 17 * beat, bpm, true, dc);
        const int on = firstOpen(r, 500), again = firstOpen(r, on + beat);
        printf("      1/4, step 5 only: opens at sample %d (bar 2 = %d), next at %d (bar 6 = %d)\n", on, 4 * beat, again, 20 * beat);
        check(std::abs(on - 4 * beat) < 200 && again == -1, "Rate 1/4: 16 steps span 4 bars (step 5 lands on bar 2)");
        OCKMGate q; q.set(kRate, 5.f); onlyStep(q, 0);
        auto s2 = run(q, 2 * beat, bpm, true, dc, 128, nullptr, 16.0 * 4);   // MPC started at bar 17
        check(avg(s2.L, 200, beat / 2) > 0.4f, "Rate 1/4: starting at bar 17 lands on step 1 (locked to the song)");
        OCKMGate f; f.set(kRate, 5.f); onlyStep(f, 4);
        auto fr = run(f, 17 * beat, bpm, false, dc);
        const int fon = firstOpen(fr, 500);
        check(std::abs(fon - 4 * beat) < 200, "Rate 1/4 while stopped: own clock also loops over 4 bars");
    }
    {
        OCKMGate p; p.set(kRate, 0.f); onlyStep(p, 8);
        auto r = run(p, 9 * beat, bpm, true, dc);
        const int on = firstOpen(r, 500);
        check(std::abs(on - 4 * beat) < 200, "Rate 1/8: 16 steps span 2 bars (step 9 lands on bar 2)");
        check(OCKMGate::patternBars(16, 0.25, 4.0) == 1 && OCKMGate::patternBars(4, 1.0, 4.0) == 1
              && OCKMGate::patternBars(16, 1.0 / 6.0, 4.0) == 1 && OCKMGate::patternBars(16, 1.0 / 3.0, 4.0) == 2,
              "pattern loop is the fewest whole bars that hold it");
    }
    // 9. Presets: 18, unique names, each has a rhythm, and none clips.
    {
        OCKMGate p; bool ok = p.numPrograms() == 18, named = true, rhythm = true; float peak = 0;
        for (int a = 0; a < p.numPrograms(); ++a)
        {
            for (int b = 0; b < a; ++b) if (std::string(p.programName(a)) == p.programName(b)) named = false;
            OCKMGate q; q.setProgram(a);
            bool any = false; for (int i = 0; i < q.getInt(kSteps); ++i) if (q.getBool(kOn1 + i) && q.get(kLv1 + i) > 0) any = true;
            if (!any) rhythm = false;
            auto r = run(q, 4 * beat * 4, bpm, true, [](int i) { return 0.95f * std::sin(i * 0.031f); });
            for (size_t i = 0; i < r.L.size(); ++i) peak = std::fmax(peak, std::fmax(std::fabs(r.L[i]), std::fabs(r.R[i])));
        }
        printf("      %d presets, loudest peak %.2f dBFS from a -0.45 dBFS input\n", p.numPrograms(), 20 * std::log10(peak));
        check(ok && named, "18 presets with unique names");
        check(rhythm, "every preset switches on at least one step inside its Steps length");
        check(peak < 1.0f, "no preset clips (peaks below 0 dBFS)");
        OCKMGate t; t.setProgram(12);
        check(std::string(t.programName(12)) == "Triplet Roll" && t.getInt(kSteps) == 12 && t.getInt(kRate) == 3, "Triplet Roll sets 1/16T and 12 steps");
    }
    // 10. Demo: every preset gating a pad chord, 2 bars each at 125 BPM.
    {
        const double dbpm = 125.0; const int bar = (int)(SR * 60.0 / dbpm * 4.0);
        auto pad = [](int i) {
            const double t = i / SR; float s = 0;
            const double f[4] = {110.0, 130.81, 164.81, 196.0};
            for (int k = 0; k < 4; ++k) for (int d = -1; d <= 1; ++d) { double ph = std::fmod(t * f[k] * (1 + d * 0.003), 1.0); s += (float)(2 * ph - 1); }
            return s * 0.045f;
        };
        std::vector<float> L, R; OCKMGate p0;
        for (int prog = 0; prog < p0.numPrograms(); ++prog)
        {
            OCKMGate p; p.setProgram(prog);
            auto r = run(p, 2 * bar, dbpm, true, pad);
            printf("      preset %d: %s\n", prog + 1, p.programName(prog));
            L.insert(L.end(), r.L.begin(), r.L.end()); R.insert(R.end(), r.R.begin(), r.R.end());
        }
        if (argc > 1) writeWav(argv[1], L, R);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
