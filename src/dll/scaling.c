#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "scale.h"
#include "iat.h"
#include "patch.h"
#include "log.h"

/* Verified presentation call and BitBlt import in the supported TOW.exe. */
#define COPY_CALL_RVA         0x1757b
#define HOVER_RVA             0x15f80
#define FRAME_CALL_RVA        0x17526
#define BITBLT_SLOT_ADDRESS   0x4a0080
#define FRAME_STYLE           0x14ca8000
#define TARGET_LIMIT          8
#define CHILD_LIMIT           128
#define CHILD_DEPTH           4
#define DC_LIMIT              64
#define FONT_LIMIT            64
#define CURSOR_LIMIT          64
#define CURSOR_SIZE           32
#define LOGICAL_WIDTH         640
#define LOGICAL_HEIGHT        480
#define BACKDROP_CLASS        L"TowFixScalingBackdrop"
#define WINDOW_LOG_LIMIT      40
#define BORDERLESS_STYLE_MASK (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

/* Supported image base, CWnd::m_hWnd and physical drag handlers. */
#define GAME_BASE        0x400000
#define CWND_HWND_OFFSET 0x20
#define DRAG_MOVE_RVA    0x48d60
#define DRAG_UP_RVA      0x48e20

typedef BOOL(WINAPI *bitblt_fn)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL(WINAPI *client_rect_fn)(HWND, LPRECT);
typedef BOOL(WINAPI *dpi_aware_fn)(void);

typedef struct {
    HWND hwnd;
    WNDPROC previous;
    BOOL unicode;
    int width;
    int height;
    int active;
    DWORD last_sweep;
    unsigned errors;
    HWND backdrop;
    int fullscreen;
    LONG_PTR saved_style;
    LONG_PTR saved_exstyle;
    int laying_out;
    int dropping_topmost;
} scale_target;

typedef struct {
    HWND hwnd;
    WNDPROC previous;
    BOOL unicode;
    int logical[4];
    int physical[4];
    int dialog;
    int text_font;
    HFONT original_font;
} scale_child;

static scale_child g_children[CHILD_LIMIT];
static int g_scaling_child;
static HWND g_moving_child;
static int logical_coordinate(int value);

static int g_factor;
static int g_installed;
static int g_dialogs;
static int g_fonts;
static int g_scale_cursor;
static int g_dc_transform;
static int g_dc_transform_controls;
static void install_paint_imports(HMODULE module, unsigned index);
static void install_ocx(void);
static HWND g_centered[64];
static void center_dialog(HWND hwnd);
static void forget_dialog(HWND hwnd);
static void install_mfc40(void);
static scale_target g_targets[TARGET_LIMIT];
static bitblt_fn g_bitblt;
static client_rect_fn g_client_rect[2];
static int g_drawing;
static unsigned g_errors;
static int g_borderless;
static int g_log_windows;
static HWINEVENTHOOK g_window_hook;
static unsigned g_window_logs;
static unsigned g_nc_logs;
static HWND g_newest_dialog;
static int target_bounds(scale_target *target);
static int position_target(scale_target *target);
static int layout_position(scale_target *target, WINDOWPOS *position);
static void sync_backdrop(scale_target *target);
static int apply_borderless(scale_target *target);

static scale_child *find_child(HWND hwnd)
{
    for (unsigned i = 0; i < CHILD_LIMIT; ++i) {
        if (hwnd && g_children[i].hwnd == hwnd) {
            return &g_children[i];
        }
    }
    return NULL;
}

/* Bound diagnostics even when a rejected HWND is encountered on every frame. */
static int scale_error(scale_target *target, const char *operation)
{
    unsigned *count = target ? &target->errors : &g_errors;
    if (*count >= 3 || g_errors >= 3) {
        return 0;
    }
    ++*count;
    if (target) {
        ++g_errors;
    }
    log_line("scaling: %s failed (window %p, error %lu)", operation, target ? (void *)target->hwnd : NULL,
             GetLastError());
    return 0;
}

static scale_target *find_target(HWND hwnd)
{
    for (unsigned i = 0; i < TARGET_LIMIT; ++i) {
        if (hwnd && g_targets[i].hwnd == hwnd) {
            return &g_targets[i];
        }
    }
    return NULL;
}

static int main_target(scale_target *target)
{
    WCHAR name[64];
    return GetAncestor(target->hwnd, GA_ROOT) == target->hwnd &&
           GetClassNameW(target->hwnd, name, ARRAYSIZE(name)) && wcscmp(name, L"#32770") != 0;
}

static BOOL logical_client_rect(unsigned index, HWND hwnd, LPRECT rect)
{
    BOOL ok = g_client_rect[index](hwnd, rect);
    if (!ok) {
        scale_error(find_target(hwnd), "GetClientRect import");
        return ok;
    }
    scale_target *target = find_target(hwnd);
    if (target && target->active) {
        RECT logical = {0, 0, LOGICAL_WIDTH, LOGICAL_HEIGHT};
        *rect = logical;
    } else if (find_child(hwnd)) {
        rect->right = logical_coordinate(rect->right);
        rect->bottom = logical_coordinate(rect->bottom);
    }
    return ok;
}

static BOOL WINAPI tow_client_rect(HWND hwnd, LPRECT rect)
{
    return logical_client_rect(0, hwnd, rect);
}

static BOOL WINAPI mfc_client_rect(HWND hwnd, LPRECT rect)
{
    return logical_client_rect(1, hwnd, rect);
}

typedef BOOL(WINAPI *cursor_fn)(LPPOINT);
typedef BOOL(WINAPI *set_cursor_fn)(int, int);
typedef BOOL(WINAPI *clip_cursor_fn)(const RECT *);
static cursor_fn g_cursor;
static set_cursor_fn g_set_cursor;
static clip_cursor_fn g_clip_cursor;

/* Math.floor, including negatives and INT_MIN. */
static int logical_coordinate(int value)
{
    return value / g_factor - (value % g_factor < 0);
}

static int active_origin(POINT *origin)
{
    scale_target *target = find_target(GetAncestor(GetForegroundWindow(), GA_ROOT));
    origin->x = 0;
    origin->y = 0;
    return target && target->active && ClientToScreen(target->hwnd, origin);
}

static BOOL WINAPI logical_cursor(LPPOINT point)
{
    DWORD saved = GetLastError();
    POINT origin;
    int active = active_origin(&origin);
    SetLastError(saved);
    BOOL result = g_cursor(point);
    if (result && point && active) {
        point->x = origin.x + logical_coordinate(point->x - origin.x);
        point->y = origin.y + logical_coordinate(point->y - origin.y);
    }
    return result;
}

static BOOL WINAPI physical_cursor(int x, int y)
{
    DWORD error = GetLastError();
    POINT origin;
    if (active_origin(&origin)) {
        x = origin.x + (x - origin.x) * g_factor;
        y = origin.y + (y - origin.y) * g_factor;
    }
    SetLastError(error);
    return g_set_cursor(x, y);
}

static BOOL WINAPI physical_clip(const RECT *rect)
{
    DWORD error = GetLastError();
    POINT origin;
    RECT copy;
    if (rect && active_origin(&origin)) {
        copy.left = origin.x + (rect->left - origin.x) * g_factor;
        copy.top = origin.y + (rect->top - origin.y) * g_factor;
        copy.right = origin.x + (rect->right - origin.x) * g_factor;
        copy.bottom = origin.y + (rect->bottom - origin.y) * g_factor;
        rect = &copy;
    }
    SetLastError(error);
    return g_clip_cursor(rect);
}

static int scaled_view(HWND hwnd)
{
    scale_target *root = find_target(GetAncestor(hwnd, GA_ROOT));
    scale_target *target = find_target(hwnd);
    if (find_child(hwnd)) {
        return 1;
    }
    if ((!root || !root->active) && (!target || !target->active)) {
        return 0;
    }
    RECT rect;
    return GetClientRect(hwnd, &rect) && rect.right == LOGICAL_WIDTH * g_factor &&
           rect.bottom == LOGICAL_HEIGHT * g_factor;
}

/* Bounded DC registry; nested messages leave an existing transform intact. */
typedef struct {
    HDC dc;
    int graphics;
    int stretch;
} paint_dc;

static paint_dc g_paint_dcs[DC_LIMIT];
static SRWLOCK g_dc_lock = SRWLOCK_INIT;

static int transformed_dc(HDC dc)
{
    int found = 0;
    AcquireSRWLockShared(&g_dc_lock);
    for (unsigned i = 0; i < DC_LIMIT; ++i) {
        if (dc && g_paint_dcs[i].dc == dc) {
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_dc_lock);
    return found;
}

static int transform_dc(HDC dc)
{
    if (!g_dc_transform || !dc) {
        return 0;
    }
    AcquireSRWLockExclusive(&g_dc_lock);
    unsigned free_slot = DC_LIMIT;
    for (unsigned i = 0; i < DC_LIMIT; ++i) {
        if (g_paint_dcs[i].dc == dc) {
            ReleaseSRWLockExclusive(&g_dc_lock);
            return 0;
        }
        if (!g_paint_dcs[i].dc) {
            free_slot = i;
        }
    }
    if (free_slot == DC_LIMIT) {
        ReleaseSRWLockExclusive(&g_dc_lock);
        return scale_error(NULL, "paint DC registry capacity");
    }
    int graphics = SetGraphicsMode(dc, GM_ADVANCED);
    XFORM transform = {(FLOAT)g_factor, 0, 0, (FLOAT)g_factor, 0, 0};
    int stretch = 0;
    if (graphics && SetWorldTransform(dc, &transform)) {
        stretch = SetStretchBltMode(dc, COLORONCOLOR);
    }
    if (stretch) {
        g_paint_dcs[free_slot].dc = dc;
        g_paint_dcs[free_slot].graphics = graphics;
        g_paint_dcs[free_slot].stretch = stretch;
    } else if (graphics) {
        ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
        SetGraphicsMode(dc, graphics);
    }
    ReleaseSRWLockExclusive(&g_dc_lock);
    if (!stretch) {
        scale_error(NULL, "paint DC transform");
    }
    return stretch != 0;
}

static void restore_dc(HDC dc)
{
    AcquireSRWLockExclusive(&g_dc_lock);
    for (unsigned i = 0; i < DC_LIMIT; ++i) {
        paint_dc *entry = &g_paint_dcs[i];
        if (!dc || entry->dc != dc) {
            continue;
        }
        BOOL identity = ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
        int graphics = SetGraphicsMode(dc, entry->graphics);
        int stretch = SetStretchBltMode(dc, entry->stretch);
        memset(entry, 0, sizeof(*entry));
        if (!identity || !graphics || !stretch) {
            scale_error(NULL, "paint DC restore");
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_dc_lock);
}

static void logical_rect(RECT *rect)
{
    rect->left = logical_coordinate(rect->left);
    rect->top = logical_coordinate(rect->top);
    rect->right = rect->right / g_factor + (rect->right % g_factor > 0);
    rect->bottom = rect->bottom / g_factor + (rect->bottom % g_factor > 0);
}

/* Control WndProcs already paint against physical client geometry. */
static int transform_message_dc(WNDPROC previous)
{
    if (g_dc_transform_controls) {
        return 1;
    }
    HMODULE module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)previous, &module)) {
        return 1;
    }
    return module != GetModuleHandleA("MFC40.DLL") && module != GetModuleHandleA("Anibtn32.ocx");
}

