#include "diag.h"
#include "log.h"
#include "iat.h"
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <tlhelp32.h>
#include <mmsystem.h>

/* Producers never wait for a logger or disk. Process exit deliberately owns cleanup. */
#define DIAG_LINES         256
#define DIAG_TEXT          1024
#define DIAG_LIMIT         3000
#define DRAW_STATE_ADDRESS 0x4cde48

/* Timer owner is the frame object (dwUser), not g_Game. */
#define TIMER_STEP_FLAG     0x4ce2a8
#define TIMER_DRAW_FLAG     0x4ce2ac
#define TIMER_STEP_CALLBACK 0x446ed0
#define TIMER_DRAW_CALLBACK 0x446f00
#define TIMER_HWND_OFFSET   0x20
#define TIMER_STEP_ID       0xcc
#define TIMER_DRAW_ID       0xd0

static const char *const g_names[] = {"cursor", "yesno", "masks", "ui", "transitions", "timers"};
static int g_enabled[DIAG_COUNT];
int g_diag_yesno_enabled;
static SRWLOCK g_lock = SRWLOCK_INIT;
static char g_lines[DIAG_LINES][DIAG_TEXT];
static LONG g_log_dropped;
static int g_writer_ready;
static unsigned g_head;
static unsigned g_count;
static unsigned g_events[DIAG_COUNT];
static unsigned g_window_events[DIAG_COUNT];
static unsigned g_capped[DIAG_COUNT];
static LONG g_dropped[DIAG_COUNT];
static DWORD g_summary[DIAG_COUNT];
static LONG g_ui_calls[8];
static const char *const g_ui_names[] = {"ShowWindow",          "SetWindowPos",    "BringWindowToTop",
                                         "SetForegroundWindow", "SetActiveWindow", "SetFocus",
                                         "EnableWindow",        "DestroyWindow"};
static char g_last[DIAG_COUNT][DIAG_TEXT];
/* Read/written only by the game's UI thread. Zero means no observed copy. */
static int g_copy_width;
static int g_copy_height;
/* Capture/hide are UI-owned; present is worker-owned. Slots are not shared. */
static diag_frame g_transition_last[3];
static DWORD g_transition_time[3];
static int g_transition_composed[3];
static int g_transition_seen[3];

/* Original callbacks are never wrapped or called by diagnostics. */
typedef MMRESULT(WINAPI *timer_set_fn)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef MMRESULT(WINAPI *timer_kill_fn)(UINT);
static timer_set_fn g_timer_set;
static timer_kill_fn g_timer_kill;
static PVOID volatile g_timer_owner;
static LONG g_timer_created[2];
static LONG g_timer_delivered[2];
static LONG g_timer_second[2];
static LONG g_timer_last[2];
static LONG g_timer_sets;
static LONG g_timer_failures;
static LONG g_timer_kills;
static LONG g_timer_stalls;
static LONG g_timer_draw_cancelled;

typedef struct {
    const void *caller;
    UINT delay;
    DWORD error;
    DWORD tick;
} timer_failure;

/* Protect snapshots only; never hold this lock while logging or calling the game. */
static SRWLOCK g_timer_failure_lock = SRWLOCK_INIT;
static timer_failure g_timer_failure[2];
static DWORD g_timer_failure_window;
static unsigned g_timer_failure_events;

typedef struct {
    HWND hwnd;
    unsigned step_flag;
    unsigned draw_flag;
    UINT step_id;
    UINT draw_id;
} timer_state;

static void timers_install(void);
static void timers_poll(DWORD now);
static void timers_message(const MSG *msg);
static LRESULT CALLBACK timers_messages(int code, WPARAM wp, LPARAM lp);
static void ui_watch_thread(DWORD thread);

static DWORD WINAPI diag_writer(void *parameter)
{
    (void)parameter;
    for (;;) {
        char line[DIAG_TEXT];
        int found = 0;
        AcquireSRWLockExclusive(&g_lock);
        if (g_count) {
            strcpy_s(line, sizeof(line), g_lines[g_head]);
            g_head = (g_head + 1) % DIAG_LINES;
            --g_count;
            found = 1;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (found) {
            log_line("%s", line);
        } else {
            Sleep(20);
        }
        LONG dropped = InterlockedExchange(&g_log_dropped, 0);
        if (dropped) {
            log_line("render_log dropped=%ld", dropped);
        }
        DWORD now = GetTickCount();
        if (g_enabled[DIAG_TIMERS]) {
            timers_poll(now);
        }
        for (unsigned i = 0; i < DIAG_COUNT; ++i) {
            unsigned events = 0;
            unsigned capped = 0;
            if (!g_enabled[i]) {
                continue;
            }
            AcquireSRWLockExclusive(&g_lock);
            if ((DWORD)(now - g_summary[i]) >= 60000) {
                g_summary[i] = now;
                events = g_events[i];
                capped = g_capped[i];
                g_window_events[i] = 0;
                strcpy_s(line, sizeof(line), g_last[i]);
            }
            ReleaseSRWLockExclusive(&g_lock);
            if (i == DIAG_UI && events) {
                for (unsigned api = 0; api < 8; ++api) {
                    log_line("diag_ui_summary api=%s calls_interval=%ld", g_ui_names[api],
                             InterlockedExchange(&g_ui_calls[api], 0));
                }
            }
            if (events) {
                log_line("diag_summary key=%s events=%u capped=%u dropped=%ld last={%s}", g_names[i], events,
                         capped, InterlockedCompareExchange(&g_dropped[i], 0, 0), line);
            }
        }
    }
}

void diag_configure(const char *dir)
{
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.ini") > sizeof(path)) {
        return;
    }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), "tow_fix.ini");
    int all = GetPrivateProfileIntA("diag", "all", 0, path) != 0;
    for (unsigned i = 0; i < DIAG_COUNT; ++i) {
        g_enabled[i] = all || GetPrivateProfileIntA("diag", g_names[i], 0, path) != 0;
        g_summary[i] = GetTickCount();
    }
    g_diag_yesno_enabled = g_enabled[DIAG_YESNO];
    /* The writer also handles ordinary render logs when diagnostics are off. */
    {
        HANDLE thread = CreateThread(NULL, 0, diag_writer, NULL, 0, NULL);
        if (thread) {
            g_writer_ready = 1;
            CloseHandle(thread);
        } else {
            memset(g_enabled, 0, sizeof(g_enabled));
            log_line("diag: writer unavailable error=%lu; diagnostics disabled", GetLastError());
        }
    }
    log_line("[diag] all=%d cursor=%d yesno=%d masks=%d ui=%d transitions=%d timers=%d", all,
             g_enabled[DIAG_CURSOR], g_enabled[DIAG_YESNO], g_enabled[DIAG_MASKS], g_enabled[DIAG_UI],
             g_enabled[DIAG_TRANSITIONS], g_enabled[DIAG_TIMERS]);
    if (g_enabled[DIAG_TIMERS]) {
        timers_install();
    }
}

