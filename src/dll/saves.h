#ifndef TOW_SAVES_H
#define TOW_SAVES_H
#include <windows.h>
int saves_prepare(const char *dir, char path[MAX_PATH], int *created);
int save_redirect_apply(const char *dir);
#endif
