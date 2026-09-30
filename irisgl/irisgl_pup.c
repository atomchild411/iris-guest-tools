/*
 * IRIS GL popup menus: defpup, newpup, addtopup, setpup, freepup, dopup.
 *
 * A menu is a list of entries separated by '|', each its text and, anywhere
 * in it, these (pup(3G)):
 *
 *   %t   the entry is the menu's title, not an entry
 *   %xN  choosing the entry returns N (else its position, from 1)
 *   %l   a line under the entry
 *   %m   the entry opens a submenu: the next argument is its menu
 *   %f   choosing the entry calls a function with its value, and returns
 *        what that returns: the next argument
 *   %F   (usually on the title) every choice in the menu is passed through
 *        this function too: the next argument
 *   %n   the entry is not passed through the menu's %F function
 *
 * The functions and submenus are taken from the arguments in the order their
 * %f, %F and %m appear. setpup greys an entry (PUP_GREY: it cannot be
 * chosen) or gives it a check box, empty (PUP_BOX) or ticked (PUP_CHECK);
 * its entries count from 1, the title not counted.
 *
 * dopup shows the menu at the pointer and returns the value chosen, or -1.
 * With the button still down, moving onto an entry and releasing chooses
 * it; a release before the pointer has moved -- a click -- leaves the menu
 * up until the next click, which chooses or (outside it) dismisses. Click
 * and hold are told apart by movement, not time: under emulation a click's
 * release can arrive a second after the menu opened.
 *
 * The menu is drawn with X in the server's overlay visual, as 4Dwm's menus
 * are -- IRIS GL's popup menus are in the popup planes -- so the windows
 * under it keep their pixels. Events for other windows that arrive while it
 * is up go to the program's queue as usual, and so does the release that
 * ended it.
 */
#include "irisgl_shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define MAXPUP 128
#define MAXITEMS 256
#define PUPLINE 128
#define LEVELS 8

typedef long (*PupFn)(long);

struct pup_item {
	char text[PUPLINE];
	long value;
	int line;                /* %l */
	long sub;                /* %m: a submenu, or 0 */
	PupFn fn;                /* %f */
	int no_menu_fn;          /* %n */
	unsigned long mode;      /* setpup: PUP_GREY, PUP_BOX, PUP_CHECK */
};

static struct {
	int used;
	char title[PUPLINE];
	PupFn fn;                /* %F */
	int n, cap;
	struct pup_item *item;   /* grown as entries are added */
} pups[MAXPUP];

/* ---- building menus ---- */

/* Parse `s` into menu p, taking functions and submenus from `ap`. */
static void
parse(long p, const char *s, va_list ap)
{
	char entry[PUPLINE * 2];
	const char *e;
	size_t len;

	while (s != NULL && *s) {
		struct pup_item it;
		int title = 0, has_value = 0, k = 0;
		const char *c;

		e = strchr(s, '|');
		len = e ? (size_t)(e - s) : strlen(s);
		if (len >= sizeof entry)
			len = sizeof entry - 1;
		memcpy(entry, s, len);
		entry[len] = '\0';
		s = e ? e + 1 : NULL;

		memset(&it, 0, sizeof it);
		for (c = entry; *c; c++) {
			if (*c != '%' || c[1] == '\0') {
				if (k < PUPLINE - 1)
					it.text[k++] = *c;
				continue;
			}
			switch (*++c) {
			case 't': title = 1; break;
			case 'l': it.line = 1; break;
			case 'n': it.no_menu_fn = 1; break;
			case 'm': it.sub = va_arg(ap, long); break;
			case 'f': it.fn = va_arg(ap, PupFn); break;
			case 'F': pups[p].fn = va_arg(ap, PupFn); break;
			case 'x':
				it.value = strtol(c + 1, (char **)&c, 10);
				c--;
				has_value = 1;
				break;
			case '%':
				if (k < PUPLINE - 1)
					it.text[k++] = '%';
				break;
			default:
				break;
			}
		}
		/* Trailing blanks are the separator's, not the entry's. */
		while (k > 0 && it.text[k - 1] == ' ')
			k--;
		it.text[k] = '\0';
		if (title) {
			strcpy(pups[p].title, it.text);
			continue;
		}
		if (pups[p].n >= pups[p].cap) {
			int cap = pups[p].cap ? pups[p].cap * 2 : 8;
			struct pup_item *grown;

			if (cap > MAXITEMS ||
			    (grown = realloc(pups[p].item, (size_t)cap * sizeof *grown)) == NULL)
				continue;
			pups[p].item = grown;
			pups[p].cap = cap;
		}
		if (!has_value)
			it.value = pups[p].n + 1;
		pups[p].item[pups[p].n++] = it;
	}
}

