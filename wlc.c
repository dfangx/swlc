/******************************************************************************
 * File:             wlc.c
 *
 * Author:           Cyrus Ng
 * Created:          01/03/21
 * Description:      Main C file for wlc wayland compositor
 *****************************************************************************/

/* NOTE
 * OUTPUT COORDINATE SYSTEM (output)
 * LAYOUT COORDINATE SYSTEM (cursor, output)
 * VIEW COORDINATE SYSTEM (client)
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
// #include <wlr/util/log.h>

#include "wlc.h"
#include "config.h"

// #define INFO(...) wlr_log(WLR_INFO, __VA_ARGS__)
// #define ERROR(...) wlr_log(WLR_ERROR, __VA_ARGS__)
// #define VISIBLE(c, o) (c->output == o && c->tag & o->tag)

// static struct wl_list lstack; // Client layout configuration (size and positioning)
struct wl_list lstack;
static struct wl_list fstack; // Client focusing
static struct wl_list zstack; // Client stacking

static struct wl_display *display;
static struct wlr_backend *backend;
static struct wlr_renderer *renderer;
static struct wl_list outputs;
static struct wl_listener new_output;
static struct wlr_output_layout *output_layout;
struct wlc_output *foutput;

static struct wlr_xdg_shell *xdg_shell;
static struct wl_listener new_xdg_surface;

static struct wlr_cursor *csr;
static struct wlr_xcursor_manager *cursor_mgr;
static struct wl_listener cursor_motion;
static struct wl_listener cursor_motion_absolute;
static struct wl_listener cursor_button;
static struct wl_listener cursor_axis;
static struct wl_listener cursor_frame;
static struct wlr_seat *seat;
static struct wl_listener request_cursor;
static struct wl_listener new_input;
static struct wl_list keyboards;

// Grabbed client
static struct wlc_client *gc;
static uint32_t gcx;
static uint32_t gcy;

static uint8_t cursor_mode;

/*
static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data);
static void xdg_toplevel_request_move(struct wl_listener *listener, void *data);
*/


static bool run();
static bool setup();
static void cleanup();
static void cursor_axis_notify(struct wl_listener *listener, void *data);
static void cursor_button_notify(struct wl_listener *listener, void *data);
static void cursor_frame_notify(struct wl_listener *listener, void *data);
static void cursor_motion_absolute_notify(struct wl_listener *listener, void *data);
static void cursor_motion_notify(struct wl_listener *listener, void *data);
static void keyboard_destroy_notify(struct wl_listener *listener, void *data);
static void keyboard_key_notify(struct wl_listener *listener, void *data);
static void keyboard_modifiers_notify(struct wl_listener *listener, void *data);
static void new_input_notify(struct wl_listener *listener, void *data);
static void new_output_notify(struct wl_listener *listener, void *data);
static void new_xdg_surface_notify(struct wl_listener *listener, void *data);
static void output_destroy_notify(struct wl_listener *listener, void *data);
static void output_frame_notify(struct wl_listener *listener, void *data);
static void process_cursor_motion(uint32_t time);
static void render_surface(struct wlr_surface *surface, int x, int y, void *data);
static void scale_box(struct wlr_box *box, uint32_t scale);
static void seat_request_cursor(struct wl_listener *listener, void *data);
static void xdg_surface_destroy_notify(struct wl_listener *listener, void *data);
static void xdg_surface_map_notify(struct wl_listener *listener, void *data);
static void xdg_surface_unmap_notify(struct wl_listener *listener, void *data);
static bool process_keybindings(xkb_keysym_t sym);
static struct wlc_client *find_client(double_t cursor_x, 
        double_t cursor_y,
        struct wlr_surface **surface,
        double_t *surface_x, 
        double_t *surface_y);
static void find_surface(struct wlc_client *client, 
        double_t lx, 
        double_t ly,
        struct wlr_surface **surface, 
        double_t *sx,
        double_t *sy);
