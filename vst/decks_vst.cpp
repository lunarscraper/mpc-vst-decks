/* =============================================================================
 * decks_vst.cpp - Decks: a 2-deck DJ player as a VST2 instrument for the MPC OS plugin host
 * (Force, MPC Live/One/X/Key), armhf, after the manner of Mixxx (concepts only, no Mixxx code).
 *   decks_engine.h    a deck: vinyl-style playback, locked to the Force clock
 *   decks_analysis.h  tempo and beatgrid of a track
 *   decks_library.h   drives, music folders, track list, the worker thread
 * This file is the plug-in around them: parameters, the skin's text, pads (MIDI notes 36-47),
 * the mixer (two volumes, crossfader, master), the project chunk (settings + the tracks on the
 * decks). Hand-written VST2 shell as in mpc-vst-rattler, no Steinberg SDK.
 * MIT license (see ../LICENSE). Mixxx is a trademark of its owners; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "decks_library.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
enum { kVstMidiType = 1 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum {
    VOL_A, VOL_B, XFADE, MASTER,
    PLAY_A, CUE_A, SYNC_A, PITCH_A, NUDGE_A,
    PLAY_B, CUE_B, SYNC_B, PITCH_B, NUDGE_B,
    QUANT, TRACK, LOAD_A, LOAD_B, RESCAN,
    ROW_1, ROW_2, ROW_3, ROW_4, ROW_5, ROW_6, ROW_7, ROW_8,
    DRIVE, DRIVE_PREV, DRIVE_NEXT, FOLDER,
    FOLD_1, FOLD_2, FOLD_3, FOLD_4, FOLD_5, FOLD_6,
    UP, USE, REMOVE,
    TITLE_A, INFO_A, BAR_A, TITLE_B, INFO_B, BAR_B, LIB_INFO, STATUS, PATH, SOURCES,
    /* update 0.2, appended */
    CLOCK, GLIDE,
    /* update 0.3, appended */
    OPEN, HELP_1, HELP_2, HELP_3, HELP_4, HELP_5,
    NKEYS
};
static const char *const KEYS[NKEYS] = {
    "vol_a", "vol_b", "xfade", "master",
    "play_a", "cue_a", "sync_a", "pitch_a", "nudge_a",
    "play_b", "cue_b", "sync_b", "pitch_b", "nudge_b",
    "quant", "track", "load_a", "load_b", "rescan",
    "row_1", "row_2", "row_3", "row_4", "row_5", "row_6", "row_7", "row_8",
    "drive", "drive_prev", "drive_next", "folder",
    "fold_1", "fold_2", "fold_3", "fold_4", "fold_5", "fold_6",
    "up", "use", "remove",
    "title_a", "info_a", "bar_a", "title_b", "info_b", "bar_b", "lib_info", "status", "path", "sources",
    "clock", "glide",
    "open", "help_1", "help_2", "help_3", "help_4", "help_5",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours */
static bool is_trigger(int k) {
    return k == PLAY_A || k == CUE_A || k == PLAY_B || k == CUE_B || k == LOAD_A || k == LOAD_B || k == RESCAN ||
           k == DRIVE_PREV || k == DRIVE_NEXT || k == UP || k == USE || k == REMOVE || k == OPEN;
}
static bool is_row(int k) { return k >= ROW_1 && k <= ROW_8; }
static bool is_fold(int k) { return k >= FOLD_1 && k <= FOLD_6; }
static bool is_text(int k) { return (k >= TITLE_A && k <= SOURCES) || (k >= HELP_1 && k <= HELP_5); }
/* what the project chunk keeps: the mixer and the deck settings */
static bool is_setting(int k) {
    return k == VOL_A || k == VOL_B || k == XFADE || k == MASTER || k == SYNC_A || k == PITCH_A || k == NUDGE_A ||
           k == SYNC_B || k == PITCH_B || k == NUDGE_B || k == QUANT || k == CLOCK || k == GLIDE;
}

enum { NEV = 256 };
struct MidiEv { int32_t frame; uint8_t d[3]; };
enum { REQ_PLAY = 1, REQ_CUE = 2, REQ_SYNC = 4 };

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};
    std::atomic<bool> dirty{true};
    decks::Deck deck[2];
    decks::Library lib;
    decks::Clock clk;
    decks::Track *to_retire[2] = {nullptr, nullptr};
    std::atomic<int> req[2] = {{0}, {0}};     /* skin buttons -> audio thread */
    int quant = decks::Q_BAR;
    float gain[2] = {0, 0};
    MidiEv ev[NEV];
    int nev = 0;
    std::atomic<int> row_rep[decks::ROWS];   /* the on-state the host was last told, -1 = tell again */
    std::atomic<int> fold_rep[decks::FROWS];   /* as row_rep, for the folder rows */
    int last_sel = -1, last_count = -1, last_fsel = -1, last_fcount = -1;
    /* for the skin's text, written by the audio thread each block */
    struct Ui {
        std::atomic<int> state{0};             /* 0 empty, 1 stopped, 2 playing, 3 waiting */
        std::atomic<float> pos{0}, beat{0}, eff{0}, rate{1}, err{0};
        std::atomic<bool> locked{false};
    } ui[2];
    std::atomic<bool> rolling{false};
    /* the clock. CLOCK FORCE: the Force's song position. CLOCK DECKS: an own song position
     * (vppq) running at the tempo of the leading deck - the deck started last; the other deck
     * follows it with SYNC on. GLIDE moves the tempo from the old leader's to the new one's. */
    bool force_clock = false;
    int leader = -1;
    double vppq = 0, tempo = 120;
    double glide_from = 120, glide_len = 0, glide_pos = 0;
    double last_ppq = -1e9;
    bool force_rolling = false;
    double force_tempo = 120;
    std::atomic<int> ui_leader{-1};
    std::atomic<float> ui_tempo{120};
    std::vector<uint8_t> chunk;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float val(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]); }
