/* Offline comparison of vst/pitch_shift.h against a two-tap H910-style reference shifter.
 *   g++ -O2 -std=c++17 -Ivst vst/pitch_test.cpp -o vst/build/pitch_test && vst/build/pitch_test
 * Per shift and signal: output pitch (zero-crossing estimate, sine only), amplitude wobble
 * (max/min of 20ms RMS frames on a steady tone -- comb filtering and splice cancellation show up
 * here; ideal is 0 dB), and the worst sample-to-sample jump relative to a clean sine's (clicks). */
#include "pitch_shift.h"
#include <cstdio>
#include <functional>

struct TwoTapPitch {   /* reference: two triangle-windowed taps half a window apart */
    static const int BUF = 4096, WINDOW = 2048;
    std::vector<float> buf = std::vector<float>(BUF, 0.f);
    int writePos = 0;
    float phase0 = 0.f, phase1 = WINDOW * 0.5f;
    static float wrap(float p) { while (p >= WINDOW) p -= WINDOW; while (p < 0) p += WINDOW; return p; }
    float readInterp(float r) const {
        int i0 = ((int)std::floor(r)) & (BUF - 1), i1 = (i0 + 1) & (BUF - 1);
        float f = r - std::floor(r);
        return buf[i0] * (1 - f) + buf[i1] * f;
    }
    void process(float *b, int n, double, float semis, float mix) {
        float delta = 1.f - std::pow(2.f, semis / 12.f);
        for (int i = 0; i < n; i++) {
            buf[writePos & (BUF - 1)] = b[i];
            phase0 = wrap(phase0 + delta); phase1 = wrap(phase1 + delta);
            float e0 = 1 - std::fabs(2 * phase0 / WINDOW - 1), e1 = 1 - std::fabs(2 * phase1 / WINDOW - 1);
            float r0 = writePos - phase0; if (r0 < 0) r0 += BUF;
            float r1 = writePos - phase1; if (r1 < 0) r1 += BUF;
            float wet = e0 * readInterp(r0) + e1 * readInterp(r1);
            b[i] = b[i] * (1 - mix) + wet * mix;
            writePos = (writePos + 1) & (BUF - 1);
        }
    }
};

static const double SR = 44100.0;
static const int SECS = 3, BLOCK = 128;

template <class P>
static std::vector<float> run(P &p, const std::function<float(long)> &sig, float semis, double *meanDelay = nullptr) {
    long total = (long)(SECS * SR) / BLOCK * BLOCK;
    std::vector<float> out((size_t)total);
    double dsum = 0;
    for (long off = 0; off < total; off += BLOCK) {
        float blk[BLOCK];
        for (int i = 0; i < BLOCK; i++) blk[i] = sig(off + i);
        p.process(blk, BLOCK, SR, semis, 1.f);
        for (int i = 0; i < BLOCK; i++) out[(size_t)(off + i)] = blk[i];
        if constexpr (std::is_same_v<P, PitchShift>) dsum += p.delay;
    }
    if (meanDelay) *meanDelay = dsum / ((double)total / BLOCK) / SR * 1000.0;
    return out;
}

struct Stats { double freq, wobbleDb, click; };

static Stats measure(const std::vector<float> &y, double fOut) {
    size_t start = (size_t)(0.5 * SR);   /* skip the fill/settle */
    int zc = 0;
    for (size_t i = start + 1; i < y.size(); i++) if (y[i - 1] < 0 && y[i] >= 0) zc++;
    double freq = zc / ((y.size() - start) / SR);
    size_t frame = (size_t)(0.02 * SR);
    double lo = 1e30, hi = 0, rmsAll = 0;
    for (size_t f = start; f + frame <= y.size(); f += frame) {
        double s = 0;
        for (size_t i = f; i < f + frame; i++) s += (double)y[i] * y[i];
        double r = std::sqrt(s / frame);
        lo = std::min(lo, r); hi = std::max(hi, r); rmsAll += s;
    }
    rmsAll = std::sqrt(rmsAll / (y.size() - start));
    double maxStep = 0;
    for (size_t i = start + 1; i < y.size(); i++) maxStep = std::max(maxStep, (double)std::fabs(y[i] - y[i - 1]));
    double cleanStep = 2 * M_PI * fOut / SR * rmsAll * std::sqrt(2.0);
    return {freq, 20 * std::log10(hi / std::max(lo, 1e-9)), maxStep / cleanStep};
}

int main() {
    const float shifts[] = {-24, -12, -7, -5, -1, -0.1f, 0.1f, 1, 5, 7, 12, 24};
    auto sine = [](long n) { return 0.5f * (float)std::sin(2 * M_PI * 220.0 * n / SR); };
    auto saw = [](long n) {   /* band-limited 110Hz saw, guitar-ish harmonic content */
        double s = 0;
        for (int h = 1; h <= 30; h++) s += std::sin(2 * M_PI * 110.0 * h * n / SR) / h;
        return (float)(0.3 * s);
    };
    printf("%7s | %-26s | %-26s | %s\n", "shift", "sine 220Hz: freq / wobble", "  (splice)", "saw 110Hz wobble 2-tap -> splice, splice latency");
    for (float st : shifts) {
        double ratio = std::pow(2.0, st / 12.0), fOut = 220.0 * ratio;
        TwoTapPitch o1, o2; PitchShift n1, n2;
        Stats so = measure(run(o1, sine, st), fOut);
        double lat = 0;
        Stats sn = measure(run(n1, sine, st, &lat), fOut);
        Stats wo = measure(run(o2, saw, st), 110.0 * ratio);
        Stats wn = measure(run(n2, saw, st), 110.0 * ratio);
        printf("%+6.1f  | 2tap %7.1fHz %5.1fdB clk%4.1f | splice %7.1fHz %5.1fdB clk%4.1f | saw %5.1fdB -> %4.1fdB  %4.1fms  (want %.1fHz)\n",
               st, so.freq, so.wobbleDb, so.click, sn.freq, sn.wobbleDb, sn.click, wo.wobbleDb, wn.wobbleDb, lat, fOut);
    }
}
