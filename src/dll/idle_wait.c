#include <windows.h>
#include <string.h>
#include "idle_wait.h"
#include "patch.h"
#include "log.h"

/* Empty-queue draw check, its JZ, and the next idle iteration. */
#define DRAW_PENDING_ADDRESS 0x4082b9
#define IDLE_BRANCH_ADDRESS  0x4082be
#define IDLE_RESUME_ADDRESS  0x4081f8
#define REPORT_INTERVAL_MS   60000
static const uint8_t DRAW_PENDING_BYTES[] = {0x8a, 0x45, 0xef, 0x84, 0xc0, 0x0f,
                                             0x84, 0x34, 0xff, 0xff, 0xff};
static const uint8_t IDLE_BRANCH_BYTES[] = {0x0f, 0x84, 0x34, 0xff, 0xff, 0xff};
static const uint8_t IDLE_RESUME_BYTES[] = {0x8a, 0x45, 0xee, 0x84, 0xc0};

static DWORD g_resume = IDLE_RESUME_ADDRESS;
static unsigned g_max_wait_ms;
static DWORD g_last_report;
static unsigned g_waits;
static unsigned g_timeouts;
static DWORD g_waited_ms;

/* UI thread only; preserve the game's last-error value through stats and logging. */
static void __stdcall idle_wait_block(void)
{
    DWORD error = GetLastError();
    DWORD start = GetTickCount();
    DWORD result = MsgWaitForMultipleObjectsEx(0, NULL, g_max_wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    DWORD now = GetTickCount();

    ++g_waits;
    if (result == WAIT_TIMEOUT) {
        ++g_timeouts;
    }
    g_waited_ms += (DWORD)(now - start);

    if ((DWORD)(now - g_last_report) >= REPORT_INTERVAL_MS) {
        log_line("idle_wait: waits %u, timeouts %u, waited %lu ms of 60000", g_waits, g_timeouts,
                 g_waited_ms);
        g_last_report = now;
        g_waits = 0;
        g_timeouts = 0;
        g_waited_ms = 0;
    }
    SetLastError(error);
}

/* Entry: taken JZ, no return address pushed; EBP=Run frame, draw-pending=0.
 * EBX (idle count), ESI (app), EDI and EBP are live; the C callee preserves them.
 * EAX/ECX/EDX and flags are dead. CALL/RET balances ESP; exit tail-jumps to
 * the idle iteration with the original ESP, skipping the wait if bIdle is set. */
// clang-format off
__declspec(naked) static void idle_wait_thunk(void)
{
    __asm {
        cmp byte ptr [ebp - 0x12], 0
        jne resume
        call idle_wait_block
    resume:
        jmp dword ptr [g_resume]
    }
}

// clang-format on

int idle_wait_apply(unsigned max_wait_ms)
{
    if (!patch_check((void *)DRAW_PENDING_ADDRESS, DRAW_PENDING_BYTES, sizeof(DRAW_PENDING_BYTES)) ||
        !patch_check((void *)IDLE_RESUME_ADDRESS, IDLE_RESUME_BYTES, sizeof(IDLE_RESUME_BYTES))) {
        log_line("idle_wait: failed: unexpected code bytes");
        return 0;
    }

    g_max_wait_ms = max_wait_ms;
    g_last_report = GetTickCount();
    uint8_t bytes[sizeof(IDLE_BRANCH_BYTES)] = {0x0f, 0x84};
    uint32_t rel = (uint32_t)(uintptr_t)idle_wait_thunk - IDLE_BRANCH_ADDRESS - sizeof(bytes);
    memcpy(bytes + 2, &rel, sizeof(rel));
    if (!patch_write((void *)IDLE_BRANCH_ADDRESS, IDLE_BRANCH_BYTES, bytes, sizeof(bytes))) {
        log_line("idle_wait: failed: code patch failed");
        return 0;
    }

    log_line("idle_wait: ok, max wait %u ms", g_max_wait_ms);
    return 1;
}