static float pct(Plugin *w, int k) { return val(w, k) * 0.01f; }
static bool sw(Plugin *w, int k) { return val(w, k) > 0.5f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void set_norm(Plugin *w, int k, float n) {
    const int i = IDX[k];
    if (i < 0) return;
    w->cache[i].store(clamp01(n));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

static void start_values(Plugin *w) {
    start(w, VOL_A, 80); start(w, VOL_B, 80); start(w, XFADE, 50); start(w, MASTER, 80);
    start(w, SYNC_A, 1); start(w, SYNC_B, 1); start(w, PITCH_A, 0); start(w, PITCH_B, 0);
    start(w, NUDGE_A, 0); start(w, NUDGE_B, 0); start(w, QUANT, decks::Q_BAR);
    start(w, CLOCK, 0); start(w, GLIDE, 3);
}

/* knob -> deck (audio thread) */
static void configure(Plugin *w) {
    const bool fc = sw(w, CLOCK);
    if (fc != w->force_clock) {   /* clock switched: both decks lock anew; the own clock starts from the deck playing */
        w->force_clock = fc;
        w->leader = -1;
        for (int d = 0; d < 2; d++) w->deck[d].locked = false;
        if (!fc)
            for (int d = 0; d < 2 && w->leader < 0; d++)
                if (w->deck[d].playing && w->deck[d].t && w->deck[d].t->bpm > 0) {
                    w->leader = d;
                    w->glide_from = w->tempo = w->force_tempo;
                    w->glide_pos = 0;
                    w->glide_len = 16;
                    w->vppq = w->deck[d].beat_at(w->deck[d].pos);
                }
    }
    for (int d = 0; d < 2; d++) {
        decks::Deck &k = w->deck[d];
        k.sync = sw(w, d ? SYNC_B : SYNC_A) || (!w->force_clock && d == w->leader);
        k.pitch = val(w, d ? PITCH_B : PITCH_A);
        k.nudge = val(w, d ? NUDGE_B : NUDGE_A) * 0.01f;
    }
    w->quant = clampi((int)val(w, QUANT), 0, 2);
    /* crossfader: both decks full in the middle, one side fades out towards the other end */
    const float x = pct(w, XFADE);
    const float xa = std::cos((float)M_PI * std::max(0.0f, x - 0.5f)), xb = std::cos((float)M_PI * std::max(0.0f, 0.5f - x));
    const float m = pct(w, MASTER) * pct(w, MASTER);
    w->gain[0] = pct(w, VOL_A) * pct(w, VOL_A) * xa * m;
    w->gain[1] = pct(w, VOL_B) * pct(w, VOL_B) * xb * m;
}

/* ---- the track list: selection, rows ---------------------------------------- */
static void select_track(Plugin *w, int s) {
    const int n = w->lib.count.load();
    s = n > 0 ? clampi(s, 0, n - 1) : 0;
    w->lib.sel.store(s);
}
static int top_row(Plugin *w) { return (w->lib.sel.load() / decks::ROWS) * decks::ROWS; }

/* ---- pads ------------------------------------------------------------------------ */
static void midi(Plugin *w, const uint8_t *d) {
    const int st = d[0] & 0xf0, n = d[1] & 0x7f;
    if (st != 0x90 || d[2] == 0) return;
    switch (n) {
    case 36: w->req[0].fetch_or(REQ_PLAY); break;
    case 37: w->req[0].fetch_or(REQ_CUE); break;
    case 38: w->req[0].fetch_or(REQ_SYNC); break;
    case 39: w->lib.load_req[0].store(w->lib.sel.load() + 1); break;
    case 40: w->req[1].fetch_or(REQ_PLAY); break;
    case 41: w->req[1].fetch_or(REQ_CUE); break;
    case 42: w->req[1].fetch_or(REQ_SYNC); break;
    case 43: w->lib.load_req[1].store(w->lib.sel.load() + 1); break;
    case 44: select_track(w, w->lib.sel.load() - 1); break;
    case 45: select_track(w, w->lib.sel.load() + 1); break;
    case 46: select_track(w, w->lib.sel.load() - decks::ROWS); break;
    case 47: select_track(w, w->lib.sel.load() + decks::ROWS); break;
    default: break;
    }
}

/* the tempo moves from where it is to the new leader's over GLIDE (OFF, 1 .. 16 bars) */
static void start_glide(Plugin *w) {
    static const double BEATS[6] = {0, 4, 8, 16, 32, 64};
    w->glide_from = w->tempo;
    w->glide_len = BEATS[clampi((int)val(w, GLIDE), 0, 5)];
    w->glide_pos = 0;
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->dirty.exchange(false)) configure(w);
    float *L = out[0], *R = out[1];
    std::memset(L, 0, sizeof(float) * (size_t)n);
    std::memset(R, 0, sizeof(float) * (size_t)n);

    /* the Force's clock. "Rolling" means the song position moves: a host may report its
     * transport as playing while the position stands still, and that must count as stopped. */
    const VstTimeInfo *ti = (const VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0.0f);
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo > 20 && ti->tempo < 1000) w->force_tempo = ti->tempo;
    {
        const bool flag = ti && (ti->flags & kVstTransportPlaying) && (ti->flags & kVstPpqPosValid);
        const double ppq = flag ? ti->ppqPos : 0;
        w->force_rolling = flag && std::fabs(ppq - w->last_ppq) > 1e-9;
        w->last_ppq = flag ? ppq : -1e9;
    }

    for (int k = 0; k < w->nev; k++) midi(w, w->ev[k].d);
    w->nev = 0;

    decks::Clock &c = w->clk;
    bool was_on[2];
    for (int d = 0; d < 2; d++) was_on[d] = w->deck[d].playing;
    if (w->force_clock) {
        w->leader = -1;
        c.tempo = w->force_tempo;
        c.rolling = w->force_rolling;
        c.ppq = c.rolling ? ti->ppqPos : 0;
    } else {
        /* the leader stopped: the other deck, if it plays, takes the lead (and the tempo glides to its own) */
        if (w->leader >= 0 && !w->deck[w->leader].playing && !w->deck[w->leader].waiting) {
            const int o = 1 - w->leader;
            w->leader = w->deck[o].playing ? o : -1;
            if (w->leader >= 0) start_glide(w);
        }
        if (w->leader >= 0) {
            const decks::Deck &ld = w->deck[w->leader];
            if (ld.t && ld.t->bpm > 0) {
                const double target = ld.t->bpm * (1.0 + val(w, w->leader ? PITCH_B : PITCH_A) * 0.01);
                w->tempo = w->glide_pos < w->glide_len ? w->glide_from + (target - w->glide_from) * w->glide_pos / w->glide_len : target;
            }
        }
        c.tempo = w->tempo;
        c.rolling = w->deck[0].playing || w->deck[1].playing;
        c.ppq = w->vppq;
    }
    w->rolling.store(c.rolling);

    for (int d = 0; d < 2; d++) {
        decks::Deck &k = w->deck[d];
        if (w->to_retire[d] && w->lib.retire(w->to_retire[d])) w->to_retire[d] = nullptr;
        if (!k.playing && !k.waiting && !w->to_retire[d]) {   /* a new track only onto a stopped deck */
            if (decks::Track *nt = w->lib.incoming[d].exchange(nullptr)) {
                w->to_retire[d] = k.t;
                k.load(nt);
                if (w->to_retire[d] && w->lib.retire(w->to_retire[d])) w->to_retire[d] = nullptr;
            }
        }
        const int r = w->req[d].exchange(0);
        if (r & REQ_SYNC) {
            set_val(w, IDX[d ? SYNC_B : SYNC_A], sw(w, d ? SYNC_B : SYNC_A) ? 0 : 1);
            configure(w);
        }
        /* the leader always follows the own clock (it makes it); a follower only with SYNC on */
        if (!w->force_clock && d == w->leader) k.sync = true;
        if (r & REQ_CUE) k.cue_press();
        if (r & REQ_PLAY) {
            const bool alone = !w->force_clock && !w->deck[1 - d].playing && !k.playing && !k.waiting;
            k.toggle(c, w->quant);
            if (alone && k.playing && k.t) {   /* nothing else plays: this deck starts the own clock at its tempo */
                w->leader = d;
                k.sync = true;
                if (k.t->bpm > 0) {
                    w->tempo = c.tempo = k.t->bpm * (1.0 + k.pitch * 0.01);
                    w->vppq = c.ppq = k.beat_at(k.pos);
                }
                w->glide_len = w->glide_pos = 0;
                c.rolling = true;
                k.anchor(c, w->quant);
            }
        }
        k.render(L, R, n, c, w->quant, w->gain[d]);
    }
    if (!w->force_clock) {
        /* a deck that started while the other played is the new leader: its tempo rules, reached over GLIDE */
        for (int d = 0; d < 2; d++)
            if (!was_on[d] && w->deck[d].playing && w->leader != d) {
                w->leader = d;
                start_glide(w);
            }
        if (c.rolling) {
            const double adv = n * w->tempo / 60.0 / c.sr;
            w->vppq += adv;
            w->glide_pos += adv;
        }
    }
    w->ui_leader.store(w->leader);
    w->ui_tempo.store((float)c.tempo);
    for (int d = 0; d < 2; d++) {
        decks::Deck &k = w->deck[d];
        k.sync = sw(w, d ? SYNC_B : SYNC_A) || (!w->force_clock && d == w->leader);
        w->lib.busy[d].store(k.playing || k.waiting);
        Plugin::Ui &u = w->ui[d];
        u.state.store(!k.t ? 0 : k.waiting ? 3 : k.playing ? 2 : 1);
        if (k.t) {
            u.pos.store((float)(k.pos / k.t->sr));
            u.beat.store(k.t->bpm > 0 ? (float)k.beat_at(k.pos) : 0.0f);
            u.rate.store((float)(k.rate / (k.t->sr / c.sr)));
            u.eff.store(k.synced() ? (float)c.tempo : (float)(k.t->bpm * (1 + k.pitch * 0.01)));
            u.err.store((float)k.err);
            u.locked.store(k.locked);
        }
    }

    for (int ch = 0; ch < 2; ch++) {   /* output safety above -3 dBFS, ceiling 0.98 */
        float *y = out[ch];
        for (int i = 0; i < n; i++) {
            const float a = std::fabs(y[i]);
            if (a > 0.7f) {
                float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                y[i] = std::copysign(0.7f + 0.28f * t * (27 + t2) / (27 + 9 * t2), y[i]);
            }
        }
    }

    /* the TRACK knob and the list rows follow the selection */
    const int sel = w->lib.sel.load(), cnt = w->lib.count.load();
    if (w->lib.sel_changed.exchange(0) || sel != w->last_sel || cnt != w->last_count) {
        const bool recount = cnt != w->last_count;
        w->last_sel = sel; w->last_count = cnt;
        const float kn = cnt > 1 ? (float)sel / (float)(cnt - 1) : 0.0f;
        /* a new list length always: the host shows the knob's text only after a report */
        if (recount || std::fabs(w->cache[IDX[TRACK]].load() * (cnt > 1 ? cnt - 1 : 1) - sel) > 0.5f) set_norm(w, TRACK, kn);
    }
    const int fsel = w->lib.fsel.load(), fcnt = w->lib.fcount.load();
    if (fsel != w->last_fsel || fcnt != w->last_fcount) {   /* the FOLDER knob follows the folder selection */
        const bool recount = fcnt != w->last_fcount;
        w->last_fsel = fsel; w->last_fcount = fcnt;
        const float kn = fcnt > 1 ? (float)fsel / (float)(fcnt - 1) : 0.0f;
        if (recount || std::fabs(w->cache[IDX[FOLDER]].load() * (fcnt > 1 ? fcnt - 1 : 1) - fsel) > 0.5f) set_norm(w, FOLDER, kn);
    }
    bool any = false;
    const int top = top_row(w);
    for (int r = 0; r < decks::ROWS; r++) {
        const int want = top + r == sel && sel < cnt ? 1 : 0;
        if (w->row_rep[r].load() != want) {
            w->row_rep[r].store(want);
            w->master(&w->fx, audioMasterAutomate, IDX[ROW_1 + r], 0, 0, (float)want);
            any = true;
        }
    }
    for (int r = 0; r < decks::FROWS; r++) {
        const int want = w->lib.ftop.load() + r == fsel && fsel < fcnt ? 1 : 0;
        if (w->fold_rep[r].load() != want) {
            w->fold_rep[r].store(want);
            w->master(&w->fx, audioMasterAutomate, IDX[FOLD_1 + r], 0, 0, (float)want);
            any = true;
        }
    }
    for (int i = 0; i < NPARAMS; i++) {
        /* a trigger reports itself released to the host (the Force sends no setParameter(0) after a tap) */
        if (w->release[i]) { w->release[i] = 0; w->down[i] = false; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static intptr_t process_events(Plugin *w, const VstEvents *evs) {
    if (!evs) return 0;
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent *ev = evs->events[i];
        if (!ev || ev->type != kVstMidiType || w->nev >= NEV) continue;
        const VstMidiEvent *m = (const VstMidiEvent *)ev;
        MidiEv &q = w->ev[w->nev++];
        q.frame = m->deltaFrames;
        q.d[0] = m->midiData[0]; q.d[1] = m->midiData[1]; q.d[2] = m->midiData[2];
    }
    std::stable_sort(w->ev, w->ev + w->nev, [](const MidiEv &a, const MidiEv &b) { return a.frame < b.frame; });
    return 1;
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key < 0 || is_text(key)) return;
    if (key == TRACK) {   /* the knob browses the whole list */
        const int cnt = w->lib.count.load();
        select_track(w, (int)std::lround(clamp01(n) * (cnt > 1 ? cnt - 1 : 0)));
        w->cache[i].store(clamp01(n));
        return;
    }
    if (is_row(key)) {   /* a tap toggles the row's value: anything but what we reported is a tap */
        const int r = key - ROW_1;
        const int want = n > 0.5f ? 1 : 0;
        if (want == w->row_rep[r].load()) return;
        const int s = top_row(w) + r;
        if (s < w->lib.count.load()) select_track(w, s);
        w->row_rep[r].store(-1);   /* tell the host the real state again */
        return;
    }
    if (is_fold(key)) {   /* a tap opens the folder (as for the track rows: a value change is a tap) */
        const int r = key - FOLD_1;
        if ((n > 0.5f ? 1 : 0) == w->fold_rep[r].load()) return;
        const int f = w->lib.ftop.load() + r;
        if (f < w->lib.fcount.load()) w->lib.enter_req.store(f + 1);
        w->fold_rep[r].store(-1);
        return;
    }
    if (key == FOLDER) {   /* the knob selects a folder of the whole list, the rows page along; OPEN opens it */
        const int fc = w->lib.fcount.load();
        const int f = fc > 1 ? clampi((int)std::lround(clamp01(n) * (fc - 1)), 0, fc - 1) : 0;
        w->lib.fsel.store(f);
        w->lib.ftop.store((f / decks::FROWS) * decks::FROWS);
        w->cache[i].store(clamp01(n));
        return;
    }
    if (key == DRIVE) {
        const int nd = std::max(1, w->lib.ndrives.load());
        w->lib.fdrive.store(clampi((int)std::lround(clamp01(n) * 7), 0, nd - 1));
        w->cache[i].store(clamp01(n));
        return;
    }
    if (is_trigger(key)) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (rising) {
            const int nd = std::max(1, w->lib.ndrives.load());
            switch (key) {
            case PLAY_A: w->req[0].fetch_or(REQ_PLAY); break;
            case CUE_A: w->req[0].fetch_or(REQ_CUE); break;
            case PLAY_B: w->req[1].fetch_or(REQ_PLAY); break;
            case CUE_B: w->req[1].fetch_or(REQ_CUE); break;
            case LOAD_A: w->lib.load_req[0].store(w->lib.sel.load() + 1); break;
            case LOAD_B: w->lib.load_req[1].store(w->lib.sel.load() + 1); break;
            case RESCAN: w->lib.rescan_req.store(true); break;
            case DRIVE_PREV: case DRIVE_NEXT: {
                const int d = clampi(w->lib.fdrive.load() + (key == DRIVE_NEXT ? 1 : -1), 0, nd - 1);
                w->lib.fdrive.store(d);
                set_norm(w, DRIVE, d / 7.0f);
                break;
            }
            case UP: w->lib.up_req.store(true); break;
            case OPEN: if (w->lib.fcount.load() > 0) w->lib.enter_req.store(w->lib.fsel.load() + 1); break;
            case USE: w->lib.use_req.store(true); break;
            case REMOVE: w->lib.remove_req.store(true); break;
            default: break;
            }
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key >= 0 && (is_trigger(key) || is_text(key))) return 0.0f;
    if (key >= 0 && is_fold(key)) return w->fold_rep[key - FOLD_1].load() > 0 ? 1.0f : 0.0f;
    if (key >= 0 && is_row(key)) return w->row_rep[key - ROW_1].load() > 0 ? 1.0f : 0.0f;
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* ---- project chunk: "DECKS1" then one "key=value" per line; path_a/path_b = the tracks on
 * the decks (loaded again, stopped at their first kick) --------------------------------- */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "DECKS1\n";
    char buf[96];
    for (int k = 0; k < NKEYS; k++) {
        if (!is_setting(k) || IDX[k] < 0) continue;
        std::snprintf(buf, sizeof buf, "%s=%.3f\n", KEYS[k], (double)val(w, k));
        t += buf;
    }
    {
        std::lock_guard<std::mutex> lk(w->lib.mtx);
        for (int d = 0; d < 2; d++) if (!w->lib.deck_path[d].empty()) t += std::string(d ? "path_b=" : "path_a=") + w->lib.deck_path[d] + "\n";
    }
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 7) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));
    if (t.compare(0, 7, "DECKS1\n")) return 0;
    size_t pos = 7;
    while (pos < t.size()) {
        size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        const std::string line = t.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), v = line.substr(eq + 1);
        if (key == "path_a" || key == "path_b") {
            std::lock_guard<std::mutex> lk(w->lib.mtx);
            w->lib.path_req[key == "path_b"] = v;
            continue;
        }
        for (int k = 0; k < NKEYS; k++) if (is_setting(k) && key == KEYS[k]) set_val(w, IDX[k], std::atof(v.c_str()));
    }
    w->dirty.store(true);
    return 1;
}

