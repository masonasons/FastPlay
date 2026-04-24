#ifdef USE_STEAM_AUDIO

#include "spatial_audio.h"
#include "effects.h"
#include <phonon_version.h>
#include <windows.h>
#include <cstring>
#include <cmath>

static constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

// Capture Steam Audio log messages for error reporting
static char g_phononLog[1024] = {};
static void IPLCALL PhononLogCallback(IPLLogLevel level, const char* message) {
    if (level == IPL_LOGLEVEL_ERROR || level == IPL_LOGLEVEL_WARNING) {
        // Keep the last error/warning message
        strncpy(g_phononLog, message ? message : "(null)", sizeof(g_phononLog) - 1);
        g_phononLog[sizeof(g_phononLog) - 1] = '\0';
    }
}

// Virtual speaker angles
static constexpr float ANGLE_FL = -30.0f;
static constexpr float ANGLE_FR =  30.0f;
static constexpr float ANGLE_C  =   0.0f;
static constexpr float ANGLE_SL = -135.0f;  // Rear-left
static constexpr float ANGLE_SR =  135.0f;  // Rear-right
static constexpr float ANGLE_RC =  180.0f;  // Rear center (directly behind)

static IPLVector3 DirectionFromAngle(float angleDeg) {
    float rad = angleDeg * DEG2RAD;
    return {sinf(rad), 0.0f, -cosf(rad)};
}

// Direction from listener position to a virtual speaker at angleDeg on unit circle
static IPLVector3 DirectionFromAngleWithListener(float angleDeg, float lx, float ly, float lz) {
    float rad = angleDeg * DEG2RAD;
    float dx = sinf(rad) - lx;
    float dy = -ly;
    float dz = -cosf(rad) - lz;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.0001f) return {0.0f, 0.0f, -1.0f};
    return {dx / len, dy / len, dz / len};
}

// Function pointers for Steam Audio — resolved manually to avoid delay-load issues
typedef IPLerror (IPLCALL *pfn_iplContextCreate)(IPLContextSettings*, IPLContext*);
typedef void    (IPLCALL *pfn_iplContextRelease)(IPLContext*);
typedef IPLerror (IPLCALL *pfn_iplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*);
typedef void    (IPLCALL *pfn_iplHRTFRelease)(IPLHRTF*);
typedef IPLerror (IPLCALL *pfn_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*);
typedef void    (IPLCALL *pfn_iplBinauralEffectRelease)(IPLBinauralEffect*);
typedef IPLAudioEffectState (IPLCALL *pfn_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);

static pfn_iplContextCreate         p_iplContextCreate = nullptr;
static pfn_iplContextRelease        p_iplContextRelease = nullptr;
static pfn_iplHRTFCreate            p_iplHRTFCreate = nullptr;
static pfn_iplHRTFRelease           p_iplHRTFRelease = nullptr;
static pfn_iplBinauralEffectCreate  p_iplBinauralEffectCreate = nullptr;
static pfn_iplBinauralEffectRelease p_iplBinauralEffectRelease = nullptr;
static pfn_iplBinauralEffectApply   p_iplBinauralEffectApply = nullptr;

