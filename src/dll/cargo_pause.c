#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <wchar.h>
#include "cargo_pause.h"
#include "patch.h"
#include "log.h"

/* CWinApp (0x4ca2c8) + m_pMainWnd (0x20), mission and timer state. */
#define FRAME_ADDRESS      0x4ca2e8
#define MISSION_ADDRESS    0x4cddd8
#define DRAW_STATE_ADDRESS 0x4cde48
#define NETWORK_ADDRESS    0x4cdd55
#define STEP_ADDRESS       0x4ce2a8
#define DRAW_ADDRESS       0x4ce2ac
#define HWND_OFFSET        0x20
#define CARGO_OFFSET       0xe8
#define CARGO_VTABLE       0x4a1e14
/* sub_41BA40: CargoTabs resource 0x95; ctor stores Cargo at +0x2ac. */
#define TABS_OFFSET       0x350
#define TABS_VTABLE       0x4a20f4
#define TABS_CARGO_OFFSET 0x2ac
#define POPUP_LIMIT       32
#define FRAME_VTABLE      0x4a4f24
#define APP_INIT_ADDRESS  0x407540

/* Frame MFC message map: AfxSig_lwl -> pause/resume with lParam bit 4. */
#define PAUSE_MAP_ADDRESS  0x4a4e90
#define RESUME_MAP_ADDRESS 0x4a4ea8
#define PAUSE_BIT_ADDRESS  0x447fcf
#define PAUSE_MESSAGE      0x8023
#define RESUME_MESSAGE     0x8024
#define PAUSE_BANNER_BIT   4
#define SUBCLASS_ID        ((UINT_PTR) & g_enabled)
#define LOG_INTERVAL_MS    60000
#define LOG_LIMIT          40

static const uint32_t PAUSE_MAP[] = {PAUSE_MESSAGE, 0, 0, 0, 10, 0x447f10};
static const uint32_t RESUME_MAP[] = {RESUME_MESSAGE, 0, 0, 0, 10, 0x448150};
static const uint8_t PAUSE_BIT_BYTES[] = {0x8b, 0x44, 0x24, 0x2c, 0x32, 0xd2, 0x8b, 0xc8, 0x80, 0xe1, 0x04};
static const uint8_t APP_INIT_BYTES[] = {0xb9, 0xc8, 0xa2, 0x4c, 0x00};

typedef struct {
    HWND frame;
    HWND cargo;
    HWND tabs;
    const void *object;
    const void *mission;
    int state;
    int network;
    int stopped;
    int cargo_type;
} cargo_state;

typedef struct {
    HWND hwnd;
    HWND owner;
    WCHAR name[256];
    int hidden;
} hidden_popup;

static hidden_popup g_popups[POPUP_LIMIT];
static unsigned g_popup_count;

static int g_enabled;
static HWND g_frame;
static cargo_state g_hidden;
static int g_changing;
static DWORD g_log_window;
static unsigned g_logged;
static int g_banner;
static int g_skip_capture;
static int g_poll_known;
static cargo_state g_last_poll;
static int g_last_paused;
static int g_last_visible;
static int g_last_valid;

static LRESULT CALLBACK cargo_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

static int read_state(cargo_state *state)
{
    ZeroMemory(state, sizeof(*state));
    __try {
        const BYTE *frame = *(const BYTE *const *)(uintptr_t)FRAME_ADDRESS;
        if (!frame) {
            return 0;
        }
        if (*(const DWORD *)frame != FRAME_VTABLE) {
            return 0;
        }
        state->frame = *(HWND const *)(frame + HWND_OFFSET);
        state->object = *(const BYTE *const *)(frame + CARGO_OFFSET);
        if (state->object) {
            state->cargo_type = *(const DWORD *)state->object == CARGO_VTABLE;
            if (state->cargo_type) {
                state->cargo = *(HWND const *)((const BYTE *)state->object + HWND_OFFSET);
                const BYTE *tabs = *(const BYTE *const *)((const BYTE *)state->object + TABS_OFFSET);
                if (tabs && *(const DWORD *)tabs == TABS_VTABLE &&
                    *(const void *const *)(tabs + TABS_CARGO_OFFSET) == state->object) {
                    state->tabs = *(HWND const *)(tabs + HWND_OFFSET);
                }
            }
        }
        state->mission = *(const void *const *)(uintptr_t)MISSION_ADDRESS;
        state->state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
        state->network = *(const BYTE *)(uintptr_t)NETWORK_ADDRESS;
        state->stopped = !*(const BYTE *)(uintptr_t)STEP_ADDRESS && !*(const BYTE *)(uintptr_t)DRAW_ADDRESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static int mission_view(const cargo_state *state)
{
    return state->mission && !state->network &&
           (state->state == 2 || state->state == 3 || state->state == 4 || state->state == 8);
}

static int own_window(HWND hwnd)
{
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(hwnd, &process);
    return hwnd && thread == GetCurrentThreadId() && process == GetCurrentProcessId();
}

static int cargo_window(const cargo_state *state)
{
    WCHAR name[32];
    return own_window(state->cargo) && GetWindow(state->cargo, GW_OWNER) == state->frame &&
           !(GetWindowLongPtrW(state->cargo, GWL_STYLE) & WS_CHILD) &&
           GetClassNameW(state->cargo, name, ARRAYSIZE(name)) && !wcscmp(name, L"#32770");
}

static int log_allowed(void)
{
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_log_window) >= LOG_INTERVAL_MS) {
        g_log_window = now;
        g_logged = 0;
    }
    if (g_logged < LOG_LIMIT) {
        ++g_logged;
        return 1;
    }
    return 0;
}

