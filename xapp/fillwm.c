/*
 * fillwm -- the window manager inside an xephyr-app display: it makes the
 * application look like an ordinary window of the IRIX desktop by keeping
 * its main window exactly the size of the nested screen, which Xephyr
 * (-resizeable) resizes whenever 4Dwm resizes Xephyr's window.
 *
 *   fillwm [-d display]
 *
 * Policy, per top-level window the application maps:
 *   - a main window (no WM_TRANSIENT_FOR, and _NET_WM_WINDOW_TYPE absent or
 *     _NET_WM_WINDOW_TYPE_NORMAL) is placed at 0,0 at the size of the
 *     screen, with no border, and put back there whatever size it asks for;
 *     when the screen changes size (Xephyr sends a ConfigureNotify on the
 *     root through RANDR) every main window follows;
 *   - anything else (dialogs, transients, utility windows) keeps the size it
 *     asks for and is centred on the screen, clamped to it;
 *   - override-redirect windows (menus, tooltips) are the application's own
 *     business and never reach a window manager.
 * The most recently mapped window has the input focus; when it goes, the
 * focus returns to the newest remaining one.
 *
 * It exits when the display goes away, and refuses to start when another
 * window manager already runs on the display.
 *
 * Plain Xlib and the core protocol only, so it links with IRIX's own
 * libX11 and needs nothing from pkgsrc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#define MAXWIN	256

struct managed {
	Window	w;
	int	fill;		/* main window: kept at the screen's size */
};

static Display *dpy;
static Window root;
static int scr_w, scr_h;
static struct managed wins[MAXWIN];
static int nwins;
static int other_wm;

static Atom a_wm_type, a_wm_type_normal;

static int
on_error(Display *d, XErrorEvent *e)
{
	/* Windows vanish between an event and our request about them. */
	(void)d;
	(void)e;
	return 0;
}

static int
on_startup_error(Display *d, XErrorEvent *e)
{
	(void)d;
	if (e->error_code == BadAccess)
		other_wm = 1;
	return 0;
}

static struct managed *
find(Window w)
{
	int i;

	for (i = 0; i < nwins; i++)
		if (wins[i].w == w)
			return &wins[i];
	return NULL;
}

static int
is_main_window(Window w)
{
	Window owner;
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	int main_window = 1;

	if (XGetTransientForHint(dpy, w, &owner))
		return 0;
	if (XGetWindowProperty(dpy, w, a_wm_type, 0, 1, False, XA_ATOM,
	    &type, &format, &n, &after, &data) == Success && data) {
		if (n > 0 && format == 32 &&
		    ((Atom *)data)[0] != a_wm_type_normal)
			main_window = 0;
		XFree(data);
	}
	return main_window;
}

/* Tell a main window where it is, as ICCCM asks when a request is refused
 * or changes nothing (no real ConfigureNotify comes then). */
static void
send_geometry(Window w)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xconfigure.type = ConfigureNotify;
	ev.xconfigure.event = w;
	ev.xconfigure.window = w;
	ev.xconfigure.x = 0;
	ev.xconfigure.y = 0;
	ev.xconfigure.width = scr_w;
	ev.xconfigure.height = scr_h;
	ev.xconfigure.border_width = 0;
	ev.xconfigure.above = None;
	ev.xconfigure.override_redirect = False;
	XSendEvent(dpy, w, False, StructureNotifyMask, &ev);
}

static void
fill(Window w)
{
	XSetWindowBorderWidth(dpy, w, 0);
	XMoveResizeWindow(dpy, w, 0, 0, scr_w, scr_h);
	send_geometry(w);
}

static void
centre(Window w)
{
	XWindowAttributes wa;
	int x, y, width, height;

	if (!XGetWindowAttributes(dpy, w, &wa))
		return;
	width = wa.width < scr_w ? wa.width : scr_w;
	height = wa.height < scr_h ? wa.height : scr_h;
	x = (scr_w - width) / 2 - wa.border_width;
	y = (scr_h - height) / 2 - wa.border_width;
	XMoveResizeWindow(dpy, w, x < 0 ? 0 : x, y < 0 ? 0 : y,
	    width, height);
}

static void
focus_newest(void)
{
	if (nwins > 0) {
		XRaiseWindow(dpy, wins[nwins - 1].w);
		XSetInputFocus(dpy, wins[nwins - 1].w, RevertToPointerRoot,
		    CurrentTime);
	} else
		XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot,
		    CurrentTime);
}