static bool EnsurePhononLoaded() {
    static bool attempted = false;
    static bool loaded = false;
    if (attempted) return loaded;
    attempted = true;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';

    // Add lib subfolder to DLL search path for phonon.dll's own dependencies
    wchar_t libDir[MAX_PATH];
    wcscpy_s(libDir, exePath);
    wcscat_s(libDir, MAX_PATH, L"lib");
    SetDllDirectoryW(libDir);

    wchar_t dllPath[MAX_PATH];
    wcscpy_s(dllPath, exePath);
    wcscat_s(dllPath, MAX_PATH, L"lib\\phonon.dll");
    HMODULE hPhonon = LoadLibraryW(dllPath);
    if (!hPhonon) return false;

    // Resolve all functions manually — no delay-load needed
    p_iplContextCreate         = (pfn_iplContextCreate)GetProcAddress(hPhonon, "iplContextCreate");
    p_iplContextRelease        = (pfn_iplContextRelease)GetProcAddress(hPhonon, "iplContextRelease");
    p_iplHRTFCreate            = (pfn_iplHRTFCreate)GetProcAddress(hPhonon, "iplHRTFCreate");
    p_iplHRTFRelease           = (pfn_iplHRTFRelease)GetProcAddress(hPhonon, "iplHRTFRelease");
    p_iplBinauralEffectCreate  = (pfn_iplBinauralEffectCreate)GetProcAddress(hPhonon, "iplBinauralEffectCreate");
    p_iplBinauralEffectRelease = (pfn_iplBinauralEffectRelease)GetProcAddress(hPhonon, "iplBinauralEffectRelease");
    p_iplBinauralEffectApply   = (pfn_iplBinauralEffectApply)GetProcAddress(hPhonon, "iplBinauralEffectApply");

    loaded = p_iplContextCreate && p_iplContextRelease &&
             p_iplHRTFCreate && p_iplHRTFRelease &&
             p_iplBinauralEffectCreate && p_iplBinauralEffectRelease &&
             p_iplBinauralEffectApply;
    return loaded;
}

static SpatialAudio* g_spatialAudio = nullptr;
SpatialAudio* GetSpatialAudio() {
    if (!g_spatialAudio) g_spatialAudio = new SpatialAudio();
    return g_spatialAudio;
}
void FreeSpatialAudio() {
    if (g_spatialAudio) { g_spatialAudio->Shutdown(); delete g_spatialAudio; g_spatialAudio = nullptr; }
}

SpatialAudio::SpatialAudio()
    : m_context(nullptr), m_hrtf(nullptr)
    , m_effectL(nullptr), m_effectR(nullptr)
    , m_effectFL(nullptr), m_effectFR(nullptr), m_effectC(nullptr), m_effectSL(nullptr), m_effectSR(nullptr), m_effectRC(nullptr)
    , m_mono(nullptr), m_tmpL(nullptr), m_tmpR(nullptr), m_savL(nullptr), m_savR(nullptr)
    , m_upmix(nullptr), m_outAccL(nullptr), m_outAccR(nullptr)
    , m_convBuf(nullptr), m_convBufSize(0)
    , m_carryCount(0), m_queueCount(0), m_sampleRate(0), m_initialized(false)
    , m_mode(SpatialMode::Binaural), m_rearCenter(true), m_lastError{} {
    InitializeCriticalSection(&m_cs);
}

SpatialAudio::~SpatialAudio() {
    Shutdown();
    DeleteCriticalSection(&m_cs);
}

