// Test harness for OCKM DRIFT: includes the plugin source directly and checks what SPEC.md
// promises (section 11). Ends with ALL PASS, and renders the demo WAV.
// Run from the MPC-Plugins repo root (SDK = $MPC_SDK or /Users/simon/Projects/mpc-plugin-builder-sdk):
//   clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM DRIFT/src" "OCKM DRIFT/tests/harness.cpp" -o "OCKM DRIFT/build/harness"
//   "OCKM DRIFT/build/harness" "OCKM DRIFT/build/demo.wav"
#include "OCKMDRIFT.cpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <set>
#include <vector>

static const int SR = 44100;
static int gFails = 0;
static void check(bool ok, const char* what, const char* detail = "")
{
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail[0] ? ": " : "", detail);
    if (!ok) ++gFails;
}

// ---------------------------------------------------------------------------
// A tiny MPC: tempo, song position, play/stop, an optional sequence loop, notes in.
// ---------------------------------------------------------------------------
struct Logged { long long frame; double ppq; int step; int note; bool accent, legato, late; };
struct KeyEv { long long frame; int note; bool on; };

struct Mpc
{
    OCKMDRIFT& p;
    double bpm = 124.0;
    int block = 128;
    double ppq = 0.0;          // song position at the next block
    double loopBeats = 0.0;    // 0 = no loop; else the sequence jumps back to 0 here
    bool playing = true;
    long long frame = 0;       // frames rendered so far
    std::vector<float> out;    // left channel (L == R always; checked separately)
    std::vector<float> outR;
    std::vector<Logged> log;
    std::vector<KeyEv> keys;
    double blockPpq = 0.0;
    std::function<void(Mpc&)> perBlock;

    explicit Mpc(OCKMDRIFT& plug, int blk = 128) : p(plug), block(blk)
    {
        p.internalSetSampleRate(SR); p.internalSetBlockSize(block); p.internalResume();
    }
    static void hook(void* user, const NoteInfo& n)
    {
        Mpc* m = (Mpc*)user;
        const double bpb = m->bpm / 60.0 / SR;
        m->log.push_back({m->frame + n.offset, m->blockPpq + n.offset * bpb, n.step, n.note, n.accent, n.legato, n.late});
    }
    double framesPerBeat() const { return SR * 60.0 / bpm; }
    void run(long long frames)
    {
        gNoteHook = &Mpc::hook; gNoteHookUser = this;
        std::vector<mpc::MidiEvent> mev;
        const long long end = frame + frames;
        while (frame < end)
        {
            const int n = (int)std::min<long long>(block, end - frame);
            if (perBlock) perBlock(*this);
            if (loopBeats > 0.0 && ppq >= loopBeats - 1.0e-9) ppq -= loopBeats;
            mev.clear();
            for (const auto& k : keys) if (k.frame >= frame && k.frame < frame + n)
                mev.push_back(k.on ? mpc::MidiEvent::noteOn(0, k.note, 100, (int)(k.frame - frame)) : mpc::MidiEvent::noteOff(0, k.note, (int)(k.frame - frame)));
            const size_t at = out.size();
            out.resize(at + n, 0.0f); outR.resize(at + n, 0.0f);
            float* outs[2] = {out.data() + at, outR.data() + at};
            mpc::ProcessContext c; c.outputs = outs; c.numOutputs = 2; c.numFrames = n; c.sampleRate = SR;
            c.midi = mev.data(); c.numMidiEvents = (int)mev.size();
            c.transport.valid = true; c.transport.tempoValid = true; c.transport.bpm = bpm;
            c.transport.playing = playing; c.transport.ppqValid = true;
            c.transport.ppqPos = ppq; c.transport.barStartPpq = std::floor(ppq / 4.0 + 1e-9) * 4.0;
            blockPpq = ppq;
            p.process(c);
            if (playing) ppq += n * bpm / 60.0 / SR;
            frame += n;
        }
        gNoteHook = nullptr;
    }
    long long bars(double b) const { return (long long)std::llround(b * 4.0 * framesPerBeat()); }
    void runBars(double b) { run(bars(b)); }
    void chord(long long fr, int note, long long len) { keys.push_back({fr, note, true}); keys.push_back({fr + len, note, false}); }
};

