/* Pitch shifter: one variable-speed read pointer into a delay line, re-spliced with a short
 * raised-cosine crossfade only when it drifts out of range. Each splice's jump is picked by
 * normalized cross-correlation so the outgoing and incoming segments line up in phase (the
 * Eventide H949 / AMS "de-glitch" idea, WSOLA-style).
 *
 * Unlike an H910-style shifter (two taps half a window apart, triangle-crossfaded continuously),
 * which is a comb filter and sounds metallic even at small shifts, only one reader is heard between
 * splices, and during a splice the two readers sit a whole number of waveform periods apart, so
 * they add in phase.
 *
 * Mono, in place. Header-only so vst/pitch_test.cpp can exercise it natively. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct PitchShift {
    static constexpr int BUF = 16384, MASK = BUF - 1;
    static constexpr double GUARD = 3.0;   /* min reader delay: the 4-point read touches i+2 */

    std::vector<float> buf = std::vector<float>(BUF, 0.f);
    std::vector<float> fade, dec, score;
    double sr = 0;
    int xf = 0, jbase = 0, search = 0, corrLen = 0;
    uint32_t w = 0;                  /* write counter; buffer index is w & MASK */
    double delay = 0, oldDelay = 0;  /* reader delays behind w; oldDelay is the reader fading out */
    int xfPos = -1;                  /* position in the splice crossfade, -1 = no splice running */

    void configure(double rate) {
        sr = rate;
        xf = std::max(32, (int)std::lround(0.006 * sr));
        jbase = (int)std::lround(0.004 * sr);
        search = (int)std::lround(0.016 * sr);           /* one period down to ~62Hz */
        corrLen = 2 * (int)std::lround(0.004 * sr);      /* even: correlated at half rate */
        fade.resize((size_t)xf);
        for (int k = 0; k < xf; k++) fade[(size_t)k] = 0.5f - 0.5f * std::cos(3.14159265f * (k + 0.5f) / xf);
        int maxJ = std::max(jbase, 3 * xf + 2) + search + 2;   /* ratio is at most 4 (+24 st) */
        dec.assign((size_t)((corrLen + maxJ) / 2 + 8), 0.f);
        score.assign((size_t)(search / 2 + 4), 0.f);
        reset();
    }

    void reset() {
        std::fill(buf.begin(), buf.end(), 0.f);
        w = 0;
        delay = jbase;
        xfPos = -1;
    }

    /* 4-point Hermite read at `d` samples behind the write head. */
    float read(double d) const {
        d = std::min(std::max(d, 2.0), (double)(BUF - 8));
        double pos = (double)w - d, fl = std::floor(pos);
        uint32_t i = (uint32_t)(int64_t)fl;
        float t = (float)(pos - fl);
        float xm1 = buf[(i - 1) & MASK], x0 = buf[i & MASK], x1 = buf[(i + 1) & MASK], x2 = buf[(i + 2) & MASK];
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float corrAt(uint32_t a0, uint32_t b0) const {   /* full-rate normalized correlation */
        float ab = 0, aa = 0, bb = 0;
        for (int k = 1; k <= corrLen; k++) {
            float a = buf[(a0 - (uint32_t)k) & MASK], b = buf[(b0 - (uint32_t)k) & MASK];
            ab += a * b; aa += a * a; bb += b * b;
        }
        return ab / std::sqrt(aa * bb + 1e-12f);
    }

    /* Jump length in [jlo, jhi] whose segment best matches the one just behind the current reader.
     * dir = +1 moves the reader back in time (up-shift), -1 forward (down-shift). Coarse search on a
     * 2x-decimated copy (contiguous, so it vectorizes), then refined at full rate. Among near-equal
     * matches (a periodic signal matches at every period) it takes the one with the least latency. */
    int pickJump(int dir, int jlo, int jhi) {
        jlo += jlo & 1;
        jhi -= jhi & 1;
        if (jhi < jlo) return jlo;
        const int L2 = corrLen / 2;
        uint32_t a0 = (uint32_t)(int64_t)std::floor((double)w - delay) + 1u;   /* one past the read point */
        uint32_t lo = dir > 0 ? a0 - (uint32_t)(jhi + corrLen) : a0 - (uint32_t)corrLen;
        int span = dir > 0 ? jhi + corrLen : corrLen + jhi;
        int m = span / 2 + 1;
        for (int k = 0; k < m; k++) dec[(size_t)k] = buf[(lo + 2u * (uint32_t)k) & MASK];

        const float *ref = dec.data() + (a0 - lo) / 2 - L2;
        float aa = 0;
        for (int q = 0; q < L2; q++) aa += ref[q] * ref[q];

        int nc = (jhi - jlo) / 2 + 1;
        float best = -2.f;
        for (int c = 0; c < nc; c++) {
            int J = jlo + 2 * c;
            const float *cand = dec.data() + ((int64_t)(a0 - lo) - dir * J) / 2 - L2;
            float ab = 0, bb = 0;
            for (int q = 0; q < L2; q++) { ab += ref[q] * cand[q]; bb += cand[q] * cand[q]; }
            float s = ab / std::sqrt(aa * bb + 1e-12f);
            score[(size_t)c] = s;
            best = std::max(best, s);
        }
        /* First candidate (in least-latency order) that's near the best, then climb to its local
         * peak: stopping at the threshold edge would land consistently off-period, skipping a
         * little phase on every splice and detuning the output (~20 cents at +7 st). */
        float thr = best > 0 ? 0.95f * best : best;
        int pick = 0;
        if (dir > 0) {
            for (int c = 0; c < nc; c++) if (score[(size_t)c] >= thr) { pick = c; break; }
            while (pick + 1 < nc && score[(size_t)pick + 1] >= score[(size_t)pick]) pick++;
        } else {
            for (int c = nc - 1; c >= 0; c--) if (score[(size_t)c] >= thr) { pick = c; break; }
            while (pick > 0 && score[(size_t)pick - 1] >= score[(size_t)pick]) pick--;
        }

        int J = jlo + 2 * pick, bestJ = J;
        float bestS = -2.f;
        for (int j = J - 1; j <= J + 1; j++) {
            if (j < jlo - 1 || j > jhi + 1 || j < 1) continue;
            float s = corrAt(a0, a0 - (uint32_t)(dir * j));
            if (s > bestS) { bestS = s; bestJ = j; }
        }
        return bestJ;
    }

    void process(float *b, int32_t n, double rate, float semitones, float mix) {
        if (rate != sr) configure(rate);
        const double ratio = std::pow(2.0, semitones / 12.0);
        /* Reader position is w - delay and w advances 1/sample, so reading at `ratio` samples per
         * sample means delay changes by 1 - ratio per sample: shrinks going up, grows going down. */
        const double drift = 1.0 - ratio;
        const double over = std::fabs(ratio - 1.0);
        /* A jump must outlast its own crossfade, or the new reader re-splices mid-fade. */
        const int jmin = std::max(jbase, (int)std::ceil(xf * over) + 2);
        /* Up: splice early enough that the fading-out reader never catches the write head. */
        const double lowT = GUARD + 1.0 + (xf + 2) * (ratio - 1.0);
        const double highT = GUARD + jmin + search;
        for (int32_t i = 0; i < n; i++) {
            buf[w & MASK] = b[i];
            if (xfPos < 0) {
                if (ratio > 1.0 && delay <= lowT) {
                    int J = pickJump(+1, jmin, jmin + search);
                    oldDelay = delay; delay += J; xfPos = 0;
                } else if (ratio < 1.0 && delay >= highT) {
                    int J = pickJump(-1, jmin, std::min(jmin + search, (int)(delay - GUARD) - 1));
                    oldDelay = delay; delay -= J; xfPos = 0;
                }
            }
            float y = read(delay);
            if (xfPos >= 0) {
                float g = fade[(size_t)xfPos];
                y = g * y + (1.f - g) * read(oldDelay);
                oldDelay += drift;
                if (++xfPos >= xf) xfPos = -1;
            }
            delay += drift;
            b[i] = b[i] * (1.f - mix) + y * mix;
            w++;
        }
    }
};