void SpatialAudio::SetMode(SpatialMode mode) {
    EnterCriticalSection(&m_cs);
    if (m_mode != mode) {
        bool wasInit = m_initialized;
        int sr = m_sampleRate;
        // Mark uninitialized first so DSP callback bails out immediately
        m_initialized = false;
        // Release old effects
        if (m_effectL)  { p_iplBinauralEffectRelease(&m_effectL);  m_effectL  = nullptr; }
        if (m_effectR)  { p_iplBinauralEffectRelease(&m_effectR);  m_effectR  = nullptr; }
        if (m_effectFL) { p_iplBinauralEffectRelease(&m_effectFL); m_effectFL = nullptr; }
        if (m_effectFR) { p_iplBinauralEffectRelease(&m_effectFR); m_effectFR = nullptr; }
        if (m_effectC)  { p_iplBinauralEffectRelease(&m_effectC);  m_effectC  = nullptr; }
        if (m_effectSL) { p_iplBinauralEffectRelease(&m_effectSL); m_effectSL = nullptr; }
        if (m_effectSR) { p_iplBinauralEffectRelease(&m_effectSR); m_effectSR = nullptr; }
        if (m_effectRC) { p_iplBinauralEffectRelease(&m_effectRC); m_effectRC = nullptr; }
        m_mode = mode;
        // Recreate effects for new mode
        if (wasInit && m_hrtf) {
            IPLAudioSettings as = {};
            as.samplingRate = sr;
            as.frameSize = FRAME_SIZE;
            IPLBinauralEffectSettings bs = {};
            bs.hrtf = m_hrtf;
            bool ok = true;
            if (m_mode == SpatialMode::Binaural) {
                ok = (p_iplBinauralEffectCreate(m_context, &as, &bs, &m_effectL) == IPL_STATUS_SUCCESS) &&
                     (p_iplBinauralEffectCreate(m_context, &as, &bs, &m_effectR) == IPL_STATUS_SUCCESS);
            } else {
                IPLBinauralEffect* effects[] = {&m_effectFL, &m_effectFR, &m_effectC, &m_effectSL, &m_effectSR, &m_effectRC};
                for (int i = 0; i < 6 && ok; i++)
                    ok = (p_iplBinauralEffectCreate(m_context, &as, &bs, effects[i]) == IPL_STATUS_SUCCESS);
            }
            m_carryCount = 0;
            memset(m_queueL, 0, FRAME_SIZE * sizeof(float));
            memset(m_queueR, 0, FRAME_SIZE * sizeof(float));
            m_queueCount = FRAME_SIZE;
            m_initialized = ok;
        }
    }
    LeaveCriticalSection(&m_cs);
}