static long
new_menu(void)
{
	long i;

	for (i = 1; i < MAXPUP; i++)
		if (!pups[i].used) {
			free(pups[i].item);
			memset(&pups[i], 0, sizeof pups[i]);
			pups[i].used = 1;
			return i;
		}
	return 0;
}

static int
valid(long p)
{
	return p > 0 && p < MAXPUP && pups[p].used;
}

long
defpup(String s, ...)
{
	va_list ap;
	long p;

	TRACE("defpup");
	if ((p = new_menu()) == 0)
		return -1;
	va_start(ap, s);
	parse(p, s, ap);
	va_end(ap);
	return p;
}

long
newpup(void)
{
	TRACE("newpup");
	return new_menu();
}

void
addtopup(long p, String s, ...)
{
	va_list ap;

	TRACE("addtopup");
	if (!valid(p))
		return;
	va_start(ap, s);
	parse(p, s, ap);
	va_end(ap);
}

void
setpup(long p, long entry, unsigned long mode)
{
	if (valid(p) && entry >= 1 && entry <= pups[p].n)
		pups[p].item[entry - 1].mode = mode;
}

void
freepup(long p)
{
	if (valid(p))
		pups[p].used = 0;
}

/* ---- showing them ---- */

/* The colours a menu is drawn in. */
enum { C_BG, C_TITLE, C_TEXT, C_GREY, C_HI, C_HITEXT, C_LIGHT, C_DARK, NCOL };
static const unsigned short rgb[NCOL][3] = {
	{ 0xb4, 0xb4, 0xb4 },    /* background */
	{ 0x8c, 0x8c, 0x8c },    /* title */
	{ 0x00, 0x00, 0x00 },    /* text */
	{ 0x70, 0x70, 0x70 },    /* greyed text */
	{ 0x5a, 0x5a, 0x82 },    /* the entry under the pointer */
	{ 0xff, 0xff, 0xff },    /* ... its text */
	{ 0xe6, 0xe6, 0xe6 },    /* bevel, lit */
	{ 0x50, 0x50, 0x50 },    /* bevel, shaded; separators */
};

static struct {
	int ready;
	Visual *visual;
	int depth;
	Colormap cmap;
	int own_cmap;
	unsigned long px[NCOL];
	XFontStruct *font;
} look;

/* The server's overlay visual with a transparent pixel (SERVER_OVERLAY_VISUALS:
 * layer 1), the one 4Dwm's own menus use, or NULL. */
static Visual *
popup_visual(Display *d, int *depth)
{
	Atom prop, type;
	int format, n;
	unsigned long count, after, i;
	unsigned char *data = NULL;
	long *v;
	XVisualInfo tmpl, *vi;
	Visual *found = NULL;

	if (getenv("IRIS_IRISGL_PUP_NORMAL") != NULL)
		return NULL;
	if ((prop = XInternAtom(d, "SERVER_OVERLAY_VISUALS", True)) == None)
		return NULL;
	if (XGetWindowProperty(d, DefaultRootWindow(d), prop, 0, 64, False, AnyPropertyType,
	    &type, &format, &count, &after, &data) != Success || data == NULL)
		return NULL;
	v = (long *)data;
	for (i = 0; format == 32 && i + 4 <= count && found == NULL; i += 4) {
		if (v[i + 1] == 0 || v[i + 3] != 1)
			continue;
		tmpl.visualid = (VisualID)v[i];
		if ((vi = XGetVisualInfo(d, VisualIDMask, &tmpl, &n)) != NULL) {
			found = vi->visual;
			*depth = vi->depth;
			XFree(vi);
		}
	}
	XFree(data);
	return found;
}