/* ---- the skin's text ------------------------------------------------------------ */
static std::string deck_title(Plugin *w, int d) {
    std::string s = d ? "B   " : "A   ";
    if (w->lib.loading[d].load()) return s + "LOADING...";
    std::lock_guard<std::mutex> lk(w->lib.mtx);
    if (w->lib.deck_path[d].empty()) return s + "(EMPTY)";
    return s + decks::ascii(decks::strip_ext(decks::base_name(w->lib.deck_path[d])), 80);
}
static void deck_meta(Plugin *w, int d, double *bpm, double *secs) {
    std::lock_guard<std::mutex> lk(w->lib.mtx);
    *bpm = w->lib.deck_bpm[d];
    *secs = w->lib.deck_secs[d];
}
static std::string deck_info(Plugin *w, int d) {
    const Plugin::Ui &u = w->ui[d];
    const int st = u.state.load();
    if (st == 0) return "";
    double bpm, secs;
    deck_meta(w, d, &bpm, &secs);
    const bool sync = sw(w, d ? SYNC_B : SYNC_A);
    const bool own = !sw(w, CLOCK);
    const bool lead = own && w->ui_leader.load() == d;
    std::string s = st == 2 ? (lead ? "LEAD" : own ? (sync && bpm > 0 ? "SYNC" : "PLAY") : (sync && bpm > 0 && !w->rolling.load() ? "FREE" : "PLAY"))
                  : st == 3 ? (w->quant == decks::Q_BAR ? "WAIT BAR" : "WAIT BEAT") : "STOP";
    char b[128];
    if (bpm > 0) {
        const double eff = u.eff.load();
        std::snprintf(b, sizeof b, "  %.2f > %.2f BPM  %+.1f%%", bpm, eff, (eff / bpm - 1) * 100);
    } else std::snprintf(b, sizeof b, "  NO TEMPO  %+.1f%%", (u.rate.load() - 1) * 100);
    s += b;
    const double p = u.pos.load();
    s += "  " + decks::fmt_time(p) + " / -" + decks::fmt_time(secs - p);
    return s;
}
static std::string deck_bar(Plugin *w, int d) {
    const Plugin::Ui &u = w->ui[d];
    if (u.state.load() == 0) return "";
    double bpm, secs;
    deck_meta(w, d, &bpm, &secs);
    const int W = 36;
    const int done = secs > 0 ? clampi((int)std::lround(u.pos.load() / secs * W), 0, W) : 0;
    std::string s = "[" + std::string((size_t)done, '|') + std::string((size_t)(W - done), '.') + "]";
    if (bpm > 0) {
        const double b = u.beat.load();
        char t[96];
        if (b < 0) std::snprintf(t, sizeof t, "   INTRO");
        else {
            const long long k = (long long)std::floor(b);
            std::snprintf(t, sizeof t, "   BAR %lld.%lld", k / 4 + 1, k % 4 + 1);
        }
        s += t;
        if (u.state.load() == 2 && u.locked.load()) {
            std::snprintf(t, sizeof t, "   LOCK %+.0f ms", -u.err.load() * 60000.0 / std::max(1.0f, u.eff.load()));
            s += t;
        }
    }
    return s;
}
static std::string row_text(Plugin *w, int r) {
    std::lock_guard<std::mutex> lk(w->lib.mtx);
    const int i = (w->lib.sel.load() / decks::ROWS) * decks::ROWS + r;
    if (i < 0 || i >= (int)w->lib.entries.size()) return "";
    const decks::Entry &e = w->lib.entries[(size_t)i];
    char b[24];
    if (!e.online) std::snprintf(b, sizeof b, "OFFLINE  ");
    else if (e.bpm > 0) std::snprintf(b, sizeof b, "%6.2f   ", e.bpm);
    else if (e.bpm == 0) std::snprintf(b, sizeof b, "  ---    ");
    else std::snprintf(b, sizeof b, "         ");
    std::string s = b + decks::ascii(e.name, 70);
    if (e.path == w->lib.deck_path[0]) s += "   [A]";
    if (e.path == w->lib.deck_path[1]) s += "   [B]";
    return s;
}
static std::string drive_label_locked(Plugin *w, const std::string &mount) {
    for (const decks::Drive &d : w->lib.drives) if (d.mount == mount) return decks::ascii(d.label, 30);
    return decks::ascii(decks::base_name(mount), 30);
}
static std::string text_of(Plugin *w, int key) {
    char b[160];
    switch (key) {
    case TITLE_A: case TITLE_B: return deck_title(w, key == TITLE_B);
    case INFO_A: case INFO_B: return deck_info(w, key == INFO_B);
    case BAR_A: case BAR_B: return deck_bar(w, key == BAR_B);
    case STATUS: { std::lock_guard<std::mutex> lk(w->lib.mtx); return decks::ascii(w->lib.status, 100); }
    case HELP_1: return sw(w, CLOCK) ? "FORCE: both decks follow the Force transport" : "DECKS: the deck started last leads at its own tempo, the other follows";
    case HELP_2: return "Pads  36 PLAY A   37 CUE A   38 SYNC A   39 LOAD A";
    case HELP_3: return "Pads  40 PLAY B   41 CUE B   42 SYNC B   43 LOAD B";
    case HELP_4: return "Pads  44 TRACK UP   45 TRACK DOWN   46 PAGE UP   47 PAGE DOWN";
    case HELP_5: return "Decks 0.3.0";
    case LIB_INFO: {
        const int cnt = w->lib.count.load(), sel = w->lib.sel.load();
        if (!cnt) return "NO TRACKS";
        std::snprintf(b, sizeof b, "TRACK %d OF %d      PAGE %d / %d", sel + 1, cnt, sel / decks::ROWS + 1, (cnt + decks::ROWS - 1) / decks::ROWS);
        return b;
    }
    case PATH: {
        std::lock_guard<std::mutex> lk(w->lib.mtx);
        const int d = w->lib.fpath_drive;
        if (d < 0 || d >= (int)w->lib.drives.size()) return "";
        const decks::Drive &dv = w->lib.drives[(size_t)d];
        std::string rel = w->lib.fpath.size() > dv.mount.size() ? w->lib.fpath.substr(dv.mount.size()) : "/";
        std::snprintf(b, sizeof b, "   (%d FOLDERS)", (int)w->lib.folders.size());
        return decks::ascii(dv.label, 30) + ":" + decks::ascii(rel, 80) + b;
    }
    case SOURCES: {
        std::lock_guard<std::mutex> lk(w->lib.mtx);
        if (w->lib.sources.empty()) return "SOURCES: NONE - OPEN A FOLDER, THEN USE FOLDER";
        std::string s = "SOURCES: ";
        for (size_t i = 0; i < w->lib.sources.size(); i++) {
            const auto &src = w->lib.sources[i];
            bool on = false;
            for (const decks::Drive &d : w->lib.drives) on |= d.mount == src.first;
            if (i) s += "   |   ";
            const std::string rel = src.second.size() > src.first.size() ? src.second.substr(src.first.size()) : "/";
            s += drive_label_locked(w, src.first) + ":" + decks::ascii(rel, 50) + (on ? "" : " (OFFLINE)");
        }
        return s;
    }
    default: break;
    }
    if (is_fold(key)) {
        std::lock_guard<std::mutex> lk(w->lib.mtx);
        const int i = w->lib.ftop.load() + key - FOLD_1;
        if (i < 0 || i >= (int)w->lib.folders.size()) return "";
        return "> " + decks::ascii(w->lib.folders[(size_t)i], 80);
    }
    return "";
}

