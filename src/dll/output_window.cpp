#include "output_window.h"
#include "renderer.h"
#include "render_raii.h"
#include "log.h"
#include "scale.h"
#include "diag.h"
#include <stdint.h>
#include <string.h>
#include <new>

#define DRAW_STATE_ADDRESS 0x4cde48
/* Yes/No background is 288x288 before scaling. */
#define YESNO_SIZE 288

int OutputWindow::snapshot_current(void)
{
    if (!frame_valid_ || frame_epoch_ != FrameQueue::atomic_read(&queue.epoch_) ||
        frame_epoch_ != queue.latest_epoch_ || blocked_by_dialog()) {
        return 0;
    }

    __try {
        return FrameQueue::screen_supported() &&
               frame_state_ == *(const int *)(uintptr_t)DRAW_STATE_ADDRESS &&
               frame_dib_ == FrameQueue::source_dib(frame_source_);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int OutputWindow::begin_gdi_refresh(int force)
{
    if (gdi_refresh_ms_ < 0 || gdi_refreshing_ || !queue.serial_ || !IsWindow(frame_window_) ||
        GetWindowThreadProcessId(frame_window_, NULL) != GetCurrentThreadId() || !snapshot_current()) {
        return 0;
    }

    LONG epoch = FrameQueue::atomic_read(&queue.epoch_);
    DWORD now = GetTickCount();
    int changed = gdi_window_ != frame_window_ || gdi_dib_ != frame_dib_ || gdi_state_ != frame_state_ ||
                  gdi_epoch_ != epoch;
    if (!force && !changed && (DWORD)(now - gdi_refreshed_at_) < (DWORD)gdi_refresh_ms_) {
        return 0;
    }

    gdi_refreshing_ = 1;
    return 1;
}

void OutputWindow::end_gdi_refresh(int ok)
{
    if (ok) {
        gdi_refreshed_at_ = GetTickCount();
        gdi_window_ = frame_window_;
        gdi_dib_ = frame_dib_;
        gdi_state_ = frame_state_;
        gdi_epoch_ = frame_epoch_;
        InterlockedIncrement(&gdi_refresh_count_);
    }

    gdi_refreshing_ = 0;
}

void OutputWindow::refresh_gdi(int force)
{
    if (!begin_gdi_refresh(force)) {
        return;
    }

    /* Only this UI producer exchanges latest_; the worker only reads it. */
    end_gdi_refresh(scaling_refresh_gdi(frame_window_, queue.latest_, factor_));
}

int OutputWindow::yesno_blit(HWND window)
{
    HWND root = GetAncestor(window, GA_ROOT);
    WCHAR name[64];
    RECT client = {};
    if (!GetClassNameW(root, name, ARRAYSIZE(name)) || wcscmp(name, L"#32770") ||
        !GetClientRect(root, &client)) {
        return 0;
    }

    return (client.right == YESNO_SIZE && client.bottom == YESNO_SIZE) ||
           (client.right == YESNO_SIZE * factor_ && client.bottom == YESNO_SIZE * factor_);
}

void OutputWindow::hide_for_blit(HDC destination)
{
    if (!child_ || !wanted_) {
        return;
    }

    DWORD error = GetLastError();
    HWND window = WindowFromDC(destination);
    if (!window || GetObjectType(destination) != OBJ_DC) {
        InterlockedIncrement(&skipped_hide_memory_dc_);
    } else if (GetAncestor(window, GA_ROOT) == root_ || blocked_by_dialog() || yesno_blit(window)) {
        /* Preserve Yes/No opening and disabled-owner/child-dialog fallback.
         * Owned modeless popups have their own surface, unlike frame children. */
        hide();
    } else {
        InterlockedIncrement(&skipped_hide_popup_);
    }
    SetLastError(error);
}

void OutputWindow::hide(const char *reason)
{
    if (!child_ || !wanted_) {
        return;
    }
    refresh_gdi(1);
    wanted_ = 0;
    if (diag_enabled(DIAG_UI) || diag_enabled(DIAG_TRANSITIONS)) {
        DWORD error = GetLastError();
        diag_frame frame = queue.diag_latest_;
        frame.epoch = FrameQueue::atomic_read(&queue.epoch_);
        __try {
            frame.state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            frame.state = -1;
        }
        RECT client = {};
        GetClientRect(root_, &client);
        int copy_width;
        int copy_height;
        diag_copy_size(&copy_width, &copy_height);
        diag_line(
            DIAG_UI,
            "ui_hide reason=%s epoch=%ld serial=%ld DrawState=%d source=%p captured_CDib=%p last_copy=%dx%d "
            "client=(%ld,%ld,%ld,%ld)",
            reason, frame.epoch, frame.serial, frame.state, (void *)diagnostic_source_, frame.dib, copy_width,
            copy_height, client.left, client.top, client.right, client.bottom);
        if (diag_enabled(DIAG_TRANSITIONS)) {
            diag_transition("hide", &frame, -1, S_OK);
        }
        SetLastError(error);
    }
    if (diagnostics_) {
        log_line("d3d11: hide epoch=%ld", FrameQueue::atomic_read(&queue.epoch_));
    }
    InterlockedIncrement(&queue.epoch_);
    ShowWindow(child_, SW_HIDE);
    if (gdi_refresh_ms_ < 0) {
        InvalidateRect(root_, NULL, FALSE);
    } else {
        RedrawWindow(root_, NULL, NULL, RDW_INVALIDATE | RDW_NOCHILDREN);
    }
}

BOOL CALLBACK OutputWindow::find_dialog(HWND hwnd, LPARAM parameter)
{
    WCHAR name[64];
    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, name, ARRAYSIZE(name)) && !wcscmp(name, L"#32770")) {
        *(int *)parameter = 1;
        return FALSE;
    }
    return TRUE;
}

