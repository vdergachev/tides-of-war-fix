#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "display.h"
#include "iat.h"
#include "log.h"
#include "patch.h"
#include "scale.h"

/* Supported game entries: fullscreen preference and fullscreen mode setup. */
#define FULLSCREEN_PROFILE_ADDRESS 0x408350
#define FULLSCREEN_WRITE_ADDRESS   0x4083d0
#define STARTUP_PROFILE_CALL       0x4275f8
#define ENTER_FULLSCREEN_ADDRESS   0x4279e0

static const uint8_t FULLSCREEN_PROFILE_BYTES[7] = {0x6a, 0xff, 0x68, 0x18, 0x58, 0x49, 0x00};
static const uint8_t ENTER_FULLSCREEN_BYTES[7] = {0x81, 0xec, 0x9c, 0x00, 0x00, 0x00, 0x56};
static const uint8_t STARTUP_PROFILE_BYTES[5] = {0xe8, 0x53, 0x0d, 0xfe, 0xff};
/* Both entries have no stack arguments; return zero in EAX with a plain ret. */
static const uint8_t RETURN_FALSE_BYTES[3] = {0x31, 0xc0, 0xc3};

typedef uint8_t(__fastcall *fullscreen_profile_fn)(void *);
/* CWinApp::WriteProfileString inside needs ECX = app: fastcall(app, unused EDX, value), callee pops 4. */
typedef void(__fastcall *fullscreen_write_fn)(void *, void *, int);

typedef LONG(WINAPI *change_display_settings_fn)(DEVMODEA *, DWORD);

static change_display_settings_fn g_change_display_settings;
static LONG g_temporary_logged;
static int g_borderless_requested;
static void *g_app;

/* ECX=app, no stack arguments, plain ret; the original reader has the same ABI.
 * Return AL=0 so startup never enters the game's fullscreen path. */
static uint8_t __fastcall startup_fullscreen_profile(void *app)
{
    fullscreen_profile_fn read_profile = (fullscreen_profile_fn)FULLSCREEN_PROFILE_ADDRESS;
    g_app = app;
    g_borderless_requested = read_profile(app) != 0;
    scaling_set_borderless(g_borderless_requested);
    return 0;
}

/* No stack arguments, plain ret, AL=0. Preserve C callee-saved registers.
 * The profile writer takes ECX = app and one 4-byte stack slot (only its low byte is used). */
static uint8_t __cdecl toggle_borderless_fullscreen(void)
{
    g_borderless_requested = !g_borderless_requested;
    scaling_set_borderless(g_borderless_requested);
    if (g_app) {
        fullscreen_write_fn write_profile = (fullscreen_write_fn)FULLSCREEN_WRITE_ADDRESS;
        write_profile(g_app, NULL, g_borderless_requested);
    } else {
        log_line("display: preference not saved (application object unknown)");
    }
    log_line("display: borderless fullscreen toggled %s", g_borderless_requested ? "on" : "off");
    return 0;
}

static LONG WINAPI temporary_display_settings(DEVMODEA *devmode, DWORD flags)
{
    if (devmode && !(flags & CDS_TEST) && !(flags & CDS_UPDATEREGISTRY)) {
        flags |= CDS_FULLSCREEN;
        DWORD error = GetLastError();
        if (InterlockedCompareExchange(&g_temporary_logged, 1, 0) == 0) {
            log_line("display: mode change made temporary (CDS_FULLSCREEN)");
        }
        SetLastError(error);
    }
    return g_change_display_settings(devmode, flags);
}

static void install_temporary_display(void)
{
    DWORD *slot = iat_find_slot("user32.dll", "ChangeDisplaySettingsA");
    if (!slot || !*slot) {
        log_line("display: ChangeDisplaySettingsA import missing (skipped)");
        return;
    }

    g_change_display_settings = (change_display_settings_fn)(uintptr_t)*slot;
    int result = iat_replace(slot, (void *)(uintptr_t)g_change_display_settings,
                             (void *)(uintptr_t)temporary_display_settings);
    if (result != IAT_OK) {
        log_line("display: ChangeDisplaySettingsA IAT patch failed (reason %d)", result);
        return;
    }
    log_line("display: ChangeDisplaySettingsA IAT installed");
}

static void disable_fullscreen(void)
{
    void *profile = (void *)FULLSCREEN_PROFILE_ADDRESS;
    void *enter = (void *)ENTER_FULLSCREEN_ADDRESS;
    if (!patch_check(profile, FULLSCREEN_PROFILE_BYTES, sizeof(FULLSCREEN_PROFILE_BYTES)) ||
        !patch_check(enter, ENTER_FULLSCREEN_BYTES, sizeof(ENTER_FULLSCREEN_BYTES))) {
        log_line("display: fullscreen entry verification failed (skipped)");
        return;
    }

    if (!patch_write(profile, FULLSCREEN_PROFILE_BYTES, RETURN_FALSE_BYTES, sizeof(RETURN_FALSE_BYTES))) {
        log_line("display: fullscreen preference patch failed");
        return;
    }
    if (!patch_write(enter, ENTER_FULLSCREEN_BYTES, RETURN_FALSE_BYTES, sizeof(RETURN_FALSE_BYTES))) {
        log_line("display: fullscreen mode patch failed");
        if (!patch_write(profile, RETURN_FALSE_BYTES, FULLSCREEN_PROFILE_BYTES, sizeof(RETURN_FALSE_BYTES))) {
            log_line("display: fullscreen preference rollback failed");
        }
        return;
    }
    log_line("display: fullscreen disabled while scaling");
}

static void enable_borderless_fullscreen(void)
{
    void *enter = (void *)ENTER_FULLSCREEN_ADDRESS;
    void *startup = (void *)STARTUP_PROFILE_CALL;
    if (!patch_check(enter, ENTER_FULLSCREEN_BYTES, sizeof(ENTER_FULLSCREEN_BYTES)) ||
        !patch_check(startup, STARTUP_PROFILE_BYTES, sizeof(STARTUP_PROFILE_BYTES))) {
        log_line("display: fullscreen entry verification failed (skipped)");
        return;
    }
    if (!patch_jmp_padded(enter, ENTER_FULLSCREEN_BYTES, sizeof(ENTER_FULLSCREEN_BYTES),
                          (void *)(uintptr_t)toggle_borderless_fullscreen)) {
        log_line("display: borderless fullscreen patch failed");
        return;
    }
    uint8_t hooked[sizeof(ENTER_FULLSCREEN_BYTES)];
    memcpy(hooked, enter, sizeof(hooked));
    if (!patch_call(startup, STARTUP_PROFILE_BYTES, (void *)(uintptr_t)startup_fullscreen_profile)) {
        log_line("display: startup fullscreen call patch failed");
        if (!patch_write(enter, hooked, ENTER_FULLSCREEN_BYTES, sizeof(hooked))) {
            log_line("display: fullscreen entry rollback failed");
        }
        return;
    }
    log_line("display: independent borderless fullscreen hooks installed");
}

void display_apply(int borderless_fullscreen, int restore_on_exit)
{
    log_line("display: fullscreen mode %s", borderless_fullscreen ? "borderless" : "off");
    if (restore_on_exit) {
        install_temporary_display();
    }
    if (!scaling_active()) {
        return;
    }
    if (borderless_fullscreen) {
        enable_borderless_fullscreen();
    } else {
        disable_fullscreen();
    }
}
