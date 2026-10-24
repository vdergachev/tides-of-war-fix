#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "registry.h"
#include "iat.h"
#include "game.h"
#include "saves.h"
#include "patch.h"
#include "log.h"

/* Verified InitInstance call and MFC 6 CWinApp layout. */
#define PROFILE_SETUP_ADDRESS    0x407759
#define PROFILE_CALL_ADDRESS     0x407760
#define PROFILE_NEXT_ADDRESS     0x407765
#define SET_REGISTRY_KEY_ADDRESS 0x494400
#define APP_INSTANCE_OFFSET      0x6c
#define APP_NAME_OFFSET          0x7c
#define APP_REGISTRY_OFFSET      0x80
#define APP_EXE_OFFSET           0x8c
#define APP_PROFILE_OFFSET       0x94
static const uint8_t PROFILE_SETUP_BYTES[] = {0x8b, 0x4c, 0x24, 0x08, 0x51, 0x8b, 0xce};
static const uint8_t PROFILE_CALL_BYTES[] = {0xe8, 0x9b, 0xcc, 0x08, 0x00};
static const uint8_t PROFILE_NEXT_BYTES[] = {0xe8, 0x90, 0xcc, 0x08, 0x00};
static const uint8_t SET_REGISTRY_KEY_BYTES[] = {0xff, 0x25, 0x38, 0x02, 0x4a, 0x00};
static const char PROFILE_KEY[] = "Software\\Tides of War\\Tides of War";

typedef char *(__cdecl *crt_strdup_fn)(const char *);
typedef void(__cdecl *crt_free_fn)(void *);
typedef void(__fastcall *set_registry_key_fn)(void *, void *, const char *);

static crt_strdup_fn g_duplicate;
static crt_free_fn g_free;
static char g_settings[MAX_PATH];
static LONG g_blocked_lines;

/* Read only through the terminator, including near page boundaries. Caller uses SEH. */
static int read_string(const char *source, char output[MAX_PATH])
{
    if (!source) {
        return 0;
    }
    for (size_t i = 0; i < MAX_PATH; ++i) {
        char value = ((const volatile char *)source)[i];
        output[i] = value;
        if (!value) {
            return 1;
        }
    }
    return 0;
}