int OutputWindow::blocked_by_dialog(void)
{
    int found = 0;
    EnumChildWindows(root_, find_dialog, (LPARAM)&found);
    return found || !IsWindowEnabled(root_);
}

void OutputWindow::update_window(void)
{
    const char *reason = !FrameQueue::screen_supported()     ? "unsupported_screen"
                         : FrameQueue::atomic_read(&failed_) ? "worker_failed"
                         : IsIconic(root_)                   ? "iconic"
                         : blocked_by_dialog()               ? "dialog"
                                                             : NULL;
    if (reason) {
        hide(reason);
        return;
    }
    if (wanted_ && FrameQueue::atomic_read(&drawn_epoch_) == FrameQueue::atomic_read(&queue.epoch_)) {
        int was_visible = IsWindowVisible(child_);
        if (!was_visible) {
            refresh_gdi(1);
        }
        SetWindowPos(child_, HWND_TOP, 0, 0, FRAME_WIDTH * factor_, FRAME_HEIGHT * factor_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!was_visible) {
            InterlockedExchange(&refresh_, 1);
            SetEvent(wake_);
            if (diagnostics_) {
                log_line("d3d11: show epoch=%ld factor=%d", FrameQueue::atomic_read(&queue.epoch_), factor_);
            }
        }
    }
}

void OutputWindow::stop_worker(void)
{
    if (stopping_) {
        return;
    }
    stopping_ = 1;
    if (thread_) {
        SetEvent(stop_);
        /* DXGI may send synchronous window messages. Drain sent messages while joining,
         * without dispatching posted game work inside its destruction handler. */
        while (WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT) {
            MSG message;
            PeekMessageW(&message, NULL, WM_NULL, WM_NULL, PM_NOREMOVE);
            HANDLE thread = thread_;
            MsgWaitForMultipleObjects(1, &thread, FALSE, 10, QS_SENDMESSAGE);
        }
        CloseHandle(thread_);
        thread_ = NULL;
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = NULL;
    }
    if (wake_) {
        CloseHandle(wake_);
        wake_ = NULL;
    }
    stopping_ = 0;
}

LRESULT CALLBACK OutputWindow::output_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    OutputWindow *host = reinterpret_cast<OutputWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<OutputWindow *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (!host) {
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    return host->window_message(hwnd, message, wp, lp);
}

