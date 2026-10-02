/* The windows, the configuration and the event queue. See irisgl_shim.h. */
#include "irisgl_shim.h"
#include <gl/glws.h>
#include <X11/extensions/shape.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

HglIris hgl_iris;
static int zbsize_given;        /* see zbsize */

void
hgl_irisgl_missing(const char *name)
{
	/* One line per entry point, not per call: a program in a loop would
	 * otherwise bury everything else in the same message. */
	static const char *said[256];
	static int n = 0;
	int i;

	for (i = 0; i < n; i++)
		if (said[i] == name)
			return;
	if (n < 256)
		said[n++] = name;
	fprintf(stderr, "IRIS GL: %s is not implemented\n", name);
}

void
hgl_irisgl_trace(const char *name)
{
	static int on = -1;
	static long left = 400;

	/* IRIS_IRISGL_TRACE=1 traces the first 400 calls; a larger number,
	 * that many. */
	if (on < 0) {
		char *v = getenv("IRIS_IRISGL_TRACE");
		on = v && *v >= '1' && *v <= '9';
		if (on && atol(v) > 1)
			left = atol(v);
	}
	/* Capped: the interesting part is the order of the first frame, and a
	 * draw loop would otherwise write millions of lines a second. */
	if (on && left > 0) {
		left--;
		fprintf(stderr, "[irisgl] %s%s\n", name, left ? "" : "  (trace full)");
		fflush(stderr);
	}
}

/* The trace with arguments; formatted only when tracing is on. */
void
hgl_irisgl_tracef(const char *fmt, ...)
{
	static int on = -1;
	char buf[160];
	va_list ap;

	if (on < 0) {
		char *v = getenv("IRIS_IRISGL_TRACE");
		on = v && *v >= '1' && *v <= '9';
	}
	if (!on)
		return;
	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);
	hgl_irisgl_trace(buf);
}

/* IRIS_IRISGL_XSYNC=1: every X connection the library uses is synchronous,
 * so an X error is reported at the call that caused it -- in the trace, just
 * after it -- instead of some requests later. */
static void
xsync_if_asked(Display *d)
{
	static int want = -1;

	if (want < 0)
		want = getenv("IRIS_IRISGL_XSYNC") != NULL;
	if (want && d != NULL)
		XSynchronize(d, True);
}

Display *
hgl_display(void)
{
	if (hgl_iris.dpy == NULL) {
		hgl_iris.dpy = XOpenDisplay(NULL);
		if (hgl_iris.dpy == NULL) {
			fprintf(stderr, "IRIS GL: no display\n");
			exit(1);
		}
		xsync_if_asked(hgl_iris.dpy);
	}
	return hgl_iris.dpy;
}

static int
screen_height(void)
{
	return HeightOfScreen(DefaultScreenOfDisplay(hgl_display()));
}

/* ---- the window table ----
 *
 * A window's identifier (gid) is its index here; 0 is never one. A slot
 * holds the per-window half of HglIris while the window is not current.
 */
#define HGL_MAXWIN 64

typedef struct {
	int used;
	Display *gldpy;
	Window win;
	GLXContext ctx;
	int w, h;
	int opened, glx, layer, noport;
	int origin_unknown;      /* see getorigin */
	int want_rgb, want_double, want_zbuf, want_ms;
	int front, blend, mmode;
	int mask_l, mask_r, mask_b, mask_t;
	int defaults_done;       /* hgl_window_defaults ran in its context */
	struct hgl_layers *layers;
	unsigned enables;
	int index_win;
	Colormap index_cmap;
	char title[128];
} HglWin;

static HglWin wins[HGL_MAXWIN];
/* Every context shares display lists with the first: IRIS GL's objects and
 * fonts belong to the program, not to one window. */
static GLXContext first_ctx;

static void
save_current(void)
{
	HglWin *s;
	HglIris *g = &hgl_iris;

	if (g->gid <= 0 || g->gid >= HGL_MAXWIN || !wins[g->gid].used)
		return;
	s = &wins[g->gid];
	s->gldpy = g->gldpy;
	s->win = g->win;
	s->ctx = g->ctx;
	s->w = g->w;
	s->h = g->h;
	s->opened = g->opened;
	s->glx = g->glx;
	s->layer = g->layer;
	s->layers = g->layers;
	s->enables = g->enables;
	s->index_win = g->index_win;
	s->index_cmap = g->index_cmap;
	s->want_rgb = g->want_rgb;
	s->want_double = g->want_double;
	s->want_zbuf = g->want_zbuf;
	s->want_ms = g->want_ms;
	s->front = g->front;
	s->blend = g->blend;
	s->mmode = g->mmode;
	s->mask_l = g->mask_l;
	s->mask_r = g->mask_r;
	s->mask_b = g->mask_b;
	s->mask_t = g->mask_t;
	memcpy(s->title, g->title, sizeof s->title);
}

static void
load_current(long gid)
{
	HglWin *s = &wins[gid];
	HglIris *g = &hgl_iris;

	g->gid = gid;
	g->gldpy = s->gldpy;
	g->win = s->win;
	g->ctx = s->ctx;
	g->w = s->w;
	g->h = s->h;
	g->opened = s->opened;
	g->glx = s->glx;
	g->layer = s->layer;
	g->layers = s->layers;
	g->enables = s->enables;
	g->index_win = s->index_win;
	g->index_cmap = s->index_cmap;
	g->want_rgb = s->want_rgb;
	g->want_double = s->want_double;
	g->want_zbuf = s->want_zbuf;
	g->want_ms = s->want_ms;
	g->front = s->front;
	g->blend = s->blend;
	g->mmode = s->mmode;
	g->mask_l = s->mask_l;
	g->mask_r = s->mask_r;
	g->mask_b = s->mask_b;
	g->mask_t = s->mask_t;
	memcpy(g->title, s->title, sizeof g->title);
}

static long
gid_of(Window w)
{
	long i;

	if (hgl_iris.gid > 0 && hgl_iris.win == w)
		return hgl_iris.gid;
	for (i = 1; i < HGL_MAXWIN; i++)
		if (wins[i].used && wins[i].win == w)
			return i;
	return 0;
}

static long
new_slot(void)
{
	long i;

	for (i = 1; i < HGL_MAXWIN; i++) {
		if (!wins[i].used) {
			memset(&wins[i], 0, sizeof wins[i]);
			wins[i].used = 1;
			return i;
		}
	}
	return -1;
}

/*
 * A window's size, asked of the server. An IRIS GL window's is kept up to date
 * by ConfigureNotify; a GLXlink'ed one belongs to the program, whose event
 * loop sees its resizes, so it is asked whenever it matters.
 */
static void
refresh_glx_size(void)
{
	XWindowAttributes wa;

	if (hgl_iris.glx && hgl_iris.win &&
	    XGetWindowAttributes(hgl_iris.gldpy, hgl_iris.win, &wa)) {
		hgl_iris.w = wa.width;
		hgl_iris.h = wa.height;
	}
}

/* ---- viewport, screen mask and a new window's state ---- */

void
hgl_apply_scrmask(void)
{
	HglIris *g = &hgl_iris;

	if (g->mask_l > g->mask_r || g->mask_b > g->mask_t) {
		/* scrmask(3G): an inverted mask protects every pixel. */
		glScissor(0, 0, 0, 0);
	} else {
		glScissor(g->mask_l, g->mask_b, g->mask_r - g->mask_l + 1, g->mask_t - g->mask_b + 1);
	}
	glEnable(GL_SCISSOR_TEST);
}

void
hgl_set_viewport(int l, int r, int b, int t)
{
	HglIris *g = &hgl_iris;

	glViewport(l, b, r - l + 1, t - b + 1);
	/* "When viewport is called, the screen mask is set to match the newly
	 * specified viewport" -- which is also what makes clear() fill only
	 * the viewport, as it does in IRIS GL. */
	g->mask_l = l;
	g->mask_r = r;
	g->mask_b = b;
	g->mask_t = t;
	hgl_apply_scrmask();
}

void
hgl_window_defaults(void)
{
	HglIris *g = &hgl_iris;

	hgl_set_viewport(0, g->w - 1, 0, g->h - 1);
	/* mmode MSINGLE, whose matrix is ortho2 matching the window: a program
	 * that draws in pixels without ever setting a matrix relies on it. */
	g->mmode = MSINGLE;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glOrtho(-0.5, g->w - 0.5, -0.5, g->h - 0.5, -1.0, 1.0);
	/* Lighting uses normals of unit length whatever the matrix does to them:
	 * see nmode. */
	glEnable(GL_NORMALIZE);
	/* IRIS GL's depth test passes equal depths (zfunction(3G): ZF_LEQUAL);
	 * OpenGL's starts as GL_LESS, which loses a second pass over the same
	 * surface -- an outline over its faces, a highlight. */
	glDepthFunc(GL_LEQUAL);
	if (g->gid > 0 && g->gid < HGL_MAXWIN)
		wins[g->gid].defaults_done = 1;
}