static LRESULT paint_message(WNDPROC previous, BOOL unicode, HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    DWORD error = GetLastError();
    install_mfc40();
    HDC owned = NULL;
    DRAWITEMSTRUCT *item = NULL;
    RECT saved = {0};
    if (g_dc_transform && message == WM_ERASEBKGND && transform_message_dc(previous)) {
        WCHAR name[64];
        if (GetClassNameW(hwnd, name, ARRAYSIZE(name)) &&
            (!wcsncmp(name, L"Afx", 3) || !wcscmp(name, L"#32770")) && transform_dc((HDC)wp)) {
            owned = (HDC)wp;
        }
    } else if (g_dc_transform && message == WM_DRAWITEM && lp && transform_message_dc(previous)) {
        DRAWITEMSTRUCT *data = (DRAWITEMSTRUCT *)lp;
        if (scaled_view(data->hwndItem) && transform_dc(data->hDC)) {
            owned = data->hDC;
            item = data;
            saved = item->rcItem;
            logical_rect(&item->rcItem);
        }
    }
    LRESULT result = 0;
    SetLastError(error);
    __try {
        result = unicode ? CallWindowProcW(previous, hwnd, message, wp, lp)
                         : CallWindowProcA(previous, hwnd, message, wp, lp);
    } __finally {
        error = GetLastError();
        if (item) {
            item->rcItem = saved;
        }
        if (owned) {
            restore_dc(owned);
        }
        SetLastError(error);
    }
    return result;
}

typedef void(__fastcall *mouse_fn)(void *, void *, UINT, int, int);

typedef struct {
    DWORD table_rva;
    DWORD message;
    DWORD handler_rva;
    mouse_fn thunk;
} mouse_route;

static void dispatch_mouse(unsigned index, void *self, UINT flags, int x, int y);

/* AfxSig_vwp: ECX=self, EDX ignored; stack=flags,x,y; callee pops 12.
 * Original thiscall has the same layout. Message-map dispatch comes from MFC42;
 * no caller check is needed. Direct internal calls still reach original code. */
#define MOUSE_THUNK(n)                                                                                       \
    static void __fastcall mouse_##n(void *self, void *unused_edx, UINT flags, int x, int y)                 \
    {                                                                                                        \
        (void)unused_edx;                                                                                    \
        dispatch_mouse(n, self, flags, x, y);                                                                \
    }

MOUSE_THUNK(0)
MOUSE_THUNK(1)
MOUSE_THUNK(2)
MOUSE_THUNK(3)
MOUSE_THUNK(4)
MOUSE_THUNK(5)
MOUSE_THUNK(6)
MOUSE_THUNK(7)
MOUSE_THUNK(8)
MOUSE_THUNK(9)
MOUSE_THUNK(10)
MOUSE_THUNK(11)
MOUSE_THUNK(12)
MOUSE_THUNK(13)
MOUSE_THUNK(14)
MOUSE_THUNK(15)
MOUSE_THUNK(16)
MOUSE_THUNK(17)
MOUSE_THUNK(18)
MOUSE_THUNK(19)
MOUSE_THUNK(20)
MOUSE_THUNK(21)
MOUSE_THUNK(22)
MOUSE_THUNK(23)
MOUSE_THUNK(24)
MOUSE_THUNK(25)
MOUSE_THUNK(26)
MOUSE_THUNK(27)
MOUSE_THUNK(28)
MOUSE_THUNK(29)
MOUSE_THUNK(30)
MOUSE_THUNK(31)
MOUSE_THUNK(32)

/* Generated from tow_launcher/input_handlers.json; hexadecimal values. */
static const mouse_route g_mouse_routes[] = {
    {0xa1368, 0x201, 0xa8a0, mouse_0},   {0xa1380, 0x207, 0xa900, mouse_1},
    {0xa1398, 0x204, 0xa960, mouse_2},   {0xa1c70, 0x200, 0x1bf80, mouse_3},
    {0xa1c88, 0x201, 0x1c160, mouse_4},  {0xa1ca0, 0x202, 0x1c1b0, mouse_5},
    {0xa2368, 0x200, 0x1fdf0, mouse_6},  {0xa2380, 0x201, 0x20750, mouse_7},
    {0xa3c58, 0x201, 0x30890, mouse_8},  {0xa3c70, 0x200, 0x309e0, mouse_9},
    {0xa4188, 0x201, 0x3a8b0, mouse_10}, {0xa41b8, 0x204, 0x3bad0, mouse_11},
    {0xa41d0, 0x200, 0x3ab10, mouse_12}, {0xa41e8, 0x202, 0x3abe0, mouse_13},
    {0xa4200, 0x203, 0x3aa30, mouse_14}, {0xa49b0, 0x200, 0x48d60, mouse_15},
    {0xa49c8, 0x202, 0x48e20, mouse_16}, {0xae0b8, 0x201, 0x628b0, mouse_17},
    {0xaf710, 0x201, 0x6e820, mouse_18}, {0xaf8a8, 0x201, 0x75910, mouse_19},
    {0xaf8c0, 0x200, 0x76b60, mouse_20}, {0xb01d0, 0x201, 0x814a0, mouse_21},
    {0xb01e8, 0x202, 0x814f0, mouse_22}, {0xb0200, 0x200, 0x81610, mouse_23},
    {0xb0230, 0x204, 0x81590, mouse_24}, {0xb0b38, 0x202, 0x8a9e0, mouse_25},
    {0xb0b68, 0x200, 0x8b670, mouse_26}, {0xb0b80, 0x201, 0x8b9d0, mouse_27},
    {0xb1040, 0x201, 0x936b0, mouse_28}, {0xb1058, 0x204, 0x93c20, mouse_29},
    {0xb1070, 0x205, 0x93dc0, mouse_30}, {0xb1088, 0x200, 0x93df0, mouse_31},
    {0xb10b8, 0x202, 0x93a80, mouse_32},
};

static void dispatch_mouse(unsigned index, void *self, UINT flags, int x, int y)
{
    DWORD error = GetLastError();
    const mouse_route *route = &g_mouse_routes[index];
    if (route->handler_rva != DRAG_MOVE_RVA && route->handler_rva != DRAG_UP_RVA && self) {
        HWND hwnd = NULL;
        __try {
            hwnd = *(HWND *)((BYTE *)self + CWND_HWND_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hwnd = NULL;
        }
        if (hwnd && scaled_view(hwnd)) {
            x = logical_coordinate(x);
            y = logical_coordinate(y);
        }
    }
    SetLastError(error);
    mouse_fn original = (mouse_fn)(uintptr_t)(GAME_BASE + route->handler_rva);
    original(self, NULL, flags, x, y);
}

static int install_mouse_handlers(void)
{
    /* Verify every complete entry before patching any .rdata pointer. */
    for (unsigned i = 0; i < ARRAYSIZE(g_mouse_routes); ++i) {
        const mouse_route *route = &g_mouse_routes[i];
        DWORD expected[6] = {route->message, 0, 0, 0, 0x31, GAME_BASE + route->handler_rva};
        if (!patch_check((void *)(uintptr_t)(GAME_BASE + route->table_rva), (const uint8_t *)expected,
                         sizeof(expected))) {
            log_line("scaling: mouse map verification failed at RVA 0x%lx", route->table_rva);
            log_line("scaling: mouse handlers patched 0/33");
            return 0;
        }
    }
    unsigned installed = 0;
    for (; installed < ARRAYSIZE(g_mouse_routes); ++installed) {
        const mouse_route *route = &g_mouse_routes[installed];
        DWORD *slot = (DWORD *)(uintptr_t)(GAME_BASE + route->table_rva + 20);
        int result = iat_replace(slot, (void *)(uintptr_t)(GAME_BASE + route->handler_rva),
                                 (void *)(uintptr_t)route->thunk);
        if (result != IAT_OK) {
            log_line("scaling: mouse pointer patch failed at RVA 0x%lx (reason %d)", route->table_rva,
                     result);
            break;
        }
    }
    log_line("scaling: mouse handlers patched %u/33", installed);
    return installed == ARRAYSIZE(g_mouse_routes);
}

static int install_mouse_apis(void)
{
    const char *names[] = {"GetCursorPos", "SetCursorPos", "ClipCursor"};
    void *hooks[] = {(void *)(uintptr_t)logical_cursor, (void *)(uintptr_t)physical_cursor,
                     (void *)(uintptr_t)physical_clip};
    unsigned installed = 0;
    for (unsigned i = 0; i < ARRAYSIZE(names); ++i) {
        DWORD *slot = iat_find_slot("user32.dll", names[i]);
        if (!slot || !*slot) {
            log_line("scaling: mouse import lookup failed: %s (skipped)", names[i]);
            continue;
        }
        void *original = (void *)(uintptr_t)*slot;
        if (i == 0) {
            g_cursor = (cursor_fn)(uintptr_t)original;
        } else if (i == 1) {
            g_set_cursor = (set_cursor_fn)(uintptr_t)original;
        } else {
            g_clip_cursor = (clip_cursor_fn)(uintptr_t)original;
        }
        int result = iat_replace(slot, original, hooks[i]);
        if (result != IAT_OK) {
            log_line("scaling: mouse import patch failed: %s (reason %d)", names[i], result);
            continue;
        }
        ++installed;
        log_line("scaling: mouse import installed: %s", names[i]);
    }
    return installed == ARRAYSIZE(names);
}

typedef struct {
    HFONT original;
    HFONT scaled;
} scale_font;

/* Fonts stay alive until process exit: controls or DCs may still select them. */
static scale_font g_font_cache[FONT_LIMIT];
static SRWLOCK g_font_lock = SRWLOCK_INIT;

static int own_font(HFONT font)
{
    int found = 0;
    AcquireSRWLockShared(&g_font_lock);
    for (unsigned i = 0; i < FONT_LIMIT; ++i) {
        if (font && g_font_cache[i].scaled == font) {
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_font_lock);
    return found;
}

static HFONT create_scaled_font(HFONT font)
{
    LOGFONTW description;
    if (GetObjectW(font, sizeof(description), &description) != sizeof(description) || !description.lfHeight) {
        return NULL;
    }
    /* Reject malformed metrics instead of overflowing signed LONGs. */
    if (description.lfHeight > LONG_MAX / g_factor || description.lfHeight < LONG_MIN / g_factor ||
        description.lfWidth > LONG_MAX / g_factor || description.lfWidth < LONG_MIN / g_factor) {
        return NULL;
    }
    description.lfHeight *= g_factor;
    description.lfWidth *= g_factor;
    if (wcscmp(description.lfFaceName, L"MS Sans Serif") == 0) {
        wcscpy_s(description.lfFaceName, LF_FACESIZE, L"Microsoft Sans Serif");
    }
    return CreateFontIndirectW(&description);
}

static HFONT scaled_font(HFONT font)
{
    HFONT result = NULL;
    AcquireSRWLockExclusive(&g_font_lock);
    for (unsigned i = 0; i < FONT_LIMIT; ++i) {
        scale_font *entry = &g_font_cache[i];
        if (entry->original == font) {
            result = entry->scaled;
            break;
        }
        if (!entry->original) {
            result = create_scaled_font(font);
            if (result) {
                entry->original = font;
                entry->scaled = result;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_font_lock);
    return result;
}

static int text_control(HWND hwnd)
{
    WCHAR name[64];
    if (!GetClassNameW(hwnd, name, ARRAYSIZE(name))) {
        return 0;
    }
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (_wcsicmp(name, L"Static") == 0) {
        return (style & SS_TYPEMASK) != SS_OWNERDRAW;
    }
    if (_wcsicmp(name, L"Button") == 0) {
        return (style & BS_TYPEMASK) != BS_OWNERDRAW;
    }
    return _wcsicmp(name, L"Edit") == 0 || _wcsicmp(name, L"ListBox") == 0 ||
           _wcsicmp(name, L"ComboBox") == 0;
}

static WPARAM child_font(scale_child *child, WPARAM wp)
{
    HFONT original = (HFONT)wp;
    if (original && own_font(original)) {
        return wp;
    }
    HFONT scaled = original ? scaled_font(original) : NULL;
    /* A reset or a failed replacement must not expose a stale original font. */
    child->original_font = scaled ? original : NULL;
    return scaled ? (WPARAM)scaled : wp;
}

static void enlarge_font(scale_child *child)
{
    if (!g_fonts || !text_control(child->hwnd)) {
        return;
    }
    child->text_font = 1;
    HFONT original = (HFONT)SendMessageW(child->hwnd, WM_GETFONT, 0, 0);
    if (!original || own_font(original)) {
        return;
    }
    WPARAM scaled = child_font(child, (WPARAM)original);
    if (scaled != (WPARAM)original) {
        SendMessageW(child->hwnd, WM_SETFONT, scaled, TRUE);
    }
}

/* Child WndProc uses four stack arguments, stdcall, LRESULT in EAX. */
static LRESULT CALLBACK child_window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    scale_child *child = find_child(hwnd);
    if (!child) {
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    if (message == WM_GETFONT && child->text_font && child->original_font) {
        return (LRESULT)child->original_font;
    }
    if (message == WM_SETFONT && child->text_font) {
        wp = child_font(child, wp);
    }
    LPARAM forwarded = lp;
    if (message == WM_SIZE && wp != SIZE_MINIMIZED && (g_scaling_child || g_moving_child == hwnd)) {
        forwarded = MAKELPARAM(logical_coordinate(LOWORD(lp)), logical_coordinate(HIWORD(lp)));
    }
    LRESULT result = paint_message(child->previous, child->unicode, hwnd, message, wp, forwarded);
    if (message == WM_NCDESTROY) {
        forget_dialog(hwnd);
        memset(child, 0, sizeof(*child));
    }
    return result;
}

static int relative_child(HWND hwnd, int rect[4])
{
    RECT outer;
    POINT origin = {0, 0};
    if (!GetWindowRect(hwnd, &outer) || !ClientToScreen(GetParent(hwnd), &origin)) {
        return 0;
    }
    rect[0] = outer.left - origin.x;
    rect[1] = outer.top - origin.y;
    rect[2] = outer.right - outer.left;
    rect[3] = outer.bottom - outer.top;
    return 1;
}

static scale_child *register_child(HWND hwnd, const int rect[4])
{
    for (unsigned i = 0; i < CHILD_LIMIT; ++i) {
        scale_child *child = &g_children[i];
        if (child->hwnd) {
            continue;
        }
        child->hwnd = hwnd;
        child->unicode = IsWindowUnicode(hwnd);
        memcpy(child->logical, rect, sizeof(child->logical));
        memcpy(child->physical, rect, sizeof(child->physical));
        LONG_PTR previous = child->unicode
                                ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)child_window_proc)
                                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)child_window_proc);
        if (!previous) {
            memset(child, 0, sizeof(*child));
            scale_error(NULL, "child subclass");
            return NULL;
        }
        child->previous = (WNDPROC)previous;
        return child;
    }
    return NULL;
}

