#include "wlc.h"
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
