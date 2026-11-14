// Test harness for OCKM-03: includes the plugin source directly.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework -IOCKM-03/src OCKM-03/tests/harness.cpp -o OCKM-03/build/harness
//   OCKM-03/build/harness OCKM-03/build/demo-pattern.wav
#include "OCKM03.cpp"
#include <cstdio>
#include <vector>
struct Ev { int frame; int note; int vel; bool on; };
struct Out { std::vector<float> L; };
static std::vector<float> render(OCKM03& p, std::vector<Ev> evs, int frames, int block = 128)
{
    p.internalSetSampleRate(44100); p.internalSetBlockSize(block); p.internalResume();
    std::vector<float> L(frames), R(frames);
    std::vector<mpc::MidiEvent> mev;
    for (int pos = 0; pos < frames; pos += block)
    {
        int n = std::min(block, frames - pos);
        mev.clear();
        for (auto& e : evs) if (e.frame >= pos && e.frame < pos + n)
            mev.push_back(e.on ? mpc::MidiEvent::noteOn(0, e.note, e.vel, e.frame - pos) : mpc::MidiEvent::noteOff(0, e.note, e.frame - pos));
        float* outs[2] = {L.data() + pos, R.data() + pos};
        for (int i = 0; i < n; ++i) outs[0][i] = outs[1][i] = 0;
        mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = n; c.sampleRate = 44100;
        c.midi = mev.data(); c.numMidiEvents = (int)mev.size();
        p.process(c);
    }
    return L;
}
static float peak(const std::vector<float>& x, int a, int b) { float m = 0; for (int i = a; i < b && i < (int)x.size(); ++i) m = std::fmax(m, std::fabs(x[i])); return m; }
static float rms(const std::vector<float>& x, int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += x[i] * x[i]; return (float)std::sqrt(s / (b - a)); }
static void writeWav(const char* path, const std::vector<float>& x)
{
    FILE* f = fopen(path, "wb"); int n = (int)x.size(); int sr = 44100;
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 2); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32(sr); w32(sr * 2); w16(2); w16(16);
    fwrite("data", 1, 4, f); w32(n * 2);
    for (float v : x) w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767));
    fclose(f);
}
int main(int argc, char** argv)
{
    const int sr = 44100, step = sr * 60 / 125 / 4;   // 16ths at 125 BPM
    int fails = 0;
    auto check = [&](bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

    // 1. Slide: overlapping notes must not retrigger the envelope.
    {
        OCKM03 a; a.set(kReso, 0); a.set(kEnvMod, 80); a.set(kDecay, 20); a.set(kSlide, 0);
        // A: separate notes (retrigger). B: overlapping (slide).
        auto sep = render(a, {{0, 36, 80, true}, {step - 200, 36, 0, false}, {step, 48, 80, true}, {2 * step, 48, 0, false}}, 3 * step);
        OCKM03 b; b.set(kReso, 0); b.set(kEnvMod, 80); b.set(kDecay, 20); b.set(kSlide, 0);
        auto leg = render(b, {{0, 36, 80, true}, {step, 48, 80, true}, {step + 100, 36, 0, false}, {2 * step, 48, 0, false}}, 3 * step);
        // Brightness (HF energy / RMS) once the glide has settled: a retrigger restarts the filter env.
        auto bright = [](const std::vector<float>& x, int a, int b) { double h = 0, e = 0; for (int i = a; i < b; ++i) { float d = x[i] - x[i - 1]; h += d * d; e += x[i] * x[i]; } return (float)std::sqrt(h / e); };
        float bs = bright(sep, step + 1500, step + 3500), bl = bright(leg, step + 1500, step + 3500);
        printf("      brightness after 2nd note: separate %.4f, overlapping %.4f\n", bs, bl);
        check(bl < bs * 0.8f, "overlapping notes slide without retriggering the envelope");
        // Glide: zero-crossing period right after the slid note should be between the two pitches.
        int zc = 0; for (int i = step + 50; i < step + 1500; ++i) if (leg[i - 1] < 0 && leg[i] >= 0) ++zc;
        int zc2 = 0; for (int i = 2 * step - 1500; i < 2 * step - 50; ++i) if (leg[i - 1] < 0 && leg[i] >= 0) ++zc2;
        printf("      zero crossings early/late in slid note: %d / %d\n", zc, zc2);
        check(zc < zc2, "pitch glides up to the new note");
    }
    // 2. Accent: consecutive accents build up the filter.
    {
        OCKM03 p; p.set(kReso, 70); p.set(kAccent, 100); p.set(kDecay, 70); p.set(kCutoff, 25); p.set(kEnvMod, 30);
        std::vector<Ev> ev;
        for (int k = 0; k < 6; ++k) { ev.push_back({k * step, 36, 127, true}); ev.push_back({k * step + step / 2, 36, 0, false}); }
        auto x = render(p, ev, 7 * step);
        float hf[6];
        for (int k = 0; k < 6; ++k) { double s = 0; for (int i = k * step + 300; i < k * step + 3000; ++i) { float d = x[i] - x[i - 1]; s += d * d; } hf[k] = (float)std::sqrt(s); }
        printf("      high-frequency energy per accented note: %.3f %.3f %.3f %.3f %.3f %.3f\n", hf[0], hf[1], hf[2], hf[3], hf[4], hf[5]);
        check(hf[3] > hf[0] * 1.1f, "consecutive accents build up the filter sweep");
        OCKM03 q; q.set(kReso, 70); q.set(kAccent, 100);
        auto y = render(q, {{0, 36, 99, true}, {step / 2, 36, 0, false}, {step, 36, 100, true}, {step + step / 2, 36, 0, false}}, 2 * step);
        printf("      peak velocity 99: %.3f, velocity 100: %.3f\n", peak(y, 0, step / 2), peak(y, step, step + step / 2));
        check(peak(y, step, step + step / 2) > peak(y, 0, step / 2) * 1.3f, "velocity 100 accents, 99 does not (threshold)");
    }
    // 3. Retired macro parameters (v1.1): no effect on the sound at any value, hidden from automation.
    std::vector<Ev> pattern;
    {
        const int notes[16] = {36, 36, 48, 36, 39, 36, 46, 48, 36, 43, 36, 48, 41, 36, 51, 48};
        const int vels[16]  = {127, 80, 80, 127, 80, 80, 127, 80, 80, 127, 80, 80, 127, 127, 80, 80};
        const bool slide[16] = {0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0};
        for (int bar = 0; bar < 4; ++bar)
            for (int k = 0; k < 16; ++k)
            {
                int t = (bar * 16 + k) * step;
                int len = slide[k] ? step + 300 : step / 2;
                pattern.push_back({t, notes[k], vels[k], true});
                pattern.push_back({t + len, notes[k], 0, false});
            }
        std::sort(pattern.begin(), pattern.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame || (a.frame == b.frame && !a.on && b.on); });
        OCKM03 a; auto base = render(a, pattern, 64 * step);
        OCKM03 b; for (int i = kSquelch; i <= kGrRes; ++i) b.set(i, (i % 2) ? b.param(i).max : b.param(i).min);
        auto old = render(b, pattern, 64 * step);
        bool same = true; for (size_t i = 0; i < base.size(); ++i) if (base[i] != old[i]) { same = false; break; }
        check(same, "retired macro parameters at any value leave the sound bit-identical");
        bool hidden = true; for (int i = kSquelch; i <= kGrRes; ++i) hidden = hidden && !a.param(i).automatable;
        bool live = true;   for (int i = 0; i < kSquelch; ++i) live = live && a.param(i).automatable;
        check(hidden && live && a.numParams() == 26, "26 parameters kept; only the 11 classic ones are automatable");
    }
    // 3b. Arp: every note at full velocity (all accented). The classic knobs must still work.
    {
        std::vector<Ev> arp; const int an[4] = {36, 43, 48, 39};
        for (int k = 0; k < 32; ++k) { arp.push_back({k * step, an[k % 4], 127, true}); arp.push_back({k * step + step / 2, an[k % 4], 0, false}); }
        auto bright = [](const std::vector<float>& x, int a, int b) { double h = 0, e = 0; for (int i = a + 1; i < b; ++i) { float d = x[i] - x[i - 1]; h += d * d; e += x[i] * x[i]; } return e > 0 ? (float)std::sqrt(h / e) : 0.f; };
        // Brightness 40-60 ms into each note (after the attack), averaged over notes 9-32.
        auto late = [&](int idx, float v) { OCKM03 p; p.setProgram(1); p.set(idx, v); auto x = render(p, arp, 32 * step);
            double s = 0; for (int k = 8; k < 32; ++k) s += bright(x, k * step + 1760, k * step + 2640); return (float)(s / 24); };
        float d0 = late(kDecay, 0), d100 = late(kDecay, 100);
        float e25 = late(kEnvMod, 25), e75 = late(kEnvMod, 75);
        float c25 = late(kCutoff, 25), c50 = late(kCutoff, 50);
        printf("      arp @127, Classic Squelch: Decay 0/100 %.3f/%.3f  EnvMod 25/75 %.3f/%.3f  Cutoff 25/50 %.3f/%.3f\n", d0, d100, e25, e75, c25, c50);
        check(d100 > d0 * 1.8f, "arp at full velocity: Decay still shapes accented notes");
        check(e75 > e25 * 1.5f, "arp at full velocity: Env Mod still opens the filter");
        check(c50 > c25 * 1.5f, "arp at full velocity: Cutoff still opens the filter");
        auto crest = [&](float drive) { OCKM03 p; p.setProgram(1); p.set(kDrive, drive); auto x = render(p, arp, 32 * step);
            return peak(x, 0, (int)x.size()) / rms(x, 0, (int)x.size()); };
        float k25 = crest(25), k50 = crest(50), k100 = crest(100);
        printf("      arp @127, Drive 25/50/100 crest factor %.2f/%.2f/%.2f\n", k25, k50, k100);
        check(k25 > k50 * 1.3f && k50 > k100 * 1.15f, "Drive keeps changing the sound across its whole travel");
    }
    // 4. Demo: each preset playing the pattern, Cutoff swept up and back in bars 3-4.
    {
        std::vector<float> all;
        for (int prog = 0; prog < 4; ++prog)
        {
            OCKM03 p; p.setProgram(prog);
            p.internalSetSampleRate(44100); p.internalSetBlockSize(128); p.internalResume();
            std::vector<float> L(64 * step), R(64 * step); std::vector<mpc::MidiEvent> mev;
            for (int pos = 0; pos < (int)L.size(); pos += 128)
            {
                int n = std::min(128, (int)L.size() - pos);
                if (pos >= 32 * step)
                {
                    static float base; if (pos < 32 * step + 128) base = p.getNormalised(kCutoff);
                    float ph = (pos - 32 * step) / (32.f * step);              // 0..1 over bars 3-4
                    p.setNormalised(kCutoff, std::fmin(1.f, base + 0.35f * std::sin(3.14159265f * ph)));
                }
                mev.clear();
                for (auto& e : pattern) if (e.frame >= pos && e.frame < pos + n)
                    mev.push_back(e.on ? mpc::MidiEvent::noteOn(0, e.note, e.vel, e.frame - pos) : mpc::MidiEvent::noteOff(0, e.note, e.frame - pos));
                float* outs[2] = {L.data() + pos, R.data() + pos};
                for (int i = 0; i < n; ++i) outs[0][i] = outs[1][i] = 0;
                mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = n; c.sampleRate = 44100;
                c.midi = mev.data(); c.numMidiEvents = (int)mev.size(); p.process(c);
            }
            printf("      preset %-16s peak %.2f dBFS\n", p.programName(prog), 20 * std::log10(peak(L, 0, (int)L.size()) + 1e-9));
            all.insert(all.end(), L.begin(), L.end());
        }
        writeWav(argv[1], all);
        float pk = peak(all, 0, (int)all.size());
        check(pk < 1.0f, "demo renders without clipping");
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