static void scale_child_window(HWND hwnd)
{
    WCHAR name[64];
    if (find_target(hwnd) || (GetClassNameW(hwnd, name, ARRAYSIZE(name)) && wcscmp(name, L"#32770") == 0)) {
        return;
    }
    int now[4];
    if (!relative_child(hwnd, now)) {
        return;
    }
    scale_child *child = find_child(hwnd);
    if (!child) {
        child = register_child(hwnd, now);
        if (!child) {
            return;
        }
        enlarge_font(child);
    } else {
        if (memcmp(now, child->physical, sizeof(now)) == 0) {
            return;
        }
        /* A game reset may change only the position or only the size. */
        for (unsigned part = 0; part < 4; part += 2) {
            if (now[part] != child->physical[part] || now[part + 1] != child->physical[part + 1]) {
                child->logical[part] = now[part];
                child->logical[part + 1] = now[part + 1];
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        child->physical[i] = child->logical[i] * g_factor;
    }
    int previous = g_scaling_child;
    g_scaling_child = 1;
    BOOL ok = SetWindowPos(hwnd, NULL, child->physical[0], child->physical[1], child->physical[2],
                           child->physical[3], SWP_NOZORDER | SWP_NOACTIVATE);
    g_scaling_child = previous;
    if (!ok) {
        scale_error(NULL, "child SetWindowPos");
    }
    /* Keep the actual rectangle, including control-imposed constraints. */
    if (child->hwnd == hwnd) {
        relative_child(hwnd, child->physical);
    }
}

static void walk_children(HWND parent, unsigned depth)
{
    for (HWND hwnd = GetWindow(parent, GW_CHILD); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)) {
            continue;
        }
        scale_child_window(hwnd);
        if (depth < CHILD_DEPTH) {
            walk_children(hwnd, depth + 1);
        }
    }
}

static void sweep_children(HWND root)
{
    install_mfc40();
    if (!root || GetWindowThreadProcessId(root, NULL) != GetCurrentThreadId()) {
        return;
    }
    for (unsigned i = 0; i < CHILD_LIMIT; ++i) {
        if (g_children[i].hwnd && !IsWindow(g_children[i].hwnd)) {
            memset(&g_children[i], 0, sizeof(g_children[i]));
        }
    }
    walk_children(root, 1);
}

typedef BOOL(WINAPI *window_rect_fn)(HWND, LPRECT);
typedef BOOL(WINAPI *point_fn)(HWND, LPPOINT);
typedef BOOL(WINAPI *position_fn)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI *move_fn)(HWND, int, int, int, int, BOOL);

static window_rect_fn g_window_rect;
static point_fn g_to_screen;
static point_fn g_to_client;
static position_fn g_position[3];
static move_fn g_move[3];

static void logical_screen(HWND hwnd, POINT *point)
{
    POINT origin = {0, 0};
    if (ClientToScreen(GetAncestor(hwnd, GA_ROOT), &origin)) {
        point->x = origin.x + logical_coordinate(point->x - origin.x);
        point->y = origin.y + logical_coordinate(point->y - origin.y);
    }
}

static BOOL WINAPI child_window_rect(HWND hwnd, LPRECT rect)
{
    BOOL ok = g_window_rect(hwnd, rect);
    DWORD error = GetLastError();
    if (ok && rect && find_child(hwnd)) {
        POINT first = {rect->left, rect->top};
        POINT last = {rect->right, rect->bottom};
        logical_screen(hwnd, &first);
        logical_screen(hwnd, &last);
        SetRect(rect, first.x, first.y, last.x, last.y);
    }
    SetLastError(error);
    return ok;
}

static BOOL child_point(HWND hwnd, LPPOINT point, int screen)
{
    POINT input = {0, 0};
    if (point) {
        input = *point;
    }
    BOOL ok = screen ? g_to_screen(hwnd, point) : g_to_client(hwnd, point);
    DWORD error = GetLastError();
    if (ok && point && find_child(hwnd)) {
        POINT origin = {0, 0};
        if (ClientToScreen(hwnd, &origin)) {
            logical_screen(hwnd, &origin);
            int sign = screen ? 1 : -1;
            point->x = input.x + sign * origin.x;
            point->y = input.y + sign * origin.y;
        }
    }
    SetLastError(error);
    return ok;
}

static BOOL WINAPI child_to_screen(HWND hwnd, LPPOINT point)
{
    return child_point(hwnd, point, 1);
}

static BOOL WINAPI child_to_client(HWND hwnd, LPPOINT point)
{
    return child_point(hwnd, point, 0);
}

static int child_move_rect(HWND hwnd, int rect[4], UINT flags)
{
    scale_child *child = find_child(hwnd);
    if (g_scaling_child || !child || child->dialog) {
        return 0;
    }
    int changed = 0;
    for (unsigned part = 0; part < 4; part += 2) {
        UINT skip = part == 0 ? SWP_NOMOVE : SWP_NOSIZE;
        if ((flags & skip) ||
            (rect[part] == child->physical[part] && rect[part + 1] == child->physical[part + 1])) {
            continue;
        }
        child->logical[part] = rect[part];
        child->logical[part + 1] = rect[part + 1];
        child->physical[part] = rect[part] * g_factor;
        child->physical[part + 1] = rect[part + 1] * g_factor;
        changed = 1;
    }
    if (changed) {
        memcpy(rect, child->physical, sizeof(child->physical));
    }
    return changed;
}

static BOOL position_child(unsigned index, HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    DWORD error = GetLastError();
    int rect[4] = {x, y, w, h};
    install_mfc40();
    int changed = child_move_rect(hwnd, rect, flags);
    HWND previous = g_moving_child;
    if (changed) {
        g_moving_child = hwnd;
    }
    scale_target *target = find_target(hwnd);
    WINDOWPOS position = {hwnd, after, rect[0], rect[1], rect[2], rect[3], flags};
    if (g_borderless && target && target->active) {
        layout_position(target, &position);
    }
    SetLastError(error);
    BOOL ok = g_position[index](hwnd, position.hwndInsertAfter, position.x, position.y, position.cx,
                                position.cy, position.flags);
    error = GetLastError();
    g_moving_child = previous;
    if (target && target->active) {
        sync_backdrop(target);
    }
    center_dialog(hwnd);
    SetLastError(error);
    return ok;
}

static BOOL move_child(unsigned index, HWND hwnd, int x, int y, int w, int h, BOOL repaint)
{
    DWORD error = GetLastError();
    int rect[4] = {x, y, w, h};
    int changed = child_move_rect(hwnd, rect, 0);
    HWND previous = g_moving_child;
    if (changed) {
        g_moving_child = hwnd;
    }
    SetLastError(error);
    BOOL ok = g_move[index](hwnd, rect[0], rect[1], rect[2], rect[3], repaint);
    error = GetLastError();
    g_moving_child = previous;
    SetLastError(error);
    return ok;
}

/* IAT wrappers retain each module's original API and stdcall stack layout. */
#define CHILD_MOVE_WRAPPERS(n)                                                                               \
    static BOOL WINAPI position_##n(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags)           \
    {                                                                                                        \
        return position_child(n, hwnd, after, x, y, w, h, flags);                                            \
    }                                                                                                        \
    static BOOL WINAPI move_##n(HWND hwnd, int x, int y, int w, int h, BOOL repaint)                         \
    {                                                                                                        \
        return move_child(n, hwnd, x, y, w, h, repaint);                                                     \
    }

CHILD_MOVE_WRAPPERS(0)
CHILD_MOVE_WRAPPERS(1)
CHILD_MOVE_WRAPPERS(2)

