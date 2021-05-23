#include "wlc.h"
void monocle(){
    struct wlc_client *c;
    wl_list_for_each(c, &lstack, llink) {
        if (!visible(c, foutput)) continue;
            
        move(c, 0, 0);
        resize(c, foutput->geom->width, foutput->geom->height);
    }
    c = fstack_top();
}