bool SpatialAudio::Initialize(int sampleRate) {
    EnterCriticalSection(&m_cs);
    m_lastError[0] = L'\0';

    if (m_initialized) {
        if (m_sampleRate == sampleRate) { LeaveCriticalSection(&m_cs); return true; }
        m_initialized = false;
        // Inline shutdown while holding lock
        if (m_effectL)  { p_iplBinauralEffectRelease(&m_effectL);  m_effectL  = nullptr; }
        if (m_effectR)  { p_iplBinauralEffectRelease(&m_effectR);  m_effectR  = nullptr; }
        if (m_effectFL) { p_iplBinauralEffectRelease(&m_effectFL); m_effectFL = nullptr; }
        if (m_effectFR) { p_iplBinauralEffectRelease(&m_effectFR); m_effectFR = nullptr; }
        if (m_effectC)  { p_iplBinauralEffectRelease(&m_effectC);  m_effectC  = nullptr; }
        if (m_effectSL) { p_iplBinauralEffectRelease(&m_effectSL); m_effectSL = nullptr; }
        if (m_effectSR) { p_iplBinauralEffectRelease(&m_effectSR); m_effectSR = nullptr; }
        if (m_effectRC) { p_iplBinauralEffectRelease(&m_effectRC); m_effectRC = nullptr; }
        if (m_hrtf)     { p_iplHRTFRelease(&m_hrtf);    m_hrtf    = nullptr; }
        if (m_context)  { p_iplContextRelease(&m_context); m_context = nullptr; }
        delete[] m_mono; m_mono = nullptr;
        delete[] m_tmpL; m_tmpL = nullptr;
        delete[] m_tmpR; m_tmpR = nullptr;
        delete[] m_savL; m_savL = nullptr;
        delete[] m_savR; m_savR = nullptr;
        delete[] m_upmix; m_upmix = nullptr;
        delete[] m_outAccL; m_outAccL = nullptr;
        delete[] m_outAccR; m_outAccR = nullptr;
        delete[] m_convBuf; m_convBuf = nullptr;
        m_convBufSize = 0;
        m_carryCount = 0;
        m_queueCount = 0;
    }
    if (!EnsurePhononLoaded()) {
        wcscpy_s(m_lastError, L"Failed to load phonon.dll from lib\\ folder. "
                 L"Make sure phonon.dll is in the lib subfolder next to FastPlay.exe.");
        LeaveCriticalSection(&m_cs);
        return false;
    }
    m_sampleRate = sampleRate;

    g_phononLog[0] = '\0';
    IPLContextSettings cs = {};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = PhononLogCallback;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;
    IPLerror err = p_iplContextCreate(&cs, &m_context);
    if (err != IPL_STATUS_SUCCESS) {
        if (g_phononLog[0]) {
            wchar_t logW[512];
            MultiByteToWideChar(CP_UTF8, 0, g_phononLog, -1, logW, 512);
            swprintf(m_lastError, 256, L"Steam Audio context creation failed (error %d): %s", (int)err, logW);
        } else {
            swprintf(m_lastError, 256, L"Steam Audio context creation failed (error %d). "
                     L"phonon.dll may be corrupt or incompatible.", (int)err);
        }
        LeaveCriticalSection(&m_cs);
        return false;
    }

    IPLAudioSettings as = {};
    as.samplingRate = sampleRate;
    as.frameSize = FRAME_SIZE;

    IPLHRTFSettings hs = {};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    g_phononLog[0] = '\0';
    err = p_iplHRTFCreate(m_context, &as, &hs, &m_hrtf);
    if (err != IPL_STATUS_SUCCESS) {
        if (g_phononLog[0]) {
            wchar_t logW[512];
            MultiByteToWideChar(CP_UTF8, 0, g_phononLog, -1, logW, 512);
            swprintf(m_lastError, 256, L"HRTF creation failed (error %d): %s", (int)err, logW);
        } else {
            swprintf(m_lastError, 256, L"HRTF creation failed (error %d, sample rate %d Hz). "
                     L"The audio format may be unsupported.", (int)err, sampleRate);
        }
        LeaveCriticalSection(&m_cs);
        Shutdown();
        return false;
    }

    IPLBinauralEffectSettings bs = {};
    bs.hrtf = m_hrtf;

    const wchar_t* modeName = (m_mode == SpatialMode::Binaural) ? L"Binaural" : L"5.1 Surround";

    if (m_mode == SpatialMode::Binaural) {
        err = p_iplBinauralEffectCreate(m_context, &as, &bs, &m_effectL);
        if (err != IPL_STATUS_SUCCESS) {
            swprintf(m_lastError, 256, L"Binaural effect (L) creation failed (error %d).", (int)err);
            LeaveCriticalSection(&m_cs);
            Shutdown();
            return false;
        }
        err = p_iplBinauralEffectCreate(m_context, &as, &bs, &m_effectR);
        if (err != IPL_STATUS_SUCCESS) {
            swprintf(m_lastError, 256, L"Binaural effect (R) creation failed (error %d).", (int)err);
            LeaveCriticalSection(&m_cs);
            Shutdown();
            return false;
        }
    } else {
        const wchar_t* names[] = {L"FL", L"FR", L"C", L"SL", L"SR", L"RC"};
        IPLBinauralEffect* effects[] = {&m_effectFL, &m_effectFR, &m_effectC, &m_effectSL, &m_effectSR, &m_effectRC};
        for (int i = 0; i < 6; i++) {
            err = p_iplBinauralEffectCreate(m_context, &as, &bs, effects[i]);
            if (err != IPL_STATUS_SUCCESS) {
                swprintf(m_lastError, 256, L"%s mode: binaural effect (%s) creation failed (error %d).",
                         modeName, names[i], (int)err);
                LeaveCriticalSection(&m_cs);
                Shutdown();
                return false;
            }
        }
    }

    m_mono = new float[FRAME_SIZE]();
    m_tmpL = new float[FRAME_SIZE]();
    m_tmpR = new float[FRAME_SIZE]();
    m_savL = new float[FRAME_SIZE]();
    m_savR = new float[FRAME_SIZE]();
    m_upmix = new float[6 * FRAME_SIZE]();
    m_outAccL = new float[FRAME_SIZE]();
    m_outAccR = new float[FRAME_SIZE]();

    // Verify effects actually work by processing a silent test frame.
    // This catches DLL version mismatches, missing dependencies, or corrupt
    // effects that passed creation but crash during apply.
    {
        bool testOk = true;
        DWORD exCode = 0;
        g_phononLog[0] = '\0';
        float* testMono[1] = { m_mono };
        float* testOut[2] = { m_tmpL, m_tmpR };
        IPLAudioBuffer testIn = { 1, FRAME_SIZE, testMono };
        IPLAudioBuffer testOutBuf = { 2, FRAME_SIZE, testOut };
        IPLBinauralEffectParams testParams = {};
        testParams.direction = {0.0f, 0.0f, -1.0f};
        testParams.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
        testParams.spatialBlend = 1.0f;
        testParams.hrtf = m_hrtf;

        __try {
            if (m_mode == SpatialMode::Binaural) {
                p_iplBinauralEffectApply(m_effectL, &testParams, &testIn, &testOutBuf);
                p_iplBinauralEffectApply(m_effectR, &testParams, &testIn, &testOutBuf);
            } else {
                IPLBinauralEffect testEffects[] = {m_effectFL, m_effectFR, m_effectC, m_effectSL, m_effectSR, m_effectRC};
                for (int i = 0; i < 6; i++)
                    p_iplBinauralEffectApply(testEffects[i], &testParams, &testIn, &testOutBuf);
            }
        } __except(exCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            testOk = false;
        }

        if (!testOk) {
            if (exCode == 0xC0000005) {
                swprintf(m_lastError, 256, L"Steam Audio crashed with an access violation (0x%08X) during processing. "
                         L"phonon.dll may be incompatible with this system.", exCode);
            } else if (exCode == 0xC000001D || exCode == 0xC000001E) {
                swprintf(m_lastError, 256, L"Steam Audio crashed with an illegal instruction (0x%08X). "
                         L"Your CPU may not support the required instruction set.", exCode);
            } else if ((exCode & 0xFFFF0000) == 0xC06D0000) {
                // Delay-load failure (module not found or proc not found)
                swprintf(m_lastError, 256, L"Steam Audio failed to load a required DLL (0x%08X). "
                         L"phonon.dll may have missing dependencies.", exCode);
            } else {
                swprintf(m_lastError, 256, L"Steam Audio crashed during processing (exception 0x%08X). "
                         L"phonon.dll may be incompatible or corrupt.", exCode);
            }
            LeaveCriticalSection(&m_cs);
            Shutdown();
            return false;
        }
    }

    m_carryCount = 0;

    // Pre-fill output queue with FRAME_SIZE silence to cover initial deficit
    memset(m_queueL, 0, FRAME_SIZE * sizeof(float));
    memset(m_queueR, 0, FRAME_SIZE * sizeof(float));
    m_queueCount = FRAME_SIZE;

    m_initialized = true;
    LeaveCriticalSection(&m_cs);
    return true;
}

