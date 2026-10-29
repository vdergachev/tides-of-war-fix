#include <windows.h>
#include <mmsystem.h>
#include <intrin.h>
#include <stdint.h>
#include "timer_guard.h"
#include "iat.h"
#include "patch.h"
#include "log.h"
#include "cargo_pause.h"

/* winmm callbacks of the frame object (dwUser): step posts 0x8001, draw posts 0x8002. */
#define STEP_CALLBACK_ADDRESS 0x446ed0
#define DRAW_CALLBACK_ADDRESS 0x446f00

/* Step re-arm after Game_Step and App_Run's empty-queue PeekMessageA (return address). */
#define STEP_REARM_ADDRESS 0x44718e
#define RUN_PEEK_ADDRESS   0x408235
#define RUN_PEEK_RETURN    0x40823b

/* TOW.exe IAT slots: WINMM timeSetEvent, USER32 PeekMessageA. */
#define TIME_SET_EVENT_SLOT 0x4a0888
#define PEEK_MESSAGE_SLOT   0x4a074c

/* Game state: step timer flag (byte), network game flag (byte, 0 = single player). */
#define STEP_FLAG_ADDRESS    0x4ce2a8
#define NETWORK_GAME_ADDRESS 0x4cdd55

/* Frame object fields: HWND, stored step ID, timer resolution, step delay. */
#define OWNER_HWND_OFFSET       0x20
#define OWNER_STEP_ID_OFFSET    0xcc
#define OWNER_RESOLUTION_OFFSET 0xf0
#define OWNER_STEP_DELAY_OFFSET 0xf4
#define STEP_MESSAGE            0x8001

/* Watchdog: stall threshold max(4 x step delay, 500 ms); backoff after a failed re-arm. */
#define STALL_DELAY_FACTOR 4
#define STALL_MIN_MS       500
#define STEP_DELAY_MAX_MS  1000
#define RETRY_BACKOFF_MS   100

/* A live draw event older than max(16 x delay, 2000 ms) is assumed lost. */
#define DRAW_EXPIRE_FACTOR 16
#define DRAW_EXPIRE_MIN_MS 2000
#define DRAW_CREATING      (-1L)

#define REPORT_INTERVAL_MS 60000
#define LOG_LIMIT          20

/* Callback prologues: mov eax,[esp+0Ch]; mov dword [eax+0CCh/0D0h],0. */
static const uint8_t STEP_CALLBACK_BYTES[] = {0x8b, 0x44, 0x24, 0x0c, 0xc7, 0x80, 0xcc,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t DRAW_CALLBACK_BYTES[] = {0x8b, 0x44, 0x24, 0x0c, 0xc7, 0x80, 0xd0,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* call [timeSetEvent]; mov [esi+0CCh],eax and call [PeekMessageA]. */
static const uint8_t STEP_REARM_BYTES[] = {0xff, 0x15, 0x88, 0x08, 0x4a, 0x00,
                                           0x89, 0x86, 0xcc, 0x00, 0x00, 0x00};
static const uint8_t RUN_PEEK_BYTES[] = {0xff, 0x15, 0x4c, 0x07, 0x4a, 0x00};

typedef MMRESULT(WINAPI *time_set_fn)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef BOOL(WINAPI *peek_fn)(LPMSG, HWND, UINT, UINT, UINT);

typedef struct {
    HWND hwnd;
    UINT step_id;
    UINT resolution;
    UINT delay;
    BYTE step_flag;
    BYTE network;
} owner_state;

static time_set_fn g_time_set;
static peek_fn g_peek;
static int g_poll_installed;
static int g_watchdog_enabled;

/* Draw slot: 0 = none, DRAW_CREATING, or the live winmm ID. Shared with the winmm thread. */
static LONG volatile g_draw_live;

/* Game thread only (all draw requests come from UI-thread handlers). */
static DWORD_PTR g_draw_user;
static DWORD g_draw_born;

/* Frame object from step requests and tick of the last step request. */
static PVOID volatile g_owner;
static LONG volatile g_step_last;

/* Game thread only: watchdog backoff, report and log limits. */
static DWORD g_retry_after;
static DWORD g_last_report;
static DWORD g_log_window;
static unsigned g_logged;
static unsigned g_suppressed;

static LONG g_draw_requests;
static LONG g_draw_reused;
static LONG g_draw_created;
static LONG g_draw_expired;
static LONG g_step_rearmed;
static LONG g_step_failures;
static LONG g_step_retried;

/* Game thread only; at most LOG_LIMIT event lines per 60 s, the rest is counted. */
static int log_allowed(void)
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_log_window) >= REPORT_INTERVAL_MS) {
        g_log_window = now;
        g_logged = 0;
    }

    if (g_logged >= LOG_LIMIT) {
        ++g_suppressed;
        return 0;
    }
    ++g_logged;
    return 1;
}