static float peakOf(const std::vector<float>& x, size_t a = 0, size_t b = (size_t)-1)
{ b = std::min(b, x.size()); float m = 0; for (size_t i = a; i < b; ++i) m = std::fmax(m, std::fabs(x[i])); return m; }
static float db(float g) { return 20.0f * std::log10(std::fmax(g, 1e-12f)); }
static bool finiteAll(const std::vector<float>& x) { for (float v : x) if (!std::isfinite(v)) return false; return true; }

// Notes grouped per bar of play (by frame), as (step, note, accent) triples.
using BarNotes = std::vector<std::array<int, 3>>;
static std::vector<BarNotes> perBar(const Mpc& m, int nbars, long long startFrame = 0)
{
    std::vector<BarNotes> b(nbars);
    const double fpb = 4.0 * m.framesPerBeat();
    for (const auto& l : m.log)
    {
        const long long rel = l.frame - startFrame;
        if (rel < 0) continue;
        const int bi = (int)std::floor((rel + 1) / fpb);
        if (bi < nbars) b[bi].push_back({l.step, l.note, l.accent ? 1 : 0});
    }
    return b;
}

static bool inScale(int note, int root, int scale)
{
    const int rel = ((note - root) % 12 + 12) % 12;
    for (int k = 0; k < kScaleLen[scale]; ++k) if (kScaleNotes[scale][k] == rel) return true;
    return false;
}

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
    FILE* f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); return; }
    const int n = (int)L.size();
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + n * 4); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(SR); w32(SR * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(n * 4);
    for (int i = 0; i < n; ++i)
    {
        w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, L[i])) * 32767));
        w16((short)std::lround(std::fmax(-1.f, std::fmin(1.f, R[i])) * 32767));
    }
    fclose(f);
}

static const char* kPresetNames[] = {"Init Drift", "Slow Burn", "Rolling Techno", "Peak Arp", "Sparse Dub",
                                     "Singing Lead", "Deep Walker", "Chaos Garden", "Frozen Hook"};
static const int kNumPresets = 9;

// Chord roots A F C G (one per bar), as written in an MPC sequence: octave 2.
static void progression(Mpc& m, long long start, int nbars, int lateFrames = 0)
{
    static const int roots[4] = {45, 41, 48, 43};
    const long long bar = m.bars(1);
    for (int b = 0; b < nbars; ++b) m.chord(start + m.bars(b) + lateFrames, roots[b % 4], bar - 200);
}

