/* =============================================================================
 * decks_engine.h - one deck: plays a track held in RAM (16-bit stereo at its own sample rate),
 * vinyl style (tempo and pitch move together, cubic interpolation), locked to the Force clock.
 *
 * Sync. A deck with a tempo and SYNC on plays at   rate = Force BPM / track BPM   (times the
 * track's rate over the host's). Its phase is locked too: when it starts, the deck remembers
 * how its beat count relates to the song position (off = track beat - ppqPos), and keeps that
 * relation modulo one bar (4 beats). Modulo a bar, so a Force loop of whole bars or a jump in
 * the song does not throw the deck around: it stays where it is in its track, on the same beat
 * of the bar. Small errors are pulled in by a rate correction of at most 3 % (inaudible as a
 * jump, like a nudge on a turntable); errors over 1/4 beat (transport restart, SYNC switched on)
 * are a jump with a 6 ms crossfade.
 *   QUANT OFF   PLAY starts at once, the offset is whatever it is (tempo sync only)
 *   QUANT BEAT  PLAY starts on the next beat of the Force, cue on a beat -> beats line up
 *   QUANT BAR   PLAY starts on the next bar -> with the cue on the track's first kick, bars line up
 * NUDGE shifts the locked phase by up to +-1/2 beat (live). With the Force transport stopped a
 * deck runs free at the last tempo seen; when the transport starts, the lock re-anchors to the
 * nearest beat (QUANT BEAT/BAR) or keeps the phase as it is (QUANT OFF).
 * Everything here runs on the audio thread; no allocation, no locks.
 * MIT license (see ../LICENSE).
 * ========================================================================== */
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace decks {

/* a decoded track; made and freed by the worker thread, read by the audio thread */
struct Track {
    int16_t *pcm = nullptr;   /* interleaved stereo */
    int64_t frames = 0;
    double sr = 44100;
    double bpm = 0, beat0 = 0;
    std::string path, title;
    void (*release)(void *) = nullptr;
    ~Track() { if (pcm && release) release(pcm); }
    double spb() const { return bpm > 0 ? sr * 60.0 / bpm : 0; }   /* frames per beat */
};

enum { Q_OFF, Q_BEAT, Q_BAR };
enum { BAR = 4, XF = 256 };

/* the host's clock as one block sees it */
struct Clock {
    double tempo = 120;     /* last valid tempo */
    bool rolling = false;   /* transport running, ppq valid */
    double ppq = 0;         /* song position at the block start (beats) */
    double sr = 44100;
};

inline float hermite(const int16_t *p, int64_t frames, double pos, int ch) {
    const int64_t i = (int64_t)std::floor(pos);
    const float f = (float)(pos - (double)i);
    auto s = [&](int64_t k) -> float { return (k < 0 || k >= frames) ? 0.0f : p[2 * k + ch] * (1.0f / 32768.0f); };
    const float xm = s(i - 1), x0 = s(i), x1 = s(i + 1), x2 = s(i + 2);
    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

struct Deck {
    Track *t = nullptr;
    double pos = 0, cue = 0;
    bool playing = false;
    bool waiting = false;       /* PLAY pressed, waiting for the next beat/bar of the Force */
    double start_ppq = 0;
    bool locked = false;        /* offset valid (deck started with the transport rolling) */
    double off = 0;             /* track beat - ppq, kept modulo a bar */
    bool sync = true;
    float pitch = 0;            /* % with SYNC off */
    float nudge = 0;            /* beats, -0.5 .. 0.5 */
    double xpos = 0;            /* crossfade from a jump: the old read head */
    int xleft = 0;
    double rate = 1;            /* last rate, for the display */
    double err = 0;             /* last phase error in beats, for the display */
    bool was_rolling = false;
    float g = 0;                /* smoothed gain */

    bool synced() const { return sync && t && t->bpm > 0; }
    double beat_at(double p) const { return (p - t->beat0) / t->spb(); }

    /* PLAY: starts (quantised) or pauses */
    void toggle(const Clock &c, int quant) {
        if (!t) return;
        if (playing || waiting) { playing = waiting = false; return; }
        if (synced() && c.rolling && quant != Q_OFF) {
            const double q = quant == Q_BAR ? BAR : 1;
            start_ppq = std::ceil(c.ppq / q - 1e-6) * q;
            waiting = true;
        } else {
            playing = true;
            anchor(c, Q_OFF);
        }
    }
    /* CUE: playing -> back to the cue point and stop; stopped -> the cue point is here (on the
     * nearest beat when the track has a grid) */
    void cue_press() {
        if (!t) return;
        if (playing || waiting) { playing = waiting = false; jump_to(cue); return; }
        double p = pos;
        if (t->bpm > 0) p = t->beat0 + std::round(beat_at(p)) * t->spb();
        cue = std::max(0.0, std::min(p, (double)t->frames - 1));
        jump_to(cue);
    }
    void jump_to(double p) {
        if (playing && xleft == 0) { xpos = pos; xleft = XF; }
        pos = p;
    }
    /* the phase relation from where the deck is now; quantised re-anchors snap to the beat */
    void anchor(const Clock &c, int quant) {
        locked = synced() && c.rolling;
        if (!locked) return;
        off = beat_at(pos) - c.ppq - nudge;
        if (quant != Q_OFF) off = std::round(off);
    }
    void load(Track *nt) {
        t = nt;
        playing = waiting = false;
        locked = false;
        xleft = 0;
        cue = nt ? nt->beat0 : 0;
        pos = cue;
    }

    /* renders n frames, adds them times gain (ramped from the last block's gain) */
    void render(float *L, float *R, int n, const Clock &c, int quant, float gain) {
        const float g0 = g;
        g = gain;
        if (!t) { err = 0; return; }
        const double ratio = t->sr / c.sr;
        const bool syn = synced();
        double base = ratio * (syn ? c.tempo / t->bpm : 1.0 + pitch * 0.01);
        int from = 0;
        if (waiting) {
            if (!c.rolling || !syn) { waiting = false; playing = true; anchor(c, Q_OFF); }
            else {
                const double fpb = c.sr * 60.0 / c.tempo;   /* host frames per beat */
                const double at = (start_ppq - c.ppq) * fpb;
                if (at < n) {
                    from = std::max(0, (int)std::ceil(at - 1e-9));
                    waiting = false;
                    playing = true;
                    locked = true;
                    off = beat_at(pos) - start_ppq;
                    if (quant != Q_OFF) off = std::round(off);
                } else if (at > 8 * c.sr * 60.0 / c.tempo * BAR) {   /* the song jumped back: re-aim */
                    const double q = quant == Q_BAR ? BAR : 1;
                    start_ppq = std::ceil(c.ppq / q - 1e-6) * q;
                }
            }
        }
        if (c.rolling && !was_rolling && playing) anchor(c, quant);   /* transport (re)started */
        if (!c.rolling || !syn) locked = false;
        was_rolling = c.rolling;
        if (!playing) { rate = 0; err = 0; return; }
        if (syn && c.rolling && !locked) anchor(c, quant);           /* SYNC switched on, or a new grid */

        double r = base;
        err = 0;
        if (syn && locked) {
            const double ppq = c.ppq + from * c.tempo / 60.0 / c.sr;
            double e = (off + nudge) - (beat_at(pos) - ppq);           /* beats the deck is behind */
            e -= BAR * std::floor(e / BAR + 0.5);                     /* modulo a bar, -2 .. 2 */
            err = e;
            if (std::fabs(e) > 0.25) jump_to(pos + e * t->spb());
            else r = base * (1.0 + std::max(-0.03, std::min(0.03, e * t->spb() / (0.2 * t->sr))));
        }
        rate = r;
        const int64_t fr = t->frames;
        for (int i = from; i < n; i++) {
            const float gi = g0 + (g - g0) * (float)(i + 1) / (float)n;
            float l = hermite(t->pcm, fr, pos, 0), rr = hermite(t->pcm, fr, pos, 1);
            if (xleft > 0) {
                const float w = (float)xleft / XF;
                l = l * (1 - w) + hermite(t->pcm, fr, xpos, 0) * w;
                rr = rr * (1 - w) + hermite(t->pcm, fr, xpos, 1) * w;
                xpos += r;
                xleft--;
            }
            L[i] += l * gi;
            R[i] += rr * gi;
            pos += r;
            if (pos >= (double)fr) { playing = false; pos = (double)fr; xleft = 0; break; }
        }
    }
};

}  // namespace decks
