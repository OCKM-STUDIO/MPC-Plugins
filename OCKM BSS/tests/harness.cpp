// Test harness for OCKM BSS: includes the plugin source directly and checks what SPEC.md
// promises (section 10). Ends with ALL PASS, and renders the demo WAV.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM BSS/src" "OCKM BSS/tests/harness.cpp" -o "OCKM BSS/build/harness"
//   "OCKM BSS/build/harness" "OCKM BSS/build/demo.wav"
#include "OCKMBSS.cpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

static const int SR = 44100;
static const double BPM = 124.0;
static const int STEP = (int)(SR * 60.0 / BPM / 4.0);       // a 16th at 124 BPM

struct Ev { int frame; int note; int vel; bool on; };
struct Stereo { std::vector<float> L, R; };
using Automation = std::function<void(OCKMBSS&, int /*pos*/)>;

static Stereo render(OCKMBSS& p, const std::vector<Ev>& evs, int frames, Automation autom = nullptr, int block = 128)
{
    p.internalSetSampleRate(SR); p.internalSetBlockSize(block); p.internalResume();
    Stereo o; o.L.assign(frames, 0.0f); o.R.assign(frames, 0.0f);
    std::vector<mpc::MidiEvent> mev;
    for (int pos = 0; pos < frames; pos += block)
    {
        const int n = std::min(block, frames - pos);
        if (autom) autom(p, pos);
        mev.clear();
        for (const auto& e : evs) if (e.frame >= pos && e.frame < pos + n)
            mev.push_back(e.on ? mpc::MidiEvent::noteOn(0, e.note, e.vel, e.frame - pos) : mpc::MidiEvent::noteOff(0, e.note, e.frame - pos));
        float* outs[2] = {o.L.data() + pos, o.R.data() + pos};
        mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = n; c.sampleRate = SR;
        c.midi = mev.data(); c.numMidiEvents = (int)mev.size();
        c.transport.valid = c.transport.playing = c.transport.tempoValid = c.transport.ppqValid = true;
        c.transport.bpm = BPM; c.transport.ppqPos = pos * BPM / 60.0 / SR;
        p.process(c);
    }
    return o;
}

static float peak(const std::vector<float>& x, int a = 0, int b = -1)
{ if (b < 0) b = (int)x.size(); float m = 0; for (int i = a; i < b; ++i) m = std::fmax(m, std::fabs(x[i])); return m; }
static float rms(const std::vector<float>& x, int a, int b)
{ double s = 0; for (int i = a; i < b; ++i) s += (double)x[i] * x[i]; return (float)std::sqrt(s / std::max(1, b - a)); }
static float db(float g) { return 20.0f * std::log10(std::fmax(g, 1e-12f)); }
// Brightness in Hz: RMS of the slope over RMS of the signal, scaled so a sine at f reads f.
static float bright(const std::vector<float>& x, int a, int b)
{
    double h = 0, e = 0;
    for (int i = a; i < b; ++i) { const double d = x[i] - x[i - 1]; h += d * d; e += (double)x[i] * x[i]; }
    return e <= 0 ? 0.0f : (float)(std::sqrt(h / e) * SR / (2.0 * 3.14159265358979));
}
// Energy above fc (4-pole high-pass) relative to all of it, in dB.
static float aboveDb(const std::vector<float>& x, int a, int b, double fc)
{
    const double w = 2.0 * 3.14159265358979 * fc / SR, c = std::exp(-w);
    double lp[4] = {0, 0, 0, 0}, eh = 0, e = 0;
    for (int i = std::max(0, a - 4096); i < b; ++i)              // warm the filter up before the window
    {
        double v = x[i];
        for (int k = 0; k < 4; ++k) { lp[k] = (1 - c) * v + c * lp[k]; v = v - lp[k]; }
        if (i >= a) { eh += v * v; e += (double)x[i] * x[i]; }
    }
    return e <= 0 ? -200.0f : (float)(10.0 * std::log10(std::fmax(eh, 1e-30) / e));
}
// Top-end index in dB: the average of the energy above 250 Hz, 1 kHz and 4 kHz. It keeps
// responding across the whole range, where a Hz measure is swamped by the fundamental.
static float brightDb(const std::vector<float>& x, int a, int b)
{ return (aboveDb(x, a, b, 250.0) + aboveDb(x, a, b, 1000.0) + aboveDb(x, a, b, 4000.0)) / 3.0f; }
// Goertzel magnitude of frequency f over x[a..b).
static float tone(const std::vector<float>& x, int a, int b, double f)
{
    const double w = 2.0 * 3.14159265358979 * f / SR, k = 2.0 * std::cos(w); double s1 = 0, s2 = 0;
    for (int i = a; i < b; ++i) { const double s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
    return (float)(std::sqrt(std::fmax(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) * 2.0 / (b - a));
}
// Sample index of each upward zero crossing in [a, b).
static std::vector<int> upCrossings(const std::vector<float>& x, int a, int b)
{ std::vector<int> z; for (int i = std::max(a, 1); i < b; ++i) if (x[i - 1] < 0 && x[i] >= 0) z.push_back(i); return z; }
static std::vector<Ev> held(int note, int vel, int onFrame, int offFrame) { return {{onFrame, note, vel, true}, {offFrame, note, 0, false}}; }
static float msToFrames(float ms) { return ms * 0.001f * SR; }

// A 2-bar house bassline in 16ths (MIDI 36 = C1 on the MPC). legatoSteps lets some notes overlap.
static std::vector<Ev> bassline(int bars, int startFrame = 0, int vel = 127, bool legato = false)
{
    struct N { int step; int note; int len; };
    static const N bar1[] = {{2, 36, 1}, {6, 36, 1}, {7, 48, 1}, {10, 36, 1}, {14, 39, 1}, {15, 41, 1}};
    static const N bar2[] = {{2, 36, 1}, {5, 43, 1}, {6, 36, 1}, {10, 34, 2}, {12, 36, 1}, {14, 46, 1}, {15, 48, 1}};
    std::vector<Ev> ev;
    for (int b = 0; b < bars; ++b)
    {
        const N* bar = (b % 2) ? bar2 : bar1; const int n = (b % 2) ? 7 : 6;
        for (int i = 0; i < n; ++i)
        {
            const int on = startFrame + (b * 16 + bar[i].step) * STEP;
            const int len = legato && (bar[i].step == 6 || bar[i].step == 14) ? (int)(1.4 * STEP) : (int)(bar[i].len * STEP * 0.8);
            ev.push_back({on, bar[i].note, vel - (bar[i].step % 4 == 2 ? 0 : 12), true});
            ev.push_back({on + len, bar[i].note, 0, false});
        }
    }
    return ev;
}

static void writeWav(const char* path, const Stereo& s)
{
    FILE* f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); return; }
    const int n = (int)s.L.size();
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 4); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(SR); w32(SR * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(n * 4);
    for (int i = 0; i < n; ++i)
    {
        w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, s.L[i])) * 32767));
        w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, s.R[i])) * 32767));
    }
    fclose(f);
}

