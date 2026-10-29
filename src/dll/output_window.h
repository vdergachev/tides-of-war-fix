#ifndef TOW_OUTPUT_WINDOW_H
#define TOW_OUTPUT_WINDOW_H
#include "frame_queue.h"
#define OUTPUT_CLASS  L"TowFixD3D11Output"
#define OUTPUT_TIMER  1
#define OUTPUT_FAILED (WM_APP + 0x311)

/* UI owns the HWND and joins its worker. All fields have trivial destruction;
 * process detach never runs window, lock, HANDLE or COM cleanup. */
class OutputWindow
{
  public:
    void hide(const char *reason = "external_partial_blit");
    void hide_for_blit(HDC destination);
    int frame(HWND destination, HDC source, int factor);
    int begin_gdi_refresh(int force);
    void end_gdi_refresh(int ok);
    void window(HWND hwnd, UINT message);
    int owns(HWND hwnd);
    int enabled(void);
    void configure(const char *dir);

  private:
    friend class Renderer;
    static BOOL CALLBACK find_dialog(HWND hwnd, LPARAM parameter);
    int blocked_by_dialog(void);
    int yesno_blit(HWND window);
    void update_window(void);
    void refresh_gdi(int force);
    int snapshot_current(void);
    void stop_worker(void);
    static LRESULT CALLBACK output_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    LRESULT window_message(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
    int create_window(HWND root, int factor);
    static DWORD WINAPI render_thread(void *host);
    FrameQueue queue;
    HWND child_;
    HWND root_;
    HANDLE thread_;
    HANDLE wake_;
    HANDLE stop_;
    HINSTANCE instance_;
    int enabled_;
    int warp_;
    int diagnostics_;
    int smooth_;
    int factor_;
    int wanted_;
    HDC diagnostic_source_;
    int stopping_;
    LONG ready_epoch_;
    LONG drawn_epoch_;
    LONG refresh_;
    LONG failed_;
    LONG presented_;
    LONG dropped_;
    LONG stale_;
    LONG fallback_;
    int gdi_refresh_ms_;
    int gdi_refreshing_;
    DWORD gdi_refreshed_at_;
    HWND frame_window_;
    void *frame_dib_;
    int frame_state_;
    HDC frame_source_;
    LONG frame_epoch_;
    int frame_valid_;
    HWND gdi_window_;
    void *gdi_dib_;
    int gdi_state_;
    LONG gdi_epoch_;
    LONG gdi_refresh_count_;
    LONG skipped_hide_memory_dc_;
    LONG skipped_hide_popup_;
};
#endif
