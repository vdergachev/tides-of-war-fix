#include <windows.h>
#include <string.h>
#include "game_folder.h"
#include "game.h"
#include "patch.h"
#include "log.h"

/* Game folder helper and imported CString constructor in the supported executable. */
#define GAME_FOLDER_ADDRESS 0x42d470

static const uint8_t FOLDER_ENTRY_BYTES[] = {0x6a, 0xff, 0x68, 0x5b, 0x7f, 0x49, 0x00};

static char g_dir[MAX_PATH];

static void *__cdecl game_folder_replacement(void *out)
{
    return game_cstring_construct(out, g_dir);
}

int game_folder_apply(const char *dir)
{
    if (strlen(dir) >= sizeof(g_dir)) {
        log_line("game_folder: failed: directory too long");
        return 0;
    }
    if (!patch_check((void *)GAME_FOLDER_ADDRESS, FOLDER_ENTRY_BYTES, sizeof(FOLDER_ENTRY_BYTES)) ||
        !game_cstring_check()) {
        log_line("game_folder: failed: unexpected code bytes");
        return 0;
    }
    strcpy_s(g_dir, sizeof(g_dir), dir);
    if (!patch_jmp((void *)GAME_FOLDER_ADDRESS, FOLDER_ENTRY_BYTES,
                   (void *)(uintptr_t)game_folder_replacement)) {
        log_line("game_folder: failed: code patch failed");
        return 0;
    }
    log_line("game_folder: directory %s", g_dir);
    log_line("game_folder: ok");
    return 1;
}
