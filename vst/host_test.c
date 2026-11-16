/* Offline smoke test: dlopen the host .so, check the AEffect ABI, push audio through. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>

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
enum { effClose = 1, effGetParamDisplay = 7, effGetParamName = 8, effGetPlugCategory = 35, effGetEffectName = 45 };

/* Model loads since the last call: the .nam files opened in the watched folder, their names added to `which`. */
static int nam_loads(int fd, char *which, size_t sz) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int count = 0;
    ssize_t len;
    while ((len = read(fd, buf, sizeof buf)) > 0)
        for (char *q = buf; q < buf + len; q += sizeof(struct inotify_event) + ((struct inotify_event *)q)->len) {
            struct inotify_event *ev = (struct inotify_event *)q;
            size_t nl = ev->len ? strlen(ev->name) : 0;
            if (!(ev->mask & IN_OPEN) || nl < 4 || strcmp(ev->name + nl - 4, ".nam")) continue;
            count++;
            snprintf(which + strlen(which), sz - strlen(which), "%s%s", which[0] ? " " : "", ev->name);
        }
    return count;
}

/* Host callback for the model instance. On each redraw request (audioMasterUpdateDisplay), another thread reads
 * Model back, as a UI thread might while the plugin's own thread is in the redraw. It must get in at once: a redraw
 * made with the plugin's library lock still held can deadlock against a UI thread that waits on that lock. */
static volatile int redraws, redraws_locked;
static int probe_gen;
static int probe_done;   /* the gen of the last probe that got in (__atomic) */
static void *probe(void *arg) {
    void **a = arg;
    AEffect *e = a[0];
    int gen = (int)(intptr_t)a[1];
    free(a);
    e->getParameter(e, 0);
    __atomic_store_n(&probe_done, gen, __ATOMIC_SEQ_CST);
    return 0;
}
static intptr_t model_host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)idx; (void)v; (void)p; (void)o;
    if (op == 1) return 2400;   /* audioMasterVersion */
    if (op != 42 || !e) return 0;   /* audioMasterUpdateDisplay */
    redraws++;
    void **arg = malloc(2 * sizeof *arg);
    pthread_t th;
    int gen = __atomic_add_fetch(&probe_gen, 1, __ATOMIC_SEQ_CST);
    arg[0] = e; arg[1] = (void *)(intptr_t)gen;
    if (pthread_create(&th, 0, probe, arg)) { free(arg); return 0; }
    pthread_detach(th);   /* a probe stuck behind a held lock finishes once the redraw returns */
    for (int ms = 0; ms < 500 && __atomic_load_n(&probe_done, __ATOMIC_SEQ_CST) != gen; ms++) usleep(1000);
    if (__atomic_load_n(&probe_done, __ATOMIC_SEQ_CST) != gen) redraws_locked++;
    return 0;
}

