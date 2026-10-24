#ifndef TOW_LOG_H
#define TOW_LOG_H
int log_open(const char *path);
void log_line(const char *format, ...);
#endif