static void install_child_apis(void)
{
    const char *modules[2] = {"TOW.exe", "MFC42.DLL"};
    const char *names[5] = {"SetWindowPos", "MoveWindow", "GetWindowRect", "ClientToScreen",
                            "ScreenToClient"};
    void *hooks[2][5] = {{(void *)(uintptr_t)position_0, (void *)(uintptr_t)move_0,
                          (void *)(uintptr_t)child_window_rect, (void *)(uintptr_t)child_to_screen,
                          (void *)(uintptr_t)child_to_client},
                         {(void *)(uintptr_t)position_1, (void *)(uintptr_t)move_1, NULL, NULL, NULL}};
    for (unsigned module = 0; module < 2; ++module) {
        HMODULE handle = module ? GetModuleHandleA(modules[module]) : GetModuleHandleA(NULL);
        for (unsigned api = 0; api < (module ? 2u : 5u); ++api) {
            DWORD *slot = iat_find_slot_in(handle, "user32.dll", names[api]);
            if (!slot || !*slot) {
                log_line("scaling: child import lookup failed: %s!%s (skipped)", modules[module], names[api]);
                continue;
            }
            void *original = (void *)(uintptr_t)*slot;
            if (api == 0) {
                g_position[module] = (position_fn)(uintptr_t)original;
            } else if (api == 1) {
                g_move[module] = (move_fn)(uintptr_t)original;
            } else if (api == 2) {
                g_window_rect = (window_rect_fn)(uintptr_t)original;
            } else if (api == 3) {
                g_to_screen = (point_fn)(uintptr_t)original;
            } else {
                g_to_client = (point_fn)(uintptr_t)original;
            }
            int result = iat_replace(slot, original, hooks[module][api]);
            log_line("scaling: child import %s!%s %s (reason %d)", modules[module], names[api],
                     result == IAT_OK ? "installed" : "failed", result);
        }
    }
}

static LRESULT CALLBACK backdrop_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATEANDEAT;
    }
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorA(NULL, IDC_ARROW));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static HWND create_backdrop(scale_target *target)
{
    /* Window creation and destruction belong to the target's UI thread. */
    if (GetWindowThreadProcessId(target->hwnd, NULL) != GetCurrentThreadId()) {
        return NULL;
    }
    HMODULE instance = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)backdrop_proc, &instance)) {
        scale_error(target, "backdrop module");
        return NULL;
    }
    WNDCLASSW window_class = {0};
    window_class.lpfnWndProc = backdrop_proc;
    window_class.hInstance = instance;
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window_class.lpszClassName = BACKDROP_CLASS;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        scale_error(target, "backdrop class");
        return NULL;
    }
    /* Unowned popup: an owned window would be kept ABOVE its game owner. */
    HWND backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, BACKDROP_CLASS, L"", WS_POPUP, 0, 0,
                                    0, 0, NULL, NULL, instance, NULL);
    if (!backdrop) {
        scale_error(target, "backdrop creation");
    }
    return backdrop;
}

static void sync_backdrop(scale_target *target)
{
    if (target->dropping_topmost) {
        return;
    }
    if (!target->fullscreen || IsIconic(target->hwnd) || !IsWindowVisible(target->hwnd)) {
        if (target->backdrop) {
            ShowWindow(target->backdrop, SW_HIDE);
        }
        return;
    }
    if (!target->backdrop) {
        target->backdrop = create_backdrop(target);
        if (!target->backdrop) {
            return;
        }
    }
    MONITORINFO info = {0};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromWindow(target->hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        ShowWindow(target->backdrop, SW_HIDE);
        scale_error(target, "backdrop monitor");
        return;
    }
    LONG backdrop_topmost = GetWindowLongW(target->backdrop, GWL_EXSTYLE) & WS_EX_TOPMOST;
    if (backdrop_topmost) {
        if (!SetWindowPos(target->backdrop, HWND_NOTOPMOST, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)) {
            scale_error(target, "backdrop topmost band");
            return;
        }
    }
    /* Both windows stay non-topmost; insert directly below the game without activation. */
    if (!SetWindowPos(target->backdrop, target->hwnd, info.rcMonitor.left, info.rcMonitor.top,
                      info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        scale_error(target, "backdrop position");
    }
}

/* Ordinary stdcall WndProc: forwards to the original ANSI/Unicode procedure.
 * The compiler preserves callee-saved registers and pops the four arguments. */
static LRESULT CALLBACK scale_window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    scale_target *target = find_target(hwnd);
    if (!target) {
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    if (g_log_windows && target->active && target->fullscreen && main_target(target) && g_nc_logs < 20 &&
        (message == WM_NCACTIVATE || message == WM_NCPAINT || message == WM_ACTIVATE)) {
        DWORD error = GetLastError();
        ++g_nc_logs;
        const char *name = message == WM_NCACTIVATE ? "WM_NCACTIVATE"
                           : message == WM_NCPAINT  ? "WM_NCPAINT"
                                                    : "WM_ACTIVATE";
        log_line("scaling: nc message=%s hwnd=%p wParam=0x%lx style=0x%08lx count=%u", name, (void *)hwnd,
                 (unsigned long)wp, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), g_nc_logs);
        SetLastError(error);
    }
    if (target->active && message == WM_NCCALCSIZE && target->fullscreen) {
        /* Keep the attached menu, but reserve no non-client area for it. */
        return 0;
    }
    LPARAM forwarded = lp;
    if (target->active && message == WM_SIZE && wp != SIZE_MINIMIZED) {
        forwarded = MAKELPARAM(LOGICAL_WIDTH, LOGICAL_HEIGHT);
    }
    LRESULT result = paint_message(target->previous, target->unicode, hwnd, message, wp, forwarded);

    /* Override the game's fixed-size constraints after its handler runs. */
    if (target->active && target->width && lp) {
        if (message == WM_GETMINMAXINFO) {
            target_bounds(target);
            MINMAXINFO *info = (MINMAXINFO *)lp;
            info->ptMaxSize.x = target->width;
            info->ptMaxSize.y = target->height;
            info->ptMinTrackSize = info->ptMaxSize;
            info->ptMaxTrackSize = info->ptMaxSize;
        } else if (message == WM_WINDOWPOSCHANGING) {
            WINDOWPOS *position = (WINDOWPOS *)lp;
            if (g_borderless) {
                layout_position(target, position);
            }
            if (!(position->flags & SWP_NOSIZE) && !IsIconic(hwnd)) {
                position->cx = target->width;
                position->cy = target->height;
            }
        }
    }
    if (target->active &&
        (message == WM_WINDOWPOSCHANGED || message == WM_SIZE || message == WM_SHOWWINDOW)) {
        sync_backdrop(target);
    }
    if (message == WM_NCDESTROY) {
        if (target->backdrop) {
            DestroyWindow(target->backdrop);
        }
        forget_dialog(hwnd);
        memset(target, 0, sizeof(*target));
    }
    return result;
}

static int window_long(scale_target *target, int index, LONG_PTR *value)
{
    SetLastError(ERROR_SUCCESS);
    *value = GetWindowLongPtrW(target->hwnd, index);
    if (!*value && GetLastError() != ERROR_SUCCESS) {
        return scale_error(target, "GetWindowLongPtr");
    }
    return 1;
}

static int target_bounds(scale_target *target)
{
    LONG_PTR style;
    LONG_PTR extended;
    if (!window_long(target, GWL_STYLE, &style) || !window_long(target, GWL_EXSTYLE, &extended)) {
        return 0;
    }
    RECT bounds = {0, 0, LOGICAL_WIDTH * g_factor, LOGICAL_HEIGHT * g_factor};
    if (target->fullscreen) {
        target->width = bounds.right;
        target->height = bounds.bottom;
        return 1;
    }
    /* NULL from GetMenu is the documented no-menu case. */
    if (!AdjustWindowRectEx(&bounds, (DWORD)style, GetMenu(target->hwnd) != NULL, (DWORD)extended)) {
        return scale_error(target, "AdjustWindowRectEx");
    }
    target->width = bounds.right - bounds.left;
    target->height = bounds.bottom - bounds.top;
    return 1;
}

