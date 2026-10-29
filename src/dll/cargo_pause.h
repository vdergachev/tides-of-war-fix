#ifndef TOW_CARGO_PAUSE_H
#define TOW_CARGO_PAUSE_H

void cargo_pause_configure(const char *path);
int cargo_pause_enabled(void);
void cargo_pause_poll(void);

#endif