/* winmm thread. Mark the slot free before the game callback can post 0x8002,
 * so a request made while handling that message creates the next event. */
static void CALLBACK draw_callback(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2)
{
    if (InterlockedCompareExchange(&g_draw_live, 0, (LONG)id) != (LONG)id) {
        /* Fired before timeSetEvent returned: the creator will see the slot free. */
        InterlockedCompareExchange(&g_draw_live, 0, DRAW_CREATING);
    }

    LPTIMECALLBACK original = (LPTIMECALLBACK)(uintptr_t)DRAW_CALLBACK_ADDRESS;
    original(id, msg, user, dw1, dw2);
}

static int draw_expired(DWORD now, UINT delay)
{
    DWORD limit = DRAW_EXPIRE_FACTOR * delay;
    if (limit < DRAW_EXPIRE_MIN_MS) {
        limit = DRAW_EXPIRE_MIN_MS;
    }
    return (DWORD)(now - g_draw_born) > limit;
}

static MMRESULT draw_create(UINT delay, UINT resolution, DWORD_PTR user, DWORD now)
{
    InterlockedExchange(&g_draw_live, DRAW_CREATING);
    g_draw_user = user;
    g_draw_born = now;

    MMRESULT id = g_time_set(delay, resolution, draw_callback, user, TIME_ONESHOT);
    if (!id) {
        InterlockedCompareExchange(&g_draw_live, 0, DRAW_CREATING);
        return 0;
    }

    InterlockedIncrement(&g_draw_created);
    InterlockedCompareExchange(&g_draw_live, (LONG)id, DRAW_CREATING);
    return id;
}

/* Keep at most one live draw event; the game never kills or checks draw IDs. */
static MMRESULT draw_request(UINT delay, UINT resolution, DWORD_PTR user)
{
    InterlockedIncrement(&g_draw_requests);
    DWORD now = GetTickCount();
    LONG live = InterlockedCompareExchange(&g_draw_live, 0, 0);

    if (live != 0 && live != DRAW_CREATING && user == g_draw_user) {
        if (!draw_expired(now, delay)) {
            InterlockedIncrement(&g_draw_reused);
            return (MMRESULT)live;
        }
        InterlockedIncrement(&g_draw_expired);
        if (log_allowed()) {
            log_line("timer_guard: draw event %ld expired after %lu ms; creating a new one", live,
                     (DWORD)(now - g_draw_born));
        }
    }

    return draw_create(delay, resolution, user, now);
}

/* Step events pass through unchanged; a failure is retried once at once. */
static MMRESULT step_request(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user)
{
    InterlockedExchangePointer(&g_owner, (PVOID)user);
    InterlockedExchange(&g_step_last, (LONG)GetTickCount());

    MMRESULT id = g_time_set(delay, resolution, callback, user, TIME_ONESHOT);
    if (id) {
        return id;
    }

    InterlockedIncrement(&g_step_failures);
    id = g_time_set(delay, resolution, callback, user, TIME_ONESHOT);
    if (id) {
        InterlockedIncrement(&g_step_retried);
    }
    if (log_allowed()) {
        log_line("timer_guard: step timeSetEvent failed (delay %u), retry %s", delay, id ? "ok" : "failed");
    }
    return id;
}