static int drop_target_topmost(scale_target *target)
{
    if (!(GetWindowLongW(target->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        return 1;
    }
    /* The nested WM_WINDOWPOSCHANGING must retain HWND_NOTOPMOST. */
    target->dropping_topmost = 1;
    BOOL ok =
        SetWindowPos(target->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    target->dropping_topmost = 0;
    if (!ok) {
        return scale_error(target, "game topmost band");
    }
    return 1;
}

static int layout_position(scale_target *target, WINDOWPOS *position)
{
    if (target->dropping_topmost) {
        return 1;
    }
    if (!target_bounds(target)) {
        return 0;
    }
    if (IsIconic(target->hwnd)) {
        return 1;
    }
    HWND root = GetAncestor(target->hwnd, GA_ROOT);
    if (!root) {
        return scale_error(target, "GetAncestor");
    }
    WCHAR name[64];
    if (!GetClassNameW(target->hwnd, name, 64)) {
        return scale_error(target, "GetClassName");
    }
    if (root == target->hwnd && wcscmp(name, L"#32770") != 0) {
        HMONITOR monitor = MonitorFromWindow(target->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {0};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info)) {
            return scale_error(target, "monitor work area");
        }
        RECT area = target->fullscreen ? info.rcMonitor : info.rcWork;
        int dx = (area.right - area.left - target->width) / 2;
        int dy = (area.bottom - area.top - target->height) / 2;
        position->x = area.left + (target->fullscreen ? dx : max(0, dx));
        position->y = area.top + (target->fullscreen ? dy : max(0, dy));
        position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        /* Touch the z-order only to drop a topmost state. Forcing HWND_NOTOPMOST/HWND_TOP on every layout
         * lifted the game above its own modal dialogs (the Escape menu stayed hidden). */
        if (g_borderless && (GetWindowLongW(target->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            position->hwndInsertAfter = HWND_NOTOPMOST;
            position->flags &= ~SWP_NOZORDER;
            if (target->fullscreen && drop_target_topmost(target)) {
                position->hwndInsertAfter = HWND_TOP;
            }
        }
    }
    position->cx = target->width;
    position->cy = target->height;
    return 1;
}

static int position_target(scale_target *target)
{
    if (IsIconic(target->hwnd)) {
        sync_backdrop(target);
        return 1;
    }
    if (target->laying_out) {
        return 1;
    }
    WINDOWPOS position = {
        target->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED};
    if (!layout_position(target, &position)) {
        return 0;
    }
    target->laying_out = 1;
    BOOL ok = SetWindowPos(target->hwnd, position.hwndInsertAfter, position.x, position.y, position.cx,
                           position.cy, position.flags);
    target->laying_out = 0;
    sync_backdrop(target);
    if (!ok) {
        return scale_error(target, "SetWindowPos");
    }
    return 1;
}

static int set_target_style(scale_target *target, int index, LONG_PTR value)
{
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(target->hwnd, index, value) && GetLastError() != ERROR_SUCCESS) {
        return scale_error(target, "SetWindowLongPtr style");
    }
    return 1;
}

static int apply_borderless(scale_target *target)
{
    if (!main_target(target) || target->fullscreen == g_borderless) {
        return 1;
    }
    if (g_borderless && (!window_long(target, GWL_STYLE, &target->saved_style) ||
                         !window_long(target, GWL_EXSTYLE, &target->saved_exstyle))) {
        return 0;
    }

    /* Set our state before style messages reenter the subclass. Never detach the menu. */
    int previous = target->fullscreen;
    target->fullscreen = g_borderless;
    target->laying_out = 1;
    LONG_PTR style = target->saved_style;
    if (target->fullscreen) {
        style &= ~(LONG_PTR)BORDERLESS_STYLE_MASK;
    }
    int ok = set_target_style(target, GWL_STYLE, style);
    if (ok && !target->fullscreen) {
        ok = set_target_style(target, GWL_EXSTYLE, target->saved_exstyle);
    }
    target->laying_out = 0;
    if (!ok) {
        target->fullscreen = previous;
        LONG_PTR rollback =
            previous ? target->saved_style & ~(LONG_PTR)BORDERLESS_STYLE_MASK : target->saved_style;
        set_target_style(target, GWL_STYLE, rollback);
        position_target(target);
        return 0;
    }
    /* Includes SWP_FRAMECHANGED, normal work-area layout on leave and backdrop sync. */
    return position_target(target);
}

static int resize_target(scale_target *target)
{
    if (!apply_borderless(target)) {
        return 0;
    }
    if (!target_bounds(target)) {
        return 0;
    }
    RECT client;
    if (!GetClientRect(target->hwnd, &client)) {
        return scale_error(target, "GetClientRect physical");
    }
    if (client.right == LOGICAL_WIDTH * g_factor && client.bottom == LOGICAL_HEIGHT * g_factor) {
        return 1;
    }
    if (!target_bounds(target) || !position_target(target)) {
        return 0;
    }
    if (!GetClientRect(target->hwnd, &client) || client.right != LOGICAL_WIDTH * g_factor ||
        client.bottom != LOGICAL_HEIGHT * g_factor) {
        return scale_error(target, "verify resized client");
    }
    return 1;
}

static int subclass_target(scale_target *target)
{
    target->unicode = IsWindowUnicode(target->hwnd);
    SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = target->unicode
                            ? SetWindowLongPtrW(target->hwnd, GWLP_WNDPROC, (LONG_PTR)scale_window_proc)
                            : SetWindowLongPtrA(target->hwnd, GWLP_WNDPROC, (LONG_PTR)scale_window_proc);
    if (!previous) {
        return scale_error(target, "SetWindowLongPtr subclass");
    }
    target->previous = (WNDPROC)previous;
    target->active = 1;
    return 1;
}

static void reject_target(scale_target *target)
{
    if (target->fullscreen) {
        target->fullscreen = 0;
        set_target_style(target, GWL_STYLE, target->saved_style);
        set_target_style(target, GWL_EXSTYLE, target->saved_exstyle);
        position_target(target);
    }
    if (target->backdrop) {
        DestroyWindow(target->backdrop);
        target->backdrop = NULL;
    }
    target->active = 0;
    SetLastError(ERROR_SUCCESS);
    LONG_PTR previous = target->unicode
                            ? SetWindowLongPtrW(target->hwnd, GWLP_WNDPROC, (LONG_PTR)target->previous)
                            : SetWindowLongPtrA(target->hwnd, GWLP_WNDPROC, (LONG_PTR)target->previous);
    if (!previous) {
        /* Keep forwarding state if Windows could not remove our procedure. */
        scale_error(target, "restore window procedure");
        return;
    }
    memset(target, 0, sizeof(*target));
}

static int target_parent(HWND hwnd, HWND *parent)
{
    *parent = NULL;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hwnd, &pid)) {
        return scale_error(NULL, "GetWindowThreadProcessId");
    }
    if (pid != GetCurrentProcessId()) {
        return 0;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) {
        return scale_error(NULL, "GetAncestor target");
    }
    if (root != hwnd) {
        *parent = GetParent(hwnd);
        if (!*parent) {
            return scale_error(NULL, "GetParent target");
        }
    }
    return 1;
}

static scale_target *create_target(HWND hwnd)
{
    RECT client;
    if (!GetClientRect(hwnd, &client)) {
        scale_error(NULL, "GetClientRect candidate");
        return NULL;
    }
    if (!((client.right == LOGICAL_WIDTH && client.bottom == LOGICAL_HEIGHT) ||
          (client.right == LOGICAL_WIDTH * g_factor && client.bottom == LOGICAL_HEIGHT * g_factor))) {
        return NULL;
    }
    scale_target *target = NULL;
    for (unsigned i = 0; i < TARGET_LIMIT; ++i) {
        if (!g_targets[i].hwnd) {
            target = &g_targets[i];
            break;
        }
    }
    if (!target) {
        scale_error(NULL, "live target limit");
        return NULL;
    }
    target->hwnd = hwnd;
    if (!subclass_target(target)) {
        memset(target, 0, sizeof(*target));
        return NULL;
    }
    if (!resize_target(target)) {
        reject_target(target);
        return NULL;
    }
    log_line("scaling: window %p client %dx%d, factor %d", (void *)hwnd, LOGICAL_WIDTH * g_factor,
             LOGICAL_HEIGHT * g_factor, g_factor);
    return target;
}

static scale_target *ensure_target(HWND hwnd, unsigned depth)
{
    if (!hwnd || depth >= TARGET_LIMIT) {
        return NULL;
    }
    HWND parent;
    if (!target_parent(hwnd, &parent) || (parent && !ensure_target(parent, depth + 1))) {
        return NULL;
    }
    scale_target *target = find_target(hwnd);
    if (target) {
        return target->active && resize_target(target) ? target : NULL;
    }
    return create_target(hwnd);
}

static int copy_update_region(scale_target *target, HDC original, HDC fresh)
{
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) {
        return scale_error(target, "CreateRectRgn");
    }
    int status = GetRandomRgn(original, region, SYSRGN);
    int ok = status != -1;
    if (status == 1) {
        POINT origin = {0, 0};
        ok = ClientToScreen(target->hwnd, &origin) && OffsetRgn(region, -origin.x, -origin.y) != ERROR &&
             SelectClipRgn(fresh, region) != ERROR;
    }
    if (!DeleteObject(region)) {
        scale_error(target, "DeleteObject region");
    }
    return ok ? 1 : scale_error(target, "copy update region");
}

static BOOL draw_frame(scale_target *target, HDC dst, HDC src, int sx, int sy, DWORD rop)
{
    HDC fresh = GetDC(target->hwnd);
    if (!fresh) {
        return scale_error(target, "GetDC");
    }
    BOOL ok = FALSE;
    if (copy_update_region(target, dst, fresh)) {
        int previous = SetStretchBltMode(fresh, COLORONCOLOR);
        if (previous) {
            ok = StretchBlt(fresh, 0, 0, LOGICAL_WIDTH * g_factor, LOGICAL_HEIGHT * g_factor, src, sx, sy,
                            LOGICAL_WIDTH, LOGICAL_HEIGHT, rop);
            if (!ok) {
                scale_error(target, "StretchBlt");
            }
            if (!SetStretchBltMode(fresh, previous)) {
                scale_error(target, "restore stretch mode");
            }
        } else {
            scale_error(target, "SetStretchBltMode");
        }
    }
    if (!ReleaseDC(target->hwnd, fresh)) {
        scale_error(target, "ReleaseDC");
    }
    return ok;
}

/* BitBlt ABI: nine stack arguments, stdcall pops 36 bytes, BOOL in EAX.
 * No game code is called except by the normal WndProc message dispatch. */
static BOOL WINAPI frame_blit(HDC dst, int x, int y, int width, int height, HDC src, int sx, int sy,
                              DWORD rop)
{
    if (!g_drawing && x == 0 && y == 0 && width == LOGICAL_WIDTH && height == LOGICAL_HEIGHT &&
        rop == SRCCOPY) {
        BOOL ok = FALSE;
        g_drawing = 1;
        __try {
            HWND hwnd = WindowFromDC(dst);
            if (hwnd) {
                scale_target *target = ensure_target(hwnd, 0);
                if (target) {
                    if (transformed_dc(dst)) {
                        ok = g_bitblt(dst, x, y, width, height, src, sx, sy, rop);
                        sweep_children(GetAncestor(hwnd, GA_ROOT));
                        return ok;
                    }
                    ok = draw_frame(target, dst, src, sx, sy, rop);
                    if (ok) {
                        sweep_children(GetAncestor(hwnd, GA_ROOT));
                    }
                }
            }
        } __finally {
            g_drawing = 0;
        }
        if (ok) {
            return TRUE;
        }
    }
    BOOL result = g_bitblt(dst, x, y, width, height, src, sx, sy, rop);
    if (!result) {
        scale_error(NULL, "original BitBlt");
    }
    return result;
}

static void enable_dpi_awareness(void)
{
    HMODULE user = GetModuleHandleA("user32.dll");
    dpi_aware_fn aware = user ? (dpi_aware_fn)(uintptr_t)GetProcAddress(user, "SetProcessDPIAware") : NULL;
    if (!aware) {
        log_line("scaling: SetProcessDPIAware unavailable; continuing");
        return;
    }
    if (!aware()) {
        log_line("scaling: SetProcessDPIAware failed (error %lu); continuing", GetLastError());
    } else {
        log_line("scaling: process DPI aware");
    }
}

static int auto_factor(void)
{
    POINT point = {0, 0};
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {0};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        scale_error(NULL, "auto monitor work area");
        return 0;
    }
    int best = 0;
    for (int factor = 2; factor <= 4; ++factor) {
        RECT rect = {0, 0, LOGICAL_WIDTH * factor, LOGICAL_HEIGHT * factor};
        if (!AdjustWindowRectEx(&rect, FRAME_STYLE, FALSE, 0)) {
            scale_error(NULL, "auto AdjustWindowRectEx");
            return 0;
        }
        if (rect.right - rect.left <= info.rcWork.right - info.rcWork.left &&
            rect.bottom - rect.top <= info.rcWork.bottom - info.rcWork.top) {
            best = factor;
        }
    }
    log_line("scaling: auto factor %d, work area %ldx%ld (largest fitting framed window)", best,
             info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
    return best;
}

static int read_factor(const char *dir)
{
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.ini") > sizeof(path)) {
        return scale_error(NULL, "ini path length");
    }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), "tow_fix.ini");
    g_dialogs = GetPrivateProfileIntA("scaling", "dialogs", 1, path) != 0;
    g_fonts = GetPrivateProfileIntA("scaling", "fonts", 1, path) != 0;
    g_scale_cursor = GetPrivateProfileIntA("scaling", "cursor", 1, path) != 0;
    g_dc_transform = GetPrivateProfileIntA("scaling", "dc_transform", 1, path) != 0;
    g_dc_transform_controls = GetPrivateProfileIntA("scaling", "dc_transform_controls", 0, path) != 0;
    g_log_windows = GetPrivateProfileIntA("scaling", "log_windows", 0, path) != 0;
    log_line("scaling: dc_transform_controls=%d", g_dc_transform_controls);
    char value[32];
    DWORD length = GetPrivateProfileStringA("scaling", "factor", "0", value, sizeof(value), path);
    if (!length || length == sizeof(value) - 1) {
        log_line("scaling: factor 0, empty or truncated setting");
        return 0;
    }
    if (!strcmp(value, "0")) {
        log_line("scaling: factor 0, disabled by configuration");
        return 0;
    }
    if (strcmp(value, "auto") != 0 && !(length == 1 && value[0] >= '2' && value[0] <= '4')) {
        log_line("scaling: factor 0, invalid setting (expected 0, 2..4 or auto)");
        return 0;
    }
    enable_dpi_awareness();
    if (!strcmp(value, "auto")) {
        return auto_factor();
    }
    int factor = value[0] - '0';
    log_line("scaling: factor %d, fixed by configuration", factor);
    return factor;
}