static void focus_client(struct wlc_client *client, struct wlr_surface *surface);
static void toggle_tag(uint16_t tag);
static void move_resize(enum wlc_cursor_mode);
static struct wlc_output* cursor_to_output(double_t csrx, double_t csry);
static inline void listen(struct wl_listener* l, void (*h)(), struct wl_signal* s);
static inline void set_lstack_head(struct wlc_client *c);
static inline void set_fstack_head(struct wlc_client *c);
static inline void set_zstack_head(struct wlc_client *c);

inline uint8_t visible(struct wlc_client *c, struct wlc_output *o) {
    return c->output == o && c->tag & o->tag;
}

inline void set_lstack_head(struct wlc_client *c) {
    wl_list_remove(&c->llink);
    wl_list_insert(&lstack, &c->llink);
}

inline void set_fstack_head(struct wlc_client *c) {
    wl_list_remove(&c->flink);
    wl_list_insert(&fstack, &c->flink);
}

inline void set_zstack_head(struct wlc_client *c) {
    wl_list_remove(&c->zlink);
    wl_list_insert(&zstack, &c->zlink);
}

inline void listen(struct wl_listener* l, void (*h)(), struct wl_signal* s) {
    l->notify = h;
    wl_signal_add(s, l);
}

void swap_master() {
    struct wlc_client *cc = fstack_top();
    struct wlc_client *c;
    wl_list_for_each(c, &lstack, llink) {
        if (visible(c, foutput)) { // master
            if (&c->llink == &cc->llink) return; 
            else break;
        }
    }

    set_lstack_head(cc);
}

struct wlc_output* cursor_to_output(double_t csrx, double_t csry) {
    struct wlr_output *o = wlr_output_layout_output_at(output_layout, csrx, csry);
    if (o) return o->data;
    return NULL;
}

void focus_next(int8_t dir){
    struct wlc_client *cc = fstack_top();
    if (!cc) return;
    
    struct wlc_client *c;
    if (dir > 0) {
        wl_list_for_each(c, &cc->llink, llink) {
            if (&c->llink == &cc->llink) continue;
            if (visible(c, foutput)) break; 
        }
    }
    else {
        wl_list_for_each_reverse(c, &cc->llink, llink) {
            if (&c->llink == &cc->llink) continue;
            if (visible(c, foutput)) break; 
        }
    }

    if (c) focus_client(c, c->xdg_surface->surface);
}

/*
void tile() {
    struct wlc_client *c;
    uint32_t nc = 0;

    wl_list_for_each(c, &lstack, llink) {
        if (visible(c, foutput)) ++nc;
    }

    uint32_t ow = foutput->geom->width;
    uint32_t oh = foutput->geom->height;

    double_t mh = oh / (double_t) foutput->n_master;
    double_t mw = ow;
    double_t ch = 0;
    uint32_t mx = 0;
    uint32_t my = 0;

    if (nc > foutput->n_master) {
        mw *= foutput->f_master; 
        ch = oh / (double_t) (nc - foutput->n_master);
    }

    double_t cw = ow - mw;
    uint32_t cx = mw;
    uint32_t cy = 0;

    uint32_t n = 0;
    wl_list_for_each(c, &lstack, llink) {
        if (!visible(c, foutput)) continue; 
        if (n < foutput->n_master) {
            resize(c, mw, mh);
            move(c, mx, my);
            my += mh;
            ++n;
            continue;
        }

        resize(c, cw, ch);
        move(c, cx, cy);
        cy += ch;
    }
}
*/

struct wlc_client* fstack_top() {
    struct wlc_client *c;
    wl_list_for_each(c, &fstack, flink) {
        if (visible(c, foutput)) return c;
    }
    return NULL;
}

void move(struct wlc_client *c, uint32_t x, uint32_t y) {
    c->geom.x = x;
    c->geom.y = y;
}

void resize(struct wlc_client *c, double_t w, double_t h) {
    c->geom.width = w;
    c->geom.height = h;
    wlr_xdg_toplevel_set_size(c->xdg_surface, w, h);
}

// Toggle the tag. Arrange the clients visible and focus the client on top of
// the focus stack
void toggle_tag(uint16_t t) {
    if ((foutput->tag ^ t) == 0) return;
        
    foutput->tag  ^= t;
    if (layouts[foutput->layout].l) layouts[foutput->layout].l();
    struct wlc_client *c = fstack_top();
    if (c) {
        focus_client(c, c->xdg_surface->surface);
    }
}

