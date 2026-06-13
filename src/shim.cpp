// shim.cpp
// -----------------------------------------------------------------------------
// Airwindows AutoGain shim. One compiled DLL, reused for every wrapped plugin.
//
// At load it finds its own filename, reads "<self>.cfg", loads the real
// Airwindows plugin named there, and returns an AEffect that proxies the inner
// plugin completely while inserting a loudness-matching gain stage.
//
// Build: see CMakeLists.txt. No Steinberg SDK required (see vst2_min.h).
// -----------------------------------------------------------------------------
#include "vst2_min.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  using LibHandle = HMODULE;
#else
  #include <dlfcn.h>
  using LibHandle = void*;
#endif

// ---- number of parameters the shim appends after the inner plugin's own -----
static constexpr int K_EXTRA = 3;
enum { P_MODE = 0, P_SPEED = 1, P_TRIM = 2 };   // offsets within the extra block

// ---- ITU-R BS.1770 K-weighting: two biquads applied before measuring energy,
//      so "Match" targets perceived loudness (LUFS) rather than flat RMS. For a
//      pure gain change dry and wet share a spectrum, so it still lands on 0 dB;
//      for a saturator/EQ it accounts for the harmonics a loudness meter hears.
struct Biq   { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
struct BiqSt { double z1 = 0, z2 = 0; };
static inline double biqProcess(const Biq& c, BiqSt& s, double x) {
    double y = c.b0 * x + s.z1;                 // transposed direct form II
    s.z1 = c.b1 * x - c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}
static constexpr int    KW_CH = 8;              // channels we K-weight for metering
static constexpr double KW_PI = 3.14159265358979323846;

// ============================ per-instance state =============================
struct Wrap {
    AEffect    outer{};            // what we hand the host
    AEffect*   inner = nullptr;    // the real Airwindows plugin
    LibHandle  lib   = nullptr;
    AMCallback hostCb = nullptr;   // the real host callback

    // appended parameters (all stored 0..1, as VST expects)
    float pMode  = 1.0f;   // <0.5 = Off (unity), >=0.5 = Match. Default: Match on.
    float pSpeed = 0.5f;   // integration time, mapped to a one-pole coefficient
    float pTrim  = 0.5f;   // 0.5 = 0 dB, range +/- 12 dB

    // gain-matching running state
    double srate    = 44100.0;
    double smDry    = 0.0;     // smoothed K-weighted mean-square of dry input
    double smWet    = 0.0;     // smoothed K-weighted mean-square of wet output
    double curGain  = 1.0;     // per-sample-smoothed applied gain
    bool   primed   = false;
    int    dbgAccum = 0;       // sample counter for throttled debug logging

    // K-weighting filters (coefficients computed once the sample rate is known)
    Biq    k1{}, k2{};
    bool   kReady = false;
    BiqSt  dryK[KW_CH][2]{};
    BiqSt  wetK[KW_CH][2]{};

    // buffer kept alive between effGetChunk and the host reading it
    std::vector<unsigned char> chunkBuf;

