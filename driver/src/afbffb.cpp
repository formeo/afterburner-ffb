// afbffb.cpp - DirectInput force feedback effect driver for
// Thrustmaster Top Gun Afterburner Force Feedback (USB 044F:B550).
//
// DirectInput loads this COM DLL into the game process when the joystick's
// OEMForceFeedback registry key points to our CLSID. Every DirectInput effect
// call is translated into the device's own HID output reports, whose layout
// comes from its 851-byte report descriptor and was verified on hardware:
//   0x0B Set Effect   0x0C Envelope   0x0D Constant   0x0E Periodic
//   0x0F Condition    0x51 Effect Op  0x52 Device Control   0x43 Gain
// No kernel driver is involved: the stock Windows HID driver stays in place.
//
// Every call and every report is logged to %TEMP%\afbffb.log.

#define INITGUID
#define _USE_MATH_DEFINES
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <dinputd.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <olectl.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// {82B91B86-B647-425D-8D3A-322DD473AD89}
DEFINE_GUID(CLSID_AfbFFB, 0x82b91b86, 0xb647, 0x425d, 0x8d, 0x3a, 0x32, 0x2d, 0xd4, 0x73, 0xad, 0x89);
static const wchar_t* kClsidStr = L"{82B91B86-B647-425D-8D3A-322DD473AD89}";
static const wchar_t* kOemKey =
    L"SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_044F&PID_B550";
static const wchar_t* kUserOemKey =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_044F&PID_B550";

// Device facts (from feature report 0x0B and the descriptor).
static const int kBlocks = 20;          // simultaneous effects
static const int kBlockStride = 48;     // 20 * 48 = 960 <= RAM pool of 1000
static const BYTE AX_X = 1 << 6, AX_Y = 1 << 7, DIR_EN = 1 << 5;
enum : BYTE { T_CONST = 1, T_SQUARE, T_TRIANGLE, T_SINE, T_SAWUP, T_SAWDOWN, T_SPRING, T_DAMPER };
enum : BYTE { OP_STOP = 1, OP_START = 2, OP_SOLO = 3 };
enum : BYTE { DC_RESET = 1, DC_STOPALL, DC_ENABLE, DC_DISABLE, DC_PAUSE, DC_CONTINUE };

static HINSTANCE g_hinst;
static LONG g_locks;
static CRITICAL_SECTION g_cs;
static FILE* g_log;

// ---------------------------------------------------------------- logging
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu/%d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentProcessId(), (int)sizeof(void*) * 8);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- device
struct Fx {
    bool used, playing;
    BYTE type;
    DWORD durMs;   // 0xFFFF = infinite
    DWORD loops;
    DWORD started;
};

static struct {
    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT outLen = 0;
    bool actuators = true, paused = false;
    BYTE gain = 128;  // device gain 0..128, re-sent after every reset
    Fx fx[kBlocks] = {};
} g_dev;

static bool sendReport(const BYTE* data, size_t n) {
    char hex[3 * 64 + 1] = {};
    for (size_t i = 0; i < n && i < 64; i++) sprintf(hex + 3 * i, "%02x ", data[i]);
    if (g_dev.h == INVALID_HANDLE_VALUE) {
        logf("  OUT (no device) %s", hex);
        return false;
    }
    BYTE buf[64] = {};
    size_t len = g_dev.outLen ? g_dev.outLen : n;
    if (len > sizeof(buf)) len = sizeof(buf);
    memcpy(buf, data, n < len ? n : len);
    DWORD written = 0;
    BOOL ok = WriteFile(g_dev.h, buf, (DWORD)len, &written, nullptr);
    if (!ok) {
        DWORD err = GetLastError();
        ok = HidD_SetOutputReport(g_dev.h, buf, (ULONG)len);
        logf("  OUT %s-> WriteFile err %lu, SetOutputReport %s", hex, err, ok ? "ok" : "FAILED");
    } else {
        logf("  OUT %s", hex);
    }
    return ok != FALSE;
}