static void
manage(Window w)
{
	struct managed *m = find(w);

	if (!m) {
		if (nwins == MAXWIN)
			return;
		m = &wins[nwins++];
		m->w = w;
		/* Unmap and destroy of our windows come to the root anyway
		 * (SubstructureNotify); nothing else is needed from them. */
	}
	m->fill = is_main_window(w);
	if (m->fill)
		fill(w);
	else
		centre(w);
	XMapWindow(dpy, w);
	focus_newest();
}

static void
unmanage(Window w)
{
	int i;

	for (i = 0; i < nwins; i++)
		if (wins[i].w == w) {
			memmove(&wins[i], &wins[i + 1],
			    (nwins - i - 1) * sizeof wins[0]);
			nwins--;
			focus_newest();
			return;
		}
}

static void
configure_request(XConfigureRequestEvent *e)
{
	struct managed *m = find(e->window);
	XWindowChanges wc;

	if (m && m->fill) {
		/* Stacking changes are fine; the geometry is ours. */
		if (e->value_mask & (CWSibling | CWStackMode)) {
			wc.sibling = e->above;
			wc.stack_mode = e->detail;
			XConfigureWindow(dpy, e->window,
			    e->value_mask & (CWSibling | CWStackMode), &wc);
		}
		fill(e->window);
		return;
	}
	wc.x = e->x;
	wc.y = e->y;
	wc.width = e->width;
	wc.height = e->height;
	wc.border_width = e->border_width;
	wc.sibling = e->above;
	wc.stack_mode = e->detail;
	XConfigureWindow(dpy, e->window, e->value_mask, &wc);
}

static void
screen_resized(int width, int height)
{
	int i;

	if (width == scr_w && height == scr_h)
		return;
	scr_w = width;
	scr_h = height;
	for (i = 0; i < nwins; i++)
		if (wins[i].fill)
			fill(wins[i].w);
		else
			centre(wins[i].w);
}

/* Windows mapped before we started. */
static void
adopt_existing(void)
{
	Window r, p, *children = NULL;
	unsigned int n, i;
	XWindowAttributes wa;

	if (!XQueryTree(dpy, root, &r, &p, &children, &n))
		return;
	for (i = 0; i < n; i++)
		if (XGetWindowAttributes(dpy, children[i], &wa) &&
		    !wa.override_redirect && wa.map_state == IsViewable)
			manage(children[i]);
	if (children)
		XFree(children);
}

int
main(int argc, char **argv)
{
	const char *name = NULL;
	XEvent ev;

	if (argc == 3 && strcmp(argv[1], "-d") == 0)
		name = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "usage: fillwm [-d display]\n");
		return 2;
	}
	dpy = XOpenDisplay(name);
	if (!dpy) {
		fprintf(stderr, "fillwm: cannot open display %s\n",
		    XDisplayName(name));
		return 1;
	}
	root = DefaultRootWindow(dpy);
	scr_w = DisplayWidth(dpy, DefaultScreen(dpy));
	scr_h = DisplayHeight(dpy, DefaultScreen(dpy));
	a_wm_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	a_wm_type_normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL",
	    False);

	/* Only one client may redirect the root's substructure. */
	XSetErrorHandler(on_startup_error);
	XSelectInput(dpy, root, SubstructureRedirectMask |
	    SubstructureNotifyMask | StructureNotifyMask);
	XSync(dpy, False);
	if (other_wm) {
		fprintf(stderr, "fillwm: another window manager runs on %s\n",
		    XDisplayString(dpy));
		return 1;
	}
	XSetErrorHandler(on_error);

	adopt_existing();
	for (;;) {
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case MapRequest:
			manage(ev.xmaprequest.window);
			break;
		case ConfigureRequest:
			configure_request(&ev.xconfigurerequest);
			break;
		case ConfigureNotify:
			if (ev.xconfigure.window == root)
				screen_resized(ev.xconfigure.width,
				    ev.xconfigure.height);
			break;
		case UnmapNotify:
			/* Our own unmaps do not happen: we never unmap. */
			if (ev.xunmap.event == root)
				unmanage(ev.xunmap.window);
			break;
		case DestroyNotify:
			unmanage(ev.xdestroywindow.window);
			break;
		}
	}
}