static void
look_init(Display *d)
{
	int i;
	XColor c;

	if (look.ready)
		return;
	look.ready = 1;
	look.visual = popup_visual(d, &look.depth);
	if (look.visual != NULL) {
		look.cmap = XCreateColormap(d, DefaultRootWindow(d), look.visual, AllocNone);
		look.own_cmap = 1;
		/* Pixel 0 is taken and never drawn with: it is the overlay
		 * planes' transparent pixel on some boards whatever the visual
		 * says (the emulated IMPACT's among them), and a menu drawn in it
		 * would show the windows under it. */
		c.red = c.green = c.blue = 0;
		c.flags = DoRed | DoGreen | DoBlue;
		(void)XAllocColor(d, look.cmap, &c);
	} else {
		look.visual = DefaultVisual(d, DefaultScreen(d));
		look.depth = DefaultDepth(d, DefaultScreen(d));
		look.cmap = DefaultColormap(d, DefaultScreen(d));
	}
	for (i = 0; i < NCOL; i++) {
		c.red = (unsigned short)(rgb[i][0] * 257);
		c.green = (unsigned short)(rgb[i][1] * 257);
		c.blue = (unsigned short)(rgb[i][2] * 257);
		c.flags = DoRed | DoGreen | DoBlue;
		look.px[i] = XAllocColor(d, look.cmap, &c) ? c.pixel
		    : (i == C_TEXT || i == C_DARK ? BlackPixel(d, DefaultScreen(d)) : WhitePixel(d, DefaultScreen(d)));
	}
	look.font = XLoadQueryFont(d, "-*-helvetica-bold-o-normal--14-*-*-*-*-*-iso8859-1");
	if (look.font == NULL)
		look.font = XLoadQueryFont(d, "fixed");
}

/* One open menu: a window, its entries' geometry, the entry under the
 * pointer. */
struct level {
	long p;
	Window win;
	GC gc;
	int x, y, w, h;          /* on the screen */
	int title_h, item_h;
	int hot;                 /* entry under the pointer, or -1 */
};

static int
text_width(const char *s)
{
	return look.font ? XTextWidth(look.font, s, (int)strlen(s)) : 8 * (int)strlen(s);
}

