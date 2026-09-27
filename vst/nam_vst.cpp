/* nam_vst.cpp — Neural Amp Modeler as a native VST2 EFFECT for MPC OS standalone.
 *
 * The framework's generic wrapper/vst2_wrap.c is synth-shaped (numInputs=0, effFlagsIsSynth, its
 * processReplacing discards `in`). NAM is an effect that must read the input, so this is a
 * port-specific hand-written AEffect on the same VST2 ABI (docs/PORTING.md category 2), modelled on
 * poc/gain.c.
 *
 * Signal path (mono internally -- one NAM model is one mono amp; per-channel would double RK3288
 * CPU): stereo in -> mono sum -> Input Gain -> Noise Gate -> NAM model -> Tone Stack (Bass/Mid/
 * Treble) -> Cab IR (optional convolution) -> Pitch Shift (optional) -> Delay (optional) ->
 * Reverb (optional) -> Output Gain -> dup to L/R.
 *
 * Params: see the P_* enum below. The list is append-only -- MPC stores automation and Q-Link
 * mappings by index, so an existing index must never move or change meaning. Every block after
 * the model defaults to a transparent/off state.
 *
 * Models are discovered at load in the first existing of: $NAM_MODELS_DIR, <dir of this .so>/models,
 * /media/0180-2800/nam-host/models, /media/acvs-content/nam-host/models. Cab IRs use the same
 * search order under NAM_CABS_DIR / .../cabs, loading mono .wav files (16/24/32-bit PCM or float),
 * truncated to CAB_MAXTAPS samples (~93ms @44.1kHz) -- direct time-domain convolution, so longer
 * IRs are capped rather than resampled/streamed; this covers the vast majority of real cab IR packs
 * and keeps the worst case around 180M MACs/sec, comfortably inside the RK3288's budget.
 *
 * Pitch shift is a time-domain splicing shifter with correlation-chosen splice points (see
 * pitch_shift.h): no FFT, so it stays cheap and low-latency (~2-11ms) for playing through live.
 *
 * The selected model and cab IR are saved by NAME (survives directory reordering/reinstalls); every
 * other parameter is saved by index as plain text key=value lines in the VST2 chunk (effFlagsProgramChunks
 * means the host defers ALL state to this chunk, not just per-index automation, so every knob must be
 * included here for save/reload to round-trip). A chunk with no '=' is read as a bare model name.
 */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <map>
#include <set>
#include <strings.h>
#include <filesystem>
#include <fstream>
#include "pitch_shift.h"
#include <sstream>
#include <sys/stat.h>
#include <dlfcn.h>
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "NAM/activations.h"
#include "json.hpp"
#include "t3k.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct AEffect AEffect;
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

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effCanBeAutomated = 26, effGetPlugCategory = 35, effGetEffectName = 45,
    effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49,
    effCanDo = 51, effGetVstVersion = 58,
};
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };
enum { audioMasterUpdateDisplay = 42 };

#define PLUG_NAME    "NAM"
#define PLUG_VENDOR  "jacob-sabella"
#ifndef PLUG_VERSION
#error "PLUG_VERSION is set by vst/build.sh (scripts/version.py --vst)"
#endif
#define PLUG_UID     0x4E416D31 /* 'NAm1' -- keep fixed across versions; projects find the plugin by uid */
#define MAXBLOCK     4096
#define CAB_MAXTAPS  4096

enum {
    P_MODEL = 0, P_INGAIN, P_OUTGAIN,
    P_GATE_THRESH, P_GATE_RELEASE,
    P_BASS, P_MID, P_TREBLE,
    P_CAB_ON, P_CAB_SELECT,
    P_PITCH_ON, P_PITCH_SEMI, P_PITCH_MIX,
    P_DELAY_TIME, P_DELAY_FB, P_DELAY_MIX,
    P_REVERB_SIZE, P_REVERB_MIX,
    /* Momentary browse triggers for the skin's "stepper" widgets (docs/SKIN_STUDIO.md's
     * "<key>_prev"/"<key>_next" convention) -- Model/Cab IR are non-knob list browsers on screen,
     * not automatable knobs, so they need a distinct prev/next VST param pair each. */
    P_MODEL_PREV, P_MODEL_NEXT, P_CAB_PREV, P_CAB_NEXT,
    /* Model browser (BROWSE tab): the skin binds the first SLOTS_PER_PAGE slots to list rows
     * (layout.conf); P_SLOT_7/8 are unbound and reserved to keep later indices stable.
     * A slot reads 1.0 when its row holds the loaded model (tile highlight); a tap acts on that row.
     * Display text is the label of view row browse_page*SLOTS_PER_PAGE + slot. */
    P_SLOT_1, P_SLOT_2, P_SLOT_3, P_SLOT_4, P_SLOT_5, P_SLOT_6, P_SLOT_7, P_SLOT_8,
    P_MODEL_PAGE, P_MODEL_PAGE_PREV, P_MODEL_PAGE_NEXT,
    /* A2 "slimmable" models carry a lite submodel next to the full one; LITE runs the lite one
     * (~1/6 the MACs), which is the difference between fitting and not fitting on the RK3288. */
    P_QUALITY,
    /* Tone3000 browser (TONE3000 tab): sign-in, search and download run in-process in t3k.cpp's
     * service; this side only turns taps into requests and shows the answers. */
    P_T3K_STATUS, P_T3K_SORT,
    /* Search filters (TONE3000 tab). ARCH defaults to MPC (architecture=2 + lite/feather/nano):
     * without it the API returns A1+custom captures, which don't fit on the RK3288. GEAR and MAKE
     * are steppers (many values); QUALITY packs the calibrated/verified booleans into one enum. */
    P_T3K_ARCH, P_T3K_GEAR, P_T3K_GEAR_PREV, P_T3K_GEAR_NEXT,
    P_T3K_QUALITY, P_T3K_MAKE, P_T3K_MAKE_PREV, P_T3K_MAKE_NEXT,
    P_T3K_SLOT_1, P_T3K_SLOT_2, P_T3K_SLOT_3, P_T3K_SLOT_4, P_T3K_SLOT_5, P_T3K_SLOT_6,
    P_T3K_PAGE, P_T3K_PAGE_PREV, P_T3K_PAGE_NEXT,
    /* Select-then-confirm download: tapping a result row only sets t3k_selected (below) and shows
     * it in T3K_DETAIL; the download request (t3k::download) waits for T3K_CONFIRM. */
    P_T3K_DETAIL, P_T3K_CONFIRM, P_T3K_CANCEL,
    /* BROWSE management: ALL (pack folders + loose models) / FAVORITES / RECENT (newest file first),
     * a readout for the loaded model or a pending delete, and FAVORITE/DELETE buttons that act on the
     * loaded model. DELETE is two-tap (arm, then confirm within 6s) and moves the file to
     * ../models-removed rather than unlinking it. */
    P_BROWSE_FILTER, P_BROWSE_DETAIL, P_BROWSE_FAV, P_BROWSE_DELETE,
    NPARAMS
};
#define SLOTS_PER_PAGE 6
#define T3K_SLOTS_PER_PAGE 6
#define T3K_ROWS 5            /* results per API page = rows the TONE3000 list shows (layout.conf);
                                 * P_T3K_SLOT_6 is unbound */

static const float GAIN_MIN_DB = -20.f, GAIN_MAX_DB = 20.f;
static const float GATE_MIN_DB = -80.f, GATE_MAX_DB = 0.f;
static const float GATE_REL_MIN = 20.f, GATE_REL_MAX = 1000.f;
static const float TONE_MIN_DB = -15.f, TONE_MAX_DB = 15.f;
static const float PITCH_MIN_ST = -24.f, PITCH_MAX_ST = 24.f;
static const float DELAY_MIN_MS = 1.f, DELAY_MAX_MS = 1000.f;
static const float DELAY_FB_MAX = 95.f;