// Swith to new tag. Arrange the clients on the new tag and focus the client on
// top of the focus stack
void switch_tag(uint16_t t) {
    foutput->tag = t;
    if (layouts[foutput->layout].l) layouts[foutput->layout].l();
    struct wlc_client *c = fstack_top();
    if (c) focus_client(c, c->xdg_surface->surface);
}

void set_tag(uint16_t t) {
    struct wlc_client *c = fstack_top();
    if (c) c->tag = t;
    if (layouts[foutput->layout].l) layouts[foutput->layout].l();
}

// Gives client keyboard focus
void focus_client(struct wlc_client *c, struct wlr_surface *surface) {
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;

    // If previous surface is the current surface
    if (prev_surface == surface) {
        return;
    }

    // If there was a different previous surface
    if (prev_surface) {
        // Deactivate previously focused surface. Client will repaint
        struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
            seat->keyboard_state.focused_surface);
        wlr_xdg_toplevel_set_activated(previous, false);
    }

    // If no client is to be focused
    if (c == NULL) {
        wlr_seat_keyboard_clear_focus(seat);
        return;
    }

    // Move the view to the front
    // wl_list_remove(&c->flink);
    // wl_list_insert(&fstack, &c->flink);
    // wl_list_remove(&c->zlink);
    // wl_list_insert(&zstack, &c->zlink);
    set_fstack_head(c);
    set_zstack_head(c);

    // Activate new surface
    wlr_xdg_toplevel_set_activated(c->xdg_surface, true);

    // Have keyboard enter surface. Key events will be sent to the correct client
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_seat_keyboard_notify_enter(seat, 
            c->xdg_surface->surface,
            keyboard->keycodes, 
            keyboard->num_keycodes,
            &keyboard->modifiers);
}

// Test if any nested surfaces are underneath layout coordinates (lx, ly). If
// so, surface pointer is set to the wlr_surface and the surface coordinates
// (sx, sy) to relative of that surface's top-left corner.
void find_surface(struct wlc_client *c, 
        double_t lx, 
        double_t ly,
        struct wlr_surface **surface, 
        double_t *sx, 
        double_t *sy) {
    double_t csx = lx - c->geom.x;
    double_t csy = ly - c->geom.y;
    *surface = wlr_xdg_surface_surface_at(c->xdg_surface, csx, csy, sx, sy);
}

// Find the client under the cursor by iterating over all clients
// lx, ly - cursor coordinates in layout coordinates
struct wlc_client *find_client(double_t lx, 
        double_t ly,
        struct wlr_surface **s, 
        double_t *sx,
        double_t *sy) {
    struct wlc_client *c;
    wl_list_for_each(c, &lstack, llink) {
        find_surface(c, lx, ly, s, sx, sy);
        if (*s && visible(c, foutput)) return c;
    }
    return NULL;
}

// Sent when cursor emits frame event. Sent after regular pointer events to
// group pointer events together (ex. two axis events may happen at the same
// time. Frame event will not be sent in between those events)
void cursor_frame_notify(struct wl_listener *listener, void *data) {
    wlr_seat_pointer_notify_frame(seat);
}

// Raised by cursor when axis event occurs (ex. scroll wheel)
void cursor_axis_notify(struct wl_listener *listener, void *data) {
    struct wlr_event_pointer_axis *event = data;
    wlr_seat_pointer_notify_axis(seat, 
            event->time_msec, 
            event->orientation,
            event->delta, event->delta_discrete,
            event->source);
}

static void move_resize(enum wlc_cursor_mode mode) {
    double_t sx, sy;
    struct wlr_surface *surface;
    gc = find_client(csr->x, csr->y, &surface, &sx, &sy);
    if (!gc) {
        return;
    }

    switch(cursor_mode = mode) {
        case WLC_CURSOR_MOVE:
            gcx = csr->x - gc->geom.x;
            gcy = csr->y - gc->geom.y;
            wlr_xcursor_manager_set_cursor_image(cursor_mgr, "fleur", csr);
            break;
        case WLC_CURSOR_RESIZE:
            wlr_cursor_warp_closest(csr, 
                    NULL, 
                    gc->geom.x + gc->geom.width, 
                    gc->geom.y + gc->geom.height);
            wlr_xcursor_manager_set_cursor_image(cursor_mgr, "bottom_right_corner", csr);
            break;
    }
}

