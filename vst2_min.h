// vst2_min.h
// -----------------------------------------------------------------------------
// Minimal, clean-room declaration of the VST 2.4 host/plugin ABI.
//
// This intentionally re-declares ONLY the struct layout, magic number, opcodes
// and flags that the AutoGain shim actually touches, derived from the public
// binary interface (the same approach taken by long-standing open-source
// "vestige" headers used by Ardour, LMMS, etc.). It contains no Steinberg SDK
// source. The struct field ORDER and sizes must match the real ABI exactly,
// because the host indexes named fields of the AEffect we hand back.
// -----------------------------------------------------------------------------
#pragma once
#include <cstdint>

using VstIntPtr = intptr_t;

struct AEffect;

using AMCallback        = VstIntPtr (*)(AEffect*, int32_t op, int32_t idx, VstIntPtr val, void* ptr, float opt);
using DispatcherProc    = VstIntPtr (*)(AEffect*, int32_t op, int32_t idx, VstIntPtr val, void* ptr, float opt);
using ProcessProc       = void (*)(AEffect*, float**  in, float**  out, int32_t frames);
using ProcessDoubleProc = void (*)(AEffect*, double** in, double** out, int32_t frames);
using SetParamProc      = void  (*)(AEffect*, int32_t idx, float v);
using GetParamProc      = float (*)(AEffect*, int32_t idx);

struct AEffect {
    int32_t           magic;            // kEffectMagic
    DispatcherProc    dispatcher;
    ProcessProc       process;          // deprecated (accumulating)
    SetParamProc      setParameter;
    GetParamProc      getParameter;
    int32_t           numPrograms;
    int32_t           numParams;
    int32_t           numInputs;
    int32_t           numOutputs;
    int32_t           flags;
    VstIntPtr         resvd1;
    VstIntPtr         resvd2;
    int32_t           initialDelay;
    int32_t           realQualities;    // deprecated
    int32_t           offQualities;     // deprecated
    float             ioRatio;          // deprecated
    void*             object;
    void*             user;
    int32_t           uniqueID;
    int32_t           version;
    ProcessProc       processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char              future[56];
};

// Entry point exported by every VST2 plugin DLL.
using VSTPluginMainProc = AEffect* (*)(AMCallback host);

static constexpr int32_t kEffectMagic = 0x56737450; // 'VstP'

// Plugin flags we test / copy.
enum {
    effFlagsProgramChunks      = 1 << 5,   // state is a chunk, not individual params
    effFlagsCanReplacing       = 1 << 4,
    effFlagsCanDoubleReplacing = 1 << 12,
};

// Dispatcher opcodes the shim special-cases. Everything else is forwarded.
enum {
    effOpen                 = 0,
    effClose                = 1,
    effSetProgram           = 2,
    effGetProgram           = 3,
    effGetParamLabel        = 6,
    effGetParamDisplay      = 7,
    effGetParamName         = 8,
    effSetSampleRate        = 10,
    effSetBlockSize         = 11,
    effMainsChanged         = 12,
    effGetChunk             = 23,
    effSetChunk             = 24,
    effCanBeAutomated       = 26,
    effString2Parameter     = 27,
    effGetEffectName        = 45,
    effGetProductString     = 48,
    effGetParameterProperties = 56,
    effCanDo                = 51,
    effSetProcessPrecision  = 77,
};