struct ParamInfo { const char *name; const char *unit; float lo, hi; bool isBool; bool isEnum; };
static const ParamInfo PINFO[NPARAMS] = {
    /*P_MODEL*/        {"Model", "", 0, 0, false, true},
    /*P_INGAIN*/       {"Input Gain", "dB", GAIN_MIN_DB, GAIN_MAX_DB, false, false},
    /*P_OUTGAIN*/      {"Output Gain", "dB", GAIN_MIN_DB, GAIN_MAX_DB, false, false},
    /*P_GATE_THRESH*/  {"Gate Thresh", "dB", GATE_MIN_DB, GATE_MAX_DB, false, false},
    /*P_GATE_RELEASE*/ {"Gate Release", "ms", GATE_REL_MIN, GATE_REL_MAX, false, false},
    /*P_BASS*/         {"Bass", "dB", TONE_MIN_DB, TONE_MAX_DB, false, false},
    /*P_MID*/          {"Mid", "dB", TONE_MIN_DB, TONE_MAX_DB, false, false},
    /*P_TREBLE*/       {"Treble", "dB", TONE_MIN_DB, TONE_MAX_DB, false, false},
    /*P_CAB_ON*/       {"Cab On", "", 0, 1, true, false},
    /*P_CAB_SELECT*/   {"Cab IR", "", 0, 0, false, true},
    /*P_PITCH_ON*/     {"Pitch On", "", 0, 1, true, false},
    /*P_PITCH_SEMI*/   {"Pitch", "st", PITCH_MIN_ST, PITCH_MAX_ST, false, false},
    /*P_PITCH_MIX*/    {"Pitch Mix", "%", 0.f, 100.f, false, false},
    /*P_DELAY_TIME*/   {"Delay Time", "ms", DELAY_MIN_MS, DELAY_MAX_MS, false, false},
    /*P_DELAY_FB*/     {"Delay FB", "%", 0.f, DELAY_FB_MAX, false, false},
    /*P_DELAY_MIX*/    {"Delay Mix", "%", 0.f, 100.f, false, false},
    /*P_REVERB_SIZE*/  {"Reverb Size", "%", 0.f, 100.f, false, false},
    /*P_REVERB_MIX*/   {"Reverb Mix", "%", 0.f, 100.f, false, false},
    /*P_MODEL_PREV*/   {"Model -", "", 0.f, 1.f, false, false},
    /*P_MODEL_NEXT*/   {"Model +", "", 0.f, 1.f, false, false},
    /*P_CAB_PREV*/     {"Cab -", "", 0.f, 1.f, false, false},
    /*P_CAB_NEXT*/     {"Cab +", "", 0.f, 1.f, false, false},
    /*P_SLOT_1*/       {"Model 1", "", 0, 0, false, true},
    /*P_SLOT_2*/       {"Model 2", "", 0, 0, false, true},
    /*P_SLOT_3*/       {"Model 3", "", 0, 0, false, true},
    /*P_SLOT_4*/       {"Model 4", "", 0, 0, false, true},
    /*P_SLOT_5*/       {"Model 5", "", 0, 0, false, true},
    /*P_SLOT_6*/       {"Model 6", "", 0, 0, false, true},
    /*P_SLOT_7*/       {"Model 7", "", 0, 0, false, true},
    /*P_SLOT_8*/       {"Model 8", "", 0, 0, false, true},
    /*P_MODEL_PAGE*/   {"Page", "", 0, 0, false, true},
    /*P_MODEL_PAGE_PREV*/ {"Page -", "", 0.f, 1.f, false, false},
    /*P_MODEL_PAGE_NEXT*/ {"Page +", "", 0.f, 1.f, false, false},
    /*P_QUALITY*/      {"Quality", "", 0, 1, true, false},
    /*P_T3K_STATUS*/   {"T3K Status", "", 0, 0, false, true},
    /*P_T3K_SORT*/     {"T3K Sort", "", 0, 3, false, true},
    /*P_T3K_ARCH*/     {"T3K Arch", "", 0, 2, false, true},
    /*P_T3K_GEAR*/     {"T3K Gear", "", 0, 0, false, true},
    /*P_T3K_GEAR_PREV*/{"T3K Gear -", "", 0.f, 1.f, false, false},
    /*P_T3K_GEAR_NEXT*/{"T3K Gear +", "", 0.f, 1.f, false, false},
    /*P_T3K_QUALITY*/  {"T3K Quality", "", 0, 3, false, true},
    /*P_T3K_MAKE*/     {"T3K Make", "", 0, 0, false, true},
    /*P_T3K_MAKE_PREV*/{"T3K Make -", "", 0.f, 1.f, false, false},
    /*P_T3K_MAKE_NEXT*/{"T3K Make +", "", 0.f, 1.f, false, false},
    /*P_T3K_SLOT_1*/   {"T3K 1", "", 0, 0, false, true},
    /*P_T3K_SLOT_2*/   {"T3K 2", "", 0, 0, false, true},
    /*P_T3K_SLOT_3*/   {"T3K 3", "", 0, 0, false, true},
    /*P_T3K_SLOT_4*/   {"T3K 4", "", 0, 0, false, true},
    /*P_T3K_SLOT_5*/   {"T3K 5", "", 0, 0, false, true},
    /*P_T3K_SLOT_6*/   {"T3K 6", "", 0, 0, false, true},
    /*P_T3K_PAGE*/     {"T3K Page", "", 0, 0, false, true},
    /*P_T3K_PAGE_PREV*/{"T3K Page -", "", 0.f, 1.f, false, false},
    /*P_T3K_PAGE_NEXT*/{"T3K Page +", "", 0.f, 1.f, false, false},
    /*P_T3K_DETAIL*/   {"T3K Detail", "", 0, 0, false, true},
    /*P_T3K_CONFIRM*/  {"T3K Download", "", 0.f, 1.f, false, false},
    /*P_T3K_CANCEL*/   {"T3K Cancel", "", 0.f, 1.f, false, false},
    /*P_BROWSE_FILTER*/{"Browse Filter", "", 0, 2, false, true},
    /*P_BROWSE_DETAIL*/{"Browse Detail", "", 0, 0, false, true},
    /*P_BROWSE_FAV*/   {"Favorite", "", 0.f, 1.f, false, false},
    /*P_BROWSE_DELETE*/{"Delete", "", 0.f, 1.f, false, false},
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float mapRange(float norm, float lo, float hi) { return lo + (hi - lo) * clamp01(norm); }
static float dbToLin(float db) { return std::pow(10.f, db / 20.f); }

static std::string self_dir() {
    Dl_info info;
    if (dladdr((void *)&self_dir, &info) && info.dli_fname) {
        try { return std::filesystem::path(info.dli_fname).parent_path().string(); }
        catch (...) {}
    }
    return "";
}

static bool has_ext(const std::string &dir, const char *ext) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return false;
    for (auto &e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ext) return true;
    return false;
}

static std::string models_dir() {
    if (const char *e = std::getenv("NAM_MODELS_DIR")) { if (has_ext(e, ".nam")) return e; }
    std::string sd = self_dir();
    if (!sd.empty() && has_ext(sd + "/models", ".nam")) return sd + "/models";
    if (!sd.empty() && has_ext(sd, ".nam")) return sd;
    std::error_code ec;   /* every model may live in a pack folder, leaving none at the top level */
    if (!sd.empty() && std::filesystem::is_directory(sd + "/models", ec)) return sd + "/models";
    for (const char *c : {"/media/0180-2800/nam-host/models", "/media/acvs-content/nam-host/models"})
        if (has_ext(c, ".nam")) return c;
    return sd.empty() ? "." : sd;
}

static std::string cabs_dir() {
    if (const char *e = std::getenv("NAM_CABS_DIR")) { if (has_ext(e, ".wav")) return e; }
    std::string sd = self_dir();
    if (!sd.empty() && has_ext(sd + "/cabs", ".wav")) return sd + "/cabs";
    for (const char *c : {"/media/0180-2800/nam-host/cabs", "/media/acvs-content/nam-host/cabs"})
        if (has_ext(c, ".wav")) return c;
    return sd.empty() ? "cabs" : sd + "/cabs";
}

/* Minimal RIFF/WAVE reader: mono-izes (averages channels), supports 16/24/32-bit PCM and 32-bit
 * float, truncates to maxFrames. Good enough for cab IRs; not a general-purpose WAV loader. */
static std::vector<float> load_wav_mono(const std::string &path, size_t maxFrames) {
    std::vector<float> out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char riff[4], wave[4];
    uint32_t riffSize;
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) ||
        fread(&riffSize, 4, 1, f) != 1 ||
        fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4)) { fclose(f); return out; }

    uint16_t audioFormat = 1, numChannels = 1, bitsPerSample = 16;
    bool haveFmt = false;
    long dataPos = -1;
    uint32_t dataSize = 0;
    for (;;) {
        char id[4]; uint32_t sz;
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
        long chunkStart = ftell(f);
        if (!memcmp(id, "fmt ", 4) && sz >= 16) {
            uint8_t fb[16];
            if (fread(fb, 1, 16, f) != 16) break;
            audioFormat = (uint16_t)(fb[0] | (fb[1] << 8));
            numChannels = (uint16_t)(fb[2] | (fb[3] << 8));
            bitsPerSample = (uint16_t)(fb[14] | (fb[15] << 8));
            haveFmt = true;
        } else if (!memcmp(id, "data", 4)) {
            dataPos = chunkStart;
            dataSize = sz;
        }
        if (fseek(f, chunkStart + (long)sz + (long)(sz & 1), SEEK_SET) != 0) break;
        if (feof(f)) break;
    }
    if (!haveFmt || dataPos < 0 || numChannels == 0 || bitsPerSample == 0) { fclose(f); return out; }
    fseek(f, dataPos, SEEK_SET);

    size_t bytesPerSample = bitsPerSample / 8;
    size_t frameBytes = bytesPerSample * numChannels;
    size_t nFrames = frameBytes ? dataSize / frameBytes : 0;
    size_t nUse = std::min(nFrames, maxFrames);
    out.reserve(nUse);
    std::vector<uint8_t> raw(frameBytes);
    for (size_t i = 0; i < nUse; i++) {
        if (fread(raw.data(), 1, frameBytes, f) != frameBytes) break;
        double sum = 0;
        for (int c = 0; c < numChannels; c++) {
            const uint8_t *s = &raw[(size_t)c * bytesPerSample];
            double v = 0;
            if (audioFormat == 3 && bytesPerSample == 4) { float fv; memcpy(&fv, s, 4); v = fv; }
            else if (bytesPerSample == 2) { int16_t iv = (int16_t)(s[0] | (s[1] << 8)); v = iv / 32768.0; }
            else if (bytesPerSample == 3) {
                int32_t iv = s[0] | (s[1] << 8) | (s[2] << 16);
                if (iv & 0x800000) iv |= (int32_t)0xFF000000u;
                v = iv / 8388608.0;
            } else if (bytesPerSample == 4) { int32_t iv; memcpy(&iv, s, 4); v = iv / 2147483648.0; }
            sum += v;
        }
        out.push_back((float)(sum / numChannels));
    }
    fclose(f);
    return out;
}

/* ---- Noise gate: envelope follower + smoothed on/off gain (fast open, param'd release). ---- */
struct Gate {
    float env = 0.f, gain = 1.f;
    void process(float *buf, int32_t n, double sr, float threshDb, float releaseMs) {
        float threshLin = dbToLin(threshDb);
        float envAtk = std::exp(-1.f / (0.002f * (float)sr));
        float envRel = std::exp(-1.f / (0.050f * (float)sr));
        float gOpen = std::exp(-1.f / (0.002f * (float)sr));
        float gClose = std::exp(-1.f / (std::max(releaseMs, 1.f) / 1000.f * (float)sr));
        for (int32_t i = 0; i < n; i++) {
            float rect = std::fabs(buf[i]);
            env = rect > env ? envAtk * env + (1.f - envAtk) * rect : envRel * env + (1.f - envRel) * rect;
            float target = env >= threshLin ? 1.f : 0.f;
            gain = target > gain ? gOpen * gain + (1.f - gOpen) * target : gClose * gain + (1.f - gClose) * target;
            buf[i] *= gain;
        }
    }
};

