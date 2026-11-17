// Test harness for OCKM Wave: includes the plugin source directly and checks what SPEC.md
// section 9 promises. Also renders a demo of every preset.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM Wave/src" "OCKM Wave/tests/harness.cpp" -o "OCKM Wave/build/harness"
//   "OCKM Wave/build/harness" "OCKM Wave/build/demo.wav"
#include <algorithm>
#include <chrono>
static double gBankMs = 0.0;
#include "OCKMWave.cpp"
#include <cstdio>
#include <functional>
#include <vector>

static const double SR = 44100.0;
using Clock = std::chrono::steady_clock;

struct Ev { int frame; mpc::MidiEvent e; };
struct Render { std::vector<float> L, R; double seconds = 0.0; };

static Ev on(int frame, int note, int vel = 127) { return {frame, mpc::MidiEvent::noteOn(0, note, vel)}; }
static Ev off(int frame, int note) { return {frame, mpc::MidiEvent::noteOff(0, note)}; }
static Ev cc(int frame, int c, int v) { return {frame, mpc::MidiEvent::cc(0, c, v)}; }
static int ms(double m) { return (int)(m * 0.001 * SR); }

static void start(OCKMWave& p, int block = 128)
{
    p.internalSetSampleRate(SR); p.internalSetBlockSize(block); p.internalResume();
}

// Render `frames` samples. beforeBlock(pos) can change parameters between blocks.
static Render run(OCKMWave& p, int frames, std::vector<Ev> evs, int block = 128,
                  std::function<void(int)> beforeBlock = nullptr, double bpm = 124.0)
{
    Render r; r.L.assign((size_t)frames, 0.0f); r.R.assign((size_t)frames, 0.0f);
    std::vector<mpc::MidiEvent> midi;
    size_t ev = 0;
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
    const auto t0 = Clock::now();
    for (int pos = 0; pos < frames; pos += block)
    {
        if (beforeBlock) beforeBlock(pos);
        const int n = std::min(block, frames - pos);
        midi.clear();
        while (ev < evs.size() && evs[ev].frame < pos + n) { mpc::MidiEvent e = evs[ev].e; e.sampleOffset = std::max(0, evs[ev].frame - pos); midi.push_back(e); ++ev; }
        float* out[2] = {r.L.data() + pos, r.R.data() + pos};
        mpc::ProcessContext c;
        c.outputs = out; c.numInputs = 0; c.numOutputs = 2; c.numFrames = n; c.sampleRate = SR;
        c.midi = midi.data(); c.numMidiEvents = (int)midi.size();
        c.transport.valid = c.transport.tempoValid = c.transport.playing = c.transport.ppqValid = true;
        c.transport.bpm = bpm; c.transport.ppqPos = pos / SR * bpm / 60.0;
        p.process(c);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return r;
}

static float peak(const Render& r, int a = 0, int b = -1)
{
    if (b < 0) b = (int)r.L.size();
    float m = 0; for (int i = a; i < b; ++i) m = std::max(m, std::max(std::fabs(r.L[(size_t)i]), std::fabs(r.R[(size_t)i])));
    return m;
}
static float rms(const std::vector<float>& x, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += (double)x[(size_t)i] * x[(size_t)i]; return (float)std::sqrt(s / std::max(1, b - a)); }
static float maxStep(const std::vector<float>& x, int a, int b) { float m = 0; for (int i = std::max(1, a); i < b; ++i) m = std::max(m, std::fabs(x[(size_t)i] - x[(size_t)i - 1])); return m; }
static bool finite(const Render& r) { for (size_t i = 0; i < r.L.size(); ++i) if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) return false; return true; }
static bool noDenormals(const Render& r) { for (size_t i = 0; i < r.L.size(); ++i) { const float a = std::fabs(r.L[i]); if (a > 0 && a < 1e-30f) return false; } return true; }
static double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// Magnitude spectrum of N samples from `start` (Blackman-Harris window).
static std::vector<double> spectrum(const std::vector<float>& x, int start, int N)
{
    std::vector<std::complex<double>> a((size_t)N);
    for (int i = 0; i < N; ++i)
    {
        const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * i / N) + 0.14128 * std::cos(4 * M_PI * i / N) - 0.01168 * std::cos(6 * M_PI * i / N);
        a[(size_t)i] = x[(size_t)(start + i)] * w;
    }
    fft(a, -1);
    std::vector<double> m((size_t)N / 2);
    for (int k = 0; k < N / 2; ++k) m[(size_t)k] = std::abs(a[(size_t)k]);
    return m;
}
static double at(const std::vector<double>& m, double hz, int N) { const int k = (int)std::lround(hz / SR * N); double v = 0; for (int j = k - 3; j <= k + 3; ++j) if (j > 0 && j < (int)m.size()) v = std::max(v, m[(size_t)j]); return v; }
// Loudest component that is not a harmonic of f0, relative to the fundamental (dB).
static double nonHarmonicDb(const std::vector<double>& m, double f0, int N)
{
    const double fund = at(m, f0, N), binHz = SR / N;
    double worst = 0;
    for (int k = 8; k < (int)m.size(); ++k)
    {
        const double hz = k * binHz, h = std::round(hz / f0);
        if (h >= 1 && std::fabs(hz - h * f0) < 8 * binHz) continue;
        worst = std::max(worst, m[(size_t)k]);
    }
    return db(worst / fund);
}

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = fopen(path, "wb"); if (!f) { printf("could not write %s\n", path); return; }
    const int n = (int)L.size(), sr = 44100;
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 4); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(sr); w32(sr * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(n * 4);
    for (int i = 0; i < n; ++i) { w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, L[(size_t)i])) * 32767)); w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, R[(size_t)i])) * 32767)); }
    fclose(f);
}

