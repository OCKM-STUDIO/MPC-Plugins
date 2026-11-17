// Test harness for OCKM-03: includes the plugin source directly.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework -IOCKM-03/src OCKM-03/tests/harness.cpp -o OCKM-03/build/harness
//   OCKM-03/build/harness OCKM-03/build/demo-pattern.wav
#include "OCKM03.cpp"
#include <cstdio>
#include <vector>
struct Ev { int frame; int note; int vel; bool on; };
struct Out { std::vector<float> L; };
// When true, render() gives the plugin a running MPC transport (125 BPM, 4/4, from bar 1).
static bool gTransport = false;
static double gBpm = 125.0;
static void fillTransport(mpc::ProcessContext& c, int pos)
{
    if (!gTransport) return;
    auto& t = c.transport; t.valid = t.playing = t.ppqValid = t.tempoValid = true; t.bpm = gBpm;
    t.ppqPos = pos * gBpm / 60.0 / 44100.0; t.barStartPpq = std::floor(t.ppqPos / 4.0) * 4.0;
    t.timeSigNumerator = 4; t.timeSigDenominator = 4;
}
// Goertzel magnitude of frequency f over x[a..b).
static float tone(const std::vector<float>& x, int a, int b, double f)
{
    const double w = 2.0 * 3.14159265358979 * f / 44100.0, k = 2.0 * std::cos(w); double s1 = 0, s2 = 0;
    for (int i = a; i < b; ++i) { double s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
    return (float)std::sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / (b - a);
}
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
        c.midi = mev.data(); c.numMidiEvents = (int)mev.size(); fillTransport(c, pos);
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
        OCKM03 a; a.set(kReso, 0); a.set(kEnvMod, 80); a.set(kDecay, 20); a.set(kSlide, 0); a.set(kCutoff, 55);
        // A: separate notes (retrigger). B: overlapping (slide).
        auto sep = render(a, {{0, 36, 80, true}, {step - 200, 36, 0, false}, {step, 48, 80, true}, {2 * step, 48, 0, false}}, 3 * step);
        OCKM03 b; b.set(kReso, 0); b.set(kEnvMod, 80); b.set(kDecay, 20); b.set(kSlide, 0); b.set(kCutoff, 55);
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
        bool mut = true; for (int i = kMuOct; i <= kMuVar; ++i) mut = mut && a.param(i).automatable;
        bool pat = true; for (int i = kPtMode; i <= kFullBass; ++i) pat = pat && a.param(i).automatable;
        check(hidden && live && mut && pat && a.param(kEcho).automatable && a.numParams() == 42,
              "42 parameters: the 11 classic, 4 Mutate, Echo, 8 Pattern and 3 Grit are automatable, 15 retired are not");
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
    // 5. v1.2 Mutate, key tracking, low cut.
    {
        // 5a. Decisions: nested amounts, Variation matters, no degenerate seeds.
        bool nested = true, varDiffers = false, sane = true;
        for (int var = 1; var <= 64; ++var)
            for (int st = 0; st < 16; ++st)
                for (uint32_t lane = 1; lane <= 3; ++lane)
                    if (Mutator::uniform(st, var, lane) < 0.3f && !(Mutator::uniform(st, var, lane) < 0.6f)) nested = false;
        for (int st = 0; st < 16; ++st) if ((Mutator::uniform(st, 1, 1) < 0.5f) != (Mutator::uniform(st, 2, 1) < 0.5f)) varDiffers = true;
        for (int var = 1; var <= 8; ++var)
            for (uint32_t lane = 1; lane <= 3; ++lane)
            { int c = 0, q = 0; for (int st = 0; st < 16; ++st) { c += Mutator::uniform(st, var, lane) < 0.5f; q += Mutator::uniform(st, var, lane) < 0.25f; }
              if (c != 8 || q != 4) sane = false; }
        check(nested, "Mutate amounts are nested: every step mutated at 30% is mutated at 60%");
        check(varDiffers, "a different Variation gives a different set");
        check(sane, "amounts are exact: 25% mutates 4 and 50% mutates 8 of 16 steps (Variations 1-8)");

        // Shared 1-bar loop: 16 sixteenths of MIDI 36, velocity 80, half-step gate, 4 bars, MPC transport running.
        std::vector<Ev> loop;
        for (int k = 0; k < 64; ++k) { loop.push_back({k * step, 36, 80, true}); loop.push_back({k * step + step / 2, 36, 0, false}); }
        auto plain = [](OCKM03& p) { p.set(kReso, 0); p.set(kEnvMod, 0); p.set(kCutoff, 55); p.set(kAccent, 0); };

        // 5b. Mutate at 0: Variation makes no difference (the programmed notes play unchanged).
        gTransport = true;
        { OCKM03 a, b; b.set(kMuVar, 37); auto x = render(a, loop, 64 * step), y = render(b, loop, 64 * step);
          bool same = true; for (size_t i = 0; i < x.size(); ++i) if (x[i] != y[i]) { same = false; break; }
          check(same, "Mutate at 0: output identical whatever the Variation"); }

        // 5c. Rests repeat every bar, on the steps the Mutator predicts.
        { OCKM03 p; plain(p); p.set(kMuRst, 40); p.set(kMuVar, 5); auto x = render(p, loop, 64 * step);
          bool repeat = true, predicted = true; int rests = 0;
          for (int bar = 1; bar < 4; ++bar)
              for (int st = 0; st < 16; ++st)
              {
                  int k = bar * 16 + st; bool silent = rms(x, k * step + 300, k * step + step / 2) < 1e-3f;
                  bool want = Mutator::decide(st, 5, 0, 0, 0.4f).rest;
                  if (silent != want) predicted = false;
                  bool silent1 = rms(x, (16 + st) * step + 300, (16 + st) * step + step / 2) < 1e-3f;
                  if (silent != silent1) repeat = false;
                  if (bar == 1 && silent) ++rests;
              }
          printf("      Add Rest 40%%, Variation 5: %d of 16 steps rest\n", rests);
          check(repeat && rests > 0 && rests < 16, "Add Rest: the same steps rest in every bar");
          check(predicted, "Add Rest follows the step in the bar from the MPC's playhead"); }

        // 5d. Octave jumps repeat every bar, on the predicted steps (pitch from zero crossings).
        { OCKM03 p; plain(p); p.set(kMuOct, 50); p.set(kMuVar, 9); auto x = render(p, loop, 64 * step);
          auto zc = [&](int k) { int c = 0; for (int i = k * step + 900; i < k * step + step / 2; ++i) if (x[i - 1] < 0 && x[i] >= 0) ++c; return c; };
          bool ok = true; int jumps = 0;
          for (int bar = 1; bar < 4; ++bar)
              for (int st = 0; st < 16; ++st)
              {
                  bool up = zc(bar * 16 + st) >= 4;    // about 2.6 cycles at C1, 5.2 at C2 in this window
                  if (up != Mutator::decide(st, 9, 0.5f, 0, 0).octave) ok = false;
                  if (bar == 1 && up) ++jumps;
              }
          printf("      Oct Jump 50%%, Variation 9: %d of 16 steps jump\n", jumps);
          check(ok && jumps > 0 && jumps < 16, "Oct Jump: the predicted steps jump an octave, the same in every bar"); }

        // 5e. No hanging notes: Oct Jump 100% with overlapping (slid) notes, then silence.
        { OCKM03 p; p.set(kMuOct, 100); p.set(kMuSld, 100);
          std::vector<Ev> ev = {{0, 36, 80, true}, {step, 43, 80, true}, {step + 200, 36, 0, false}, {2 * step, 48, 120, true},
                                {2 * step + 100, 43, 0, false}, {3 * step, 48, 0, false}};
          auto x = render(p, ev, 6 * step);
          check(peak(x, 5 * step, 6 * step) < 1e-4f, "Oct Jump 100%: every note-off still ends its (jumped) note"); }
        gTransport = false;

        // 5f. Slide in after a released note: pitch glides and the filter is not re-plucked.
        { auto run = [&](float sld) { OCKM03 p; p.set(kReso, 0); p.set(kEnvMod, 80); p.set(kDecay, 20); p.set(kSlide, 30); p.set(kMuSld, sld);
              return render(p, {{0, 36, 80, true}, {step / 2, 36, 0, false}, {step, 48, 80, true}, {2 * step, 48, 0, false}}, 3 * step); };
          auto pluck = run(0), slid = run(100);
          auto bright = [](const std::vector<float>& x, int a, int b) { double h = 0, e = 0; for (int i = a; i < b; ++i) { float d = x[i] - x[i - 1]; h += d * d; e += x[i] * x[i]; } return (float)std::sqrt(h / e); };
          float bp = bright(pluck, step + 600, step + 2600), bs = bright(slid, step + 600, step + 2600);
          int z0 = 0, z1 = 0; for (int i = step + 50; i < step + 1500; ++i) { if (pluck[i - 1] < 0 && pluck[i] >= 0) ++z0; if (slid[i - 1] < 0 && slid[i] >= 0) ++z1; }
          printf("      2nd note brightness plucked %.4f, slid in %.4f; early zero crossings %d / %d\n", bp, bs, z0, z1);
          check(bs < bp * 0.8f, "Add Slide: a slid-in note does not re-pluck the filter");
          check(z1 < z0, "Add Slide: the pitch glides up instead of jumping"); }

        // 5g. Key tracking: two octaves up loses far less level through a low cutoff than it would untracked.
        { auto lvl = [&](int note) { OCKM03 p; p.set(kCutoff, 20); p.set(kEnvMod, 0); p.set(kReso, 0);
              auto x = render(p, {{0, note, 80, true}, {step * 3, note, 0, false}}, step * 3); return rms(x, step, step * 3); };
          float lo = lvl(36), hi = lvl(60);
          printf("      cutoff 20%%: level at C3 vs C1 %.1f dB\n", 20 * std::log10(hi / lo));
          check(hi / lo > 0.03f, "key tracking: higher notes are not choked by the filter (better than -30 dB)"); }

        // 5h. Low cut: a 16 Hz fundamental is trimmed relative to its 33 Hz harmonic (a plain saw has 2:1).
        { OCKM03 p; p.set(kCutoff, 100); p.set(kEnvMod, 0); p.set(kReso, 0);
          auto x = render(p, {{0, 12, 80, true}, {sr, 12, 0, false}}, sr);
          float f1 = tone(x, sr / 4, sr, 16.35), f2 = tone(x, sr / 4, sr, 32.70);
          printf("      MIDI 12 saw: fundamental / 2nd harmonic %.2f (2.0 without the low cut)\n", f1 / f2);
          check(f1 / f2 < 1.2f, "low cut trims sub-30 Hz rumble"); }

        // 5i. Every preset with Mutate pushed hard still never clips.
        { float worst = 0; gTransport = true;
          for (int prog = 0; prog < 8; ++prog)
          { OCKM03 p; p.setProgram(prog); p.set(kMuOct, 100); p.set(kMuSld, 100); p.set(kMuRst, 30);
            auto x = render(p, pattern, 64 * step); worst = std::fmax(worst, peak(x, 0, (int)x.size())); }
          gTransport = false;
          printf("      worst preset peak with Mutate at max: %.2f dBFS\n", 20 * std::log10(worst));
          check(worst < 1.0f, "every preset with Mutate at maximum peaks below 0 dBFS"); }
    }
    // 6. v1.3 Echo.
    {
        const std::vector<Ev> blip = {{0, 48, 80, true}, {900, 48, 0, false}};     // one 20 ms note
        auto envPeakAt = [](const std::vector<float>& x, int a, int b) { int at = a; float m = 0; for (int i = a; i < b; ++i) if (std::fabs(x[i]) > m) { m = std::fabs(x[i]); at = i; } return at; };
        gTransport = true;
        // 6a. Echo 0: nothing after the note has released.
        { OCKM03 p; auto x = render(p, blip, sr); check(peak(x, 8000, sr) < 1e-6f, "Echo 0: no repeats, silence after the note"); }
        // 6b. Dotted-8th timing, following the MPC tempo.
        for (double bpm : {125.0, 100.0})
        { gBpm = bpm; OCKM03 p; p.set(kEcho, 50); auto x = render(p, blip, sr);
          const int want = (int)(0.75 * 60.0 / bpm * 44100.0), got = envPeakAt(x, 4000, want + 6000);
          printf("      Echo at %.0f BPM: first repeat at %d samples (dotted 8th = %d)\n", bpm, got, want);
          check(std::abs(got - want) < 1200 && peak(x, want - 200, want + 2000) > 0.02f, bpm == 125.0 ? "Echo: first repeat lands a dotted 8th later (125 BPM)"
                                                                                                        : "Echo: follows the MPC tempo (100 BPM)"); }
        gBpm = 125.0;
        // 6c. Repeats die away, even at maximum.
        { OCKM03 p; p.set(kEcho, 100); auto x = render(p, blip, 6 * sr);
          float early = peak(x, 10000, 30000), late = peak(x, 5 * sr, 6 * sr);
          printf("      Echo 100: early repeat %.3f, after 5 s %.6f\n", early, late);
          check(late < early * 0.001f, "Echo at maximum: repeats fall below -60 dB within 5 s"); }
        // 6d. Turning Echo off and on again never replays old echoes.
        { OCKM03 p; p.internalSetSampleRate(44100); p.internalSetBlockSize(128); p.internalResume();
          std::vector<float> out; std::vector<float> L(128), R(128);
          for (int pos = 0; pos < 4 * sr; pos += 128)
          {
              p.set(kEcho, pos < sr / 2 ? 100.f : (pos < 3 * sr ? 0.f : 60.f));
              std::vector<mpc::MidiEvent> mev; if (pos == 0) { mev.push_back(mpc::MidiEvent::noteOn(0, 48, 80, 0)); }
              if (pos == 1024) mev.push_back(mpc::MidiEvent::noteOff(0, 48, 0));
              float* outs[2] = {L.data(), R.data()}; std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f);
              mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = 128; c.sampleRate = 44100;
              c.midi = mev.data(); c.numMidiEvents = (int)mev.size(); fillTransport(c, pos); p.process(c);
              out.insert(out.end(), L.begin(), L.end());
          }
          check(peak(out, 3 * sr, 4 * sr) < 1e-6f, "Echo turned off then on again: no stale echoes come back"); }
        // 6e. Bass-light repeats: a low note's echo keeps far less of its low end than the dry note.
        { OCKM03 p; p.set(kEcho, 60); p.set(kCutoff, 100); p.set(kEnvMod, 0); p.set(kReso, 0);
          const int d = (int)(0.75 * 60.0 / 125.0 * 44100.0);
          auto x = render(p, {{0, 24, 80, true}, {6000, 24, 0, false}}, 2 * d);
          float dryLo = tone(x, 1000, 5000, 32.7), dryHi = tone(x, 1000, 5000, 32.7 * 16);
          float wetLo = tone(x, d + 1000, d + 5000, 32.7), wetHi = tone(x, d + 1000, d + 5000, 32.7 * 16);
          printf("      low/high harmonic ratio: dry %.2f, first repeat %.2f\n", dryLo / dryHi, wetLo / wetHi);
          check(wetLo / wetHi < 0.5f * dryLo / dryHi, "Echo repeats are bass-light (the bass stays dry and solid)"); }
        // 6f. Every preset with Echo at maximum still never clips.
        { float worst = 0;
          for (int prog = 0; prog < 8; ++prog) { OCKM03 p; p.setProgram(prog); p.set(kEcho, 100); auto x = render(p, pattern, 64 * step); worst = std::fmax(worst, peak(x, 0, (int)x.size())); }
          printf("      worst preset peak with Echo at max: %.2f dBFS\n", 20 * std::log10(worst));
          check(worst < 1.0f, "every preset with Echo at maximum peaks below 0 dBFS"); }
        gTransport = false;
    }
    // 3e. v1.4 diode-ladder filter, on its own (2x oversampled at 44.1 kHz, as in the voice).
    {
        const float os = 88200.0f;
        auto runF = [&](DiodeLadder& f, float fc, float res, auto input, int n, std::vector<float>& y)
        { y.resize(n); for (int i = 0; i < n; ++i) { if ((i & 15) == 0) f.set(fc, res, os); y[i] = f.process(input(i)); } };
        bool decays = true; std::vector<float> y;
        for (float fc : {100.f, 300.f, 1000.f, 3000.f, 8000.f})
        {
            DiodeLadder f; f.reset(); runF(f, fc, 1.0f, [](int i) { return i == 0 ? 0.5f : 0.0f; }, 2 * sr, y);
            float tail = peak(y, sr + sr / 2, 2 * sr);
            if (!(tail < 1e-6f)) { decays = false; printf("      fc %.0f: tail %.3g\n", fc, tail); }
        }
        check(decays, "filter at maximum Resonance: an impulse dies away (no self-oscillation), 100 Hz - 8 kHz");
        bool peaks = true;
        for (float fc : {1000.f, 3000.f})
        {
            int best = 0; float bestA = 0;
            for (int st = -12; st <= 12; ++st)
            {
                const double f0 = fc * std::pow(2.0, st / 12.0);
                DiodeLadder f; f.reset();
                runF(f, fc, 0.9f, [&](int i) { return 0.01f * (float)std::sin(2 * 3.14159265358979 * f0 * i / 44100.0); }, sr / 2, y);
                const float a = rms(y, sr / 4, sr / 2);
                if (a > bestA) { bestA = a; best = st; }
            }
            printf("      Resonance 90, cutoff %.0f Hz: response peaks %+d semitones away\n", fc, best);
            peaks = peaks && std::abs(best) <= 4;
        }
        check(peaks, "the resonant peak sits within a third of an octave of the displayed cutoff");
        DiodeLadder f; f.reset();
        runF(f, 5000.f, 1.0f, [](int i) { return ((i / 37) & 1) ? 100.0f : -100.0f; }, sr, y);
        bool finite = true; for (float v : y) finite = finite && std::isfinite(v) && std::fabs(v) < 20.0f;
        check(finite, "filter stays finite and bounded with +-100 input at maximum Resonance");
        auto level = [&](double f0) { DiodeLadder g; g.reset();
            runF(g, 2000.f, 0.0f, [&](int i) { return 0.01f * (float)std::sin(2 * 3.14159265358979 * f0 * i / 44100.0); }, sr, y);
            return rms(y, sr / 2, sr); };
        const float l40 = level(40), l200 = level(200);
        printf("      40 Hz / 200 Hz through the filter: %.2f\n", l40 / l200);
        check(l40 / l200 < 0.7f, "the in-loop coupling stage thins the deep bass (40 Hz vs 200 Hz)");
    }
    // 3f. v1.4 accent behaviour.
    {
        auto bright = [](const std::vector<float>& x, int a, int b) { double h = 0, e = 0; for (int i = a + 1; i < b; ++i) { float d = x[i] - x[i - 1]; h += d * d; e += x[i] * x[i]; } return e > 0 ? (float)std::sqrt(h / e) : 0.f; };
        // Accent sweep: with Env Mod 0 only the accent moves the filter. High Resonance swells later.
        auto peakTime = [&](float reso) { OCKM03 p; p.set(kEnvMod, 0); p.set(kAccent, 100); p.set(kCutoff, 45); p.set(kDecay, 70); p.set(kReso, reso);
            auto x = render(p, {{0, 36, 127, true}, {sr / 2, 36, 0, false}}, sr / 2);
            int bestT = 0; float best = 0; for (int t = 0; t + 441 < sr / 2; t += 441) { float b = bright(x, t, t + 441); if (b > best) { best = b; bestT = t; } }
            return bestT * 1000 / sr; };
        const int t10 = peakTime(10), t90 = peakTime(90);
        printf("      accented note brightest at %d ms (Resonance 10) and %d ms (Resonance 90)\n", t10, t90);
        check(t90 > t10 + 10, "accent sweep: at high Resonance the accent swells later (the 'wow')");
        // Accent punch: the extra level dies away with the accent instead of lasting the whole note.
        auto lv = [&](int vel, int a, int b) { OCKM03 p; p.set(kAccent, 100); p.set(kEnvMod, 0); p.set(kReso, 0); p.set(kCutoff, 60);
            auto x = render(p, {{0, 36, vel, true}, {sr / 2, 36, 0, false}}, sr / 2); return rms(x, a, b); };
        const float rStart = lv(127, 300, 1300) / lv(80, 300, 1300), rLate = lv(127, 11000, 12000) / lv(80, 11000, 12000);
        printf("      accented / normal level: %.2f at the start, %.2f at 250 ms\n", rStart, rLate);
        check(rLate < rStart * 0.75f && rStart > 1.5f, "accent punch: a hit at the start that dies away, not a louder note");
        // Env Mod: more Env Mod also lowers the resting cutoff.
        auto restB = [&](float env) { OCKM03 p; p.set(kEnvMod, env); p.set(kDecay, 0); p.set(kReso, 0); p.set(kCutoff, 50);
            auto x = render(p, {{0, 36, 80, true}, {sr, 36, 0, false}}, sr); return bright(x, sr * 6 / 10, sr * 9 / 10); };
        const float b0 = restB(0), b100 = restB(100);
        printf("      late brightness of a held note: Env Mod 0 %.4f, Env Mod 100 %.4f\n", b0, b100);
        check(b100 < b0 * 0.9f, "Env Mod also lowers the resting cutoff (the sweep goes below the knob too)");
    }
    // 3g. v1.5 Pattern generator.
    {
        // Exact and nested amounts, straight from the generator.
        bool exact = true, nested = true;
        for (int seed = 1; seed <= 40; ++seed)
        {
            int c8 = 0, c4 = 0;
            for (int st = 0; st < 16; ++st)
            {
                c8 += Pattern::plays(st, 16, seed, 0.5f); c4 += Pattern::plays(st, 16, seed, 0.25f);
                if (Pattern::plays(st, 16, seed, 0.3f) && !Pattern::plays(st, 16, seed, 0.6f)) nested = false;
                const PatternStep lo = Pattern::at(st, 16, seed, 1.0f, 0.3f, 0.3f, 2, 0), hi = Pattern::at(st, 16, seed, 1.0f, 0.6f, 0.6f, 3, 0);
                if ((lo.accent && !hi.accent) || (lo.slide && !hi.slide)) nested = false;
            }
            if (c8 != 8 || c4 != 4) exact = false;
        }
        check(exact, "Pattern: Density 50% plays exactly 8 and 25% exactly 4 of 16 steps (40 Pattern #s)");
        check(nested, "Pattern: turning Density, Accent or Slide up only adds steps");
        bool inScale = true;
        for (int seed = 1; seed <= 20; ++seed) for (int st = 0; st < 32; ++st)
        { const int d = Pattern::at(st, 32, seed, 1.0f, 0, 0, 1, 3).semis; if (d != 0 && d != 3 && d != 5 && d != 7 && d != 10) inScale = false; }
        check(inScale, "Pattern: Minor Pent lines use only its five notes (Range 1)");

        gTransport = true;
        const int bar = 16 * step;
        auto held = [&](int note, int bars) { return std::vector<Ev>{{0, note, 100, true}, {bars * bar, note, 0, false}}; };
        auto setup = [&](OCKM03& p) { p.set(kPtMode, 1); p.set(kPtSld, 0); p.set(kPtAcc, 0); p.set(kReso, 0); p.set(kCutoff, 70); p.set(kEnvMod, 0); };
        { OCKM03 a; setup(a); auto x1 = render(a, held(36, 2), 2 * bar);
          OCKM03 b; setup(b); auto x2 = render(b, held(36, 2), 2 * bar);
          OCKM03 c; setup(c); c.set(kPtSeed, 2); auto x3 = render(c, held(36, 2), 2 * bar);
          check(x1 == x2 && x1 != x3, "Pattern: the same Pattern # gives the same line, another gives a different one"); }
        { OCKM03 a; a.setProgram(1); auto x1 = render(a, pattern, 16 * step);
          OCKM03 b; b.setProgram(1); b.set(kPtSeed, 500); b.set(kPtDen, 10); b.set(kPtLen, 5); b.set(kPtRng, 3); b.set(kPtScl, 2);
          auto x2 = render(b, pattern, 16 * step);
          check(x1 == x2, "Pattern Off: the Pattern knobs change nothing (plays as v1.4)"); }
        { OCKM03 a; setup(a); auto x = render(a, {}, 2 * bar);
          check(peak(x, 0, (int)x.size()) == 0.0f, "Pattern Hold: silent while no note is held"); }
        { OCKM03 a; a.setProgram(6); a.set(kEcho, 0); a.set(kMuOct, 100); a.set(kPtRng, 3); a.set(kPtSld, 100); auto x = render(a, held(36, 2), 3 * bar);
          check(peak(x, 2 * bar + 4410, 3 * bar) < 1e-4f, "Pattern Hold: releasing the note stops the line (no hanging note, even with slides and Mutate)"); }
        // Which steps start a note: energy in the first quarter of each step against the last quarter of the previous one.
        auto onsets = [&](const std::vector<float>& x, int steps) { std::vector<int> o(steps);
            for (int k = 0; k < steps; ++k) o[k] = rms(x, k * step + 100, k * step + step / 4) > 0.01f; return o; };
        { OCKM03 a; setup(a); a.set(kPtDen, 50); a.set(kPtSeed, 9); auto x = render(a, held(36, 4), 4 * bar);
          auto o = onsets(x, 64); bool ok = true; int n = 0;
          for (int k = 0; k < 64; ++k) { n += o[k]; if (o[k] != (int)Pattern::plays(k % 16, 16, 9, 0.5f)) ok = false; }
          printf("      Density 50%%, 4 bars: %d notes\n", n);
          check(ok, "Pattern: notes land on the predicted 16ths of the MPC grid, the same in every bar"); }
        { OCKM03 a; setup(a); a.set(kPtDen, 50); a.set(kPtLen, 12); a.set(kPtSeed, 3); auto x = render(a, held(36, 4), 4 * bar);
          auto o = onsets(x, 60); bool ok = true;
          for (int k = 0; k < 60; ++k) if (o[k] != (int)Pattern::plays(k % 12, 12, 3, 0.5f)) ok = false;
          check(ok, "Pattern: Length 12 repeats every 12 steps (follows the song's 16ths)"); }
        { auto zc = [&](int note) { OCKM03 a; setup(a); a.set(kPtDen, 100); auto x = render(a, held(note, 1), bar);
              int z = 0; for (int i = 1; i < bar; ++i) if (x[i - 1] < 0 && x[i] >= 0) ++z; return z; };
          const int z36 = zc(36), z48 = zc(48);
          printf("      zero crossings holding C1 %d, C2 %d\n", z36, z48);
          check(z48 > z36 * 1.8 && z48 < z36 * 2.2, "Pattern: the held note transposes the line (an octave up doubles the pitch)"); }
        { auto bright = [](const std::vector<float>& x, int a, int b) { double h = 0, e = 0; for (int i = a + 1; i < b; ++i) { float d = x[i] - x[i - 1]; h += d * d; e += x[i] * x[i]; } return e > 0 ? (float)std::sqrt(h / e) : 0.f; };
          auto avgB = [&](float sld) { OCKM03 a; setup(a); a.set(kCutoff, 50); a.set(kEnvMod, 80); a.set(kDecay, 20); a.set(kPtDen, 100); a.set(kPtSld, sld);
              auto x = render(a, held(36, 1), bar); double t = 0; for (int k = 4; k < 14; ++k) t += bright(x, k * step + 300, k * step + 2000); return (float)(t / 10); };
          const float b0 = avgB(0), b100 = avgB(100);
          printf("      Pattern brightness, Slide 0 %.4f, Slide 100 %.4f\n", b0, b100);
          check(b100 < b0 * 0.85f, "Pattern: slide steps glide without re-plucking the filter"); }
        { OCKM03 a; setup(a); a.set(kPtDen, 100); a.set(kMuRst, 40); a.set(kMuVar, 5); auto x = render(a, held(36, 2), 2 * bar);
          auto o = onsets(x, 32); bool ok = true;
          for (int k = 0; k < 32; ++k) if (o[k] != (int)!Mutator::decide(k % 16, 5, 0, 0, 0.4f).rest) ok = false;
          check(ok, "Pattern + Add Rest: Mutate rests the predicted steps of the generated line"); }
        { OCKM03 a; a.setProgram(6); auto x1 = render(a, held(36, 2), 2 * bar, 128);
          OCKM03 b; b.setProgram(6); auto x2 = render(b, held(36, 2), 2 * bar, 37);
          OCKM03 c; c.setProgram(6); auto x3 = render(c, held(36, 2), 2 * bar, 512);
          double d = 0; for (size_t i = 0; i < x1.size(); ++i) d = std::fmax(d, std::fmax(std::fabs(x1[i] - x2[i]), std::fabs(x1[i] - x3[i])));
          printf("      block sizes 37 / 128 / 512: largest difference %.2g\n", d);
          check(d < 3e-3, "Pattern: the line is the same whatever the MPC's block size (differences below -50 dB)"); }
        gTransport = false;
        { OCKM03 a; setup(a); auto x = render(a, {{1000, 36, 100, true}, {1000 + bar, 36, 0, false}}, bar + 2000);
          check(rms(x, 1100, 1100 + step / 4) > 0.01f || !Pattern::plays(0, 16, 1, 0.75f), "Pattern, transport stopped: the line starts as the note is pressed"); }
    }
    // 3h. v1.6 Grit.
    {
        const float os = 88200.0f;
        auto runSaw = [&](float fc, float res, float drive, bool fb, float amp, std::vector<float>& y)
        { DiodeLadder f; f.reset(); y.resize(sr); double ph = 0;
          for (int i = 0; i < sr; ++i) { if ((i & 15) == 0) f.set(fc, res, os, drive, fb); ph += 65.406 / sr; if (ph >= 1) ph -= 1; y[i] = f.process(amp * (float)(2 * ph - 1)); } };
        auto runSine = [&](float fc, float res, float drive, bool fb, float amp, double f0, std::vector<float>& y)
        { DiodeLadder f; f.reset(); y.resize(sr);
          for (int i = 0; i < sr; ++i) { if ((i & 15) == 0) f.set(fc, res, os, drive, fb); y[i] = f.process(amp * (float)std::sin(2 * 3.14159265358979 * f0 * i / 44100.0)); } };
        std::vector<float> y;
        // DC gain 1 at Resonance 0 (Full Bass on moves the coupling to 16 Hz; measure at 60 Hz through a 5 kHz filter).
        runSine(5000.f, 0.f, 1.f, true, 0.01f, 60.0, y);
        const float g60 = rms(y, sr / 2, sr) / (0.01f / std::sqrt(2.f)) / DiodeLadder::kPostBase;
        printf("      passband gain at Resonance 0 (60 Hz, Full Bass): %.2f\n", g60);
        check(g60 > 0.85f && g60 < 1.1f, "filter: unity passband gain at Resonance 0 (the input pair feeds a current, v1.6 fix)");
        bool decays = true;
        for (bool fb : {false, true}) for (float fc : {100.f, 300.f, 1000.f, 3000.f, 8000.f})
        { DiodeLadder f; f.reset(); std::vector<float> z(2 * sr);
          for (int i = 0; i < 2 * sr; ++i) { if ((i & 15) == 0) f.set(fc, 1.0f, os, 66.6f, fb); z[i] = f.process(i == 0 ? 0.5f : 0.0f); }
          if (!(peak(z, sr + sr / 2, 2 * sr) < 1e-6f)) decays = false; }
        check(decays, "filter with saturation and Overdrive 100: an impulse at maximum Resonance dies away (with and without Full Bass)");
        { DiodeLadder f; f.reset(); bool ok = true;
          for (int i = 0; i < sr; ++i) { if ((i & 15) == 0) f.set(19000.f, 1.0f, os, 66.6f, true); float v = f.process(((i / 7) & 1) ? 100.f : -100.f); ok = ok && std::isfinite(v) && std::fabs(v) < 20.f; }
          check(ok, "filter stays bounded with +-100 input, Overdrive 100 and maximum cutoff"); }
        // Stage saturation: a loud sine has relatively more 3rd harmonic than a quiet one.
        auto h3 = [&](float amp) { runSine(2000.f, 0.f, 1.f, false, amp, 200.0, y); return tone(y, sr / 2, sr, 600.0) / tone(y, sr / 2, sr, 200.0); };
        const float hq = h3(0.05f), hl = h3(1.5f);
        printf("      3rd harmonic / fundamental: quiet %.4f, loud %.4f\n", hq, hl);
        check(hl > hq * 10.f, "filter saturates when driven (inner stages and input pair)");
        auto evenOdd = [&](float drive) { runSaw(800.f, 0.5f, drive, false, 0.8f, y); double e = 0, o = 0;
            for (int hh = 2; hh <= 8; hh += 2) e += tone(y, sr / 2, sr, 65.406 * hh); for (int hh = 3; hh <= 9; hh += 2) o += tone(y, sr / 2, sr, 65.406 * hh); return (float)(e / o); };
        const float u0 = evenOdd(1.f), u1 = evenOdd(1 + 65.6f * 0.36f);
        printf("      even / odd harmonics (2-9): Overdrive 0 %.3f, Overdrive 60 %.3f\n", u0, u1);
        check(u1 < u0 * 0.75f && u1 < 1.0f, "Overdrive squares up the wave inside the filter (odd harmonics take over)");
        auto bassRatio = [&](bool fb) { runSine(3000.f, 0.f, 1.f, fb, 0.01f, 40.0, y); float a = rms(y, sr / 2, sr);
            runSine(3000.f, 0.f, 1.f, fb, 0.01f, 200.0, y); return a / rms(y, sr / 2, sr); };
        const float bOff = bassRatio(false), bOn = bassRatio(true);
        printf("      40 Hz / 200 Hz: Full Bass off %.2f, on %.2f\n", bOff, bOn);
        check(bOn > bOff * 1.2f && bOn > 0.9f, "Full Bass keeps the deep bass (40 Hz close to 200 Hz)");
        // Muffler lowers the crest factor of a played line.
        auto crestM = [&](float m) { OCKM03 p; p.setProgram(3); p.set(kMuffler, m); auto x = render(p, pattern, 16 * step);
            return peak(x, 0, (int)x.size()) / rms(x, 0, (int)x.size()); };
        const float c0 = crestM(0), c100 = crestM(100);
        printf("      crest factor: Muffler 0 %.2f, Muffler 100 %.2f\n", c0, c100);
        check(c100 < c0 * 0.8f, "Muffler rounds off the peaks (lower crest factor)");
        // Soft attack: a normal note takes longer to reach half level than an accented one.
        auto rise = [&](int vel) { OCKM03 p; p.set(kCutoff, 70); p.set(kEnvMod, 0); p.set(kReso, 0); p.set(kAccent, 0);
            auto x = render(p, {{0, 36, vel, true}, {step, 36, 0, false}}, step); return peak(x, 0, 60) / peak(x, 0, 1500); };
        const float r80 = rise(80), r127 = rise(127);
        printf("      level in the first 1.4 ms (share of the note's peak): normal %.2f, accented %.2f\n", r80, r127);
        check(r80 < r127 * 0.8f, "soft attack: normal notes start more gently than accented ones");
        { float worst = 0;
          for (int prog = 0; prog < 8; ++prog) { OCKM03 p; p.setProgram(prog); p.set(kOverdrive, 100); p.set(kMuffler, 100); p.set(kFullBass, 1);
              auto x = render(p, pattern, 32 * step); worst = std::fmax(worst, peak(x, 0, (int)x.size())); }
          printf("      worst preset peak with Overdrive, Muffler and Full Bass at maximum: %.2f dBFS\n", 20 * std::log10(worst));
          check(worst < 1.0f, "every preset with Overdrive, Muffler and Full Bass at maximum peaks below 0 dBFS"); }
    }
    // 4. Demo: each preset playing the pattern, Cutoff swept up and back in bars 3-4.
    {
        std::vector<float> all;
        for (int prog = 0; prog < 5; ++prog)
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
                gTransport = true; fillTransport(c, pos); gTransport = false;
                c.midi = mev.data(); c.numMidiEvents = (int)mev.size(); p.process(c);
            }
            printf("      preset %-16s peak %.2f dBFS\n", p.programName(prog), 20 * std::log10(peak(L, 0, (int)L.size()) + 1e-9));
            all.insert(all.end(), L.begin(), L.end());
        }
        { OCKM03 p; p.setProgram(7); auto L = render(p, pattern, 32 * step);
          printf("      preset %-16s peak %.2f dBFS\n", p.programName(7), 20 * std::log10(peak(L, 0, (int)L.size()) + 1e-9));
          all.insert(all.end(), L.begin(), L.end()); }
        for (int prog = 5; prog < 7; ++prog)
        {
            gTransport = true;
            std::vector<Ev> bars; const int keys[4] = {36, 36, 39, 34};
            for (int b = 0; b < 4; ++b) { bars.push_back({b * 16 * step, keys[b], 100, true}); bars.push_back({(b + 1) * 16 * step, keys[b], 0, false}); }
            std::sort(bars.begin(), bars.end(), [](const Ev& a, const Ev& b) { return a.frame < b.frame || (a.frame == b.frame && !a.on && b.on); });
            OCKM03 p; p.setProgram(prog); auto L = render(p, bars, 64 * step + 4410);
            gTransport = false;
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
