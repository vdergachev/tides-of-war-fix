#include <windows.h>
#include <string.h>
#include "wake_config.h"
#include "log.h"

/* Written once before the render thread starts. */
static wake_config g_wake;

void wake_configure(const char *path)
{
    g_wake.enabled = GetPrivateProfileIntA("wake", "enabled", 1, path) != 0;
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
    log_line(
        "[wake] enabled=%d land=%d damping=%d strength=%d resolution=%d budget=%d budget_mode=%s own=game",
        g_wake.enabled, g_wake.land, g_wake.damping, g_wake.strength, g_wake.resolution, g_wake.budget, mode);
}

wake_config wake_settings(void)
{
    return g_wake;
}