int diag_enabled(unsigned kind)
{
    return kind < DIAG_COUNT && g_enabled[kind];
}

void diag_line(unsigned kind, const char *format, ...)
{
    if (!diag_enabled(kind)) {
        return;
    }
    DWORD error = GetLastError();
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        InterlockedIncrement(&g_dropped[kind]);
        SetLastError(error);
        return;
    }
    char line[DIAG_TEXT];
    int prefix = _snprintf_s(line, sizeof(line), _TRUNCATE, "diag t=%lu tid=%lu ", GetTickCount(),
                             GetCurrentThreadId());
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line + prefix, sizeof(line) - prefix, _TRUNCATE, format, args);
    va_end(args);
    strcpy_s(g_last[kind], DIAG_TEXT, line);
    ++g_events[kind];
    ++g_window_events[kind];
    if (g_window_events[kind] <= DIAG_LIMIT) {
        if (g_count < DIAG_LINES) {
            strcpy_s(g_lines[(g_head + g_count) % DIAG_LINES], DIAG_TEXT, line);
            ++g_count;
        } else {
            InterlockedIncrement(&g_dropped[kind]);
        }
    } else {
        ++g_capped[kind];
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetLastError(error);
}

/* Render producers never wait for the writer or the shared log lock. */
void diag_log(const char *format, ...)
{
    DWORD error = GetLastError();
    if (!g_writer_ready || !TryAcquireSRWLockExclusive(&g_lock)) {
        InterlockedIncrement(&g_log_dropped);
        SetLastError(error);
        return;
    }
    if (g_count < DIAG_LINES) {
        va_list args;
        va_start(args, format);
        _vsnprintf_s(g_lines[(g_head + g_count) % DIAG_LINES], DIAG_TEXT, _TRUNCATE, format, args);
        va_end(args);
        ++g_count;
    } else {
        InterlockedIncrement(&g_log_dropped);
    }
    ReleaseSRWLockExclusive(&g_lock);
    SetLastError(error);
}

unsigned diag_hash565(const WORD *pixels, int pitch, RECT rect)
{
    unsigned hash = 2166136261u;
    for (int y = rect.top; y < rect.bottom; ++y) {
        const WORD *row = (const WORD *)((const BYTE *)pixels + y * pitch);
        for (int x = rect.left; x < rect.right; ++x) {
            hash = (hash ^ row[x]) * 16777619u;
        }
    }
    return hash;
}

void diag_transition(const char *phase, const diag_frame *frame, int composed, LONG status)
{
    if (!diag_enabled(DIAG_TRANSITIONS)) {
        return;
    }
    unsigned slot = !strcmp(phase, "capture") ? 0 : !strcmp(phase, "present") ? 1 : 2;
    DWORD now = GetTickCount();
    const diag_frame *last = &g_transition_last[slot];
    if (slot != 2 && g_transition_seen[slot] && frame->state == last->state && frame->dib == last->dib &&
        frame->epoch == last->epoch && composed == g_transition_composed[slot] && status == S_OK &&
        (DWORD)(now - g_transition_time[slot]) < 500) {
        return;
    }
    g_transition_last[slot] = *frame;
    g_transition_time[slot] = now;
    g_transition_composed[slot] = composed;
    g_transition_seen[slot] = 1;
    diag_line(DIAG_TRANSITIONS,
              "transition phase=%s epoch=%ld serial=%ld DrawState=%d CDib=%p composed=%d menu_hash=%08x "
              "status=%08lx",
              phase, frame->epoch, frame->serial, frame->state, frame->dib, composed, frame->menu_hash,
              (DWORD)status);
}