static void event_log(const char *event, HWND hwnd)
{
    if (log_allowed()) {
        log_line("cargo_pause: %s hwnd=%p", event, (void *)hwnd);
    }
}

static void forget_hidden(void)
{
    for (unsigned i = 0; i < g_popup_count; ++i) {
        if (g_popups[i].hwnd) {
            RemoveWindowSubclass(g_popups[i].hwnd, cargo_proc, SUBCLASS_ID);
        }
    }
    g_popup_count = 0;
    ZeroMemory(g_popups, sizeof(g_popups));
    HWND hwnd = g_hidden.cargo;
    ZeroMemory(&g_hidden, sizeof(g_hidden));
    if (hwnd) {
        RemoveWindowSubclass(hwnd, cargo_proc, SUBCLASS_ID);
    }
}

static int cargo_captured(HWND cargo)
{
    HWND capture = GetCapture();
    return capture && (capture == cargo || IsChild(cargo, capture));
}

static int captured_at_pause(HWND frame)
{
    cargo_state state;
    return read_state(&state) && state.frame == frame && cargo_captured(state.cargo);
}

/* A subclass belongs to this window lifetime, unlike an HWND value alone. */
static int same_hidden(const cargo_state *state)
{
    DWORD_PTR data = 0;
    return state->frame == g_hidden.frame && state->cargo == g_hidden.cargo &&
           state->object == g_hidden.object && state->mission == g_hidden.mission && mission_view(state) &&
           cargo_window(state) && GetWindowSubclass(state->cargo, cargo_proc, SUBCLASS_ID, &data);
}

static LRESULT CALLBACK cargo_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    (void)id;
    (void)data;
    DWORD error = GetLastError();
    /* Close/destroy or an external visibility change cancels our pending restore. */
    const WINDOWPOS *position = (const WINDOWPOS *)lp;
    int visibility =
        msg == WM_WINDOWPOSCHANGING && position && (position->flags & (SWP_HIDEWINDOW | SWP_SHOWWINDOW));
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_NCDESTROY ||
        ((msg == WM_SHOWWINDOW || visibility) && !g_changing)) {
        if (hwnd == g_hidden.cargo) {
            forget_hidden();
        } else {
            for (unsigned i = 0; i < g_popup_count; ++i) {
                if (g_popups[i].hwnd == hwnd) {
                    g_popups[i].hwnd = NULL;
                    RemoveWindowSubclass(hwnd, cargo_proc, SUBCLASS_ID);
                    break;
                }
            }
        }
    }
    SetLastError(error);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* Snapshot before owner hiding can change owned-window visibility. */
