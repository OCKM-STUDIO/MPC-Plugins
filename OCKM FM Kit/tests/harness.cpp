// Test harness for OCKM FM Kit: includes the plugin source directly and checks what SPEC.md promises.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM FM Kit/src" "OCKM FM Kit/tests/harness.cpp" -o "OCKM FM Kit/build/harness"
//   "OCKM FM Kit/build/harness" "OCKM FM Kit/build/demo.wav"
#include "OCKMFMKit.cpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace fmkit;

static const double SR = 44100.0;
static const int kPrograms = OCKMFMKit().numPrograms();

struct Hit { int at; int note; int vel; };
struct Out { std::vector<float> ch[4]; };

// Play `hits` (sorted by time) through the plugin. numOut = 2 or 4. Extra MIDI (CC) can be given as note < 0: CC 120.
static Out run(OCKMFMKit& p, int frames, std::vector<Hit> hits, int numOut = 4, int block = 128)
{
    p.internalSetSampleRate(SR); p.internalSetBlockSize(block); p.internalResume();
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
    Out o; for (int c = 0; c < 4; ++c) o.ch[c].assign((size_t)frames, 0.0f);
    size_t h = 0;
    std::vector<mpc::MidiEvent> ev;
    for (int pos = 0; pos < frames; pos += block)
    {
        const int n = std::min(block, frames - pos);
        ev.clear();
        while (h < hits.size() && hits[h].at < pos + n)
        {
            const Hit& x = hits[h++];
            ev.push_back(x.note >= 0 ? mpc::MidiEvent::noteOn(0, x.note, x.vel, x.at - pos)
                                     : mpc::MidiEvent::cc(0, 120, 0, x.at - pos));
        }
        float* outs[4]; for (int c = 0; c < 4; ++c) { outs[c] = o.ch[c].data() + pos; std::fill(outs[c], outs[c] + n, 0.0f); }
        mpc::ProcessContext c;
        c.inputs = nullptr; c.outputs = outs; c.numInputs = 0; c.numOutputs = numOut; c.numFrames = n; c.sampleRate = SR;
        c.midi = ev.data(); c.numMidiEvents = (int)ev.size();
        p.process(c);
    }
    return o;
}

static float peak(const std::vector<float>& x, int a = 0, int b = -1)
{ if (b < 0) b = (int)x.size(); float m = 0; for (int i = a; i < b; ++i) m = std::fmax(m, std::fabs(x[(size_t)i])); return m; }
static float peakAll(const Out& o, int a = 0, int b = -1) { float m = 0; for (auto& c : o.ch) m = std::fmax(m, peak(c, a, b)); return m; }
static double energy(const std::vector<float>& x, int a = 0, int b = -1)
{ if (b < 0) b = (int)x.size(); double s = 0; for (int i = a; i < b; ++i) s += (double)x[(size_t)i] * x[(size_t)i]; return s; }
static double energyBus(const Out& o, int bus) { return energy(o.ch[bus * 2]) + energy(o.ch[bus * 2 + 1]); }
static float db(double x) { return x > 0 ? 20.0f * (float)std::log10(x) : -200.0f; }
static int ms(double m) { return (int)(m * 0.001 * SR); }

// Brightness of a mono signal: the "average frequency" sqrt(energy of the slope / energy), in Hz.
static float brightness(const std::vector<float>& x, int a = 0, int b = -1)
{
    if (b < 0) b = (int)x.size();
    double e = 0, de = 0; for (int i = a + 1; i < b; ++i) { e += (double)x[i] * x[i]; const double s1 = x[i] - x[i - 1]; de += s1 * s1; }
    return (float)(std::sqrt(de / std::fmax(e, 1e-30)) * SR / 6.2831853);
}

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); return; }
    int n = (int)L.size(); int sr = (int)SR;
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 4); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(sr); w32(sr * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(n * 4);
    for (int i = 0; i < n; ++i) { w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, L[i])) * 32767)); w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, R[i])) * 32767)); }
    fclose(f);
}

