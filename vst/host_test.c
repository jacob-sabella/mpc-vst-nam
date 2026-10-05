/* Offline smoke test: dlopen the host .so, check the AEffect ABI, push audio through. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
enum { effGetParamDisplay = 7, effGetParamName = 8, effGetPlugCategory = 35, effGetEffectName = 45 };

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
        if (oL[i] != oR[i]) fail = 1;   /* the plugin duplicates mono to L/R */
    }
    printf("nonfinite  = %d %s\n", nonfinite, nonfinite ? "BAD" : "ok");
    printf("out peak   = %.5f, energy = %.5f %s\n", peak, energy,
           (energy > 1e-9) ? "ok (audio present)" : (fail = 1, "BAD silent"));

    /* Pitch moves in whole semitones: MPC sends a Q-Link event as the read-back value + 1/128 of the range (a fast
     * spin up to ~10/128) and a data wheel click as +-0.01, and each must move exactly one semitone (48 in range). */
    int pi = -1;
    for (int i = 0; i < e->numParams; i++) {
        char pn[64] = {0};
        e->dispatcher(e, effGetParamName, i, 0, pn, 0);
        if (!strcmp(pn, "Pitch")) pi = i;
    }
    if (pi < 0) { printf("pitch      = BAD no \"Pitch\" param\n"); fail = 1; }
    else {
        struct { float delta; int want; const char *what; } steps[] = {
            {1.f / 128, 1, "Q-Link up"}, {1.f / 128, 2, "Q-Link up"}, {-0.01f, 1, "wheel down"},
            {-1.f / 128, 0, "Q-Link down"}, {-1.f / 128, -1, "Q-Link down"}, {0.01f, 0, "wheel up"},
        };
        e->setParameter(e, pi, 0.5f);
        for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
            e->setParameter(e, pi, e->getParameter(e, pi) + steps[k].delta);
            char t[64] = {0};
            e->dispatcher(e, effGetParamDisplay, pi, 0, t, 0);
            int got = atoi(t);
            int ok = got == steps[k].want && fabsf(e->getParameter(e, pi) * 48.f - (24 + steps[k].want)) < 1e-4f;
            printf("pitch      = %-4s after %s %s\n", t, steps[k].what, ok ? "ok" : (fail = 1, "BAD"));
        }
        e->setParameter(e, pi, e->getParameter(e, pi) + 10.f / 128);   /* fast spin: 3.75 semitones */
        float st = e->getParameter(e, pi) * 48.f - 24;
        int ok = fabsf(st - roundf(st)) < 1e-4f && st >= 3 && st <= 4;
        printf("pitch      = %+.0f after a fast spin %s\n", st, ok ? "ok" : (fail = 1, "BAD"));
        /* A drag sends positions from where it started (here 0.13 semitone apart, from 0): steady, no flicker,
         * up to the end and back down. */
        e->setParameter(e, pi, 0.5f);
        float prev = 0, pos = 0; ok = 1;
        for (int k = 1; k <= 120; k++) {
            pos = 0.13f * k;
            e->setParameter(e, pi, (24 + pos) / 48.f);
            float v = e->getParameter(e, pi) * 48.f - 24;
            if (v < prev - 1e-4f || fabsf(v - roundf(v)) > 1e-4f) ok = 0;
            prev = v;
        }
        printf("pitch      = %+.0f after a drag up %s\n", prev, ok && fabsf(prev - ceilf(pos - 0.001f)) < 1e-4f ? "ok" : (fail = 1, "BAD"));
        ok = 1;
        for (int k = 1; k <= 120; k++) {
            e->setParameter(e, pi, (24 + pos - 0.13f * k) / 48.f);
            float v = e->getParameter(e, pi) * 48.f - 24;
            if (v > prev + 1e-4f || fabsf(v - roundf(v)) > 1e-4f) ok = 0;
            prev = v;
        }
        printf("pitch      = %+.0f after a drag back down %s\n", prev, ok && fabsf(prev) < 1e-4f ? "ok" : (fail = 1, "BAD"));
        usleep(200000);   /* then a Q-Link, up and straight back down: one step each way */
        e->setParameter(e, pi, e->getParameter(e, pi) + 1.f / 128);
        float a = e->getParameter(e, pi) * 48.f - 24;
        e->setParameter(e, pi, e->getParameter(e, pi) - 1.f / 128);
        float b = e->getParameter(e, pi) * 48.f - 24;
        printf("pitch      = %+.0f then %+.0f, Q-Link after a drag %s\n", a, b, fabsf(a - 1) < 1e-4f && fabsf(b) < 1e-4f ? "ok" : (fail = 1, "BAD"));
        e->setParameter(e, pi, 0.75f);   /* automation / a preset: exactly +12 */
        st = e->getParameter(e, pi) * 48.f - 24;
        printf("pitch      = %+.0f after a direct set %s\n", st, fabsf(st - 12) < 1e-4f ? "ok" : (fail = 1, "BAD"));
    }

    printf(fail ? "\nFAILED\n" : "\nPASSED\n");
    return fail;
}