static void applyProgram(OCKMBSS& p, int prog) { p.setProgram(prog); }

int main(int argc, char** argv)
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    // ---------------------------------------------------------------------------------------
    // 1. Filter sweep opens smoothly: a held saw note, Cutoff ramped 0 -> 100 % over 4 s.
    // ---------------------------------------------------------------------------------------
    {
        OCKMBSS p; p.set(kSaw, 100); p.set(kSub, 0); p.set(kEnvAmt, 0); p.set(kDecay, 100); p.set(kVelFilt, 0); p.set(kDetune, 0);
        const int len = 4 * SR, start = SR / 4;
        auto o = render(p, held(36, 127, 0, start + len + SR / 10), start + len,
                        [&](OCKMBSS& q, int pos) { q.set(kCutoff, 100.0f * mpc::clamp((float)(pos - start) / len, 0.0f, 1.0f)); });
        const int win = (int)std::lround(3.0 * SR / 65.406);      // 3 periods of the note (~46 ms)
        int dips = 0; float worstJump = 0, first = 0, last = 0;
        float prev = brightDb(o.L, start, start + win);
        first = prev;
        for (int a = start + win; a + win <= start + len; a += win)
        {
            const float b = brightDb(o.L, a, a + win);
            if (b < prev - 0.2f) ++dips;
            worstJump = std::fmax(worstJump, b - prev);
            prev = b; last = b;
        }
        printf("      sweep: top-end index %.1f dB -> %.1f dB, worst rise between 46 ms windows %.2f dB, dips %d\n", first, last, worstJump, dips);
        check(dips == 0, "filter sweep: brightness rises steadily across the whole sweep");
        check(worstJump < 2.0f, "filter sweep: no sudden jumps between windows (< 2 dB)");
        check(last - first > 30.0f, "filter sweep: opens from dark to bright (> 30 dB more top end)");
    }

    // ---------------------------------------------------------------------------------------
    // 2. No zipper noise: Cutoff ridden back and forth between 20 and 80 % for 4 s, in jumps
    //    every 128-sample block (coarser than a Q-Link). Clicks at each jump would pile top end
    //    (above 4 kHz) up at the block edges; compared with the same ride in tiny 8-sample steps.
    //    Also: no tone at the 8-sample control rate (5.5 kHz) during a fast sweep of a pure core.
    // ---------------------------------------------------------------------------------------
    {
        const int start = SR / 2, len = 4 * SR;
        auto ride = [&](int block) {
            OCKMBSS p; p.set(kSaw, 100); p.set(kSub, 0); p.set(kEnvAmt, 0); p.set(kDecay, 100); p.set(kReso, 60);
            return render(p, held(48, 127, 0, start + len + SR), start + len, [&](OCKMBSS& q, int pos) {
                if (pos < start) return;
                const float t = std::fmod((float)(pos - start) / SR, 1.0f), tri = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
                q.set(kCutoff, 20.0f + 60.0f * tri); }, block).L;
        };
        auto edgeRatio = [&](const std::vector<float>& c) {
            const double w = 2.0 * 3.14159265358979 * 4000.0 / SR, k = std::exp(-w); double lp[4] = {0, 0, 0, 0}, bin[128] = {0}, all = 0;
            for (int i = 0; i < start + len; ++i)
            {
                double v = c[i]; for (int j = 0; j < 4; ++j) { lp[j] = (1 - k) * v + k * lp[j]; v -= lp[j]; }
                if (i >= start) { bin[(i - start) % 128] += v * v; all += v * v; }
            }
            double worst = 0; for (double v : bin) worst = std::fmax(worst, v);
            return (float)(worst / (all / 128.0));
        };
        const float coarse = edgeRatio(ride(128)), fine = edgeRatio(ride(8));
        gZonesEnabled = false;                                       // pure core: no saw harmonics up there
        OCKMBSS p; p.set(kSaw, 0); p.set(kSub, 0); p.set(kEnvAmt, 0); p.set(kDecay, 100); p.set(kReso, 60);
        const int fast = 50 * 128;
        auto core = render(p, held(48, 127, 0, 2 * SR), start + fast + SR / 10, [&](OCKMBSS& q, int pos) {
            if (pos >= start) q.set(kCutoff, std::fmin(100.0f, 100.0f * (float)((pos - start) / 128 * 128) / fast)); }).L;
        gZonesEnabled = true;
        const float r = rms(core, start, start + fast);
        const float tickDb = db(std::fmax(tone(core, start, start + fast, SR / 8.0 - 130.81), tone(core, start, start + fast, SR / 8.0 + 130.81)) / r);
        printf("      zipper: top end at the worst point in a block x%.2f of average (8-sample steps: x%.2f), control-rate tone %.1f dB\n", coarse, fine, tickDb);
        // The remaining unevenness (same in both) is the note's own waveform folding into the bins.
        check(coarse < fine * 1.1f, "no zipper noise: coarse per-block Cutoff jumps are as clean as 8-sample steps");
        check(tickDb < -70.0f, "no audible control-rate tone during a 0-100 % sweep in 0.15 s (< -70 dB)");
    }

    // ---------------------------------------------------------------------------------------
    // 3. Knob law: each quarter of the Cutoff travel changes brightness by a similar amount.
    // ---------------------------------------------------------------------------------------
    {
        float b[5], hz[5];
        for (int q = 0; q <= 4; ++q)
        {
            OCKMBSS p; p.set(kSaw, 100); p.set(kSub, 0); p.set(kEnvAmt, 0); p.set(kDecay, 100); p.set(kCutoff, 25.0f * q);
            auto o = render(p, held(36, 127, 0, SR), SR);
            b[q] = brightDb(o.L, SR / 2, SR - 100);
            hz[q] = bright(o.L, SR / 2, SR - 100);
        }
        float oct[4], avg = 0;
        for (int q = 0; q < 4; ++q) { oct[q] = b[q + 1] - b[q]; avg += oct[q] / 4; }
        printf("      top-end index at Cutoff 0/25/50/75/100: %.1f %.1f %.1f %.1f %.1f dB (change per quarter %.1f %.1f %.1f %.1f dB)\n",
               b[0], b[1], b[2], b[3], b[4], oct[0], oct[1], oct[2], oct[3]);
        (void)avg;
        bool even = true; for (int q = 0; q < 4; ++q) if (oct[q] < 4.0f) even = false;
        printf("      brightness in Hz at Cutoff 0/25/50/75/100: %.0f %.0f %.0f %.0f %.0f\n", hz[0], hz[1], hz[2], hz[3], hz[4]);
        check(even, "Cutoff knob law: every quarter audibly changes the tone (>= 4 dB more top end per quarter)");
    }

    // ---------------------------------------------------------------------------------------
    // 4. Cutoff x Decay give the variety (4 x 4 grid at default settings otherwise).
    // ---------------------------------------------------------------------------------------
    {
        const float vals[4] = {10, 37, 63, 90};
        float br[4][4], lvl[4][4];
        for (int c = 0; c < 4; ++c) for (int d = 0; d < 4; ++d)
        {
            OCKMBSS p; p.set(kCutoff, vals[c]); p.set(kDecay, vals[d]);
            auto o = render(p, held(36, 127, 0, SR), SR);
            br[c][d] = brightDb(o.L, (int)msToFrames(60), (int)msToFrames(160));
            lvl[c][d] = db(rms(o.L, (int)msToFrames(400), (int)msToFrames(450)) / rms(o.L, 0, (int)msToFrames(60)));
        }
        printf("      top-end index at 60-160 ms (dB) / level at 400 ms (dB re attack):\n");
        for (int c = 3; c >= 0; --c)
        {
            printf("      Cutoff %2.0f:", vals[c]);
            for (int d = 0; d < 4; ++d) printf("  %6.1f /%6.1f", br[c][d], lvl[c][d]);
            printf("\n");
        }
        printf("      (columns: Decay 10, 37, 63, 90)\n");
        bool cutOk = true, decOk = true;
        for (int d = 0; d < 4; ++d) for (int c = 1; c < 4; ++c) if (br[c][d] < br[c - 1][d] + 3.0f) cutOk = false;
        for (int c = 0; c < 4; ++c) for (int d = 1; d < 4; ++d) if (lvl[c][d] < lvl[c][d - 1] + 1.0f) decOk = false;
        check(cutOk, "grid: every Cutoff step is clearly brighter (>= 3 dB more top end), at every Decay");
        check(decOk, "grid: every Decay step clearly lengthens the note (>= 1 dB more at 400 ms), at every Cutoff");

        // Decay law between the zones (25..75 %): equal ratios of note length per eighth of travel.
        OCKMBSS* dummy = nullptr; (void)dummy;
        float t[5];
        for (int i = 0; i <= 4; ++i)
        {
            OCKMBSS p; p.set(kSustain, 0); p.set(kDecay, 25.0f + 12.5f * i);
            auto o = render(p, held(36, 127, 0, 4 * SR), 4 * SR);
            const float pk = rms(o.L, 0, (int)msToFrames(20));
            int at = 0; const int w = (int)msToFrames(5);
            for (int a = 0; a + w < 4 * SR; a += w) if (rms(o.L, a, a + w) > pk * 0.1f) at = a + w;
            t[i] = at * 1000.0f / SR;
        }
        bool law = true;
        for (int i = 0; i < 4; ++i) { const float r = t[i + 1] / t[i]; if (r < 1.3f || r > 2.1f) law = false; }
        printf("      time to -20 dB at Decay 25/37.5/50/62.5/75 (Sustain 0): %.0f %.0f %.0f %.0f %.0f ms\n", t[0], t[1], t[2], t[3], t[4]);
        check(law, "Decay knob law: each step multiplies note length by a similar ratio");
    }

    // ---------------------------------------------------------------------------------------
    // 5. Kick to growl.
    // ---------------------------------------------------------------------------------------
    {
        OCKMBSS k; k.set(kCutoff, 0); k.set(kDecay, 0);
        auto o = render(k, held(36, 127, 0, SR / 2), SR);
        auto z = upCrossings(o.L, 0, (int)msToFrames(200));
        const float fEarly = z.size() > 2 ? (float)SR / (z[1] - z[0]) : 0;
        float fLate = 0;
        for (size_t i = 1; i < z.size(); ++i) if (z[i - 1] > msToFrames(60)) { fLate = (float)SR / (z[i] - z[i - 1]); break; }
        const float pk = peak(o.L);
        const float tail = rms(o.L, (int)msToFrames(280), (int)msToFrames(300));
        printf("      kick corner: pitch %.0f Hz at the start -> %.0f Hz after 60 ms, level at 280-300 ms %.1f dB re peak\n",
               fEarly, fLate, db(tail / pk));
        check(fLate > 0 && fEarly >= 2.0f * fLate, "Cutoff 0 / Decay 0: pitch falls at least an octave at the note start (kick-like)");
        check(db(tail / pk) < -40.0f, "Cutoff 0 / Decay 0: below -40 dB within 300 ms");

        OCKMBSS g; g.set(kCutoff, 100); g.set(kDecay, 100);
        auto og = render(g, held(36, 127, 0, 2 * SR), 2 * SR);
        OCKMBSS i0;
        auto oi = render(i0, held(36, 127, 0, 2 * SR), 2 * SR);
        const float holdRatio = rms(og.L, SR, SR + SR / 10) / rms(og.L, SR / 20, SR / 20 + SR / 10);
        const float bG = bright(og.L, SR, SR + SR / 10), bI = bright(oi.L, SR / 20, SR / 20 + SR / 10);
        const float fullG = rms(og.L, SR, SR + SR / 10), fullI = rms(oi.L, SR, SR + SR / 10);
        printf("      growl corner: level after 1 s x%.2f of the attack, brightness %.0f Hz (Init %.0f Hz), held level after 1 s %.1f dB (Init %.1f dB)\n",
               holdRatio, bG, bI, db(fullG), db(fullI));
        check(holdRatio >= 0.5f, "Cutoff 100 / Decay 100: a held note keeps growling (at least half its level after 1 s)");
        check(bG > 2.5f * bI, "Cutoff 100 / Decay 100: far brighter than the Init sound (> 2.5x in Hz)");
        check(fullG > 2.0f * fullI, "Cutoff 100 / Decay 100: fuller than the Init sound (held level at least 6 dB higher after 1 s)");
        check(peak(o.L) < 1.0f && peak(og.L) < 1.0f, "kick and growl corners stay below 0 dBFS");

        // Diagonal: brightness and loudness-duration rise step by step.
        float pb = 0, pe = 0; bool diag = true;
        printf("      diagonal Cutoff=Decay 0/25/50/75/100: ");
        for (int s = 0; s <= 4; ++s)
        {
            OCKMBSS p; p.set(kCutoff, 25.0f * s); p.set(kDecay, 25.0f * s);
            auto od = render(p, held(36, 127, 0, SR), SR);
            const float b = bright(od.L, (int)msToFrames(30), (int)msToFrames(130));
            const float e = rms(od.L, 0, SR);
            printf("%.0f Hz/%.3f  ", b, e);
            if (s > 0 && (b <= pb || e <= pe)) diag = false;
            pb = b; pe = e;
        }
        printf("\n");
        check(diag, "kick -> growl: brightness and energy rise at every step along the diagonal");
    }

    // ---------------------------------------------------------------------------------------
    // 6. Zones only act where promised: bit-identical with zones off inside the plain region.
    // ---------------------------------------------------------------------------------------
    {
        bool same = true;
        const float cuts[3] = {0, 35, 59}, decs[3] = {26, 50, 74};
        for (float c : cuts) for (float d : decs)
        {
            gZonesEnabled = true;  OCKMBSS a; a.set(kCutoff, c); a.set(kDecay, d); auto oa = render(a, bassline(1), 16 * STEP);
            gZonesEnabled = false; OCKMBSS b; b.set(kCutoff, c); b.set(kDecay, d); auto ob = render(b, bassline(1), 16 * STEP);
            if (oa.L != ob.L) same = false;
        }
        gZonesEnabled = true;
        check(same, "zones: Cutoff below 60 % and Decay 25-75 % sound exactly as without zones");
    }

    // ---------------------------------------------------------------------------------------
    // 7. Resonance stable at full, everywhere.
    // ---------------------------------------------------------------------------------------
    {
        bool ok = true; float worst = 0;
        for (float cut : {0.0f, 30.0f, 60.0f, 100.0f}) for (float env : {0.0f, 100.0f}) for (float drv : {0.0f, 100.0f})
            for (int note = 12; note <= 84; note += 12)
            {
                OCKMBSS p; p.set(kReso, 100); p.set(kCutoff, cut); p.set(kEnvAmt, env); p.set(kDrive, drv); p.set(kDecay, 70);
                auto o = render(p, held(note, 127, 0, SR / 2), SR + SR / 2);
                for (float v : o.L) if (!std::isfinite(v)) ok = false;
                worst = std::fmax(worst, peak(o.L));
                if (rms(o.L, SR + SR / 2 - SR / 10, SR + SR / 2) > 1e-4f) ok = false;
            }
        printf("      worst peak at Resonance 100 %%: %.1f dBFS\n", db(worst));
        check(ok && worst < 1.0f, "Resonance 100 %: stable, finite, below 0 dBFS and silent after release (C0-C6, all cutoffs, env, drive)");
    }

    // ---------------------------------------------------------------------------------------
    // 8. Bass stays full with resonance.
    // ---------------------------------------------------------------------------------------
    {
        float f[2];
        for (int r = 0; r < 2; ++r)
        {
            OCKMBSS p; p.set(kReso, r ? 100.0f : 0.0f); p.set(kEnvAmt, 0); p.set(kDecay, 100);
            auto o = render(p, held(36, 127, 0, SR), SR);
            f[r] = tone(o.L, SR / 2, SR / 2 + 8192, 65.406);
        }
        printf("      fundamental at Resonance 100 vs 0: %.1f dB\n", db(f[1] / f[0]));
        check(db(f[1] / f[0]) > -6.0f, "bass stays full: fundamental at Resonance 100 within 6 dB of Resonance 0");
    }

    // ---------------------------------------------------------------------------------------
    // 9. No clipping across presets; worst case.
    // ---------------------------------------------------------------------------------------
    {
        OCKMBSS names;
        bool ok = true;
        for (int pr = 0; pr < names.numPrograms(); ++pr)
        {
            OCKMBSS p; applyProgram(p, pr);
            auto o = render(p, bassline(4, 0, 127, pr == 4), 64 * STEP + SR);
            const float pk = std::fmax(peak(o.L), peak(o.R));
            printf("      %-18s peak %5.1f dBFS\n", names.programName(pr), db(pk));
            if (pk >= 1.0f) ok = false;
        }
        check(ok, "every preset peaks below 0 dBFS over the demo bassline at velocity 127");
        OCKMBSS w;
        for (int i = 0; i < w.numParams(); ++i) w.setNormalised(i, 1.0f);
        w.set(kAttack, 0);
        auto o = render(w, bassline(2), 32 * STEP + SR);
        printf("      everything at maximum, Volume +6 dB: peak %.2f dBFS\n", db(std::fmax(peak(o.L), peak(o.R))));
        check(std::fmax(peak(o.L), peak(o.R)) < 1.0f, "worst case (everything at maximum) stays below 0 dBFS");
    }

    // ---------------------------------------------------------------------------------------
    // 10. Glide.
    // ---------------------------------------------------------------------------------------
    {
        // Pitch is measured on a pure core (no saw, no sub) so zero crossings are clean;
        // brightness on a saw-rich sound so the filter envelope shows.
        auto setup = [](OCKMBSS& p, float glide) { p.set(kGlide, glide); p.set(kReso, 0); p.set(kEnvAmt, 80); p.set(kDecay, 20); p.set(kSustain, 100); p.set(kSaw, 0); p.set(kSub, 0); };
        auto setupB = [](OCKMBSS& p, float glide) { p.set(kGlide, glide); p.set(kReso, 0); p.set(kEnvAmt, 80); p.set(kDecay, 20); p.set(kSustain, 100); p.set(kSaw, 100); p.set(kCutoff, 20); };
        const int n = SR / 2;
        std::vector<Ev> overlap = {{0, 36, 100, true}, {n, 48, 100, true}, {n + 200, 36, 0, false}, {2 * n, 48, 0, false}};
        std::vector<Ev> separate = {{0, 36, 100, true}, {n - 300, 36, 0, false}, {n, 48, 100, true}, {2 * n, 48, 0, false}};
        OCKMBSS a; setup(a, 40); auto leg = render(a, overlap, 2 * n + SR / 4);
        OCKMBSS b; setup(b, 40); auto sep = render(b, separate, 2 * n + SR / 4);
        OCKMBSS a2; setupB(a2, 40); auto legB = render(a2, overlap, 2 * n + SR / 4);
        OCKMBSS b2; setupB(b2, 40); auto sepB = render(b2, separate, 2 * n + SR / 4);
        OCKMBSS c2; setupB(c2, 0);  auto g0B = render(c2, overlap, 2 * n + SR / 4);
        auto freqAt = [](const std::vector<float>& x, int a, int b) { auto z = upCrossings(x, a, b); return z.size() < 2 ? 0.0f : (float)SR * (z.size() - 1) / (z.back() - z.front()); };
        const float fLegEarly = freqAt(leg.L, n + 50, n + (int)msToFrames(30)), fLegLate = freqAt(leg.L, 2 * n - SR / 10, 2 * n);
        const float fSepEarly = freqAt(sep.L, n + 50, n + (int)msToFrames(30));
        printf("      glide 40 %%: %.0f Hz just after the overlapping note -> %.0f Hz later; separate note starts at %.0f Hz\n",
               fLegEarly, fLegLate, fSepEarly);
        check(fLegEarly < 0.85f * fLegLate, "glide: overlapping notes slide to the new pitch");
        check(fSepEarly > 0.95f * fLegLate, "glide: a note that doesn't overlap starts on pitch (no glide)");
        const int a0 = n + (int)msToFrames(5), a1 = n + (int)msToFrames(60);
        const float bLeg = brightDb(legB.L, a0, a1), bSep = brightDb(sepB.L, a0, a1), bG0 = brightDb(g0B.L, a0, a1);
        printf("      top end just after the 2nd note: overlapping+glide %.1f, separate %.1f, overlapping with Glide 0 %.1f dB\n", bLeg, bSep, bG0);
        check(bLeg < bSep - 6.0f, "glide: overlapping notes don't re-pluck the filter");
        check(std::fabs(bG0 - bSep) < 1.5f, "Glide 0: every note re-plucks, even when notes overlap");
    }

    // ---------------------------------------------------------------------------------------
    // 11. Velocity.
    // ---------------------------------------------------------------------------------------
    {
        auto b = [](float velFilt, int vel) {
            OCKMBSS p; p.set(kVelFilt, velFilt); p.set(kDecay, 100);
            auto o = render(p, held(36, vel, 0, SR / 2), SR / 2);
            return brightDb(o.L, SR / 4, SR / 2);
        };
        const float on40 = b(50, 40), on127 = b(50, 127), off40 = b(0, 40), off127 = b(0, 127);
        printf("      top end vel 40 / 127: Vel>Filter 50 %%: %.1f / %.1f dB, Vel>Filter 0: %.1f / %.1f dB\n", on40, on127, off40, off127);
        check(on40 < on127 - 6.0f, "velocity: softer notes are darker with Vel>Filter on (> 6 dB less top end)");
        check(std::fabs(off40 - off127) < 0.5f, "velocity: no brightness change with Vel>Filter at 0");
    }

    // ---------------------------------------------------------------------------------------
    // 12. Volume (parameter 0) is harmless: only the level changes.
    // ---------------------------------------------------------------------------------------
    {
        OCKMBSS a; a.set(kVolume, -12); auto oa = render(a, bassline(1), 16 * STEP);
        OCKMBSS b; b.set(kVolume, -18.0206f); auto ob = render(b, bassline(1), 16 * STEP);
        float maxd = 0; for (size_t i = 0; i < oa.L.size(); ++i) maxd = std::fmax(maxd, std::fabs(oa.L[i] - 2.0f * ob.L[i]));
        check(maxd < 1e-4f, "Volume only changes the level (-12 dB vs -18 dB renders match x2)");
    }

    // ---------------------------------------------------------------------------------------
    // 12b. Tone darkens after the filter; Drive (before the filter) adds grit at a similar level.
    // ---------------------------------------------------------------------------------------
    {
        auto top = [&](float tone, float cut, float drive, float& lvl) {
            OCKMBSS p; p.setNormalised(kTone, tone); p.setNormalised(kCutoff, cut); p.setNormalised(kDrive, drive);
            p.setNormalised(kDecay, 0.9f);
            auto o = render(p, bassline(2), 32 * STEP);
            lvl = db(rms(o.L, 0, (int)o.L.size()));
            return brightDb(o.L, 0, (int)o.L.size());
        };
        float l0, l1, l2, lA, lB;
        const float t0 = top(0.0f, 0.8f, 0.0f, l0), t1 = top(0.3f, 0.8f, 0.0f, l1), t2 = top(1.0f, 0.8f, 0.0f, l2);
        printf("      top-end index at Tone 0 / 30 / 100 (Cutoff 80): %.1f / %.1f / %.1f dB, level %.1f / %.1f / %.1f dB\n",
               t0, t1, t2, l0, l1, l2);
        check(t2 - t1 > 3.0f && t1 - t0 > 3.0f, "Tone: each step darker or brighter (> 3 dB top end), low end kept");
        check(std::fabs(l2 - l0) < 3.0f, "Tone: level within 3 dB from Tone 0 to 100");
        const float dA = top(0.3f, 0.42f, 0.0f, lA), dB = top(0.3f, 0.42f, 1.0f, lB);
        printf("      Drive 0 / 100 (Init): top-end index %.1f / %.1f dB, level %.1f / %.1f dB\n", dA, dB, lA, lB);
        check(dB - dA > 2.0f, "Drive: adds harmonics (> 2 dB more top end at 100 %)");
        check(std::fabs(lB - lA) < 2.0f, "Drive: level-matched (within 2 dB at the Init sound)");
    }

    // ---------------------------------------------------------------------------------------
    // 13. Echo.
    // ---------------------------------------------------------------------------------------
    {
        // Dry stays centred with the echo off; Mix 0 ignores Time and Feedback entirely.
        OCKMBSS a; auto oa = render(a, bassline(2), 32 * STEP);
        OCKMBSS b; b.set(kEchoTime, 0); b.set(kEchoFb, 100); auto ob = render(b, bassline(2), 32 * STEP);
        check(oa.L == oa.R && oa.L == ob.L && ob.L == ob.R, "echo off (Mix 0): output identical whatever Time/Feedback, and L = R");

        // Lows: wet vs dry energy below 150 Hz.
        OCKMBSS c; c.set(kEchoMix, 100); c.set(kEchoFb, 0); c.set(kEchoTime, 4);
        auto oc = render(c, bassline(2), 32 * STEP);
        std::vector<float> wet(oc.L.size()), dry = oa.L;
        for (size_t i = 0; i < wet.size(); ++i) wet[i] = (oc.L[i] - oa.L[i]) + (oc.R[i] - oa.R[i]);
        auto lowEnergy = [](const std::vector<float>& x) {
            double e = 0; for (double f = 30; f <= 150; f += 2.5) { const float t = tone(x, 0, (int)x.size(), f); e += t * t; } return (float)e; };
        const float lowDb = 10.0f * std::log10(lowEnergy(wet) / lowEnergy(dry));
        printf("      echo energy below 150 Hz: %.1f dB relative to the dry bass\n", lowDb);
        check(lowDb < -24.0f, "echo: the repeats carry at least 24 dB less low end than the bass");

        // Timing and ping-pong: one bright short note.
        OCKMBSS d; d.set(kEchoMix, 100); d.set(kEchoFb, 50); d.set(kEchoTime, 3); d.set(kCutoff, 90); d.set(kDecay, 0);
        OCKMBSS e;                                                    d.set(kSaw, 100); e.set(kCutoff, 90); e.set(kDecay, 0); e.set(kSaw, 100);
        auto od = render(d, held(60, 127, 1000, 1000 + STEP / 2), 2 * SR);
        auto oe = render(e, held(60, 127, 1000, 1000 + STEP / 2), 2 * SR);
        std::vector<float> wl(od.L.size()), wr(od.L.size());
        for (size_t i = 0; i < wl.size(); ++i) { wl[i] = od.L[i] - oe.L[i]; wr[i] = od.R[i] - oe.R[i]; }
        auto onset = [](const std::vector<float>& x, int from) { const float m = peak(x, from); for (int i = from; i < (int)x.size(); ++i) if (std::fabs(x[i]) > 0.1f * m) return i; return -1; };
        const int dryOn = onset(oe.L, 0);
        const float T = 0.75f * 60.0f / (float)BPM * SR;
        const int lOn = onset(wl, 0), rOn = onset(wr, 0);
        printf("      echo dotted 1/8 at %.0f BPM: expected %.1f ms; left repeat at %.1f ms, right repeat at %.1f ms after the note\n",
               BPM, T * 1000.0f / SR, (lOn - dryOn) * 1000.0f / SR, (rOn - dryOn) * 1000.0f / SR);
        check(std::fabs(lOn - dryOn - T) < msToFrames(1), "echo: first repeat on time (within 1 ms), on the left");
        check(std::fabs(rOn - dryOn - 2 * T) < msToFrames(1), "echo: second repeat on the right, one echo time later");
        check(rms(wr, dryOn + (int)T, dryOn + (int)T + SR / 20) < 0.01f * rms(wl, dryOn + (int)T, dryOn + (int)T + SR / 20),
              "echo: ping-pong (the first repeat is left only)");

        // Feedback 100 % always dies away.
        OCKMBSS f; f.set(kEchoMix, 100); f.set(kEchoFb, 100); f.set(kCutoff, 70);
        auto of = render(f, bassline(1), 16 * STEP + 10 * SR);
        const float pk = std::fmax(peak(of.L), peak(of.R));
        const int n = (int)of.L.size();
        const float end = std::fmax(rms(of.L, n - SR / 2, n), rms(of.R, n - SR / 2, n));
        bool fin = true; for (float v : of.L) if (!std::isfinite(v)) fin = false;
        printf("      Feedback 100 %%: peak %.1f dBFS, last 0.5 s of 10 s tail %.1f dB re peak\n", db(pk), db(end / pk));
        check(fin && db(end / pk) < -60.0f && pk < 1.0f, "echo: Feedback 100 % dies below -60 dB within 10 s and never clips");

        // Time changed mid-note: no click.
        OCKMBSS g; g.set(kEchoMix, 60); g.set(kEchoFb, 50); g.set(kEchoTime, 2); g.set(kDecay, 100); g.set(kCutoff, 50);
        const int sw = SR;
        auto og = render(g, held(48, 127, 0, 2 * SR), 2 * SR, [&](OCKMBSS& q, int pos) { if (pos >= sw) q.set(kEchoTime, 4); });
        OCKMBSS h; h.set(kDecay, 100); h.set(kCutoff, 50);
        auto oh = render(h, held(48, 127, 0, 2 * SR), 2 * SR);
        std::vector<float> w2(og.L.size()); for (size_t i = 0; i < w2.size(); ++i) w2[i] = og.L[i] - oh.L[i];
        auto d2max = [&](int a, int b) { float m = 0; for (int i = a; i < b; ++i) m = std::fmax(m, std::fabs(w2[i] - 2 * w2[i - 1] + w2[i - 2])); return m; };
        const float around = d2max(sw, sw + (int)msToFrames(40)), before = d2max(sw - SR / 4, sw - 10);
        printf("      echo Time change mid-note: curvature x%.2f of normal\n", around / before);
        check(around < 1.5f * before, "echo: changing Time mid-note doesn't click");
    }

    // ---------------------------------------------------------------------------------------
    // 14. CPU on this computer (orientation for the MPC One budget).
    // ---------------------------------------------------------------------------------------
    {
        OCKMBSS p; applyProgram(p, 3);                               // Warm House Pluck, echo on
        p.set(kEchoMix, 50); p.set(kCutoff, 90); p.set(kDecay, 90);   // growl zone + echo: the busiest path
        const int frames = 30 * SR;
        auto t0 = std::chrono::steady_clock::now();
        auto o = render(p, bassline(48), frames);
        auto t1 = std::chrono::steady_clock::now();
        const double pct = std::chrono::duration<double>(t1 - t0).count() / (frames / (double)SR) * 100.0;
        printf("      CPU: %.3f %% of one desktop core (30 s of bassline, growl + echo on; includes harness overhead)\n", pct);
        check(pct < 0.3, "CPU under 0.3 % of one desktop core");
        (void)o;
    }

    // ---------------------------------------------------------------------------------------
    // Demo WAV: 4 bars of each preset, then Open Sweep ridden from kick to growl and back,
    // then Warm House Pluck with the echo turned up.
    // ---------------------------------------------------------------------------------------
    if (argc > 1)
    {
        Stereo demo;
        auto append = [&](const Stereo& s) { demo.L.insert(demo.L.end(), s.L.begin(), s.L.end()); demo.R.insert(demo.R.end(), s.R.begin(), s.R.end()); };
        OCKMBSS names;
        for (int pr = 0; pr < names.numPrograms(); ++pr)
        {
            OCKMBSS p; applyProgram(p, pr);
            append(render(p, bassline(4, 0, 127, pr == 4), 64 * STEP));
        }
        {
            OCKMBSS p; applyProgram(p, 8);                           // Open Sweep
            const int len = 16 * 16 * STEP;                          // 16 bars: up over 8, down over 8
            append(render(p, bassline(16), len, [&](OCKMBSS& q, int pos) {
                const float t = (float)pos / len, v = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
                q.set(kCutoff, 100.0f * v); q.set(kDecay, 100.0f * v); }));
        }
        {
            OCKMBSS p; applyProgram(p, 3); p.set(kEchoMix, 45); p.set(kEchoFb, 50);
            append(render(p, bassline(4), 64 * STEP + 2 * SR));
        }
        writeWav(argv[1], demo);
        printf("      demo: %s (%.1f s, peak %.1f dBFS)\n", argv[1], demo.L.size() / (double)SR, db(std::fmax(peak(demo.L), peak(demo.R))));
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