/* `ms` of 128-frame blocks, as the audio thread runs them, with no read of any parameter in between. */
static void run_blocks(AEffect *e, float **in, float **out, int ms) {
    for (int t = 0; t < ms; t += 3) { e->processReplacing(e, in, out, 128); usleep(3000); }
}

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

    /* Model is parameter 0, and MPC's host sets parameter 0 to the far end of its range and straight back (0.0 instead
     * when it is at 0.5 or more) on insert and on every STOP. That pair must load no model; a real change (a Q-Link, the
     * data wheel, the skin) still loads, at the next block or within 50 ms; a read of it reports the new model without
     * loading it, since MPC 3.10 reads it back between the two sets on insert. A second instance, on a
     * folder of three models, watched with inotify for which .nam files get opened. */
    char base[] = "/tmp/nam_host_test.XXXXXX", mdir[64], path[96];
    const char *models[3] = {"a", "b", "c"};
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    snprintf(mdir, sizeof mdir, "%s/models", base);
    mkdir(mdir, 0755);
    for (int k = 0; k < 3; k++) {
        snprintf(path, sizeof path, "%s/%s.nam", mdir, models[k]);
        FILE *f = fopen(path, "w");
        if (f) { fputs("{\"version\": \"0.5.0\", \"architecture\": \"Linear\", \"config\": {\"receptive_field\": 1, "
                       "\"bias\": false}, \"weights\": [1.0], \"sample_rate\": 48000}\n", f); fclose(f); }
    }
    setenv("NAM_MODELS_DIR", mdir, 1);
    AEffect *m = entry(model_host);   /* loads a.nam, before the watch */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (!m || ifd < 0 || inotify_add_watch(ifd, mdir, IN_OPEN) < 0) { printf("model      = BAD no second instance or inotify\n"); fail = 1; }
    else {
        char which[256] = "", t[64];
        struct { const char *what; float to; int nudge, blocks, read_back, want_loads; const char *want; } steps[] = {
            {"a set, then blocks", 0.5f, 0, 1, 0, 1, "b"},
            {"host nudge, then blocks", 0, 1, 1, 0, 0, "b"},
            {"a set, read back at once", 0.f, 0, 0, 1, 1, "a"},
            {"host nudge, no blocks", 0, 1, 0, 0, 0, "a"},
            {"a set, no blocks or reads", 1.f, 0, 0, 0, 1, "c"},
            {"host nudge, read back at once", 0, 1, 0, 1, 0, "c"},
            {"host nudge read back in between", 0, 2, 0, 1, 0, "c"},   /* MPC 3.10 on insert */
            {"host nudge read back in between, then blocks", 0, 2, 1, 0, 0, "c"},
        };
        for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
            float old = m->getParameter(m, 0), want_v = steps[k].nudge ? old : steps[k].to;
            nam_loads(ifd, which, sizeof which);   /* drop anything before */
            which[0] = 0;
            if (steps[k].nudge) {
                m->setParameter(m, 0, old < 0.5f ? 1.f : 0.f);
                if (steps[k].nudge == 2) {   /* as MPC 3.10 does on insert: read it back, set it back 17 ms later */
                    float mid = m->getParameter(m, 0);
                    memset(t, 0, sizeof t);
                    m->dispatcher(m, effGetParamDisplay, 0, 0, t, 0);
                    if (fabsf(mid - (old < 0.5f ? 1.f : 0.f)) > 1e-6f || strcmp(t, old < 0.5f ? "c" : "a")) {
                        printf("model      = %-2s %.1f read back mid-nudge BAD\n", t, mid); fail = 1;
                    }
                    usleep(17000);
                }
                m->setParameter(m, 0, old);
            }
            else m->setParameter(m, 0, steps[k].to);
            if (steps[k].blocks) run_blocks(m, in, out, 120);
            else if (!steps[k].read_back) usleep(120000);   /* the poll thread applies it on its own */
            int loads = nam_loads(ifd, which, sizeof which);   /* read before the checks below, which apply it */
            float got = m->getParameter(m, 0);
            memset(t, 0, sizeof t);
            m->dispatcher(m, effGetParamDisplay, 0, 0, t, 0);
            if (steps[k].read_back && nam_loads(ifd, which, sizeof which)) { printf("model      = a read loaded BAD\n"); fail = 1; }
            usleep(120000);   /* a read reports a held set without loading it; the poll thread loads it */
            loads += nam_loads(ifd, which, sizeof which);
            int ok = loads == steps[k].want_loads && fabsf(got - want_v) < 1e-6f && !strcmp(t, steps[k].want);
            printf("model      = %-2s %.1f after %s, %d load%s%s%s %s\n", t, got, steps[k].what, loads, loads == 1 ? "" : "s",
                   loads ? ": " : "", which, ok ? "ok" : (fail = 1, "BAD"));
        }
        printf("model      = %d redraws, %d with the library lock held %s\n", redraws, redraws_locked,
               redraws >= 3 && !redraws_locked ? "ok" : (fail = 1, "BAD"));
        usleep(600000);   /* any stuck probe gets in before the close */
        m->dispatcher(m, effClose, 0, 0, 0, 0);
    }
    if (ifd >= 0) close(ifd);
    for (int k = 0; k < 3; k++) { snprintf(path, sizeof path, "%s/%s.nam", mdir, models[k]); unlink(path); }
    rmdir(mdir);
    rmdir(base);

    e->dispatcher(e, effClose, 0, 0, 0, 0);
    printf(fail ? "\nFAILED\n" : "\nPASSED\n");
    return fail;
}
