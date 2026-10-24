#include <windows.h>
#include "draw_sync.h"
#include "patch.h"
#include "log.h"

/* Step message handler, its game call, scene draw, and timer-owned state. */
#define STEP_HANDLER_ADDRESS  0x447160
#define STEP_SETUP_ADDRESS    0x44716c
#define STEP_CALL_ADDRESS     0x447173
#define STEP_FUNCTION_ADDRESS 0x42ae70
#define DRAW_FUNCTION_ADDRESS 0x42b110
#define GAME_OBJECT_ADDRESS   0x4cdcc0
#define CURSOR_ADDRESS        0x4ce298
#define UNDRAWN_STATE_ADDRESS 0x4ce05c
#define RENDER_TIMER_ADDRESS  0x4ce2ac
#define OWNER_DWELL_OFFSET    0xc0
static const uint8_t STEP_HANDLER_BYTES[] = {0xa0, 0xa8, 0xe2, 0x4c, 0x00, 0x56, 0x84, 0xc0, 0x8b, 0xf1};
static const uint8_t STEP_SETUP_BYTES[] = {0x6a, 0x00, 0xb9, 0xc0, 0xdc, 0x4c, 0x00};
static const uint8_t STEP_CALL_BYTES[] = {0xe8, 0xf8, 0x3c, 0xfe, 0xff};
static const uint8_t DRAW_FUNCTION_BYTES[] = {0x83, 0xec, 0x10, 0x53, 0x55, 0x56, 0x8b, 0xf1};

static DWORD g_step_fn = STEP_FUNCTION_ADDRESS;
static DWORD g_draw_fn = DRAW_FUNCTION_ADDRESS;
static DWORD g_last_report;
static unsigned g_synced;
static unsigned g_skipped;

/* UI thread only; reads game state without calling game code. */
static int __stdcall draw_sync_decide(const BYTE *owner)
{
    int dwell = -1;
    if (*(const BYTE *)UNDRAWN_STATE_ADDRESS == 0 || *(const BYTE *)RENDER_TIMER_ADDRESS == 0) {
        ++g_skipped;
    } else {
        ++g_synced;
        dwell = *(const WORD *)(owner + OWNER_DWELL_OFFSET) > 8 ? 1 : 0;
    }

    DWORD now = GetTickCount();
    if ((DWORD)(now - g_last_report) >= 60000) {
        log_line("draw_sync: synced %u, skipped %u", g_synced, g_skipped);
        g_last_report = now;
        g_synced = 0;
        g_skipped = 0;
    }
    return dwell;
}

/* Entry: [ESP]=return to handler, [ESP+4]=step argument; ESI=owner, ECX=game.
 * All callees preserve ESI; exit pops the original argument as the step did. */
// clang-format off
__declspec(naked) static void draw_sync_thunk(void)
{
    __asm {
        push dword ptr [esp + 4]
        mov ecx, GAME_OBJECT_ADDRESS
        call dword ptr [g_step_fn]
        /* Step popped the copied argument; [ESP]=return, [ESP+4]=original argument. */
        push esi
        call draw_sync_decide
        /* Decide popped owner; same stack, EAX=dwell or -1, ESI=owner. */
        cmp eax, -1
        je done
        push CURSOR_ADDRESS
        push eax
        mov ecx, GAME_OBJECT_ADDRESS
        call dword ptr [g_draw_fn]
        /* Draw popped both arguments; same stack, ESI=owner, EAX is volatile. */
    done:
        ret 4
    }
}

// clang-format on

int draw_sync_apply(void)
{
    if (!patch_check((void *)STEP_HANDLER_ADDRESS, STEP_HANDLER_BYTES, sizeof(STEP_HANDLER_BYTES)) ||
        !patch_check((void *)STEP_SETUP_ADDRESS, STEP_SETUP_BYTES, sizeof(STEP_SETUP_BYTES)) ||
        !patch_check((void *)STEP_CALL_ADDRESS, STEP_CALL_BYTES, sizeof(STEP_CALL_BYTES)) ||
        !patch_check((void *)DRAW_FUNCTION_ADDRESS, DRAW_FUNCTION_BYTES, sizeof(DRAW_FUNCTION_BYTES))) {
        log_line("draw_sync: failed: unexpected code bytes");
        return 0;
    }

    g_last_report = GetTickCount();
    if (!patch_call((void *)STEP_CALL_ADDRESS, STEP_CALL_BYTES, (void *)(uintptr_t)draw_sync_thunk)) {
        log_line("draw_sync: failed: code patch failed");
        return 0;
    }

    log_line("draw_sync: ok");
    return 1;
}