LRESULT OutputWindow::window_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        {
            paint_scope paint(hwnd);
        }

        InterlockedExchange(&refresh_, 1);
        return 0;
    }
    if (message == WM_SIZE || message == WM_SHOWWINDOW) {
        InterlockedExchange(&refresh_, 1);
    }
    if (message == WM_TIMER || message == OUTPUT_FAILED) {
        update_window();
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(hwnd, OUTPUT_TIMER);
        stop_worker();
        child_ = NULL;
        root_ = NULL;
        wanted_ = 0;
        /* A destroyed game window is not a request to restart the backend. */
        InterlockedExchange(&failed_, 1);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int OutputWindow::create_window(HWND root, int factor)
{
    if (GetWindowThreadProcessId(root, NULL) != GetCurrentThreadId()) {
        return 0;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = output_proc;
    wc.hInstance = instance_;
    wc.lpszClassName = OUTPUT_CLASS;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    root_ = root;
    factor_ = factor;
    child_ = CreateWindowExW(WS_EX_NOACTIVATE, OUTPUT_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                             FRAME_WIDTH * factor, FRAME_HEIGHT * factor, root, NULL, instance_, this);
    if (!child_) {
        return 0;
    }
    stop_ = CreateEventW(NULL, TRUE, FALSE, NULL);
    wake_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!stop_ || !wake_ || !SetTimer(child_, OUTPUT_TIMER, 100, NULL)) {
        DestroyWindow(child_);
        return 0;
    }
    thread_ = CreateThread(NULL, 0, render_thread, this, 0, NULL);
    if (!thread_) {
        DestroyWindow(child_);
        return 0;
    }
    return 1;
}

int OutputWindow::frame(HWND destination, HDC source, int factor)
{
    /* A rejected capture must never authorize refresh from an older snapshot. */
    frame_valid_ = 0;

    if (diag_enabled(DIAG_UI) || diag_enabled(DIAG_TRANSITIONS)) {
        diagnostic_source_ = source;
    }
    if (!enabled_ || FrameQueue::atomic_read(&failed_) || stopping_) {
        hide("disabled_failed_stopping");
        return 0;
    }
    HWND root = GetAncestor(destination, GA_ROOT);
    int qualifies = 0;
    void *dib = NULL;
    int state = 0;
    __try {
        dib = FrameQueue::source_dib(source);
        state = *(const int *)(uintptr_t)DRAW_STATE_ADDRESS;
        qualifies = FrameQueue::screen_supported() && dib != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        qualifies = 0;
    }
    if (!qualifies || !root || IsIconic(root)) {
        hide(!qualifies ? "unsupported_source" : !root ? "no_root" : "iconic");
        return 0;
    }
    if (!child_ && !create_window(root, factor)) {
        InterlockedExchange(&failed_, 1);
        InterlockedIncrement(&fallback_);
        log_line("d3d11: window/thread creation failed; using StretchBlt error=%lu", GetLastError());
        return 0;
    }
    const char *reason = root != root_                         ? "different_root"
                         : blocked_by_dialog()                 ? "dialog"
                         : !queue.copy_pixels(source, smooth_) ? "copy_failed"
                                                               : NULL;
    if (reason) {
        hide(reason);
        return 0;
    }
    frame_window_ = destination;
    frame_dib_ = dib;
    frame_state_ = state;
    frame_source_ = source;
    frame_epoch_ = FrameQueue::atomic_read(&queue.epoch_);
    frame_valid_ = 1;
    wanted_ = 1;
    SetEvent(wake_);
    update_window();
    /* Until the first successful Present, leave the existing GDI picture visible. */
    int accepted = IsWindowVisible(child_) &&
                   FrameQueue::atomic_read(&ready_epoch_) == FrameQueue::atomic_read(&queue.epoch_) &&
                   !FrameQueue::atomic_read(&failed_);
    return accepted;
}

void OutputWindow::window(HWND hwnd, UINT message)
{
    if (hwnd != root_ || !child_) {
        return;
    }
    if (message == WM_DESTROY) {
        DestroyWindow(child_);
    } else if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED || message == WM_PAINT ||
               message == WM_SHOWWINDOW) {
        update_window();
    }
}

int OutputWindow::owns(HWND hwnd)
{
    return hwnd && hwnd == child_;
}

int OutputWindow::enabled(void)
{
    return enabled_;
}

void OutputWindow::configure(const char *dir)
{
    queue.prepare();
    char path[MAX_PATH];
    if (strlen(dir) + sizeof("tow_fix.ini") > sizeof(path)) {
        return;
    }
    strcpy_s(path, sizeof(path), dir);
    strcat_s(path, sizeof(path), "tow_fix.ini");
    enabled_ = GetPrivateProfileIntA("d3d11", "enabled", 0, path) != 0;
    warp_ = GetPrivateProfileIntA("d3d11", "warp", 0, path) != 0;
    diagnostics_ = GetPrivateProfileIntA("d3d11", "log", 0, path) != 0;
    gdi_refresh_ms_ = (int)GetPrivateProfileIntA("d3d11", "gdi_refresh_ms", 250, path);
    if (gdi_refresh_ms_ < -1) {
        gdi_refresh_ms_ = -1;
    }
    log_line("[d3d11] gdi_refresh_ms=%d", gdi_refresh_ms_);
    smooth_ = enabled_ && GetPrivateProfileIntA("d3d11", "smooth", 0, path) != 0;
    if (smooth_) {
        smooth_ = queue.initialize();
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)output_proc, &instance_);
    log_line("[d3d11] enabled=%d warp=%d log=%d smooth=%d", enabled_, warp_, diagnostics_, smooth_);
}

DWORD WINAPI OutputWindow::render_thread(void *parameter)
{
    OutputWindow &host = *static_cast<OutputWindow *>(parameter);
    Renderer *renderer = new (std::nothrow) Renderer(host);
    if (!renderer) {
        InterlockedExchange(&host.failed_, 1);
        InterlockedIncrement(&host.fallback_);
        log_line("d3d11: fallback to StretchBlt hr=0x%08lx removed=0x%08lx", (DWORD)E_OUTOFMEMORY, (DWORD)0);
        PostMessageW(host.child_, OUTPUT_FAILED, 0, 0);
        return 0;
    }
    DWORD result = renderer->run();
    delete renderer;
    return result;
}