/* IAT hook of timeSetEvent in TOW.exe. Only one-shot step/draw events are touched. */
static MMRESULT WINAPI guard_time_set(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user,
                                      UINT flags)
{
    if (flags != TIME_ONESHOT || !user) {
        return g_time_set(delay, resolution, callback, user, flags);
    }

    DWORD error = GetLastError();
    MMRESULT id = 0;
    if ((uintptr_t)callback == DRAW_CALLBACK_ADDRESS) {
        id = draw_request(delay, resolution, user);
    } else if ((uintptr_t)callback == STEP_CALLBACK_ADDRESS) {
        id = step_request(delay, resolution, callback, user);
    } else {
        id = g_time_set(delay, resolution, callback, user, flags);
    }

    SetLastError(error);
    return id;
}

static int read_owner(const BYTE *owner, owner_state *state)
{
    __try {
        state->step_flag = *(const BYTE *)(uintptr_t)STEP_FLAG_ADDRESS;
        state->network = *(const BYTE *)(uintptr_t)NETWORK_GAME_ADDRESS;
        state->hwnd = *(HWND const *)(owner + OWNER_HWND_OFFSET);
        state->step_id = *(const UINT *)(owner + OWNER_STEP_ID_OFFSET);
        state->resolution = *(const UINT *)(owner + OWNER_RESOLUTION_OFFSET);
        state->delay = *(const UINT *)(owner + OWNER_STEP_DELAY_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static void write_step_id(BYTE *owner, UINT id)
{
    __try {
        *(UINT *)(owner + OWNER_STEP_ID_OFFSET) = id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

/* Single-player step chain is dead: flag set, no stored ID, no queued 0x8001. */
static int step_stalled(const owner_state *state, DWORD now)
{
    if (!state->step_flag || state->network || state->step_id || !state->hwnd) {
        return 0;
    }
    if (!state->delay || state->delay > STEP_DELAY_MAX_MS) {
        return 0;
    }

    DWORD limit = STALL_DELAY_FACTOR * state->delay;
    if (limit < STALL_MIN_MS) {
        limit = STALL_MIN_MS;
    }
    DWORD last = (DWORD)InterlockedCompareExchange(&g_step_last, 0, 0);
    if ((DWORD)(now - last) <= limit || (LONG)(now - g_retry_after) < 0) {
        return 0;
    }

    MSG pending;
    return !g_peek(&pending, state->hwnd, STEP_MESSAGE, STEP_MESSAGE, PM_NOREMOVE | PM_NOYIELD);
}

/* Same call and store as 0x44718E, through the real winmm function; no game code runs here. */
static void step_rearm(BYTE *owner, const owner_state *state, DWORD now)
{
    DWORD stalled = now - (DWORD)InterlockedExchange(&g_step_last, (LONG)now);
    LPTIMECALLBACK callback = (LPTIMECALLBACK)(uintptr_t)STEP_CALLBACK_ADDRESS;
    MMRESULT id = g_time_set(state->delay, state->resolution, callback, (DWORD_PTR)owner, TIME_ONESHOT);

    if (!id) {
        InterlockedIncrement(&g_step_failures);
        g_retry_after = now + RETRY_BACKOFF_MS;
        if (log_allowed()) {
            log_line("timer_guard: step re-arm failed after %lu ms stall; retry in %u ms", stalled,
                     RETRY_BACKOFF_MS);
        }
        return;
    }

    write_step_id(owner, id);
    InterlockedIncrement(&g_step_rearmed);
    if (log_allowed()) {
        log_line("timer_guard: step re-armed id=%u delay=%u after %lu ms stall", id, state->delay, stalled);
    }
}

static void step_watch(DWORD now)
{
    BYTE *owner = (BYTE *)InterlockedCompareExchangePointer(&g_owner, NULL, NULL);
    if (!owner) {
        return;
    }

    owner_state state;
    if (!read_owner(owner, &state) || !step_stalled(&state, now)) {
        return;
    }
    step_rearm(owner, &state, now);
}

static void report(DWORD now)
{
    if ((DWORD)(now - g_last_report) < REPORT_INTERVAL_MS) {
        return;
    }

    g_last_report = now;
    log_line("timer_guard: draw_requests %ld, draw_reused %ld, draw_created %ld, draw_expired %ld, "
             "step_rearmed %ld, step_failures %ld, step_retried %ld, log_suppressed %u",
             InterlockedExchange(&g_draw_requests, 0), InterlockedExchange(&g_draw_reused, 0),
             InterlockedExchange(&g_draw_created, 0), InterlockedExchange(&g_draw_expired, 0),
             InterlockedExchange(&g_step_rearmed, 0), InterlockedExchange(&g_step_failures, 0),
             InterlockedExchange(&g_step_retried, 0), g_suppressed);
    g_suppressed = 0;
}

/* IAT hook of PeekMessageA. The watchdog runs only from App_Run's top-level
 * empty-queue check: no message handler (and no Game_Step) is on the stack. */
static BOOL WINAPI guard_peek(LPMSG msg, HWND hwnd, UINT first, UINT last, UINT remove)
{
    if (_ReturnAddress() == (void *)(uintptr_t)RUN_PEEK_RETURN) {
        DWORD error = GetLastError();
        DWORD now = GetTickCount();
        cargo_pause_poll();
        if (g_watchdog_enabled) {
            step_watch(now);
            report(now);
        }
        SetLastError(error);
    }
    return g_peek(msg, hwnd, first, last, remove);
}

static int code_matches(void)
{
    return patch_check((void *)STEP_CALLBACK_ADDRESS, STEP_CALLBACK_BYTES, sizeof(STEP_CALLBACK_BYTES)) &&
           patch_check((void *)DRAW_CALLBACK_ADDRESS, DRAW_CALLBACK_BYTES, sizeof(DRAW_CALLBACK_BYTES)) &&
           patch_check((void *)STEP_REARM_ADDRESS, STEP_REARM_BYTES, sizeof(STEP_REARM_BYTES)) &&
           patch_check((void *)RUN_PEEK_ADDRESS, RUN_PEEK_BYTES, sizeof(RUN_PEEK_BYTES));
}

/* Shared UI poll also works with timer_guard disabled; no timer policy is enabled. */
int timer_guard_ui_poll_apply(void)
{
    if (g_poll_installed) {
        return 1;
    }
    DWORD *slot = iat_find_slot("user32.dll", "PeekMessageA");
    if (!patch_check((void *)RUN_PEEK_ADDRESS, RUN_PEEK_BYTES, sizeof(RUN_PEEK_BYTES)) ||
        slot != (DWORD *)PEEK_MESSAGE_SLOT || !*slot) {
        log_line("ui_poll: failed: unexpected PeekMessageA call or import");
        return 0;
    }
    g_peek = (peek_fn)(uintptr_t)*slot;
    int result = iat_replace(slot, (void *)(uintptr_t)g_peek, (void *)(uintptr_t)guard_peek);
    if (result != IAT_OK) {
        log_line("ui_poll: failed: PeekMessageA hook result=%d", result);
        return 0;
    }
    g_poll_installed = 1;
    return 1;
}

int timer_guard_apply(void)
{
    DWORD *set_slot = iat_find_slot("winmm.dll", "timeSetEvent");
    DWORD *peek_slot = iat_find_slot("user32.dll", "PeekMessageA");
    if (!code_matches() || set_slot != (DWORD *)TIME_SET_EVENT_SLOT ||
        peek_slot != (DWORD *)PEEK_MESSAGE_SLOT || !*set_slot || !*peek_slot) {
        log_line("timer_guard: failed: unexpected code bytes or imports");
        return 0;
    }

    g_time_set = (time_set_fn)(uintptr_t)*set_slot;
    g_peek = (peek_fn)(uintptr_t)*peek_slot;
    g_last_report = GetTickCount();
    g_log_window = g_last_report;

    int result = iat_replace(set_slot, (void *)(uintptr_t)g_time_set, (void *)(uintptr_t)guard_time_set);
    if (result != IAT_OK) {
        log_line("timer_guard: failed: timeSetEvent hook result=%d", result);
        return 0;
    }

    g_watchdog_enabled = 1;
    if (!timer_guard_ui_poll_apply()) {
        log_line("timer_guard: ok without step watchdog");
        return 1;
    }

    log_line("timer_guard: ok");
    return 1;
}