// Raised when cursor emits a button event (ex. mouse click)
void cursor_button_notify(struct wl_listener *listener, void *data) {
    struct wlr_event_pointer_button *event = data;

    double_t sx, sy;
    struct wlr_surface *s;
    struct wlc_client *client = find_client(csr->x, 
            csr->y, 
            &s, 
            &sx, 
            &sy);
    switch(event->state) {
        case WLR_BUTTON_PRESSED:
            focus_client(client, s);
            // move_resize(WLC_CURSOR_MOVE);
            break;
        case WLR_BUTTON_RELEASED:
            if (cursor_mode != WLC_CURSOR_NORMAL) {
                wlr_xcursor_manager_set_cursor_image(cursor_mgr, "left_ptr", csr);    
                cursor_mode = WLC_CURSOR_NORMAL;
                return;
            } 
            break;
    }
    // Notify client with pointer focus that button press has occurred
    wlr_seat_pointer_notify_button(seat, 
            event->time_msec, 
            event->button,
            event->state);
}

void process_cursor_resize(uint32_t time) {
    // b = border
    double_t bx = csr->x - gcx;
    double_t by = csr->y - gcy;

    uint32_t new_left = gcx;
    if (WLR_EDGE_TOP) {
         
    }
}

void process_cursor_move(uint32_t time) {
    gc->geom.x = csr->x - gcx;
    gc->geom.y = csr->y - gcy;
}

// TODO
void process_cursor_motion(uint32_t time) {
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wlc_client *c = find_client(csr->x, csr->y, &surface, &sx, &sy);

    switch(cursor_mode) {
        case WLC_CURSOR_MOVE:
            process_cursor_move(time);
            return;
        case WLC_CURSOR_RESIZE:
            process_cursor_resize(time);
            return;
    }
    // If no client under cursor, then use default cursor image
    if (!c) {
        wlr_xcursor_manager_set_cursor_image(cursor_mgr, "left_ptr", csr);
    }

    if (surface) {
        bool focus_changed = seat->pointer_state.focused_surface != surface;
        // Enter the surface - lets the client know that the cursor has entered
        // one of its surfaces. Gives the surface pointer focus (distinct from
        // keyboard focus). Get pointer focus by moving pointer over surface
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        if (!focus_changed) {
            wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        }
        if (follow_mouse) focus_client(c, surface);
    } else {
        // Clear pointer focus so future pointer events are not sent to the last
        // focused client
        wlr_seat_pointer_clear_focus(seat);
    }
}

// Raised by cursor when pointer emits absolute motion event, from 0..1 on each
// axis.
void cursor_motion_absolute_notify(struct wl_listener *listener, void *data) {
    struct wlr_event_pointer_motion_absolute *event = data;

    wlr_cursor_warp_absolute(csr, event->device, event->x, event->y);
    process_cursor_motion(event->time_msec);
}

// Raised when cursor emits relative pointer motion event (delta)
void cursor_motion_notify(struct wl_listener *listener, void *data) {
    struct wlr_event_pointer_motion *event = data;

    wlr_cursor_move(csr, event->device, event->delta_x, event->delta_y);
    process_cursor_motion(event->time_msec);
}

// Called when surface is unmapped
void xdg_surface_unmap_notify(struct wl_listener *listener, void *data) {
    struct wlc_client *c = wl_container_of(listener, c, unmap);
    c->output = NULL;
    wl_list_remove(&c->llink);
    wl_list_remove(&c->flink);
    wl_list_remove(&c->zlink);

    struct wlc_client *next;
    wl_list_for_each(next, &fstack, flink) {
        if (visible(next, foutput)) {
            focus_client(next, next->xdg_surface->surface);
            break;
        }
    }

    if (layouts[foutput->layout].l) layouts[foutput->layout].l();
}