static void put16(BYTE* p, long v) { p[0] = (BYTE)(v & 0xFF); p[1] = (BYTE)((v >> 8) & 0xFF); }
static long clampl(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }
static long scale(double v, double from, double to) { return lround(v * to / from); }
static long usToMs(DWORD us, long maxMs) {
    if (us == INFINITE) return 0xFFFF;
    return clampl((long)(us / 1000), 0, maxMs);
}

static void sendGain() { BYTE r[2] = {0x43, g_dev.gain}; sendReport(r, 2); }
static void devCtrl(BYTE c) {
    BYTE r[2] = {0x52, c};
    sendReport(r, 2);
    if (c == 1) sendGain();  // Reset may clear the device gain
}
static void effectOp(int idx, BYTE op, BYTE loops) { BYTE r[4] = {0x51, (BYTE)idx, op, loops}; sendReport(r, 4); }

static bool openDevice(const wchar_t* path) {
    g_dev.h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (g_dev.h == INVALID_HANDLE_VALUE)
        g_dev.h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_dev.h == INVALID_HANDLE_VALUE) {
        logf("CreateFile failed, err %lu", GetLastError());
        return false;
    }
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(g_dev.h, &pp)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) g_dev.outLen = caps.OutputReportByteLength;
        HidD_FreePreparsedData(pp);
    }
    logf("device opened, OutputReportByteLength=%u", g_dev.outLen);
    return true;
}

// ---------------------------------------------------------------- effect translation
// Axis identifiers arrive as DirectInput object IDs (or offsets); X = instance/offset 0, Y = 1/4.
static int axisIndex(DWORD v, DWORD flags) {
    if (flags & DIEFF_OBJECTOFFSETS) return v == DIJOFS_Y ? 1 : 0;
    return DIDFT_GETINSTANCE(v) == 1 ? 1 : 0;
}

struct Geometry {
    BYTE axesBits = 0, dirByte = 0;
    bool dirEnable = false;
    int sign = 1;            // single-axis cartesian direction sign
    int axis[2] = {0, 1};    // axis index of each DIEFFECT axis
    int nAxes = 0;
};

static Geometry geometry(LPCDIEFFECT e) {
    Geometry g;
    g.nAxes = (int)(e->cAxes > 2 ? 2 : e->cAxes);
    for (int i = 0; i < g.nAxes; i++) {
        g.axis[i] = e->rgdwAxes ? axisIndex(e->rgdwAxes[i], e->dwFlags) : i;
        g.axesBits |= g.axis[i] ? AX_Y : AX_X;
    }
    if (!e->rglDirection) return g;
    if (g.nAxes == 1) {
        if ((e->dwFlags & DIEFF_CARTESIAN) && e->rglDirection[0] < 0) g.sign = -1;
        return g;
    }
    double ang;  // DirectInput polar: centidegrees clockwise from north, "where the force comes from"
    if (e->dwFlags & DIEFF_POLAR) {
        ang = e->rglDirection[0];
    } else if (e->dwFlags & DIEFF_SPHERICAL) {
        ang = e->rglDirection[0] + 9000.0;
    } else {
        double x = e->rglDirection[g.axis[0] == 0 ? 0 : 1], y = e->rglDirection[g.axis[0] == 0 ? 1 : 0];
        ang = atan2(x, -y) * 18000.0 / M_PI;
    }
    ang = fmod(fmod(ang, 36000.0) + 36000.0, 36000.0);
    g.dirByte = (BYTE)(lround(ang * 256.0 / 36000.0) & 0xFF);
    g.dirEnable = true;
    return g;
}

static void sendEnvelope(WORD off, LPCDIENVELOPE env, double gain) {
    BYTE r[9] = {0x0C};
    put16(r + 1, off);
    if (env && env->dwSize >= sizeof(DIENVELOPE)) {
        put16(r + 3, usToMs(env->dwAttackTime, 10000));
        r[5] = (BYTE)clampl(scale(env->dwAttackLevel * gain, 10000, 127), 0, 127);
        put16(r + 6, usToMs(env->dwFadeTime, 10000));
        r[8] = (BYTE)clampl(scale(env->dwFadeLevel * gain, 10000, 127), 0, 127);
    } else {
        r[5] = 127;  // no envelope: no attack/fade, full levels
        r[8] = 127;
    }
    sendReport(r, sizeof(r));
}