static int install_scaling(void)
{
    HMODULE main = GetModuleHandleA(NULL);
    HMODULE mfc = GetModuleHandleA("MFC42.DLL");
    DWORD *blit = iat_find_slot("gdi32.dll", "BitBlt");
    DWORD *slots[2] = {iat_find_slot_in(main, "user32.dll", "GetClientRect"),
                       iat_find_slot_in(mfc, "user32.dll", "GetClientRect")};
    if (!main || !mfc || !blit || (uintptr_t)blit != BITBLT_SLOT_ADDRESS || !*blit || !slots[0] ||
        !slots[1] || !*slots[0] || !*slots[1]) {
        return scale_error(NULL, "required module or import lookup");
    }
    BYTE *site = (BYTE *)main + FRAME_CALL_RVA;
    uint8_t expected[6] = {0xff, 0x15};
    uint32_t address = (uint32_t)(uintptr_t)blit;
    memcpy(expected + 2, &address, sizeof(address));
    if (!patch_check(site, expected, sizeof(expected))) {
        return scale_error(NULL, "frame call-site verification");
    }
    g_bitblt = (bitblt_fn)(uintptr_t)*blit;
    void *hooks[2] = {(void *)(uintptr_t)tow_client_rect, (void *)(uintptr_t)mfc_client_rect};
    unsigned installed = 0;
    for (unsigned i = 0; i < 2; ++i) {
        g_client_rect[i] = (client_rect_fn)(uintptr_t)*slots[i];
        if (iat_replace(slots[i], (void *)(uintptr_t)g_client_rect[i], hooks[i]) != IAT_OK) {
            scale_error(NULL, "GetClientRect IAT patch");
            break;
        }
        ++installed;
    }
    uint8_t bytes[6] = {0xe8, 0, 0, 0, 0, 0x90};
    uint32_t relative = (uint32_t)(uintptr_t)frame_blit - (uint32_t)(uintptr_t)site - 5;
    memcpy(bytes + 1, &relative, sizeof(relative));
    if (installed == 2 && patch_write(site, expected, bytes, sizeof(bytes))) {
        log_line("scaling: stage 1 installed");
        return 1;
    }
    while (installed) {
        unsigned i = --installed;
        if (iat_replace(slots[i], hooks[i], (void *)(uintptr_t)g_client_rect[i]) != IAT_OK) {
            scale_error(NULL, "GetClientRect IAT rollback");
        }
    }
    return scale_error(NULL, "stage 1 installation");
}

static void install_stage4(void);

int scaling_active(void)
{
    return g_factor > 0 && g_installed;
}

void scaling_set_borderless(int enabled)
{
    g_borderless = enabled != 0;
    for (unsigned i = 0; i < TARGET_LIMIT; ++i) {
        scale_target *target = &g_targets[i];
        if (target->active) {
            apply_borderless(target);
        }
    }
}

static int borderless_main_active(void)
{
    for (unsigned i = 0; i < TARGET_LIMIT; ++i) {
        scale_target *target = &g_targets[i];
        if (target->active && target->fullscreen && main_target(target)) {
            return 1;
        }
    }
    return 0;
}

typedef HRESULT(WINAPI *dwm_attribute_fn)(HWND, DWORD, PVOID, DWORD);

typedef struct zdump_timer {
    UINT_PTR id;
    HWND shown;
    UINT delay;
    struct zdump_timer *next;
} zdump_timer;

/* Hook callbacks and timers run on the main UI thread. */
static zdump_timer *g_zdump_timers;

static void dump_window_details(const char *role, HWND hwnd, HWND game)
{
    if (!hwnd || !IsWindow(hwnd)) {
        log_line("scaling: zdump details role=%s hwnd=%p unavailable", role, (void *)hwnd);
        return;
    }
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    dwm_attribute_fn attribute =
        dwm ? (dwm_attribute_fn)(uintptr_t)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
    DWORD cloaked = 0;
    char cloak[32] = "n/a";
    /* DWMWA_CLOAKED is attribute 14, including with older SDK headers. */
    if (attribute && SUCCEEDED(attribute(hwnd, 14, &cloaked, sizeof(cloaked)))) {
        wsprintfA(cloak, "0x%lx", cloaked);
    }
    if (dwm) {
        FreeLibrary(dwm);
    }

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    int region_type = region ? GetWindowRgn(hwnd, region) : ERROR;
    if (region) {
        DeleteObject(region);
    }
    RECT client = {0};
    BOOL client_ok = GetClientRect(hwnd, &client);
    RECT rect = {0};
    MONITORINFO info = {0};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    int inside = -1;
    if (GetWindowRect(hwnd, &rect) && GetMonitorInfoW(monitor, &info)) {
        inside = rect.left >= info.rcMonitor.left && rect.top >= info.rcMonitor.top &&
                 rect.right <= info.rcMonitor.right && rect.bottom <= info.rcMonitor.bottom;
    }
    log_line(
        "scaling: zdump details role=%s hwnd=%p style=0x%08lx exstyle=0x%08lx "
        "visible=%d cloaked=%s rgn=%d client_ok=%d client=(%ld,%ld,%ld,%ld) monitor=%p inside_monitor=%d",
        role, (void *)hwnd, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE),
        IsWindowVisible(hwnd), cloak, region_type, client_ok, client.left, client.top, client.right,
        client.bottom, (void *)monitor, inside);
}

static void dump_zorder_window(HWND hwnd, int index, int ours)
{
    WCHAR name[128] = {0};
    char utf8[512] = {0};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), NULL, NULL);
    RECT rect = {0};
    BOOL rect_ok = GetWindowRect(hwnd, &rect);
    log_line("scaling: zdump z=%d hwnd=%p class=\"%s\" process=%s visible=%d iconic=%d "
             "topmost=%d enabled=%d rect_ok=%d rect=(%ld,%ld,%ld,%ld) owner=%p",
             index, (void *)hwnd, utf8, ours ? "our" : "foreign", IsWindowVisible(hwnd), IsIconic(hwnd),
             (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, IsWindowEnabled(hwnd), rect_ok,
             rect.left, rect.top, rect.right, rect.bottom, (void *)GetWindow(hwnd, GW_OWNER));
}

static void dump_zorder(scale_target *target)
{
    int game_z = -1;
    int dialog_z = -1;
    int backdrop_z = -1;
    unsigned foreign = 0;
    int index = 0;
    for (HWND hwnd = GetTopWindow(NULL); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT), ++index) {
        if (hwnd == target->hwnd) {
            game_z = index;
        }
        if (hwnd == g_newest_dialog) {
            dialog_z = index;
        }
        if (hwnd == target->backdrop) {
            backdrop_z = index;
        }
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        int ours = process == GetCurrentProcessId();
        if (ours) {
            dump_zorder_window(hwnd, index, 1);
        } else if (game_z < 0 && foreign < 5 && IsWindowVisible(hwnd)) {
            ++foreign;
            dump_zorder_window(hwnd, index, 0);
        }
    }
    log_line("scaling: zdump input foreground=%p active=%p focus=%p capture=%p thread=%lu",
             (void *)GetForegroundWindow(), (void *)GetActiveWindow(), (void *)GetFocus(),
             (void *)GetCapture(), GetCurrentThreadId());
    dump_window_details("game", target->hwnd, target->hwnd);
    dump_window_details("dialog", g_newest_dialog, target->hwnd);
    log_line("scaling: zdump relative game=%p z=%d dialog=%p z=%d backdrop=%p z=%d "
             "backdrop_above_game=%d backdrop_above_dialog=%d (z=0 is top; -1 is absent)",
             (void *)target->hwnd, game_z, (void *)g_newest_dialog, dialog_z, (void *)target->backdrop,
             backdrop_z, backdrop_z < 0 || game_z < 0 ? -1 : backdrop_z < game_z,
             backdrop_z < 0 || dialog_z < 0 ? -1 : backdrop_z < dialog_z);
}

static void CALLBACK zdump_callback(HWND hwnd, UINT message, UINT_PTR id, DWORD time)
{
    (void)hwnd;
    (void)message;
    (void)time;
    DWORD error = GetLastError();
    KillTimer(NULL, id);
    zdump_timer **link = &g_zdump_timers;
    while (*link && (*link)->id != id) {
        link = &(*link)->next;
    }
    if (*link) {
        zdump_timer *entry = *link;
        *link = entry->next;
        log_line("scaling: zdump begin shown=%p delay_ms=%u timer=%lu", (void *)entry->shown, entry->delay,
                 (unsigned long)id);
        HeapFree(GetProcessHeap(), 0, entry);
        for (unsigned i = 0; i < TARGET_LIMIT; ++i) {
            scale_target *target = &g_targets[i];
            if (g_log_windows && target->hwnd && target->active && target->fullscreen &&
                main_target(target)) {
                dump_zorder(target);
            }
        }
        log_line("scaling: zdump end timer=%lu", (unsigned long)id);
    }
    SetLastError(error);
}

static void schedule_zdump(HWND shown, UINT delay)
{
    zdump_timer *entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
    if (!entry) {
        log_line("scaling: zdump allocation failed delay_ms=%u", delay);
        return;
    }
    entry->shown = shown;
    entry->delay = delay;
    entry->id = SetTimer(NULL, 0, delay, zdump_callback);
    if (!entry->id) {
        log_line("scaling: zdump SetTimer failed delay_ms=%u error=%lu", delay, GetLastError());
        HeapFree(GetProcessHeap(), 0, entry);
        return;
    }
    entry->next = g_zdump_timers;
    g_zdump_timers = entry;
}

static void log_shown_window(HWND hwnd)
{
    if (!g_log_windows || !scaling_active() || !hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return;
    }
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) {
        return;
    }
    WCHAR name[128] = {0};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (borderless_main_active()) {
        if (!wcscmp(name, L"#32770")) {
            g_newest_dialog = hwnd;
        }
        schedule_zdump(hwnd, 500);
        schedule_zdump(hwnd, 2000);
    }
    if (!wcscmp(name, BACKDROP_CLASS) || g_window_logs >= WINDOW_LOG_LIMIT) {
        return;
    }
    /* Reserve the line before querying text, which can reenter window procedures. */
    ++g_window_logs;
    WCHAR text[41] = {0};
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    for (unsigned i = 0; text[i]; ++i) {
        if (text[i] < L' ') {
            text[i] = L' ';
        }
    }
    char class_utf8[512] = {0};
    char text_utf8[164] = {0};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, class_utf8, sizeof(class_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, text_utf8, sizeof(text_utf8), NULL, NULL);

    RECT rect = {0};
    GetWindowRect(hwnd, &rect);
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    DWORD extended = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
    log_line("scaling: window show hwnd=%p class=\"%s\" text=\"%s\" owner=%p "
             "style=0x%08lx exstyle=0x%08lx topmost=%d rect=(%ld,%ld,%ld,%ld) borderless=%d",
             (void *)hwnd, class_utf8, text_utf8, (void *)GetWindow(hwnd, GW_OWNER), style, extended,
             (extended & WS_EX_TOPMOST) != 0, rect.left, rect.top, rect.right, rect.bottom,
             borderless_main_active());
}

static void CALLBACK window_show_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                       DWORD thread, DWORD time)
{
    (void)hook;
    (void)child;
    (void)thread;
    (void)time;
    DWORD error = GetLastError();
    if (event == EVENT_OBJECT_SHOW && object == OBJID_WINDOW) {
        log_shown_window(hwnd);
    }
    SetLastError(error);
}

static void install_window_diagnostics(void)
{
    if (!g_log_windows || !scaling_active() || g_window_hook) {
        return;
    }
    /* Called by tow_init on the main UI thread; include this thread's own events.
     * Out-of-context callbacks use its message loop; the hook lasts until process exit. */
    g_window_hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, window_show_event,
                                    GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    if (!g_window_hook) {
        log_line("scaling: window diagnostics hook failed (error %lu)", GetLastError());
        return;
    }
    log_line("scaling: window diagnostics enabled (limit %d)", WINDOW_LOG_LIMIT);
}

