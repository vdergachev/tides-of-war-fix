#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "patch.h"
#include "log.h"
#include "game_folder.h"
#include "saves.h"
#include "registry.h"
#include "startup_warning.h"
#include "game_speed.h"
#include "draw_sync.h"
#include "idle_wait.h"
#include "d3d_fix.h"
#include "scale.h"
#include "display.h"
#include "d3d11_out.h"
#include "../common/sha256.h"

#define TOW_FIX_VERSION "0.15.0"

/* Supported game image and CRT entry point, before any game initialization. */
#define GAME_IMAGE_BASE    0x400000
#define GAME_ENTRY_RVA     0x94d10
#define GAME_ENTRY_ADDRESS 0x494d10

static const uint8_t ENTRY_BYTES[5] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
static uint8_t g_original[5];
static uint8_t g_hook[5];
static DWORD g_entry = GAME_ENTRY_ADDRESS;

static int hash_text_section(char hash[65])
{
    int found = 0;
    HMODULE base = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)((BYTE *)base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text\0\0\0", 8) == 0) {
            DWORD size = sec[i].Misc.VirtualSize;
            if (size > sec[i].SizeOfRawData) {
                size = sec[i].SizeOfRawData;
            }
            if (sec[i].VirtualAddress > nt->OptionalHeader.SizeOfImage ||
                size > nt->OptionalHeader.SizeOfImage - sec[i].VirtualAddress) {
                break;
            }
            found = sha256_memory((BYTE *)base + sec[i].VirtualAddress, size, hash);
            break;
        }
    }
    return found;
}

static int accept_game_version(void)
{
    char hash[65];

    if (!hash_text_section(hash)) {
        log_line("exe version rejected: cannot hash .text");
        return 0;
    }

    log_line("exe .text SHA-256: %s", hash);
    if (!strcmp(hash, "a84e1f6f6481e6ef70d6f7a1cc2222192c42443a87e8f8c990167e228f5f6cfa")) {
        log_line("exe version accepted: original");
    } else if (!strcmp(hash, "0d92125629563c7d95fcd7769973a4de2fef80316bceee45e0aecaaebd4777f2")) {
        log_line("exe version accepted: cd-patched");
    } else {
        log_line("exe version rejected");
        return 0;
    }
    return 1;
}

static int open_game_log(char exe[MAX_PATH], char dir[MAX_PATH])
{
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        OutputDebugStringA("tow_fix: executable path too long\n");
        return 0;
    }
    strcpy_s(dir, MAX_PATH, exe);
    char *slash = strrchr(dir, '\\');
    if (!slash) {
        return 0;
    }
    slash[1] = 0;
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.log") > MAX_PATH) {
        return 0;
    }
    strcpy_s(path, MAX_PATH, dir);
    strcat_s(path, MAX_PATH, "tow_fix.log");
    if (!log_open(path)) {
        OutputDebugStringA("tow_fix: cannot open log; continuing without log");
    }
    log_line("tow_fix " TOW_FIX_VERSION " loaded, exe %s", exe);
    return 1;
}

typedef struct {
    int game_folder;
    int saves;
    int registry;
    int startup_warning;
    int game_speed;
    int draw_sync;
    int idle_wait;
    int max_wait_ms;
    int rgb_device;
    int white_diffuse;
    DWORD untextured_color;
    int borderless_fullscreen;
    int restore_on_exit;
} tow_settings;

static int read_settings(const char *dir, tow_settings *settings)
{
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.ini") > sizeof(path)) {
        log_line("ini path too long");
        return 0;
    }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), "tow_fix.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        log_line("tow_fix.ini missing; all fixes off");
    }
    settings->game_folder = (int)GetPrivateProfileIntA("game_folder", "enabled", 0, path);
    log_line("[game_folder] enabled=%d", settings->game_folder);
    settings->saves = (int)GetPrivateProfileIntA("save_redirect", "enabled", 0, path);
    log_line("[save_redirect] enabled=%d", settings->saves);
    settings->registry = (int)GetPrivateProfileIntA("registry_guard", "enabled", 0, path);
    log_line("[registry_guard] enabled=%d", settings->registry);
    settings->startup_warning = (int)GetPrivateProfileIntA("startup_warning", "enabled", 0, path);
    log_line("[startup_warning] enabled=%d", settings->startup_warning);
    settings->game_speed = (int)GetPrivateProfileIntA("game_speed", "percent", 100, path);
    log_line("[game_speed] percent=%d", settings->game_speed);
    settings->draw_sync = (int)GetPrivateProfileIntA("draw_sync", "enabled", 0, path);
    log_line("[draw_sync] enabled=%d", settings->draw_sync);
    settings->idle_wait = (int)GetPrivateProfileIntA("idle_wait", "enabled", 0, path);
    log_line("[idle_wait] enabled=%d", settings->idle_wait);
    settings->max_wait_ms = (int)GetPrivateProfileIntA("idle_wait", "max_wait_ms", 10, path);
    if (settings->max_wait_ms < 1) {
        settings->max_wait_ms = 1;
    } else if (settings->max_wait_ms > 100) {
        settings->max_wait_ms = 100;
    }
    log_line("[idle_wait] max_wait_ms=%d", settings->max_wait_ms);
    settings->rgb_device = (int)GetPrivateProfileIntA("rgb_device", "enabled", 0, path);
    log_line("[rgb_device] enabled=%d", settings->rgb_device);
    settings->white_diffuse = (int)GetPrivateProfileIntA("white_diffuse", "enabled", 0, path);
    log_line("[white_diffuse] enabled=%d", settings->white_diffuse);
    settings->untextured_color = GetPrivateProfileIntA("white_diffuse", "untextured_color", 0x503820, path);
    if (settings->untextured_color > 0xffffffUL) {
        log_line("white_diffuse: invalid untextured_color; using 0x503820");
        settings->untextured_color = 0x503820;
    }
    log_line("[white_diffuse] untextured_color=0x%06lx", settings->untextured_color);
    char fullscreen[32];
    GetPrivateProfileStringA("scaling", "fullscreen", "borderless", fullscreen, sizeof(fullscreen), path);
    settings->borderless_fullscreen = _stricmp(fullscreen, "off") != 0;
    if (_stricmp(fullscreen, "borderless") != 0 && _stricmp(fullscreen, "off") != 0) {
        log_line("display: invalid fullscreen setting (using borderless)");
    }
    settings->restore_on_exit = (int)GetPrivateProfileIntA("display", "restore_on_exit", 0, path);
    log_line("[display] restore_on_exit=%d", settings->restore_on_exit);
    return 1;
}