/* ---- window constraints ----
 *
 * Set before winopen (or before winconstraints on an open window), consumed by
 * it, then back to the defaults. What each turns into was read off SGI's own
 * libgl.so on Xsgi, with no window manager running:
 *   - no size: 100x100 at (500,500), hints of increment 1x1, base 0x0;
 *   - prefsize: that size, min and max size both set to it, and the Motif
 *     function hint without resize and maximize;
 *   - prefposition: the same plus a USPosition at the rectangle;
 *   - minsize, maxsize, keepaspect, stepunit, fudge: hints only (min/max
 *     size, min = max aspect, increment, base size) -- the size is still
 *     100x100;
 *   - noborder: Motif decorations 0, not override-redirect.
 */
static struct {
	int pos, x1, x2, y1, y2;
	int size, sw, sh;
	int min, minw, minh;
	int max, maxw, maxh;
	int aspect, ax, ay;
	int step, sx, sy;
	int fudge, fx, fy;
	int noborder, noport;
} cons;

static void
cons_reset(void)
{
	memset(&cons, 0, sizeof cons);
}

void
prefposition(long x1, long x2, long y1, long y2)
{
	TRACE("prefposition");
	cons.pos = 1;
	cons.x1 = (int)(x1 < x2 ? x1 : x2);
	cons.x2 = (int)(x1 < x2 ? x2 : x1);
	cons.y1 = (int)(y1 < y2 ? y1 : y2);
	cons.y2 = (int)(y1 < y2 ? y2 : y1);
}

void
prefsize(long x, long y)
{
	TRACE("prefsize");
	if (x <= 0 || y <= 0)
		return;
	cons.size = 1;
	cons.sw = (int)x;
	cons.sh = (int)y;
}

void minsize(long x, long y) { cons.min = 1; cons.minw = (int)x; cons.minh = (int)y; }
void maxsize(long x, long y) { cons.max = 1; cons.maxw = (int)x; cons.maxh = (int)y; }
void keepaspect(long x, long y) { if (x > 0 && y > 0) { cons.aspect = 1; cons.ax = (int)x; cons.ay = (int)y; } }
void stepunit(long x, long y) { if (x > 0 && y > 0) { cons.step = 1; cons.sx = (int)x; cons.sy = (int)y; } }
void fudge(long x, long y) { cons.fudge = 1; cons.fx = (int)x; cons.fy = (int)y; }
void noborder(void) { cons.noborder = 1; }
void noport(void) { cons.noport = 1; }
void iconsize(long w, long h) { (void)w; (void)h; }
void icontitle(String s) { (void)s; }
/* There is only one screen background, and nothing here draws behind the
 * other windows; the program's window is an ordinary one. */
void imakebackground(void) { }
void foreground(void) { /* do not fork into the background: we never do */ }

/* The rectangle the constraints ask for, in X coordinates. */
static int
cons_geometry(int *x, int *y, int *w, int *h)
{
	if (cons.pos) {
		*w = cons.x2 - cons.x1 + 1;
		*h = cons.y2 - cons.y1 + 1;
		*x = cons.x1;
		/* IRIS GL's origin is the bottom left of the screen. */
		*y = screen_height() - cons.y2 - 1;
		return 2;
	}
	if (cons.size) {
		*w = cons.sw;
		*h = cons.sh;
		return 1;
	}
	return 0;
}

static void
set_hints(Window win, int x, int y, int w, int h)
{
	Display *d = hgl_display();
	XSizeHints *sh = XAllocSizeHints();
	Atom motif = XInternAtom(d, "_MOTIF_WM_HINTS", False);
	long mh[4];
	int fixed = cons.pos || cons.size;

	if (sh == NULL)
		return;
	sh->flags = PResizeInc | PBaseSize;
	sh->width_inc = cons.step ? cons.sx : 1;
	sh->height_inc = cons.step ? cons.sy : 1;
	sh->base_width = cons.fudge ? cons.fx : 0;
	sh->base_height = cons.fudge ? cons.fy : 0;
	if (cons.pos) {
		sh->flags |= USPosition;
		sh->x = x;
		sh->y = y;
	}
	if (fixed) {
		sh->flags |= PMinSize | PMaxSize;
		sh->min_width = sh->max_width = w;
		sh->min_height = sh->max_height = h;
	} else {
		if (cons.min) {
			sh->flags |= PMinSize;
			sh->min_width = cons.minw;
			sh->min_height = cons.minh;
		}
		if (cons.max) {
			sh->flags |= PMaxSize;
			sh->max_width = cons.maxw;
			sh->max_height = cons.maxh;
		}
	}
	if (cons.aspect) {
		sh->flags |= PAspect;
		sh->min_aspect.x = sh->max_aspect.x = cons.ax;
		sh->min_aspect.y = sh->max_aspect.y = cons.ay;
	}
	XSetWMNormalHints(d, win, sh);
	XFree(sh);
	/* MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS, as SGI's library sets
	 * them: every function, less resize and maximize for a fixed size. */
	mh[0] = 3;
	mh[1] = fixed ? 0x80002cL : 0x80003eL;
	mh[2] = cons.noborder ? 0 : 1;
	mh[3] = 0;
	XChangeProperty(d, win, motif, motif, 32, PropModeReplace, (unsigned char *)mh, 4);
}

/*
 * WM_CLASS from the winopen name, as SGI's library makes it: blanks, dots and
 * colons dropped with the letter after a blank capitalised, the first letter
 * lower case for the instance and upper case for the class; "noName" if
 * nothing is left.
 */
static void
set_class(Window win, const char *name)
{
	char inst[128], cls[128];
	XClassHint ch;
	int n = 0, up = 0;
	const char *p;

	for (p = name; *p && n < (int)sizeof inst - 1; p++) {
		if (*p == ' ' || *p == '\t') {
			up = 1;
			continue;
		}
		if (*p == '.' || *p == ':' || *p == '*' || *p == '?')
			continue;
		inst[n++] = up && *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
		up = 0;
	}
	inst[n] = '\0';
	if (n == 0)
		strcpy(inst, "noName");
	strcpy(cls, inst);
	if (inst[0] >= 'A' && inst[0] <= 'Z')
		inst[0] = inst[0] - 'A' + 'a';
	if (cls[0] >= 'a' && cls[0] <= 'z')
		cls[0] = cls[0] - 'a' + 'A';
	ch.res_name = inst;
	ch.res_class = cls;
	XSetClassHint(hgl_display(), win, &ch);
}

static XVisualInfo *
gl_visual(Display *d)
{
	static XVisualInfo *vi;
	int attrs[16], n = 0;

	if (vi != NULL)
		return vi;
	/*
	 * Always RGBA and double-buffered, whatever the program asked for. The
	 * OpenGL shim puts a frame on the X window when the buffers are swapped
	 * and at no other time, so a single-buffered IRIS GL program -- which
	 * expects its drawing to appear as it happens -- would draw into a
	 * buffer nobody ever showed. Instead we take two buffers and present at
	 * the points where such a program expects to be seen. Colour-map mode is
	 * a table here (irisgl_draw.c), so the visual does not depend on it.
	 */
	attrs[n++] = GLX_RGBA;
	attrs[n++] = GLX_DOUBLEBUFFER;
	attrs[n++] = GLX_RED_SIZE;   attrs[n++] = 8;
	attrs[n++] = GLX_GREEN_SIZE; attrs[n++] = 8;
	attrs[n++] = GLX_BLUE_SIZE;  attrs[n++] = 8;
	attrs[n++] = GLX_DEPTH_SIZE; attrs[n++] = 24;
	attrs[n] = None;
	vi = glXChooseVisual(d, DefaultScreen(d), attrs);
	if (vi == NULL) {
		fprintf(stderr, "IRIS GL: no visual\n");
		exit(1);
	}
	return vi;
}

static void
make_context(void)
{
	HglIris *g = &hgl_iris;
	XVisualInfo tmpl, *ivi = NULL;
	XWindowAttributes wa;
	int n;

	/* An index window's context is made for the window's own colour-map
	 * visual, which puts it in the OpenGL shim's colour index mode. */
	if (g->index_win && XGetWindowAttributes(g->gldpy, g->win, &wa)) {
		tmpl.visualid = XVisualIDFromVisual(wa.visual);
		ivi = XGetVisualInfo(g->gldpy, VisualIDMask, &tmpl, &n);
	}
	g->ctx = glXCreateContext(g->gldpy, ivi != NULL ? ivi : gl_visual(g->gldpy), first_ctx, True);
	if (ivi != NULL)
		XFree(ivi);
	if (!g->ctx) {
		fprintf(stderr, "IRIS GL: glXCreateContext failed\n");
		exit(1);
	}
	if (first_ctx == NULL)
		first_ctx = g->ctx;
	if (!glXMakeCurrent(g->gldpy, g->win, g->ctx)) {
		fprintf(stderr, "IRIS GL: glXMakeCurrent failed\n");
		exit(1);
	}
	hgl_irisgl_trace("context current");
}