float* SpatialAudio::GetConversionBuffer(int samples) {
    if (samples > m_convBufSize) {
        delete[] m_convBuf;
        m_convBuf = new float[samples];
        m_convBufSize = samples;
    }
    return m_convBuf;
}

void SpatialAudio::Shutdown() {
    EnterCriticalSection(&m_cs);
    m_initialized = false;  // Stop DSP callback from using effects
    if (m_effectL)  { p_iplBinauralEffectRelease(&m_effectL);  m_effectL  = nullptr; }
    if (m_effectR)  { p_iplBinauralEffectRelease(&m_effectR);  m_effectR  = nullptr; }
    if (m_effectFL) { p_iplBinauralEffectRelease(&m_effectFL); m_effectFL = nullptr; }
    if (m_effectFR) { p_iplBinauralEffectRelease(&m_effectFR); m_effectFR = nullptr; }
    if (m_effectC)  { p_iplBinauralEffectRelease(&m_effectC);  m_effectC  = nullptr; }
    if (m_effectSL) { p_iplBinauralEffectRelease(&m_effectSL); m_effectSL = nullptr; }
    if (m_effectSR) { p_iplBinauralEffectRelease(&m_effectSR); m_effectSR = nullptr; }
    if (m_effectRC) { p_iplBinauralEffectRelease(&m_effectRC); m_effectRC = nullptr; }
    if (m_hrtf)     { p_iplHRTFRelease(&m_hrtf);    m_hrtf    = nullptr; }
    if (m_context)  { p_iplContextRelease(&m_context); m_context = nullptr; }
    delete[] m_mono; m_mono = nullptr;
    delete[] m_tmpL; m_tmpL = nullptr;
    delete[] m_tmpR; m_tmpR = nullptr;
    delete[] m_savL; m_savL = nullptr;
    delete[] m_savR; m_savR = nullptr;
    delete[] m_upmix; m_upmix = nullptr;
    delete[] m_outAccL; m_outAccL = nullptr;
    delete[] m_outAccR; m_outAccR = nullptr;
    delete[] m_convBuf; m_convBuf = nullptr;
    m_convBufSize = 0;
    m_carryCount = 0;
    m_queueCount = 0;
    LeaveCriticalSection(&m_cs);
}