static void __cdecl tow_init(void)
{
    if (!patch_write((void *)GAME_ENTRY_ADDRESS, g_hook, g_original, 5)) {
        OutputDebugStringA("tow_fix: cannot restore entry point; terminating to avoid recursion\n");
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }

    char dir[MAX_PATH];
    char exe[MAX_PATH];
    if (!open_game_log(exe, dir)) {
        return;
    }

    tow_settings settings;
    if (!read_settings(dir, &settings)) {
        return;
    }
    if (!accept_game_version()) {
        return;
    }
    log_line("game_folder: %s", settings.game_folder ? "enabled" : "disabled");
    if (settings.game_folder) {
        game_folder_apply(dir);
    }
    log_line("save_redirect: %s", settings.saves ? "enabled" : "disabled");
    if (settings.saves) {
        save_redirect_apply(dir);
    }
    log_line("registry_guard: %s", settings.registry ? "enabled" : "disabled");
    if (settings.registry) {
        registry_guard_apply(dir);
    }
    log_line("startup_warning: %s", settings.startup_warning ? "enabled" : "disabled");
    if (settings.startup_warning) {
        startup_warning_apply();
    }
    game_speed_apply(settings.game_speed);
    log_line("draw_sync: %s", settings.draw_sync ? "enabled" : "disabled");
    if (settings.draw_sync) {
        draw_sync_apply();
    }
    log_line("idle_wait: %s", settings.idle_wait ? "enabled" : "disabled");
    if (settings.idle_wait) {
        idle_wait_apply((unsigned)settings.max_wait_ms);
    }
    log_line("rgb_device: %s", settings.rgb_device ? "enabled" : "disabled");
    log_line("white_diffuse: %s", settings.white_diffuse ? "enabled" : "disabled");
    if (settings.rgb_device || settings.white_diffuse) {
        d3d_fix_apply(settings.rgb_device, settings.white_diffuse, settings.untextured_color);
    }
    d3d11_out_configure(dir);
    scaling_apply(dir);
    display_apply(settings.borderless_fullscreen, settings.restore_on_exit);
}

/* Entry: original loader stack/registers. Save all GPRs and flags across tow_init;
 * exit: restore them and tail-jump to the restored CRT entry with the same ESP. */
// clang-format off
__declspec(naked) static void entry_thunk(void)
{
    __asm {
        pushfd
        pushad
        call tow_init
        popad
        popfd
        jmp dword ptr [g_entry]
    }
}

// clang-format on

void __stdcall TowFixImport(void)
{
}

static int install_entry_hook(BYTE *base)
{
    __try {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) {
            __leave;
        }
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.AddressOfEntryPoint != GAME_ENTRY_RVA) {
            __leave;
        }
        if (!patch_check((void *)GAME_ENTRY_ADDRESS, ENTRY_BYTES, 5)) {
            __leave;
        }
        memcpy(g_original, ENTRY_BYTES, 5);
        if (!patch_jmp((void *)GAME_ENTRY_ADDRESS, ENTRY_BYTES, (void *)(uintptr_t)entry_thunk)) {
            __leave;
        }
        memcpy(g_hook, (void *)GAME_ENTRY_ADDRESS, 5);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(instance);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    if (base != (BYTE *)GAME_IMAGE_BASE) {
        OutputDebugStringA("tow_fix: unexpected image base\n");
        return TRUE;
    }
    if (install_entry_hook(base)) {
        return TRUE;
    }
    OutputDebugStringA("tow_fix: entry hook rejected (PE, entry bytes or protection)\n");
    return TRUE;
}