static LONG WINAPI blocked_reg_set_value(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data,
                                         DWORD size)
{
    (void)key;
    (void)reserved;
    (void)type;
    (void)data;
    (void)size;
    LONG count;
    do {
        count = InterlockedCompareExchange(&g_blocked_lines, 0, 0);
        if (count >= 20) {
            return ERROR_SUCCESS;
        }
    } while (InterlockedCompareExchange(&g_blocked_lines, count + 1, count) != count);
    {
        char text[MAX_PATH];
        strcpy_s(text, sizeof(text), "<unreadable>");
        __try {
            if (!name) {
                strcpy_s(text, sizeof(text), "<default>");
            } else if (!read_string(name, text)) {
                strcpy_s(text, sizeof(text), "<too long>");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strcpy_s(text, sizeof(text), "<unreadable>");
        }
        log_line("registry_guard: blocked RegSetValueExA %s", text);
    }
    return ERROR_SUCCESS;
}

static int block_registry_write(void)
{
    DWORD *slot = iat_find_slot("ADVAPI32.dll", "RegSetValueExA");
    HMODULE module = GetModuleHandleA("advapi32.dll");
    FARPROC original = module ? GetProcAddress(module, "RegSetValueExA") : NULL;
    if (!slot || !original || *slot != (DWORD)(uintptr_t)original) {
        log_line("registry_guard: IAT failed: missing import or unknown state");
        return 0;
    }

    int result = iat_replace(slot, (void *)(uintptr_t)original, (void *)(uintptr_t)blocked_reg_set_value);
    if (result != IAT_OK) {
        const char *reason = result == IAT_NOT_WRITABLE   ? "cannot make slot writable"
                             : result == IAT_NOT_RESTORED ? "cannot restore protection"
                                                          : "missing import or unknown state";
        log_line("registry_guard: IAT failed: %s", reason);
        return 0;
    }
    log_line("registry_guard: IAT ok");
    return 1;
}

static int profile_skipped(const char *reason)
{
    log_line("registry_guard: profile switch skipped: %s", reason);
    return 0;
}

static int switch_profile_to_ini(void *app)
{
    __try {
        BYTE *object = (BYTE *)app;
        if (!object || *(uintptr_t *)(object + APP_INSTANCE_OFFSET) != GAME_IMAGE_BASE) {
            return profile_skipped("unexpected m_hInstance");
        }
        if (*(void **)(object + APP_REGISTRY_OFFSET)) {
            return profile_skipped("m_pszRegistryKey is not NULL");
        }

        char app_name[MAX_PATH];
        char exe_name[MAX_PATH];
        char old_name[MAX_PATH];
        char **profile = (char **)(object + APP_PROFILE_OFFSET);
        if (!read_string(*(char **)(object + APP_NAME_OFFSET), app_name) ||
            strcmp(app_name, "Tides of War") != 0) {
            return profile_skipped("unexpected m_pszAppName");
        }
        if (!read_string(*(char **)(object + APP_EXE_OFFSET), exe_name) || _stricmp(exe_name, "TOW") != 0) {
            return profile_skipped("unexpected m_pszExeName");
        }
        if (!read_string(*profile, old_name)) {
            return profile_skipped("unreadable or long m_pszProfileName");
        }
        size_t length = strlen(old_name);
        if (length < 4 || _stricmp(old_name + length - 4, ".ini") != 0) {
            return profile_skipped("unexpected m_pszProfileName");
        }

        char *replacement = g_duplicate(g_settings);
        if (!replacement) {
            return profile_skipped("msvcrt _strdup failed");
        }
        g_free(*profile);
        *profile = replacement;
        log_line("registry_guard: profile %s -> %s", old_name, g_settings);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return profile_skipped("exception reading CWinApp or replacing profile");
    }
}

/* ECX=app, EDX unused, key on stack; fastcall pops key like the original thiscall. */
static void __fastcall set_registry_key_replacement(void *app, void *unused_edx, const char *key)
{
    (void)unused_edx;
    if (switch_profile_to_ini(app)) {
        return;
    }
    ((set_registry_key_fn)SET_REGISTRY_KEY_ADDRESS)(app, NULL, key);
}

static int profile_bytes_check(void)
{
    return patch_check((void *)PROFILE_SETUP_ADDRESS, PROFILE_SETUP_BYTES, sizeof(PROFILE_SETUP_BYTES)) &&
           patch_check((void *)PROFILE_CALL_ADDRESS, PROFILE_CALL_BYTES, sizeof(PROFILE_CALL_BYTES)) &&
           patch_check((void *)PROFILE_NEXT_ADDRESS, PROFILE_NEXT_BYTES, sizeof(PROFILE_NEXT_BYTES)) &&
           patch_check((void *)SET_REGISTRY_KEY_ADDRESS, SET_REGISTRY_KEY_BYTES,
                       sizeof(SET_REGISTRY_KEY_BYTES));
}

static int migrate_section(HKEY section, const char *section_name)
{
    DWORD max_name;
    DWORD max_data;
    if (RegQueryInfoKeyA(section, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &max_name, &max_data, NULL,
                         NULL) != ERROR_SUCCESS ||
        max_name == MAXDWORD || max_data == MAXDWORD) {
        return 0;
    }

    char *name = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)max_name + 1);
    BYTE *data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)max_data + 1);
    if (!name || !data) {
        if (name) {
            HeapFree(GetProcessHeap(), 0, name);
        }
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
        }
        return 0;
    }

    int ok = 1;
    for (DWORD index = 0;; ++index) {
        DWORD name_size = max_name + 1;
        DWORD data_size = max_data;
        DWORD type;
        LONG result = RegEnumValueA(section, index, name, &name_size, NULL, &type, data, &data_size);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            ok = 0;
            break;
        }

        char decimal[16];
        const char *text;
        if (type == REG_SZ) {
            data[data_size] = 0;
            text = (char *)data;
        } else if (type == REG_DWORD && data_size == sizeof(DWORD)) {
            DWORD value;
            memcpy(&value, data, sizeof(value));
            sprintf_s(decimal, sizeof(decimal), "%lu", value);
            text = decimal;
        } else {
            continue;
        }

        if (!WritePrivateProfileStringA(section_name, name, text, g_settings)) {
            ok = 0;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
    HeapFree(GetProcessHeap(), 0, name);
    return ok;
}