int main(int argc, char** argv)
{
    char buf[256];

    // ---------------- Same Seed, same line ----------------
    {
        OCKMDRIFT a, b, c;
        Mpc ma(a), mb(b), mc(c);
        c.set(kSeed, 2);
        ma.runBars(32); mb.runBars(32); mc.runBars(32);
        bool sameLog = ma.log.size() == mb.log.size(), sameAudio = ma.out == mb.out;
        for (size_t i = 0; sameLog && i < ma.log.size(); ++i)
            sameLog = ma.log[i].frame == mb.log[i].frame && ma.log[i].note == mb.log[i].note && ma.log[i].accent == mb.log[i].accent;
        bool differs = ma.log.size() != mc.log.size();
        for (size_t i = 0; !differs && i < ma.log.size(); ++i) differs = ma.log[i].note != mc.log[i].note;
        snprintf(buf, sizeof buf, "%zu notes over 32 bars", ma.log.size());
        check(sameLog && ma.log.size() > 100, "same Seed gives the same note log", buf);
        check(sameAudio, "same Seed gives bit-identical audio");
        check(differs, "a different Seed gives a different line");
        check(ma.out == ma.outR, "output is mono (L == R)");
    }

    // ---------------- Press Play again: the same line ----------------
    {
        OCKMDRIFT a; Mpc m(a);
        m.runBars(8);
        const auto first = perBar(m, 8);
        m.playing = false; m.runBars(1);
        m.playing = true; m.ppq = 0.0;
        const long long restart = m.frame;
        const size_t before = m.log.size();
        m.runBars(8);
        std::vector<Logged> second(m.log.begin() + (long)before, m.log.end());
        Mpc tmp(a); tmp.log = second;
        const auto again = perBar(tmp, 8, restart);
        check(first == again && !first[0].empty(), "Stop then Play restarts the same line");
    }

    // ---------------- Mutate 0 never changes it ----------------
    {
        bool ok = true;
        for (int ev = 0; ev < 4; ++ev)
        {
            OCKMDRIFT a; a.set(kMutate, 0); a.set(kEvolve, ev); a.set(kSlide, 0);
            Mpc m(a);
            const Pattern p0 = a.pattern();
            bool patOk = true;
            m.perBlock = [&](Mpc& mm) { if (mm.p.pattern() != p0) patOk = false; };
            m.runBars(64);
            const auto bars = perBar(m, 64);
            for (int b = 1; b < 64; ++b) if (bars[b] != bars[0]) ok = false;
            if (!patOk) ok = false;
        }
        check(ok, "Mutate 0%: the pattern never changes (64 bars, every Evolve setting)");
    }

    // ---------------- Mutate changes, at Evolve points only, by small steps ----------------
    {
        OCKMDRIFT a; a.set(kMutate, 25); a.set(kEvolve, 2);  // every 4 bars
        Mpc m(a);
        Pattern last = a.pattern();
        int changes = 0; bool onlyAtPoints = true, small = true;
        m.perBlock = [&](Mpc& mm) {
            const Pattern& p = mm.p.pattern();
            if (p != last)
            {
                ++changes;
                // The change happened inside the previous block; it must contain a 4-bar line.
                const double prev = mm.ppq - mm.block * mm.bpm / 60.0 / SR;
                const double barLine = std::floor(mm.ppq / 16.0) * 16.0;
                if (!(barLine > prev - 1e-9 && barLine <= mm.ppq + 1e-9)) onlyAtPoints = false;
                for (int i = 0; i < kSteps; ++i) if (std::abs(p.s[i].deg - last.s[i].deg) > 2) small = false;
                last = p;
            }
        };
        m.runBars(64);
        snprintf(buf, sizeof buf, "%d changes in 64 bars at Evolve 4", changes);
        check(changes >= 12 && changes <= 16, "Mutate 25%: the pattern changes at Evolve points", buf);
        check(onlyAtPoints, "changes land only on the 4-bar lines");
        check(small, "a change moves a note by at most 2 scale steps");
    }

    // ---------------- Lock freezes; state brings it back ----------------
    {
        OCKMDRIFT a; a.set(kMutate, 100); a.set(kEvolve, 0);
        Mpc m(a);
        m.runBars(5);
        a.set(kLock, 1);
        m.runBars(27);
        const auto bars = perBar(m, 32);
        bool frozen = true;
        for (int b = 6; b < 32; ++b) if (bars[b] != bars[5]) frozen = false;
        bool changedBefore = bars[1] != bars[2] || bars[2] != bars[3];
        check(changedBefore, "Mutate 100%: the line changes every bar while unlocked");
        check(frozen, "Lock: bars 5-31 all repeat the locked pattern");

        std::vector<uint8_t> st; a.saveExtraState(st);
        const Pattern locked = a.pattern();
        OCKMDRIFT b; b.set(kMutate, 100); b.set(kEvolve, 0); b.set(kLock, 1);
        b.loadExtraState(st.data(), st.size());
        Mpc mb(b);                                         // internalResume -> reset swaps it in
        check(b.pattern() == locked, "Lock: the locked pattern is saved and restored with the state");
        mb.runBars(4);
        const auto bb = perBar(mb, 4);
        check(bb[1] == bars[10] && bb[3] == bars[10], "Lock: the restored instance plays the locked line");

        a.set(kLock, 0);
        const long long from = m.frame;
        m.runBars(4);
        Mpc tmp(a); tmp.log = m.log;
        const auto after = perBar(tmp, 4, from);
        check(after[1] != bars[10] || after[2] != bars[10], "Lock off: the line evolves again from the locked pattern");
    }

    // ---------------- Seed change while playing: next bar ----------------
    {
        OCKMDRIFT a; a.set(kMutate, 0);
        Mpc m(a);
        m.runBars(2.5);
        a.set(kSeed, 77);
        m.runBars(1.5);
        OCKMDRIFT b; b.set(kMutate, 0); b.set(kSeed, 77);
        Mpc mb(b); mb.runBars(1);
        const auto bars = perBar(m, 4), ref = perBar(mb, 1);
        check(bars[2] == perBar(m, 4)[1] && bars[3] == ref[0], "a new Seed takes over at the next bar line");
    }

    // ---------------- Every note in the scale ----------------
    {
        bool ok = true; long long count = 0;
        for (int keys = 0; keys < 3 && ok; ++keys)
            for (int sc = 0; sc < kNumScales && ok; ++sc)
                for (int root = 0; root < 12 && ok; ++root)
                    for (int range = 1; range <= 3 && ok; ++range)
                    {
                        OCKMDRIFT a; a.set(kKeys, keys); a.set(kScale, sc); a.set(kRoot, root); a.set(kRange, range);
                        a.set(kHits, 16); a.set(kMutate, 100); a.set(kEvolve, 0); a.set(kSeed, 1 + sc * 37 + root * 5 + range);
                        Mpc m(a, 256);
                        // a "chord" note per bar, any octave, inside or outside the key
                        const int chordNotes[4] = {30 + (root * 7) % 12, 55 + (sc * 5) % 12, 40 + range, 71 - root};
                        for (int b = 0; b < 4; ++b) m.chord(m.bars(b), chordNotes[b], m.bars(1) - 100);
                        m.runBars(4);
                        for (const auto& l : m.log)
                        {
                            int r = root;
                            if (keys == kKeysTranspose)
                            {
                                const int b = (int)(l.frame / m.bars(1));
                                const int rel = ((chordNotes[b] % 12) - root + 12) % 12;
                                r = root + (rel >= 6 ? rel - 12 : rel);
                            }
                            ++count;
                            if (!inScale(l.note, r, sc)) { ok = false; snprintf(buf, sizeof buf, "keys %d scale %d root %d note %d", keys, sc, root, l.note); }
                        }
                    }
        if (ok) snprintf(buf, sizeof buf, "%lld notes, 8 scales x 12 roots x 3 ranges x Keys Off/In Key/Transpose", count);
        check(ok, "every note is in the scale (Transpose: in the transposed scale)", buf);
    }

    // ---------------- Range and walk ----------------
    {
        bool ok = true;
        for (int range = 1; range <= 3; ++range)
        {
            OCKMDRIFT a; a.set(kRange, range); a.set(kHits, 16); a.set(kMutate, 100); a.set(kEvolve, 0); a.set(kKeys, 0);
            Mpc m(a); m.runBars(16);
            for (const auto& l : m.log) if (l.note < 69 || l.note > 69 + 12 * range) ok = false;
        }
        check(ok, "notes stay between the root and Range octaves above it (A3 at Octave 0)");
    }

    // ---------------- On the MPC's grid ----------------
    {
        bool ok = true; int count = 0; double worst = 0;
        const double bpms[3] = {98, 124, 140};
        for (double bpm : bpms)
            for (int rate = 0; rate < 3; ++rate)
                for (float swing : {50.0f, 66.0f})
                {
                    OCKMDRIFT a; a.set(kHits, 16); a.set(kRate, rate); a.set(kSwing, swing);
                    Mpc m(a); m.bpm = bpm; m.runBars(8);
                    const double stepB = kRateBeats[rate];
                    const double bpb = bpm / 60.0 / SR;
                    for (const auto& l : m.log)
                    {
                        const double loopB = std::max(4.0, 16 * stepB);
                        const double inLoop = std::fmod(l.ppq, loopB);
                        const double pair = 2 * stepB;
                        const double expect = std::floor(l.step / 2) * pair + ((l.step & 1) ? swing * 0.01 * pair : 0.0);
                        const double lateSamples = (inLoop - expect) / bpb;
                        worst = std::max(worst, std::fabs(lateSamples));
                        if (lateSamples < -1e-6 || lateSamples >= 1.0 + 1e-6) ok = false;
                        ++count;
                    }
                    // every step plays: exact count
                    if ((int)m.log.size() != (int)std::lround(8 * 4 / stepB)) ok = false;
                }
        snprintf(buf, sizeof buf, "%d notes at 98/124/140 BPM, 1/16 1/8 1/4, swing 50/66: worst %.3f samples late", count, worst);
        check(ok, "every step starts within one sample of the MPC's grid", buf);

        // An MPC sequence looping every 2 bars: steps stay on the grid, evolution keeps counting.
        OCKMDRIFT a; a.set(kHits, 16); a.set(kEvolve, 0);
        Mpc m(a); m.loopBeats = 8.0; m.runBars(16);
        bool gridOk = true;
        const double bpb = m.bpm / 60.0 / SR;
        for (const auto& l : m.log)
        {
            const double off = (std::fmod(l.ppq, 4.0) - l.step * 0.25) / bpb;
            if (off < -1e-6 || off >= 1.0 + 1e-6) gridOk = false;
        }
        snprintf(buf, sizeof buf, "%zu notes, evolved %u times in 16 bars", m.log.size(), a.cycle());
        check(gridOk && m.log.size() == 256 && a.cycle() >= 14 && a.cycle() <= 16, "a looping MPC sequence: on the grid and still evolving", buf);
    }

    // ---------------- Silent when stopped ----------------
    {
        OCKMDRIFT a; Mpc m(a); m.runBars(2);
        const size_t stopAt = m.out.size();
        m.playing = false; m.runBars(2);
        const size_t settle = stopAt + (size_t)(0.25 * SR);
        float tail = peakOf(m.out, settle);
        bool notes = false;
        for (const auto& l : m.log) if ((size_t)l.frame >= stopAt) notes = true;
        snprintf(buf, sizeof buf, "peak %.1f dBFS from 0.25 s after Stop", db(tail));
        check(tail == 0.0f && !notes, "stopped: exact silence, no notes", buf);

        OCKMDRIFT b; b.set(kRunStop, 1); Mpc mb(b); mb.playing = false; mb.runBars(2);
        snprintf(buf, sizeof buf, "%zu notes, peak %.1f dBFS", mb.log.size(), db(peakOf(mb.out)));
        check(mb.log.size() >= 12 && peakOf(mb.out) > 0.05f, "Run Stopped: the line plays with the MPC stopped", buf);
    }

    // ---------------- Keys: chord changes ----------------
    {
        OCKMDRIFT a; a.set(kMutate, 0); Mpc m(a);
        progression(m, 0, 8);
        m.runBars(8);
        static const int roots[4] = {9, 5, 0, 7};
        bool downbeats = true, key = true;
        const auto bars = perBar(m, 8);
        for (int b = 0; b < 8; ++b)
            if (bars[b].empty() || bars[b][0][0] != 0 || bars[b][0][1] % 12 != roots[b % 4]) downbeats = false;
        for (const auto& l : m.log) if (!inScale(l.note, 9, 0)) key = false;
        check(downbeats, "In Key: step 1 of every bar is the chord root (A F C G)");
        check(key, "In Key: every note stays in A minor while following the chords");

        // Nearest: A -> F moves the line down, not up.
        OCKMDRIFT n; n.set(kMutate, 0); Mpc mn(n); mn.chord(mn.bars(1), 41, mn.bars(1) - 100); mn.runBars(2);
        const auto nb = perBar(mn, 2);
        check(!nb[1].empty() && nb[1][0][1] == 65, "In Key: A to F moves the line to the nearest F (F4 = 65, below A4)");

        // Held: silent with no note, plays where notes are written.
        OCKMDRIFT h; h.set(kPlay, 1); Mpc mh(h);
        mh.runBars(1);
        const float silentPeak = peakOf(mh.out);
        mh.chord(mh.bars(1), 45, mh.bars(1) - 100);
        mh.runBars(2);
        snprintf(buf, sizeof buf, "%.1f dBFS with no note", db(silentPeak));
        check(silentPeak == 0.0f && mh.log.size() > 0 && mh.log.front().frame >= mh.bars(1), "Held: silent with no note, plays while one is held", buf);

        // A chord note 10 ms late still catches the downbeat (Held mode: starts late).
        OCKMDRIFT l; l.set(kPlay, 1); Mpc ml(l);
        ml.chord(ml.bars(1) + (long long)(0.010 * SR), 41, ml.bars(1));
        ml.runBars(2);
        bool caught = false;
        for (const auto& e : ml.log) if (e.step == 0 && e.late && e.note % 12 == 5) caught = true;
        check(caught, "a chord note 10 ms late still plays the downbeat (Held)");

        // Always mode: a late note re-pitches the sounding downbeat. Simple tone to measure pitch.
        auto pitchAt = [](const std::vector<float>& x, size_t a, size_t b, double f) {
            double s = 0, c = 0; for (size_t i = a; i < b; ++i) { s += x[i] * std::sin(2 * M_PI * f * i / SR); c += x[i] * std::cos(2 * M_PI * f * i / SR); }
            return std::sqrt(s * s + c * c) / (b - a); };
        OCKMDRIFT r; r.set(kMutate, 0); r.set(kCutoff, 70); r.set(kEnvAmt, 0); r.set(kLength, 100); r.set(kSlide, 0); r.set(kWave, 0); r.set(kSub, 0);
        Mpc mr(r); mr.chord(mr.bars(1) + (long long)(0.010 * SR), 41, mr.bars(1)); mr.runBars(2);
        const size_t w0 = (size_t)(mr.bars(1) + 0.030 * SR), w1 = (size_t)(mr.bars(1) + 0.110 * SR);
        const double fNew = 440.0 * std::pow(2.0, (65 - 69) / 12.0), fOld = 440.0;
        const double pNew = pitchAt(mr.out, w0, w1, fNew), pOld = pitchAt(mr.out, w0, w1, fOld);
        snprintf(buf, sizeof buf, "F %.4f vs A %.4f", pNew, pOld);
        check(pNew > 3 * pOld, "a chord note 10 ms late re-pitches the sounding downbeat (Always)", buf);
    }

    // ---------------- Rhythm ----------------
    {
        bool even = true, all = true, none = true;
        for (int hits = 1; hits <= 16; ++hits)
            for (int rot = 0; rot < 16; ++rot)
            {
                OCKMDRIFT a; a.set(kHits, hits); a.set(kRotate, rot);
                std::vector<int> on;
                for (int i = 0; i < kSteps; ++i) if (a.stepNote(i) >= 0) on.push_back(i);
                if ((int)on.size() != hits) { even = false; continue; }
                if (std::find(on.begin(), on.end(), rot) == on.end()) even = false;      // first hit at Rotate
                int mn = 99, mx = 0;
                for (size_t k = 0; k < on.size(); ++k)
                {
                    const int gap = (on[(k + 1) % on.size()] - on[k] + 16) % 16;
                    mn = std::min(mn, gap == 0 ? 16 : gap); mx = std::max(mx, gap == 0 ? 16 : gap);
                }
                if (mx - mn > 1) even = false;
                a.set(kDensity, 100); for (int i = 0; i < kSteps; ++i) if (a.stepNote(i) < 0) all = false;
                a.set(kDensity, 0);   for (int i = 0; i < kSteps; ++i) if (a.stepNote(i) >= 0) none = false;
            }
        check(even, "Density 50%: exactly the Euclidean hits, evenly spread, shifted by Rotate (all 16 x 16)");
        check(all, "Density 100%: every step plays");
        check(none, "Density 0%: no step plays");
        OCKMDRIFT e; e.set(kHits, 7);
        std::string pat; for (int i = 0; i < kSteps; ++i) pat += e.stepNote(i) >= 0 ? 'x' : '.';
        check(pat == "x..x.x.x..x.x.x.", "Hits 7 = x..x.x.x..x.x.x.", pat.c_str());
    }

    // ---------------- Volume (parameter 0) is harmless ----------------
    {
        OCKMDRIFT a, b; b.set(kVolume, 0.0f);
        Mpc ma(a), mb(b);
        mb.perBlock = [](Mpc& m) { m.p.setNormalised(0, (m.frame / 128) % 2 ? 1.0f : 0.0f); };
        ma.runBars(8); mb.runBars(8);
        bool same = ma.log.size() == mb.log.size();
        for (size_t i = 0; same && i < ma.log.size(); ++i) same = ma.log[i].note == mb.log[i].note && ma.log[i].frame == mb.log[i].frame;
        check(same, "Volume (param 0) wiggled: the line is unchanged");
    }

    // ---------------- No clipping across presets ----------------
    float worstPreset = 0.0f;
    {
        for (int pr = 0; pr < kNumPresets; ++pr)
        {
            OCKMDRIFT a; a.setProgram(pr);
            Mpc m(a); progression(m, m.bars(8), 8);
            m.runBars(16);
            const float pk = peakOf(m.out);
            worstPreset = std::max(worstPreset, pk);
            snprintf(buf, sizeof buf, "%-15s peak %6.1f dBFS, %4zu notes", kPresetNames[pr], db(pk), m.log.size());
            check(pk < 0.89f && finiteAll(m.out) && m.log.size() > 8, "preset", buf);
        }
        bool ok = true; float worst = 0;
        for (int oct : {-2, 2})
        {
            OCKMDRIFT w;
            w.set(kReso, 100); w.set(kDrive, 100); w.set(kAccent, 100); w.set(kEnvAmt, 100); w.set(kVolume, 6);
            w.set(kCutoff, 100); w.set(kOctave, oct); w.set(kRange, 3); w.set(kHits, 16); w.set(kSub, 100); w.set(kWave, 100);
            w.set(kMutate, 100); w.set(kEvolve, 0); w.set(kCutDrift, 100);
            Mpc m(w); m.runBars(8);
            worst = std::max(worst, peakOf(m.out));
            if (!(peakOf(m.out) < 1.0f) || !finiteAll(m.out)) ok = false;
        }
        snprintf(buf, sizeof buf, "peak %.2f dBFS", db(worst));
        check(ok, "worst case (everything at 100%, Volume +6 dB, Octave -2 and +2) stays below 0 dBFS", buf);
    }

    // ---------------- Resonance stable ----------------
    {
        OCKMDRIFT a; a.set(kReso, 100); a.set(kHits, 16); a.set(kRange, 3);
        Mpc m(a);
        m.perBlock = [](Mpc& mm) { mm.p.set(kCutoff, (float)(50.0 + 50.0 * std::sin(mm.frame * 2e-5))); };
        m.runBars(16);
        snprintf(buf, sizeof buf, "peak %.1f dBFS", db(peakOf(m.out)));
        check(finiteAll(m.out) && peakOf(m.out) < 1.0f, "Resonance 100% swept across the cutoff: finite, bounded", buf);
        m.perBlock = nullptr; m.playing = false; m.runBars(4);
        check(peakOf(m.out, m.out.size() - SR) == 0.0f, "...and decays to exact silence after Stop (no denormal tail)");
    }

    // ---------------- CPU ----------------
    {
        OCKMDRIFT a; a.set(kHits, 16); a.set(kDrive, 60); a.set(kCutDrift, 50);
        Mpc m(a);
        const auto t0 = std::chrono::steady_clock::now();
        m.run(60LL * SR);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        snprintf(buf, sizeof buf, "%.3f%% of one desktop core (60 s of 16ths rendered in %.3f s)", 100.0 * sec / 60.0, sec);
        check(sec / 60.0 < 0.004, "CPU", buf);
    }

    // ---------------- Demo WAV ----------------
    if (argc > 1)
    {
        std::vector<float> L, R;
        auto add = [&](const Mpc& m) { L.insert(L.end(), m.out.begin(), m.out.end()); R.insert(R.end(), m.outR.begin(), m.outR.end()); };
        { OCKMDRIFT a; Mpc m(a); m.runBars(16); m.playing = false; m.runBars(0.5); add(m); }
        for (int pr = 1; pr < kNumPresets; ++pr) { OCKMDRIFT a; a.setProgram(pr); Mpc m(a); m.runBars(8); m.playing = false; m.runBars(0.5); add(m); }
        { OCKMDRIFT a; a.set(kMutate, 20); Mpc m(a); progression(m, 0, 8); m.runBars(8); m.playing = false; m.runBars(1); add(m); }
        writeWav(argv[1], L, R);
        snprintf(buf, sizeof buf, "%s, %.1f s, peak %.1f dBFS", argv[1], L.size() / (double)SR, db(peakOf(L)));
        check(peakOf(L) < 1.0f, "demo written", buf);
    }

    printf(gFails ? "\n%d FAILED\n" : "\nALL PASS\n", gFails);
    return gFails ? 1 : 0;
}
