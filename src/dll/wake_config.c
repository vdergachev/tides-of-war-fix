#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "wake_config.h"
#include "log.h"
#include "patch.h"

/* Length is in model units (hull length is about 0.11), strokes per world pixel. */
static const int OARS_PER_SIDE = 3;
static const double OAR_LENGTH = .035;
static const double OAR_AMPLITUDE_DEG = 30;
static const double OAR_STROKES_PER_UNIT = .08;

/* Original emitter's lookup: expected CALL 0x489360 at 0x489c0c. */
#define RIPPLE_FOLLOW_CALL 0x489c0c
#define CAMERA_FOLLOW_FN   0x489360
#define OBJECT_TYPE        0x48
static const BYTE RIPPLE_FOLLOW_BYTES[] = {0xe8, 0x4f, 0xf7, 0xff, 0xff};

/* Written once before the render thread starts. */
static wake_config g_wake;
static boat_oars_config g_oars;
static DWORD g_follow_fn = CAMERA_FOLLOW_FN;

static double read_number(const char *path, const char *key, double fallback)
{
    char value[64];
    GetPrivateProfileStringA("boat_oars", key, "", value, sizeof(value), path);
    if (!value[0]) {
        return fallback;
    }
    char *end;
    double result = strtod(value, &end);
    return end != value && !*end && isfinite(result) ? result : -1;
}

static void configure_oars(const char *path)
{
    g_oars.enabled = GetPrivateProfileIntA("boat_oars", "enabled", 1, path) != 0;
    g_oars.per_side = (int)GetPrivateProfileIntA("boat_oars", "per_side", OARS_PER_SIDE, path);
    g_oars.length = read_number(path, "length", OAR_LENGTH);
    g_oars.amplitude_deg = read_number(path, "amplitude_deg", OAR_AMPLITUDE_DEG);
    g_oars.strokes_per_unit = read_number(path, "strokes_per_unit", OAR_STROKES_PER_UNIT);
    if (g_oars.per_side < 1 || g_oars.per_side > 8 || g_oars.length < .005 || g_oars.length > .1 ||
        g_oars.amplitude_deg < 0 || g_oars.amplitude_deg > 75 || g_oars.strokes_per_unit < .001 ||
        g_oars.strokes_per_unit > 2) {
        g_oars.enabled = 0;
        log_line("boat_oars: invalid settings; disabled");
    }
    log_line("[boat_oars] enabled=%d per_side=%d length=%.4f amplitude_deg=%.2f strokes_per_unit=%.4f",
             g_oars.enabled, g_oars.per_side, g_oars.length, g_oars.amplitude_deg, g_oars.strokes_per_unit);
}

void wake_configure(const char *path)
{
    g_wake.enabled = GetPrivateProfileIntA("wake", "enabled", 1, path) != 0;
    g_wake.boats = GetPrivateProfileIntA("wake", "boats", 0, path) != 0;
    configure_oars(path);
    g_wake.land = GetPrivateProfileIntA("wake", "land", 1, path) != 0;
    g_wake.damping = (int)GetPrivateProfileIntA("wake", "damping", 4, path);
    g_wake.strength = (int)GetPrivateProfileIntA("wake", "strength", 127, path);
    g_wake.resolution = (int)GetPrivateProfileIntA("wake", "resolution", 2, path);
    g_wake.budget = (int)GetPrivateProfileIntA("wake", "budget", 1000, path);
    char mode[32];
    GetPrivateProfileStringA("wake", "budget_mode", "per-ship", mode, sizeof(mode), path);
    g_wake.shared = strcmp(mode, "shared") == 0;
    if (g_wake.damping < 1 || g_wake.damping > 8 || g_wake.strength < 1 || g_wake.strength > 32767 ||
        (g_wake.resolution != 1 && g_wake.resolution != 2 && g_wake.resolution != 4) || g_wake.budget < 1 ||
        g_wake.budget > 100000 || (!g_wake.shared && strcmp(mode, "per-ship") != 0)) {
        g_wake.enabled = 0;
        log_line("wake: invalid settings; disabled");
    }
    log_line("[wake] enabled=%d boats=%d land=%d damping=%d strength=%d resolution=%d budget=%d "
             "budget_mode=%s own=game",
             g_wake.enabled, g_wake.boats, g_wake.land, g_wake.damping, g_wake.strength, g_wake.resolution,
             g_wake.budget, mode);
}

wake_config wake_settings(void)
{
    return g_wake;
}

boat_oars_config boat_oars_settings(void)
{
    return g_oars;
}

/* Only the original ripple emitter's follow lookup is filtered. Water alignment,
 * propagation, rendering, and the user's Ship Wake option remain untouched. */
static DWORD __stdcall filter_boat(DWORD object)
{
    DWORD error = GetLastError();
    __try {
        if (object) {
            unsigned type = *(unsigned *)(object + OBJECT_TYPE);
            if (type == 9 || type == 10) {
                object = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Preserve the original result on an unknown object. */
    }
    SetLastError(error);
    return object;
}

// clang-format off
/* CALL replacement: ECX is camera, no arguments, EAX is object or NULL.
 * Original lookup executes once in native code. Preserve its flags/GPR/FPU/SSE
 * result except EAX, which the emitter already tests for NULL. */
__declspec(naked) static void ripple_follow(void)
{
    __asm {
        call dword ptr [g_follow_fn]
        pushfd
        pushad
        mov ebx, esp
        sub esp, 528
        and esp, -16
        fxsave [esp]
        push eax
        call filter_boat
        mov [ebx + 28], eax
        fxrstor [esp]
        mov esp, ebx
        popad
        popfd
        ret
    }
}

// clang-format on

void wake_apply(void)
{
    if (!g_wake.boats) {
        if (!patch_call((void *)RIPPLE_FOLLOW_CALL, RIPPLE_FOLLOW_BYTES, (void *)ripple_follow)) {
            log_line("wake: boat ripple filter failed (unexpected bytes or patch write failed)");
        } else {
            log_line("wake: original boat ripple emission disabled");
        }
    }
}