/* ---- Tone stack: RBJ cookbook low-shelf / peaking / high-shelf biquads, transposed DF-II. ---- */
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float process(float x) {
        double in = x;
        double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return (float)out;
    }
    void setLowShelf(double freq, double sr, double gainDB) {
        double A = std::pow(10.0, gainDB / 40.0), w0 = 2.0 * M_PI * freq / sr;
        double cw = std::cos(w0), sw = std::sin(w0), S = 1.0;
        double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        double sA2a = 2.0 * std::sqrt(A) * alpha;
        double a0 = (A + 1) + (A - 1) * cw + sA2a;
        b0 = (A * ((A + 1) - (A - 1) * cw + sA2a)) / a0;
        b1 = (2 * A * ((A - 1) - (A + 1) * cw)) / a0;
        b2 = (A * ((A + 1) - (A - 1) * cw - sA2a)) / a0;
        a1 = (-2 * ((A - 1) + (A + 1) * cw)) / a0;
        a2 = ((A + 1) + (A - 1) * cw - sA2a) / a0;
    }
    void setHighShelf(double freq, double sr, double gainDB) {
        double A = std::pow(10.0, gainDB / 40.0), w0 = 2.0 * M_PI * freq / sr;
        double cw = std::cos(w0), sw = std::sin(w0), S = 1.0;
        double alpha = sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        double sA2a = 2.0 * std::sqrt(A) * alpha;
        double a0 = (A + 1) - (A - 1) * cw + sA2a;
        b0 = (A * ((A + 1) + (A - 1) * cw + sA2a)) / a0;
        b1 = (-2 * A * ((A - 1) + (A + 1) * cw)) / a0;
        b2 = (A * ((A + 1) + (A - 1) * cw - sA2a)) / a0;
        a1 = (2 * ((A - 1) - (A + 1) * cw)) / a0;
        a2 = ((A + 1) - (A - 1) * cw - sA2a) / a0;
    }
    void setPeaking(double freq, double sr, double gainDB, double Q) {
        double A = std::pow(10.0, gainDB / 40.0), w0 = 2.0 * M_PI * freq / sr;
        double cw = std::cos(w0), sw = std::sin(w0);
        double alpha = sw / (2.0 * Q);
        double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cw) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cw) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }
};
struct ToneStack {
    Biquad bassF, midF, trebF;
    float lastBass = 1e9f, lastMid = 1e9f, lastTreb = 1e9f;
    void update(float bassDb, float midDb, float trebDb, double sr) {
        if (bassDb != lastBass) { bassF.setLowShelf(120.0, sr, bassDb); lastBass = bassDb; }
        if (midDb != lastMid) { midF.setPeaking(800.0, sr, midDb, 0.7); lastMid = midDb; }
        if (trebDb != lastTreb) { trebF.setHighShelf(3000.0, sr, trebDb); lastTreb = trebDb; }
    }
    void process(float *buf, int32_t n) {
        for (int32_t i = 0; i < n; i++) buf[i] = trebF.process(midF.process(bassF.process(buf[i])));
    }
};

/* ---- Cab IR: direct time-domain convolution, capped at CAB_MAXTAPS. ---- */
struct CabIR {
    std::string dir;
    std::vector<std::string> names;
    int idx = 0;
    std::mutex mtx;
    std::vector<float> taps, hist, combined;

    void enumerate() {
        dir = cabs_dir();
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec))
            for (auto &e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".wav") names.push_back(e.path().stem().string());
        std::sort(names.begin(), names.end());
    }
    void load(int i) {
        if (names.empty()) return;
        if (i < 0) i = 0;
        if (i >= (int)names.size()) i = (int)names.size() - 1;
        std::string path = dir + "/" + names[i] + ".wav";
        std::vector<float> freshTaps = load_wav_mono(path, CAB_MAXTAPS);
        std::reverse(freshTaps.begin(), freshTaps.end());   /* stored time-reversed, see process() */
        std::vector<float> freshHist(freshTaps.size(), 0.f);
        std::lock_guard<std::mutex> lk(mtx);
        taps = std::move(freshTaps);
        hist = std::move(freshHist);
        idx = i;
    }
    void process(float *buf, int32_t n) {
        std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
        if (!lk.owns_lock() || taps.empty()) return;
        size_t L = taps.size();
        if (combined.size() < L + (size_t)n) combined.resize(L + n);
        memcpy(combined.data(), hist.data(), L * sizeof(float));
        memcpy(combined.data() + L, buf, (size_t)n * sizeof(float));
        /* taps are time-reversed, so y[i] = sum_j taps[j] * x[i+1+j] is a forward dot product. */
        const float *t = taps.data();
        for (int32_t i = 0; i < n; i++) {
            const float *x = combined.data() + i + 1;
            float acc = 0.f;
            for (size_t j = 0; j < L; j++) acc += t[j] * x[j];
            buf[i] = acc;
        }
        memmove(hist.data(), combined.data() + n, L * sizeof(float));
    }
};

/* ---- Delay: single feedback tap, mix-controlled. ---- */
struct Delay {
    std::vector<float> buf;
    int pos = 0;
    void ensureSize(double sr) {
        size_t want = (size_t)(2.0 * sr) + 16;
        if (buf.size() < want) { buf.assign(want, 0.f); pos = 0; }
    }
    void process(float *b, int32_t n, double sr, float timeMs, float feedback, float mix) {
        ensureSize(sr);
        int bufN = (int)buf.size();
        int delaySamples = std::max(1, std::min((int)(timeMs / 1000.0 * sr), bufN - 1));
        feedback = std::min(std::max(feedback, 0.f), 0.98f);
        for (int32_t i = 0; i < n; i++) {
            int readIdx = pos - delaySamples; if (readIdx < 0) readIdx += bufN;
            float wet = buf[(size_t)readIdx];
            float in = b[i];
            buf[(size_t)pos] = in + wet * feedback;
            b[i] = in * (1.f - mix) + wet * mix;
            pos++; if (pos >= bufN) pos = 0;
        }
    }
};

/* ---- Reverb: mono Freeverb-lite (8 combs + 4 allpasses, fixed 44.1kHz tunings/damping). ---- */
struct RevComb {
    std::vector<float> buf; int pos = 0; float filt = 0.f;
    void init(int size) { buf.assign((size_t)size, 0.f); pos = 0; filt = 0.f; }
    inline float process(float in, float feedback, float damp1, float damp2) {
        float out = buf[(size_t)pos];
        filt = out * damp2 + filt * damp1;
        buf[(size_t)pos] = in + filt * feedback;
        pos++; if (pos >= (int)buf.size()) pos = 0;
        return out;
    }
};
struct RevAllpass {
    std::vector<float> buf; int pos = 0;
    void init(int size) { buf.assign((size_t)size, 0.f); pos = 0; }
    inline float process(float in, float feedback = 0.5f) {
        float bufout = buf[(size_t)pos];
        float out = -in + bufout;
        buf[(size_t)pos] = in + bufout * feedback;
        pos++; if (pos >= (int)buf.size()) pos = 0;
        return out;
    }
};
struct Reverb {
    static const int NCOMB = 8, NALLPASS = 4;
    RevComb combs[NCOMB];
    RevAllpass allpasses[NALLPASS];
    void init() {
        static const int combTuning[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1497, 1568, 1617};
        static const int apTuning[NALLPASS] = {556, 441, 341, 225};
        for (int i = 0; i < NCOMB; i++) combs[i].init(combTuning[i]);
        for (int i = 0; i < NALLPASS; i++) allpasses[i].init(apTuning[i]);
    }
    void process(float *b, int32_t n, float size, float mix) {
        float feedback = 0.7f + size * 0.28f;
        float damp1 = 0.2f, damp2 = 1.f - damp1;
        for (int32_t i = 0; i < n; i++) {
            float in = b[i], out = 0.f;
            for (int c = 0; c < NCOMB; c++) out += combs[c].process(in, feedback, damp1, damp2);
            out *= (1.f / NCOMB);
            for (int a = 0; a < NALLPASS; a++) out = allpasses[a].process(out, 0.5f);
            b[i] = in * (1.f - mix) + out * mix;
        }
    }
};

struct Nam {
    AEffect fx;
    audioMasterCallback master = nullptr;
    double sr = 44100.0;
    float p[NPARAMS];

    /* Model library. names are paths relative to dir without ".nam": "Clean Tone" for a loose model,
     * "Soldano SLO 100/AMP_SLO100..." for one inside a pack folder (Tone3000 packs download into
     * their own folder). The poll thread rescans when the folders change, so lib_mtx guards
     * names/mtimes/favs/the BROWSE view -- everything here but dir. */
    std::string dir;
    std::recursive_mutex lib_mtx;
    std::vector<std::string> names;
    std::vector<long long> mtimes;   /* per names[k], for RECENT */
    std::set<std::string> favs;      /* names[] entries, persisted in <dir>/../favorites.txt */
    int model_idx = 0;
    /* BROWSE view: the rows the list currently shows, rebuilt by build_view on any filter/folder/
     * library change. browse_page pages through view, not names. */
    struct Row { int kind; int idx; std::string key, label; };   /* kind: 0 model(idx), 1 folder(key), 2 back */
    std::vector<Row> view;
    int filter = 0;                  /* 0 ALL, 1 FAVORITES, 2 RECENT (matches browse_filter options) */
    std::string folder;              /* ALL: pack folder being shown, "" = top level */
    int browse_page = 0;
    long long lib_sig = 0, favs_key = -1;
    long long del_armed_ms = 0;      /* DELETE pressed once at this time; a second press within 6s removes */
    std::string browse_msg;          /* one-shot result line ("Removed ..."), cleared by the next action */

    std::mutex dsp_mtx;
    std::unique_ptr<nam::DSP> dsp;
    std::vector<NAM_SAMPLE> din, dout;
    std::vector<float> mbuf;

