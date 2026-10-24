#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "saves.h"
#include "game.h"
#include "patch.h"
#include "log.h"

/* Original stdcall save-folder helper. */
#define SAVE_FOLDER_ADDRESS 0x408fd0
static const uint8_t SAVE_FOLDER_BYTES[] = {0x6a, 0xff, 0x68, 0xaf, 0x5a, 0x49, 0x00};
static const char OLD_SAVE_SUFFIX[] = "Program Files\\Devil's Thumb Entertainment\\Tides of War\\Save\\";
static char g_save_dir[MAX_PATH];

int saves_prepare(const char *dir, char path[MAX_PATH], int *created)
{
    *created = 0;
    if (strlen(dir) + sizeof("saves\\") > MAX_PATH) {
        return 0;
    }

    strcpy_s(path, MAX_PATH, dir);
    strcat_s(path, MAX_PATH, "saves\\");
    if (CreateDirectoryA(path, NULL)) {
        *created = 1;
        return 1;
    }

    DWORD error = GetLastError();
    DWORD attributes = GetFileAttributesA(path);
    return error == ERROR_ALREADY_EXISTS && attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned copy_old_saves(const char *folder)
{
    char pattern[MAX_PATH];
    if (strlen(folder) + sizeof("*.tow") > sizeof(pattern)) {
        return 0;
    }
    sprintf_s(pattern, sizeof(pattern), "%s*.tow", folder);

    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    unsigned count = 0;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        char source[MAX_PATH];
        char target[MAX_PATH];
        if (strlen(folder) + strlen(data.cFileName) >= sizeof(source) ||
            strlen(g_save_dir) + strlen(data.cFileName) >= sizeof(target)) {
            log_line("save_redirect: migration skipped: path too long");
            continue;
        }

        strcpy_s(source, sizeof(source), folder);
        strcat_s(source, sizeof(source), data.cFileName);
        strcpy_s(target, sizeof(target), g_save_dir);
        strcat_s(target, sizeof(target), data.cFileName);
        if (CopyFileA(source, target, TRUE)) {
            ++count;
            log_line("save_redirect: copied %s -> %s", source, target);
        } else if (GetLastError() != ERROR_FILE_EXISTS) {
            log_line("save_redirect: copy failed: %s (error %lu)", source, GetLastError());
        }
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return count;
}

static void migrate_saves(void)
{
    unsigned count = 0;

    /* Empty card readers must not show a "no disk" dialog. */
    UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    for (int drive = 'A'; drive <= 'Z'; ++drive) {
        char root[] = "A:\\";
        root[0] = (char)drive;
        UINT type = GetDriveTypeA(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            continue;
        }

        char folder[MAX_PATH];
        sprintf_s(folder, sizeof(folder), "%s%s", root, OLD_SAVE_SUFFIX);
        count += copy_old_saves(folder);
    }
    SetErrorMode(old_mode);

    char local[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    if (length && length < sizeof(local)) {
        char folder[MAX_PATH];
        if (strlen(local) + sizeof("\\VirtualStore\\") + strlen(OLD_SAVE_SUFFIX) <= sizeof(folder)) {
            sprintf_s(folder, sizeof(folder), "%s\\VirtualStore\\%s", local, OLD_SAVE_SUFFIX);
            count += copy_old_saves(folder);
        } else {
            log_line("save_redirect: migration skipped: VirtualStore path too long");
        }
    }
    log_line("save_redirect: migrated %u files", count);
}

/* Stack: output CString; stdcall returns out in EAX and pops four bytes. */
static void *__stdcall save_folder_replacement(void *out)
{
    CreateDirectoryA(g_save_dir, NULL);
    return game_cstring_construct(out, g_save_dir);
}

int save_redirect_apply(const char *dir)
{
    if (!patch_check((void *)SAVE_FOLDER_ADDRESS, SAVE_FOLDER_BYTES, sizeof(SAVE_FOLDER_BYTES)) ||
        !game_cstring_check()) {
        log_line("save_redirect: failed: unexpected code bytes");
        return 0;
    }

    int created;
    if (!saves_prepare(dir, g_save_dir, &created)) {
        log_line("save_redirect: failed: cannot create saves directory");
        return 0;
    }

    log_line("save_redirect: directory %s", g_save_dir);
    if (created) {
        migrate_saves();
    }
    if (!patch_jmp((void *)SAVE_FOLDER_ADDRESS, SAVE_FOLDER_BYTES,
                   (void *)(uintptr_t)save_folder_replacement)) {
        log_line("save_redirect: failed: code patch failed");
        return 0;
    }

    log_line("save_redirect: ok");
    return 1;
}