    // scratch for synthesized parameter strings
    char strBuf[64]{};
    std::string effectName;      // e.g. "ToTape6 AG"
};

// inner-AEffect -> Wrap, so the audioMaster trampoline can translate pointers.
static std::map<AEffect*, Wrap*> g_map;
static std::mutex                g_mapMx;
// Construction is serialized by g_ctorMx; g_constructing lets the audioMaster
// trampoline resolve callbacks the inner plugin makes from inside its own
// constructor (before we know its AEffect pointer). This used to be a
// thread_local, but file-scope thread_local puts a .tls section in every copy
// of the shim DLL, and Windows runs out of static-TLS slots once a host has
// loaded a few hundred modules during a scan (LoadLibrary then fails with
// error 126). A plain global guarded by a mutex avoids that entirely.
static std::mutex                g_ctorMx;
static Wrap*                     g_constructing = nullptr;   // guarded by g_ctorMx

static Wrap* findWrap(AEffect* inner) {
    std::lock_guard<std::mutex> lk(g_mapMx);
    auto it = g_map.find(inner);
    if (it != g_map.end()) return it->second;
    return g_constructing;   // only set during construction, on this same thread
}

// ============================ small helpers ==================================
static void diaglog(const std::string& m);   // defined later; file-based log
static bool awDebug() { static bool v = std::getenv("AWDEBUG") != nullptr; return v; }

static inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
static inline double linToDb(double lin){ return 20.0 * std::log10(lin); }

// speed 0..1 -> one-pole smoothing coefficient for a ~50 ms .. 5 s window
static double speedCoef(double speed01, double srate) {
    double tau = 0.050 * std::pow(100.0, clampd(speed01, 0.0, 1.0)); // 0.05..5 s
    return std::exp(-1.0 / (tau * srate));
}
static inline double trimDb(float p)  { return (clampd(p, 0.0, 1.0) - 0.5) * 24.0; } // +/-12 dB

// =============================== DSP =========================================
// Compute the BS.1770 K-weighting coefficients for the current sample rate.
static void computeK(Wrap* w) {
    double fs = w->srate > 0 ? w->srate : 48000.0;
    // Stage 1: high-shelf "head" filter.
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K  = std::tan(KW_PI * f0 / fs);
    double Vh = std::pow(10.0, G / 20.0);
    double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    w->k1.b0 = (Vh + Vb * K / Q + K * K) / a0;
    w->k1.b1 = 2.0 * (K * K - Vh) / a0;
    w->k1.b2 = (Vh - Vb * K / Q + K * K) / a0;
    w->k1.a1 = 2.0 * (K * K - 1.0) / a0;
    w->k1.a2 = (1.0 - K / Q + K * K) / a0;
    // Stage 2: RLB high-pass (~38 Hz).
    const double f2 = 38.13547087602444, Q2 = 0.5003270373238773;
    double K2  = std::tan(KW_PI * f2 / fs);
    double a02 = 1.0 + K2 / Q2 + K2 * K2;
    w->k2.b0 = 1.0; w->k2.b1 = -2.0; w->k2.b2 = 1.0;
    w->k2.a1 = 2.0 * (K2 * K2 - 1.0) / a02;
    w->k2.a2 = (1.0 - K2 / Q2 + K2 * K2) / a02;
    for (int c = 0; c < KW_CH; ++c) { w->dryK[c][0] = {}; w->dryK[c][1] = {}; w->wetK[c][0] = {}; w->wetK[c][1] = {}; }
    w->kReady = true;
}

// Generic over float/double sample type so both process paths share one body.
template <typename T>
static void processGeneric(Wrap* w, T** in, T** out, int32_t n) {
    int nin  = w->inner->numInputs;
    int nout = w->inner->numOutputs;
    if (!w->kReady) computeK(w);

    int mdry = nin  < KW_CH ? (nin  > 0 ? nin  : 1) : KW_CH;
    int mwet = nout < KW_CH ? (nout > 0 ? nout : 1) : KW_CH;

    // 1) K-weighted dry energy, measured from the host input BEFORE the inner
    //    plugin runs (inner may process in place).
    double dry = 0.0;
    for (int c = 0; c < mdry; ++c)
        for (int i = 0; i < n; ++i) {
            double y = biqProcess(w->k2, w->dryK[c][1], biqProcess(w->k1, w->dryK[c][0], (double)in[c][i]));
            dry += y * y;
        }

    // 2) run the real plugin
    if (sizeof(T) == sizeof(double) && w->inner->processDoubleReplacing)
        w->inner->processDoubleReplacing(w->inner, (double**)in, (double**)out, n);
    else
        w->inner->processReplacing(w->inner, (float**)in, (float**)out, n);

    // 3) K-weighted wet energy from the output
    double wet = 0.0;
    for (int c = 0; c < mwet; ++c)
        for (int i = 0; i < n; ++i) {
            double y = biqProcess(w->k2, w->wetK[c][1], biqProcess(w->k1, w->wetK[c][0], (double)out[c][i]));
            wet += y * y;
        }

    double dryMS = dry / (double)(n * mdry);
    double wetMS = wet / (double)(n * mwet);

    // 4) integrate. Only update when there is real signal, so silence does not
    //    drive the estimate to garbage.
    const double FLOOR = 1e-9; // ~ -90 dBFS
    double a = speedCoef(w->pSpeed, w->srate);
    if (!w->primed) { w->smDry = dryMS; w->smWet = wetMS; w->primed = true; }
    if (dryMS > FLOOR && wetMS > FLOOR) {
        w->smDry = a * w->smDry + (1.0 - a) * dryMS;
        w->smWet = a * w->smWet + (1.0 - a) * wetMS;
    }

    // 5) target gain
    bool match = w->pMode >= 0.5f;
    double matchLin = 1.0;
    if (match && w->smWet > FLOOR) {
        matchLin = std::sqrt(w->smDry / w->smWet);
        matchLin = clampd(matchLin, dbToLin(-24.0), dbToLin(24.0)); // sanity clamp
    }
    double target = matchLin * dbToLin(trimDb(w->pTrim));

    // optional: once-per-second readout of what the gain stage is doing
    if (awDebug()) {
        w->dbgAccum += n;
        if (w->dbgAccum >= (int)w->srate) {
            w->dbgAccum = 0;
            char b[200];
            std::snprintf(b, sizeof b,
                "[gain] %s mode=%s dryLK=%.1fdB wetLK=%.1fdB match=%+.1fdB trim=%+.1fdB applied=%+.1fdB",
                w->effectName.c_str(), match ? "Match" : "Off",
                10.0 * std::log10(w->smDry + 1e-30),
                10.0 * std::log10(w->smWet + 1e-30),
                linToDb(matchLin), trimDb(w->pTrim),
                linToDb(w->curGain + 1e-30));
            diaglog(b);
        }
    }

    // 6) per-sample ramp to the target (avoids zipper noise on the applied gain)
    double g = w->curGain;
    double ramp = std::exp(-1.0 / (0.010 * w->srate)); // ~10 ms gain glide
    for (int i = 0; i < n; ++i) {
        g = ramp * g + (1.0 - ramp) * target;
        for (int c = 0; c < nout; ++c) out[c][i] = (T)((double)out[c][i] * g);
    }
    w->curGain = g;
}

static void procRepl  (AEffect* e, float**  in, float**  out, int32_t n) {
    processGeneric<float >(static_cast<Wrap*>(e->user), in, out, n);
}
static void procDouble(AEffect* e, double** in, double** out, int32_t n) {
    processGeneric<double>(static_cast<Wrap*>(e->user), in, out, n);
}

// =========================== parameter proxying ==============================
static void setParam(AEffect* e, int32_t idx, float v) {
    Wrap* w = static_cast<Wrap*>(e->user);
    int innerN = w->inner->numParams;
    if (idx < innerN) { w->inner->setParameter(w->inner, idx, v); return; }
    switch (idx - innerN) {
        case P_MODE:  w->pMode  = v; break;
        case P_SPEED: w->pSpeed = v; break;
        case P_TRIM:  w->pTrim  = v; break;
    }
}
static float getParam(AEffect* e, int32_t idx) {
    Wrap* w = static_cast<Wrap*>(e->user);
    int innerN = w->inner->numParams;
    if (idx < innerN) return w->inner->getParameter(w->inner, idx);
    switch (idx - innerN) {
        case P_MODE:  return w->pMode;
        case P_SPEED: return w->pSpeed;
        case P_TRIM:  return w->pTrim;
    }
    return 0.0f;
}

// ============================== dispatcher ===================================
static void copyStr(void* ptr, const char* s) {
    if (ptr) { std::strncpy((char*)ptr, s, 31); ((char*)ptr)[31] = 0; }
}

static VstIntPtr dispatch(AEffect* e, int32_t op, int32_t idx, VstIntPtr val, void* ptr, float opt) {
    Wrap* w = static_cast<Wrap*>(e->user);
    AEffect* in = w->inner;
    int innerN = in->numParams;
    int extra  = idx - innerN;            // >=0 means one of our appended params

    switch (op) {
    case effSetSampleRate:
        w->srate = (double)opt;
        w->kReady = false;                    // recompute K-weighting at new rate
        return in->dispatcher(in, op, idx, val, ptr, opt);

    // ---- queries about OUR appended parameters --------------------------------
    case effGetParamName:
        if (extra >= 0) {
            const char* n[] = { "AG Mode", "AG Speed", "AG Trim" };
            copyStr(ptr, n[extra]); return 1;
        }
        break;
    case effGetParamLabel:
        if (extra >= 0) {
            const char* l[] = { "", "", "dB" };
            copyStr(ptr, l[extra]); return 1;
        }
        break;
    case effGetParamDisplay:
        if (extra >= 0) {
            char buf[32];
            if (extra == P_MODE)  std::snprintf(buf, sizeof buf, "%s", w->pMode >= 0.5f ? "Match" : "Off");
            else if (extra == P_SPEED) {
                double tau = 0.050 * std::pow(100.0, clampd(w->pSpeed,0.0,1.0));
                std::snprintf(buf, sizeof buf, "%.2f s", tau);
            } else std::snprintf(buf, sizeof buf, "%+.1f", trimDb(w->pTrim));
            copyStr(ptr, buf); return 1;
        }
        break;
    case effCanBeAutomated:
        if (extra >= 0) return 1;
        break;
    case effGetParameterProperties:
        if (extra >= 0) return 0; // let host use defaults for our params
        break;

    // ---- rename so the wrapper is identifiable in the host --------------------
    case effGetEffectName:
    case effGetProductString:
        copyStr(ptr, w->effectName.c_str());
        return 1;

    // ---- chunk wrapping: persist OUR params alongside the inner chunk ----------
    case effGetChunk: {
        if (!(in->flags & effFlagsProgramChunks))
            return in->dispatcher(in, op, idx, val, ptr, opt);
        void* innerData = nullptr;
        VstIntPtr innerLen = in->dispatcher(in, effGetChunk, idx, val, &innerData, opt);
        const char tag[4] = {'A','G','W','1'};
        float p[K_EXTRA] = { w->pMode, w->pSpeed, w->pTrim };
        w->chunkBuf.clear();
        auto push = [&](const void* src, size_t n){
            const unsigned char* b = (const unsigned char*)src;
            w->chunkBuf.insert(w->chunkBuf.end(), b, b + n);
        };
        int32_t il = (int32_t)innerLen;
        push(tag, 4); push(&il, 4); push(p, sizeof p);
        if (innerData && innerLen > 0) push(innerData, (size_t)innerLen);
        if (ptr) *(void**)ptr = w->chunkBuf.data();
        return (VstIntPtr)w->chunkBuf.size();
    }
    case effSetChunk: {
        if (!(in->flags & effFlagsProgramChunks))
            return in->dispatcher(in, op, idx, val, ptr, opt);
        unsigned char* b = (unsigned char*)ptr;
        VstIntPtr len = val;
        if (b && len >= 12 && std::memcmp(b, "AGW1", 4) == 0) {
            int32_t il; std::memcpy(&il, b + 4, 4);
            float p[K_EXTRA]; std::memcpy(p, b + 8, sizeof p);
            w->pMode = p[P_MODE]; w->pSpeed = p[P_SPEED]; w->pTrim = p[P_TRIM];
            return in->dispatcher(in, effSetChunk, idx, (VstIntPtr)il, b + 8 + sizeof p, opt);
        }
        // legacy / unwrapped chunk: hand it straight to the inner plugin
        return in->dispatcher(in, op, idx, val, ptr, opt);
    }

    case effClose: {
        VstIntPtr r = in->dispatcher(in, effClose, idx, val, ptr, opt);
        { std::lock_guard<std::mutex> lk(g_mapMx); g_map.erase(in); }
        #if defined(_WIN32)
            if (w->lib) FreeLibrary(w->lib);
        #else
            if (w->lib) dlclose(w->lib);
        #endif
        delete w;
        return r;
    }
    default: break;
    }

    // everything else: straight through to the real plugin
    return in->dispatcher(in, op, idx, val, ptr, opt);
}

// ===================== audioMaster pointer translation =======================
// The inner plugin calls back with ITS pointer; the host only knows OURS, so we
// swap it before forwarding (matters for automation / beginEdit / getTime).
static VstIntPtr amTrampoline(AEffect* inner, int32_t op, int32_t idx,
                              VstIntPtr val, void* ptr, float opt) {
    Wrap* w = findWrap(inner);
    if (w && w->hostCb) return w->hostCb(&w->outer, op, idx, val, ptr, opt);
    return 0;
}

// =============================== config ======================================
struct Cfg { std::string target, name; int32_t id = 0; };

static std::string selfDir;   // directory of this shim DLL (with trailing sep)

static bool readCfg(const std::string& path, Cfg& c) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        while (!v.empty() && (v.back()=='\n'||v.back()=='\r'||v.back()==' ')) v.pop_back();
        if      (k == "target") c.target = v;
        else if (k == "name")   c.name = v;
        else if (k == "id")     c.id = (int32_t)std::strtoul(v.c_str(), nullptr, 0);
    }
    std::fclose(f);
    return !c.target.empty();
}

