#ifndef TOW_SCALE_H
#define TOW_SCALE_H

/* Scaling owns factor, target state and mouse mapping in scaling.c. */
void scaling_apply(const char *dir);
int scaling_active(void);
void scaling_set_borderless(int enabled);

#endif