void diag_blit(HWND hwnd, HDC source, int x, int y, int w, int h, int sx, int sy)
{
    if (!diag_enabled(DIAG_UI)) {
        return;
    }
    DWORD error = GetLastError();
    g_copy_width = w;
    g_copy_height = h;
    RECT client = {0};
    GetClientRect(hwnd, &client);
    int state = -1;
    __try {
        state = *(int *)(uintptr_t)DRAW_STATE_ADDRESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    static DWORD last_time;
    static HWND last_hwnd;
    static HDC last_source;
    static int last_state;
    static int last_area[6];
    static RECT last_client;
    int area[6] = {x, y, w, h, sx, sy};
    DWORD now = GetTickCount();
    int repeat = last_hwnd == hwnd && last_source == source && last_state == state &&
                 memcmp(last_area, area, sizeof(area)) == 0 &&
                 memcmp(&last_client, &client, sizeof(client)) == 0;
    if (repeat && (DWORD)(now - last_time) < 1000) {
        SetLastError(error);
        return;
    }
    last_hwnd = hwnd;
    last_source = source;
    last_state = state;
    memcpy(last_area, area, sizeof(area));
    last_client = client;
    last_time = now;
    diag_line(
        DIAG_UI,
        "ui_blit hwnd=%p DrawState=%d source=%p copy=(%d,%d,%d,%d) src=(%d,%d) client=(%ld,%ld,%ld,%ld)",
        (void *)hwnd, state, (void *)source, x, y, w, h, sx, sy, client.left, client.top, client.right,
        client.bottom);
    SetLastError(error);
}

void diag_copy_size(int *width, int *height)
{
    *width = g_copy_width;
    *height = g_copy_height;
}

/* Same UI thread as dialog layout and the legacy GDI callers. */
static HWND g_yesno_window;

void diag_yesno_window(HWND hwnd)
{
    g_yesno_window = hwnd;
}

void diag_yesno_gdi(const char *api, const void *caller, HDC dc, int x, int y, int w, int h, int sw, int sh,
                    DWORD rop)
{
    if (!diag_enabled(DIAG_YESNO)) {
        return;
    }
    DWORD error = GetLastError();
    HWND hwnd = WindowFromDC(dc);
    if (hwnd && g_yesno_window && (hwnd == g_yesno_window || IsChild(g_yesno_window, hwnd))) {
        MEMORY_BASIC_INFORMATION memory = {0};
        char path[MAX_PATH] = "unknown";
        const char *name = path;
        if (VirtualQuery(caller, &memory, sizeof(memory)) &&
            GetModuleFileNameA((HMODULE)memory.AllocationBase, path, sizeof(path))) {
            path[sizeof(path) - 1] = 0;
            const char *slash = strrchr(path, '\\');
            name = slash ? slash + 1 : path;
        }
        POINT device[2] = {{x, y}, {x + w, y + h}};
        LPtoDP(dc, device, 2);
        diag_line(DIAG_YESNO,
                  "yesno_gdi dialog=158 api=%s caller=%s+0x%lx address=%p hwnd=%p dc=%p "
                  "dst_xywh=(%d,%d,%d,%d) device=(%ld,%ld,%ld,%ld) src_size=(%d,%d) rop=%08lx",
                  api, name, (DWORD)((uintptr_t)caller - (uintptr_t)memory.AllocationBase), caller,
                  (void *)hwnd, (void *)dc, x, y, w, h, device[0].x, device[0].y, device[1].x, device[1].y,
                  sw, sh, rop);
    }
    SetLastError(error);
}

typedef BOOL(WINAPI *yesno_BitBlt_fn)(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
static yesno_BitBlt_fn g_yesno_BitBlt[4];

static BOOL WINAPI yesno_BitBlt_0(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    diag_yesno_gdi("BitBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, w, h, rop);
    return g_yesno_BitBlt[0](dc, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI yesno_BitBlt_1(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    diag_yesno_gdi("BitBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, w, h, rop);
    return g_yesno_BitBlt[1](dc, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI yesno_BitBlt_2(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    diag_yesno_gdi("BitBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, w, h, rop);
    return g_yesno_BitBlt[2](dc, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI yesno_BitBlt_3(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    diag_yesno_gdi("BitBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, w, h, rop);
    return g_yesno_BitBlt[3](dc, x, y, w, h, src, sx, sy, rop);
}

typedef BOOL(WINAPI *yesno_StretchBlt_fn)(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                          int sh, DWORD rop);
static yesno_StretchBlt_fn g_yesno_StretchBlt[4];

static BOOL WINAPI yesno_StretchBlt_0(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                      int sh, DWORD rop)
{
    diag_yesno_gdi("StretchBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchBlt[0](dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static BOOL WINAPI yesno_StretchBlt_1(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                      int sh, DWORD rop)
{
    diag_yesno_gdi("StretchBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchBlt[1](dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static BOOL WINAPI yesno_StretchBlt_2(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                      int sh, DWORD rop)
{
    diag_yesno_gdi("StretchBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchBlt[2](dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static BOOL WINAPI yesno_StretchBlt_3(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                                      int sh, DWORD rop)
{
    diag_yesno_gdi("StretchBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchBlt[3](dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

typedef int(WINAPI *yesno_StretchDIBits_fn)(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw,
                                            int sh, const void *bits, const BITMAPINFO *info, UINT usage,
                                            DWORD rop);
static yesno_StretchDIBits_fn g_yesno_StretchDIBits[4];

static int WINAPI yesno_StretchDIBits_0(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                        const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop)
{
    diag_yesno_gdi("StretchDIBits", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchDIBits[0](dc, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

static int WINAPI yesno_StretchDIBits_1(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                        const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop)
{
    diag_yesno_gdi("StretchDIBits", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchDIBits[1](dc, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

static int WINAPI yesno_StretchDIBits_2(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                        const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop)
{
    diag_yesno_gdi("StretchDIBits", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchDIBits[2](dc, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

static int WINAPI yesno_StretchDIBits_3(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                        const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop)
{
    diag_yesno_gdi("StretchDIBits", _ReturnAddress(), dc, x, y, (int)w, (int)h, sw, sh, rop);
    return g_yesno_StretchDIBits[3](dc, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

typedef int(WINAPI *yesno_SetDIBitsToDevice_fn)(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                                UINT start, UINT lines, const void *bits,
                                                const BITMAPINFO *info, UINT usage);
static yesno_SetDIBitsToDevice_fn g_yesno_SetDIBitsToDevice[4];

static int WINAPI yesno_SetDIBitsToDevice_0(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                            UINT start, UINT lines, const void *bits, const BITMAPINFO *info,
                                            UINT usage)
{
    diag_yesno_gdi("SetDIBitsToDevice", _ReturnAddress(), dc, x, y, (int)w, (int)h, (int)w, (int)h, SRCCOPY);
    return g_yesno_SetDIBitsToDevice[0](dc, x, y, w, h, sx, sy, start, lines, bits, info, usage);
}

static int WINAPI yesno_SetDIBitsToDevice_1(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                            UINT start, UINT lines, const void *bits, const BITMAPINFO *info,
                                            UINT usage)
{
    diag_yesno_gdi("SetDIBitsToDevice", _ReturnAddress(), dc, x, y, (int)w, (int)h, (int)w, (int)h, SRCCOPY);
    return g_yesno_SetDIBitsToDevice[1](dc, x, y, w, h, sx, sy, start, lines, bits, info, usage);
}

static int WINAPI yesno_SetDIBitsToDevice_2(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                            UINT start, UINT lines, const void *bits, const BITMAPINFO *info,
                                            UINT usage)
{
    diag_yesno_gdi("SetDIBitsToDevice", _ReturnAddress(), dc, x, y, (int)w, (int)h, (int)w, (int)h, SRCCOPY);
    return g_yesno_SetDIBitsToDevice[2](dc, x, y, w, h, sx, sy, start, lines, bits, info, usage);
}

static int WINAPI yesno_SetDIBitsToDevice_3(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                            UINT start, UINT lines, const void *bits, const BITMAPINFO *info,
                                            UINT usage)
{
    diag_yesno_gdi("SetDIBitsToDevice", _ReturnAddress(), dc, x, y, (int)w, (int)h, (int)w, (int)h, SRCCOPY);
    return g_yesno_SetDIBitsToDevice[3](dc, x, y, w, h, sx, sy, start, lines, bits, info, usage);
}

typedef BOOL(WINAPI *yesno_PatBlt_fn)(HDC dc, int x, int y, int w, int h, DWORD rop);
static yesno_PatBlt_fn g_yesno_PatBlt[4];

static BOOL WINAPI yesno_PatBlt_0(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    diag_yesno_gdi("PatBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, 0, 0, rop);
    return g_yesno_PatBlt[0](dc, x, y, w, h, rop);
}

static BOOL WINAPI yesno_PatBlt_1(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    diag_yesno_gdi("PatBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, 0, 0, rop);
    return g_yesno_PatBlt[1](dc, x, y, w, h, rop);
}

static BOOL WINAPI yesno_PatBlt_2(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    diag_yesno_gdi("PatBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, 0, 0, rop);
    return g_yesno_PatBlt[2](dc, x, y, w, h, rop);
}

static BOOL WINAPI yesno_PatBlt_3(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    diag_yesno_gdi("PatBlt", _ReturnAddress(), dc, x, y, (int)w, (int)h, 0, 0, rop);
    return g_yesno_PatBlt[3](dc, x, y, w, h, rop);
}

void diag_yesno_imports(HMODULE module, unsigned index)
{
    if (!diag_enabled(DIAG_YESNO) || !module || index >= 4) {
        return;
    }
    static SRWLOCK install_lock = SRWLOCK_INIT;
    static HMODULE pinned[4];
    static int installed[4][5];
    const wchar_t *names[] = {L"TOW.exe", L"MFC42.DLL", L"MFC40.DLL", L"Anibtn32.ocx"};
    AcquireSRWLockExclusive(&install_lock);
    if (!pinned[index]) {
        HMODULE handle;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, names[index], &handle)) {
            log_line("diag: module pin failed module=%ls error=%lu", names[index], GetLastError());
            ReleaseSRWLockExclusive(&install_lock);
            return;
        }
        pinned[index] = handle;
    }
    /* The caller's earlier lookup could be stale; use only the pinned handle. */
    module = pinned[index];
    const char *modules[] = {"TOW.exe", "MFC42", "MFC40", "Anibtn32"};
    unsigned count = 0;
    {
        void *hooks[] = {(void *)(uintptr_t)yesno_BitBlt_0, (void *)(uintptr_t)yesno_BitBlt_1,
                         (void *)(uintptr_t)yesno_BitBlt_2, (void *)(uintptr_t)yesno_BitBlt_3};
        DWORD *slot = iat_find_slot_in(module, "gdi32.dll", "BitBlt");
        if (!installed[index][0] && slot && *slot == (DWORD)(uintptr_t)hooks[index]) {
            installed[index][0] = 1;
        }
        if (!installed[index][0] && slot && *slot) {
            void *original = (void *)(uintptr_t)*slot;
            g_yesno_BitBlt[index] = (yesno_BitBlt_fn)(uintptr_t)original;
            int result = iat_replace(slot, original, hooks[index]);
            if (result == IAT_OK) {
                installed[index][0] = 1;
                ++count;
            } else {
                log_line("diag: yesno GDI hook failed module=%s api=BitBlt reason=%d", modules[index],
                         result);
            }
        }
    }
    {
        void *hooks[] = {(void *)(uintptr_t)yesno_StretchBlt_0, (void *)(uintptr_t)yesno_StretchBlt_1,
                         (void *)(uintptr_t)yesno_StretchBlt_2, (void *)(uintptr_t)yesno_StretchBlt_3};
        DWORD *slot = iat_find_slot_in(module, "gdi32.dll", "StretchBlt");
        if (!installed[index][1] && slot && *slot == (DWORD)(uintptr_t)hooks[index]) {
            installed[index][1] = 1;
        }
        if (!installed[index][1] && slot && *slot) {
            void *original = (void *)(uintptr_t)*slot;
            g_yesno_StretchBlt[index] = (yesno_StretchBlt_fn)(uintptr_t)original;
            int result = iat_replace(slot, original, hooks[index]);
            if (result == IAT_OK) {
                installed[index][1] = 1;
                ++count;
            } else {
                log_line("diag: yesno GDI hook failed module=%s api=StretchBlt reason=%d", modules[index],
                         result);
            }
        }
    }
    {
        void *hooks[] = {(void *)(uintptr_t)yesno_StretchDIBits_0, (void *)(uintptr_t)yesno_StretchDIBits_1,
                         (void *)(uintptr_t)yesno_StretchDIBits_2, (void *)(uintptr_t)yesno_StretchDIBits_3};
        DWORD *slot = iat_find_slot_in(module, "gdi32.dll", "StretchDIBits");
        if (!installed[index][2] && slot && *slot == (DWORD)(uintptr_t)hooks[index]) {
            installed[index][2] = 1;
        }
        if (!installed[index][2] && slot && *slot) {
            void *original = (void *)(uintptr_t)*slot;
            g_yesno_StretchDIBits[index] = (yesno_StretchDIBits_fn)(uintptr_t)original;
            int result = iat_replace(slot, original, hooks[index]);
            if (result == IAT_OK) {
                installed[index][2] = 1;
                ++count;
            } else {
                log_line("diag: yesno GDI hook failed module=%s api=StretchDIBits reason=%d", modules[index],
                         result);
            }
        }
    }
    {
        void *hooks[] = {
            (void *)(uintptr_t)yesno_SetDIBitsToDevice_0, (void *)(uintptr_t)yesno_SetDIBitsToDevice_1,
            (void *)(uintptr_t)yesno_SetDIBitsToDevice_2, (void *)(uintptr_t)yesno_SetDIBitsToDevice_3};
        DWORD *slot = iat_find_slot_in(module, "gdi32.dll", "SetDIBitsToDevice");
        if (!installed[index][3] && slot && *slot == (DWORD)(uintptr_t)hooks[index]) {
            installed[index][3] = 1;
        }
        if (!installed[index][3] && slot && *slot) {
            void *original = (void *)(uintptr_t)*slot;
            g_yesno_SetDIBitsToDevice[index] = (yesno_SetDIBitsToDevice_fn)(uintptr_t)original;
            int result = iat_replace(slot, original, hooks[index]);
            if (result == IAT_OK) {
                installed[index][3] = 1;
                ++count;
            } else {
                log_line("diag: yesno GDI hook failed module=%s api=SetDIBitsToDevice reason=%d",
                         modules[index], result);
            }
        }
    }
    {
        void *hooks[] = {(void *)(uintptr_t)yesno_PatBlt_0, (void *)(uintptr_t)yesno_PatBlt_1,
                         (void *)(uintptr_t)yesno_PatBlt_2, (void *)(uintptr_t)yesno_PatBlt_3};
        DWORD *slot = iat_find_slot_in(module, "gdi32.dll", "PatBlt");
        if (!installed[index][4] && slot && *slot == (DWORD)(uintptr_t)hooks[index]) {
            installed[index][4] = 1;
        }
        if (!installed[index][4] && slot && *slot) {
            void *original = (void *)(uintptr_t)*slot;
            g_yesno_PatBlt[index] = (yesno_PatBlt_fn)(uintptr_t)original;
            int result = iat_replace(slot, original, hooks[index]);
            if (result == IAT_OK) {
                installed[index][4] = 1;
                ++count;
            } else {
                log_line("diag: yesno GDI hook failed module=%s api=PatBlt reason=%d", modules[index],
                         result);
            }
        }
    }
    if (count) {
        log_line("diag: yesno GDI hooks module=%s installed=%u", modules[index], count);
    }
    ReleaseSRWLockExclusive(&install_lock);
}

/* Window observers retain scaling's final IAT chain. */
static int ui_top(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return hwnd && pid == GetCurrentProcessId() && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD);
}

static int ui_state(void)
{
    int state = -1;
    __try {
        state = *(int *)(uintptr_t)DRAW_STATE_ADDRESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return state;
}

static void ui_watch_thread(DWORD thread);

static void ui_call(unsigned api, const void *caller, HWND hwnd, HWND after, UINT arg, int x, int y, int w,
                    int h)
{
    DWORD error = GetLastError();
    ui_watch_thread(GetCurrentThreadId());
    if (ui_top(hwnd)) {
        InterlockedIncrement(&g_ui_calls[api]);
        char cls[48] = {0};
        char title[48] = {0};
        GetClassNameA(hwnd, cls, sizeof(cls));
        /* Read stored caption without sending WM_GETTEXT to game code. */
        DefWindowProcA(hwnd, WM_GETTEXT, sizeof(title), (LPARAM)title);
        for (unsigned i = 0; i < sizeof(title); ++i) {
            if (title[i] && (unsigned char)title[i] < 32) {
                title[i] = ' ';
            }
        }
        diag_line(DIAG_UI,
                  "ui_api api=%s caller=%p hwnd=%p class={%s} title={%s} cmd_flags=%08x insertAfter=%p "
                  "xywh=(%d,%d,%d,%d) DrawState=%d",
                  g_ui_names[api], caller, (void *)hwnd, cls, title, arg, (void *)after, x, y, w, h,
                  ui_state());
    }
    SetLastError(error);
}

void diag_ui_own(unsigned api, const void *caller, HWND hwnd, HWND after, UINT arg, int x, int y, int w,
                 int h)
{
    if (diag_enabled(DIAG_UI) && api < 8) {
        ui_call(api, caller, hwnd, after, arg, x, y, w, h);
    }
}

void diag_ui_scaling(HWND hwnd, HWND before_after, UINT before, HWND after, UINT flags)
{
    if (!diag_enabled(DIAG_UI)) {
        return;
    }
    DWORD error = GetLastError();
    UINT mask = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_HIDEWINDOW;
    if (((before ^ flags) & mask) || before_after != after) {
        diag_line(DIAG_UI,
                  "ui_scaling api=SetWindowPos hwnd=%p flags=%08x->%08x insertAfter=%p->%p DrawState=%d",
                  (void *)hwnd, before, flags, (void *)before_after, (void *)after, ui_state());
    }
    SetLastError(error);
}

static timer_state timers_state(void)
{
    timer_state state = {0};
    const BYTE *owner = (const BYTE *)InterlockedCompareExchangePointer(&g_timer_owner, NULL, NULL);
    __try {
        state.step_flag = *(const BYTE *)(uintptr_t)TIMER_STEP_FLAG;
        state.draw_flag = *(const BYTE *)(uintptr_t)TIMER_DRAW_FLAG;
        if (owner) {
            state.hwnd = *(HWND const *)(owner + TIMER_HWND_OFFSET);
            state.step_id = *(const UINT *)(owner + TIMER_STEP_ID);
            state.draw_id = *(const UINT *)(owner + TIMER_DRAW_ID);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        state = (timer_state){0};
    }
    return state;
}

static LONG timers_pending_draw(void)
{
    /* Delivery is a proxy for firing: includes queued messages and callbacks
     * suppressed by the game's flag. Only identifiable cancellations subtract. */
    LONG pending = InterlockedCompareExchange(&g_timer_created[1], 0, 0) -
                   InterlockedCompareExchange(&g_timer_delivered[1], 0, 0) -
                   InterlockedCompareExchange(&g_timer_draw_cancelled, 0, 0);
    return max(0, pending);
}

static void timers_failure(int channel, const void *caller, UINT delay, DWORD error)
{
    DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_timer_failure_lock);
    if (channel >= 0) {
        g_timer_failure[channel] = (timer_failure){caller, delay, error, now};
    }
    if ((DWORD)(now - g_timer_failure_window) >= 60000) {
        g_timer_failure_window = now;
        g_timer_failure_events = 0;
    }
    int emit = g_timer_failure_events < 200;
    if (emit) {
        ++g_timer_failure_events;
    }
    ReleaseSRWLockExclusive(&g_timer_failure_lock);
    if (emit) {
        diag_log("timeSetEvent failure channel=%d caller=%p delay=%u error=%lu tick=%lu", channel, caller,
                 delay, error, now);
    }
}

static MMRESULT WINAPI timers_set(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user,
                                  UINT flags)
{
    const void *caller = _ReturnAddress();
    MMRESULT id = g_timer_set(delay, resolution, callback, user, flags);
    DWORD error = GetLastError();
    int channel = (uintptr_t)callback == TIMER_STEP_CALLBACK   ? 0
                  : (uintptr_t)callback == TIMER_DRAW_CALLBACK ? 1
                                                               : -1;
    InterlockedIncrement(&g_timer_sets);
    if (!id) {
        InterlockedIncrement(&g_timer_failures);
        timers_failure(channel, caller, delay, error);
    }
    if (channel >= 0 && !(flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))) {
        InterlockedExchangePointer(&g_timer_owner, (void *)user);
        if (id) {
            InterlockedIncrement(&g_timer_created[channel]);
        }
        InterlockedCompareExchange(&g_timer_last[channel], (LONG)GetTickCount(), 0);
    }
    timer_state state = timers_state();
    diag_line(DIAG_TIMERS,
              "timeSetEvent caller=%p delay=%u resolution=%u callback=%p user=%08lx fuEvent=%08x "
              "id=%u error=%lu step_flag=%u draw_flag=%u stored_step_id=%u stored_draw_id=%u",
              caller, delay, resolution, (void *)(uintptr_t)callback, (DWORD)user, flags, id, id ? 0 : error,
              state.step_flag, state.draw_flag, state.step_id, state.draw_id);
    SetLastError(error);
    return id;
}

static MMRESULT WINAPI timers_kill(UINT id)
{
    const void *caller = _ReturnAddress();
    DWORD incoming_error = GetLastError();
    timer_state state = timers_state();
    SetLastError(incoming_error);
    MMRESULT result = g_timer_kill(id);
    DWORD error = GetLastError();
    InterlockedIncrement(&g_timer_kills);
    if (result == TIMERR_NOERROR && id && id == state.draw_id) {
        InterlockedIncrement(&g_timer_draw_cancelled);
    }
    diag_line(DIAG_TIMERS, "timeKillEvent caller=%p id=%u result=%u", caller, id, result);
    SetLastError(error);
    return result;
}

static void timers_message(const MSG *msg)
{
    if (msg->message != 0x8001 && msg->message != 0x8002) {
        return;
    }
    timer_state state = timers_state();
    if (!state.hwnd || msg->hwnd != state.hwnd) {
        return;
    }
    unsigned channel = msg->message - 0x8001;
    DWORD now = GetTickCount();
    DWORD previous = (DWORD)InterlockedExchange(&g_timer_last[channel], (LONG)now);
    InterlockedIncrement(&g_timer_delivered[channel]);
    InterlockedIncrement(&g_timer_second[channel]);
    if (previous && (DWORD)(now - previous) > 500) {
        diag_line(DIAG_TIMERS,
                  "timer_gap message=%04x hwnd=%p gap_ms=%lu step_flag=%u draw_flag=%u "
                  "stored_step_id=%u stored_draw_id=%u pending_draw_estimate=%ld",
                  msg->message, (void *)msg->hwnd, now - previous, state.step_flag, state.draw_flag,
                  state.step_id, state.draw_id, timers_pending_draw());
    }
}

/* Writer-thread polling detects a silent UI thread without adding game timers. */
static void timers_poll(DWORD now)
{
    static DWORD second;
    static DWORD summary;
    static DWORD reported_stall;
    if (!second) {
        second = now;
        summary = now;
    }
    if ((DWORD)(now - second) < 1000) {
        return;
    }
    DWORD elapsed = now - second;
    second = now;
    timer_state state = timers_state();
    diag_line(DIAG_TIMERS, "timer_delivery interval_ms=%lu step=%ld draw=%ld pending_draw_estimate=%ld",
              elapsed, InterlockedExchange(&g_timer_second[0], 0), InterlockedExchange(&g_timer_second[1], 0),
              timers_pending_draw());
    /* The game's own pause may use stall-plain to show a fresh static frame. */
    DWORD last = (DWORD)InterlockedCompareExchange(&g_timer_last[0], 0, 0);
    if (last && state.step_flag && (DWORD)(now - last) > 1000 && reported_stall != last) {
        reported_stall = last;
        InterlockedIncrement(&g_timer_stalls);
        diag_line(DIAG_TIMERS,
                  "step_stall gap_ms=%lu hwnd=%p step_flag=%u draw_flag=%u stored_step_id=%u "
                  "stored_draw_id=%u pending_draw_estimate=%ld",
                  now - last, (void *)state.hwnd, state.step_flag, state.draw_flag, state.step_id,
                  state.draw_id, timers_pending_draw());
    }
    if ((DWORD)(now - summary) >= 60000) {
        summary = now;
        timer_failure failures[2];
        AcquireSRWLockShared(&g_timer_failure_lock);
        memcpy(failures, g_timer_failure, sizeof(failures));
        ReleaseSRWLockShared(&g_timer_failure_lock);
        log_line("timer_summary sets_interval=%ld failures_interval=%ld kills_interval=%ld "
                 "step_stalls_interval=%ld created_step=%ld created_draw=%ld delivered_step=%ld "
                 "delivered_draw=%ld pending_draw_estimate=%ld "
                 "last_step_failure={caller=%p delay=%u error=%lu tick=%lu} "
                 "last_draw_failure={caller=%p delay=%u error=%lu tick=%lu}",
                 InterlockedExchange(&g_timer_sets, 0), InterlockedExchange(&g_timer_failures, 0),
                 InterlockedExchange(&g_timer_kills, 0), InterlockedExchange(&g_timer_stalls, 0),
                 InterlockedCompareExchange(&g_timer_created[0], 0, 0),
                 InterlockedCompareExchange(&g_timer_created[1], 0, 0),
                 InterlockedCompareExchange(&g_timer_delivered[0], 0, 0),
                 InterlockedCompareExchange(&g_timer_delivered[1], 0, 0), timers_pending_draw(),
                 failures[0].caller, failures[0].delay, failures[0].error, failures[0].tick,
                 failures[1].caller, failures[1].delay, failures[1].error, failures[1].tick);
    }
}

static void timers_install(void)
{
    DWORD *set = iat_find_slot("winmm.dll", "timeSetEvent");
    DWORD *kill = iat_find_slot("winmm.dll", "timeKillEvent");
    if (set && *set) {
        g_timer_set = (timer_set_fn)(uintptr_t)*set;
        log_line("diag: timeSetEvent hook result=%d",
                 iat_replace(set, (void *)g_timer_set, (void *)timers_set));
    } else {
        log_line("diag: timeSetEvent import unavailable");
    }
    if (kill && *kill) {
        g_timer_kill = (timer_kill_fn)(uintptr_t)*kill;
        log_line("diag: timeKillEvent hook result=%d",
                 iat_replace(kill, (void *)g_timer_kill, (void *)timers_kill));
    } else {
        log_line("diag: timeKillEvent import unavailable");
    }
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)timers_messages, &self);
    if (!SetWindowsHookExW(WH_GETMESSAGE, timers_messages, self, GetCurrentThreadId())) {
        log_line("diag: timer GETMESSAGE hook failed error=%lu", GetLastError());
    }
}

static LRESULT CALLBACK timers_messages(int code, WPARAM wp, LPARAM lp)
{
    DWORD error = GetLastError();
    if (code >= 0 && wp == PM_REMOVE && g_enabled[DIAG_TIMERS]) {
        timers_message((const MSG *)lp);
    }
    SetLastError(error);
    LRESULT result = CallNextHookEx(NULL, code, wp, lp);
    SetLastError(error);
    return result;
}

static LRESULT CALLBACK ui_messages(int code, WPARAM wp, LPARAM lp)
{
    DWORD error = GetLastError();
    if (code >= 0 && g_enabled[DIAG_UI]) {
        const CWPSTRUCT *msg = (const CWPSTRUCT *)lp;
        UINT m = msg->message;
        int wanted = m == WM_ACTIVATE || m == WM_ACTIVATEAPP || m == WM_SHOWWINDOW || m == WM_NCACTIVATE;
        UINT flags = 0;
        HWND after = NULL;
        if (m == WM_WINDOWPOSCHANGED && msg->lParam) {
            const WINDOWPOS *pos = (const WINDOWPOS *)msg->lParam;
            flags = pos->flags;
            after = pos->hwndInsertAfter;
            wanted = !(flags & SWP_NOZORDER) || (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW));
        }
        if (wanted && ui_top(msg->hwnd)) {
            char order[640] = {0};
            size_t used = 0;
            unsigned scanned = 0;
            for (HWND hwnd = GetTopWindow(NULL); hwnd && scanned < 1024;
                 hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
                ++scanned;
                if (ui_top(hwnd) && used < sizeof(order) - 40) {
                    int n = _snprintf_s(order + used, sizeof(order) - used, _TRUNCATE, " %p:%d", (void *)hwnd,
                                        IsWindowVisible(hwnd));
                    if (n > 0) {
                        used += (size_t)n;
                    }
                }
            }
            diag_line(DIAG_UI,
                      "zorder hwnd=%p message=%04x wp=%08lx lp=%08lx flags=%08x insertAfter=%p foreground=%p "
                      "DrawState=%d top_first_visible={%s}",
                      (void *)msg->hwnd, m, (DWORD)msg->wParam, (DWORD)msg->lParam, flags, (void *)after,
                      (void *)GetForegroundWindow(), ui_state(), order);
        }
    }
    SetLastError(error);
    LRESULT result = CallNextHookEx(NULL, code, wp, lp);
    SetLastError(error);
    return result;
}

typedef BOOL(WINAPI *ui_ShowWindow_fn)(HWND hwnd, int cmd);
static ui_ShowWindow_fn g_ui_ShowWindow[2];

static BOOL WINAPI ui_ShowWindow_0(HWND hwnd, int cmd)
{
    ui_call(0, _ReturnAddress(), hwnd, NULL, (UINT)cmd, 0, 0, 0, 0);
    return g_ui_ShowWindow[0](hwnd, cmd);
}

static BOOL WINAPI ui_ShowWindow_1(HWND hwnd, int cmd)
{
    ui_call(0, _ReturnAddress(), hwnd, NULL, (UINT)cmd, 0, 0, 0, 0);
    return g_ui_ShowWindow[1](hwnd, cmd);
}

typedef BOOL(WINAPI *ui_SetWindowPos_fn)(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags);
static ui_SetWindowPos_fn g_ui_SetWindowPos[2];

static BOOL WINAPI ui_SetWindowPos_0(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    ui_call(1, _ReturnAddress(), hwnd, after, flags, x, y, w, h);
    return g_ui_SetWindowPos[0](hwnd, after, x, y, w, h, flags);
}

static BOOL WINAPI ui_SetWindowPos_1(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    ui_call(1, _ReturnAddress(), hwnd, after, flags, x, y, w, h);
    return g_ui_SetWindowPos[1](hwnd, after, x, y, w, h, flags);
}

typedef BOOL(WINAPI *ui_BringWindowToTop_fn)(HWND hwnd);
static ui_BringWindowToTop_fn g_ui_BringWindowToTop[2];

static BOOL WINAPI ui_BringWindowToTop_0(HWND hwnd)
{
    ui_call(2, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_BringWindowToTop[0](hwnd);
}

static BOOL WINAPI ui_BringWindowToTop_1(HWND hwnd)
{
    ui_call(2, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_BringWindowToTop[1](hwnd);
}

typedef BOOL(WINAPI *ui_SetForegroundWindow_fn)(HWND hwnd);
static ui_SetForegroundWindow_fn g_ui_SetForegroundWindow[2];

static BOOL WINAPI ui_SetForegroundWindow_0(HWND hwnd)
{
    ui_call(3, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetForegroundWindow[0](hwnd);
}

static BOOL WINAPI ui_SetForegroundWindow_1(HWND hwnd)
{
    ui_call(3, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetForegroundWindow[1](hwnd);
}

typedef HWND(WINAPI *ui_SetActiveWindow_fn)(HWND hwnd);
static ui_SetActiveWindow_fn g_ui_SetActiveWindow[2];

static HWND WINAPI ui_SetActiveWindow_0(HWND hwnd)
{
    ui_call(4, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetActiveWindow[0](hwnd);
}

static HWND WINAPI ui_SetActiveWindow_1(HWND hwnd)
{
    ui_call(4, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetActiveWindow[1](hwnd);
}

typedef HWND(WINAPI *ui_SetFocus_fn)(HWND hwnd);
static ui_SetFocus_fn g_ui_SetFocus[2];

static HWND WINAPI ui_SetFocus_0(HWND hwnd)
{
    ui_call(5, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetFocus[0](hwnd);
}

static HWND WINAPI ui_SetFocus_1(HWND hwnd)
{
    ui_call(5, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_SetFocus[1](hwnd);
}

typedef BOOL(WINAPI *ui_EnableWindow_fn)(HWND hwnd, BOOL enable);
static ui_EnableWindow_fn g_ui_EnableWindow[2];

static BOOL WINAPI ui_EnableWindow_0(HWND hwnd, BOOL enable)
{
    ui_call(6, _ReturnAddress(), hwnd, NULL, (UINT)enable, 0, 0, 0, 0);
    return g_ui_EnableWindow[0](hwnd, enable);
}

static BOOL WINAPI ui_EnableWindow_1(HWND hwnd, BOOL enable)
{
    ui_call(6, _ReturnAddress(), hwnd, NULL, (UINT)enable, 0, 0, 0, 0);
    return g_ui_EnableWindow[1](hwnd, enable);
}

typedef BOOL(WINAPI *ui_DestroyWindow_fn)(HWND hwnd);
static ui_DestroyWindow_fn g_ui_DestroyWindow[2];

static BOOL WINAPI ui_DestroyWindow_0(HWND hwnd)
{
    ui_call(7, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_DestroyWindow[0](hwnd);
}

static BOOL WINAPI ui_DestroyWindow_1(HWND hwnd)
{
    ui_call(7, _ReturnAddress(), hwnd, NULL, 0, 0, 0, 0, 0);
    return g_ui_DestroyWindow[1](hwnd);
}

/* Hooks live until process exit, like the existing diagnostic writer. */
static void ui_watch_thread(DWORD thread)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static DWORD threads[128];
    static unsigned count;
    AcquireSRWLockExclusive(&lock);
    for (unsigned i = 0; i < count; ++i) {
        if (threads[i] == thread) {
            ReleaseSRWLockExclusive(&lock);
            return;
        }
    }
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)ui_messages, &self);
    if (count < ARRAYSIZE(threads) && SetWindowsHookExW(WH_CALLWNDPROC, ui_messages, self, thread)) {
        threads[count++] = thread;
    } else {
        diag_line(DIAG_UI, "ui_message_hook_failed tid=%lu error=%lu capacity=%u", thread, GetLastError(),
                  count);
        diag_line(DIAG_TIMERS, "timer_message_hook_failed tid=%lu error=%lu capacity=%u", thread,
                  GetLastError(), count);
    }
    ReleaseSRWLockExclusive(&lock);
}

static void ui_install_messages(void)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        log_line("diag: ui thread snapshot failed error=%lu", GetLastError());
        ui_watch_thread(GetCurrentThreadId());
        return;
    }
    THREADENTRY32 entry = {0};
    entry.dwSize = sizeof(entry);
    BOOL more = Thread32First(snapshot, &entry);
    while (more) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
            ui_watch_thread(entry.th32ThreadID);
        }
        more = Thread32Next(snapshot, &entry);
    }
    CloseHandle(snapshot);
}

static void ui_import(HMODULE module, unsigned api, void *hook, void *storage)
{
    DWORD *slot = iat_find_slot_in(module, "user32.dll", g_ui_names[api]);
    if (!slot || !*slot) {
        log_line("diag: ui import unavailable module=%p api=%s", (void *)module, g_ui_names[api]);
        return;
    }
    void *original = (void *)(uintptr_t)*slot;
    memcpy(storage, &original, sizeof(original));
    int result = iat_replace(slot, original, hook);
    if (result != IAT_OK) {
        log_line("diag: ui hook failed module=%p api=%s reason=%d", (void *)module, g_ui_names[api], result);
    }
}

void diag_ui_install(void)
{
    if (!diag_enabled(DIAG_UI)) {
        return;
    }
    static int installed;
    if (installed) {
        return;
    }
    installed = 1;
    void *hooks[8][2] = {
        {(void *)ui_ShowWindow_0, (void *)ui_ShowWindow_1},
        {(void *)ui_SetWindowPos_0, (void *)ui_SetWindowPos_1},
        {(void *)ui_BringWindowToTop_0, (void *)ui_BringWindowToTop_1},
        {(void *)ui_SetForegroundWindow_0, (void *)ui_SetForegroundWindow_1},
        {(void *)ui_SetActiveWindow_0, (void *)ui_SetActiveWindow_1},
        {(void *)ui_SetFocus_0, (void *)ui_SetFocus_1},
        {(void *)ui_EnableWindow_0, (void *)ui_EnableWindow_1},
        {(void *)ui_DestroyWindow_0, (void *)ui_DestroyWindow_1},
    };
    void *storage[8][2] = {
        {&g_ui_ShowWindow[0], &g_ui_ShowWindow[1]},
        {&g_ui_SetWindowPos[0], &g_ui_SetWindowPos[1]},
        {&g_ui_BringWindowToTop[0], &g_ui_BringWindowToTop[1]},
        {&g_ui_SetForegroundWindow[0], &g_ui_SetForegroundWindow[1]},
        {&g_ui_SetActiveWindow[0], &g_ui_SetActiveWindow[1]},
        {&g_ui_SetFocus[0], &g_ui_SetFocus[1]},
        {&g_ui_EnableWindow[0], &g_ui_EnableWindow[1]},
        {&g_ui_DestroyWindow[0], &g_ui_DestroyWindow[1]},
    };
    ui_install_messages();
    for (unsigned i = 0; i < 2; ++i) {
        HMODULE module = i ? GetModuleHandleA("MFC42.DLL") : GetModuleHandleA(NULL);
        for (unsigned api = 0; api < 8; ++api) {
            ui_import(module, api, hooks[api][i], storage[api][i]);
        }
    }
}