// A 4-bar house/techno groove at `bpm`, starting at sample `start`.
static void groove(std::vector<Hit>& hits, int start, double bpm)
{
    const double s16 = SR * 60.0 / bpm / 4.0;
    for (int bar = 0; bar < 4; ++bar)
        for (int s = 0; s < 16; ++s)
        {
            const int t = start + (int)((bar * 16 + s) * s16);
            if (s % 4 == 0) hits.push_back({t, 36, 120});                       // four to the floor
            if (s == 4 || s == 12) hits.push_back({t, 38, 110});                // clap on 2 and 4
            if (s % 4 == 2) hits.push_back({t, 40, 100});                       // open hat on the offbeat
            else hits.push_back({t, 39, s % 2 ? 70 : 90});                      // 16th closed hats (choke it)
            if (bar % 2 == 1 && (s == 7 || s == 15)) hits.push_back({t, 37, 60});   // ghost snare
            if (s == 3 || s == 10 || (bar == 2 && s == 14)) hits.push_back({t, 43, 85});   // perc
            if (bar == 3 && s >= 12) hits.push_back({t, s < 14 ? 42 : 41, 100});  // tom fill
        }
}

int main(int argc, char** argv)
{
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };
    static const char* names[kNumDrums] = {"Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Low Tom", "High Tom", "Perc"};

    // 1. Every drum sounds, and stops by itself (one-shot, even at maximum Decay).
    {
        bool ok = true;
        for (int d = 0; d < kNumDrums; ++d)
        {
            OCKMFMKit p; p.setProgram(0);
            p.set(paramIndex(d, kDecay), 100.f);
            auto o = run(p, (int)(SR * 3.0), {{0, 36 + d, 127}});
            const float pk = peakAll(o), end = peakAll(o, (int)(SR * 2.9));
            printf("      %-10s peak %6.1f dBFS, last 100 ms %7.1f dBFS\n", names[d], db(pk), db(end));
            if (pk < 0.05f || end > 1e-4f) ok = false;
        }
        check(ok, "every drum (notes 36-43) sounds, and dies away by itself");
    }
    // 2. Wrapping: other notes play the same drum as their wrapped note, identically.
    {
        bool ok = true;
        const int pairs[][2] = {{44, 36}, {35, 43}, {60, 36}, {127, 39}, {0, 40}, {49, 41}};
        for (auto& pr : pairs)
        {
            OCKMFMKit a, b; a.setProgram(0); b.setProgram(0);
            auto oa = run(a, ms(300), {{10, pr[0], 100}}), ob = run(b, ms(300), {{10, pr[1], 100}});
            if (oa.ch[0] != ob.ch[0] || oa.ch[1] != ob.ch[1] || peakAll(oa) < 0.01f) ok = false;
        }
        check(ok, "wrapping: 44=Kick, 35=Perc, 60=Kick, 127=Closed Hat, 0=Open Hat, 49=Low Tom");
    }
    // 3. Choke: a closed hat (here at Level 0, so only the choke is heard) cuts the open hat, without a click.
    {
        OCKMFMKit p; p.setProgram(0); p.set(paramIndex(kOpenHat, kDecay), 100.f); p.set(paramIndex(kClosedHat, kLevel), 0.f);
        const int T = ms(200);
        auto o = run(p, ms(400), {{0, 40, 127}, {T, 39, 127}});
        OCKMFMKit q; q.setProgram(0); q.set(paramIndex(kOpenHat, kDecay), 100.f);
        auto u = run(q, ms(400), {{0, 40, 127}});
        const double after = energy(o.ch[1], T + ms(10), T + ms(60)), unchoked = energy(u.ch[1], T + ms(10), T + ms(60));
        const float prev = o.ch[1][(size_t)T - 1], now = o.ch[1][(size_t)T];
        printf("      open hat 10-60 ms after the choke: %.1f dB below the unchoked hat; sample before/at choke %.4f / %.4f\n",
               db(std::sqrt(unchoked / std::fmax(after, 1e-30))), prev, now);
        check(after < unchoked * 1e-4, "choke: closed hat cuts the open hat (more than 40 dB down within 10 ms)");
        check(std::fabs(now - prev) <= std::fabs(prev) * 0.15f + 1e-4f, "choke: no step to zero (the cut fades over ~1.5 ms)");
    }
    // 4. Retriggering a ringing kick doesn't click: the jump at the new hit is no bigger than the new hit's own slope.
    {
        OCKMFMKit p; p.setProgram(0); p.set(paramIndex(kKick, kDecay), 100.f);
        const int T = ms(137);
        auto o = run(p, ms(300), {{0, 36, 127}, {T, 36, 127}});
        const auto& x = o.ch[0];
        const float jump = std::fabs(x[(size_t)T] - x[(size_t)T - 1]);
        float slope = 0; for (int i = T + 1; i < T + ms(3); ++i) slope = std::fmax(slope, std::fabs(x[(size_t)i] - x[(size_t)i - 1]));
        float body = 0; for (int i = T - ms(20); i < T; ++i) body = std::fmax(body, std::fabs(x[(size_t)i] - x[(size_t)i - 1]));
        printf("      kick retrigger: jump %.4f, ringing body slope %.4f, new hit slope %.4f\n", jump, body, slope);
        check(jump <= std::fmax(slope, body) * 1.05f, "retriggered kick has no click");
    }
    // 5. No clipping: every preset, all eight drums at velocity 127 on every 16th, plus the groove.
    {
        bool ok = true;
        for (int prog = 0; prog < kPrograms; ++prog)
        {
            OCKMFMKit p; p.setProgram(prog);
            std::vector<Hit> dense;
            for (int s = 0; s < 32; ++s) for (int d = 0; d < kNumDrums; ++d) dense.push_back({s * 5000, 36 + d, 127});
            const float pkDense = peakAll(run(p, 32 * 5000 + ms(500), dense));
            std::vector<Hit> g; groove(g, 0, 124.0);
            OCKMFMKit q; q.setProgram(prog);
            const float pkGroove = peakAll(run(q, (int)(SR * 8.5), g));
            printf("      %-18s all drums at once %6.2f dBFS, groove %6.2f dBFS\n", p.programName(prog), db(pkDense), db(pkGroove));
            if (pkDense >= 1.0f || pkGroove >= 0.9f) ok = false;
        }
        OCKMFMKit p; p.setProgram(0); p.set(kMaster, 6.f);
        for (int d = 0; d < kNumDrums; ++d) { p.set(paramIndex(d, kLevel), 100.f); p.set(paramIndex(d, kTone), 100.f); }
        std::vector<Hit> all; for (int d = 0; d < kNumDrums; ++d) all.push_back({0, 36 + d, 127});
        p.set(kDrive, 100.f); p.set(kVelMorph, 100.f); p.set(kVariation, 100.f);
        const float worst = peakAll(run(p, ms(500), all));
        printf("      worst case (all levels, Tone, Drive and Master at max, all 8 together): %.2f dBFS\n", db(worst));
        check(ok && worst < 1.0f, "no clipping on any preset; the groove stays under -0.9 dBFS; worst case under 0 dBFS");
    }
    // 6. Routing: each drum lands on the right pair; with only 2 outputs everything is on Main.
    {
        bool ok = true;
        // expected bus per routing, per drum
        const int expect[4][kNumDrums] = {{0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0},
                                          {0, 0, 0, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 1, 1, 1}};
        for (int r = 0; r < 4; ++r)
            for (int d = 0; d < kNumDrums; ++d)
            {
                OCKMFMKit p; p.setProgram(0); p.set(kRouting, (float)r);
                auto o = run(p, ms(200), {{0, 36 + d, 110}});
                const int want = expect[r][d];
                if (energyBus(o, want) <= 0 || energyBus(o, 1 - want) != 0) ok = false;
                OCKMFMKit q; q.setProgram(0); q.set(kRouting, (float)r);
                auto o2 = run(q, ms(200), {{0, 36 + d, 110}}, 2);
                if (energyBus(o2, 0) <= 0) ok = false;
            }
        check(ok, "routing: All Main / Kick>Aux / Hats>Aux / Split send each drum to the right pair; 2-out host gets everything on Main");
    }
    // 7. Master: -40 dB is much quieter; nudging parameter 0 has no other effect.
    {
        OCKMFMKit a; a.setProgram(0);
        OCKMFMKit b; b.setProgram(0); b.set(kMaster, -40.f);
        const float pa = peakAll(run(a, ms(300), {{0, 36, 100}})), pb = peakAll(run(b, ms(300), {{0, 36, 100}}));
        check(db(pa) - db(pb) > 36.0f, "Master at -40 dB is about 37 dB quieter than the default -3 dB");
        OCKMFMKit c; c.setProgram(0); c.set(kMaster, 6.f); c.set(kMaster, 1.f); c.set(kMaster, -3.f);
        auto oa = run(a, ms(300), {{0, 37, 100}}), oc = run(c, ms(300), {{0, 37, 100}});
        check(oa.ch[0] == oc.ch[0], "nudging parameter 0 (Master) and back changes nothing else");
    }
    // 8. No NaN or denormals; CC 120 silences everything.
    {
        OCKMFMKit p; p.setProgram(4);
        std::vector<Hit> g, early; groove(g, 0, 128.0);
        for (auto& h : g) if (h.at < (int)(SR * 4)) early.push_back(h);
        g = early; g.push_back({(int)(SR * 4), -1, 0});
        auto o = run(p, (int)(SR * 4.5), g);
        bool finite = true, denorm = false;
        for (auto& c : o.ch) for (float v : c) { if (!std::isfinite(v)) finite = false; if (v != 0.0f && std::fabs(v) < 1e-30f) denorm = true; }
        check(finite && !denorm, "no NaN, Inf or denormal samples");
        check(peakAll(o, (int)(SR * 4) + ms(5)) < 1e-4f, "CC 120 (all sound off) silences every drum within 5 ms");
    }
    // 9. Tone changes every drum; Morph changes every drum a lot (brightness and/or length move by 1.5x or more).
    {
        bool okTone = true;
        for (int d = 0; d < kNumDrums; ++d)
        {
            OCKMFMKit a, b; a.setProgram(0); b.setProgram(0);
            a.set(paramIndex(d, kTone), 0.f); b.set(paramIndex(d, kTone), 100.f);
            auto oa = run(a, ms(400), {{0, 36 + d, 110}}), ob = run(b, ms(400), {{0, 36 + d, 110}});
            double diff = 0; for (size_t i = 0; i < oa.ch[0].size(); ++i) { const double e = oa.ch[0][i] - ob.ch[0][i]; diff += e * e; }
            if (diff < std::fmax(energy(oa.ch[0]), energy(ob.ch[0])) * 0.05) { okTone = false; printf("      %s: Tone makes little difference\n", names[d]); }
        }
        check(okTone, "Tone changes the sound of every drum");

        bool okMorph = true;
        for (int d = 0; d < kNumDrums; ++d)
        {
            float bright[3], len[3];
            for (int k = 0; k < 3; ++k)
            {
                OCKMFMKit p; p.setProgram(0); p.set(paramIndex(d, kChar), 50.f * k);
                auto o = run(p, (int)(SR * 2.0), {{0, 36 + d, 110}});
                const auto& x = o.ch[0];
                // brightness: the "average frequency" sqrt(energy of the slope / energy), in Hz
                double e = 0, de = 0; for (size_t i = 1; i < x.size(); ++i) { e += (double)x[i] * x[i]; const double s1 = x[i] - x[i - 1]; de += s1 * s1; }
                bright[k] = (float)(std::sqrt(de / std::fmax(e, 1e-30)) * SR / 6.2831853);
                // length: time until the level stays 40 dB below its peak
                const float pk = peak(x); int last = 0; for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > pk * 0.01f) last = (int)i;
                len[k] = (float)(last / SR * 1000.0);
            }
            const float br = std::fmax(bright[0], bright[2]) / std::fmin(bright[0], bright[2]);
            const float lr = std::fmax(len[0], len[2]) / std::fmin(len[0], len[2]);
            printf("      %-10s Morph 0/50/100%%: brightness %5.0f / %5.0f / %5.0f Hz, length %4.0f / %4.0f / %4.0f ms\n",
                   names[d], bright[0], bright[1], bright[2], len[0], len[1], len[2]);
            if (br < 1.5f && lr < 1.5f) okMorph = false;
        }
        check(okMorph, "Morph changes every drum's character a lot (0% vs 100%: brightness or length 1.5x or more)");
        OCKMFMKit a, b; a.setProgram(0); b.setProgram(0);
        const float soft = peakAll(run(a, ms(300), {{0, 36, 1}})), hard = peakAll(run(b, ms(300), {{0, 36, 127}}));
        printf("      kick at velocity 1 is %.1f dB below velocity 127\n", db(hard) - db(soft));
        check(db(hard) - db(soft) > 8.0f, "velocity: soft hits are quieter");
    }

    // 10. v1.2 kit-wide knobs: Variation, Vel>Morph, Drive.
    {
        // Variation 0: repeated hits are identical. Variation 100: they differ, but stay recognisably the same drum.
        auto repeats = [&](float var, double& spread) {
            OCKMFMKit p; p.setProgram(0); p.set(kVariation, var);
            const int gap = ms(250);
            std::vector<Hit> h; for (int k = 0; k < 8; ++k) h.push_back({k * gap, 39, 100});
            auto o = run(p, 8 * gap, h);
            float lo = 1e9f, hi = 0; bool same = true;
            for (int k = 0; k < 8; ++k)
            {
                const float b = brightness(o.ch[0], k * gap, k * gap + ms(60)); lo = std::fmin(lo, b); hi = std::fmax(hi, b);
                for (int i = 0; i < ms(60); ++i) if (o.ch[0][(size_t)(k * gap + i)] != o.ch[0][(size_t)i]) { same = false; break; }
            }
            spread = hi / lo; return same;
        };
        double s0, s1; const bool same0 = repeats(0.f, s0), same1 = repeats(100.f, s1);
        printf("      closed hat x8: brightness spread %.3fx at Variation 0, %.3fx at 100\n", s0, s1);
        check(same0 && !same1 && s1 < 1.6, "Variation: 0% repeats hits exactly; 100% makes every hit a little different");

        // Vel>Morph: with it up, a hard snare is brighter than a soft one by much more than with it off.
        auto velRatio = [&](float amt) {
            OCKMFMKit a, b; a.setProgram(0); b.setProgram(0); a.set(kVelMorph, amt); b.set(kVelMorph, amt);
            const float hard = brightness(run(a, ms(300), {{0, 37, 127}}).ch[0]), soft = brightness(run(b, ms(300), {{0, 37, 60}}).ch[0]);
            return hard / soft;
        };
        const float r0 = velRatio(0.f), r1 = velRatio(100.f);
        printf("      snare hard/soft brightness: %.2fx with Vel>Morph 0, %.2fx with 100\n", r0, r1);
        check(r1 > r0 * 1.3f, "Vel>Morph: harder hits move further along Morph");

        // Drive: fatter (more energy) on every drum, peaks never higher than without it.
        bool okDrive = true;
        for (int d = 0; d < kNumDrums; ++d)
        {
            OCKMFMKit a, b; a.setProgram(0); b.setProgram(0); b.set(kDrive, 100.f);
            auto oa = run(a, ms(400), {{0, 36 + d, 110}}), ob = run(b, ms(400), {{0, 36 + d, 110}});
            const float gainDb = db(std::sqrt(energy(ob.ch[0]) / energy(oa.ch[0])));
            const float pkDb = db(peakAll(ob)) - db(peakAll(oa));
            printf("      %-10s Drive 100%%: %+5.1f dB louder on average, peak %+5.2f dB\n", names[d], gainDb, pkDb);
            if (gainDb < 1.5f || pkDb > 5.0f) okDrive = false;
        }
        check(okDrive, "Drive: every drum gets fatter (1.5 dB+ more energy) while its peak moves less than 5 dB");
    }

    // Demo: the groove through every preset, 4 bars each at 124 BPM.
    if (argc > 1)
    {
        const double bpm = 124.0;
        const int bars4 = (int)(SR * 60.0 / bpm * 16.0);
        std::vector<float> L, R;
        for (int prog = 0; prog < kPrograms; ++prog)
        {
            OCKMFMKit p; p.setProgram(prog);
            std::vector<Hit> g; groove(g, 0, bpm);
            auto o = run(p, bars4, g, 2);
            L.insert(L.end(), o.ch[0].begin(), o.ch[0].end());
            R.insert(R.end(), o.ch[1].begin(), o.ch[1].end());
        }
        writeWav(argv[1], L, R);
        printf("      wrote %s (%.1f s: the 8 presets in order, 4 bars each at 124 BPM)\n", argv[1], L.size() / SR);
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