static int migrate_sections(HKEY root)
{
    /* Registry subkey names are limited to 255 characters. */
    char name[256];
    for (DWORD index = 0;; ++index) {
        DWORD length = sizeof(name);
        LONG result = RegEnumKeyExA(root, index, name, &length, NULL, NULL, NULL, NULL);
        if (result == ERROR_NO_MORE_ITEMS) {
            return 1;
        }
        if (result != ERROR_SUCCESS) {
            return 0;
        }

        HKEY section;
        if (RegOpenKeyExA(root, name, 0, KEY_READ, &section) != ERROR_SUCCESS) {
            return 0;
        }
        int ok = migrate_section(section, name);
        RegCloseKey(section);
        if (!ok) {
            return 0;
        }
    }
}

static int migrate_settings(void)
{
    DWORD attributes = GetFileAttributesA(g_settings);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        log_line("registry_guard: settings already exist: %s", g_settings);
        return !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return 0;
    }

    HKEY root;
    LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, PROFILE_KEY, 0, KEY_READ, &root);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
        log_line("registry_guard: settings migration: registry key missing");
        return 1;
    }
    if (result != ERROR_SUCCESS) {
        return 0;
    }

    /* Reserve a new file so an existing INI is never migrated over. */
    HANDLE file = CreateFileA(g_settings, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD create_error = GetLastError();
        RegCloseKey(root);
        return create_error == ERROR_FILE_EXISTS;
    }
    CloseHandle(file);
    int ok = migrate_sections(root);
    RegCloseKey(root);
    log_line("registry_guard: settings migration %s", ok ? "ok" : "failed");
    return ok;
}

static int redirect_profile(const char *dir)
{
    if (!profile_bytes_check()) {
        log_line("registry_guard: profile failed: unexpected code bytes");
        return 0;
    }

    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    g_duplicate = crt ? (crt_strdup_fn)(uintptr_t)GetProcAddress(crt, "_strdup") : NULL;
    g_free = crt ? (crt_free_fn)(uintptr_t)GetProcAddress(crt, "free") : NULL;
    if (!g_duplicate || !g_free) {
        log_line("registry_guard: profile failed: missing msvcrt exports");
        return 0;
    }

    char saves[MAX_PATH];
    int created;
    if (!saves_prepare(dir, saves, &created) || strlen(saves) + sizeof("settings.ini") > sizeof(g_settings)) {
        log_line("registry_guard: profile failed: cannot prepare settings path");
        return 0;
    }
    strcpy_s(g_settings, sizeof(g_settings), saves);
    strcat_s(g_settings, sizeof(g_settings), "settings.ini");
    if (!migrate_settings()) {
        log_line("registry_guard: profile failed: settings migration failed");
        return 0;
    }

    if (!patch_call((void *)PROFILE_CALL_ADDRESS, PROFILE_CALL_BYTES,
                    (void *)(uintptr_t)set_registry_key_replacement)) {
        log_line("registry_guard: profile failed: code patch failed");
        return 0;
    }
    log_line("registry_guard: profile ok");
    return 1;
}

void registry_guard_apply(const char *dir)
{
    block_registry_write();
    redirect_profile(dir);
}