// Called when surface is destroyed and should never be shown again
void xdg_surface_destroy_notify(struct wl_listener *listener, void *data) {
    struct wlc_client *c = wl_container_of(listener, c, destroy);

    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->map.link);
    wl_list_remove(&c->unmap.link);
    free(c);
}

// Called to notify when surface is mapped or ready to display
void xdg_surface_map_notify(struct wl_listener *listener, void *data) {
    struct wlc_client *c = wl_container_of(listener, c, map);
    c->output = foutput;

    // Add to c list
    wl_list_insert(&lstack, &c->llink);
    wl_list_insert(&fstack, &c->flink);
    wl_list_insert(&zstack, &c->zlink);
    wlr_xdg_surface_get_geometry(c->xdg_surface, &c->geom);
    focus_client(c, c->xdg_surface->surface);
    if (layouts[foutput->layout].l) layouts[foutput->layout].l();
}

/*
// Called when client wants to begin interactive move
void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_move_event *event = data;
    struct wlc_client *client = wl_container_of(listener, client, request_resize);

}

// Called when client wants to being interactive resize
void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct wlc_client *client = wl_container_of(listener, client, request_resize);
}
*/
// Raised when new xdg surface is received
void new_xdg_surface_notify(struct wl_listener *listener, void *data) {
    struct wlr_xdg_surface *xdg_surface = data;
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    // Allocate a client struct for the surface
    struct wlc_client *c = calloc(1, sizeof(struct wlc_client));
    c->xdg_surface = xdg_surface;

    // See header file for description
    wlr_xdg_toplevel_set_tiled(c->xdg_surface,
            WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
            WLR_EDGE_RIGHT | WLR_EDGE_TOP);

    // Listen to events that surface can emit
    // c->map.notify = xdg_surface_map_notify;
    // wl_signal_add(&xdg_surface->events.map, &c->map);
    // c->unmap.notify = xdg_surface_unmap_notify;
    // wl_signal_add(&xdg_surface->events.unmap, &c->unmap);
    // c->destroy.notify = xdg_surface_destroy_notify;
    // wl_signal_add(&xdg_surface->events.destroy, &c->destroy);
    listen(&c->map, xdg_surface_map_notify, &xdg_surface->events.map);
    listen(&c->unmap, xdg_surface_unmap_notify, &xdg_surface->events.unmap);
    listen(&c->destroy, xdg_surface_destroy_notify, &xdg_surface->events.destroy);
    c->tag = foutput->tag;

    // Top level resize and move events
    // struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;
    // client -> request_move = xdg_toplevel_request_move;
    // wl_signal_add(&xdg_toplevel->events.request_move, &c->request_move);
    // client -> request_resize = xdg_toplevel_request_resize;
    // wl_signal_add(&xdg_toplevel->events.request_resize, &c->request_resize);

}

// Scales the provided box with a provided scale factor
void scale_box(struct wlr_box *box, uint32_t scale) {
    box->x *= scale;
    box->y *= scale;
    box->width *= scale;
    box->height *= scale;
}

// Called for every surface that needs to be rendered
void render_surface(struct wlr_surface *s, 
        int x, 
        int y, 
        void *data) {
    struct render_data *rdata = data;
    struct wlc_client *c = rdata->client;
    struct wlr_output *o = rdata->output;

    // Obtain a wlr_texture, which is a GPU resource.wlroots handles this
    struct wlr_texture *texture = wlr_surface_get_texture(s);
    if (texture == NULL) {
        ERROR("Could not obtain wlr_texture");
        return;
    }

    // View has output layout coordinates. Need to translate to output local
    // coordinates (ex. layout coordinates: 2000, 100, two displays 1920x1080,
    // then local coordinates = 2000-1920,100)
    double_t ox = 0, oy = 0;
    wlr_output_layout_output_coords(output_layout, 
            o, 
            &ox,
            &oy);

    // Apply scale factor for HiDPI outputs
    struct wlr_box box = {
        .x = ox + c->geom.x + x,
        .y = oy + c->geom.y + y,
        .width = s->current.width,
        .height = s->current.height,
    };
    scale_box(&box, o->scale);

    // Create a matrix for model-view-projection matrix
    float matrix[9];
    enum wl_output_transform transform = wlr_output_transform_invert(s->current.transform);
    wlr_matrix_project_box(matrix, 
            &box, 
            transform, 
            0,
            o->transform_matrix);

    // Takes matrix, texture, alpha, and renderer and performs rendering
    wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

    // Let client know frame is done rendering and can now prepare new frame if
    // needed
    wlr_surface_send_frame_done(s, rdata->when);
}

