/* Offline x86 test of decks_vst.cpp (test.sh builds it with ASan/UBSan). Writes its own test
 * tracks into a temp folder with two "drives" (DECKS_ROOTS) and checks: drive list, folder
 * browser and USE FOLDER, the scan (subfolders, sorting, umlauts), the tempo/grid analysis
 * (128 BPM with an intro, 140 BPM mono at 48 kHz, a track without beat), loading onto a deck,
 * PLAY quantised to the bar and phase-locked to the Force clock (kicks on the host's beats),
 * NUDGE, a Force loop jump (deck keeps running), SYNC off + PITCH, free running with the
 * transport stopped, CUE, loading onto a playing deck refused, the pads (notes 36-47), the
 * project chunk (settings + tracks), a drive going offline and coming back, a random stress run
 * and NaN/denormal-free output. With "bench" as the second argument it only measures CPU load.
 * Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

static VstTimeInfo g_ti;
static bool g_play = false, g_frozen = false;   /* frozen: transport "playing" but the position stands still */
static double g_ppq = 0, g_tempo = 140;
static int g_automates = 0, g_updates = 0;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op == 0) g_automates++;
    if (op == 42) g_updates++;
    if (op != 7) return 0;
    std::memset(&g_ti, 0, sizeof g_ti);
    g_ti.ppqPos = g_ppq; g_ti.tempo = g_tempo;
    g_ti.flags = (1 << 9) | (1 << 10) | (g_play ? 1 << 1 : 0);
    return (intptr_t)&g_ti;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static const double SR = 44100;
static const int BS = 128;
static bool bad = false;
typedef std::vector<float> Buf;