static bool sendTypeSpecific(BYTE type, WORD off1, WORD off2, LPCDIEFFECT e, const Geometry& g, double gain) {
    if (!e->lpvTypeSpecificParams) return false;
    if (type == T_CONST) {
        if (e->cbTypeSpecificParams < sizeof(DICONSTANTFORCE)) return false;
        auto* c = (const DICONSTANTFORCE*)e->lpvTypeSpecificParams;
        BYTE r[4] = {0x0D};
        put16(r + 1, off1);
        r[3] = (BYTE)(signed char)clampl(scale(c->lMagnitude * gain, 10000, 127) * g.sign, -127, 127);
        return sendReport(r, sizeof(r));
    }
    if (type >= T_SQUARE && type <= T_SAWDOWN) {
        if (e->cbTypeSpecificParams < sizeof(DIPERIODIC)) return false;
        auto* p = (const DIPERIODIC*)e->lpvTypeSpecificParams;
        BYTE r[8] = {0x0E};
        put16(r + 1, off1);
        r[3] = (BYTE)clampl(scale(p->dwMagnitude * gain, 10000, 127), 0, 127);
        r[4] = (BYTE)(signed char)clampl(scale(p->lOffset * gain, 10000, 127) * g.sign, -127, 127);
        r[5] = (BYTE)clampl(scale(p->dwPhase % 36000, 36000, 256), 0, 255);
        put16(r + 6, clampl((long)(p->dwPeriod / 1000), 1, 10000));
        return sendReport(r, sizeof(r));
    }
    // Conditions: one block per axis, X -> off1, Y -> off2.
    int n = (int)(e->cbTypeSpecificParams / sizeof(DICONDITION));
    if (n < 1) return false;
    auto* conds = (const DICONDITION*)e->lpvTypeSpecificParams;
    const DICONDITION zero = {};
    for (int ax = 0; ax < 2; ax++) {
        const DICONDITION* c = &zero;
        for (int i = 0; i < g.nAxes; i++)
            if (g.axis[i] == ax) c = &conds[i < n ? i : n - 1];
        BYTE r[11] = {0x0F};
        put16(r + 1, ax ? off2 : off1);
        r[3] = (BYTE)(signed char)clampl(scale(c->lPositiveCoefficient * gain, 10000, 100), -100, 100);
        r[4] = (BYTE)(signed char)clampl(scale(c->lNegativeCoefficient * gain, 10000, 100), -100, 100);
        put16(r + 5, clampl(scale(c->lOffset, 10000, 500), -500, 500));
        put16(r + 7, clampl(scale(c->lDeadBand, 10000, 1000), 0, 1000));
        r[9] = (BYTE)clampl(scale(c->dwPositiveSaturation, 10000, 100), 0, 100);
        r[10] = (BYTE)clampl(scale(c->dwNegativeSaturation, 10000, 100), 0, 100);
        sendReport(r, sizeof(r));
    }
    return true;
}

static void sendSetEffect(int idx, BYTE type, WORD off1, WORD off2, LPCDIEFFECT e, const Geometry& g) {
    bool cond = type >= T_SPRING;
    BYTE trig = 0;
    if (e->dwTriggerButton != DIEB_NOTRIGGER)
        trig = (BYTE)clampl((e->dwFlags & DIEFF_OBJECTOFFSETS) ? (long)(e->dwTriggerButton - DIJOFS_BUTTON0) + 1
                                                               : (long)DIDFT_GETINSTANCE(e->dwTriggerButton) + 1, 1, 8);
    BYTE r[15] = {0x0B, (BYTE)idx, type};
    r[3] = (BYTE)((trig & 0x1F) | ((!cond && g.dirEnable) ? DIR_EN : 0) | g.axesBits);
    put16(r + 4, usToMs(e->dwDuration, 0xFFFE));
    r[6] = cond ? 0 : g.dirByte;
    put16(r + 7, e->dwTriggerRepeatInterval == INFINITE ? 0 : usToMs(e->dwTriggerRepeatInterval, 0xFFFE));
    put16(r + 9, off1);
    put16(r + 11, off2);
    put16(r + 13, e->dwSize >= sizeof(DIEFFECT) ? usToMs(e->dwStartDelay, 0xFFFE) : 0);
    sendReport(r, sizeof(r));
}

