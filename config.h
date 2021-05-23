#include "wlc.h"
// #include "layouts.h"
void tile();
void monocle();

static struct wlc_layout layouts[] = {
    { tile, "t" },
    { monocle, "m" },
    { NULL, "f" },
};

#define MODKEY WLR_MODIFIER_ALT

uint32_t follow_mouse = 0;