void scaling_apply(const char *dir)
{
    g_factor = read_factor(dir);
    if (g_factor) {
        if (install_scaling()) {
            g_installed = 1;
            install_child_apis();
            install_mouse_apis();
            install_mouse_handlers();
            install_stage4();
            install_window_diagnostics();
        }
    }
}

static void forget_dialog(HWND hwnd)
{
    for (unsigned i = 0; i < ARRAYSIZE(g_centered); ++i) {
        if (g_centered[i] == hwnd) {
            g_centered[i] = NULL;
        }
    }
}

static void center_dialog(HWND hwnd)
{
    if (g_scaling_child || !hwnd) {
        return;
    }
    unsigned free_slot = ARRAYSIZE(g_centered);
    for (unsigned i = 0; i < ARRAYSIZE(g_centered); ++i) {
        if (g_centered[i] == hwnd) {
            return;
        }
        if (!g_centered[i]) {
            free_slot = i;
        }
    }
    WCHAR name[64];
    HWND root = GetAncestor(GetParent(hwnd), GA_ROOT);
    scale_target *target = find_target(root);
    RECT outer;
    RECT client;
    RECT own;
    POINT origin = {0, 0};
    if (free_slot == ARRAYSIZE(g_centered) || !target || !target->active ||
        !GetClassNameW(hwnd, name, ARRAYSIZE(name)) || wcscmp(name, L"#32770") != 0 ||
        !GetWindowRect(hwnd, &outer) || !GetClientRect(root, &client) || !GetClientRect(hwnd, &own) ||
        !ClientToScreen(root, &origin)) {
        return;
    }
    int w = outer.right - outer.left;
    int h = outer.bottom - outer.top;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE;
    int full = (own.right == LOGICAL_WIDTH && own.bottom == LOGICAL_HEIGHT) ||
               (own.right == LOGICAL_WIDTH * g_factor && own.bottom == LOGICAL_HEIGHT * g_factor);
    int enlarge = !full && g_dialogs && own.right > 0 && own.bottom > 0;
    scale_child *child = NULL;
    if (enlarge) {
        int rect[4] = {0, 0, own.right, own.bottom};
        child = register_child(hwnd, rect);
        if (!child) {
            return;
        }
        child->dialog = 1;
        w += own.right * (g_factor - 1);
        h += own.bottom * (g_factor - 1);
        flags &= ~SWP_NOSIZE;
    }
    int x = origin.x + (int)((client.right - w) / 2 - ((client.right - w) % 2 < 0));
    int y = origin.y + (int)((client.bottom - h) / 2 - ((client.bottom - h) % 2 < 0));
    if (full) {
        POINT offset = {0, 0};
        if (!ClientToScreen(hwnd, &offset)) {
            return;
        }
        x = origin.x - (offset.x - outer.left);
        y = origin.y - (offset.y - outer.top);
    }
    g_centered[free_slot] = hwnd;
    g_scaling_child = 1;
    BOOL ok = SetWindowPos(hwnd, NULL, x, y, w, h, flags);
    g_scaling_child = 0;
    if (!ok) {
        forget_dialog(hwnd);
        /* Keep the forwarding subclass but do not advertise failed enlargement. */
        if (child) {
            LONG_PTR restored = child->unicode
                                    ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)child->previous)
                                    : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)child->previous);
            if (restored) {
                memset(child, 0, sizeof(*child));
            }
        }
        scale_error(NULL, "dialog positioning");
        return;
    }
    if (enlarge) {
        sweep_children(hwnd);
    }
}

typedef BOOL(WINAPI *show_fn)(HWND, int);
typedef BOOL(WINAPI *destroy_fn)(HWND);
typedef BOOL(WINAPI *update_fn)(HWND, const RECT *, BOOL);
typedef BOOL(WINAPI *validate_fn)(HWND, const RECT *);
typedef BOOL(WINAPI *redraw_fn)(HWND, const RECT *, HRGN, UINT);
typedef int(WINAPI *clip_box_fn)(HDC, LPRECT);

static show_fn g_show[3];
static destroy_fn g_destroy[3];
static update_fn g_invalidate[2];
static validate_fn g_validate[2];
static redraw_fn g_redraw[2];
static clip_box_fn g_clip_box[2];

static const RECT *update_rect(HWND hwnd, const RECT *rect, RECT *copy)
{
    if (!rect || !scaled_view(hwnd)) {
        return rect;
    }
    SetRect(copy, rect->left * g_factor, rect->top * g_factor, rect->right * g_factor,
            rect->bottom * g_factor);
    return copy;
}

#define DIALOG_WRAPPERS(n)                                                                                   \
    static BOOL WINAPI show_##n(HWND hwnd, int command)                                                      \
    {                                                                                                        \
        install_mfc40();                                                                                     \
        BOOL ok = g_show[n](hwnd, command);                                                                  \
        DWORD error = GetLastError();                                                                        \
        center_dialog(hwnd);                                                                                 \
        SetLastError(error);                                                                                 \
        return ok;                                                                                           \
    }                                                                                                        \
    static BOOL WINAPI destroy_##n(HWND hwnd)                                                                \
    {                                                                                                        \
        BOOL ok = g_destroy[n](hwnd);                                                                        \
        if (ok) {                                                                                            \
            forget_dialog(hwnd);                                                                             \
        }                                                                                                    \
        return ok;                                                                                           \
    }

DIALOG_WRAPPERS(0)
DIALOG_WRAPPERS(1)
DIALOG_WRAPPERS(2)

#define PAINT_WRAPPERS(n)                                                                                    \
    static BOOL WINAPI invalidate_##n(HWND hwnd, const RECT *rect, BOOL erase)                               \
    {                                                                                                        \
        RECT copy;                                                                                           \
        return g_invalidate[n](hwnd, update_rect(hwnd, rect, &copy), erase);                                 \
    }                                                                                                        \
    static BOOL WINAPI validate_##n(HWND hwnd, const RECT *rect)                                             \
    {                                                                                                        \
        RECT copy;                                                                                           \
        return g_validate[n](hwnd, update_rect(hwnd, rect, &copy));                                          \
    }                                                                                                        \
    static BOOL WINAPI redraw_##n(HWND hwnd, const RECT *rect, HRGN region, UINT flags)                      \
    {                                                                                                        \
        RECT copy;                                                                                           \
        return g_redraw[n](hwnd, update_rect(hwnd, rect, &copy), region, flags);                             \
    }                                                                                                        \
    static int WINAPI clip_box_##n(HDC dc, LPRECT rect)                                                      \
    {                                                                                                        \
        int result = g_clip_box[n](dc, rect);                                                                \
        DWORD error = GetLastError();                                                                        \
        if (result != ERROR && rect && !transformed_dc(dc) && scaled_view(WindowFromDC(dc))) {               \
            rect->left = logical_coordinate(rect->left);                                                     \
            rect->top = logical_coordinate(rect->top);                                                       \
            rect->right = rect->right / g_factor + (rect->right % g_factor > 0);                             \
            rect->bottom = rect->bottom / g_factor + (rect->bottom % g_factor > 0);                          \
        }                                                                                                    \
        SetLastError(error);                                                                                 \
        return result;                                                                                       \
    }

PAINT_WRAPPERS(0)
PAINT_WRAPPERS(1)

static void install_import(HMODULE module, const char *dll, const char *name, void *hook, void *storage)
{
    DWORD *slot = iat_find_slot_in(module, dll, name);
    if (!slot || !*slot) {
        log_line("scaling: import %s unavailable (skipped)", name);
        return;
    }
    void *original = (void *)(uintptr_t)*slot;
    memcpy(storage, &original, sizeof(original));
    int result = iat_replace(slot, original, hook);
    if (result != IAT_OK) {
        log_line("scaling: import %s patch failed (reason %d)", name, result);
    }
}

typedef HDC(WINAPI *begin_paint_fn)(HWND, LPPAINTSTRUCT);
typedef BOOL(WINAPI *end_paint_fn)(HWND, const PAINTSTRUCT *);
typedef HDC(WINAPI *get_dc_fn)(HWND);
typedef int(WINAPI *release_dc_fn)(HWND, HDC);

static begin_paint_fn g_begin_paint[4];
static end_paint_fn g_end_paint[4];
static get_dc_fn g_get_dc[4];
static release_dc_fn g_release_dc[4];

static HDC WINAPI begin_paint_0(HWND hwnd, LPPAINTSTRUCT paint)
{
    HDC dc = g_begin_paint[0](hwnd, paint);
    DWORD error = GetLastError();
    if (scaled_view(hwnd) && transform_dc(dc) && paint) {
        logical_rect(&paint->rcPaint);
    }
    SetLastError(error);
    return dc;
}

static BOOL WINAPI end_paint_0(HWND hwnd, const PAINTSTRUCT *paint)
{
    DWORD error = GetLastError();
    if (paint) {
        restore_dc(paint->hdc);
    }
    SetLastError(error);
    return g_end_paint[0](hwnd, paint);
}

static HDC WINAPI get_dc_0(HWND hwnd)
{
    HDC dc = g_get_dc[0](hwnd);
    DWORD error = GetLastError();
    if (scaled_view(hwnd)) {
        transform_dc(dc);
    }
    SetLastError(error);
    return dc;
}

static int WINAPI release_dc_0(HWND hwnd, HDC dc)
{
    DWORD error = GetLastError();
    restore_dc(dc);
    SetLastError(error);
    return g_release_dc[0](hwnd, dc);
}

static HDC WINAPI begin_paint_1(HWND hwnd, LPPAINTSTRUCT paint)
{
    HDC dc = g_begin_paint[1](hwnd, paint);
    DWORD error = GetLastError();
    if (scaled_view(hwnd) && transform_dc(dc) && paint) {
        logical_rect(&paint->rcPaint);
    }
    SetLastError(error);
    return dc;
}

static BOOL WINAPI end_paint_1(HWND hwnd, const PAINTSTRUCT *paint)
{
    DWORD error = GetLastError();
    if (paint) {
        restore_dc(paint->hdc);
    }
    SetLastError(error);
    return g_end_paint[1](hwnd, paint);
}

static HDC WINAPI get_dc_1(HWND hwnd)
{
    HDC dc = g_get_dc[1](hwnd);
    DWORD error = GetLastError();
    if (scaled_view(hwnd)) {
        transform_dc(dc);
    }
    SetLastError(error);
    return dc;
}

static int WINAPI release_dc_1(HWND hwnd, HDC dc)
{
    DWORD error = GetLastError();
    restore_dc(dc);
    SetLastError(error);
    return g_release_dc[1](hwnd, dc);
}

static HDC WINAPI begin_paint_2(HWND hwnd, LPPAINTSTRUCT paint)
{
    HDC dc = g_begin_paint[2](hwnd, paint);
    DWORD error = GetLastError();
    if (scaled_view(hwnd) && transform_dc(dc) && paint) {
        logical_rect(&paint->rcPaint);
    }
    SetLastError(error);
    return dc;
}

static BOOL WINAPI end_paint_2(HWND hwnd, const PAINTSTRUCT *paint)
{
    DWORD error = GetLastError();
    if (paint) {
        restore_dc(paint->hdc);
    }
    SetLastError(error);
    return g_end_paint[2](hwnd, paint);
}

static HDC WINAPI get_dc_2(HWND hwnd)
{
    HDC dc = g_get_dc[2](hwnd);
    DWORD error = GetLastError();
    if (scaled_view(hwnd)) {
        transform_dc(dc);
    }
    SetLastError(error);
    return dc;
}

