WAYLAND_PROTOCOLS=/usr/share/wayland-protocols
CC=gcc
CFLAGS=-DWLR_USE_UNSTABLE -Wall 
INC=-I. -I/usr/include/pixman-1
LDFLAGS=-lwlroots -lwayland-server -lxkbcommon

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
xdg-shell-unstable-v6-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-shell/xdg-shell-unstable-v6.xml $@

xdg-shell-unstable-v6-protocol.c: xdg-shell-unstable-v6-protocol.h
	wayland-scanner private-code \
		$(WAYLAND_PROTOCOLS)/unstable/xdg-shell/xdg-shell-unstable-v6.xml $@

xdg-shell-protocol.h:
	wayland-scanner server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c: xdg-shell-protocol.h
	wayland-scanner private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.o: xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) -c -Werror -o $@ $<

wlc: wlc.o tile.o monocle.o 
	$(CC) $(CFLAGS) $(INC) $^ -o $@ $(LDFLAGS)

wlc.o: wlc.c xdg-shell-protocol.o
	$(CC) $(INC) $(CFLAGS) -c -o $@ $<

tile.o: tile.c 
	$(CC) $(INC) $(CFLAGS) -c -o $@ $< 

monocle.o: monocle.c 
	$(CC) $(INC) $(CFLAGS) -c -o $@ $< 

clean:
	rm -f wlc xdg-shell-protocol.h xdg-shell-protocol.c *.o

.DEFAULT_GOAL=wlc
.PHONY: clean

# wlc: wlc.c tile.c monocle.c xdg-shell-protocol.o
# 	$(CC) $(CFLAGS) \
# 		-g -Werror -I. \
# 		-I/usr/include/pixman-1 \
# 		-DWLR_USE_UNSTABLE \
# 		-o $@ $< \
# 		-lwlroots \
# 		-lwayland-server \
# 		-lxkbcommon