static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key < 0) { buf[0] = 0; return; }
    if (is_trigger(key)) { buf[0] = 0; return; }
    if (is_text(key) || is_fold(key)) { std::snprintf(buf, n, "%s", text_of(w, key).c_str()); return; }
    if (is_row(key)) { std::snprintf(buf, n, "%s", row_text(w, key - ROW_1).c_str()); return; }
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const float v = val_at(w, idx);
    switch (key) {
    case TRACK: {
        const int cnt = w->lib.count.load();
        if (cnt) std::snprintf(buf, n, "%d / %d", w->lib.sel.load() + 1, cnt); else std::snprintf(buf, n, "NO TRACKS");
        return;
    }
    case FOLDER: {
        const int fc = w->lib.fcount.load();
        if (fc) std::snprintf(buf, n, "%d / %d", w->lib.fsel.load() + 1, fc); else std::snprintf(buf, n, "NO FOLDERS");
        return;
    }
    case DRIVE: {
        std::lock_guard<std::mutex> lk(w->lib.mtx);
        const int d = w->lib.fdrive.load(), nd = (int)w->lib.drives.size();
        if (!nd) { std::snprintf(buf, n, "NO DRIVE FOUND"); return; }
        const decks::Drive &dv = w->lib.drives[(size_t)clampi(d, 0, nd - 1)];
        std::snprintf(buf, n, "DRIVE %d/%d:  %s   (%s)", clampi(d, 0, nd - 1) + 1, nd, decks::ascii(dv.label, 30).c_str(), decks::ascii(dv.mount, 60).c_str());
        return;
    }
    case PITCH_A: case PITCH_B: std::snprintf(buf, n, "%+.1f %%", v); return;
    case NUDGE_A: case NUDGE_B: std::snprintf(buf, n, "%+.2f BEAT", v * 0.01f); return;
    case XFADE: {
        const int u = (int)std::lround(v);
        if (u == 50) std::snprintf(buf, n, "CENTER"); else if (u < 50) std::snprintf(buf, n, "A %d", 50 - u); else std::snprintf(buf, n, "B %d", u - 50);
        return;
    }
    default: break;
    }
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[norm_to_ui(pp, w->cache[idx].load())]); return; }
    std::snprintf(buf, n, "%d %%", (int)std::lround(v));
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        w->lib.stop();
        for (int d = 0; d < 2; d++) { delete w->deck[d].t; delete w->to_retire[d]; }
        delete w;
        return 1;
    case effSetProgram: return 0;
    case effGetProgram: return 0;
    case effGetProgramName: if (p) copy_str(p, "Decks", 24); return 0;
    case effGetProgramNameIndexed: if (idx != 0 || !p) return 0; copy_str(p, "Decks", 24); return 1;
    case effSetProgramName: return 0;
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    /* every control: the Q-Links only move parameters the plugin reports as automatable */
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && KEY_OF[idx] >= 0 && !is_text(KEY_OF[idx]);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[160];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 128);   /* JUCE's host reads up to 256 chars here */
        return 1;
    }
    case effSetSampleRate: if (o > 0) w->clk.sr = o; return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: return 1;
    case effProcessEvents: return process_events(w, (const VstEvents *)p);
    case effCanDo:
        if (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent") || !std::strcmp((const char *)p, "receiveVstTimeInfo"))) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    for (auto &r : w->row_rep) r.store(-1);
    for (auto &r : w->fold_rep) r.store(-1);
    start_values(w);
    set_norm(w, DRIVE, 0);
    w->lib.start(decks::data_dir("/decks.so"));
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = 1;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