static LibHandle loadLib(const std::string& p) {
#if defined(_WIN32)
    return LoadLibraryA(p.c_str());
#else
    return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* sym(LibHandle h, const char* n) {
#if defined(_WIN32)
    return (void*)GetProcAddress(h, n);
#else
    return dlsym(h, n);
#endif
}

// Always-on diagnostic log, written next to the OS temp dir, so failures are
// visible even when the host runs the plugin in a sandboxed child process.
static void diaglog(const std::string& m) {
#if defined(_WIN32)
    char tmp[MAX_PATH]{}; DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string p = (n ? std::string(tmp) : std::string()) + "awautogain.log";
#else
    std::string p = "/tmp/awautogain.log";
#endif
    FILE* f = std::fopen(p.c_str(), "ab");
    if (f) { std::fprintf(f, "%s\n", m.c_str()); std::fclose(f); }
}

// ============================= entry point ===================================
static AEffect* createShim(AMCallback hostCb) {
    bool dbg = std::getenv("AWDEBUG") != nullptr;
    auto log = [&](const char* m){ if (dbg) std::fprintf(stderr, "[awautogain] %s\n", m); };
#if !defined(_WIN32)
    if (selfDir.empty()) {
        Dl_info info;
        if (dladdr((void*)&createShim, &info) && info.dli_fname) selfDir = info.dli_fname;
    }
#endif
    // locate "<self without extension>.cfg"
    std::string base = selfDir;            // full module path, set above / in DllMain
    std::string cfgPath;
    // selfDir actually holds the full module path here; split it.
    size_t slash = base.find_last_of("/\\");
    std::string dir  = (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
    std::string file = (slash == std::string::npos) ? base : base.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? file : file.substr(0, dot);
    cfgPath = dir + stem + ".cfg";

    Cfg cfg;
    diaglog("---- load attempt ----");
    diaglog("self = " + selfDir);
    diaglog("cfg  = " + cfgPath);
    if (dbg) std::fprintf(stderr, "[awautogain] self='%s' cfg='%s'\n", selfDir.c_str(), cfgPath.c_str());
    if (!readCfg(cfgPath, cfg)) { diaglog("FAIL: cannot read cfg (missing or no target= line)"); log("cfg read failed"); return nullptr; }

    // target may be absolute or relative to the shim's folder
    std::string target = cfg.target;
    bool absolute =
        (!target.empty() && (target[0] == '/' || target[0] == '\\')) ||
        (target.size() > 1 && target[1] == ':');
    if (!absolute) target = dir + target;
    diaglog("target = " + target);

    Wrap* w = new Wrap();
    w->hostCb = hostCb;
    w->effectName = cfg.name.empty() ? stem : cfg.name;

    w->lib = loadLib(target);
    if (!w->lib) {
#if defined(_WIN32)
        diaglog("FAIL: LoadLibrary error " + std::to_string((long)GetLastError())
                + " (193=wrong bitness, 126=missing dependency, 2/3=file not found)");
#else
        diaglog("FAIL: dlopen could not load target");
#endif
        if (dbg) std::fprintf(stderr, "[awautogain] loadLib failed: '%s'\n", target.c_str());
        delete w; return nullptr;
    }

    auto vmain = (VSTPluginMainProc)sym(w->lib, "VSTPluginMain");
    if (!vmain) vmain = (VSTPluginMainProc)sym(w->lib, "main");
    if (!vmain) { diaglog("FAIL: inner DLL has no VSTPluginMain/main export"); log("no VSTPluginMain export"); delete w; return nullptr; }

    g_ctorMx.lock();                          // serialize use of g_constructing
    g_constructing = w;                       // inner ctor may call back now
    w->inner = vmain(amTrampoline);
    g_constructing = nullptr;
    g_ctorMx.unlock();
    if (!w->inner || w->inner->magic != kEffectMagic) { diaglog("FAIL: inner returned null or bad magic"); log("inner null or bad magic"); delete w; return nullptr; }
    diaglog("OK: inner loaded, numParams=" + std::to_string(w->inner->numParams));

    { std::lock_guard<std::mutex> lk(g_mapMx); g_map[w->inner] = w; }

    // Build the outer AEffect by copying the inner one, then overriding.
    AEffect& o = w->outer;
    o = *w->inner;                            // copies flags, io counts, etc.
    o.user                  = w;
    o.object                = nullptr;
    o.dispatcher            = dispatch;
    o.setParameter          = setParam;
    o.getParameter          = getParam;
    o.processReplacing      = procRepl;
    o.processDoubleReplacing= w->inner->processDoubleReplacing ? procDouble : nullptr;
    o.process               = nullptr;        // deprecated path unused
    o.numParams             = w->inner->numParams + K_EXTRA;
    o.uniqueID              = cfg.id ? cfg.id : (w->inner->uniqueID ^ 0x41470000); // 'AG'
    return &o;
}

// platform glue: capture our own module path, export VSTPluginMain
#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);         // we have no per-thread DLL work
        char buf[MAX_PATH]{};
        GetModuleFileNameA(h, buf, MAX_PATH);
        selfDir = buf;
    }
    return TRUE;
}
extern "C" __declspec(dllexport) AEffect* VSTPluginMain(AMCallback cb) { return createShim(cb); }
extern "C" __declspec(dllexport) AEffect* MAIN(AMCallback cb)          { return createShim(cb); }
#else
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(AMCallback cb) { return createShim(cb); }
extern "C" __attribute__((visibility("default"))) AEffect* main_plugin(AMCallback cb)   { return createShim(cb); }
#endif