static const int kChord[4] = {48, 55, 58, 63};      // C minor 7-ish house chord

int main(int argc, char** argv)
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    // 0. Table build time (first use of the shared bank).
    {
        const auto t0 = Clock::now();
        (void)bank();
        gBankMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        printf("      wavetable bank: %.1f ms to build, %.2f MB\n", gBankMs, bank().data.size() * 4.0 / 1048576.0);
        check(gBankMs < 50.0, "wavetables build in under 50 ms on the desktop (about 1 s budget on the MPC One)");
    }

    // 1. Program 0 equals the parameter defaults.
    {
        OCKMWave p; p.setProgram(0);
        bool ok = true;
        for (int i = 0; i < p.numParams(); ++i) if (std::fabs(p.get(i) - p.param(i).def) > 1e-3f * std::max(1.0f, std::fabs(p.param(i).def))) ok = false;
        check(ok, "program 01 Init Saw equals the parameter defaults");
    }

    // 1b. Choosing a preset sets EVERY parameter: scramble all knobs first, then load the preset,
    //     and compare with a fresh plugin that loaded the same preset.
    {
        OCKMWave ref;
        bool ok = true;
        for (int prog = 0; prog < ref.numPrograms(); ++prog)
        {
            OCKMWave a, b;
            for (int i = 0; i < a.numParams(); ++i) { const auto& q = a.param(i); a.set(i, q.min + 0.37f * (q.max - q.min)); }
            a.setProgram(prog); b.setProgram(prog);
            for (int i = 0; i < a.numParams(); ++i)
                if (std::fabs(a.get(i) - b.get(i)) > 1e-4f * std::max(1.0f, std::fabs(b.get(i)))) { ok = false;
                    printf("      %s leaves %s unchanged\n", a.programName(prog), a.param(i).name.c_str()); }
        }
        printf("      %d presets\n", ref.numPrograms());
        check(ok, "choosing any preset sets every parameter (all 82)");
        check(ref.numPrograms() == 56 && ref.numParams() == 82, "56 presets, 82 parameters");
    }

    // 1c. Every parameter is used (moved from its default) by at least one preset.
    {
        OCKMWave p;
        std::vector<bool> used((size_t)p.numParams(), false);
        for (int prog = 0; prog < p.numPrograms(); ++prog)
        {
            p.setProgram(prog);
            for (int i = 0; i < p.numParams(); ++i)
                if (std::fabs(p.get(i) - p.param(i).def) > 1e-3f * std::max(1.0f, std::fabs(p.param(i).def))) used[(size_t)i] = true;
        }
        bool ok = true;
        for (int i = 0; i < p.numParams(); ++i) if (!used[(size_t)i]) { ok = false; printf("      no preset uses %s\n", p.param(i).name.c_str()); }
        check(ok, "every parameter is used by at least one preset");
    }

    // 2. Every preset: 4-note chord at velocity 127 peaks below 0 dBFS, sounds, no NaN, no denormal tail.
    {
        bool okPeak = true, okSound = true, okFinite = true;
        OCKMWave counter;
        for (int prog = 0; prog < counter.numPrograms(); ++prog)
        {
            OCKMWave p; p.setProgram(prog); start(p);
            std::vector<Ev> evs = {cc(0, 1, 127)};
            for (int n : kChord) { evs.push_back(on(0, n)); evs.push_back(off(ms(2500), n)); }
            Render r = run(p, ms(6000), evs);
            const float pk = peak(r);
            const float level = rms(r.L, ms(50), ms(2500));
            if (pk >= 1.0f) okPeak = false;
            if (pk < 0.03f) okSound = false;               // plucks decay fast: judge by peak (> -30 dBFS)
            if (!finite(r) || !noDenormals(r)) okFinite = false;
            printf("      %-20s peak %6.1f dBFS  rms %6.1f dB\n", p.programName(prog), db(pk), db(level));
        }
        check(okPeak, "every preset: 4-note chord at full velocity peaks below 0 dBFS");
        check(okSound, "every preset makes sound (peak above -30 dBFS)");
        check(okFinite, "every preset: no NaN/Inf, no denormals");
    }

    // 2b. Step lanes (1.2).
    {
        const double bpm = 120.0;
        const int step = (int)(SR * 60.0 / bpm / 4.0);              // one 1/16 step
        auto gate = [&](int len, std::initializer_list<float> pat) {
            OCKMWave p; start(p);
            p.set(kASus, 100.f); p.set(kPosition, 0.f); p.set(kADest, (float)kToVolume); p.set(kAAmt, 100.f); p.set(kASmooth, 0.f);
            p.set(kALen, (float)len); p.set(kARate, (float)kDivisionSixteenth);
            int i = 0; for (float x : pat) p.set(kAStep1 + i++, x);
            for (; i < 16; ++i) p.set(kAStep1 + i, 0.f);
            return run(p, step * 8, {on(0, 60)}, 128, nullptr, bpm);
        };
        auto stepRms = [&](const Render& r, int k) { return db(rms(r.L, k * step + step / 4, k * step + 3 * step / 4)); };
        const Render a = gate(16, {100, 0, 100, 0, 100, 0, 100, 0});
        const double on0 = stepRms(a, 2), off1 = stepRms(a, 3);
        printf("      Volume lane 100/0 at 1/16: on step %.1f dB, off step %.1f dB\n", on0, off1);
        check(on0 - off1 > 40.0, "step lane on Volume gates in time (on/off steps differ by over 40 dB)");
        const Render b = gate(3, {100, 0, 0});
        const double s3 = stepRms(b, 3), s4 = stepRms(b, 4), s6 = stepRms(b, 6);
        check(s3 - s4 > 40.0 && s6 - s4 > 40.0, "lane Length 3 repeats every 3 steps");
        // A sine with a gate: the biggest sample-to-sample jump anywhere (edges included) is no bigger than
        // the sine's own steepest slope, i.e. the gate's edges are ramped, not cut.
        const float edges = maxStep(a.L, step, step * 8), still = maxStep(a.L, 2 * step + step / 4, 2 * step + 3 * step / 4);
        printf("      gate: largest sample step %.4f (steady sine %.4f)\n", edges, still);
        check(edges <= still * 2.0f, "Smooth 0%: gate edges are ramped (1 ms), no clicks");

        // Pitch lane: step 2 = +12 st -> the zero-crossing rate doubles.
        OCKMWave p; start(p);
        p.set(kASus, 100.f); p.set(kPosition, 0.f); p.set(kADest, (float)kToPitch); p.set(kAAmt, 100.f); p.set(kASmooth, 0.f);
        for (int i = 0; i < 16; ++i) p.set(kAStep1 + i, i % 2 ? 100.f : 0.f);
        const Render c = run(p, step * 4, {on(0, 57)}, 128, nullptr, bpm);
        auto zc = [&](int k) { int z = 0; for (int i = k * step + step / 4; i < k * step + 3 * step / 4; ++i) z += (c.L[(size_t)i - 1] < 0) != (c.L[(size_t)i] < 0); return z; };
        const double ratio = (double)zc(3) / std::max(1, zc(2));
        printf("      Pitch lane: zero crossings step 2 -> 3 ratio %.2f\n", ratio);
        check(ratio > 1.9 && ratio < 2.1, "step lane on Pitch moves by whole semitones (100% step = one octave up)");
    }

    // 2c. Filter character (1.2): 24 dB is steeper; Drive, Fat and Res at maximum stay safe.
    {
        auto bright = [&](int slope) {
            OCKMWave p; start(p);
            p.set(kASus, 100.f); p.set(kCutoff, 500.f); p.set(kReso, 0.f); p.set(kKeyTrack, 0.f); p.set(kSlope, (float)slope);
            const Render r = run(p, ms(600), {on(0, 36)});
            const int N = 8192; auto m = spectrum(r.L, ms(300), N);
            const double f0 = 440.0 * std::pow(2.0, (36 - 69) / 12.0);
            return db(at(m, f0 * 31, N) / at(m, f0, N));             // the 31st harmonic, about 2 kHz
        };
        const double d12 = bright(0), d24 = bright(1);
        printf("      cutoff 500 Hz, 31st harmonic vs fundamental: 12 dB %.1f dB, 24 dB %.1f dB\n", d12, d24);
        check(d24 < d12 - 15.0, "24 dB slope cuts the highs much harder than 12 dB");

        bool okPeak = true, okFin = true; float worst = 0, base = 0;
        for (int ft = 0; ft < 3; ++ft) for (int sl = 0; sl < 2; ++sl) for (int on12 = 0; on12 < 2; ++on12)
        {
            OCKMWave p; start(p);
            p.set(kFType, (float)ft); p.set(kSlope, (float)sl); p.set(kReso, 100.f);
            if (on12) { p.set(kDrive, 100.f); p.set(kFat, 100.f); }
            p.set(kCutoff, 300.f); p.set(kOutput, 0.f); p.set(kUnison, 5.f); p.set(kSub, 100.f); p.set(kEnvCut, 48.f);
            p.set(kMAtk, 1.f); p.set(kMDec, 300.f);
            std::vector<Ev> evs; for (int i = 0; i < 6; ++i) evs.push_back(on(0, 36 + 5 * i));
            const Render r = run(p, ms(2000), evs);
            (on12 ? worst : base) = std::max(on12 ? worst : base, peak(r));
            if (!finite(r) || !noDenormals(r)) okFin = false;
        }
        printf("      Res 100%%, Sub 100%%, 6 notes x 5 copies, Master 0 dB: peak %.1f dBFS; with Drive and Fat 100%%: %.1f dBFS\n", db(base), db(worst));
        if (db(worst) > db(base) + 3.0) okPeak = false;
        check(okPeak, "Drive and Fat at 100% add no more than 3 dB to the loudest peak (all filter types and slopes)");
        check(okFin, "extreme filter settings: no NaN/Inf, no denormals");
    }

    // 3. Aliasing: Shapes saw at C7 stays clean.
    {
        OCKMWave p; start(p);
        const int N = 32768;
        Render r = run(p, ms(1500), {on(0, 96)});
        const double f0 = 440.0 * std::pow(2.0, (96 - 69) / 12.0);
        const double nh = nonHarmonicDb(spectrum(r.L, ms(200), N), f0, N);
        printf("      saw at C7 (%.0f Hz): loudest non-harmonic %.1f dB below the fundamental\n", f0, -nh);
        check(nh < -60.0, "aliasing: saw at C7 has non-harmonic content at least 60 dB down");
        // For information: the warps at the same note.
        const char* names[5] = {"Off", "Sync", "Bend", "Mirror", "Quantize"};
        for (int w = 1; w <= 4; ++w)
        {
            OCKMWave q; q.set(kWarp, (float)w); q.set(kWarpAmt, 50.f); start(q);
            Render rq = run(q, ms(1500), {on(0, 96)});
            OCKMWave q5; q5.set(kWarp, (float)w); q5.set(kWarpAmt, 50.f); start(q5);
            Render r5 = run(q5, ms(1500), {on(0, 72)});
            printf("      (info) %s warp 50%%: loudest non-harmonic %.1f dB down at C7, %.1f dB down at C5\n", names[w],
                   -nonHarmonicDb(spectrum(rq.L, ms(200), N), f0, N), -nonHarmonicDb(spectrum(r5.L, ms(200), N), f0 / 4, N));
        }
    }

    // 4. Morph: Shapes at position 0 is a sine, at 100 % a square (odd harmonics only).
    {
        const int N = 16384; const double f0 = 220.0;
        OCKMWave a; a.set(kPosition, 0.f); start(a);
        auto sa = spectrum(run(a, ms(800), {on(0, 57)}).L, ms(300), N);
        const double h2 = db(at(sa, 2 * f0, N) / at(sa, f0, N)), h3 = db(at(sa, 3 * f0, N) / at(sa, f0, N));
        printf("      position 0%%: harmonic 2 %.1f dB, harmonic 3 %.1f dB\n", h2, h3);
        check(h2 < -40 && h3 < -40, "position 0% on Shapes is a pure sine");
        OCKMWave b; b.set(kPosition, 100.f); start(b);
        auto sb = spectrum(run(b, ms(800), {on(0, 57)}).L, ms(300), N);
        const double e2 = db(at(sb, 2 * f0, N) / at(sb, f0, N)), o3 = db(at(sb, 3 * f0, N) / at(sb, f0, N));
        printf("      position 100%%: harmonic 2 %.1f dB, harmonic 3 %.1f dB (square: -9.5)\n", e2, o3);
        check(e2 < -40 && o3 > -12 && o3 < -8, "position 100% on Shapes is a square (odd harmonics only)");
    }

    // 5. Moving Position 0 -> 100 % over 1 s gives no clicks.
    for (int table : {0, 1})
    {
        float staticMax = 0;
        for (float pos : {0.f, 20.f, 40.f, 60.f, 80.f, 100.f})
        {
            OCKMWave s; s.set(kTable, (float)table); s.set(kPosition, pos); start(s);
            Render r = run(s, ms(400), {on(0, 48)});
            staticMax = std::max(staticMax, maxStep(r.L, ms(100), ms(400)));
        }
        OCKMWave p; p.set(kTable, (float)table); p.set(kPosition, 0.f); start(p);
        Render r = run(p, ms(1300), {on(0, 48)}, 128, [&](int pos) { if (pos > ms(200)) p.set(kPosition, std::min(100.0f, (pos - ms(200)) / (float)ms(1000) * 100.f)); });
        const float sweepMax = maxStep(r.L, ms(100), ms(1300));
        printf("      %s sweep: largest sample step %.4f vs %.4f when still\n", kTableNames[table], sweepMax, staticMax);
        check(sweepMax < 1.1f * staticMax, table == 0 ? "Position sweep on Shapes: no clicks" : "Position sweep on Harmonics: no clicks");
    }

    // 6. Warp Amt 0 % sounds identical to Warp Off, for every mode.
    {
        OCKMWave ref; start(ref);
        Render rr = run(ref, ms(500), {on(0, 60), on(0, 67)});
        bool ok = true;
        for (int w = 1; w <= 4; ++w)
        {
            OCKMWave p; p.set(kWarp, (float)w); p.set(kWarpAmt, 0.f); start(p);
            Render r = run(p, ms(500), {on(0, 60), on(0, 67)});
            float d = 0; for (size_t i = 0; i < r.L.size(); ++i) d = std::max(d, std::fabs(r.L[i] - rr.L[i]));
            if (d > 1e-5f) { ok = false; printf("      warp %d differs by %g\n", w, d); }
        }
        check(ok, "Warp Amt 0% is identical to Warp Off (Sync, Bend, Mirror, Quantize)");
    }

    // 7. Unison: Spread 0 -> mono; 5 copies, Spread 100 -> wide.
    {
        OCKMWave a; a.set(kUnison, 5.f); a.set(kSpread, 0.f); start(a);
        Render ra = run(a, ms(500), {on(0, 60)});
        float d = 0; for (size_t i = 0; i < ra.L.size(); ++i) d = std::max(d, std::fabs(ra.L[i] - ra.R[i]));
        check(d < 1e-6f, "unison with Spread 0: left equals right");
        OCKMWave b; b.set(kUnison, 5.f); b.set(kSpread, 100.f); start(b);
        Render rb = run(b, ms(500), {on(0, 60)});
        std::vector<float> side(rb.L.size()); for (size_t i = 0; i < side.size(); ++i) side[i] = rb.L[i] - rb.R[i];
        const float wide = rms(side, ms(100), ms(500)) / rms(rb.L, ms(100), ms(500));
        printf("      Unison 5, Spread 100: side/left level %.2f\n", wide);
        check(wide > 0.3f, "unison 5 with Spread 100: left and right differ (stereo)");
        // Loudness stays about the same with more copies.
        OCKMWave c; start(c);
        const float one = rms(run(c, ms(500), {on(0, 60)}).L, ms(100), ms(500));
        printf("      Unison 1 vs 5 level: %.1f dB difference\n", db(rms(rb.L, ms(100), ms(500)) / one));
    }

    // 8. Voice limit: 8 held notes -> 6 voices, stealing without clicks.
    {
        OCKMWave p; p.set(kPosition, 0.f); start(p);
        std::vector<Ev> evs; for (int i = 0; i < 8; ++i) evs.push_back(on(ms(100) * i, 48 + 3 * i));
        int during = 0;
        Render r = run(p, ms(1100), evs, 128, [&](int pos) { if (pos > ms(1000) && !during) during = p.activeVoices(); });
        const float before = maxStep(r.L, ms(450), ms(590)), steal = maxStep(r.L, ms(595), ms(800));
        printf("      voices sounding: %d; largest step before stealing %.4f, while stealing %.4f\n", during, before, steal);
        check(during == kVoices, "8 held notes play 6 voices");
        check(steal < 2.0f * before + 0.01f, "voice stealing does not click");
    }

    // 9. Mono / Legato.
    {
        auto secondNote = [&](int mode) {
            OCKMWave p; p.set(kVoiceMode, (float)mode); p.set(kADec, 50.f); p.set(kASus, 30.f); p.set(kTable, 0.f); p.set(kPosition, 0.f); start(p);
            Render r = run(p, ms(700), {on(0, 60), on(ms(400), 64)});
            return peak(r, ms(401), ms(412)) / peak(r, ms(300), ms(390));   // just after the second note
        };
        const float legato = secondNote(kLegato), mono = secondNote(kMono);
        printf("      level after an overlapping note: Legato x%.2f, Mono x%.2f\n", legato, mono);
        check(legato < 1.2f, "Legato: overlapping notes do not retrigger the envelope");
        check(mono > 2.0f, "Mono: overlapping notes retrigger the envelope");
        OCKMWave g; g.set(kVoiceMode, (float)kLegato); g.set(kGlide, 100.f); start(g);
        float mid = 0;
        run(g, ms(900), {on(0, 60), on(ms(300), 72)}, 128, [&](int pos) { if (pos >= ms(330) && mid == 0) mid = g.voicePitch(0); });
        printf("      glide 100 ms, 60 -> 72: pitch %.2f after 30 ms, %.2f after 600 ms\n", mid, g.voicePitch(0));
        check(mid > 60.5f && mid < 71.5f && std::fabs(g.voicePitch(0) - 72.f) < 0.05f, "Glide slides and reaches the target pitch");
    }

    // 10. All-notes-off: silent within 1 s; all-sound-off within 20 ms.
    {
        OCKMWave p; start(p);
        std::vector<Ev> evs; for (int n : kChord) evs.push_back(on(0, n));
        evs.push_back(cc(ms(500), 123, 0));
        Render r = run(p, ms(1600), evs);
        check(peak(r, ms(1500), ms(1600)) < 1e-4f, "all-notes-off: silent within 1 s");
        OCKMWave q; q.setProgram(21); start(q);       // 22 Wide Pad, 2 s release
        std::vector<Ev> ev2; for (int n : kChord) ev2.push_back(on(0, n));
        ev2.push_back(cc(ms(1000), 120, 0));
        Render r2 = run(q, ms(1100), ev2);
        check(peak(r2, ms(1020), ms(1100)) < 1e-6f, "all-sound-off: silent within 20 ms, even with a long release");
    }

    // 11. CPU (desktop; the MPC One is roughly 20x slower).
    {
        const int len = ms(10000);
        auto pct = [&](const Render& r) { return 100.0 * r.seconds / (len / SR); };
        // Best of 3 fresh runs, so a busy computer doesn't fail the check.
        auto best = [&](std::function<void(OCKMWave&)> setup, const std::vector<Ev>& e) {
            double b = 1e9;
            for (int k = 0; k < 3; ++k) { OCKMWave p; setup(p); start(p); b = std::min(b, pct(run(p, len, e))); }
            return b;
        };
        auto prog = [](int i) { return [i](OCKMWave& p) { p.setProgram(i); }; };
        // Typical: a sustained 4-note chord of 18 Deep Chord (2 copies) and 17 House Stab (3 copies) held
        // down (its voices free themselves once they decay to silence); the higher of the two counts.
        std::vector<Ev> evs; for (int n : kChord) evs.push_back(on(0, n));
        const double deep = best(prog(17), evs), stab = best(prog(16), evs), pad = best(prog(21), evs);
        printf("      CPU (desktop): Deep Chord %.3f%%, House Stab %.3f%%, Wide Pad (5 copies) %.3f%%\n", deep, stab, pad);
        const double typical = std::max(deep, stab);
        auto worstSetup = [](OCKMWave& w) {
            w.set(kUnison, 5.f); w.set(kWarp, (float)kWarpMirror); w.set(kWarpAmt, 60.f); w.set(kLfoWarp, 50.f);
            w.set(kLfoPos, 50.f); w.set(kEnvPos, 50.f); w.set(kEnvCut, 24.f); w.set(kSub, 50.f); w.set(kLfoPitch, 20.f);
        };
        std::vector<Ev> e6; for (int i = 0; i < 6; ++i) e6.push_back(on(0, 40 + 5 * i));
        const double worst = best(worstSetup, e6);
        // The same, plus everything 1.2 added: 24 dB ladder, Drive, Fat and both step lanes.
        const double worst12 = best([&](OCKMWave& w) { worstSetup(w); w.set(kSlope, 1.f); w.set(kDrive, 50.f); w.set(kFat, 50.f);
                                                        w.set(kAAmt, 50.f); w.set(kBAmt, 50.f); }, e6);
        // A mono ladder bass (one copy: filtered once, not twice).
        const double ladBass = best(prog(48), {on(0, 36)});
        printf("      CPU (desktop): worst case + ladder, Drive, Fat, lanes %.3f%%; 49 Ladder Bass %.3f%%\n", worst12, ladBass);
        check(worst12 < 1.1, "CPU: worst case with every 1.2 feature under 1.1% on the desktop (about 22% on the MPC One)");
        printf("      CPU (desktop): typical 4-note chord %.3f%%, worst case 6 voices x 5 copies %.3f%% of one core\n", typical, worst);
        printf("      estimated MPC One (x20): typical %.1f%%, worst case %.1f%%\n", typical * 20, worst * 20);
        check(typical < 0.25, "CPU: typical preset under 0.25% on the desktop (under 5% on the MPC One)");
        check(worst < 0.75, "CPU: worst case under 0.75% on the desktop (under 15% on the MPC One)");
    }

    // 12. Demo: every preset in order. Poly presets play a chord stab pattern; mono ones a bass line.
    if (argc > 1)
    {
        std::vector<float> L, R;
        OCKMWave counter;
        for (int prog = 0; prog < counter.numPrograms(); ++prog)
        {
            OCKMWave p; p.setProgram(prog); start(p);
            const bool mono = p.getInt(kVoiceMode) != kPoly;
            const int beat = ms(60000.0 / 124.0);
            std::vector<Ev> evs;
            if (prog == 2) for (int i = 0; i <= 20; ++i) evs.push_back(cc(beat * 8 * i / 20, 1, i * 127 / 20 * (i <= 10) + (20 - i) * 127 / 10 * (i > 10)));
            if (mono)
            {
                const int line[8] = {36, 36, 43, 46, 36, 48, 46, 43};
                for (int i = 0; i < 8; ++i) { evs.push_back(on(beat * i, line[i] + 12, 100 + 27 * (i % 2 == 0))); evs.push_back(off(beat * i + beat * 9 / 10, line[i] + 12)); }
            }
            else
            {
                const bool pad = p.get(kAAtk) > 100.0f || prog == 2 || prog == 1 || prog == 7;
                if (pad) { for (int n : kChord) { evs.push_back(on(0, n + 12)); evs.push_back(off(beat * 7, n + 12)); } }
                else for (int s = 0; s < 4; ++s) for (int n : kChord) { evs.push_back(on(beat * 2 * s + (s % 2) * beat / 2, n + 12)); evs.push_back(off(beat * 2 * s + (s % 2) * beat / 2 + beat / 2, n + 12)); }
            }
            Render r = run(p, beat * 10, evs);
            L.insert(L.end(), r.L.begin(), r.L.end()); R.insert(R.end(), r.R.begin(), r.R.end());
        }
        writeWav(argv[1], L, R);
        printf("      wrote %s (%.0f s, presets 01-56 in order, about 4.8 s each)\n", argv[1], L.size() / SR);
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