    Gate gate;
    ToneStack tone;
    CabIR cab;
    PitchShift pitch;
    bool pitch_was_on = false;
    Delay delay;
    Reverb reverb;

    /* Tone3000 tab: this instance's view of the t3k service (t3k.h). The poll thread picks up
     * results/status and asks MPC to redraw -- MPC does not poll display text on its own. Every
     * request carries a seq, so an answer to an older request is never shown as current.
     * t3k_mtx guards everything below that the poll thread writes. */
    std::recursive_mutex t3k_mtx;   /* recursive: ask_redraw under it may re-enter effGetParamDisplay */
    std::thread poll_thread;        /* shared by the TONE3000 poll and the model-library rescan */
    std::atomic<bool> poll_run{false};
    std::atomic<bool> t3k_active{false};
    int t3k_sort = 0;              /* 0=trending, 1=newest, 2=downloads, 3=favorites (matches SORT enum_h options) */
    int t3k_arch = 0;              /* 0=MPC (A2 lite/feather/nano), 1=A2 (all sizes), 2=STD (A1+custom) */
    int t3k_gear = 0;              /* index into T3K_GEARS (0 = All) */
    int t3k_quality = 0;           /* 0=Any, 1=Calibrated, 2=Verified, 3=Cal+Ver */
    int t3k_make = 0;              /* index into T3K_MAKES (0 = All) */
    int t3k_page = 0;              /* 0-based; t3k_request.json's "page" is 1-based for the API */
    int t3k_total_pages = 1;
    std::string t3k_items[T3K_SLOTS_PER_PAGE];
    long long t3k_item_ids[T3K_SLOTS_PER_PAGE] = {0};
    long long t3k_seq = 0;         /* seq of the latest request we wrote (browse or download) */
    long long t3k_browse_seq = 0;  /* seq of the latest sort/page change -- results must match it */
    bool t3k_last_was_download = false;
    std::string t3k_dl_name;       /* tone name of the last DOWNLOAD press, for the status line */
    long long t3k_req_ms = 0;      /* when t3k_seq was written, for the "worker not answering" hint */
    bool t3k_have_results = false;  /* results for t3k_browse_seq have been picked up */
    t3k::Status t3k_st;             /* last status snapshot from the in-process Tone3000 service */
    std::string t3k_status_text = "Connecting...";
    /* Select-then-confirm: index into t3k_items/t3k_item_ids (-1 = none selected). Plain
     * in-memory UI state, not a VST param -- never saved/restored via the chunk. */
    int t3k_selected = -1;

    char chunk[1024];
};

/* Load model by index. Caller must NOT hold dsp_mtx; this takes it for the swap. */
static void load_index(Nam *n, int idx) {
    std::lock_guard<std::recursive_mutex> llk(n->lib_mtx);
    if (n->names.empty()) return;
    if (idx < 0) idx = 0;
    if (idx >= (int)n->names.size()) idx = (int)n->names.size() - 1;
    std::string path = n->dir + "/" + n->names[idx] + ".nam";
    std::unique_ptr<nam::DSP> fresh;
    try {
        fresh = nam::get_dsp(std::filesystem::path(path));
        if (auto *slim = dynamic_cast<nam::SlimmableModel *>(fresh.get()))
            slim->SetSlimmableSize(n->p[P_QUALITY] >= 0.5f ? 1.0 : 0.0);
        if (fresh) fresh->Reset(n->sr, MAXBLOCK);
    } catch (...) {
        fresh.reset();
    }
    std::lock_guard<std::mutex> lk(n->dsp_mtx);
    n->dsp = std::move(fresh);
    n->model_idx = idx;
}

static void set_defaults(Nam *n) {
    for (int i = 0; i < NPARAMS; i++) n->p[i] = 0.5f;   /* correct default for every symmetric ±range param */
    n->p[P_GATE_THRESH] = (-50.f - GATE_MIN_DB) / (GATE_MAX_DB - GATE_MIN_DB);
    n->p[P_GATE_RELEASE] = (150.f - GATE_REL_MIN) / (GATE_REL_MAX - GATE_REL_MIN);
    n->p[P_CAB_ON] = 0.f;
    n->p[P_CAB_SELECT] = 0.f;
    n->p[P_PITCH_ON] = 0.f;
    n->p[P_PITCH_MIX] = 1.f;      /* whammy-style: full wet once engaged */
    n->p[P_DELAY_TIME] = (300.f - DELAY_MIN_MS) / (DELAY_MAX_MS - DELAY_MIN_MS);
    n->p[P_DELAY_FB] = 30.f / DELAY_FB_MAX;
    n->p[P_DELAY_MIX] = 0.f;
    n->p[P_REVERB_MIX] = 0.f;
    n->p[P_QUALITY] = 0.f;        /* Lite: the full A2 submodel alone is ~90% of a core here */
    /* trigger buttons start at 0 so the first tap (0 -> 1) registers as a flip */
    for (int t : {P_MODEL_PREV, P_MODEL_NEXT, P_CAB_PREV, P_CAB_NEXT, P_MODEL_PAGE_PREV, P_MODEL_PAGE_NEXT,
                  P_T3K_GEAR_PREV, P_T3K_GEAR_NEXT, P_T3K_MAKE_PREV, P_T3K_MAKE_NEXT,
                  P_T3K_PAGE_PREV, P_T3K_PAGE_NEXT, P_T3K_CONFIRM, P_T3K_CANCEL,
                  P_BROWSE_FILTER, P_BROWSE_FAV, P_BROWSE_DELETE}) n->p[t] = 0.f;
}

/* Switch the live model between its lite/full submodel. SetSlimmableSize is thread-safe and resets
 * the incoming submodel before publishing it, so the audio thread never sees a half-ready model. */
static void apply_quality(Nam *n) {
    if (auto *slim = dynamic_cast<nam::SlimmableModel *>(n->dsp.get()))
        slim->SetSlimmableSize(n->p[P_QUALITY] >= 0.5f ? 1.0 : 0.0);
}

static int browse_pages(const Nam *n) {
    int c = (int)n->view.size();
    return c > 0 ? (c + SLOTS_PER_PAGE - 1) / SLOTS_PER_PAGE : 1;
}

/* Ask MPC to re-read our parameter display strings (the list-row labels + page readout). MPC caches
 * parameter text and only refreshes it on this callback (or a full page repaint). */
static void ask_redraw(Nam *n) {
    if (n->master) n->master(&n->fx, audioMasterUpdateDisplay, 0, 0, nullptr, 0.f);
}


/* Change key for a small status/result file: nanosecond mtime mixed with size, so two writes
 * within the same second (e.g. "searching" then "idle") still read as a change. */
