#ifndef TOW_TIMER_GUARD_H
#define TOW_TIMER_GUARD_H
/* Install before diag_configure: diag then wraps this timeSetEvent hook (one chain). */
int timer_guard_apply(void);
int timer_guard_ui_poll_apply(void);
#endif