void output_commit_notify(struct wl_listener *l, void* data) {
    struct wlr_surface *surface = data;
    pixman_region32_t *damage;
    wlr_surface_get_effective_damage(surface, damage);
}

// Called when output is ready to display a frame (usually at output's refresh
// rate)
void output_frame_notify(struct wl_listener *listener, void *data) {
    struct wlc_output *o = wl_container_of(listener, o, frame);

    clock_gettime(CLOCK_MONOTONIC, &o->last_frame);

    // Makes OpenGL context current
    if (!wlr_output_attach_render(o->wlr_output, NULL)) {
        ERROR("Failed to attach renderer\n");
        return;
    }

    int width, height;
    wlr_output_effective_resolution(o->wlr_output, &width, &height);

    wlr_renderer_begin(renderer, width, height);
    float color[4] = {0.3, 0.3, 0.3, 1.0};
    wlr_renderer_clear(renderer, color);

    // Renders each client in client list. List is ordered from front to back,
    // so iterate over list backwards
    struct wlc_client *c;
    wl_list_for_each_reverse(c, &zstack, zlink) {
        // Do not render client if it is not mapped
        if (!visible(c, foutput)) continue;

        struct render_data rdata = {
            .output = o->wlr_output,
            .client = c,
            .renderer = renderer,
            .when = &o->last_frame,
        };
        wlr_xdg_surface_for_each_surface(c->xdg_surface, render_surface, &rdata);
    }
    wlr_output_render_software_cursors(o->wlr_output, NULL); // Needed for software cursor (no GPU)

    // Conclude rendering and swap buffers
    wlr_renderer_end(renderer);
    wlr_output_commit(o->wlr_output);
}

// Raised when output device is removed. Removes all lists and frees memory
void output_destroy_notify(struct wl_listener *listener, void *data) {
    struct wlc_output *o = wl_container_of(listener, o, destroy);
    wl_list_remove(&o->link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->frame.link);
    free(o);
}

// Raised by backend when new output becomes available
void new_output_notify(struct wl_listener *listener, void *data) {
    struct wlr_output *wlr_output = data;

    // Set the mode of the monityr (tuple - width, height, refresh rate). For
    // now, this is set to the output's preferred mode
    if (!wl_list_empty(&wlr_output->modes)) {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
        wlr_output_set_mode(wlr_output, mode);
        wlr_output_enable(wlr_output, true);
        if (wlr_output_commit(wlr_output)) {
            return;
        }
    }

    struct wlc_output *o = calloc(1, sizeof(struct wlc_output));
    o->wlr_output = wlr_output;

    // o->frame.notify = output_frame_notify;
    // wl_signal_add(&wlr_output->events.frame, &o->frame);
    // o->destroy.notify = output_destroy_notify;
    // wl_signal_add(&wlr_output->events.destroy, &o->destroy);
    listen(&o->frame, output_frame_notify, &wlr_output->events.frame);
    listen(&o->destroy, output_destroy_notify, &wlr_output->events.destroy);
    listen(&o->commit, output_commit_notify, &wlr_output->events.commit);

    o->n_master = 1;
    o->f_master = 0.55;
    o->layout = 0;
    o->tag = 1;

    // Add output to output layout. Arranges from left to right
    wlr_output_layout_add_auto(output_layout, wlr_output);

    o->geom = wlr_output_layout_get_box(output_layout, o->wlr_output);
    wlr_output->data = o;
    wl_list_insert(&outputs, &o->link);
}

// Event raised when cursor provides server with cursor image
void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = seat->pointer_state.focused_client;

    // Ensure that the client actually has pointer focus. Once confirmed, cursor
    // uses provied surface as cursor image.
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(csr, 
                event->surface, 
                event->hotspot_x,
                event->hotspot_y);
    }
}