// Process a single FRAME_SIZE chunk in binaural (2-speaker) mode
void SpatialAudio::ProcessBinauralFrame(float* frameL, float* frameR) {
    float width = GetParamValue(ParamId::SpatialWidth);
    float rotation = GetParamValue(ParamId::SpatialRotation);
    float lx = GetParamValue(ParamId::SpatialX);
    float ly = GetParamValue(ParamId::SpatialY);
    float lz = GetParamValue(ParamId::SpatialZ);
    IPLVector3 dirL = DirectionFromAngleWithListener(-width - rotation, lx, ly, lz);
    IPLVector3 dirR = DirectionFromAngleWithListener( width - rotation, lx, ly, lz);

    float* monoPtrs[1] = { m_mono };
    float* outPtrs[2] = { m_tmpL, m_tmpR };
    IPLAudioBuffer inBuf = { 1, FRAME_SIZE, monoPtrs };
    IPLAudioBuffer outBuf = { 2, FRAME_SIZE, outPtrs };

    IPLBinauralEffectParams pL = {};
    pL.direction = dirL;
    pL.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
    pL.spatialBlend = 1.0f;
    pL.hrtf = m_hrtf;

    IPLBinauralEffectParams pR = {};
    pR.direction = dirR;
    pR.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
    pR.spatialBlend = 1.0f;
    pR.hrtf = m_hrtf;

    // HRTF left channel
    memcpy(m_mono, frameL, FRAME_SIZE * sizeof(float));
    p_iplBinauralEffectApply(m_effectL, &pL, &inBuf, &outBuf);
    memcpy(m_savL, m_tmpL, FRAME_SIZE * sizeof(float));
    memcpy(m_savR, m_tmpR, FRAME_SIZE * sizeof(float));

    // HRTF right channel
    memcpy(m_mono, frameR, FRAME_SIZE * sizeof(float));
    p_iplBinauralEffectApply(m_effectR, &pR, &inBuf, &outBuf);

    // Sum both into frameL/frameR (output)
    for (int i = 0; i < FRAME_SIZE; i++) {
        frameL[i] = (m_savL[i] + m_tmpL[i]) * 0.707f;
        frameR[i] = (m_savR[i] + m_tmpR[i]) * 0.707f;
    }
}