static long long file_key(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + ((long long)st.st_size << 40);
}
static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static std::string read_file_to_string(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void write_file_atomic(const char *path, const std::string &data) {
    std::string tmp = std::string(path) + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << data;
    f.close();
    std::rename(tmp.c_str(), path);
}

/* ---- Model library (BROWSE tab): pack folders, favorites, delete, live rescan ---- */
static std::string base_of(const std::string &rel) {
    size_t s = rel.rfind('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}
static std::string folder_of(const std::string &rel) {
    size_t s = rel.rfind('/');
    return s == std::string::npos ? std::string() : rel.substr(0, s);
}
struct CiLess { bool operator()(const std::string &a, const std::string &b) const { return strcasecmp(a.c_str(), b.c_str()) < 0; } };

/* .nam files at the top level plus one level of pack folders; hidden entries skipped. */
static void scan_models(const std::string &dir, std::vector<std::string> &names, std::vector<long long> &mtimes) {
    std::vector<std::pair<std::string, long long>> found;
    std::error_code ec;
    auto add = [&](const std::filesystem::path &f, const std::string &rel) {
        found.push_back({rel, file_key(f.c_str()) & ((1LL << 40) - 1)});
    };
    if (std::filesystem::is_directory(dir, ec))
        for (auto &e : std::filesystem::directory_iterator(dir, ec)) {
            std::string fn = e.path().filename().string();
            if (fn.empty() || fn[0] == '.') continue;
            if (e.is_directory(ec)) {
                std::error_code ec2;
                for (auto &f : std::filesystem::directory_iterator(e.path(), ec2))
                    if (f.path().extension() == ".nam" && f.path().filename().string()[0] != '.')
                        add(f.path(), fn + "/" + f.path().stem().string());
            } else if (e.path().extension() == ".nam") add(e.path(), e.path().stem().string());
        }
    std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return CiLess()(a.first, b.first); });
    names.clear(); mtimes.clear();
    for (auto &f : found) { names.push_back(f.first); mtimes.push_back(f.second); }
}
/* Cheap change detector: a file added/removed/renamed changes its directory's mtime. */
static long long lib_signature(const std::string &dir) {
    long long sig = file_key(dir.c_str());
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
        for (auto &e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_directory(ec)) sig = sig * 31 + file_key(e.path().c_str());
    return sig;
}
static std::string favs_path(const Nam *n) {
    return (std::filesystem::path(n->dir).parent_path() / "favorites.txt").string();
}
static void load_favs(Nam *n) {
    n->favs.clear();
    std::istringstream in(read_file_to_string(favs_path(n).c_str()));
    for (std::string line; std::getline(in, line);) if (!line.empty()) n->favs.insert(line);
    n->favs_key = file_key(favs_path(n).c_str());
}
static void save_favs(Nam *n) {
    std::string s;
    for (auto &f : n->favs) s += f + "\n";
    write_file_atomic(favs_path(n).c_str(), s);
    n->favs_key = file_key(favs_path(n).c_str());
}

/* Rebuild the visible rows for the current filter/folder. Caller holds lib_mtx. */
static void build_view(Nam *n) {
    n->view.clear();
    auto model_row = [&](int k, bool mark) {
        bool fav = n->favs.count(n->names[(size_t)k]) > 0;
        n->view.push_back({0, k, std::string(), (mark && fav ? "* " : "") + base_of(n->names[(size_t)k])});
    };
    if (n->filter == 1) {
        for (int k = 0; k < (int)n->names.size(); k++) if (n->favs.count(n->names[(size_t)k])) model_row(k, false);
    } else if (n->filter == 2) {
        std::vector<int> order((size_t)n->names.size());
        for (size_t k = 0; k < order.size(); k++) order[k] = (int)k;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return n->mtimes[(size_t)a] > n->mtimes[(size_t)b]; });
        for (int k : order) model_row(k, true);
    } else {
        if (!n->folder.empty()) {
            bool any = false;
            for (auto &nm : n->names) if (folder_of(nm) == n->folder) { any = true; break; }
            if (!any) n->folder.clear();   /* pack emptied (deleted/removed externally): back to the top */
        }
        if (n->folder.empty()) {
            std::map<std::string, int, CiLess> packs;
            for (auto &nm : n->names) { std::string f = folder_of(nm); if (!f.empty()) packs[f]++; }
            for (auto &pk : packs)
                n->view.push_back({1, -1, pk.first, "+ " + pk.first + "  (" + std::to_string(pk.second) + ")"});
            for (int k = 0; k < (int)n->names.size(); k++) if (folder_of(n->names[(size_t)k]).empty()) model_row(k, true);
        } else {
            n->view.push_back({2, -1, std::string(), "< All models"});
            for (int k = 0; k < (int)n->names.size(); k++) if (folder_of(n->names[(size_t)k]) == n->folder) model_row(k, true);
        }
    }
    int pages = browse_pages(n);
    if (n->browse_page >= pages) n->browse_page = pages - 1;
    if (n->browse_page < 0) n->browse_page = 0;
}
static int view_row_of_model(const Nam *n, int k) {
    for (size_t r = 0; r < n->view.size(); r++) if (n->view[r].kind == 0 && n->view[r].idx == k) return (int)r;
    return -1;
}
/* After the model changes from outside BROWSE (AMP tab stepper), page the list to show it. */
static void reveal_current(Nam *n) {
    if (n->names.empty()) return;
    if (n->filter == 0) {
        std::string f = folder_of(n->names[(size_t)n->model_idx]);
        if (f != n->folder) { n->folder = f; build_view(n); }
    }
    int r = view_row_of_model(n, n->model_idx);
    if (r >= 0) n->browse_page = r / SLOTS_PER_PAGE;
}
/* Swap in a fresh scan, keeping model_idx on the same model by name. Caller holds lib_mtx. */
static void adopt_scan(Nam *n, std::vector<std::string> &names, std::vector<long long> &mtimes) {
    std::string cur = n->names.empty() ? std::string() : n->names[(size_t)n->model_idx];
    n->names.swap(names);
    n->mtimes.swap(mtimes);
    int k = -1;
    for (size_t i = 0; i < n->names.size(); i++) if (n->names[i] == cur) { k = (int)i; break; }
    if (k >= 0) n->model_idx = k;
    else n->model_idx = n->names.empty() ? 0 : std::min(n->model_idx, (int)n->names.size() - 1);
    build_view(n);
}
static void enumerate(Nam *n) {
    n->dir = models_dir();
    std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
    n->lib_sig = lib_signature(n->dir);
    std::vector<std::string> names; std::vector<long long> mtimes;
    scan_models(n->dir, names, mtimes);
    load_favs(n);
    adopt_scan(n, names, mtimes);
}
/* Poll-thread tick (~1s): rescan when a folder changed (download landed, file removed over ssh),
 * reload favorites another instance wrote, and expire an unconfirmed DELETE. true = redraw. */
static bool lib_poll_once(Nam *n) {
    long long sig = lib_signature(n->dir), fk = file_key(favs_path(n).c_str());
    std::vector<std::string> names; std::vector<long long> mtimes;
    bool rescan = sig != n->lib_sig;
    if (rescan) scan_models(n->dir, names, mtimes);   /* filesystem work outside the lock */
    std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
    bool changed = false;
    if (fk != n->favs_key) { load_favs(n); changed = true; }
    if (rescan) { n->lib_sig = sig; adopt_scan(n, names, mtimes); changed = true; }
    else if (changed) build_view(n);
    if (n->del_armed_ms && now_ms() - n->del_armed_ms > 6000) { n->del_armed_ms = 0; changed = true; }
    return changed;
}
/* DELETE confirmed: move the loaded model to <dir>/../models-removed (recoverable), then load the
 * row that slid into its place so culling a pack is tap DELETE twice, listen, repeat. */
static void delete_current(Nam *n) {
    std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
    if (n->names.empty()) return;
    std::string rel = n->names[(size_t)n->model_idx], base = base_of(rel);
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path src = fs::path(n->dir) / (rel + ".nam");
    fs::path trash = fs::path(n->dir).parent_path() / "models-removed";
    fs::create_directories(trash, ec);
    fs::path dst = trash / (base + ".nam");
    for (int i = 2; fs::exists(dst, ec) && i < 1000; i++) dst = trash / (base + " (" + std::to_string(i) + ").nam");
    fs::rename(src, dst, ec);
    if (ec) { n->browse_msg = "Couldn't remove \"" + base + "\": " + ec.message(); return; }
    std::string f = folder_of(rel);
    if (!f.empty()) fs::remove(fs::path(n->dir) / f, ec);   /* only succeeds once the pack is empty */
    if (n->favs.erase(rel)) save_favs(n);
    int row = view_row_of_model(n, n->model_idx), old_idx = n->model_idx;
    std::vector<std::string> names; std::vector<long long> mtimes;
    n->lib_sig = lib_signature(n->dir);
    scan_models(n->dir, names, mtimes);
    adopt_scan(n, names, mtimes);
    n->browse_msg = "Removed \"" + base + "\" (kept in models-removed)";
    if (n->names.empty()) { std::lock_guard<std::mutex> dl(n->dsp_mtx); n->dsp.reset(); return; }
    int pick = -1;
    if (row < 0) {   /* not on screen: take its neighbour by name, staying in the same pack if possible */
        int k = std::min(old_idx, (int)n->names.size() - 1);
        if (folder_of(n->names[(size_t)k]) == f) pick = k;
        else if (k > 0 && folder_of(n->names[(size_t)k - 1]) == f) pick = k - 1;
        row = 0;
    }
    for (int r = std::min(row, (int)n->view.size() - 1); pick < 0 && r >= 0 && r < (int)n->view.size(); r++)
        if (n->view[(size_t)r].kind == 0) { pick = n->view[(size_t)r].idx; break; }
    for (int r = std::min(row, (int)n->view.size() - 1); pick < 0 && r >= 0; r--)
        if (n->view[(size_t)r].kind == 0) pick = n->view[(size_t)r].idx;
    load_index(n, pick >= 0 ? pick : std::min(n->model_idx, (int)n->names.size() - 1));
    reveal_current(n);
}

/* Status line from the service's status, judged against our latest request. Caller holds t3k_mtx. */
static void t3k_eval_status(Nam *n) {
    const t3k::Status &st = n->t3k_st;
    std::string text;
    if (!st.signed_in) text = "Sign in on your phone: " + st.login_url;
    else if (st.seq != n->t3k_seq)   /* still queued behind another request (e.g. a pack download) */
        text = n->t3k_last_was_download ? "Waiting to download..." : "Searching...";
    else if (st.state == "searching") text = "Searching...";
    else if (st.state == "downloading")
        text = "Downloading " + n->t3k_dl_name + (st.message.empty() ? std::string("...") : " (" + st.message + ")...");
    else if (st.state == "error") text = st.message.empty() ? "Error" : "Error: " + st.message;
    else if (n->t3k_last_was_download)
        text = "Saved " + st.message + (st.message == "1" ? " capture" : " captures") + " of " + n->t3k_dl_name + " - see BROWSE";
    else text = "Signed in";
    n->t3k_status_text = text;
}

/* TONE3000 filter option tables. The *_API rows are the exact query tokens the Tone3000 API expects
 * (gears enum; makes matched exactly against make/model names). Index 0 is the "no filter" choice. */
static const char *T3K_GEARS[]    = {"All", "Amp", "Amp+Cab", "Pedal", "Cab", "Outboard", "Space", "Experimental"};
static const char *T3K_GEAR_API[] = {"",    "amp", "amp-cab", "pedal", "cab", "outboard", "space", "experimental"};
static const char *T3K_MAKES[]    = {"All", "Fender", "Marshall", "Mesa Boogie", "Vox", "Orange", "Peavey",
                                     "Friedman", "Bogner", "Soldano", "EVH", "Dumble", "Ampeg", "Diezel",
                                     "ENGL", "Two-Rock", "Matchless", "Supro", "PRS", "Roland"};
static const int T3K_NGEARS = (int)(sizeof(T3K_GEARS) / sizeof(*T3K_GEARS));
static const int T3K_NMAKES = (int)(sizeof(T3K_MAKES) / sizeof(*T3K_MAKES));

/* Build the API filter set from this instance's UI state. Caller holds t3k_mtx. */
static t3k::Filters t3k_filters(Nam *n) {
    t3k::Filters f;
    if (n->t3k_arch == 0) { f.architecture = 2; f.sizes = "lite-feather-nano"; }  /* MPC-friendly */
    else if (n->t3k_arch == 1) { f.architecture = 2; }                            /* all A2 */
    /* else STD: leave architecture unset -> API default (A1 + custom) */
    if (n->t3k_gear > 0 && n->t3k_gear < T3K_NGEARS) f.gears = T3K_GEAR_API[n->t3k_gear];
    f.calibrated = (n->t3k_quality == 1 || n->t3k_quality == 3);
    f.verified   = (n->t3k_quality == 2 || n->t3k_quality == 3);
    if (n->t3k_make > 0 && n->t3k_make < T3K_NMAKES) f.make = T3K_MAKES[n->t3k_make];
    return f;
}

/* Caller holds t3k_mtx. browse=true: a sort/page change (clears the rows until matching results
 * arrive); browse=false: a download of `download`, leaving the rows as they are. */
