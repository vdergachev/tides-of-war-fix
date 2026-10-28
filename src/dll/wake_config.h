#ifndef TOW_WAKE_CONFIG_H
#define TOW_WAKE_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int enabled;
    int land;
    int damping;
    int strength;
    int resolution;
    int budget;
    int shared;
} wake_config;

void wake_configure(const char *path);
wake_config wake_settings(void);
#ifdef __cplusplus
}
#endif
#endif