bool process_keybindings(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(display);
        break;
    case XKB_KEY_1:
        toggle_tag(1);
        break;
    case XKB_KEY_2:
        toggle_tag(1 << 1);
        break;
    case XKB_KEY_j:
        focus_next(1);
        break;
    case XKB_KEY_k:
        focus_next(-1);
        break;
    case XKB_KEY_t:
        foutput->layout = 0;
        layouts[foutput->layout].l();
        break;
    case XKB_KEY_m:
        foutput->layout = 1;
        layouts[foutput->layout].l();
        break;
    case XKB_KEY_s:
        swap_master();
        layouts[foutput->layout].l();
        break;
    default:
        return false;
    }
    return true;
}

// Handles modifier key presses. Passes modifier key to the client
void keyboard_modifiers_notify(struct wl_listener *listener, void *data) {
    struct wlc_keyboard *kb = wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(seat, kb->device);
    wlr_seat_keyboard_notify_modifiers(seat, &kb->device->keyboard->modifiers);
}

// Handle key presses
void keyboard_key_notify(struct wl_listener *listener, void *data) {
    struct wlc_keyboard *kb = wl_container_of(listener, kb, key);
    struct wlr_event_keyboard_key *event = data;

    // Get the keycode and convert to keysym
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(kb->device->keyboard->xkb_state,
                                       keycode, &syms);
    bool handled = false;
    uint32_t mods = wlr_keyboard_get_modifiers(kb->device->keyboard);
    // See if there are any keybinds we are aware of using these keys
    if ((mods & MODKEY) && event->state == WLR_BUTTON_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = process_keybindings(syms[i]);
        }
    }

    // Pass keyboard event to client
    if (!handled) {
        wlr_seat_set_keyboard(seat, kb->device);
        wlr_seat_keyboard_notify_key(seat, 
                event->time_msec, 
                event->keycode,
                event->state);
    }
}

// Destroy the keyboard device. Need to remove all wl_list items and free up the
// memory
void keyboard_destroy_notify(struct wl_listener *listener, void *data) {
    struct wlc_keyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->modifiers.link);
    free(kb);
}

// Create the new keyboard device
void create_new_keyboard(struct wlr_input_device *dev) {
    struct wlc_keyboard *kb = calloc(1, sizeof(struct wlc_keyboard));
    kb->device = dev;

    // Prepare xkb keymap and assigns to keyboard
    struct xkb_rule_names rules = {0};
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(dev->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(dev->keyboard, 25, 600);

    // Listen for keyboard key presses and modifiers and keyboard device removal
    // kb->modifiers.notify = keyboard_modifiers_notify;
    // wl_signal_add(&dev->keyboard->events.modifiers, &kb->modifiers);
    // kb->key.notify = keyboard_key_notify;
    // wl_signal_add(&dev->keyboard->events.key, &kb->key);
    // kb->destroy.notify = keyboard_destroy_notify;
    // wl_signal_add(&dev->keyboard->events.destroy, &kb->destroy);
    
    listen(&kb->modifiers, keyboard_modifiers_notify, &dev->keyboard->events.modifiers);
    listen(&kb->key, keyboard_key_notify , &dev->keyboard->events.key);
    listen(&kb->destroy, keyboard_destroy_notify, &dev->keyboard->events.destroy);

    wlr_seat_set_keyboard(seat, dev);
    wl_list_insert(&keyboards, &kb->link);
}

// Create the new pointer
void create_new_pointer(struct wlr_input_device *dev) {
    wlr_cursor_attach_input_device(csr, dev);
}

// What to do when a new input is detected. Is able to detect keyboard and
// pointer (mouse) devices.
void new_input_notify(struct wl_listener *listener, void *data) {
    // Set up new input device
    struct wlr_input_device *dev = data;
    INFO("New input device %d detected", dev->type);
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        INFO("New keyboard device %d detected", dev->type);
        create_new_keyboard(dev);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        INFO("New pointer device %d detected", dev->type);
        create_new_pointer(dev);
        break;
    default:
        INFO("Input device %d not recognized", dev->type);
        break;
    }

    // Inform wayland seat of its capabilities. Seat is always has pointer
    // capability and has keyboard capability if keyboard device exists
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(seat, caps);
}

