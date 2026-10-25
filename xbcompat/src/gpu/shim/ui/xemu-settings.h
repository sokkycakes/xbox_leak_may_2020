/* xemu's settings, fixed for xbcompat-gpu. */
#ifndef XBC_XEMU_SETTINGS_H
#define XBC_XEMU_SETTINGS_H
#include <stdbool.h>
typedef enum {
    CONFIG_DISPLAY_RENDERER_NULL,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT
} CONFIG_DISPLAY_RENDERER;
struct xbc_xemu_config {
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
        struct { int surface_scale; } quality;
    } display;
    struct { bool cache_shaders; } perf;
};
extern struct xbc_xemu_config g_config;
const char *xemu_settings_get_base_path(void);
#endif