static void t3k_write_request_locked(Nam *n, bool browse, long long download) {
    n->t3k_last_was_download = !browse;
    if (browse) {
        n->t3k_seq = n->t3k_browse_seq = t3k::browse(n, n->t3k_sort, n->t3k_page + 1, T3K_ROWS, t3k_filters(n));
        n->t3k_selected = -1;
        n->t3k_have_results = false;
        for (int i = 0; i < T3K_SLOTS_PER_PAGE; i++) { n->t3k_items[i].clear(); n->t3k_item_ids[i] = 0; }
    } else n->t3k_seq = t3k::download(n, download, n->t3k_dl_name, n->dir);
    n->t3k_st = t3k::status();
    t3k_eval_status(n);
}

/* Poll thread body: pick up our results and the service status, redraw on visible change. */
static bool t3k_poll_once(Nam *n) {
    t3k::Status st = t3k::status();
    t3k::Results r;
    long long want;
    { std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx); want = n->t3k_have_results ? 0 : n->t3k_browse_seq; }
    bool got = want && t3k::results(want, r);
    std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
    bool changed = false;
    if (got && r.seq == n->t3k_browse_seq) {
        n->t3k_have_results = true;
        n->t3k_total_pages = r.total_pages < 1 ? 1 : r.total_pages;
        for (int i = 0; i < T3K_SLOTS_PER_PAGE; i++) {
            bool has = i < (int)r.items.size();
            n->t3k_item_ids[i] = has ? r.items[(size_t)i].id : 0;
            n->t3k_items[i] = has ? r.items[(size_t)i].name : std::string();
        }
        changed = true;
    }
    n->t3k_st = st;
    std::string before = n->t3k_status_text;
    t3k_eval_status(n);
    return changed || n->t3k_status_text != before;
}

/* One 100ms poll thread per instance, started lazily the first time the plugin UI asks for any
 * BROWSE/TONE3000 text -- so JUCE's plugin scan never spins a thread. It watches the model folders
 * (~1s) and, once the TONE3000 tab has been shown, the Tone3000 service's answers. */