bool setup() {
    // Create wayland display
    display = wl_display_create();
    if (!display) {
        ERROR("wl_display failed to be created");
        return false;
    }

    // Create wayland backend
    backend = wlr_backend_autocreate(display);
    if (!backend) {
        ERROR("wlr_backend failed to be created");
        return false;
    }

    // Set up wayland renderer and attach to display
    renderer = wlr_backend_get_renderer(backend);
    wlr_renderer_init_wl_display(renderer, display);

    wlr_compositor_create(display, renderer); // Create wayland compositor

    // Set up wayland outputs
    output_layout = wlr_output_layout_create();
    wl_list_init(&outputs);
    // new_output.notify = new_output_notify;
    // wl_signal_add(&backend->events.new_output, &new_output);
    listen(&new_output, new_output_notify, &backend->events.new_output);

    // Initialize client lists
    wl_list_init(&lstack);
    wl_list_init(&fstack);
    wl_list_init(&zstack);

    // Set up xdg shell
    xdg_shell = wlr_xdg_shell_create(display);
    // new_xdg_surface.notify = new_xdg_surface_notify;
    // wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);
    listen(&new_xdg_surface, new_xdg_surface_notify, &xdg_shell->events.new_surface);

    wlr_xdg_decoration_manager_v1_create(display);

    // Set up cursor
    csr = wlr_cursor_create();
    wlr_cursor_attach_output_layout(csr, output_layout);

    cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(cursor_mgr, 1);

    // cursor_motion.notify = cursor_motion_notify;
    // wl_signal_add(&csr->events.motion, &cursor_motion);
    // cursor_motion_absolute.notify = cursor_motion_absolute_notify;
    // wl_signal_add(&csr->events.motion_absolute, &cursor_motion_absolute);
    // cursor_button.notify = cursor_button_notify;
    // wl_signal_add(&csr->events.button, &cursor_button);
    // cursor_axis.notify = cursor_axis_notify;
    // wl_signal_add(&csr->events.axis, &cursor_axis);
    // cursor_frame.notify = cursor_frame_notify;
    // wl_signal_add(&csr->events.frame, &cursor_frame);
    listen(&cursor_motion, cursor_motion_notify, &csr->events.motion);
    listen(&cursor_motion_absolute, cursor_motion_absolute_notify, &csr->events.motion_absolute);
    listen(&cursor_button, cursor_button_notify, &csr->events.button);
    listen(&cursor_axis, cursor_axis_notify, &csr->events.axis);
    listen(&cursor_frame, cursor_frame_notify, &csr->events.frame);

    // Set up keyboard and wayland seat
    wl_list_init(&keyboards);
    // new_input.notify = new_input_notify;
    // wl_signal_add(&backend->events.new_input, &new_input);
    listen(&new_input, new_input_notify, &backend->events.new_input);
    seat = wlr_seat_create(display, "seat0");
    // request_cursor.notify = seat_request_cursor;
    // wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
    listen(&request_cursor, seat_request_cursor, &seat->events.request_set_cursor);

    return true;
}

bool run() {
    // Connect to display socket
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        wlr_backend_destroy(backend);
        wl_display_destroy(display);
        return 1;
    }

    // Attempt to start socket
    if (!wlr_backend_start(backend)) {
        fprintf(stderr, "Failed to start backend\n");
        wlr_backend_destroy(backend);
        wl_display_destroy(display);
        return false;
    }
    foutput = cursor_to_output(csr->x, csr->y);

    // Set environment variable WAYLAND_DISPALY
    setenv("WAYLAND_DISPLAY", socket, true);
    INFO("Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);

    // Run wayland display
    wl_display_run(display);
    return true;
}

void cleanup() {
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
}

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);
    if (!setup()) {
        ERROR("Failure to create server");
        cleanup();
        return 1;
    }

    if (!run()) {
        ERROR("Something went wrong while running the compositor");
        cleanup();
        return 1;
    }

    INFO("Cleaning up");
    cleanup();

    return 0;
}