static void
draw_level(Display *d, struct level *L)
{
	int i, y, asc = look.font ? look.font->ascent : 10;
	GC gc = L->gc;

	XSetForeground(d, gc, look.px[C_BG]);
	XFillRectangle(d, L->win, gc, 0, 0, (unsigned)L->w, (unsigned)L->h);
	if (L->title_h) {
		XSetForeground(d, gc, look.px[C_TITLE]);
		XFillRectangle(d, L->win, gc, 0, 0, (unsigned)L->w, (unsigned)L->title_h);
		XSetForeground(d, gc, look.px[C_TEXT]);
		XDrawString(d, L->win, gc, (L->w - text_width(pups[L->p].title)) / 2,
		    (L->title_h - (look.font ? look.font->descent : 3) + asc) / 2 + 1,
		    pups[L->p].title, (int)strlen(pups[L->p].title));
		XSetForeground(d, gc, look.px[C_DARK]);
		XDrawLine(d, L->win, gc, 0, L->title_h - 1, L->w - 1, L->title_h - 1);
	}
	for (i = 0; i < pups[L->p].n; i++) {
		struct pup_item *it = &pups[L->p].item[i];
		int grey = (it->mode & PUP_GREY) != 0, hot = i == L->hot && !grey;
		int tx = 10;

		y = L->title_h + i * L->item_h;
		if (hot) {
			XSetForeground(d, gc, look.px[C_HI]);
			XFillRectangle(d, L->win, gc, 2, y + 1, (unsigned)(L->w - 4), (unsigned)(L->item_h - 2));
		}
		if (it->mode & (PUP_BOX | PUP_CHECK)) {
			int bs = asc - 2, by = y + (L->item_h - bs) / 2;

			XSetForeground(d, gc, look.px[hot ? C_HITEXT : C_TEXT]);
			XDrawRectangle(d, L->win, gc, 10, by, (unsigned)bs, (unsigned)bs);
			if (it->mode & PUP_CHECK) {
				XDrawLine(d, L->win, gc, 12, by + bs / 2, 10 + bs / 2, by + bs - 2);
				XDrawLine(d, L->win, gc, 10 + bs / 2, by + bs - 2, 8 + bs, by + 2);
			}
			tx = 16 + bs;
		}
		XSetForeground(d, gc, look.px[grey ? C_GREY : hot ? C_HITEXT : C_TEXT]);
		XDrawString(d, L->win, gc, tx, y + (L->item_h + asc) / 2 - 1, it->text, (int)strlen(it->text));
		if (it->sub) {
			/* An arrow at the right: a submenu. */
			XPoint a[3];

			a[0].x = (short)(L->w - 14); a[0].y = (short)(y + L->item_h / 2 - 4);
			a[1].x = (short)(L->w - 14); a[1].y = (short)(y + L->item_h / 2 + 4);
			a[2].x = (short)(L->w - 8);  a[2].y = (short)(y + L->item_h / 2);
			XFillPolygon(d, L->win, gc, a, 3, Convex, CoordModeOrigin);
		}
		if (it->line) {
			XSetForeground(d, gc, look.px[C_DARK]);
			XDrawLine(d, L->win, gc, 4, y + L->item_h - 1, L->w - 5, y + L->item_h - 1);
		}
	}
	/* A raised bevel round the whole menu. */
	XSetForeground(d, gc, look.px[C_LIGHT]);
	XDrawLine(d, L->win, gc, 0, 0, L->w - 1, 0);
	XDrawLine(d, L->win, gc, 0, 0, 0, L->h - 1);
	XSetForeground(d, gc, look.px[C_DARK]);
	XDrawLine(d, L->win, gc, 0, L->h - 1, L->w - 1, L->h - 1);
	XDrawLine(d, L->win, gc, L->w - 1, 0, L->w - 1, L->h - 1);
}

/* Open menu p with its top-left near (x, y) on the screen -- or, with
 * `at_pointer`, placed so that (x, y) is on its title (just above its first
 * entry when it has none): a release that has not moved chooses nothing. */