// ---------------------------------------------------------------- COM object
class EffectDriver : public IDirectInputEffectDriver {
    LONG m_ref = 1;

public:
    EffectDriver() { InterlockedIncrement(&g_locks); }
    virtual ~EffectDriver() { InterlockedDecrement(&g_locks); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDirectInputEffectDriver) {
            *ppv = static_cast<IDirectInputEffectDriver*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    STDMETHODIMP DeviceID(DWORD diVer, DWORD extId, DWORD begin, DWORD intId, LPVOID reserved) override {
        EnterCriticalSection(&g_cs);
        auto* info = (LPDIHIDFFINITINFO)reserved;
        logf("DeviceID ver=%lx ext=%lu begin=%lu int=%lu path=%ls", diVer, extId, begin, intId,
             (info && info->pwszDeviceInterface) ? info->pwszDeviceInterface : L"(none)");
        if (begin) {
            if (g_dev.h == INVALID_HANDLE_VALUE && info && info->pwszDeviceInterface)
                openDevice(info->pwszDeviceInterface);
            memset(g_dev.fx, 0, sizeof(g_dev.fx));
            devCtrl(DC_RESET);
            devCtrl(DC_ENABLE);
            g_dev.actuators = true;
            g_dev.paused = false;
        } else if (g_dev.h != INVALID_HANDLE_VALUE) {
            devCtrl(DC_STOPALL);
            CloseHandle(g_dev.h);
            g_dev.h = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP GetVersions(LPDIDRIVERVERSIONS v) override {
        logf("GetVersions");
        if (!v || v->dwSize < sizeof(DIDRIVERVERSIONS)) return E_INVALIDARG;
        v->dwFirmwareRevision = 0x100;
        v->dwHardwareRevision = 0x100;
        v->dwFFDriverVersion = 0x00000001;
        return S_OK;
    }

    STDMETHODIMP Escape(DWORD, DWORD, LPDIEFFESCAPE) override {
        logf("Escape (unsupported)");
        return E_NOTIMPL;
    }

    STDMETHODIMP SetGain(DWORD, DWORD gain) override {
        EnterCriticalSection(&g_cs);
        logf("SetGain %lu", gain);
        g_dev.gain = (BYTE)clampl(scale(gain, 10000, 128), 0, 128);
        sendGain();
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP SendForceFeedbackCommand(DWORD, DWORD cmd) override {
        EnterCriticalSection(&g_cs);
        logf("SendForceFeedbackCommand 0x%lx", cmd);
        if (cmd & DISFFC_RESET) {
            devCtrl(DC_RESET);
            memset(g_dev.fx, 0, sizeof(g_dev.fx));
            g_dev.paused = false;
        }
        if (cmd & DISFFC_STOPALL) {
            devCtrl(DC_STOPALL);
            for (auto& f : g_dev.fx) f.playing = false;
        }
        if (cmd & DISFFC_PAUSE) { devCtrl(DC_PAUSE); g_dev.paused = true; }
        if (cmd & DISFFC_CONTINUE) { devCtrl(DC_CONTINUE); g_dev.paused = false; }
        if (cmd & DISFFC_SETACTUATORSON) { devCtrl(DC_ENABLE); g_dev.actuators = true; }
        if (cmd & DISFFC_SETACTUATORSOFF) { devCtrl(DC_DISABLE); g_dev.actuators = false; }
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP GetForceFeedbackState(DWORD, LPDIDEVICESTATE s) override {
        if (!s || s->dwSize < sizeof(DIDEVICESTATE)) return E_INVALIDARG;
        EnterCriticalSection(&g_cs);
        int used = 0;
        bool playing = false;
        for (int i = 0; i < kBlocks; i++) {
            used += g_dev.fx[i].used;
            playing |= isPlaying(i);
        }
        s->dwState = (used ? 0 : DIGFFS_EMPTY) | (playing ? 0 : DIGFFS_STOPPED) | (g_dev.paused ? DIGFFS_PAUSED : 0) |
                     (g_dev.actuators ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF) | DIGFFS_POWERON |
                     DIGFFS_SAFETYSWITCHON | DIGFFS_USERFFSWITCHON;
        s->dwLoad = used * 100 / kBlocks;
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP DownloadEffect(DWORD, DWORD effectId, LPDWORD pHandle, LPCDIEFFECT e, DWORD flags) override {
        if (!pHandle || !e) return E_POINTER;
        EnterCriticalSection(&g_cs);
        HRESULT hr = download(effectId, pHandle, e, flags);
        LeaveCriticalSection(&g_cs);
        return hr;
    }

    STDMETHODIMP DestroyEffect(DWORD, DWORD handle) override {
        EnterCriticalSection(&g_cs);
        logf("DestroyEffect %lu", handle);
        int idx = (int)handle - 1;
        if (idx >= 0 && idx < kBlocks && g_dev.fx[idx].used) {
            effectOp(idx, OP_STOP, 0);
            g_dev.fx[idx] = Fx{};
        }
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP StartEffect(DWORD, DWORD handle, DWORD mode, DWORD count) override {
        EnterCriticalSection(&g_cs);
        logf("StartEffect %lu mode=%lx count=%lu", handle, mode, count);
        int idx = (int)handle - 1;
        HRESULT hr = DIERR_INVALIDPARAM;
        if (idx >= 0 && idx < kBlocks && g_dev.fx[idx].used) {
            if (mode & DIES_SOLO)
                for (auto& f : g_dev.fx) f.playing = false;
            BYTE loops = count == INFINITE ? 255 : (BYTE)clampl((long)count, 1, 254);
            effectOp(idx, (mode & DIES_SOLO) ? OP_SOLO : OP_START, loops);
            Fx& f = g_dev.fx[idx];
            f.playing = true;
            f.loops = count;
            f.started = GetTickCount();
            hr = S_OK;
        }
        LeaveCriticalSection(&g_cs);
        return hr;
    }

    STDMETHODIMP StopEffect(DWORD, DWORD handle) override {
        EnterCriticalSection(&g_cs);
        logf("StopEffect %lu", handle);
        int idx = (int)handle - 1;
        if (idx >= 0 && idx < kBlocks && g_dev.fx[idx].used) {
            effectOp(idx, OP_STOP, 0);
            g_dev.fx[idx].playing = false;
        }
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

    STDMETHODIMP GetEffectStatus(DWORD, DWORD handle, LPDWORD status) override {
        if (!status) return E_POINTER;
        EnterCriticalSection(&g_cs);
        int idx = (int)handle - 1;
        *status = (idx >= 0 && idx < kBlocks && isPlaying(idx)) ? DIEGES_PLAYING : 0;
        LeaveCriticalSection(&g_cs);
        return S_OK;
    }

private:
    static bool isPlaying(int idx) {
        const Fx& f = g_dev.fx[idx];
        if (!f.used || !f.playing) return false;
        if (f.durMs == 0xFFFF || f.loops == INFINITE) return true;
        return (ULONGLONG)(GetTickCount() - f.started) < (ULONGLONG)f.durMs * (f.loops ? f.loops : 1);
    }

    HRESULT download(DWORD effectId, LPDWORD pHandle, LPCDIEFFECT e, DWORD flags) {
        logf("DownloadEffect id=%lu handle=%lu flags=%lx dur=%lu gain=%lu axes=%lu [%lx %lx] dir=[%ld %ld] effFlags=%lx "
             "cbTS=%lu",
             effectId, *pHandle, flags, e->dwDuration, e->dwGain, e->cAxes, e->rgdwAxes ? e->rgdwAxes[0] : 0,
             (e->rgdwAxes && e->cAxes > 1) ? e->rgdwAxes[1] : 0, e->rglDirection ? e->rglDirection[0] : 0,
             (e->rglDirection && e->cAxes > 1) ? e->rglDirection[1] : 0, e->dwFlags, e->cbTypeSpecificParams);
        if (effectId < T_CONST || effectId > T_DAMPER) return DIERR_UNSUPPORTED;
        if (flags & DIEP_NODOWNLOAD) return S_OK;  // parameter check only

        bool fresh = *pHandle == 0;
        int idx;
        if (fresh) {
            for (idx = 0; idx < kBlocks && g_dev.fx[idx].used; idx++) {}
            if (idx == kBlocks) return DIERR_DEVICEFULL;
        } else {
            idx = (int)*pHandle - 1;
            if (idx < 0 || idx >= kBlocks || !g_dev.fx[idx].used) return DIERR_INVALIDPARAM;
        }

        BYTE type = (BYTE)effectId;
        WORD off1 = (WORD)(idx * kBlockStride), off2 = (WORD)(off1 + 16);
        Geometry g = geometry(e);
        double gain = clampl((long)e->dwGain, 0, 10000) / 10000.0;
        bool cond = type >= T_SPRING;

        DWORD all = DIEP_ALLPARAMS;
        DWORD changed = fresh ? all : flags;
        if (changed & (DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_DIRECTION | DIEP_AXES))
            if (!sendTypeSpecific(type, off1, off2, e, g, gain)) return DIERR_INVALIDPARAM;
        if (!cond && (changed & (DIEP_ENVELOPE | DIEP_GAIN)))
            sendEnvelope(off2, e->lpEnvelope, gain);
        if (changed & (DIEP_DURATION | DIEP_DIRECTION | DIEP_AXES | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL |
                       DIEP_STARTDELAY))
            sendSetEffect(idx, type, off1, off2, e, g);

        Fx& f = g_dev.fx[idx];
        f.used = true;
        f.type = type;
        f.durMs = (DWORD)usToMs(e->dwDuration, 0xFFFE);
        *pHandle = (DWORD)(idx + 1);

        if (flags & DIEP_START) {
            effectOp(idx, OP_START, 1);
            f.playing = true;
            f.loops = 1;
            f.started = GetTickCount();
        }
        return S_OK;
    }
};

// ---------------------------------------------------------------- class factory
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* d = new EffectDriver();
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release();
        logf("CreateInstance -> %08lx", hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? InterlockedIncrement(&g_locks) : InterlockedDecrement(&g_locks);
        return S_OK;
    }
};
static Factory g_factory;

// ---------------------------------------------------------------- registration
static LONG setStr(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* val, REGSAM view = 0) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr);
    if (r) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}
static LONG setBin(HKEY root, const wchar_t* path, const wchar_t* name, const void* data, DWORD n) {
    HKEY k;
    LONG r = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r) return r;
    r = RegSetValueExW(k, name, 0, REG_BINARY, (const BYTE*)data, n);
    RegCloseKey(k);
    return r;
}

struct EffectReg {
    const GUID* guid;
    const wchar_t* name;
    DWORD id, effType, staticParams, dynamicParams;
};

static void guidStr(const GUID& g, wchar_t* out) { StringFromGUID2(g, out, 40); }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH], key[512];
    GetModuleFileNameW(g_hinst, path, MAX_PATH);

    // COM class (the 32-bit DLL lands in WOW6432Node automatically).
    swprintf(key, 512, L"CLSID\\%ls", kClsidStr);
    LONG r = setStr(HKEY_CLASSES_ROOT, key, nullptr, L"Afterburner FFB effect driver");
    swprintf(key, 512, L"CLSID\\%ls\\InProcServer32", kClsidStr);
    r |= setStr(HKEY_CLASSES_ROOT, key, nullptr, path);
    r |= setStr(HKEY_CLASSES_ROOT, key, L"ThreadingModel", L"Both");

    // Joystick OEM force feedback registration (shared by 32 and 64 bit).
    swprintf(key, 512, L"%ls\\OEMForceFeedback", kOemKey);
    r |= setStr(HKEY_LOCAL_MACHINE, key, L"CLSID", kClsidStr);
    DWORD devAttr[3] = {0, 1000, 1000};  // flags, sample period us, min time resolution us
    r |= setBin(HKEY_LOCAL_MACHINE, key, L"Attributes", devAttr, sizeof(devAttr));

    const DWORD posFlags = DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY;
    const DWORD condFlags = DIEFT_SATURATION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION | DIEFT_DEADBAND |
                            DIEFT_STARTDELAY;
    const DWORD sp = DIEP_DURATION | DIEP_GAIN | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES |
                     DIEP_DIRECTION | DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    const DWORD dp = DIEP_DURATION | DIEP_GAIN | DIEP_DIRECTION | DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS;
    const DWORD csp = sp & ~DIEP_ENVELOPE, cdp = DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS;
    const EffectReg effects[] = {
        {&GUID_ConstantForce, L"Constant Force", T_CONST, DIEFT_CONSTANTFORCE | posFlags, sp, dp},
        {&GUID_Square, L"Square Wave", T_SQUARE, DIEFT_PERIODIC | posFlags, sp, dp},
        {&GUID_Triangle, L"Triangle Wave", T_TRIANGLE, DIEFT_PERIODIC | posFlags, sp, dp},
        {&GUID_Sine, L"Sine Wave", T_SINE, DIEFT_PERIODIC | posFlags, sp, dp},
        {&GUID_SawtoothUp, L"Sawtooth Up Wave", T_SAWUP, DIEFT_PERIODIC | posFlags, sp, dp},
        {&GUID_SawtoothDown, L"Sawtooth Down Wave", T_SAWDOWN, DIEFT_PERIODIC | posFlags, sp, dp},
        {&GUID_Spring, L"Spring Force", T_SPRING, DIEFT_CONDITION | condFlags, csp, cdp},
        {&GUID_Damper, L"Damper Force", T_DAMPER, DIEFT_CONDITION | condFlags, csp, cdp},
    };
    for (const auto& ef : effects) {
        wchar_t g[40];
        guidStr(*ef.guid, g);
        swprintf(key, 512, L"%ls\\OEMForceFeedback\\Effects\\%ls", kOemKey, g);
        r |= setStr(HKEY_LOCAL_MACHINE, key, nullptr, ef.name);
        DWORD attr[5] = {ef.id, ef.effType, ef.staticParams, ef.dynamicParams, DIEFF_CARTESIAN | DIEFF_POLAR};
        r |= setBin(HKEY_LOCAL_MACHINE, key, L"Attributes", attr, sizeof(attr));
    }

    // Mark X and Y as force feedback actuators (flags, usage page, usage) + (max force, resolution).
    struct { DWORD flags; WORD page, usage; } axX = {DIDOI_FFACTUATOR | DIDOI_ASPECTPOSITION | DIDOI_POLLED, 1, 0x30},
                                              axY = {DIDOI_FFACTUATOR | DIDOI_ASPECTPOSITION | DIDOI_POLLED, 1, 0x31};
    DWORD ffAttr[2] = {10, 256};
    swprintf(key, 512, L"%ls\\Axes\\0", kOemKey);
    r |= setBin(HKEY_LOCAL_MACHINE, key, L"Attributes", &axX, sizeof(axX));
    r |= setBin(HKEY_LOCAL_MACHINE, key, L"FFAttributes", ffAttr, sizeof(ffAttr));
    swprintf(key, 512, L"%ls\\Axes\\1", kOemKey);
    r |= setBin(HKEY_LOCAL_MACHINE, key, L"Attributes", &axY, sizeof(axY));
    r |= setBin(HKEY_LOCAL_MACHINE, key, L"FFAttributes", ffAttr, sizeof(ffAttr));

    // DirectInput caches the OEM type info per user; a stale copy hides our registration.
    RegDeleteTreeW(HKEY_CURRENT_USER, kUserOemKey);
    return r ? SELFREG_E_CLASS : S_OK;
}

STDAPI DllUnregisterServer() {
    wchar_t key[512];
    swprintf(key, 512, L"CLSID\\%ls", kClsidStr);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
    swprintf(key, 512, L"%ls\\OEMForceFeedback", kOemKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
    swprintf(key, 512, L"%ls\\Axes", kOemKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, key);
    RegDeleteTreeW(HKEY_CURRENT_USER, kUserOemKey);
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    logf("DllGetClassObject");
    if (clsid != CLSID_AfbFFB) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_locks ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        wchar_t tmp[MAX_PATH], file[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        swprintf(file, MAX_PATH, L"%lsafbffb.log", tmp);
        g_log = _wfopen(file, L"a");
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        logf("loaded into %ls", exe);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_dev.h != INVALID_HANDLE_VALUE) CloseHandle(g_dev.h);
        if (g_log) fclose(g_log);
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
