#ifndef BASE_H
#define BASE_H  
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/render/wlr_renderer.h>

#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

// #define VISIBLE(c, o) (c->output == o && c->tag & o->tag)
#include <wlr/util/log.h>

#include <wlr/types/wlr_output_damage.h>

#define INFO(...) wlr_log(WLR_INFO, __VA_ARGS__)
#define ERROR(...) wlr_log(WLR_ERROR, __VA_ARGS__)

enum wlc_cursor_mode {
    WLC_CURSOR_RESIZE,
    WLC_CURSOR_MOVE,
    WLC_CURSOR_NORMAL,
};

struct wlc_output {
    struct wlr_output *wlr_output;
    struct timespec last_frame;
    struct wl_listener destroy;
    struct wl_listener frame;
    struct wl_list link;
    uint32_t layout;
    uint32_t n_master;
    double_t f_master;
    struct wlr_box *geom;
    uint16_t tag;
    struct wlr_output_damage *wlr_damage;
    struct wl_listener commit;
};

struct wlc_client {
    // struct wlc_server* server;
    struct wlr_xdg_surface *xdg_surface;
    struct wlc_output *output;
    struct wlr_box geom;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    // struct wl_listener request_move;
    // struct wl_listener request_resize;
    struct wl_list llink;
    struct wl_list flink;
    struct wl_list zlink;
    uint8_t tag;
};

struct render_data {
    struct wlr_output *output;
    struct wlc_client *client;
    struct wlr_renderer *renderer;
    struct timespec *when;
};

struct wlc_keyboard {
    struct wl_list link;
    struct wlr_input_device *device;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

struct wlc_layout {
    void (*l)();
    const char *s;
};


void resize(struct wlc_client *c, double_t w, double_t h);
void move(struct wlc_client *c, uint32_t x, uint32_t y);
struct wlc_client* fstack_top();
uint8_t visible(struct wlc_client *c, struct wlc_output *o);

extern struct wlc_output *foutput;
extern struct wl_list lstack; // Client layout configuration (size and positioning)
#endif // !BASE_H
