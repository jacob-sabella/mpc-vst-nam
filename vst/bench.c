/* On-device CPU benchmark: dlopen nam_vst.so, pick a model, push N seconds of guitar-ish audio
 * through processReplacing in host-sized blocks, report the real-time load (100% = one full core
 * for the whole block period).
 * usage: bench <so> <model_idx> <seconds> <block> [param=value ...]   (models via $NAM_MODELS_DIR) */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <sched.h>
#include <time.h>

typedef struct AEffect AEffect;
typedef intptr_t (*amc)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
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
enum { effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetParamDisplay = 7 };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: bench <so> <model_idx> <seconds> <block> [p=v ...]\n"); return 2; }
    const double sr = 48000.0;
    if (getenv("BENCH_RT")) {   /* run like the host's audio thread so the worst-block number isn't scheduler noise */
        struct sched_param sp = {.sched_priority = 50};
        if (sched_setscheduler(0, SCHED_FIFO, &sp)) perror("sched_setscheduler");
    }
    int model = atoi(argv[2]), block = atoi(argv[4]);
    double secs = atof(argv[3]);
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    AEffect *(*entry)(amc) = (AEffect *(*)(amc))dlsym(h, "VSTPluginMain");
    AEffect *e = entry(0);
    e->dispatcher(e, effSetSampleRate, 0, 0, 0, (float)sr);
    e->dispatcher(e, effSetBlockSize, 0, block, 0, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, 0, 0);

    /* Model index -> normalized P_MODEL value (count from the param display is overkill; the
     * caller passes the model count as NAM_BENCH_COUNT). */
    int count = getenv("NAM_BENCH_COUNT") ? atoi(getenv("NAM_BENCH_COUNT")) : 1;
    for (int a = 5; a < argc; a++) { int p; float v; if (sscanf(argv[a], "%d=%f", &p, &v) == 2) e->setParameter(e, p, v); }
    e->setParameter(e, 0, count > 1 ? (float)model / (count - 1) : 0.f);
    char name[256] = {0};
    e->dispatcher(e, effGetParamDisplay, 0, 0, name, 0);

    float *L = malloc(sizeof(float) * block), *R = malloc(sizeof(float) * block);
    float *oL = malloc(sizeof(float) * block), *oR = malloc(sizeof(float) * block);
    float *in[2] = {L, R}, *out[2] = {oL, oR};
    long total = (long)(secs * sr), done = 0, ph = 0;
    double worst = 0, t0 = now(), energy = 0;
    while (done < total) {
        for (int i = 0; i < block; i++, ph++) {   /* decaying plucked-ish chord, then silence tails */
            double t = fmod(ph / sr, 2.0);
            double env = t < 1.5 ? exp(-3.0 * t) : 0.0;
            L[i] = R[i] = (float)(0.4 * env * (sin(2 * M_PI * 82.4 * ph / sr) + 0.5 * sin(2 * M_PI * 123.5 * ph / sr) + 0.3 * sin(2 * M_PI * 164.8 * ph / sr)));
        }
        double b0 = now();
        e->processReplacing(e, in, out, block);
        double dt = now() - b0;
        if (dt > worst) worst = dt;
        for (int i = 0; i < block; i++) energy += oL[i] * oL[i];
        done += block;
    }
    double wall = now() - t0, audio = (double)done / sr, period = block / sr;
    printf("%-40.40s avg %5.1f%%  worst-block %5.1f%%  (energy %.1f)\n", name, 100 * wall / audio, 100 * worst / period, energy);
    return 0;
}