// Process a single FRAME_SIZE chunk in virtual surround mode
void SpatialAudio::ProcessSurroundFrame(float* frameL, float* frameR) {
    float width = GetParamValue(ParamId::SpatialWidth);
    float rotation = GetParamValue(ParamId::SpatialRotation);
    float lx = GetParamValue(ParamId::SpatialX);
    float ly = GetParamValue(ParamId::SpatialY);
    float lz = GetParamValue(ParamId::SpatialZ);

    // Width controls the front speaker angle; surround placement depends on RC
    float frontAngle = width;
    float surroundAngle;
    if (m_rearCenter) {
        // With RC: surrounds go far back (135-172°) since RC fills dead center
        surroundAngle = 180.0f - (width * 0.5f);
    } else {
        // Without RC: surrounds stay to the sides (90-120°), standard 5.1 layout
        // This creates a clear side-surround without rear fill
        surroundAngle = 90.0f + (width * 0.33f);
    }

    // Upmix stereo to 6 channels using pre-allocated m_upmix buffer
    static constexpr int STRIDE = FRAME_SIZE;
    float* fl = m_upmix;
    float* fr = m_upmix + STRIDE;
    float* center = m_upmix + 2 * STRIDE;
    float* sl = m_upmix + 3 * STRIDE;
    float* sr = m_upmix + 4 * STRIDE;
    float* rc = m_upmix + 5 * STRIDE;
    for (int i = 0; i < FRAME_SIZE; i++) {
        float l = frameL[i];
        float r = frameR[i];
        float mid = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;

        fl[i] = l;
        fr[i] = r;
        center[i] = mid * 0.6f;
        sl[i] = l * 0.5f + side * 1.0f;
        sr[i] = r * 0.5f - side * 1.0f;
        rc[i] = mid * 0.9f;
    }

    float* monoPtrs[1] = { m_mono };
    float* outPtrs[2] = { m_tmpL, m_tmpR };
    IPLAudioBuffer inBuf = { 1, FRAME_SIZE, monoPtrs };
    IPLAudioBuffer outBuf = { 2, FRAME_SIZE, outPtrs };

    memset(m_outAccL, 0, FRAME_SIZE * sizeof(float));
    memset(m_outAccR, 0, FRAME_SIZE * sizeof(float));

    struct SpeakerInfo {
        float* signal;
        float angle;
        float gain;
        IPLBinauralEffect effect;
    };

    SpeakerInfo speakers[] = {
        { fl,     -frontAngle    + rotation, 0.9f, m_effectFL },
        { fr,      frontAngle    + rotation, 0.9f, m_effectFR },
        { center,  0.0f          + rotation, 0.7f, m_effectC  },
        { sl,     -surroundAngle + rotation, 1.0f, m_effectSL },
        { sr,      surroundAngle + rotation, 1.0f, m_effectSR },
        { rc,      180.0f        + rotation, 1.0f, m_effectRC },
    };
    int speakerCount = m_rearCenter ? 6 : 5;

    for (int s = 0; s < speakerCount; s++) {
        auto& spk = speakers[s];
        IPLBinauralEffectParams params = {};
        params.direction = DirectionFromAngleWithListener(spk.angle, lx, ly, lz);
        params.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
        params.spatialBlend = 1.0f;
        params.hrtf = m_hrtf;

        memcpy(m_mono, spk.signal, FRAME_SIZE * sizeof(float));
        p_iplBinauralEffectApply(spk.effect, &params, &inBuf, &outBuf);

        for (int i = 0; i < FRAME_SIZE; i++) {
            m_outAccL[i] += m_tmpL[i] * spk.gain;
            m_outAccR[i] += m_tmpR[i] * spk.gain;
        }
    }

    float norm = m_rearCenter ? 0.33f : 0.38f;
    for (int i = 0; i < FRAME_SIZE; i++) {
        frameL[i] = m_outAccL[i] * norm;
        frameR[i] = m_outAccR[i] * norm;
    }
}

