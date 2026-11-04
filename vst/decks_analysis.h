/* =============================================================================
 * decks_analysis.h - tempo and beatgrid of a whole track (runs on the worker thread, never
 * on the audio thread). Constant tempo assumed: right for techno, goa and most club music.
 *
 *   1. onset curve: amplitude rises in a low band (kick, < 150 Hz) and a high band (hats, > 3 kHz),
 *      one value per hop of ~5.8 ms, each band relative to its mean, local mean removed
 *   2. coarse tempo: autocorrelation of the onset curve, scored as a comb over 4 beat multiples,
 *      85 .. 180 BPM (so a 70 BPM half-time reading becomes 140)
 *   3. fine tempo + phase: the onset curve folded over each candidate beat period (+-1.5 BPM in
 *      0.01 steps); the sharpest fold wins - over a whole track that is a precision of a few
 *      thousandths of a BPM. Of the four 16ths the beat is the one with the loudest low-band
 *      peak (the kick, not a note of a rolling bassline)
 *   4. sample-accurate grid: the kick onset (where the low band's attack starts, found back from
 *      its steepest rise) is located near up to 256 predicted beats, a straight line through them gives period and anchor; a tempo
 *      within 0.025 of a whole number is taken as that number
 *   5. anchor = the first kick of the track: the grid's beat 1, the cue point after loading
 * MIT license (see ../LICENSE).
 * ========================================================================== */
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace decks {

struct Grid {
    double bpm = 0;     /* 0 = no tempo found (ambient, speech): the deck plays, but never syncs */
    double beat0 = 0;   /* frame of the first kick: beat 1 of the grid, the cue after loading */
    float conf = 0;     /* sharpness of the winning fold: no beat ~1.8, a club track 4 .. 9 */
};

namespace detail {
inline float mono(const int16_t *p, int64_t i) { return (p[2 * i] + p[2 * i + 1]) * (0.5f / 32768.0f); }

/* fold the onset curve over period P (in hops): histogram of K phase bins; returns the best
 * bin's share (smoothed over 3 bins) relative to the mean bin, and that bin */
inline float fold(const std::vector<float> &nov, double P, int K, int *best_bin, double *best_sum = nullptr) {
    static thread_local std::vector<float> h;
    h.assign((size_t)K, 0.0f);
    const double inv = 1.0 / P;
    double total = 0;
    for (size_t t = 0; t < nov.size(); t++) {
        const float v = nov[t];
        if (v <= 0) continue;
        double ph = t * inv;
        ph -= std::floor(ph);
        h[(size_t)std::min(K - 1, (int)(ph * K))] += v;
        total += v;
    }
    if (total <= 0) { *best_bin = 0; return 0; }
    float best = -1;
    int bb = 0;
    for (int k = 0; k < K; k++) {
        const float s = h[(size_t)((k + K - 1) % K)] * 0.5f + h[(size_t)k] + h[(size_t)((k + 1) % K)] * 0.5f;
        if (s > best) { best = s; bb = k; }
    }
    *best_bin = bb;
    if (best_sum) *best_sum = best;
    return (float)(best / (2.0 * total / K));
}
}  // namespace detail

/* cancel: polled now and then; a closing plugin stops a long analysis early */
inline Grid analyse(const int16_t *pcm, int64_t frames, double sr, const std::atomic<bool> *cancel = nullptr) {
    using namespace detail;
    Grid g;
    if (!pcm || sr < 8000 || frames < (int64_t)(10 * sr)) return g;
    const int H = std::max(64, (int)std::lround(256.0 * sr / 44100.0));
    const double fr = sr / H;   /* hops per second */
    const int64_t N = frames / H;

    /* 1. onset curve */
    std::vector<float> el((size_t)N), eh((size_t)N), ef((size_t)N);
    {
        const float al = std::exp(-2.0f * (float)M_PI * 150.0f / (float)sr);
        const float ah = std::exp(-2.0f * (float)M_PI * 3000.0f / (float)sr);
        float l1 = 0, l2 = 0, hl = 0;
        for (int64_t t = 0; t < N; t++) {
            if (cancel && (t & 4095) == 0 && cancel->load()) return g;
            double sl = 0, sh = 0, sf = 0;
            for (int i = 0; i < H; i++) {
                const float x = mono(pcm, t * H + i);
                l1 += (1 - al) * (x - l1);
                l2 += (1 - al) * (l1 - l2);
                hl += (1 - ah) * (x - hl);
                const float hi = x - hl;
                sl += (double)l2 * l2;
                sh += (double)hi * hi;
                sf += (double)x * x;
            }
            el[(size_t)t] = std::sqrt((float)(sl / H));
            eh[(size_t)t] = std::sqrt((float)(sh / H));
            ef[(size_t)t] = std::sqrt((float)(sf / H));
        }
    }
    /* amplitude rises, relative to the low band's mean level; the high band counts half and is
     * not scaled up on its own (offbeat hats would outweigh the kick). Not log energy: a log
     * curve rewards a rise out of silence, and in a rolling psytrance bassline the bass notes on
     * the 16ths rise out of silence more than the kick does - the grid would lock onto the bass. */
    double ml = 0;
    for (int64_t t = 0; t < N; t++) ml += el[(size_t)t] + eh[(size_t)t];
    const float nl = ml > 0 ? (float)(N / ml) : 0.0f;
    std::vector<float> nov((size_t)N, 0.0f);
    for (int64_t t = 1; t < N; t++)
        nov[(size_t)t] = nl * (std::max(0.0f, el[(size_t)t] - el[(size_t)t - 1]) + 0.5f * std::max(0.0f, eh[(size_t)t] - eh[(size_t)t - 1]));
    {   /* remove the local mean (+-16 hops), keep the peaks */
        std::vector<float> c((size_t)N + 1, 0.0f);
        for (int64_t t = 0; t < N; t++) c[(size_t)t + 1] = c[(size_t)t] + nov[(size_t)t];
        std::vector<float> d((size_t)N);
        for (int64_t t = 0; t < N; t++) {
            const int64_t a = std::max<int64_t>(0, t - 16), b = std::min<int64_t>(N, t + 17);
            d[(size_t)t] = std::max(0.0f, nov[(size_t)t] - (c[(size_t)b] - c[(size_t)a]) / (float)(b - a));
        }
        nov.swap(d);
    }

    /* 2. coarse tempo */
    const int Lmax = (int)std::ceil(4 * fr * 60.0 / 85.0) + 2;
    if (N < 4 * Lmax) return g;
    std::vector<double> ac((size_t)Lmax + 1, 0.0);
    for (int L = 1; L <= Lmax; L++) {
        if (cancel && (L & 31) == 0 && cancel->load()) return g;
        double s = 0;
        const float *a = nov.data(), *b = nov.data() + L;
        const int64_t n = N - L;
        for (int64_t t = 0; t < n; t++) s += (double)a[t] * b[t];
        ac[(size_t)L] = s / (double)n;
    }
    auto aci = [&](double L) {
        const int i = (int)L;
        if (i < 1 || i >= Lmax) return 0.0;
        const double f = L - i;
        return ac[(size_t)i] * (1 - f) + ac[(size_t)i + 1] * f;
    };
    double b0 = 0, s0 = -1;
    for (double b = 85.0; b < 180.0; b += 0.1) {
        const double L = fr * 60.0 / b;
        const double s = aci(L) + 0.8 * aci(2 * L) + 0.6 * aci(3 * L) + 0.5 * aci(4 * L);
        if (s > s0) { s0 = s; b0 = b; }
    }
    if (b0 <= 0) return g;

    /* 3. fine tempo + phase */
    const int K = 48;
    double b1 = b0, sum1 = 0;
    float c1 = -1;
    int bin1 = 0;
    for (int pass = 0; pass < 2; pass++) {
        const double lo = pass ? b1 - 0.02 : b0 - 1.5, hi = pass ? b1 + 0.02 : b0 + 1.5, step = pass ? 0.001 : 0.01;
        for (double b = lo; b <= hi + 1e-9; b += step) {
            if (cancel && cancel->load()) return g;
            int bin;
            double sum;
            const float c = fold(nov, fr * 60.0 / b, K, &bin, &sum);
            if (c > c1) { c1 = c; b1 = b; bin1 = bin; sum1 = sum; }
        }
    }
    g.conf = c1;
    const double Ph = fr * 60.0 / b1;                    /* period in hops */
    /* no clear beat: a flat fold, or onsets too weak to be beats (a pad's slow swells and the
     * beating of its partials make a periodic but tiny onset curve) */
    if (c1 < 2.5f || sum1 / (N / Ph) < 0.15) return g;
    double Ps = Ph * H;                                  /* period in frames */
    double a = (bin1 + 0.5) / K * Ph * H;                /* phase in frames */
    {   /* which 16th is the beat: the fold finds the strongest rise, and in a rolling bassline
         * (bass on the 16ths after the kick) that can be a bass note. The beat is the 16th with
         * the loudest peak - the kick. */
        const int span = std::max(1, (int)std::lround(0.04 * fr));
        double best = -1;
        int bj = 0;
        for (int j = 0; j < 4; j++) {
            double sum = 0;
            int cnt = 0;
            for (double h = (a + j * Ps / 4) / H; h + span < N; h += Ph) {
                float m = 0;
                for (int q = 0; q < span; q++) m = std::max(m, ef[(size_t)h + (size_t)q]);
                sum += m;
                cnt++;
            }
            if (cnt && sum / cnt > best * 1.05) { best = sum / cnt; bj = j; }   /* 5 %: the fold's own phase wins a tie */
        }
        a += bj * Ps / 4;
    }

    /* 4. sample-accurate grid from the kick onsets */
    const int W = (int)(0.012 * sr), D = std::max(1, (int)(0.002 * sr)), WU = (int)(0.03 * sr);
    const float al = std::exp(-2.0f * (float)M_PI * 150.0f / (float)sr), ae = std::exp(-1.0f / (0.001f * (float)sr));
    auto onset = [&](double s, float *rise) -> double {   /* steepest rise of the low band near frame s */
        const int64_t c = (int64_t)std::llround(s), i0 = c - W, i1 = c + W;
        if (i0 - WU < 0 || i1 >= frames) { *rise = 0; return s; }
        float l1 = 0, l2 = 0, e = 0;
        static thread_local std::vector<float> env;
        env.assign((size_t)(i1 - i0 + 1), 0.0f);
        for (int64_t i = i0 - WU; i <= i1; i++) {
            const float x = mono(pcm, i);
            l1 += (1 - al) * (x - l1);
            l2 += (1 - al) * (l1 - l2);
            e += (1 - ae) * (std::fabs(l2) - e);
            if (i >= i0) env[(size_t)(i - i0)] = e;
        }
        float best = 0;
        int64_t bi = c;
        for (int64_t i = i0 + D; i <= i1; i++) {
            const float r = env[(size_t)(i - i0)] - env[(size_t)(i - i0 - D)];
            if (r > best) { best = r; bi = i; }
        }
        *rise = best;
        /* back from the steepest rise to where the attack starts: the envelope below 1/8 of
         * its value 4 ms on (the kick's first sample, not a point inside its attack) */
        const float top = env[(size_t)(std::min(i1, bi + (int64_t)(0.004 * sr)) - i0)];
        int64_t st = bi;
        while (st > i0 && env[(size_t)(st - i0)] > 0.125f * top) st--;
        return (double)st;
    };
    const int64_t nbeats = (int64_t)((frames - a) / Ps);
    const int64_t stride = std::max<int64_t>(1, nbeats / 256);
    std::vector<double> ks, os;
    std::vector<float> rs;
    for (int64_t k = 0; k < nbeats; k += stride) {
        if (cancel && cancel->load()) return g;
        float r;
        const double o = onset(a + k * Ps, &r);
        if (r > 0) { ks.push_back((double)k); os.push_back(o); rs.push_back(r); }
    }
    float rmed = 0;
    if (!rs.empty()) {
        std::vector<float> t = rs;
        std::nth_element(t.begin(), t.begin() + t.size() / 2, t.end());
        rmed = t[t.size() / 2];
    }
    std::vector<char> use(ks.size(), 0);
    int nuse = 0;
    for (size_t i = 0; i < ks.size(); i++) if (rs[i] > 0.3f * rmed) { use[i] = 1; nuse++; }
    for (int it = 0; it < 3 && nuse >= 16; it++) {   /* least squares, then drop points off by > 4 ms */
        double sk = 0, so = 0, skk = 0, sko = 0;
        for (size_t i = 0; i < ks.size(); i++) if (use[i]) { sk += ks[i]; so += os[i]; skk += ks[i] * ks[i]; sko += ks[i] * os[i]; }
        const double den = nuse * skk - sk * sk;
        if (den <= 0) break;
        const double b = (nuse * sko - sk * so) / den, c = (so - b * sk) / nuse;
        if (b < Ps * 0.98 || b > Ps * 1.02) break;
        Ps = b; a = c;
        if (it == 2) break;
        nuse = 0;
        for (size_t i = 0; i < ks.size(); i++) {
            use[i] = rs[i] > 0.3f * rmed && std::fabs(os[i] - (a + ks[i] * Ps)) < 0.004 * sr;
            nuse += use[i];
        }
    }
    double bpm = 60.0 * sr / Ps;
    if (std::fabs(bpm - std::round(bpm)) < 0.025) {   /* a whole-number tempo: re-anchor on it */
        bpm = std::round(bpm);
        Ps = 60.0 * sr / bpm;
        std::vector<double> res;
        for (size_t i = 0; i < ks.size(); i++) if (use[i]) res.push_back(os[i] - ks[i] * Ps);
        if (!res.empty()) {
            std::nth_element(res.begin(), res.begin() + res.size() / 2, res.end());
            a = res[res.size() / 2];
        }
    }
    g.bpm = bpm;

    /* 5. the first kick: earliest grid beat with a clear low-band rise */
    double first = a - std::floor(a / Ps) * Ps;
    for (double s = first; s < frames - Ps; s += Ps) {
        float r;
        onset(s, &r);
        if (r > 0.3f * rmed) { first = s; break; }
    }
    g.beat0 = first;
    return g;
}

}  // namespace decks