static void poll_start(Nam *n) {
    if (n->poll_run.exchange(true)) return;
    n->poll_thread = std::thread([n] {
        for (int tick = 0; n->poll_run.load(); tick++) {
            bool redraw = false;
            if (n->t3k_active.load() && t3k_poll_once(n)) redraw = true;
            if (tick % 10 == 0 && lib_poll_once(n)) redraw = true;
            if (redraw) ask_redraw(n);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}
static void poll_stop(Nam *n) {
    if (n->poll_run.exchange(false) && n->poll_thread.joinable()) n->poll_thread.join();
    if (n->t3k_active.exchange(false)) t3k::stop();
}
/* The first search fires only once the TONE3000 tab is actually shown/touched. */
static void t3k_start(Nam *n) {
    poll_start(n);
    if (n->t3k_active.exchange(true)) return;
    t3k::start();
    std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
    t3k_write_request_locked(n, true, -1);
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Nam *n = (Nam *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    v = clamp01(v);
    /* Trigger buttons: MPC's skin Button is a toggle (each tap flips 0<->1, no release event), so
     * any flip of the on/off state counts as one press. */
    bool rising = (i == P_MODEL_PREV || i == P_MODEL_NEXT || i == P_CAB_PREV || i == P_CAB_NEXT ||
                   i == P_MODEL_PAGE_PREV || i == P_MODEL_PAGE_NEXT ||
                   i == P_T3K_GEAR_PREV || i == P_T3K_GEAR_NEXT || i == P_T3K_MAKE_PREV || i == P_T3K_MAKE_NEXT ||
                   i == P_T3K_PAGE_PREV || i == P_T3K_PAGE_NEXT ||
                   i == P_T3K_CONFIRM || i == P_T3K_CANCEL || i == P_BROWSE_FAV || i == P_BROWSE_DELETE) &&
                  (v >= 0.5f) != (n->p[i] >= 0.5f);
    float prev = n->p[i];
    n->p[i] = v;
    /* BROWSE tab. Any action other than a second DELETE disarms a pending delete and clears the
     * last one-shot message, so the detail line always describes what the next tap will do. */
    if ((i >= P_SLOT_1 && i <= P_MODEL_PAGE_NEXT) || (i >= P_BROWSE_FILTER && i <= P_BROWSE_DELETE)) {
        poll_start(n);
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        bool touched = v != prev || rising || (i >= P_SLOT_1 && i <= P_SLOT_8);
        if (touched && i != P_BROWSE_DELETE) { n->del_armed_ms = 0; n->browse_msg.clear(); }
        /* Act on every row tap, not only on a value change: a model row loads it, a pack row opens
         * the pack, "< All models" goes back up. The six row widgets are rebound to different view
         * rows as the list pages, so a tap can resend the value the slot already holds. These
         * indices are excluded from save/restore, so every call here is a real screen tap. */
        if (i >= P_SLOT_1 && i <= P_SLOT_8) {
            int r = n->browse_page * SLOTS_PER_PAGE + (i - P_SLOT_1);
            if (r >= 0 && r < (int)n->view.size()) {
                Nam::Row row = n->view[(size_t)r];
                if (row.kind == 0) { if (row.idx != n->model_idx) load_index(n, row.idx); }
                else if (row.kind == 1) { n->folder = row.key; n->browse_page = 0; build_view(n); }
                else {
                    std::string was = n->folder;
                    n->folder.clear(); build_view(n);
                    for (size_t k = 0; k < n->view.size(); k++)
                        if (n->view[k].kind == 1 && n->view[k].key == was) { n->browse_page = (int)k / SLOTS_PER_PAGE; break; }
                }
            }
            ask_redraw(n);
        } else if (i == P_MODEL_PAGE) {
            int pages = browse_pages(n);
            int pg = pages > 1 ? (int)lroundf(v * (pages - 1)) : 0;
            if (pg != n->browse_page) { n->browse_page = pg; ask_redraw(n); }
        } else if (rising && (i == P_MODEL_PAGE_PREV || i == P_MODEL_PAGE_NEXT)) {
            int pages = browse_pages(n);
            int pg = n->browse_page + (i == P_MODEL_PAGE_NEXT ? 1 : -1);
            n->browse_page = pg < 0 ? 0 : pg >= pages ? pages - 1 : pg;
            ask_redraw(n);
        } else if (i == P_BROWSE_FILTER) {
            int f = (int)lroundf(v * 2.f);
            if (f != n->filter) {
                n->filter = f; n->folder.clear(); n->browse_page = 0;
                build_view(n);
                if (f != 0) { int r = view_row_of_model(n, n->model_idx); if (r >= 0) n->browse_page = r / SLOTS_PER_PAGE; }
            }
            ask_redraw(n);
        } else if (rising && i == P_BROWSE_FAV && !n->names.empty()) {
            const std::string &cur = n->names[(size_t)n->model_idx];
            if (!n->favs.erase(cur)) n->favs.insert(cur);
            save_favs(n);
            build_view(n);
            ask_redraw(n);
        } else if (rising && i == P_BROWSE_DELETE && !n->names.empty()) {
            n->browse_msg.clear();
            if (n->del_armed_ms && now_ms() - n->del_armed_ms <= 6000) { n->del_armed_ms = 0; delete_current(n); }
            else n->del_armed_ms = now_ms();
            ask_redraw(n);
        }
        return;
    }
    /* Tone3000 tab: SORT/PAGE ask the service for another page of results; a result row only
     * selects a tone, and DOWNLOAD fetches it into the models folder (BROWSE picks it up live). */
    if (i >= P_T3K_STATUS && i <= P_T3K_CANCEL) t3k_start(n);
    if (i == P_T3K_SORT) {
        int s = (int)lroundf(v * 3.f);
        std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
        if (s != n->t3k_sort) {
            n->t3k_sort = s; n->t3k_page = 0; n->t3k_total_pages = 1;
            t3k_write_request_locked(n, true, -1); ask_redraw(n);
        }
        return;
    }
    /* Filters: any change resets to page 1 and re-searches. ARCH/QUALITY are enum_h (value*max);
     * GEAR/MAKE are steppers (a direct value plus wrap-around prev/next momentaries). */
    if (i == P_T3K_ARCH || i == P_T3K_QUALITY) {
        int max = i == P_T3K_ARCH ? 2 : 3;
        int s = (int)lroundf(v * max);
        s = s < 0 ? 0 : s > max ? max : s;
        std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
        int &slot = i == P_T3K_ARCH ? n->t3k_arch : n->t3k_quality;
        if (s != slot) { slot = s; n->t3k_page = 0; n->t3k_total_pages = 1; t3k_write_request_locked(n, true, -1); ask_redraw(n); }
        return;
    }
    if (i == P_T3K_GEAR || (rising && (i == P_T3K_GEAR_PREV || i == P_T3K_GEAR_NEXT)) ||
        i == P_T3K_MAKE || (rising && (i == P_T3K_MAKE_PREV || i == P_T3K_MAKE_NEXT))) {
        bool gear = (i == P_T3K_GEAR || i == P_T3K_GEAR_PREV || i == P_T3K_GEAR_NEXT);
        int cnt = gear ? T3K_NGEARS : T3K_NMAKES;
        std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
        int &slot = gear ? n->t3k_gear : n->t3k_make;
        int idx = (i == P_T3K_GEAR || i == P_T3K_MAKE) ? (cnt > 1 ? (int)lroundf(v * (cnt - 1)) : 0)
                  : slot + ((i == P_T3K_GEAR_NEXT || i == P_T3K_MAKE_NEXT) ? 1 : -1);
        idx = (idx % cnt + cnt) % cnt;   /* wrap: filter lists cycle */
        if (idx != slot) { slot = idx; n->t3k_page = 0; n->t3k_total_pages = 1; t3k_write_request_locked(n, true, -1); ask_redraw(n); }
        return;
    }
    /* Select-then-confirm: tapping a row only records which slot is selected (shown via
     * T3K_DETAIL); the actual download request waits for a P_T3K_CONFIRM press below. */
    if (i >= P_T3K_SLOT_1 && i <= P_T3K_SLOT_6) {
        if (v != prev) {
            std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
            int m = i - P_T3K_SLOT_1;
            if (n->t3k_item_ids[m] > 0) n->t3k_selected = m;
            ask_redraw(n);
        }
        return;
    }
    if (i == P_T3K_PAGE || (rising && (i == P_T3K_PAGE_PREV || i == P_T3K_PAGE_NEXT))) {
        std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
        int pages = n->t3k_total_pages < 1 ? 1 : n->t3k_total_pages;
        int pg = i == P_T3K_PAGE ? (pages > 1 ? (int)lroundf(v * (pages - 1)) : 0)
                                 : n->t3k_page + (i == P_T3K_PAGE_NEXT ? 1 : -1);
        pg = pg < 0 ? 0 : pg >= pages ? pages - 1 : pg;
        if (pg != n->t3k_page) {
            n->t3k_page = pg;
            t3k_write_request_locked(n, true, -1); ask_redraw(n);
        }
        return;
    }
    if (rising && i == P_T3K_CONFIRM) {
        std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
        if (n->t3k_selected >= 0 && n->t3k_selected < T3K_SLOTS_PER_PAGE &&
            n->t3k_item_ids[n->t3k_selected] > 0) {
            n->t3k_dl_name = n->t3k_items[n->t3k_selected];
            t3k_write_request_locked(n, false, n->t3k_item_ids[n->t3k_selected]);
            n->t3k_selected = -1;
        }
        ask_redraw(n);
        return;
    }
    if (rising && i == P_T3K_CANCEL) {
        n->t3k_selected = -1;
        ask_redraw(n);
        return;
    }
    if (i == P_QUALITY) { if ((v >= 0.5f) != (prev >= 0.5f)) apply_quality(n); return; }
    if (i == P_MODEL) {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int nopts = (int)n->names.size();
        if (nopts > 1) {
            int idx = (int)lroundf(v * (nopts - 1));
            if (idx != n->model_idx) { load_index(n, idx); reveal_current(n); ask_redraw(n); }
        }
    } else if (i == P_CAB_SELECT) {
        int nopts = (int)n->cab.names.size();
        if (nopts > 1) {
            int idx = (int)lroundf(v * (nopts - 1));
            if (idx != n->cab.idx) n->cab.load(idx);
        }
    } else if (rising && i == P_MODEL_PREV) {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int nopts = (int)n->names.size();
        if (nopts > 0) { load_index(n, (n->model_idx - 1 + nopts) % nopts); reveal_current(n); ask_redraw(n); }
    } else if (rising && i == P_MODEL_NEXT) {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int nopts = (int)n->names.size();
        if (nopts > 0) { load_index(n, (n->model_idx + 1) % nopts); reveal_current(n); ask_redraw(n); }
    } else if (rising && i == P_CAB_PREV) {
        int nopts = (int)n->cab.names.size();
        if (nopts > 0) n->cab.load((n->cab.idx - 1 + nopts) % nopts);
    } else if (rising && i == P_CAB_NEXT) {
        int nopts = (int)n->cab.names.size();
        if (nopts > 0) n->cab.load((n->cab.idx + 1) % nopts);
    }
}

static float getParameter(AEffect *e, int32_t i) {
    Nam *n = (Nam *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.f;
    if (i == P_MODEL) {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int nopts = (int)n->names.size();
        return nopts > 1 ? (float)n->model_idx / (nopts - 1) : 0.f;
    }
    if (i == P_CAB_SELECT) {
        int nopts = (int)n->cab.names.size();
        return nopts > 1 ? (float)n->cab.idx / (nopts - 1) : 0.f;
    }
    if (i >= P_SLOT_1 && i <= P_SLOT_8) {   /* tile highlight: 1 for the row holding the current model */
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int r = n->browse_page * SLOTS_PER_PAGE + (i - P_SLOT_1);
        return (r < (int)n->view.size() && n->view[(size_t)r].kind == 0 && n->view[(size_t)r].idx == n->model_idx) ? 1.f : 0.f;
    }
    if (i == P_MODEL_PAGE) {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        int pages = browse_pages(n);
        return pages > 1 ? (float)n->browse_page / (pages - 1) : 0.f;
    }
    if (i == P_BROWSE_FILTER) return n->filter / 2.f;
    if (i == P_T3K_SORT) return n->t3k_sort / 3.f;
    if (i == P_T3K_ARCH) return n->t3k_arch / 2.f;
    if (i == P_T3K_QUALITY) return n->t3k_quality / 3.f;
    if (i == P_T3K_GEAR) return T3K_NGEARS > 1 ? (float)n->t3k_gear / (T3K_NGEARS - 1) : 0.f;
    if (i == P_T3K_MAKE) return T3K_NMAKES > 1 ? (float)n->t3k_make / (T3K_NMAKES - 1) : 0.f;
    if (i >= P_T3K_SLOT_1 && i <= P_T3K_SLOT_6)   /* tile highlight: 1 for the selected (not yet confirmed) row */
        return (i - P_T3K_SLOT_1 == n->t3k_selected) ? 1.f : 0.f;
    if (i == P_T3K_PAGE) {
        int pages = n->t3k_total_pages < 1 ? 1 : n->t3k_total_pages;
        return pages > 1 ? (float)n->t3k_page / (pages - 1) : 0.f;
    }
    return n->p[i];
}

/* Flush denormals to zero for the audio thread: decaying reverb/IIR/NAM tails otherwise fall into
 * subnormals, which ARM VFP handles far slower -- the classic "CPU spikes when the note dies" bug. */
struct ScopedFlushToZero {   /* restores the host's FPSCR on exit, like JUCE's ScopedNoDenormals */
#if defined(__arm__)
    uint32_t saved;
    ScopedFlushToZero() {
        __asm__ volatile("vmrs %0, fpscr" : "=r"(saved));
        __asm__ volatile("vmsr fpscr, %0" : : "r"(saved | (1u << 24)));
    }
    ~ScopedFlushToZero() { __asm__ volatile("vmsr fpscr, %0" : : "r"(saved)); }
#endif
};

static void processReplacing(AEffect *e, float **in, float **out, int32_t count) {
    Nam *n = (Nam *)e->object;
    ScopedFlushToZero ftz;
    if ((int32_t)n->mbuf.size() < count) n->mbuf.resize((size_t)count);
    float *mbuf = n->mbuf.data();
    const float *l = in[0], *r = in[1];

    float ig = dbToLin(mapRange(n->p[P_INGAIN], GAIN_MIN_DB, GAIN_MAX_DB));
    for (int32_t i = 0; i < count; i++) mbuf[i] = 0.5f * (l[i] + r[i]) * ig;

    n->gate.process(mbuf, count, n->sr,
                     mapRange(n->p[P_GATE_THRESH], GATE_MIN_DB, GATE_MAX_DB),
                     mapRange(n->p[P_GATE_RELEASE], GATE_REL_MIN, GATE_REL_MAX));

    {
        std::unique_lock<std::mutex> lk(n->dsp_mtx, std::try_to_lock);
        if (lk.owns_lock() && n->dsp) {
            int32_t off = 0;
            while (off < count) {
                int m = (int)std::min((int32_t)MAXBLOCK, count - off);
                for (int i = 0; i < m; i++) n->din[(size_t)i] = (NAM_SAMPLE)mbuf[off + i];
                NAM_SAMPLE *ip[1] = {n->din.data()};
                NAM_SAMPLE *op[1] = {n->dout.data()};
                n->dsp->process(ip, op, m);
                for (int i = 0; i < m; i++) mbuf[off + i] = (float)n->dout[(size_t)i];
                off += m;
            }
        } /* else: reloading, or no model -- dry, pass mbuf through unmodeled */
    }

    n->tone.update(mapRange(n->p[P_BASS], TONE_MIN_DB, TONE_MAX_DB),
                    mapRange(n->p[P_MID], TONE_MIN_DB, TONE_MAX_DB),
                    mapRange(n->p[P_TREBLE], TONE_MIN_DB, TONE_MAX_DB), n->sr);
    n->tone.process(mbuf, count);

    if (n->p[P_CAB_ON] >= 0.5f) n->cab.process(mbuf, count);

    bool pitch_on = n->p[P_PITCH_ON] >= 0.5f;
    if (pitch_on) {
        if (!n->pitch_was_on) n->pitch.reset();   /* don't replay audio left from when it was last on */
        n->pitch.process(mbuf, count, n->sr,
                          mapRange(n->p[P_PITCH_SEMI], PITCH_MIN_ST, PITCH_MAX_ST),
                          mapRange(n->p[P_PITCH_MIX], 0.f, 100.f) * 0.01f);
    }
    n->pitch_was_on = pitch_on;

    if (n->p[P_DELAY_MIX] > 0.0005f)
        n->delay.process(mbuf, count, n->sr,
                          mapRange(n->p[P_DELAY_TIME], DELAY_MIN_MS, DELAY_MAX_MS),
                          mapRange(n->p[P_DELAY_FB], 0.f, DELAY_FB_MAX) * 0.01f,
                          mapRange(n->p[P_DELAY_MIX], 0.f, 100.f) * 0.01f);

    if (n->p[P_REVERB_MIX] > 0.0005f)
        n->reverb.process(mbuf, count,
                           mapRange(n->p[P_REVERB_SIZE], 0.f, 100.f) * 0.01f,
                           mapRange(n->p[P_REVERB_MIX], 0.f, 100.f) * 0.01f);

    float og = dbToLin(mapRange(n->p[P_OUTGAIN], GAIN_MIN_DB, GAIN_MAX_DB));
    for (int32_t i = 0; i < count; i++) out[0][i] = out[1][i] = mbuf[i] * og;
}

/* VST2 accumulating process(): adds to the output buffers. */
static void process(AEffect *e, float **in, float **out, int32_t count) {
    std::vector<float> tl((size_t)count), tr((size_t)count);
    float *tmp[2] = {tl.data(), tr.data()};
    processReplacing(e, in, tmp, count);
    for (int32_t i = 0; i < count; i++) { out[0][i] += tl[(size_t)i]; out[1][i] += tr[(size_t)i]; }
}

static void copy_str(void *dst, const char *src, size_t max) {
    strncpy((char *)dst, src, max - 1);
    ((char *)dst)[max - 1] = 0;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Nam *n = (Nam *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        poll_stop(n);
        delete n;
        return 1;
    case effGetPlugCategory: return 1; /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName:
        copy_str(p, (idx >= 0 && idx < NPARAMS) ? PINFO[idx].name : "", 32);
        return 1;
    case effGetParamLabel:
        copy_str(p, (idx >= 0 && idx < NPARAMS) ? PINFO[idx].unit : "", 8);
        return 1;
    case effGetParamDisplay:
        if (idx == P_MODEL) {
            std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
            copy_str(p, n->names.empty() ? "(none)" : base_of(n->names[(size_t)n->model_idx]).c_str(), 64);
        }
        else if (idx == P_CAB_SELECT) copy_str(p, n->cab.names.empty() ? "(none)" : n->cab.names[(size_t)n->cab.idx].c_str(), 24);
        else if (idx == P_CAB_ON || idx == P_PITCH_ON) copy_str(p, n->p[idx] >= 0.5f ? "On" : "Off", 8);
        else if (idx == P_QUALITY) copy_str(p, n->p[idx] >= 0.5f ? "Full" : "Lite", 8);
        else if ((idx >= P_SLOT_1 && idx <= P_MODEL_PAGE_NEXT) || (idx >= P_BROWSE_FILTER && idx <= P_BROWSE_DELETE)) {
            poll_start(n);
            std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
            if (idx >= P_SLOT_1 && idx <= P_SLOT_8) {
                int r = n->browse_page * SLOTS_PER_PAGE + (idx - P_SLOT_1);
                copy_str(p, r < (int)n->view.size() ? n->view[(size_t)r].label.c_str() : "", 64);
            } else if (idx == P_MODEL_PAGE) snprintf((char *)p, 24, "%d/%d", n->browse_page + 1, browse_pages(n));
            else if (idx == P_BROWSE_FILTER) {
                static const char *filters[3] = {"All", "Favorites", "Recent"};
                copy_str(p, filters[n->filter < 0 || n->filter > 2 ? 0 : n->filter], 24);
            } else if (idx == P_BROWSE_DETAIL) {
                std::string t;
                std::string cur = n->names.empty() ? std::string() : n->names[(size_t)n->model_idx];
                if (n->names.empty()) t = "No models yet - get some on the TONE3000 tab";
                else if (n->del_armed_ms) t = "Tap DELETE again to remove \"" + base_of(cur) + "\"";
                else if (!n->browse_msg.empty()) t = n->browse_msg;
                else if (n->filter == 1 && n->view.empty()) t = "No favorites yet - load a model, tap FAVORITE";
                else t = (n->favs.count(cur) ? "* " : "") + base_of(cur) +
                         (folder_of(cur).empty() ? std::string() : "  -  " + folder_of(cur));
                copy_str(p, t.c_str(), 96);
            } else copy_str(p, "", 8);
        }
        else if (idx >= P_T3K_STATUS && idx <= P_T3K_CANCEL) {
            /* Tone names and status lines use up to 96 bytes: JUCE's host passes effGetParamDisplay
             * a 256-byte buffer, and the usual 24 would cut names mid-word. */
            t3k_start(n);
            std::lock_guard<std::recursive_mutex> lk(n->t3k_mtx);
            if (idx == P_T3K_STATUS) copy_str(p, n->t3k_status_text.c_str(), 96);
            else if (idx == P_T3K_SORT) {
                static const char *sorts[4] = {"Trending", "Newest", "Downloads", "Favorites"};
                copy_str(p, sorts[n->t3k_sort < 0 || n->t3k_sort > 3 ? 0 : n->t3k_sort], 24);
            } else if (idx == P_T3K_ARCH) {
                static const char *arch[3] = {"MPC", "A2", "Std"};
                copy_str(p, arch[n->t3k_arch < 0 || n->t3k_arch > 2 ? 0 : n->t3k_arch], 24);
            } else if (idx == P_T3K_QUALITY) {
                static const char *q[4] = {"Any", "Calibrated", "Verified", "Cal+Ver"};
                copy_str(p, q[n->t3k_quality < 0 || n->t3k_quality > 3 ? 0 : n->t3k_quality], 24);
            } else if (idx == P_T3K_GEAR)   /* self-labelled: the steppers carry no caption (layout.conf) */
                snprintf((char *)p, 24, "Gear: %s", T3K_GEARS[n->t3k_gear < 0 || n->t3k_gear >= T3K_NGEARS ? 0 : n->t3k_gear]);
            else if (idx == P_T3K_MAKE)
                snprintf((char *)p, 24, "Make: %s", T3K_MAKES[n->t3k_make < 0 || n->t3k_make >= T3K_NMAKES ? 0 : n->t3k_make]);
            else if (idx >= P_T3K_SLOT_1 && idx <= P_T3K_SLOT_6)
                copy_str(p, n->t3k_items[idx - P_T3K_SLOT_1].c_str(), 64);
            else if (idx == P_T3K_PAGE)
                snprintf((char *)p, 24, "%d/%d", n->t3k_page + 1, n->t3k_total_pages < 1 ? 1 : n->t3k_total_pages);
            else if (idx == P_T3K_DETAIL) {
                if (n->t3k_selected >= 0 && n->t3k_selected < T3K_SLOTS_PER_PAGE &&
                    !n->t3k_items[n->t3k_selected].empty())
                    copy_str(p, ("Download \"" + n->t3k_items[n->t3k_selected] + "\"?").c_str(), 64);
                else copy_str(p, "Tap a result, then DOWNLOAD", 64);
            } else copy_str(p, "", 8);
        }
        else if (idx >= 0 && idx < NPARAMS) snprintf((char *)p, 24, "%.1f", mapRange(n->p[idx], PINFO[idx].lo, PINFO[idx].hi));
        return 1;
    case effSetSampleRate:
        n->sr = o > 0 ? o : 44100.0;
        load_index(n, n->model_idx);              /* re-Reset the model at the new rate */
        n->tone.lastBass = n->tone.lastMid = n->tone.lastTreb = 1e9f;   /* force coeff recompute */
        n->delay.buf.clear();                      /* resized lazily at the new rate */
        return 1;
    case effSetBlockSize:
    case effMainsChanged: return 1;
    case effCanDo:
        return !strcmp((char *)p, "receiveVstTimeInfo") ? 1 : -1;
    case effGetChunk: {
        std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
        std::string s;
        s += "model=" + (n->names.empty() ? std::string() : n->names[(size_t)n->model_idx]) + "\n";
        s += "cab=" + (n->cab.names.empty() ? std::string() : n->cab.names[(size_t)n->cab.idx]) + "\n";
        for (int i = 0; i < NPARAMS; i++) {
            if (i == P_MODEL || i == P_CAB_SELECT) continue;   /* saved by name above */
            if (i == P_MODEL_PREV || i == P_MODEL_NEXT || i == P_CAB_PREV || i == P_CAB_NEXT)
                continue;   /* momentary triggers, no state to save */
            if (i >= P_SLOT_1 && i <= P_MODEL_PAGE_NEXT) continue;   /* browser slots/page: ephemeral view state */
            if (i >= P_T3K_STATUS && i <= P_BROWSE_DELETE) continue;  /* Tone3000/browse: ephemeral view state */
            s += std::to_string(i) + "=" + std::to_string(n->p[i]) + "\n";
        }
        size_t len = std::min(s.size(), sizeof(n->chunk) - 1);
        memcpy(n->chunk, s.data(), len);
        n->chunk[len] = 0;
        *(void **)p = n->chunk;
        return (intptr_t)len + 1;
    }
    case effSetChunk: {
        if (v <= 0) return 0;
        std::string s((const char *)p, std::min((size_t)v, sizeof(n->chunk) - 1));
        if (s.find('=') == std::string::npos) {   /* bare model name */
            for (size_t k = 0; k < n->names.size(); k++)
                if (n->names[k] == s.substr(0, s.find('\0'))) { load_index(n, (int)k); break; }
            return 1;
        }
        size_t pos = 0;
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq), val = line.substr(eq + 1);
                if (key == "model") {
                    /* exact relative path, else by file name: a project saved before the model was
                     * moved into (or out of) a pack folder still finds it */
                    std::lock_guard<std::recursive_mutex> lk(n->lib_mtx);
                    int hit = -1;
                    for (size_t k = 0; k < n->names.size() && hit < 0; k++) if (n->names[k] == val) hit = (int)k;
                    for (size_t k = 0; k < n->names.size() && hit < 0; k++) if (base_of(n->names[k]) == base_of(val)) hit = (int)k;
                    if (hit >= 0) { load_index(n, hit); reveal_current(n); }
                } else if (key == "cab") {
                    for (size_t k = 0; k < n->cab.names.size(); k++) if (n->cab.names[k] == val) { n->cab.load((int)k); break; }
                } else {
                    int pidx = atoi(key.c_str());
                    if (pidx >= 0 && pidx < NPARAMS) n->p[pidx] = clamp01((float)atof(val.c_str()));
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        apply_quality(n);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    nam::activations::Activation::enable_fast_tanh();   /* A1 tanh models; A2 LeakyReLU unaffected */
    Nam *n = new Nam();
    n->din.resize(MAXBLOCK);
    n->dout.resize(MAXBLOCK);
    set_defaults(n);
    enumerate(n);
    load_index(n, 0);
    n->cab.enumerate();
    n->cab.load(0);
    n->reverb.init();
    n->master = master;
    AEffect *e = &n->fx;
    memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->process = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = n;
    e->user = (void *)master;
    return e;
}