void SpatialAudio::Process(float* buffer, int frameCount, float blend) {
    m_debugStep = 100;
    if (!m_initialized || frameCount <= 0) return;
    m_debugStep = 101;
    if (!TryEnterCriticalSection(&m_cs)) return;
    m_debugStep = 102;
    if (!m_initialized) { LeaveCriticalSection(&m_cs); return; }

    m_debugStep = 110;
    int newPos = 0;

    while (true) {
        int available = m_carryCount + (frameCount - newPos);
        if (available < FRAME_SIZE) break;
        if (m_queueCount + FRAME_SIZE > MAX_QUEUE) break;

        m_debugStep = 120;
        // Fill one frame of L and R from carry then input
        float fL[FRAME_SIZE], fR[FRAME_SIZE];
        int filled = 0;

        int fromCarry = m_carryCount;
        if (fromCarry > FRAME_SIZE) fromCarry = FRAME_SIZE;
        for (int i = 0; i < fromCarry; i++) {
            fL[i] = m_carryL[i];
            fR[i] = m_carryR[i];
        }
        filled = fromCarry;

        m_carryCount -= fromCarry;
        if (m_carryCount > 0) {
            memmove(m_carryL, m_carryL + fromCarry, m_carryCount * sizeof(float));
            memmove(m_carryR, m_carryR + fromCarry, m_carryCount * sizeof(float));
        }

        m_debugStep = 130;
        int fromInput = FRAME_SIZE - filled;
        for (int i = 0; i < fromInput; i++) {
            fL[filled + i] = buffer[(newPos + i) * 2];
            fR[filled + i] = buffer[(newPos + i) * 2 + 1];
        }
        newPos += fromInput;

        m_debugStep = 140;
        // Process frame based on mode
        if (m_mode == SpatialMode::Surround51) {
            m_debugStep = 150;
            ProcessSurroundFrame(fL, fR);
        } else {
            m_debugStep = 160;
            ProcessBinauralFrame(fL, fR);
        }
        m_debugStep = 170;

        // Append to output queue
        for (int i = 0; i < FRAME_SIZE; i++) {
            m_queueL[m_queueCount + i] = fL[i];
            m_queueR[m_queueCount + i] = fR[i];
        }
        m_queueCount += FRAME_SIZE;
        m_debugStep = 175;
    }

    m_debugStep = 180;

    // Save remaining new input as carry (should be < FRAME_SIZE if queue had room)
    int remaining = frameCount - newPos;
    if (remaining > FRAME_SIZE) remaining = FRAME_SIZE;  // clamp to prevent overrun
    if (remaining > 0) {
        for (int i = 0; i < remaining; i++) {
            m_carryL[m_carryCount + i] = buffer[(newPos + i) * 2];
            m_carryR[m_carryCount + i] = buffer[(newPos + i) * 2 + 1];
        }
        m_carryCount += remaining;
    }

    m_debugStep = 190;

    // Write from output queue to buffer
    bool doBlend = (blend < 1.0f);
    float wet = blend, dry = 1.0f - blend;

    int toWrite = frameCount;
    if (toWrite > m_queueCount) toWrite = m_queueCount;

    for (int i = 0; i < toWrite; i++) {
        if (doBlend) {
            buffer[i * 2]     = buffer[i * 2]     * dry + m_queueL[i] * wet;
            buffer[i * 2 + 1] = buffer[i * 2 + 1] * dry + m_queueR[i] * wet;
        } else {
            buffer[i * 2]     = m_queueL[i];
            buffer[i * 2 + 1] = m_queueR[i];
        }
    }

    m_debugStep = 195;

    if (toWrite > 0 && toWrite < m_queueCount) {
        memmove(m_queueL, m_queueL + toWrite, (m_queueCount - toWrite) * sizeof(float));
        memmove(m_queueR, m_queueR + toWrite, (m_queueCount - toWrite) * sizeof(float));
    }
    m_queueCount -= toWrite;
    m_debugStep = 200;
    LeaveCriticalSection(&m_cs);
}

#endif