/* ---- test tracks ---- */
static void write_wav(const std::string &path, const std::vector<int16_t> &pcm, int ch, int sr) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("FAIL: cannot write %s\n", path.c_str()); fails++; return; }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t data = (uint32_t)(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16((uint16_t)ch); u32((uint32_t)sr); u32((uint32_t)(sr * ch * 2)); u16((uint16_t)(ch * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}
static uint32_t rs = 12345;
static float noise() { rs = rs * 1664525u + 1013904223u; return (float)((int32_t)rs) / 2147483648.0f; }
/* kick on every beat from t0, offbeat hats, a bass on the offbeats: a minimal techno loop */
static std::vector<int16_t> techno(double bpm, double t0, double secs, int sr, int ch) {
    const int64_t n = (int64_t)(secs * sr);
    std::vector<float> x((size_t)n, 0.0f);
    const double spb = 60.0 / bpm;
    for (double t = t0; t < secs; t += spb) {
        const int64_t s0 = (int64_t)std::llround(t * sr);
        for (int64_t i = 0; i < (int64_t)(0.25 * sr) && s0 + i < n; i++) {   /* kick: pitch drop 150 -> 50 Hz */
            const double tt = (double)i / sr;
            const double ph = 2 * M_PI * (50 * tt + 100 * 0.03 * (1 - std::exp(-tt / 0.03)));
            x[(size_t)(s0 + i)] += 0.8f * (float)(std::sin(ph) * std::exp(-tt / 0.12));
        }
        const int64_t h0 = (int64_t)std::llround((t + spb / 2) * sr);   /* hat */
        for (int64_t i = 0; i < (int64_t)(0.04 * sr) && h0 + i < n; i++) x[(size_t)(h0 + i)] += 0.15f * noise() * (float)std::exp(-(double)i / sr / 0.01);
        for (int64_t i = 0; i < (int64_t)(spb / 2 * sr) && h0 + i < n; i++) {   /* bass */
            const double tt = (double)i / sr;
            x[(size_t)(h0 + i)] += 0.25f * (float)std::sin(2 * M_PI * 55 * tt) * (float)std::exp(-tt / 0.08);
        }
    }
    std::vector<int16_t> out((size_t)(n * ch));
    for (int64_t i = 0; i < n; i++) {
        const float v = std::max(-1.0f, std::min(1.0f, x[(size_t)i]));
        for (int c = 0; c < ch; c++) out[(size_t)(i * ch + c)] = (int16_t)std::lround(v * 30000);
    }
    return out;
}
static std::vector<int16_t> pad(double secs, int sr) {   /* slow chord, no beat */
    const int64_t n = (int64_t)(secs * sr);
    std::vector<int16_t> out((size_t)(n * 2));
    for (int64_t i = 0; i < n; i++) {
        const double t = (double)i / sr;
        const double v = 0.2 * (std::sin(2 * M_PI * 220 * t) + std::sin(2 * M_PI * 277.2 * t) + std::sin(2 * M_PI * 329.6 * t)) * (0.6 + 0.4 * std::sin(2 * M_PI * 0.05 * t));
        out[(size_t)(2 * i)] = out[(size_t)(2 * i + 1)] = (int16_t)std::lround(v * 20000);
    }
    return out;
}

/* ---- plugin access ---- */
static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[300] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
static void setn(AEffect *e, const char *name, float n) { e->setParameter(e, param(e, name), n); }
/* a tap on a drawn button; 260 ms apart, as the plugin ignores a second press within 250 ms */
static void press(AEffect *e, const char *name) {
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    setn(e, name, 1); setn(e, name, 0);
}
/* a tap on a list row: MPC's Toggle Switch sends the opposite of what the row shows */
static void tap(AEffect *e, const char *name) {
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    const int i = param(e, name); e->setParameter(e, i, e->getParameter(e, i) > 0.5f ? 0.0f : 1.0f);
}
static void send(AEffect *e, int a, int b, int c) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m;
    m.midiData[0] = (unsigned char)a; m.midiData[1] = (unsigned char)b; m.midiData[2] = (unsigned char)c;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    e->dispatcher(e, 25, 0, 0, &ev, 0);
}
static void pad_note(AEffect *e, int n) { send(e, 0x90, n, 100); send(e, 0x80, n, 0); }
static Buf run(AEffect *e, double sec, Buf *right = nullptr) {
    Buf outL, ol(BS), orr(BS);
    float *out[2] = {ol.data(), orr.data()};
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        e->processReplacing(e, nullptr, out, BS);
        if (g_play && !g_frozen) g_ppq += BS * g_tempo / 60.0 / SR;
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            outL.push_back(ol[i]);
            if (right) right->push_back(orr[i]);
        }
    }
    return outL;
}
/* runs blocks (the audio thread swaps tracks) until the display of `name` contains `what` */
static bool wait_for(AEffect *e, const char *name, const char *what, double timeout = 30) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < timeout) {
        if (display(e, name).find(what) != std::string::npos) { run(e, 0.02); return true; }
        run(e, 0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("  (timeout: %s = \"%s\", waiting for \"%s\")\n", name, display(e, name).c_str(), what);
    return false;
}
static double rms(const Buf &a, size_t i0 = 0, size_t i1 = 0) {
    if (!i1) i1 = a.size();
    double s = 0;
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return i1 > i0 ? std::sqrt(s / (i1 - i0)) : 0;
}
/* kick onsets of a rendered signal: steepest rise of the low band, 150 ms apart */
static std::vector<double> kicks(const Buf &a) {
    std::vector<float> env(a.size());
    const float al = std::exp(-2.0f * (float)M_PI * 150.0f / (float)SR), ae = std::exp(-1.0f / (0.001f * (float)SR));
    float l1 = 0, l2 = 0, e = 0, pk = 0;
    for (size_t i = 0; i < a.size(); i++) {
        l1 += (1 - al) * (a[i] - l1); l2 += (1 - al) * (l1 - l2); e += (1 - ae) * (std::fabs(l2) - e);
        env[i] = e; pk = std::max(pk, e);
    }
    std::vector<double> out;   /* the kicks (0.8) rise far more than the bass (0.25) */
    const int D = (int)(0.002 * SR);
    for (size_t i = (size_t)D; i < a.size();) {
        if (env[i] - env[i - (size_t)D] > 0.25f * pk) {
            size_t best = i; float br = 0;
            for (size_t j = i; j < std::min(a.size(), i + (size_t)(0.015 * SR)); j++) {
                const float r = env[j] - env[j - (size_t)D];
                if (r > br) { br = r; best = j; }
            }
            const float top = env[std::min(a.size() - 1, best + (size_t)(0.004 * SR))];
            size_t st = best;   /* back to the start of the attack, as the analysis does */
            while (st > 0 && env[st] > 0.125f * top) st--;
            out.push_back((double)st);
            i = best + (size_t)(0.15 * SR);
        } else i++;
    }
    return out;
}
static std::string read_file(const std::string &p) {
    std::string s;
    if (FILE *f = std::fopen(p.c_str(), "r")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) s.append(b, n); std::fclose(f); }
    return s;
}
/* tempo and first kick of a track in the index file */
static bool index_of(const std::string &data, const std::string &name, double *bpm, double *beat0) {
    const std::string t = read_file(data + "/decks_index.txt");
    size_t p = t.find(name);
    if (p == std::string::npos) return false;
    p = t.find('\t', p);
    long long size, mtime;
    return std::sscanf(t.c_str() + p, "\t%lld\t%lld\t%lf\t%lf", &size, &mtime, bpm, beat0) == 4;
}