static int WINAPI release_dc_2(HWND hwnd, HDC dc)
{
    DWORD error = GetLastError();
    restore_dc(dc);
    SetLastError(error);
    return g_release_dc[2](hwnd, dc);
}

static HDC WINAPI begin_paint_3(HWND hwnd, LPPAINTSTRUCT paint)
{
    HDC dc = g_begin_paint[3](hwnd, paint);
    DWORD error = GetLastError();
    if (scaled_view(hwnd) && transform_dc(dc) && paint) {
        logical_rect(&paint->rcPaint);
    }
    SetLastError(error);
    return dc;
}

static BOOL WINAPI end_paint_3(HWND hwnd, const PAINTSTRUCT *paint)
{
    DWORD error = GetLastError();
    if (paint) {
        restore_dc(paint->hdc);
    }
    SetLastError(error);
    return g_end_paint[3](hwnd, paint);
}

static HDC WINAPI get_dc_3(HWND hwnd)
{
    HDC dc = g_get_dc[3](hwnd);
    DWORD error = GetLastError();
    if (scaled_view(hwnd)) {
        transform_dc(dc);
    }
    SetLastError(error);
    return dc;
}

static int WINAPI release_dc_3(HWND hwnd, HDC dc)
{
    DWORD error = GetLastError();
    restore_dc(dc);
    SetLastError(error);
    return g_release_dc[3](hwnd, dc);
}

static void install_paint_imports(HMODULE module, unsigned index)
{
    if (!g_dc_transform || !module || (index >= 2 && !g_dc_transform_controls)) {
        return;
    }
    void *begin_paints[] = {(void *)begin_paint_0, (void *)begin_paint_1, (void *)begin_paint_2,
                            (void *)begin_paint_3};
    install_import(module, "user32.dll", "BeginPaint", begin_paints[index], &g_begin_paint[index]);
    void *end_paints[] = {(void *)end_paint_0, (void *)end_paint_1, (void *)end_paint_2, (void *)end_paint_3};
    install_import(module, "user32.dll", "EndPaint", end_paints[index], &g_end_paint[index]);
    void *get_dcs[] = {(void *)get_dc_0, (void *)get_dc_1, (void *)get_dc_2, (void *)get_dc_3};
    install_import(module, "user32.dll", "GetDC", get_dcs[index], &g_get_dc[index]);
    void *release_dcs[] = {(void *)release_dc_0, (void *)release_dc_1, (void *)release_dc_2,
                           (void *)release_dc_3};
    install_import(module, "user32.dll", "ReleaseDC", release_dcs[index], &g_release_dc[index]);
    const char *modules[] = {"TOW.exe", "MFC42.DLL", "MFC40.DLL", "Anibtn32.ocx"};
    log_line("scaling: %s paint imports checked", modules[index]);
}

static void install_ocx(void)
{
    static HMODULE installed;
    HMODULE module = GetModuleHandleA("Anibtn32.ocx");
    if (!g_dc_transform || !module || module == installed) {
        return;
    }
    installed = module;
    install_paint_imports(module, 3);
}

static void install_dialog_imports(HMODULE module, unsigned index)
{
    void *shows[] = {(void *)show_0, (void *)show_1, (void *)show_2};
    void *destroys[] = {(void *)destroy_0, (void *)destroy_1, (void *)destroy_2};
    install_import(module, "user32.dll", "ShowWindow", shows[index], &g_show[index]);
    install_import(module, "user32.dll", "DestroyWindow", destroys[index], &g_destroy[index]);
}

static void install_mfc40(void)
{
    install_ocx();
    static int attempted;
    if (attempted) {
        return;
    }
    HMODULE module = GetModuleHandleA("MFC40.DLL");
    if (!module) {
        return;
    }
    attempted = 1;
    install_import(module, "user32.dll", "SetWindowPos", (void *)position_2, &g_position[2]);
    install_import(module, "user32.dll", "MoveWindow", (void *)move_2, &g_move[2]);
    install_dialog_imports(module, 2);
    install_paint_imports(module, 2);
    log_line("scaling: MFC40 hooks installed");
}

/* BitBlt stdcall ABI: 9 arguments, callee pops 36 bytes. */
static BOOL WINAPI copy_blit(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    if (g_drawing) {
        return g_bitblt(dst, x, y, w, h, src, sx, sy, rop);
    }
    BOOL ok = FALSE;
    g_drawing = 1;
    __try {
        HWND hwnd = WindowFromDC(dst);
        if (hwnd && !scaled_view(hwnd)) {
            ensure_target(hwnd, 0);
        }
        if (find_target(hwnd)) {
            HWND root = GetAncestor(hwnd, GA_ROOT);
            scale_target *target = find_target(root);
            DWORD now = GetTickCount();
            if (target && (DWORD)(now - target->last_sweep) >= 100) {
                target->last_sweep = now;
                sweep_children(root);
            }
        }
        if (scaled_view(hwnd) && !transformed_dc(dst)) {
            int previous = SetStretchBltMode(dst, COLORONCOLOR);
            if (previous) {
                ok = StretchBlt(dst, x * g_factor, y * g_factor, w * g_factor, h * g_factor, src, sx, sy, w,
                                h, rop);
                SetStretchBltMode(dst, previous);
            }
        }
    } __finally {
        g_drawing = 0;
    }
    return ok ? TRUE : g_bitblt(dst, x, y, w, h, src, sx, sy, rop);
}

static void __stdcall hover_adjust(DWORD *args)
{
    DWORD error = GetLastError();
    __try {
        HWND hwnd = *(HWND *)(uintptr_t)(args[1] + CWND_HWND_OFFSET);
        scale_child *child = find_child(hwnd);
        if (child && !child->dialog) {
            args[2] = (DWORD)logical_coordinate((int)args[2]);
            args[3] = (DWORD)logical_coordinate((int)args[3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Invalid CWnd pointers leave the original arguments intact. */
    }
    SetLastError(error);
}

static DWORD g_hover_resume = GAME_BASE + HOVER_RVA + 5;

// clang-format off
static __declspec(naked) void hover_stub(void)
{
    __asm {
        // Entry ESP=S: return at S, a0/a1/x/y at S+4/+8/+12/+16.
        pushad                 // ESP=S-32, all general registers saved.
        pushfd                 // ESP=S-36, flags saved.
        lea eax, [esp+40]      // EAX=S+4, pointer to original a0.
        push eax               // ESP=S-40, stdcall argument.
        call hover_adjust      // Callee pops 4; ESP=S-36.
        popfd                  // ESP=S-32, original flags restored.
        popad                  // ESP=S, original registers restored.
        sub esp, 18h           // Stolen instruction 1; ESP=S-24.
        push ebx               // Stolen instruction 2; ESP=S-28.
        push esi               // Stolen instruction 3; ESP=S-32.
        jmp dword ptr [g_hover_resume] // Continue at 0x415f85, original ABI.
    }
}

// clang-format on

typedef HCURSOR(WINAPI *load_cursor_fn)(HINSTANCE, LPCSTR);

typedef struct {
    HCURSOR original;
    HCURSOR scaled;
} scale_cursor;

/* Copies and cached failures are bounded; shared originals are never destroyed. */
static scale_cursor g_cursor_cache[CURSOR_LIMIT];
static SRWLOCK g_cursor_lock = SRWLOCK_INIT;
static load_cursor_fn g_load_cursor[2];

static HCURSOR scaled_cursor(HCURSOR original)
{
    HCURSOR result = original;
    if (!original) {
        return result;
    }
    AcquireSRWLockExclusive(&g_cursor_lock);
    for (unsigned i = 0; i < CURSOR_LIMIT; ++i) {
        scale_cursor *entry = &g_cursor_cache[i];
        if (entry->original == original) {
            result = entry->scaled ? entry->scaled : original;
            break;
        }
        if (!entry->original) {
            entry->original = original;
            entry->scaled =
                (HCURSOR)CopyImage(original, IMAGE_CURSOR, CURSOR_SIZE * g_factor, CURSOR_SIZE * g_factor, 0);
            result = entry->scaled ? entry->scaled : original;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_cursor_lock);
    return result;
}

static HCURSOR WINAPI load_cursor_0(HINSTANCE instance, LPCSTR name)
{
    HCURSOR original = g_load_cursor[0](instance, name);
    DWORD error = GetLastError();
    HCURSOR result = scaled_cursor(original);
    SetLastError(error);
    return result;
}

static HCURSOR WINAPI load_cursor_1(HINSTANCE instance, LPCSTR name)
{
    HCURSOR original = g_load_cursor[1](instance, name);
    DWORD error = GetLastError();
    HCURSOR result = scaled_cursor(original);
    SetLastError(error);
    return result;
}

static void install_cursor_import(HMODULE module, unsigned index)
{
    if (!g_scale_cursor) {
        return;
    }
    /* Match scaling.js gameCaller: TOW.exe and MFC42 only. */
    void *hooks[] = {(void *)load_cursor_0, (void *)load_cursor_1};
    install_import(module, "user32.dll", "LoadCursorA", hooks[index], &g_load_cursor[index]);
}

static void install_stage4(void)
{
    void *invalidates[] = {(void *)invalidate_0, (void *)invalidate_1};
    void *validates[] = {(void *)validate_0, (void *)validate_1};
    void *redraws[] = {(void *)redraw_0, (void *)redraw_1};
    void *clips[] = {(void *)clip_box_0, (void *)clip_box_1};
    for (unsigned i = 0; i < 2; ++i) {
        HMODULE module = i ? GetModuleHandleA("MFC42.DLL") : GetModuleHandleA(NULL);
        install_cursor_import(module, i);
        install_dialog_imports(module, i);
        install_paint_imports(module, i);
        install_import(module, "user32.dll", "InvalidateRect", invalidates[i], &g_invalidate[i]);
        install_import(module, "user32.dll", "ValidateRect", validates[i], &g_validate[i]);
        install_import(module, "user32.dll", "RedrawWindow", redraws[i], &g_redraw[i]);
        install_import(module, "gdi32.dll", "GetClipBox", clips[i], &g_clip_box[i]);
    }
    install_mfc40();
    BYTE *site = (BYTE *)(uintptr_t)(GAME_BASE + COPY_CALL_RVA);
    uint8_t expected[6] = {0xff, 0x15};
    DWORD address = BITBLT_SLOT_ADDRESS;
    memcpy(expected + 2, &address, sizeof(address));
    uint8_t call[6] = {0xe8, 0, 0, 0, 0, 0x90};
    DWORD relative = (DWORD)(uintptr_t)copy_blit - (DWORD)(uintptr_t)site - 5;
    memcpy(call + 1, &relative, sizeof(relative));
    if (!patch_write(site, expected, call, sizeof(call))) {
        scale_error(NULL, "copy call-site patch");
    }
    static const uint8_t hover_bytes[] = {0x83, 0xec, 0x18, 0x53, 0x56, 0x8b, 0x74, 0x24, 0x24};
    site = (BYTE *)(uintptr_t)(GAME_BASE + HOVER_RVA);
    uint8_t jump[5] = {0xe9};
    relative = (DWORD)(uintptr_t)hover_stub - (DWORD)(uintptr_t)site - 5;
    memcpy(jump + 1, &relative, sizeof(relative));
    if (!patch_check(site, hover_bytes, sizeof(hover_bytes)) ||
        !patch_write(site, hover_bytes, jump, sizeof(jump))) {
        scale_error(NULL, "hover prologue patch");
    }
}
