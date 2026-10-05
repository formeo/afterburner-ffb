// fftest.cpp - minimal DirectInput 8 force feedback tester.
// Uses the joystick exactly the way a game does, so it exercises the effect driver.
#define DIRECTINPUT_VERSION 0x0800
#define _USE_MATH_DEFINES
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <cmath>

static IDirectInput8W* g_di;
static IDirectInputDevice8W* g_dev;
static GUID g_found;
static bool g_haveFF;

static BOOL CALLBACK enumAll(LPCDIDEVICEINSTANCEW d, LPVOID) {
    wprintf(L"  game controller: %ls\n", d->tszProductName);
    return DIENUM_CONTINUE;
}
static BOOL CALLBACK enumFF(LPCDIDEVICEINSTANCEW d, LPVOID) {
    wprintf(L"  force feedback device: %ls\n", d->tszProductName);
    g_found = d->guidInstance;
    g_haveFF = true;
    return DIENUM_STOP;
}
static BOOL CALLBACK enumEffects(LPCDIEFFECTINFOW e, LPVOID) {
    wprintf(L"  effect: %ls (type 0x%lx)\n", e->tszName, e->dwEffType);
    return DIENUM_CONTINUE;
}

static void check(const char* what, HRESULT hr) { printf("  %-28s -> 0x%08lx %s\n", what, hr, SUCCEEDED(hr) ? "ok" : "FAILED"); }

// Run one effect for `seconds`. `update` is called at ~50 Hz with elapsed time for live changes.
static void run(const GUID& type, DIEFFECT& eff, double seconds, void (*update)(IDirectInputEffect*, DIEFFECT&, double) = nullptr) {
    IDirectInputEffect* fx = nullptr;
    HRESULT hr = g_dev->CreateEffect(type, &eff, &fx, nullptr);
    check("CreateEffect", hr);
    if (FAILED(hr)) return;
    check("Start", fx->Start(1, 0));
    DWORD t0 = GetTickCount();
    while ((GetTickCount() - t0) / 1000.0 < seconds) {
        if (update) update(fx, eff, (GetTickCount() - t0) / 1000.0);
        Sleep(20);
    }
    check("Stop", fx->Stop());
    fx->Release();
}

static LONG g_mag = 6000;

static void baseEffect(DIEFFECT& e, DWORD* axes, LONG* dir, DWORD durMs) {
    ZeroMemory(&e, sizeof(e));
    e.dwSize = sizeof(DIEFFECT);
    e.dwFlags = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
    e.dwDuration = durMs == INFINITE ? INFINITE : durMs * 1000;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 2;
    e.rgdwAxes = axes;
    e.rglDirection = dir;
}

static DICONSTANTFORCE g_cf;
static void liveConstant(IDirectInputEffect* fx, DIEFFECT& e, double t) {
    g_cf.lMagnitude = (LONG)(g_mag * sin(2 * M_PI * 0.5 * t));
    e.cbTypeSpecificParams = sizeof(g_cf);
    e.lpvTypeSpecificParams = &g_cf;
    fx->SetParameters(&e, DIEP_TYPESPECIFICPARAMS);
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) g_mag = _wtoi(argv[1]);
    printf("fftest: magnitude %ld of 10000 (pass a number to change)\n", g_mag);

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"fftest", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    check("DirectInput8Create", DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                                   (void**)&g_di, nullptr));
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumAll, nullptr, DIEDFL_ATTACHEDONLY);
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumFF, nullptr, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (!g_haveFF) {
        printf("\nNo force feedback device found. DirectInput does not see the effect driver registration.\n");
        return 1;
    }
    check("CreateDevice", g_di->CreateDevice(g_found, &g_dev, nullptr));
    check("SetDataFormat", g_dev->SetDataFormat(&c_dfDIJoystick2));
    check("SetCooperativeLevel", g_dev->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND));
    DIPROPDWORD ac = {{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, DIPROPAUTOCENTER_OFF};
    check("Autocenter off", g_dev->SetProperty(DIPROP_AUTOCENTER, &ac.diph));
    check("Acquire", g_dev->Acquire());
    g_dev->EnumEffects(enumEffects, nullptr, DIEFT_ALL);

    DWORD axes[2] = {DIJOFS_X, DIJOFS_Y};
    for (;;) {
        printf("\n1 constant, sweep 0/90/180/270 deg   2 constant, live update   3 sine 200 ms\n"
               "4 square 1000 ms   5 spring   6 damper   0 quit\n> ");
        int choice = 0;
        if (scanf("%d", &choice) != 1 || choice == 0) break;
        LONG dir[2] = {0, 0};
        DIEFFECT e;
        if (choice == 1) {
            DICONSTANTFORCE cf = {g_mag};
            for (LONG angle = 0; angle < 36000; angle += 9000) {
                printf(" direction %ld deg\n", angle / 100);
                baseEffect(e, axes, dir, 1000);
                dir[0] = angle;
                e.cbTypeSpecificParams = sizeof(cf);
                e.lpvTypeSpecificParams = &cf;
                run(GUID_ConstantForce, e, 1.3);
                Sleep(500);
            }
        } else if (choice == 2) {
            baseEffect(e, axes, dir, INFINITE);
            dir[0] = 9000;
            g_cf.lMagnitude = 0;
            e.cbTypeSpecificParams = sizeof(g_cf);
            e.lpvTypeSpecificParams = &g_cf;
            run(GUID_ConstantForce, e, 6, liveConstant);
        } else if (choice == 3 || choice == 4) {
            DIPERIODIC p = {(DWORD)g_mag, 0, 0, (DWORD)(choice == 3 ? 200000 : 1000000)};
            baseEffect(e, axes, dir, 2000);
            dir[0] = 9000;
            e.cbTypeSpecificParams = sizeof(p);
            e.lpvTypeSpecificParams = &p;
            run(choice == 3 ? GUID_Sine : GUID_Square, e, 2.2);
        } else if (choice == 5 || choice == 6) {
            DICONDITION c[2] = {};
            for (auto& x : c) {
                x.lPositiveCoefficient = x.lNegativeCoefficient = g_mag;
                x.dwPositiveSaturation = x.dwNegativeSaturation = 10000;
            }
            baseEffect(e, axes, dir, 5000);
            e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            e.cbTypeSpecificParams = sizeof(c);
            e.lpvTypeSpecificParams = c;
            printf(" push the stick around for 5 s\n");
            run(choice == 5 ? GUID_Spring : GUID_Damper, e, 5.2);
        }
    }
    g_dev->SendForceFeedbackCommand(DISFFC_RESET);
    g_dev->Unacquire();
    g_dev->Release();
    g_di->Release();
    return 0;
}