static void bench(const char *so) {
    void *h = dlopen(so, RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); std::exit(1); }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = entry(master);
    e->dispatcher(e, 10, 0, 0, nullptr, (float)SR);
    if (!wait_for(e, "Status", "TRACKS", 20)) { std::printf("bench: no tracks\n"); std::exit(1); }
    pad_note(e, 45); pad_note(e, 43); wait_for(e, "Title B", "mono");                 /* list: ambient, mono, Gruesse, techno */
    pad_note(e, 45); pad_note(e, 45); pad_note(e, 39); wait_for(e, "Title A", "techno");
    g_play = true; g_ppq = 0; g_tempo = 142;
    pad_note(e, 36); pad_note(e, 40);
    run(e, 3.0);
    Buf ol(BS), orr(BS);
    float *out[2] = {ol.data(), orr.data()};
    const int nb = (int)(20 * SR / BS);
    double worst = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < nb; b++) {
        const auto a = std::chrono::steady_clock::now();
        e->processReplacing(e, nullptr, out, BS);
        g_ppq += BS * g_tempo / 60.0 / SR;
        worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count());
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench (this CPU, one core): two decks playing, synced  %.2f %%  (worst block %.1f us of %.0f us)\n",
                100 * s / 20.0, worst * 1e6, BS / SR * 1e6);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::printf("PASSED\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so [bench]\n"); return 2; }
    char tmpl[] = "/tmp/decks_test_XXXXXX";
    const std::string root = mkdtemp(tmpl);
    const std::string d1 = root + "/drive1", d2 = root + "/drive2", data = root + "/data";
    for (const std::string &p : {d1, d2, data, d1 + "/Music", d1 + "/Music/Goa", d1 + "/Other", d2 + "/Set"}) mkdir(p.c_str(), 0755);
    write_wav(d1 + "/Music/techno 128.wav", techno(128, 1.234, 120, 44100, 2), 2, 44100);
    write_wav(d1 + "/Music/Goa/mono 140 48k.wav", techno(140, 0.5, 90, 48000, 1), 1, 48000);
    write_wav(d1 + "/Music/ambient pad.wav", pad(40, 44100), 2, 44100);
    write_wav(d1 + "/Music/Gr\xc3\xbc\xc3\x9f" "e.wav", techno(132, 0.2, 30, 44100, 2), 2, 44100);
    write_wav(d1 + "/Other/not here.wav", pad(12, 44100), 2, 44100);
    write_wav(d2 + "/Set/set 135.wav", techno(135, 0.1, 40, 44100, 2), 2, 44100);
    { FILE *f = std::fopen((d1 + "/Music/readme.txt").c_str(), "w"); if (f) { std::fputs("not audio", f); std::fclose(f); } }
    setenv("DECKS_ROOTS", (d1 + ":" + d2).c_str(), 1);
    setenv("DECKS_DATA", data.c_str(), 1);
    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        /* a source must exist: write the sources file directly */
        FILE *f = std::fopen((data + "/decks_sources.txt").c_str(), "w");
        std::fprintf(f, "%s\t%s\n", d1.c_str(), (d1 + "/Music").c_str());
        std::fclose(f);
        bench(argv[1]);
        return 0;
    }

    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    AEffect *(*entry)(audioMasterCallback) = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = entry(master);
    CHECK(e && e->magic == 0x56737450, "magic");
    CHECK(e->numInputs == 0 && e->numOutputs == 2 && (e->flags & (1 << 8)), "instrument, 0 in / 2 out");
    e->dispatcher(e, 10, 0, 0, nullptr, (float)SR);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);
    CHECK(display(e, "Clock") == "DECKS" && display(e, "Glide") == "4 BARS", "defaults: own clock, glide 4 bars");
    setn(e, "Clock", 1);   /* the first part runs on the Force clock */
    run(e, 0.05);

    /* -- drives, folder browser, USE FOLDER -- */
    CHECK(wait_for(e, "Status", "NO MUSIC FOLDER"), "no source at first: %s", display(e, "Status").c_str());
    CHECK(display(e, "Drive").find("DRIVE 1/2:  drive1") != std::string::npos, "drive list: %s", display(e, "Drive").c_str());
    run(e, 0.1);
    CHECK(wait_for(e, "Folder Row 1", "> Music"), "folder rows: %s", display(e, "Folder Row 1").c_str());
    CHECK(display(e, "Folder Row 2") == "> Other", "second folder: %s", display(e, "Folder Row 2").c_str());
    tap(e, "Folder Row 1");   /* tap: open Music */
    CHECK(wait_for(e, "Path", "drive1:/Music"), "opened Music: %s", display(e, "Path").c_str());
    CHECK(display(e, "Folder Row 1") == "> Goa", "Music's subfolder: %s", display(e, "Folder Row 1").c_str());
    press(e, "Up");
    CHECK(wait_for(e, "Path", "drive1:/   (2"), "UP: %s", display(e, "Path").c_str());
    run(e, 0.05);
    CHECK(e->getParameter(e, param(e, "Folder Row 1")) > 0.5f, "first folder selected");
    CHECK(display(e, "Folder") == "1 / 2", "FOLDER knob text: %s", display(e, "Folder").c_str());
    setn(e, "Folder", 1.0f);   /* knob: the second folder ... */
    run(e, 0.05);
    CHECK(e->getParameter(e, param(e, "Folder Row 2")) > 0.5f && display(e, "Folder") == "2 / 2", "knob selects Other: %s", display(e, "Folder").c_str());
    press(e, "Open Folder");   /* ... and OPEN */
    CHECK(wait_for(e, "Path", "drive1:/Other"), "OPEN: %s", display(e, "Path").c_str());
    press(e, "Up");
    CHECK(wait_for(e, "Path", "drive1:/   (2"), "UP again: %s", display(e, "Path").c_str());
    press(e, "Up");   /* never above the drive */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    run(e, 0.1);
    CHECK(wait_for(e, "Path", "drive1:/   (2"), "UP stops at the drive: %s", display(e, "Path").c_str());
    tap(e, "Folder Row 1");
    wait_for(e, "Path", "drive1:/Music");
    press(e, "Use Folder");
    CHECK(wait_for(e, "Status", "4 TRACKS"), "scan of Music: %s", display(e, "Status").c_str());
    CHECK(display(e, "Sources").find("drive1:/Music") != std::string::npos, "sources: %s", display(e, "Sources").c_str());
    /* sorted by path, case-insensitive: ambient, Goa/mono, Gruesse, techno */
    CHECK(display(e, "Row 1").find("ambient pad") != std::string::npos, "row 1: %s", display(e, "Row 1").c_str());
    CHECK(display(e, "Row 2").find("mono 140 48k") != std::string::npos, "row 2: %s", display(e, "Row 2").c_str());
    CHECK(display(e, "Row 3").find("Gruesse") != std::string::npos, "umlauts spelled out: %s", display(e, "Row 3").c_str());
    CHECK(display(e, "Row 4").find("techno 128") != std::string::npos, "row 4: %s", display(e, "Row 4").c_str());
    CHECK(display(e, "Row 5").empty(), "row 5 empty");
    CHECK(e->getParameter(e, param(e, "Row 1")) > 0.5f, "row 1 selected at first");

    /* -- select by tapping a row, load A, analysis -- */
    tap(e, "Row 4");
    run(e, 0.05);
    CHECK(e->getParameter(e, param(e, "Row 4")) > 0.5f && e->getParameter(e, param(e, "Row 1")) < 0.5f, "tap selects row 4");
    CHECK(display(e, "Track") == "4 / 4", "TRACK knob follows: %s", display(e, "Track").c_str());
    press(e, "Load A");
    CHECK(wait_for(e, "Title A", "techno 128"), "deck A loaded");
    double bpm = 0, beat0 = 0;
    CHECK(index_of(data, "techno 128.wav", &bpm, &beat0), "index has the analysis");
    CHECK(std::fabs(bpm - 128) < 0.005, "tempo 128: %.4f", bpm);
    CHECK(std::fabs(beat0 / 44100 - 1.234) < 0.003, "first kick 1.234 s: %.4f s", beat0 / 44100);
    CHECK(display(e, "Row 4").find("128.00") != std::string::npos && display(e, "Row 4").find("[A]") != std::string::npos,
          "row shows tempo and [A]: %s", display(e, "Row 4").c_str());
    CHECK(display(e, "Info A").find("STOP") == 0, "stopped after loading: %s", display(e, "Info A").c_str());

    /* -- PLAY quantised to the bar, phase-locked to the Force -- */
    g_tempo = 140; g_play = true; g_ppq = 0.3;
    run(e, 0.1);
    press(e, "Play A");
    run(e, 0.05);
    CHECK(display(e, "Info A").find("WAIT BAR") == 0, "waits for the bar: %s", display(e, "Info A").c_str());
    const double ppq_start = g_ppq;
    Buf a = run(e, 8.0);
    CHECK(display(e, "Info A").find("PLAY") == 0 && display(e, "Info A").find("128.00 > 140.00 BPM") != std::string::npos,
          "plays at 140: %s", display(e, "Info A").c_str());
    {
        std::vector<double> k = kicks(a);
        const double fpb = SR * 60.0 / g_tempo;
        const double first_beat = (4.0 - ppq_start) * fpb;   /* bar 2 of the song, in frames of this buffer */
        CHECK(!k.empty() && std::fabs(k[0] - first_beat) < 0.003 * SR, "first kick on the bar: %.1f ms off",
              k.empty() ? 1e9 : (k[0] - first_beat) / SR * 1000);
        double worst = 0;
        int nk = 0;
        for (double x : k) {
            const double b = x / fpb + ppq_start;
            worst = std::max(worst, std::fabs(b - std::round(b)) * 60000.0 / g_tempo);
            nk++;
        }
        CHECK(nk >= 14, "kicks while playing: %d", nk);
        CHECK(worst < 3.0, "every kick on a Force beat: worst %.2f ms", worst);
        std::printf("  sync: %d kicks, worst %.2f ms from the Force beat\n", nk, worst);
    }
    {   /* readouts are re-read by MPC only on UpdateDisplay: a playing deck must ask for them, at most 10 a second */
        g_updates = 0;
        run(e, 4.0);
        CHECK(g_updates >= 8 && g_updates <= 41, "UpdateDisplay while playing: %d in 4 s", g_updates);
    }
    CHECK(display(e, "Position A").find("BAR ") != std::string::npos && display(e, "Position A").find("LOCK") != std::string::npos,
          "position readout: %s", display(e, "Position A").c_str());

    /* -- NUDGE: +0.1 beat moves the kicks earlier by 0.1 beat -- */
    setn(e, "Nudge A", 0.6f);   /* -50..50 -> +10 = 0.1 beat */
    run(e, 3.0);
    {
        const double p0 = g_ppq;
        Buf b = run(e, 3.0);
        std::vector<double> k = kicks(b);
        const double fpb = SR * 60.0 / g_tempo;
        double s = 0; int n = 0;
        for (double x : k) { double bb = x / fpb + p0; double d = bb - std::round(bb); s += d; n++; }
        CHECK(n > 4 && std::fabs(s / n + 0.1) < 0.01, "nudge +0.1 beat: kicks %.3f beat from the grid", n ? s / n : 0);
    }
    setn(e, "Nudge A", 0.5f);
    run(e, 3.0);

    /* -- a Force loop: the song jumps back 8 beats, the deck keeps playing its track -- */
    {
        const std::string before = display(e, "Info A");
        g_ppq -= 8;
        const double p0 = g_ppq;
        Buf b = run(e, 2.0);
        std::vector<double> k = kicks(b);
        const double fpb = SR * 60.0 / g_tempo;
        double worst = 0;
        for (double x : k) { double bb = x / fpb + p0; worst = std::max(worst, std::fabs(bb - std::round(bb))); }
        CHECK(worst * 60000 / g_tempo < 3.0, "after the loop jump still on the beat: %.2f ms", worst * 60000 / g_tempo);
        const std::string after = display(e, "Info A");
        /* the time readout went on (~ +2 s), no jump back */
        CHECK(after.find("PLAY") == 0, "still playing after the loop: %s", after.c_str());
        std::printf("  loop: %s -> %s\n", before.c_str(), after.c_str());
    }

    /* -- loading onto a playing deck is refused -- */
    tap(e, "Row 1");
    run(e, 0.05);
    press(e, "Load A");
    CHECK(wait_for(e, "Status", "IS PLAYING"), "load onto a playing deck refused: %s", display(e, "Status").c_str());
    CHECK(display(e, "Title A").find("techno 128") != std::string::npos, "deck A keeps its track");

    /* -- deck B: 140 BPM mono at 48 kHz, free running with the transport stopped -- */
    tap(e, "Row 2");
    run(e, 0.05);
    press(e, "Load B");
    CHECK(wait_for(e, "Title B", "mono 140"), "deck B loaded");
    CHECK(index_of(data, "mono 140 48k.wav", &bpm, &beat0) && std::fabs(bpm - 140) < 0.005, "tempo 140 (48 kHz mono): %.4f", bpm);
    CHECK(std::fabs(beat0 / 48000 - 0.5) < 0.003, "first kick 0.5 s: %.4f", beat0 / 48000);
    g_play = false;
    run(e, 0.1);
    CHECK(display(e, "Info A").find("FREE") == 0, "deck A runs free with the transport stopped: %s", display(e, "Info A").c_str());
    setn(e, "Crossfader", 1.0f);   /* only B */
    pad_note(e, 40);               /* pad: PLAY B, starts at once (no clock) */
    run(e, 0.05);
    CHECK(display(e, "Info B").find("FREE") == 0, "deck B plays free: %s", display(e, "Info B").c_str());
    {
        Buf b = run(e, 4.0);
        std::vector<double> k = kicks(b);
        CHECK(k.size() >= 8, "deck B kicks: %d", (int)k.size());
        if (k.size() >= 8) {
            const double per = (k.back() - k[1]) / (double)(k.size() - 2);
            CHECK(std::fabs(per - SR * 60 / 140.0) < 0.002 * SR, "deck B at the last tempo (140): %.1f frames per beat", per);
        }
        CHECK(rms(b) > 0.02, "deck B audible through the crossfader");
    }
    /* SYNC off + PITCH +5 %: 140 * 1.05 */
    setn(e, "Sync B", 0); setn(e, "Pitch B", (5.0f + 8) / 16);
    run(e, 0.2);
    {
        Buf b = run(e, 4.0);
        std::vector<double> k = kicks(b);
        if (k.size() >= 8) {
            const double per = (k.back() - k[1]) / (double)(k.size() - 2);
            CHECK(std::fabs(per - SR * 60 / 147.0) < 0.002 * SR, "SYNC off, PITCH +5 %%: %.1f frames per beat", per);
        } else CHECK(false, "deck B kicks with pitch");
        CHECK(display(e, "Info B").find("140.00 > 147.00") != std::string::npos, "info with pitch: %s", display(e, "Info B").c_str());
    }
    /* CUE while playing: back to the first kick, stopped */
    pad_note(e, 41);
    run(e, 0.1);
    CHECK(display(e, "Info B").find("STOP") == 0 && display(e, "Info B").find(" 0:00 /") != std::string::npos, "CUE: %s", display(e, "Info B").c_str());
    CHECK(rms(run(e, 0.5)) < 1e-6, "silent: A stopped by the crossfader, B by CUE");
    setn(e, "Crossfader", 0.5f);
    pad_note(e, 38);   /* pad SYNC A off -> back on below */
    run(e, 0.05);
    CHECK(display(e, "Sync A") == "OFF", "pad 38 toggles SYNC A: %s", display(e, "Sync A").c_str());
    pad_note(e, 38);
    run(e, 0.05);
    press(e, "Play A");   /* pause */
    run(e, 0.1);
    CHECK(display(e, "Info A").find("STOP") == 0, "PLAY pauses: %s", display(e, "Info A").c_str());
    {   /* MPC delivers one tap as two presses, 30-140 ms apart, no release between: one action */
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
        const int pa = param(e, "Play A");
        e->setParameter(e, pa, 1); run(e, 0.03);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        e->setParameter(e, pa, 1); run(e, 0.05);
        CHECK(display(e, "Info A").find("STOP") != 0, "a doubled tap starts A once (not start + stop): %s", display(e, "Info A").c_str());
        press(e, "Play A");
        run(e, 0.05);
        CHECK(display(e, "Info A").find("STOP") == 0, "the next real tap stops it: %s", display(e, "Info A").c_str());
    }

    /* -- no beat: the ambient pad -- */
    pad_note(e, 45); pad_note(e, 44); pad_note(e, 44); pad_note(e, 44);   /* 2 -> 3 -> 2 -> 1 -> 1 */
    run(e, 0.05);
    CHECK(display(e, "Track") == "1 / 4", "pads move the selection: %s", display(e, "Track").c_str());
    pad_note(e, 43);
    CHECK(wait_for(e, "Title B", "ambient"), "ambient on B");
    CHECK(index_of(data, "ambient pad.wav", &bpm, &beat0) && bpm == 0, "no tempo for the pad: %.3f", bpm);
    CHECK(display(e, "Info B").find("NO TEMPO") != std::string::npos, "info: %s", display(e, "Info B").c_str());
    CHECK(display(e, "Row 1").find("---") != std::string::npos, "row shows no tempo: %s", display(e, "Row 1").c_str());

    /* -- second drive as a second source, then offline and back -- */
    press(e, "Drive Next");
    run(e, 0.1);
    CHECK(wait_for(e, "Path", "drive2:/"), "drive 2: %s", display(e, "Path").c_str());
    tap(e, "Folder Row 1");
    wait_for(e, "Path", "drive2:/Set");
    press(e, "Use Folder");
    CHECK(wait_for(e, "Status", "5 TRACKS"), "two sources: %s", display(e, "Status").c_str());
    CHECK(rename(d2.c_str(), (root + "/gone").c_str()) == 0, "rename");
    CHECK(wait_for(e, "Status", "(1 OFFLINE)", 10), "drive 2 offline: %s", display(e, "Status").c_str());
    CHECK(display(e, "Sources").find("(OFFLINE)") != std::string::npos, "sources mark it: %s", display(e, "Sources").c_str());
    {
        bool seen = false;
        pad_note(e, 47);   /* a page on: past the end, so the last track (set 135, row 5) */
        run(e, 0.05);
        CHECK(display(e, "Track") == "5 / 5", "page down stops at the end: %s", display(e, "Track").c_str());
        for (const char *r : {"Row 4", "Row 5"}) seen |= display(e, r).find("OFFLINE  set 135") != std::string::npos;
        CHECK(seen, "offline track listed: %s", display(e, "Row 5").c_str());
        pad_note(e, 39);
        CHECK(wait_for(e, "Status", "OFFLINE: set 135"), "offline track not loaded: %s", display(e, "Status").c_str());
    }
    CHECK(rename((root + "/gone").c_str(), d2.c_str()) == 0, "rename back");
    CHECK(wait_for(e, "Status", "5 TRACKS", 10), "drive 2 back: %s", display(e, "Status").c_str());

    /* -- project chunk -- */
    setn(e, "Volume A", 0.55f); setn(e, "Quantize", 0.5f); setn(e, "Sync B", 0);
    run(e, 0.05);
    void *ck = nullptr;
    const intptr_t len = e->dispatcher(e, 23, 0, 0, &ck, 0);
    std::string chunk((const char *)ck, (size_t)len);
    CHECK(chunk.find("path_a=" + d1 + "/Music/techno 128.wav") != std::string::npos, "chunk has deck A's track");
    AEffect *e2 = entry(master);
    e2->dispatcher(e2, 10, 0, 0, nullptr, (float)SR);
    e2->dispatcher(e2, 24, 0, (intptr_t)chunk.size(), (void *)chunk.data(), 0);
    CHECK(wait_for(e2, "Title A", "techno 128") && wait_for(e2, "Title B", "ambient"), "chunk reloads both tracks");
    CHECK(display(e2, "Volume A") == "55 %" && display(e2, "Quantize") == "BEAT" && display(e2, "Sync B") == "OFF",
          "chunk settings: %s %s %s", display(e2, "Volume A").c_str(), display(e2, "Quantize").c_str(), display(e2, "Sync B").c_str());
    e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    CHECK(e->dispatcher(e, 24, 0, 5, (void *)"XXXXX", 0) == 0, "foreign chunk refused");

    /* -- own clock: the deck started last leads at its own tempo -- */
    setn(e, "Clock", 0); setn(e, "Glide", 0); setn(e, "Sync A", 1); setn(e, "Sync B", 1); setn(e, "Pitch A", 0.5f);
    setn(e, "Pitch B", 0.5f); setn(e, "Quantize", 1.0f); setn(e, "Nudge A", 0.5f); setn(e, "Nudge B", 0.5f);
    setn(e, "Crossfader", 0.5f);
    g_play = false;
    pad_note(e, 37);   /* CUE A: back to the first kick */
    tap(e, "Row 2");
    run(e, 0.05);
    pad_note(e, 43);   /* mono 140 onto B */
    CHECK(wait_for(e, "Title B", "mono 140"), "B for the own clock");
    pad_note(e, 41);
    run(e, 0.05);
    pad_note(e, 36);   /* A alone: starts at once, leads at 128 */
    run(e, 0.05);
    CHECK(display(e, "Info A").find("LEAD  128.00 > 128.00") == 0, "A leads at its own tempo: %s", display(e, "Info A").c_str());
    {
        Buf b = run(e, 4.0);
        std::vector<double> k = kicks(b);
        CHECK(k.size() >= 6, "A kicks");
        if (k.size() >= 6) {
            const double per = (k.back() - k[1]) / (double)(k.size() - 2);
            CHECK(std::fabs(per - SR * 60 / 128.0) < 0.002 * SR, "A at 128 without any Force clock: %.1f frames per beat", per);
        }
    }
    pad_note(e, 40);   /* B: waits for A's next bar, then leads at 140; A follows */
    run(e, 0.05);
    CHECK(display(e, "Info B").find("WAIT BAR") == 0, "B waits for the bar: %s", display(e, "Info B").c_str());
    run(e, 2.5);
    CHECK(display(e, "Info B").find("LEAD  140.00 > 140.00") == 0, "B leads: %s", display(e, "Info B").c_str());
    CHECK(display(e, "Info A").find("SYNC  128.00 > 140.00") == 0, "A follows B: %s", display(e, "Info A").c_str());
    {
        Buf b = run(e, 4.0);
        std::vector<double> k = kicks(b);   /* both decks: one kick per beat if they line up */
        CHECK(k.size() >= 6, "A+B kicks");
        if (k.size() >= 6) {
            const double per = (k.back() - k[1]) / (double)(k.size() - 2);
            CHECK(std::fabs(per - SR * 60 / 140.0) < 0.002 * SR, "A and B on one beat at 140: %.1f frames per beat (%d kicks)", per, (int)k.size());
        }
    }
    setn(e, "Glide", 0.4f);   /* 2 BARS */
    run(e, 0.05);
    pad_note(e, 40);           /* B stops: A takes the lead and glides back to its 128 */
    run(e, 0.5);
    {
        const std::string mid = display(e, "Info A");
        CHECK(mid.find("LEAD") == 0 && mid.find("> 128.00") == std::string::npos && mid.find("> 140.00") == std::string::npos,
              "A glides: %s", mid.c_str());
    }
    run(e, 4.0);
    CHECK(display(e, "Info A").find("LEAD  128.00 > 128.00") == 0, "A back at 128 after the glide: %s", display(e, "Info A").c_str());
    pad_note(e, 36);

    /* -- style crates: the first folder below the source -- */
    CHECK(display(e, "Style").find("STYLE 1/3:  ALL") == 0, "crates ALL, Goa, top: %s", display(e, "Style").c_str());
    press(e, "Style Next");
    CHECK(wait_for(e, "Style", "Goa   (1 TRACKS)"), "crate Goa: %s", display(e, "Style").c_str());
    CHECK(display(e, "Row 1").find("mono 140") != std::string::npos && display(e, "Row 2").empty(), "only Goa listed: %s", display(e, "Row 1").c_str());
    setn(e, "Style", 1.0f);   /* the knob: the last crate, the tracks right in the source folder */
    /* top crate: the files right in a source folder - drive1/Music's three and drive2/Set's one */
    CHECK(wait_for(e, "Style", "(TOP FOLDER)   (4 TRACKS)"), "crate top: %s", display(e, "Style").c_str());
    CHECK(display(e, "Row 1").find("ambient") != std::string::npos && display(e, "Row 2").find("Gruesse") != std::string::npos &&
          display(e, "Row 3").find("techno 128") != std::string::npos, "top crate rows");

    /* -- AUTO: the crate in its order, MIX bars of crossfade before a track ends -- */
    setn(e, "Auto Mix", 0.0f);   /* 4 BARS */
    tap(e, "Row 2");             /* start at Gruesse (132 BPM, 30 s) */
    run(e, 0.05);
    pad_note(e, 48);             /* pad: AUTO on */
    CHECK(wait_for(e, "Title A", "Gruesse") && wait_for(e, "Info A", "LEAD"), "AUTO loads the selection onto A and plays it: %s", display(e, "Info A").c_str());
    CHECK(display(e, "Crossfader") == "A 50", "AUTO puts the crossfader on A: %s", display(e, "Crossfader").c_str());
    CHECK(wait_for(e, "Title B", "techno 128"), "AUTO loads the next track onto B");
    CHECK(wait_for(e, "Info B", "AUTO NEXT  in"), "B waits for its turn: %s", display(e, "Info B").c_str());
    {
        bool mixed = false, a_stopped = false;
        for (int i = 0; i < 400 && !a_stopped; i++) {   /* run on through the end of Gruesse */
            run(e, 0.1);
            if (display(e, "Info B").find("PLAY") == 0 || display(e, "Info B").find("LEAD") == 0) {
                const std::string x = display(e, "Crossfader");
                if (x != "A 50" && x != "B 50") mixed = true;
            }
            a_stopped = display(e, "Info B").find("LEAD") == 0 && display(e, "Crossfader") == "B 50" &&
                        (display(e, "Title A").find("Gruesse") == std::string::npos || display(e, "Info A").find("PLAY") != 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(mixed, "the crossfader moved during the mix");
        CHECK(a_stopped, "B took over: %s | %s | %s", display(e, "Info A").c_str(), display(e, "Info B").c_str(), display(e, "Crossfader").c_str());
        std::printf("  auto: %s | %s\n", display(e, "Title B").c_str(), display(e, "Info B").c_str());
    }
    CHECK(wait_for(e, "Title A", "set 135"), "then the next one of the crate onto A: %s", display(e, "Title A").c_str());
    CHECK(rms(run(e, 1.0)) > 0.01, "music goes on");
    pad_note(e, 40);   /* a hand stops the only playing deck: AUTO steps aside */
    run(e, 0.1);
    CHECK(display(e, "Auto") == "OFF", "AUTO off after a manual stop: %s", display(e, "Auto").c_str());
    CHECK(wait_for(e, "Status", "AUTO OFF (STOPPED BY HAND)"), "status says so: %s", display(e, "Status").c_str());
    run(e, 1.0);
    CHECK(display(e, "Info A").find("STOP") == 0 && display(e, "Info B").find("STOP") == 0, "and nothing restarts");
    press(e, "Style Prev"); press(e, "Style Prev");
    wait_for(e, "Style", "ALL");
    setn(e, "Crossfader", 0.5f);

    /* -- Force clock, transport reported as playing but standing still: counts as stopped -- */
    setn(e, "Clock", 1);
    run(e, 0.05);
    g_play = true; g_frozen = true; g_ppq = 17.25;
    run(e, 0.2);
    pad_note(e, 36);
    run(e, 0.1);
    CHECK(display(e, "Info A").find("FREE") == 0, "standing transport = stopped, A plays free: %s", display(e, "Info A").c_str());
    CHECK(rms(run(e, 1.0)) > 0.01, "A audible while the Force stands");
    pad_note(e, 36);
    g_frozen = false;
    setn(e, "Clock", 0);
    run(e, 0.05);

    /* -- random stress -- */
    g_play = true;
    for (int i = 0; i < 400; i++) {
        const int p = (int)(noise() * 0.5f * e->numParams + e->numParams / 2) % e->numParams;
        char nm[64] = {0};
        e->dispatcher(e, 8, p, 0, nm, 0);
        if (!std::strcmp(nm, "Rescan") || !std::strcmp(nm, "Use Folder") || !std::strcmp(nm, "Remove Folder")) continue;
        e->setParameter(e, p, std::fabs(noise()));
        if (i % 7 == 0) pad_note(e, 36 + (int)(std::fabs(noise()) * 12) % 12);
        if (i % 50 == 0) g_ppq += noise() * 16;
        run(e, 0.03);
        char b[300];
        for (int q = 0; q < e->numParams; q++) e->dispatcher(e, 7, q, 0, b, 0);
    }
    run(e, 1.0);
    CHECK(!bad, "NaN, inf or denormal in the output");
    CHECK(g_automates > 0, "the plugin reports to the host");

    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