static void
open_level(Display *d, struct level *L, long p, int x, int y, int at_pointer)
{
	XSetWindowAttributes swa;
	int i, w, sw = DisplayWidth(d, DefaultScreen(d)), sh = DisplayHeight(d, DefaultScreen(d));
	int fh = look.font ? look.font->ascent + look.font->descent : 13;

	memset(L, 0, sizeof *L);
	L->p = p;
	L->hot = -1;
	L->item_h = fh + 8;
	L->title_h = pups[p].title[0] ? fh + 8 : 0;
	w = text_width(pups[p].title) + 24;
	for (i = 0; i < pups[p].n; i++) {
		int iw = text_width(pups[p].item[i].text) + 24;

		if (pups[p].item[i].mode & (PUP_BOX | PUP_CHECK))
			iw += fh + 4;
		if (pups[p].item[i].sub)
			iw += 18;
		if (iw > w)
			w = iw;
	}
	L->w = w < 60 ? 60 : w;
	L->h = L->title_h + pups[p].n * L->item_h;
	if (L->h < 4)
		L->h = 4;
	if (at_pointer) {
		x -= 20;
		y -= L->title_h ? L->title_h / 2 : -2;
	}
	L->x = x + L->w > sw ? sw - L->w : x < 0 ? 0 : x;
	L->y = y + L->h > sh ? sh - L->h : y < 0 ? 0 : y;
	memset(&swa, 0, sizeof swa);
	swa.override_redirect = True;
	swa.save_under = True;
	swa.colormap = look.cmap;
	swa.border_pixel = 0;
	swa.background_pixel = look.px[C_BG];
	swa.event_mask = ExposureMask;
	L->win = XCreateWindow(d, DefaultRootWindow(d), L->x, L->y, (unsigned)L->w, (unsigned)L->h, 0,
	    look.depth, InputOutput, look.visual,
	    CWOverrideRedirect | CWSaveUnder | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
	L->gc = XCreateGC(d, L->win, 0, NULL);
	if (look.font)
		XSetFont(d, L->gc, look.font->fid);
	XMapRaised(d, L->win);
}

static void
close_level(Display *d, struct level *L)
{
	if (L->win == None)
		return;
	XFreeGC(d, L->gc);
	XDestroyWindow(d, L->win);
	L->win = None;
}

/* The entry of level L at screen point (x, y), or -1. */
static int
entry_at(struct level *L, int x, int y)
{
	int i;

	if (x < L->x || x >= L->x + L->w || y < L->y + L->title_h || y >= L->y + L->h)
		return -1;
	i = (y - L->y - L->title_h) / L->item_h;
	return i < pups[L->p].n ? i : -1;
}

static double
now_s(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* What choosing entry i of menu p gives, after its functions. */
static long
chosen(long p, int i, long value)
{
	struct pup_item *it = &pups[p].item[i];

	if (it->fn != NULL)
		value = it->fn(value);
	if (pups[p].fn != NULL && !it->no_menu_fn)
		value = pups[p].fn(value);
	return value;
}

long
dopup(long p)
{
	Display *d = hgl_display();
	struct level lv[LEVELS];
	int depth = 1, px, py, rx, ry, wx, wy, down, sticky = 0, done = 0, grabbed = 0, moved = 0, pressed = 0, i, k;
	unsigned int mask;
	Window root, child;
	double opened;
	long result = -1;
	XEvent e;

	hgl_irisgl_tracef("dopup %ld", p);
	if (!valid(p))
		return -1;
	hgl_iris_present_if_single();
	look_init(d);
	XQueryPointer(d, DefaultRootWindow(d), &root, &child, &rx, &ry, &wx, &wy, &mask);
	down = (mask & (Button1Mask | Button2Mask | Button3Mask)) != 0;
	open_level(d, &lv[0], p, rx, ry, 1);
	if (look.own_cmap)
		XInstallColormap(d, look.cmap);
	/* Another client can hold the pointer a moment longer -- a toolkit
	 * program's own connection, until the button that opened the menu
	 * comes up -- so the grab is tried again for up to a second. */
	for (k = 0; k < 100; k++) {
		struct timespec ts = { 0, 10 * 1000 * 1000 };

		if (XGrabPointer(d, lv[0].win, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		    GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) {
			grabbed = 1;
			break;
		}
		nanosleep(&ts, NULL);
	}
	opened = now_s();
	px = rx;
	py = ry;
	if (!down)
		sticky = 1;

	hgl_irisgl_tracef("dopup: pointer %d %d, button %s, grab %s", rx, ry, down ? "down" : "up",
	    grabbed ? "taken" : "refused");
	while (!done) {
		XNextEvent(d, &e);
		for (k = 0; k < depth; k++)
			if (e.xany.window == lv[k].win)
				break;
		if (e.type == ButtonPress || e.type == ButtonRelease)
			hgl_irisgl_tracef("dopup: %s %d at %d %d on %s window, %.3f s in, %s",
			    e.type == ButtonPress ? "press" : "release", (int)e.xbutton.button,
			    e.xbutton.x_root, e.xbutton.y_root, k < depth ? "the menu's" : "another",
			    now_s() - opened, sticky ? "sticky" : "held");
		/*
		 * Pointer events that are not the menu's own were queued before
		 * its grab: the release of the click that opened it, when the
		 * program took its time over the press. They belong to the
		 * program; taken as the menu's, that release chose the entry the
		 * menu opens under the pointer.
		 */
		if (k == depth && (e.type == ButtonPress || e.type == ButtonRelease || e.type == MotionNotify)) {
			hgl_iris_event(&e);
			continue;
		}
		switch (e.type) {
		case Expose:
			if (k < depth) {
				if (e.xexpose.count == 0)
					draw_level(d, &lv[k]);
			} else {
				hgl_iris_event(&e);
			}
			continue;
		case MotionNotify:
			px = e.xmotion.x_root;
			py = e.xmotion.y_root;
			if (px - rx > 4 || rx - px > 4 || py - ry > 4 || ry - py > 4)
				moved = 1;
			break;
		case ButtonPress:
			px = e.xbutton.x_root;
			py = e.xbutton.y_root;
			pressed = 1;
			if (sticky) {
				/* A click outside every menu dismisses it. */
				for (k = 0; k < depth; k++)
					if (px >= lv[k].x && px < lv[k].x + lv[k].w && py >= lv[k].y && py < lv[k].y + lv[k].h)
						break;
				if (k == depth)
					done = 1;
			}
			continue;
		case ButtonRelease:
			px = e.xbutton.x_root;
			py = e.xbutton.y_root;
			/* Released where the menu opened: a click, which leaves it up. */
			if (!sticky && !moved) {
				sticky = 1;
				continue;
			}
			/* Once it is up, only a click made on it counts. */
			if (sticky && !pressed)
				continue;
			for (k = depth - 1; k >= 0; k--)
				if ((i = entry_at(&lv[k], px, py)) >= 0)
					break;
			if (k >= 0) {
				struct pup_item *it = &pups[lv[k].p].item[i];

				if (it->mode & PUP_GREY)
					continue;
				if (it->sub && k == depth - 1)
					continue;       /* its submenu is not open yet */
				if (!it->sub) {
					/* The choice, then each enclosing menu's
					 * functions, innermost first. */
					result = chosen(lv[k].p, i, it->value);
					while (--k >= 0)
						result = chosen(lv[k].p, lv[k].hot, result);
				} else {
					continue;
				}
			}
			/* The release goes to the program, as it would have. */
			hgl_iris_event(&e);
			done = 1;
			continue;
		default:
			hgl_iris_event(&e);
			continue;
		}

		/* The pointer moved: the deepest menu under it, and its entry. */
		for (k = depth - 1; k >= 0; k--)
			if (px >= lv[k].x && px < lv[k].x + lv[k].w && py >= lv[k].y && py < lv[k].y + lv[k].h)
				break;
		if (k < 0)
			continue;
		/* Leaving a submenu for its parent closes the deeper ones. */
		i = entry_at(&lv[k], px, py);
		if (k < depth - 1 && i != lv[k].hot) {
			while (depth - 1 > k)
				close_level(d, &lv[--depth]);
		}
		if (i != lv[k].hot) {
			lv[k].hot = i;
			draw_level(d, &lv[k]);
		}
		/* Onto an entry with a submenu: open it beside the entry. */
		if (i >= 0 && pups[lv[k].p].item[i].sub && !(pups[lv[k].p].item[i].mode & PUP_GREY) &&
		    k == depth - 1 && depth < LEVELS && valid(pups[lv[k].p].item[i].sub) &&
		    px > lv[k].x + lv[k].w / 2) {
			long sub = pups[lv[k].p].item[i].sub;

			open_level(d, &lv[depth], sub, lv[k].x + lv[k].w - 4, lv[k].y + lv[k].title_h + i * lv[k].item_h, 0);
			depth++;
		}
	}

	XUngrabPointer(d, CurrentTime);
	for (k = depth - 1; k >= 0; k--)
		close_level(d, &lv[k]);
	if (look.own_cmap)
		XUninstallColormap(d, look.cmap);
	XFlush(d);
	hgl_irisgl_tracef("dopup %ld -> %ld", p, result);
	return result;
}
