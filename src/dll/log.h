#ifndef TOW_LOG_H
#define TOW_LOG_H
#ifdef __cplusplus
extern "C" {
#endif

int log_open(const char *path);
void log_line(const char *format, ...);
#ifdef __cplusplus
}
#endif

#endif