/*
 * SGI's winopen returns once the window is on the screen, at the size the
 * window manager gave it. A window opened with only minsize or keepaspect is
 * made bigger by 4Dwm before it maps, and programs size what they draw from
 * the getsize that follows: gr_osview scales its fonts by it and, never
 * asking for REDRAW, never learns another size. So wait for the server to
 * call the window viewable, then take its size -- but not forever: with no
 * window manager, or one that leaves it unmapped, the size asked for stands.
 * Only the window's attributes are asked for; the Expose and ConfigureNotify
 * the program's queue turns into REDRAW stay queued.
 */
static void
wait_viewable(Display *d, Window win, int *w, int *h)
{
	XWindowAttributes wa;
	struct timespec ts = { 0, 10 * 1000 * 1000 };
	int i;

	for (i = 0; i < 200; i++) {
		XSync(d, False);
		if (XGetWindowAttributes(d, win, &wa) && wa.map_state == IsViewable) {
			*w = wa.width;
			*h = wa.height;
			return;
		}
		nanosleep(&ts, NULL);
	}
}

/* The X window and context for the current slot, from the constraints. */
static void
open_window(void)
{
	XSetWindowAttributes swa;
	HglIris *g = &hgl_iris;
	Display *d = hgl_display();
	XVisualInfo *vi = gl_visual(d);
	static Colormap cmap;
	int x = 500, y = 500, w = 100, h = 100;

	cons_geometry(&x, &y, &w, &h);
	if (cmap == None)
		cmap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	memset(&swa, 0, sizeof swa);
	swa.colormap = cmap;
	/* A window in a visual other than its parent's needs a border pixel
	 * of that visual; without one CreateWindow is a BadMatch on a server
	 * whose root is not 24-bit, Xsgi among them. */
	swa.border_pixel = 0;
	swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
	    ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
	    StructureNotifyMask | EnterWindowMask | LeaveWindowMask;
	g->gldpy = d;
	g->vi = vi;
	g->win = XCreateWindow(d, RootWindow(d, vi->screen), x, y, (unsigned)w, (unsigned)h,
	    0, vi->depth, InputOutput, vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XStoreName(d, g->win, g->title);
	set_class(g->win, g->title);
	set_hints(g->win, x, y, w, h);
	if (!cons.noport) {
		XMapWindow(d, g->win);
		wait_viewable(d, g->win, &w, &h);
	}
	g->w = w;
	g->h = h;
	g->opened = 1;
	wins[g->gid].origin_unknown = !cons.pos && !cons.size;
	make_context();
	hgl_window_defaults();
	cons_reset();
}

void
hgl_iris_ensure(void)
{
	if (!hgl_iris.opened)
		winopen("IRIS GL");
}

/* ---- the queue ---- */

static short tied_value(short v);
static void push(short dev, short val);

/*
 * tie(3G): each queued event of button b is followed on the queue by events
 * for v1 and then v2 carrying their values at that moment (0 for none).
 * buttonfly ties LEFTMOUSE to MOUSEX and MOUSEY and reads the pointer from
 * the two qreads after the click; with tie a no-op those read whatever came
 * next (a timer, value 0), so every click missed every button.
 */
static short tied[1024][2];

/* A button event, and the valuators tie() attached to it. */
static void
push_button(short dev, short val)
{
	int i;

	push(dev, val);
	for (i = 0; i < 2; i++)
		if (tied[dev & 1023][i])
			push(tied[dev & 1023][i], tied_value(tied[dev & 1023][i]));
}

static void
push(short dev, short val)
{
	int next = (hgl_iris.qtail + 1) % 256;

	if (next == hgl_iris.qhead)
		return;                 /* full: the oldest events matter most */
	hgl_iris.q[hgl_iris.qtail].dev = dev;
	hgl_iris.q[hgl_iris.qtail].val = val;
	hgl_iris.qtail = next;
}

/* A window's new size, wherever its fields are kept. */
static void
resized(long gid, int w, int h)
{
	if (gid == hgl_iris.gid) {
		hgl_iris.w = w;
		hgl_iris.h = h;
		hgl_layers_resize(hgl_iris.layers, hgl_iris.gldpy, w, h);
	} else {
		wins[gid].w = w;
		wins[gid].h = h;
		hgl_layers_resize(wins[gid].layers, wins[gid].gldpy, w, h);
	}
}

/* The IRIS GL key device for an unshifted keysym, from <gl/device.h>. */
static short
key_device(KeySym ks)
{
	static const struct { KeySym ks; short dev; } keys[] = {
		{ XK_a, AKEY }, { XK_b, BKEY }, { XK_c, CKEY }, { XK_d, DKEY }, { XK_e, EKEY },
		{ XK_f, FKEY }, { XK_g, GKEY }, { XK_h, HKEY }, { XK_i, IKEY }, { XK_j, JKEY },
		{ XK_k, KKEY }, { XK_l, LKEY }, { XK_m, MKEY }, { XK_n, NKEY }, { XK_o, OKEY },
		{ XK_p, PKEY }, { XK_q, QKEY }, { XK_r, RKEY }, { XK_s, SKEY }, { XK_t, TKEY },
		{ XK_u, UKEY }, { XK_v, VKEY }, { XK_w, WKEY }, { XK_x, XKEY }, { XK_y, YKEY },
		{ XK_z, ZKEY },
		{ XK_0, ZEROKEY }, { XK_1, ONEKEY }, { XK_2, TWOKEY }, { XK_3, THREEKEY },
		{ XK_4, FOURKEY }, { XK_5, FIVEKEY }, { XK_6, SIXKEY }, { XK_7, SEVENKEY },
		{ XK_8, EIGHTKEY }, { XK_9, NINEKEY },
		{ XK_Break, BREAKKEY }, { XK_Control_L, CTRLKEY }, { XK_Caps_Lock, CAPSLOCKKEY },
		{ XK_Shift_R, RIGHTSHIFTKEY }, { XK_Shift_L, LEFTSHIFTKEY }, { XK_Escape, ESCKEY },
		{ XK_Tab, TABKEY }, { XK_Return, RETKEY }, { XK_space, SPACEKEY },
		{ XK_Linefeed, LINEFEEDKEY }, { XK_BackSpace, BACKSPACEKEY }, { XK_Delete, DELKEY },
		{ XK_semicolon, SEMICOLONKEY }, { XK_period, PERIODKEY }, { XK_comma, COMMAKEY },
		{ XK_apostrophe, QUOTEKEY }, { XK_grave, ACCENTGRAVEKEY }, { XK_minus, MINUSKEY },
		{ XK_slash, VIRGULEKEY }, { XK_backslash, BACKSLASHKEY }, { XK_equal, EQUALKEY },
		{ XK_bracketleft, LEFTBRACKETKEY }, { XK_bracketright, RIGHTBRACKETKEY },
		{ XK_Left, LEFTARROWKEY }, { XK_Down, DOWNARROWKEY }, { XK_Right, RIGHTARROWKEY },
		{ XK_Up, UPARROWKEY },
		{ XK_KP_0, PAD0 }, { XK_KP_1, PAD1 }, { XK_KP_2, PAD2 }, { XK_KP_3, PAD3 },
		{ XK_KP_4, PAD4 }, { XK_KP_5, PAD5 }, { XK_KP_6, PAD6 }, { XK_KP_7, PAD7 },
		{ XK_KP_8, PAD8 }, { XK_KP_9, PAD9 }, { XK_KP_Decimal, PADPERIOD },
		{ XK_KP_Subtract, PADMINUS }, { XK_KP_Enter, PADENTER }, { XK_KP_Add, PADPLUSKEY },
		{ XK_KP_Multiply, PADASTERKEY }, { XK_KP_Divide, PADVIRGULEKEY },
		{ XK_Alt_L, LEFTALTKEY }, { XK_Alt_R, RIGHTALTKEY }, { XK_Control_R, RIGHTCTRLKEY },
		{ XK_F1, F1KEY }, { XK_F2, F2KEY }, { XK_F3, F3KEY }, { XK_F4, F4KEY },
		{ XK_F5, F5KEY }, { XK_F6, F6KEY }, { XK_F7, F7KEY }, { XK_F8, F8KEY },
		{ XK_F9, F9KEY }, { XK_F10, F10KEY }, { XK_F11, F11KEY }, { XK_F12, F12KEY },
		{ XK_Print, PRINTSCREENKEY }, { XK_Scroll_Lock, SCROLLLOCKKEY }, { XK_Pause, PAUSEKEY },
		{ XK_Insert, INSERTKEY }, { XK_Home, HOMEKEY }, { XK_Prior, PAGEUPKEY },
		{ XK_End, ENDKEY }, { XK_Next, PAGEDOWNKEY }, { XK_Num_Lock, NUMLOCKKEY },
	};
	unsigned i;

	for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
		if (keys[i].ks == ks)
			return keys[i].dev;
	return 0;
}

/* An X event as IRIS GL's queue sees it. Only the devices the program asked
 * for with qdevice reach the queue, which is what IRIS GL promises and what
 * keeps a program that never asked for motion from drowning in it. */
static void
xlate_event(XEvent *e)
{
	HglIris *g = &hgl_iris;
	short dev = 0;
	long gid = gid_of(e->xany.window);
	int ow, oh;
	KeySym ks;
	char buf[8];

	switch (e->type) {
	case Expose:
		if (gid && e->xexpose.count == 0 && g->queued[REDRAW & 1023])
			push(REDRAW, (short)gid);
		return;
	case ConfigureNotify:
		if (!gid)
			return;
		wins[gid].origin_unknown = 0;
		ow = gid == g->gid ? g->w : wins[gid].w;
		oh = gid == g->gid ? g->h : wins[gid].h;
		if (e->xconfigure.width == ow && e->xconfigure.height == oh)
			return;         /* moved, not resized */
		resized(gid, e->xconfigure.width, e->xconfigure.height);
		if (g->queued[REDRAW & 1023]) {
			/* The program redraws, and calls reshapeviewport itself. */
			push(REDRAW, (short)gid);
		} else if (gid == g->gid) {
			/*
			 * A program that never asked for REDRAW still gets a
			 * viewport that covers its window, as IRIS GL keeps it.
			 * atlantis is one: it queues only the mouse, so after a
			 * resize its sharks went on drawing into the old 512x512
			 * corner of a bigger window and were cut off at its edge.
			 */
			hgl_set_viewport(0, g->w - 1, 0, g->h - 1);
		}
		return;
	case EnterNotify:
	case LeaveNotify:
		/* No window manager hands out focus here; the pointer does. */
		if (gid && g->queued[INPUTCHANGE & 1023] && e->xcrossing.detail != NotifyInferior)
			push(INPUTCHANGE, e->type == EnterNotify ? (short)gid : 0);
		return;
	case MotionNotify:
		g->mousex = e->xmotion.x_root;
		g->mousey = screen_height() - 1 - e->xmotion.y_root;
		if (g->queued[MOUSEX & 1023])
			push(MOUSEX, (short)g->mousex);
		if (g->queued[MOUSEY & 1023])
			push(MOUSEY, (short)g->mousey);
		return;
	case ButtonPress:
	case ButtonRelease:
		g->mousex = e->xbutton.x_root;
		g->mousey = screen_height() - 1 - e->xbutton.y_root;
		switch (e->xbutton.button) {
		case 1: dev = LEFTMOUSE; break;
		case 2: dev = MIDDLEMOUSE; break;
		case 3: dev = RIGHTMOUSE; break;
		default: return;
		}
		g->button[dev & 1023] = (e->type == ButtonPress);
		if (g->queued[dev & 1023])
			push_button(dev, e->type == ButtonPress);
		return;
	case KeyPress:
	case KeyRelease:
		/* Every key is a button device of its own, and KEYBD carries the
		 * character a press types. */
		ks = XLookupKeysym(&e->xkey, 0);
		dev = key_device(ks);
		if (dev) {
			g->button[dev & 1023] = (e->type == KeyPress);
			if (g->queued[dev & 1023])
				push_button(dev, e->type == KeyPress);
		}
		if (e->type == KeyPress && g->queued[KEYBD & 1023] &&
		    XLookupString(&e->xkey, buf, sizeof buf, NULL, NULL) > 0)
			push(KEYBD, (short)(unsigned char)buf[0]);
		return;
	default:
		return;
	}
}

/* An event read by someone else -- a popup menu, while it is up -- for the
 * program's queue, as the pump would have made it. */
void
hgl_iris_event(XEvent *e)
{
	xlate_event(e);
}

/* A cursor with nothing in it, made once. */
Cursor
hgl_blank_cursor(void)
{
	static Cursor c;
	static int made;
	Pixmap p;
	XColor black;
	Display *d = hgl_display();

	if (!made) {
		char none[8];
		memset(none, 0, sizeof none);
		memset(&black, 0, sizeof black);
		p = XCreateBitmapFromData(d, RootWindow(d, DefaultScreen(d)), none, 8, 8);
		c = XCreatePixmapCursor(d, p, p, &black, &black, 0, 0);
		XFreePixmap(d, p);
		made = 1;
	}
	return c;
}

/*
 * GLX_SGI_swap_control through the OpenGL shim, which exports the entry point
 * itself. Looking it up with glXGetProcAddressARB seemed tidier and was fatal:
 * the shim does not export *that*, so rld killed every program that loaded
 * this library the moment it touched the symbol -- blast and flight died
 * before main with "unresolvable symbol", nothing to do with what they use.
 */
void
hgl_iris_swapinterval(int n)
{
	glXSwapIntervalSGI(n);
}

/* ---- timers ----
 *
 * TIMER0..3 are devices like any other: queue one and an event arrives every
 * noise() ticks, a tick being one vertical retrace. Nothing in X produces
 * them, so the pump does -- and a program that blocks in qread waiting for
 * its next tick has to be woken by the clock, not by X traffic. clock is
 * exactly that program: it draws its face once, then waits for TIMER0 to
 * move the hands, and with no timers it sat on its first frame for ever.
 */
#define TICK (1.0 / 60.0)

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static int
timer_index(Device dev)
{
	return dev >= TIMER0 && dev <= TIMER3 ? (int)(dev - TIMER0) : -1;
}

static double
timer_period(int i)
{
	return (hgl_iris.timer_ticks[i] > 0 ? hgl_iris.timer_ticks[i] : 1) * TICK;
}

/* Post the timers that are due. One event however many ticks were missed:
 * a program that was busy wants to catch up once, not replay the backlog. */
static void
fire_timers(void)
{
	double t = now();
	int i;

	for (i = 0; i < 4; i++) {
		if (!hgl_iris.queued[(TIMER0 + i) & 1023] || hgl_iris.timer_due[i] > t)
			continue;
		push((short)(TIMER0 + i), 0);
		hgl_iris.timer_due[i] += timer_period(i);
		if (hgl_iris.timer_due[i] <= t)
			hgl_iris.timer_due[i] = t + timer_period(i);
	}
}

/* Seconds until the next timer, or -1 if none is running. */
static double
next_timer(void)
{
	double t = now(), best = -1;
	int i;

	for (i = 0; i < 4; i++) {
		double d;
		if (!hgl_iris.queued[(TIMER0 + i) & 1023])
			continue;
		d = hgl_iris.timer_due[i] - t;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best;
}

void
hgl_iris_pump(int block)
{
	XEvent e;
	Display *d = hgl_display();

	fire_timers();
	if (block && hgl_iris.qhead == hgl_iris.qtail && !XPending(d)) {
		double wait = next_timer();

		if (wait < 0) {
			XNextEvent(d, &e);
			xlate_event(&e);
		} else {
			/* Wait for X or the next tick, whichever comes first. */
			int fd = ConnectionNumber(d);
			fd_set r;
			struct timeval tv;

			FD_ZERO(&r);
			FD_SET(fd, &r);
			tv.tv_sec = (long)wait;
			tv.tv_usec = (long)((wait - tv.tv_sec) * 1e6);
			select(fd + 1, &r, NULL, NULL, &tv);
			fire_timers();
		}
	}
	while (XPending(d)) {
		XNextEvent(d, &e);
		xlate_event(&e);
	}
}

/* ---- windows ---- */

/*
 * winopen makes the window there and then, as IRIS GL does: every mode it
 * could be configured in draws through the same visual here, so nothing a
 * later RGBmode or doublebuffer says changes the X window.
 */
long
winopen(String name)
{
	long gid, prev = hgl_iris.gid;

	TRACE("winopen");
	if ((gid = new_slot()) < 0)
		return -1;
	/*
	 * REDRAW is queued whether the program asks for it or not: jot never
	 * does, handles it, and relies on it to repaint what another window
	 * covered. Without it, a closed window's pixels stayed in jot's until
	 * jot next drew there.
	 */
	hgl_iris.queued[REDRAW & 1023] = 1;
	(void)hgl_layer_suspend();
	save_current();
	/* The configuration a program set before its first window carries into
	 * it; a later window starts from IRIS GL's defaults. */
	if (prev > 0) {
		hgl_iris.want_rgb = hgl_iris.want_double = 0;
		hgl_iris.want_ms = 0;
		hgl_iris.want_zbuf = 1;
	} else if (!zbsize_given) {
		hgl_iris.want_zbuf = 1;
	}
	hgl_iris.front = 0;
	hgl_iris.blend = 0;
	hgl_iris.glx = 0;
	hgl_iris.opened = 0;
	hgl_iris.layers = NULL;
	hgl_iris.enables = 0;
	hgl_iris.index_win = 0;
	hgl_iris.gid = gid;
	strncpy(hgl_iris.title, name ? name : "", sizeof hgl_iris.title - 1);
	hgl_iris.title[sizeof hgl_iris.title - 1] = '\0';
	wins[gid].noport = cons.noport;
	open_window();
	save_current();
	return gid;
}

/* swinopen: a subwindow, which here is a child X window of its parent's. */
long
swinopen(long parent)
{
	long gid;
	Window pw;
	XSetWindowAttributes swa;
	Display *d;
	XVisualInfo *vi;

	TRACE("swinopen");
	if (parent <= 0 || parent >= HGL_MAXWIN || !wins[parent].used)
		return -1;
	(void)hgl_layer_suspend();
	save_current();
	pw = wins[parent].win;
	if ((gid = new_slot()) < 0)
		return -1;
	d = hgl_display();
	vi = gl_visual(d);
	memset(&swa, 0, sizeof swa);
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	    PointerMotionMask | StructureNotifyMask | EnterWindowMask | LeaveWindowMask;
	hgl_iris.gid = gid;
	hgl_iris.gldpy = d;
	hgl_iris.vi = vi;
	hgl_iris.glx = 0;
	hgl_iris.layers = NULL;
	hgl_iris.enables = 0;
	hgl_iris.index_win = 0;
	hgl_iris.front = 0;
	hgl_iris.blend = 0;
	hgl_iris.want_rgb = hgl_iris.want_double = hgl_iris.want_zbuf = hgl_iris.want_ms = 0;
	hgl_iris.title[0] = '\0';
	hgl_iris.w = hgl_iris.h = 100;
	hgl_iris.win = XCreateWindow(d, pw, 0, 0, 100, 100, 0, vi->depth, InputOutput,
	    vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XMapWindow(d, hgl_iris.win);
	hgl_iris.opened = 1;
	make_context();
	hgl_window_defaults();
	save_current();
	return gid;
}

static void
make_current_window(void)
{
	if (hgl_iris.ctx == NULL) {
		make_context();
		refresh_glx_size();
		hgl_window_defaults();
	} else {
		glXMakeCurrent(hgl_iris.gldpy, hgl_iris.win, hgl_iris.ctx);
		refresh_glx_size();
	}
	hgl_apply_colormask();
}

void
winset(long gid)
{
	int was;

	TRACE("winset");
	if (gid <= 0 || gid >= HGL_MAXWIN || !wins[gid].used || gid == hgl_iris.gid)
		return;
	/* What a single-buffered window drew is due on the screen before
	 * drawing moves elsewhere. */
	hgl_iris_present_if_single();
	was = hgl_layer_suspend();
	save_current();
	load_current(gid);
	make_current_window();
	hgl_layer_resume(was);
}

long winget(void) { return hgl_iris.gid; }

void
hgl_origin_known(void)
{
	if (hgl_iris.gid > 0 && hgl_iris.gid < HGL_MAXWIN)
		wins[hgl_iris.gid].origin_unknown = 0;
}

long winattach(void) { return hgl_iris.gid; }

void
winclose(long gid)
{
	HglWin *s;

	TRACE("winclose");
	if (gid <= 0 || gid >= HGL_MAXWIN || !wins[gid].used)
		return;
	save_current();
	s = &wins[gid];
	hgl_layers_free(s->layers, s->gldpy);
	s->layers = NULL;
	if (gid == hgl_iris.gid) {
		hgl_iris.layers = NULL;
		glFinish();
		glXMakeCurrent(s->gldpy, None, NULL);
		hgl_iris.gid = 0;
		hgl_iris.opened = 0;
		hgl_iris.win = None;
		hgl_iris.ctx = NULL;
	}
	/* The first context holds the shared lists; it stays while any does. */
	if (s->ctx != NULL && s->ctx != first_ctx)
		glXDestroyContext(s->gldpy, s->ctx);
	if (!s->glx && s->win != None) {
		XDestroyWindow(s->gldpy, s->win);
		XFlush(s->gldpy);
	}
	s->used = 0;
}

void
winconstraints(void)
{
	int x, y, w, h, how;
	Display *d;

	TRACE("winconstraints");
	if (!hgl_iris.opened || hgl_iris.glx) {
		cons_reset();
		return;
	}
	d = hgl_display();
	x = y = 0;
	w = hgl_iris.w;
	h = hgl_iris.h;
	how = cons_geometry(&x, &y, &w, &h);
	set_hints(hgl_iris.win, x, y, w, h);
	if (how == 2)
		XMoveResizeWindow(d, hgl_iris.win, x, y, (unsigned)w, (unsigned)h);
	else if (how == 1 && (w != hgl_iris.w || h != hgl_iris.h))
		XResizeWindow(d, hgl_iris.win, (unsigned)w, (unsigned)h);
	XFlush(d);
	cons_reset();
}

void RGBmode(void) { TRACE("RGBmode"); hgl_iris.want_rgb = 1; }
void cmode(void) { TRACE("cmode"); hgl_iris.want_rgb = 0; }
void doublebuffer(void) { TRACE("doublebuffer"); hgl_iris.want_double = 1; }
void singlebuffer(void) { TRACE("singlebuffer"); hgl_iris.want_double = 0; }
void mssize(long s, long z, long c) { hgl_iris.want_ms = (int)s; (void)z; (void)c; }
/*
 * zbsize(3G): IRIS GL's default is every z-buffer bit the machine has, so a
 * window gets a z-buffer unless the program asks for none. powerflip checks
 * getgconfig(GC_BITS_ZBUFFER) right after winopen, having called nothing, and
 * refused to run ("must have a z-buffer") while the default here was none.
 */
void zbsize(long n) { hgl_iris.want_zbuf = n > 0; zbsize_given = 1; }
void subpixel(Boolean b) { (void)b; /* always on here */ }

void
gconfig(void)
{
	TRACE("gconfig");
	hgl_iris_ensure();
}

void
reshapeviewport(void)
{
	TRACE("reshapeviewport");
	hgl_iris_ensure();
	refresh_glx_size();
	hgl_set_viewport(0, hgl_iris.w - 1, 0, hgl_iris.h - 1);
}

/*
 * Where a single-buffered program becomes visible: it has no swap of its own,
 * so the frame goes out whenever it stops drawing to wait for input or asks
 * for the pipeline to be flushed.
 */
int hgl_layer_drawn;

void
hgl_iris_present_if_single(void)
{
	int was;

	if (!hgl_iris.opened || !hgl_iris.ctx)
		return;
	/* The layers that are windows of their own, then the normal planes. */
	hgl_layers_present();
	if (!hgl_iris.want_double || hgl_iris.front || hgl_layer_drawn) {
		was = hgl_layer_suspend();
		glXSwapBuffers(hgl_iris.gldpy, hgl_iris.win);
		hgl_layer_resume(was);
		hgl_layer_drawn = 0;
	}
}

void
gflush(void)
{
	hgl_iris_ensure();
	glFlush();
	hgl_iris_present_if_single();
}

void
finish(void)
{
	hgl_iris_ensure();
	glFinish();
	hgl_iris_present_if_single();
}

void
swapbuffers(void)
{
	int was;

	TRACE("swapbuffers");
	hgl_iris_ensure();
	/* The layers are single-buffered: what they hold is due now too. */
	hgl_layers_present();
	was = hgl_layer_suspend();
	glXSwapBuffers(hgl_iris.gldpy, hgl_iris.win);
	hgl_layer_resume(was);
	hgl_layer_drawn = 0;
}

long
getgconfig(long buffer)
{
	TRACE("getgconfig");
	switch (buffer) {
	case GC_BITS_ZBUFFER:  return hgl_iris.want_zbuf ? 24 : 0;
	case GC_BITS_STENCIL:  return 8;	/* the host's framebuffer: depth 24, stencil 8 */
	case GC_BITS_RED:
	case GC_BITS_GREEN:
	case GC_BITS_BLUE:
	case GC_BITS_ALPHA:    return 8;
	case GC_ZMIN:          return 0;
	case GC_ZMAX:          return 0x7fffff;
	case GC_MS_SAMPLES:    return hgl_iris.want_ms;
	default:               return 0;
	}
}

long
getgdesc(long inquiry)
{
	Display *d;
	hgl_irisgl_tracef("getgdesc %ld", inquiry);

	switch (inquiry) {
	case GD_XPMAX:
	case GD_YPMAX:
		d = hgl_display();
		return inquiry == GD_XPMAX
		    ? WidthOfScreen(DefaultScreenOfDisplay(d))
		    : HeightOfScreen(DefaultScreenOfDisplay(d));
	case GD_XMMAX:
	case GD_YMMAX:
		d = hgl_display();
		return inquiry == GD_XMMAX
		    ? WidthMMOfScreen(DefaultScreenOfDisplay(d))
		    : HeightMMOfScreen(DefaultScreenOfDisplay(d));
	case GD_BITS_NORM_SNG_RED:
	case GD_BITS_NORM_SNG_GREEN:
	case GD_BITS_NORM_SNG_BLUE:
	case GD_BITS_NORM_DBL_RED:
	case GD_BITS_NORM_DBL_GREEN:
	case GD_BITS_NORM_DBL_BLUE:
		return 8;
	case GD_BITS_NORM_SNG_CMODE:
	case GD_BITS_NORM_DBL_CMODE:
		return 12;
	case GD_BITS_NORM_ZBUFFER:
		return 24;
	case GD_BITS_STENCIL:
		return 8;	/* the host's framebuffer: depth 24, stencil 8 */
	case GD_BITS_OVER_SNG_CMODE:
		return hgl_overlay_bits(0);
	case GD_BITS_PUP_SNG_CMODE:
		return hgl_overlay_bits(1);
	case GD_ZMIN:
		return 0;
	case GD_ZMAX:
		return 0x7fffff;
	case GD_TEXTURE:
	case GD_LIGHTING_TWOSIDE:
	case GD_BLEND:
	case GD_LINESMOOTH_RGB:
	case GD_LOGICOP:
	case GD_NSCRNS:
	case GD_SCRBOX:
	case GD_FOGVERTEX:
	case GD_FOGPIXEL:
	case GD_NURBS_ORDER:
		return 1;
	case GD_WSYS:
		return GD_WSYS_4S;
	case GD_SCRNTYPE:
		return GD_SCRNTYPE_WM;
	case GD_NMMAPS:
		return 1;
	case GD_NVERTEX_POLY:
		return GD_NOLIMIT;
	case GD_MULTISAMPLE:
		return 0;   /* said honestly: nothing here resolves samples yet */
	default:
		return 0;
	}
}

long
gversion(char v[12])
{
	/* Programs switch on the first letters of this. "GL4DLG" is what a
	 * high-end machine answers, and what a demo expects to see. */
	strncpy(v, "GL4DLG-iris", 12);
	return 1;
}

/* ---- the event queue ---- */

void
qdevice(Device dev)
{
	char what[32];

	sprintf(what, "qdevice %d", (int)dev);
	TRACE(what);
	hgl_iris.queued[dev & 1023] = 1;
	if (timer_index(dev) >= 0)
		hgl_iris.timer_due[timer_index(dev)] = now() + timer_period(timer_index(dev));
	/*
	 * A newly opened IRIS GL window always owes its program a REDRAW, and
	 * programs wait for one before drawing anything. Ours arrives as an X
	 * Expose, which can easily land *before* the program asks for the
	 * device -- and an event nobody had asked for yet was dropped. atlantis
	 * then sat in qread for ever with a black window: the redraw it was
	 * waiting for had already been and gone.
	 */
	if (dev == REDRAW && hgl_iris.gid > 0)
		push(REDRAW, (short)hgl_iris.gid);
}
void unqdevice(Device dev) { hgl_iris.queued[dev & 1023] = 0; }

/* qenter(3G): put an event on the queue as though a device had produced it. */
void qenter(Device dev, short val) { push((short)dev, val); }

long
qread(short *val)
{
	short dev;
	TRACE("qread");

	hgl_iris_present_if_single();
	for (;;) {
		if (hgl_iris.qhead != hgl_iris.qtail) {
			dev = hgl_iris.q[hgl_iris.qhead].dev;
			if (val)
				*val = hgl_iris.q[hgl_iris.qhead].val;
			hgl_irisgl_tracef("  -> %d %d", (int)dev, (int)hgl_iris.q[hgl_iris.qhead].val);
			hgl_iris.qhead = (hgl_iris.qhead + 1) % 256;
			return dev;
		}
		hgl_iris_pump(1);
	}
}

long
qtest(void)
{
	hgl_iris_present_if_single();
	hgl_iris_pump(0);
	if (hgl_iris.qhead == hgl_iris.qtail)
		return 0;
	hgl_irisgl_tracef("qtest -> %d", (int)hgl_iris.q[hgl_iris.qhead].dev);
	return hgl_iris.q[hgl_iris.qhead].dev;
}

/* isqueued(3G): whether qdevice asked for the device. */
Boolean isqueued(Device dev) { return (Boolean)hgl_iris.queued[dev & 1023]; }

long
getbutton(Device dev)
{
	/* A program polling the mouse -- jot, while a button is held on its
	 * menu bar -- expects to see what it drew in the popup planes as it
	 * goes: they are on the screen as soon as drawn. Only new drawing is
	 * sent (see hgl_layers_present). */
	if (hgl_iris.opened && hgl_iris.ctx)
		hgl_layers_present();
	hgl_iris_pump(0);
	hgl_irisgl_tracef("getbutton %d -> %d", (int)dev, (int)hgl_iris.button[dev & 1023]);
	return hgl_iris.button[dev & 1023];
}

static short
tied_value(short v)
{
	if (v == MOUSEX)
		return (short)hgl_iris.mousex;
	if (v == MOUSEY)
		return (short)hgl_iris.mousey;
	return 0;
}

long
getvaluator(Device dev)
{
	/* Polled with getbutton: see there. */
	if (hgl_iris.opened && hgl_iris.ctx)
		hgl_layers_present();
	hgl_iris_pump(0);
	hgl_irisgl_tracef("getvaluator %d (mouse %d %d)", (int)dev, (int)hgl_iris.mousex, (int)hgl_iris.mousey);
	if (dev == MOUSEX)
		return hgl_iris.mousex;
	if (dev == MOUSEY)
		return hgl_iris.mousey;
	return 0;
}

/* ---- the rest of the window, cursor and menu set ----
 *
 * Found the same way as the drawing ones: by running the demos and reading
 * what the generated stubs said they wanted.
 */

void
getsize(long *w, long *h)
{
	hgl_iris_ensure();
	refresh_glx_size();
	hgl_irisgl_tracef("getsize -> %d %d", hgl_iris.w, hgl_iris.h);
	if (w)
		*w = hgl_iris.w;
	if (h)
		*h = hgl_iris.h;
}

void
getorigin(long *x, long *y)
{
	int wx = 0, wy = 0;
	Window child;
	Display *d;

	hgl_iris_ensure();
	d = hgl_display();
	refresh_glx_size();
	/*
	 * A window opened with no size or position is where the user sweeps it
	 * out, and SGI's library reports its origin as 0,0 until the server
	 * says otherwise (a ConfigureNotify) -- even though, with no window
	 * manager, it is on the screen at 500,500. sysmeter moves its window to
	 * the origin it reads, so this decides where sysmeter ends up.
	 */
	if (!hgl_iris.glx && hgl_iris.gid > 0 && wins[hgl_iris.gid].origin_unknown) {
		hgl_irisgl_tracef("getorigin -> 0 0 (not known yet)");
		if (x)
			*x = 0;
		if (y)
			*y = 0;
		return;
	}
	/* The window's own coordinates say nothing about where it ended up:
	 * a window manager reparents it. Ask the server where it is. */
	if (XTranslateCoordinates(d, hgl_iris.win, RootWindow(d, DefaultScreen(d)),
	    0, 0, &wx, &wy, &child)) {
		hgl_irisgl_tracef("getorigin -> %d %d", wx, screen_height() - wy - hgl_iris.h);
		if (x)
			*x = wx;
		if (y)
			*y = screen_height() - wy - hgl_iris.h;
		return;
	}
	if (x)
		*x = 0;
	if (y)
		*y = 0;
}

void
viewport(Screencoord l, Screencoord r, Screencoord b, Screencoord t)
{
	hgl_iris_ensure();
	hgl_set_viewport(l, r, b, t);
}

void
scrmask(Screencoord l, Screencoord r, Screencoord b, Screencoord t)
{
	hgl_irisgl_tracef("scrmask %d %d %d %d", (int)l, (int)r, (int)b, (int)t);
	TRACE("scrmask");
	hgl_iris_ensure();
	hgl_iris.mask_l = l;
	hgl_iris.mask_r = r;
	hgl_iris.mask_b = b;
	hgl_iris.mask_t = t;
	hgl_apply_scrmask();
}

void
getscrmask(Screencoord *l, Screencoord *r, Screencoord *b, Screencoord *t)
{
	hgl_iris_ensure();
	*l = (Screencoord)hgl_iris.mask_l;
	*r = (Screencoord)hgl_iris.mask_r;
	*b = (Screencoord)hgl_iris.mask_b;
	*t = (Screencoord)hgl_iris.mask_t;
}

/*
 * glcompat asks for one of a handful of old behaviours. None of them change
 * anything here -- there is no colour map to trigger, no ancient cursor
 * semantics to restore -- so they are accepted and ignored rather than
 * refused, which is what a program checks for.
 */
void glcompat(long mode, long value) { (void)mode; (void)value; }

/* Input tuning with nothing underneath it: X delivers what it delivers. */
void
noise(Device dev, short delta)
{
	int i = timer_index(dev);
	char what[40];

	sprintf(what, "noise %d %d", (int)dev, (int)delta);
	TRACE(what);
	/* For a valuator this is a motion threshold, and X has none to set. */
	if (i >= 0) {
		hgl_iris.timer_ticks[i] = delta;
		hgl_iris.timer_due[i] = now() + timer_period(i);
	}
}

void
tie(Device b, Device v1, Device v2)
{
	tied[b & 1023][0] = (short)v1;
	tied[b & 1023][1] = (short)v2;
}
void setvaluator(Device dev, short init, short min, short max)
{
	(void)dev; (void)init; (void)min; (void)max;
}

/* The cursor is the X server's; a program that defines its own gets the
 * default, which is better than no pointer at all. */
void curorigin(short n, short x, short y) { (void)n; (void)x; (void)y; }
void defcursor(short n, const unsigned short bits[]) { (void)n; (void)bits; }
void setcursor(short n, Colorindex c, Colorindex w) { (void)n; (void)c; (void)w; }
void curstype(long t) { (void)t; }

void
cursoff(void)
{
	hgl_iris_ensure();
	/* A blank cursor over our window, so a program that hides the pointer
	 * to draw its own is not left with two. */
	XDefineCursor(hgl_display(), hgl_iris.win, hgl_blank_cursor());
}

void
curson(void)
{
	hgl_iris_ensure();
	XUndefineCursor(hgl_display(), hgl_iris.win);
}

/* Overlay and underlay planes: this framebuffer has none, and a program that
 * asks for them draws into the ordinary one instead. */
void overlay(long planes) { (void)planes; }
void underlay(long planes) { (void)planes; }

void
swapinterval(short n)
{
	hgl_iris_ensure();
	/* GLX_SGI_swap_control, which the OpenGL shim already has. */
	hgl_iris_swapinterval(n);
}

/* ringbell(3G): the keyboard bell, which on X is the server's. */
void
ringbell(void)
{
	XBell(hgl_display(), 0);
	XFlush(hgl_display());
}

/* ---- the GLX mixed model ----
 *
 * An X toolkit program (libSgm's GL widgets, showcase, iconsmith) makes its
 * own X windows and draws into them with IRIS GL: GLXgetconfig says which
 * visual and colormap a window needs for the modes asked, the program creates
 * the window, puts its id into the configuration and GLXlink's it; GLXwinset
 * then makes it the current window. The layout of the answer -- every mode for
 * NORMAL, OVERLAY, UNDERLAY and POPUP in that order, then NORMAL's
 * multisample, stereo and RGB-size entries -- is SGI's, as its libgl.so
 * answers on Xsgi.
 *
 * The overlay and popup layers get the X server's overlay visual and a
 * colormap of their own with every cell allocated, so that mapcolor can set
 * any index; GLXlink makes a window in that visual an index window. The
 * underlay, and every layer on a server without an overlay visual, get the
 * normal visual and an invisible window (see link_window).
 */

/* The colormaps made here for layers and colour-index windows, and the
 * cells each holds. */
#define HGL_LAYER_CMAPS 64
#define HGL_INDEX_CELLS 4096
static struct {
	Display *d;
	Colormap cmap;
	unsigned char owned[HGL_INDEX_CELLS];
} layer_cmaps[HGL_LAYER_CMAPS];

/*
 * A colormap for an overlay (every cell but the transparent one, which the
 * server keeps: AllocAll is refused) or for a colour-index window (AllocAll).
 * Until the program maps them, its colours are IRIS GL's default map's.
 * A new one for every answer, as for the normal visual: a program's windows
 * do not share one unless it gives them the same.
 */
static Colormap
index_colormap(Display *d, int screen, XVisualInfo *vi, int overlay)
{
	static unsigned long px[HGL_INDEX_CELLS];
	unsigned char rgb[3];
	XColor c;
	Colormap cmap;
	int cells = vi->colormap_size < HGL_INDEX_CELLS ? vi->colormap_size : HGL_INDEX_CELLS;
	int n, i, k;

	cmap = XCreateColormap(d, RootWindow(d, screen), vi->visual, overlay ? AllocNone : AllocAll);
	for (k = 0; k < HGL_LAYER_CMAPS && layer_cmaps[k].cmap != None; k++)
		;
	if (k == HGL_LAYER_CMAPS)
		return cmap;
	layer_cmaps[k].d = d;
	layer_cmaps[k].cmap = cmap;
	memset(layer_cmaps[k].owned, 0, sizeof layer_cmaps[k].owned);
	if (overlay) {
		for (n = cells - 1; n > 0; n--)
			if (XAllocColorCells(d, cmap, False, NULL, 0, px, (unsigned)n))
				break;
	} else {
		for (n = 0; n < cells; n++)
			px[n] = (unsigned long)n;
	}
	c.flags = DoRed | DoGreen | DoBlue;
	for (i = 0; i < n; i++) {
		if (px[i] >= (unsigned long)cells)
			continue;
		layer_cmaps[k].owned[px[i]] = 1;
		hgl_cmap_rgb(px[i], rgb);
		c.pixel = px[i];
		c.red = (unsigned short)(rgb[0] * 257);
		c.green = (unsigned short)(rgb[1] * 257);
		c.blue = (unsigned short)(rgb[2] * 257);
		XStoreColor(d, cmap, &c);
	}
	return cmap;
}

/* The current window is an index window: its map entry i is (r, g, b). */
int
hgl_index_mapcolor(Colorindex i, short r, short g, short b)
{
	XColor c;
	int k;

	if (!hgl_iris.index_win)
		return 0;
	i &= HGL_INDEX_CELLS - 1;
	for (k = 0; k < HGL_LAYER_CMAPS; k++)
		if (layer_cmaps[k].cmap == hgl_iris.index_cmap && layer_cmaps[k].owned[i]) {
			c.pixel = i;
			c.red = (unsigned short)(r * 257);
			c.green = (unsigned short)(g * 257);
			c.blue = (unsigned short)(b * 257);
			c.flags = DoRed | DoGreen | DoBlue;
			XStoreColor(layer_cmaps[k].d, layer_cmaps[k].cmap, &c);
			break;
		}
	return 1;
}

int
hgl_index_getmcolor(Colorindex i, short *r, short *g, short *b)
{
	XColor c;

	if (!hgl_iris.index_win)
		return 0;
	c.pixel = i & (HGL_INDEX_CELLS - 1);
	XQueryColor(hgl_iris.gldpy, hgl_iris.index_cmap, &c);
	*r = (short)(c.red >> 8);
	*g = (short)(c.green >> 8);
	*b = (short)(c.blue >> 8);
	return 1;
}

/* The screen's 12-bit PseudoColor visual -- IMPACT's colour-index one -- or
 * NULL. IRIS_IRISGL_CMODE_RGB=1 keeps GLX colour-index windows in RGB, drawn
 * through the library's own colour map, as before. */
static XVisualInfo *
cmode_visual(Display *d, int screen)
{
	XVisualInfo tmpl;
	int n;

	if (getenv("IRIS_IRISGL_CMODE_RGB") != NULL)
		return NULL;
	tmpl.screen = screen;
	tmpl.depth = 12;
	tmpl.class = PseudoColor;
	return XGetVisualInfo(d, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
}

/* The overlay visual on display d, for a layer that has it, or NULL. */
static XVisualInfo *
layer_visual(Display *d, int buffer)
{
	XVisualInfo tmpl;
	int n;

	if (buffer == GLX_UNDERLAY || hgl_overlay_visual() == NULL)
		return NULL;
	tmpl.visualid = hgl_overlay_visual()->visualid;
	return XGetVisualInfo(d, VisualIDMask, &tmpl, &n);
}
static const int glx_modes[] = {
	GLX_DOUBLE, GLX_RGB, GLX_BUFSIZE, GLX_ACSIZE, GLX_ZSIZE, GLX_STENSIZE,
	GLX_VISUAL, GLX_COLORMAP, GLX_WINDOW
};
static const int glx_buffers[] = { GLX_NORMAL, GLX_OVERLAY, GLX_UNDERLAY, GLX_POPUP };

static int
glx_want(GLXconfig *desc, int buffer, int mode, int dflt)
{
	for (; desc != NULL && desc->buffer; desc++)
		if (desc->buffer == buffer && desc->mode == mode)
			return desc->arg;
	return dflt;
}

GLXconfig *
GLXgetconfig(void *dpyp, long screen, GLXconfig *desc)
{
	Display *d = (Display *)dpyp;
	GLXconfig *out, *o;
	XVisualInfo *vi, *lvi, *cvi;
	int b, m, rgb, dbl, z, st;

	TRACE("GLXgetconfig");
	if (d == NULL)
		return NULL;
	xsync_if_asked(d);
	for (o = desc; o != NULL && o->buffer; o++)
		hgl_irisgl_tracef("  want buffer %x mode %x arg %d", o->buffer, o->mode, o->arg);
	o = out = (GLXconfig *)calloc(4 * 9 + 5 + 1, sizeof *out);
	if (out == NULL)
		return NULL;
	vi = gl_visual(d);
	rgb = glx_want(desc, GLX_NORMAL, GLX_RGB, 0) != 0;
	/* Colour-index normal planes are IMPACT's 12-bit colour-index visual,
	 * whose colormap the program may fill itself (bz does, with
	 * XStoreColors); GLXlink makes such a window an index window. */
	cvi = rgb ? NULL : cmode_visual(d, screen);
	dbl = glx_want(desc, GLX_NORMAL, GLX_DOUBLE, 0) != 0;
	z = glx_want(desc, GLX_NORMAL, GLX_ZSIZE, 0);
	st = glx_want(desc, GLX_NORMAL, GLX_STENSIZE, 0);
	for (b = 0, lvi = NULL; b < 4; b++) {
		if (lvi != NULL)
			XFree(lvi);
		lvi = NULL;
		if (b != 0 && glx_want(desc, glx_buffers[b], GLX_BUFSIZE, 0) > 0)
			lvi = layer_visual(d, glx_buffers[b]);
		for (m = 0; m < 9; m++, o++) {
			o->buffer = glx_buffers[b];
			o->mode = glx_modes[m];
			o->arg = 0;
			if (b != 0) {
				/* A layer the program asked for gets a visual: see
				 * GLXlink for what becomes of its window. */
				if (glx_want(desc, glx_buffers[b], GLX_BUFSIZE, 0) <= 0)
					continue;
				switch (glx_modes[m]) {
				case GLX_BUFSIZE: o->arg = glx_buffers[b] == GLX_POPUP ? 2 : 8; break;
				case GLX_VISUAL:  o->arg = (int)(lvi != NULL ? lvi->visualid : vi->visualid); break;
				case GLX_COLORMAP:
					o->arg = lvi != NULL ? (int)index_colormap(d, screen, lvi, 1)
					    : (int)XCreateColormap(d, RootWindow(d, screen), vi->visual, AllocNone);
					break;
				default: break;
				}
				continue;
			}
			switch (glx_modes[m]) {
			case GLX_DOUBLE:   o->arg = dbl; break;
			case GLX_RGB:      o->arg = rgb; break;
			/* colour-map mode is a 12-bit index here, as on SGI's 24-bit boards */
			case GLX_BUFSIZE:  o->arg = rgb ? 24 : 12; break;
			case GLX_ZSIZE:    o->arg = z > 0 && z < 24 ? z : 24; break;
			case GLX_STENSIZE: o->arg = st > 0 ? (st < 8 ? st : 8) : 0; break;
			case GLX_VISUAL:   o->arg = (int)(cvi != NULL ? cvi->visualid : vi->visualid); break;
			case GLX_COLORMAP:
				o->arg = cvi != NULL ? (int)index_colormap(d, screen, cvi, 0)
				    : (int)XCreateColormap(d, RootWindow(d, screen), vi->visual, AllocNone);
				break;
			default: break;
			}
		}
	}
	if (lvi != NULL)
		XFree(lvi);
	if (cvi != NULL)
		XFree(cvi);
	(void)screen;
	o->buffer = GLX_NORMAL; o->mode = GLX_MSSAMPLE; o++;
	o->buffer = GLX_NORMAL; o->mode = GLX_MSZSIZE; o++;
	o->buffer = GLX_NORMAL; o->mode = GLX_MSSSIZE; o++;
	o->buffer = GLX_NORMAL; o->mode = GLX_STEREOBUF; o++;
	o->buffer = GLX_NORMAL; o->mode = GLX_RGBSIZE; o++;
	o->buffer = 0;
	for (o = out; o->buffer; o++)
		if (o->arg)
			hgl_irisgl_tracef("  answer buffer %x mode %x arg %d (%#x)", o->buffer, o->mode, o->arg, o->arg);
	return out;
}

/* One linked window: the normal framebuffer's, or a layer's. */
static long
link_window(Display *d, GLXconfig *conf, int buffer, Window w)
{
	long gid, i;
	XWindowAttributes wa;
	int have;

	/* Linking a window again replaces its configuration. */
	for (i = 1; i < HGL_MAXWIN; i++)
		if (wins[i].used && wins[i].glx && wins[i].win == w)
			GLXunlink(d, w);
	if ((gid = new_slot()) < 0)
		return GLWS_NOCONTEXT;
	hgl_irisgl_tracef("  link buffer %x window %lx as %ld", buffer, (unsigned long)w, gid);
	wins[gid].glx = 1;
	wins[gid].opened = 1;
	wins[gid].win = w;
	wins[gid].gldpy = d;
	wins[gid].want_rgb = glx_want(conf, buffer, GLX_RGB, 0) != 0;
	wins[gid].want_double = glx_want(conf, buffer, GLX_DOUBLE, 0) != 0;
	wins[gid].want_zbuf = glx_want(conf, buffer, GLX_ZSIZE, 0) > 0;
	have = XGetWindowAttributes(d, w, &wa);
	if (have) {
		wins[gid].w = wa.width;
		wins[gid].h = wa.height;
	}
	if (have && buffer != GLX_UNDERLAY && (wa.visual->class == PseudoColor || wa.visual->class == StaticColor ||
	    wa.visual->class == GrayScale)) {
		/*
		 * A window in a colour-map visual: an overlay, whose pixels the
		 * board shows over the picture, or the normal planes of a
		 * colour-index program. It is drawn in colour indices by a
		 * context of its own (make_context), in the colours of its
		 * colormap -- the one GLXgetconfig made, when the program used it.
		 */
		wins[gid].index_win = 1;
		wins[gid].index_cmap = wa.colormap;
	} else if (buffer != GLX_NORMAL) {
		/*
		 * There are no overlay planes to show a layer in, and a window of
		 * ordinary pixels stacked over the picture would hide it. So a
		 * layer's window is given an empty shape -- nothing of it is
		 * seen, and the pointer passes through to the window below --
		 * and what is drawn into it is masked off (hgl_apply_colormask).
		 * Showcase refuses to start without an overlay.
		 */
		int ev, err;

		wins[gid].layer = 1;
		if (XShapeQueryExtension(d, &ev, &err))
			XShapeCombineRectangles(d, w, ShapeBounding, 0, 0, NULL, 0, ShapeSet, Unsorted);
	}
	return GLWS_NOERROR;
}

long
GLXlink(void *dpyp, GLXconfig *conf)
{
	Display *d = (Display *)dpyp;
	GLXconfig *c;
	long r, linked = 0;

	TRACE("GLXlink");
	if (d == NULL)
		return GLWS_NODISPLAY;
	for (c = conf; c != NULL && c->buffer; c++) {
		if (c->mode != GLX_WINDOW || c->arg == 0)
			continue;
		if ((r = link_window(d, conf, c->buffer, (Window)c->arg)) != GLWS_NOERROR)
			return r;
		linked++;
	}
	return linked ? GLWS_NOERROR : GLWS_NOWINDOW;
}

long
GLXwinset(void *dpyp, unsigned long win)
{
	long gid, i;

	hgl_irisgl_tracef("GLXwinset %lx", win);
	(void)dpyp;
	for (gid = 0, i = 1; i < HGL_MAXWIN; i++)
		if (wins[i].used && wins[i].glx && wins[i].win == (Window)win)
			gid = i;
	if (gid == 0)
		return GLWS_NOWINDOW;
	if (gid != hgl_iris.gid) {
		hgl_iris_present_if_single();
		save_current();
		load_current(gid);
	}
	make_current_window();
	return GLWS_NOERROR;
}

long
GLXunlink(void *dpyp, unsigned long win)
{
	long i;

	TRACE("GLXunlink");
	(void)dpyp;
	for (i = 1; i < HGL_MAXWIN; i++)
		if (wins[i].used && wins[i].glx && wins[i].win == (Window)win) {
			winclose(i);
			return GLWS_NOERROR;
		}
	return GLWS_NOWINDOW;
}

/* Popup menus are in irisgl_pup.c. */