static BOOL CALLBACK collect_popup(HWND hwnd, LPARAM param)
{
    const cargo_state *state = (const cargo_state *)param;
    if (!own_window(hwnd) || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    WCHAR name[256] = {0};
    RECT rect = {0};
    HWND owner = GetWindow(hwnd, GW_OWNER);
    int named = GetClassNameW(hwnd, name, ARRAYSIZE(name));
    GetWindowRect(hwnd, &rect);
    if (log_allowed()) {
        log_line("cargo_pause: window hwnd=%p class=%ls size=%ldx%ld visible=1 owner=%p", (void *)hwnd, name,
                 rect.right - rect.left, rect.bottom - rect.top, (void *)owner);
    }
    int tabs = hwnd == state->tabs && named && !wcscmp(name, L"#32770");
    if (hwnd == state->cargo || (owner != state->cargo && !tabs)) {
        return TRUE;
    }
    if (!named || g_popup_count == POPUP_LIMIT) {
        return FALSE;
    }
    hidden_popup *popup = &g_popups[g_popup_count];
    popup->hwnd = hwnd;
    popup->owner = owner;
    CopyMemory(popup->name, name, sizeof(name));
    ++g_popup_count;
    return TRUE;
}

static int same_popup(const hidden_popup *popup)
{
    WCHAR name[256];
    DWORD_PTR data = 0;
    return own_window(popup->hwnd) && GetWindow(popup->hwnd, GW_OWNER) == popup->owner &&
           GetClassNameW(popup->hwnd, name, ARRAYSIZE(name)) && !wcscmp(name, popup->name) &&
           GetWindowSubclass(popup->hwnd, cargo_proc, SUBCLASS_ID, &data);
}

static int set_popups(int show)
{
    int ok = 1;
    for (unsigned i = 0; i < g_popup_count; ++i) {
        hidden_popup *popup = &g_popups[i];
        if (!same_popup(popup) || (show && !popup->hidden)) {
            continue;
        }
        if (!SetWindowPos(popup->hwnd, NULL, 0, 0, 0, 0,
                          (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                              SWP_NOACTIVATE)) {
            ok = 0;
            continue;
        }
        popup->hidden = !show;
        event_log(show ? "restore popup" : "hide popup", popup->hwnd);
    }
    return ok;
}

static void hide_cargo(HWND frame)
{
    cargo_state state;
    const char *skip = NULL;
    if (g_hidden.cargo) {
        return;
    }
    if (!read_state(&state)) {
        skip = "skip hide: unreadable frame/state";
    } else if (state.frame != frame || !own_window(frame)) {
        skip = "skip hide: frame changed or wrong UI thread";
    } else if (!mission_view(&state)) {
        skip = "skip hide: outside single-player mission";
    } else if (!state.stopped) {
        skip = "skip hide: timers running";
    } else if (g_skip_capture || cargo_captured(state.cargo)) {
        skip = "skip hide: cargo capture";
    } else if (!state.object) {
        skip = "skip hide: cargo closed";
    } else if (!state.cargo_type) {
        skip = "skip hide: unexpected cargo vtable";
    } else if (!cargo_window(&state)) {
        skip = "skip hide: invalid cargo window/owner/thread";
    } else if (!IsWindowVisible(state.cargo)) {
        skip = "skip hide: cargo not visible";
    }
    if (skip) {
        event_log(skip, state.cargo);
        return;
    }
    if (!EnumThreadWindows(GetCurrentThreadId(), collect_popup, (LPARAM)&state)) {
        event_log("skip hide: popup snapshot failed", state.cargo);
        forget_hidden();
        return;
    }
    for (unsigned i = 0; i < g_popup_count; ++i) {
        if (!SetWindowSubclass(g_popups[i].hwnd, cargo_proc, SUBCLASS_ID, 0)) {
            event_log("skip hide: popup subclass failed", g_popups[i].hwnd);
            forget_hidden();
            return;
        }
    }
    if (!SetWindowSubclass(state.cargo, cargo_proc, SUBCLASS_ID, 0)) {
        event_log("skip hide: cargo subclass failed", state.cargo);
        forget_hidden();
        return;
    }
    g_hidden = state;
    g_changing = 1;
    if (!set_popups(0)) {
        set_popups(1);
        g_changing = 0;
        event_log("skip hide: popup visibility change failed", state.cargo);
        forget_hidden();
        return;
    }
    BOOL ok = SetWindowPos(state.cargo, NULL, 0, 0, 0, 0,
                           SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_changing = 0;
    if (ok && g_hidden.cargo && !IsWindowVisible(state.cargo)) {
        event_log("hide", state.cargo);
    } else {
        g_changing = 1;
        set_popups(1);
        g_changing = 0;
        event_log("skip hide: visibility change failed", state.cargo);
        forget_hidden();
    }
}

static void restore_cargo(HWND frame)
{
    cargo_state state;
    if (!g_hidden.cargo || g_hidden.frame != frame) {
        return;
    }
    if (!read_state(&state) || !same_hidden(&state) || IsWindowVisible(state.cargo)) {
        forget_hidden();
        return;
    }
    g_changing = 1;
    BOOL ok = SetWindowPos(state.cargo, NULL, 0, 0, 0, 0,
                           SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (ok && IsWindowVisible(state.cargo)) {
        set_popups(1);
    }
    g_changing = 0;
    if (ok && IsWindowVisible(state.cargo)) {
        event_log("restore", state.cargo);
    }
    forget_hidden();
}

/* Normal UI message forwarding only: no direct calls into game functions.
 * Restore before resume so the original handler can SetFocus to its saved HWND. */
static LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    (void)id;
    (void)data;
    DWORD error = GetLastError();
    int banner = (lp & PAUSE_BANNER_BIT) != 0;
    if (msg == PAUSE_MESSAGE && banner) {
        g_skip_capture = captured_at_pause(hwnd);
    }
    if ((msg == PAUSE_MESSAGE || msg == RESUME_MESSAGE) && log_allowed()) {
        log_line("cargo_pause: message=%04x flags=%08lx capture=%d frame=%p", msg, (unsigned long)lp,
                 g_skip_capture, (void *)hwnd);
    }
    if (msg == RESUME_MESSAGE && banner) {
        restore_cargo(hwnd);
    }
    if (msg == WM_NCDESTROY) {
        forget_hidden();
        g_frame = NULL;
        g_banner = 0;
        RemoveWindowSubclass(hwnd, frame_proc, SUBCLASS_ID);
    }
    SetLastError(error);
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    error = GetLastError();
    if (banner && (msg == PAUSE_MESSAGE || msg == RESUME_MESSAGE)) {
        g_banner = msg == PAUSE_MESSAGE;
    }
    SetLastError(error);
    return result;
}

/* Called only at App_Run's existing top-level PeekMessageA hook, on the UI thread. */
void cargo_pause_poll(void)
{
    if (!g_enabled) {
        return;
    }
    cargo_state state;
    int valid = read_state(&state);
    if (!valid || !state.stopped || state.frame != g_last_poll.frame ||
        state.mission != g_last_poll.mission) {
        g_banner = 0;
    }
    int paused = valid && g_banner && state.stopped;
    int visible = valid && state.cargo && IsWindowVisible(state.cargo);
    int changed = !g_poll_known || valid != g_last_valid || state.stopped != g_last_poll.stopped ||
                  state.cargo_type != g_last_poll.cargo_type || state.frame != g_last_poll.frame ||
                  state.cargo != g_last_poll.cargo || state.object != g_last_poll.object ||
                  state.mission != g_last_poll.mission || state.state != g_last_poll.state ||
                  state.network != g_last_poll.network || paused != g_last_paused ||
                  visible != g_last_visible;
    if (changed && log_allowed()) {
        log_line("cargo_pause: poll valid=%d paused=%d banner=%d stopped=%d frame=%p cargo=%p visible=%d "
                 "cargo_type=%d view=%d network=%d",
                 valid, paused, g_banner, state.stopped, (void *)state.frame, (void *)state.cargo, visible,
                 state.cargo_type, state.state, state.network);
    }
    if (g_hidden.cargo && (!valid || !same_hidden(&state))) {
        forget_hidden();
    }
    if (valid && state.frame == g_frame) {
        if (paused && !g_last_paused) {
            hide_cargo(state.frame);
        } else if (!paused && g_hidden.cargo) {
            restore_cargo(state.frame);
        }
    }
    /* Save the pre-action observation so the next poll logs our visibility change. */
    g_poll_known = 1;
    g_last_poll = state;
    g_last_paused = paused;
    g_last_visible = visible;
    g_last_valid = valid;
    if (!valid || !own_window(state.frame) || state.frame == g_frame) {
        return;
    }
    if (g_frame) {
        RemoveWindowSubclass(g_frame, frame_proc, SUBCLASS_ID);
    }
    g_frame = NULL;
    g_banner = 0;
    if (SetWindowSubclass(state.frame, frame_proc, SUBCLASS_ID, 0)) {
        g_frame = state.frame;
        event_log("frame subclass installed", state.frame);
    } else {
        event_log("frame subclass failed", state.frame);
    }
}

void cargo_pause_configure(const char *path)
{
    int enabled = (int)GetPrivateProfileIntA("cargo_pause", "enabled", 0, path);
    log_line("[cargo_pause] enabled=%d", enabled);
    if (!enabled) {
        return;
    }
    if (!patch_check((void *)APP_INIT_ADDRESS, APP_INIT_BYTES, sizeof(APP_INIT_BYTES)) ||
        !patch_check((void *)PAUSE_MAP_ADDRESS, (const uint8_t *)PAUSE_MAP, sizeof(PAUSE_MAP)) ||
        !patch_check((void *)RESUME_MAP_ADDRESS, (const uint8_t *)RESUME_MAP, sizeof(RESUME_MAP)) ||
        !patch_check((void *)PAUSE_BIT_ADDRESS, PAUSE_BIT_BYTES, sizeof(PAUSE_BIT_BYTES))) {
        log_line("cargo_pause: failed: unexpected pause code or message map");
        return;
    }
    g_enabled = 1;
}

int cargo_pause_enabled(void)
{
    return g_enabled;
}
