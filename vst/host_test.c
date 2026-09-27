/* Offline smoke test: dlopen the host .so, check the AEffect ABI, push audio through. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

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
enum { effGetPlugCategory = 35, effGetEffectName = 45 };

int main(int argc, char **argv) {
    const char *so = argc > 1 ? argv[1] : "./nam_vst.host.so";
    void *h = dlopen(so, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    AEffect *(*entry)(amc) = (AEffect *(*)(amc))dlsym(h, "VSTPluginMain");
    if (!entry) { fprintf(stderr, "no VSTPluginMain\n"); return 1; }

    AEffect *e = entry(0);
    if (!e) { fprintf(stderr, "VSTPluginMain returned NULL\n"); return 1; }

    int fail = 0;
    printf("magic      = 0x%08x %s\n", e->magic, e->magic == 0x56737450 ? "(VstP ok)" : (fail = 1, "BAD"));
    printf("numInputs  = %d %s\n", e->numInputs, e->numInputs == 2 ? "ok" : (fail = 1, "BAD"));
    printf("numOutputs = %d %s\n", e->numOutputs, e->numOutputs == 2 ? "ok" : (fail = 1, "BAD"));
    printf("numParams  = %d\n", e->numParams);
    char name[64] = {0};
    e->dispatcher(e, effGetEffectName, 0, 0, name, 0);
    printf("name       = \"%s\"\n", name);
    intptr_t cat = e->dispatcher(e, effGetPlugCategory, 0, 0, 0, 0);
    printf("category   = %ld %s\n", (long)cat, cat == 1 ? "(effect ok)" : (fail = 1, "BAD not-effect"));

    /* Push a 128-frame sine through and check the output is finite and reacts to the signal. */
    float L[128], R[128], oL[128], oR[128];
    for (int i = 0; i < 128; i++) L[i] = R[i] = 0.3f * sinf(2.f * 3.14159f * 440.f * i / 44100.f);
    float *in[2] = {L, R}, *out[2] = {oL, oR};
    e->setParameter(e, 0, 0.5f);   /* 0 dB in */
    e->setParameter(e, 1, 0.5f);   /* 0 dB out */
    e->processReplacing(e, in, out, 128);

    double peak = 0, energy = 0;
    int nonfinite = 0;
    for (int i = 0; i < 128; i++) {
        if (!isfinite(oL[i]) || !isfinite(oR[i])) nonfinite++;
        double a = fabs(oL[i]);
        if (a > peak) peak = a;
        energy += oL[i] * oL[i];
        if (oL[i] != oR[i]) fail = 1;   /* wrapper duplicates mono to L/R */
    }
    printf("nonfinite  = %d %s\n", nonfinite, nonfinite ? "BAD" : "ok");
    printf("out peak   = %.5f, energy = %.5f %s\n", peak, energy,
           (energy > 1e-9) ? "ok (audio present)" : (fail = 1, "BAD silent"));

    printf(fail ? "\nFAILED\n" : "\nPASSED\n");
    return fail;
}
