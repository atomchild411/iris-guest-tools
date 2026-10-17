/*
 * Host OpenGL for IRIS: the GLX half of the shim libGL.
 *
 * Installed in place of SGI's libGL.so (same SONAME), this answers GLX itself.
 * Visuals are the X server's own TrueColor ones, found with the guest's Xlib;
 * a context is a context on the host (HGL_GL_CREATE); making it current sizes
 * the host's framebuffer to the window; and glXSwapBuffers either has the
 * host put the frame straight into the window -- when the X server is IRIS's
 * own, which the host can reach -- or reads the finished frame back into a
 * buffer here and draws it into the window. When the server offers MIT-SHM
 * the buffer is a SysV shared memory segment and the frame goes with
 * XShmPutImage, so the X protocol carries only the request; otherwise the
 * pixels travel with XPutImage like any other client's.
 *
 * Without IRIS's host GL service (hgl_present), glXQueryExtension says there
 * is no GLX and no context can be made, so a program stops cleanly.
 */
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <GL/gl.h>
#include <GL/glx.h>

/* GLX 1.3, which IRIX 6.5.22's glx.h has and 6.5.7's (the build host's)
 * doesn't: its FBConfigs are the SGIX ones. */
#ifndef GLX_VERSION_1_3
typedef GLXFBConfigSGIX GLXFBConfig;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
#define GLX_SCREEN		0x800C
#define GLX_RENDER_TYPE		0x8011
#define GLX_FBCONFIG_ID		0x8013
#define GLX_RGBA_TYPE		0x8014
#define GLX_COLOR_INDEX_TYPE	0x8015
#define GLX_WIDTH		0x801D
#define GLX_HEIGHT		0x801E
#define GLX_PBUFFER_HEIGHT	0x8040
#define GLX_PBUFFER_WIDTH	0x8041
#endif
#include "glshim.h"
#include "glshim_rt.h"
#include "glshim_glxcfg.h"

void hgl_present_front(Display *dpy, GLXDrawable d);
static void present_index(Display *dpy, GLXDrawable d, int flush);
static int index_window(Display *dpy, GLXDrawable d);
int hgl_front_buffers(void);

/*
 * A drawable the host draws into: a window's X id, or a pbuffer's id from the
 * host. Its size goes with it, because a pbuffer has no window to ask.
 */
#define HGL_PBUFFERS 32
static struct {
	GLXDrawable id;
	int w, h;
} pbuffers[HGL_PBUFFERS];

/*
 * A GLX pixmap is an X pixmap drawn into with GL. Nothing presents it -- it is
 * single buffered -- so its pixels are put into the X pixmap whenever the
 * program says it has finished drawing: glFlush, glFinish, glXWaitGL, making
 * another drawable current, or destroying it.
 */
#define HGL_GLXPIXMAPS 16
static struct {
	GLXDrawable id;
	int w, h;
} glxpixmaps[HGL_GLXPIXMAPS];

static int
glxpixmap_of(GLXDrawable d)
{
	int i;

	for (i = 0; i < HGL_GLXPIXMAPS; i++)
		if (glxpixmaps[i].id == d && d != None)
			return i;
	return -1;
}

static int
pbuffer_of(GLXDrawable d)
{
	int i;

	for (i = 0; i < HGL_PBUFFERS; i++)
		if (pbuffers[i].id == d && d != None)
			return i;
	return -1;
}

/*
 * An fbconfig is a visual, which is all our X server has: one TrueColor
 * visual of depth 24. SGI's programs ask for configs rather than visuals when
 * they want a pbuffer, so the two have to name the same thing.
 */
/* An FBConfig is a visual. 6.5.22's glx.h names the struct __GLXFBConfigRec
 * (GLX 1.3's, which the SGIX one now shares), 6.5.7's __GLXFBConfigSGIXRec. */
#ifdef GLX_VERSION_1_3
#define HGL_FBCONFIG_REC __GLXFBConfigRec
#else
#define HGL_FBCONFIG_REC __GLXFBConfigSGIXRec
#endif
struct HGL_FBCONFIG_REC {
	XVisualInfo vi;
	int samples;        /* GLX_SGIS_multisample */
	const struct hgl_fbcfg *fb;     /* the server's FBConfig, if it has them */
};
static struct HGL_FBCONFIG_REC configs[96];
static int nconfigs;

/* The server's FBConfig for a visual, or NULL. */
static const struct hgl_fbcfg *
fbconfig_of_visual(Display *dpy, int screen, VisualID id)
{
	const struct hgl_fbcfg *f;
	int i, n, v;

	f = hgl_glx_fbconfigs(dpy, screen, &n);
	for (i = 0; f != NULL && i < n; i++)
		if (hgl_fbcfg_attr(&f[i], GLX_VISUAL_ID, &v) && (VisualID)v == id)
			return &f[i];
	return NULL;
}

static GLXFBConfigSGIX
config_for(Display *dpy, XVisualInfo *vi)
{
	int i;

	for (i = 0; i < nconfigs; i++)
		if (configs[i].vi.visualid == vi->visualid)
			return &configs[i];
	if (nconfigs == (int)(sizeof configs / sizeof configs[0]))
		return NULL;
	configs[nconfigs].vi = *vi;
	configs[nconfigs].samples = 0;
	configs[nconfigs].fb = fbconfig_of_visual(dpy, vi->screen, vi->visualid);
	return &configs[nconfigs++];
}

static GLXContext cur_ctx;
static GLXDrawable cur_draw;
static GLXDrawable cur_read;
static Display *cur_dpy;

/* The frame buffer glXSwapBuffers fills and puts. */
static char *frame;
static XImage *frame_image;
static GC frame_gc;
static int frame_w, frame_h;
static GLXDrawable frame_draw;
static Display *frame_dpy;
/* MIT-SHM: -1 not yet asked, 0 unavailable, 1 in use. */
static int frame_shm = -1;
static XShmSegmentInfo frame_seg;

static void
frame_free(void)
{
	if (frame_image == NULL)
		return;
	if (frame_shm == 1) {
		XShmDetach(frame_dpy, &frame_seg);
		XSync(frame_dpy, False);
		frame_image->data = NULL;
		XDestroyImage(frame_image);
		shmdt(frame_seg.shmaddr);
		shmctl(frame_seg.shmid, IPC_RMID, NULL);
	} else {
		frame_image->data = NULL;
		XDestroyImage(frame_image);
		free(frame);
	}
	frame_image = NULL;
	frame = NULL;
}

/*
 * The 24-bit TrueColor visual every GL frame is in. Not the default visual:
 * like a real Indy, the server's default is 8-bit PseudoColor, and an image
 * made with that visual's (empty) masks and 24-bit data describes nothing.
 */
static Visual *
true_visual(Display *dpy)
{
	static Visual *vis;
	XVisualInfo tmpl, *vi;
	int n;

	if (vis != NULL)
		return vis;
	tmpl.screen = DefaultScreen(dpy);
	tmpl.depth = 24;
	tmpl.class = TrueColor;
	vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
	if (vi == NULL)
		return DefaultVisual(dpy, DefaultScreen(dpy));
	vis = vi->visual;
	XFree(vi);
	return vis;
}

/*
 * A shared memory frame of w x h: the segment is marked for removal once both
 * sides have it attached, so nothing outlives the program.
 */
static int
frame_alloc_shm(Display *dpy, int w, int h)
{
	Visual *vis = true_visual(dpy);

	frame_image = XShmCreateImage(dpy, vis, 24, ZPixmap, NULL, &frame_seg, w, h);
	if (frame_image == NULL)
		return 0;
	frame_seg.shmid = shmget(IPC_PRIVATE, frame_image->bytes_per_line * h, IPC_CREAT | 0600);
	if (frame_seg.shmid < 0)
		goto fail;
	frame_seg.shmaddr = shmat(frame_seg.shmid, NULL, 0);
	if (frame_seg.shmaddr == (char *)-1) {
		shmctl(frame_seg.shmid, IPC_RMID, NULL);
		goto fail;
	}
	frame_seg.readOnly = True;
	if (!XShmAttach(dpy, &frame_seg)) {
		shmdt(frame_seg.shmaddr);
		shmctl(frame_seg.shmid, IPC_RMID, NULL);
		goto fail;
	}
	XSync(dpy, False);
	frame_image->data = frame = frame_seg.shmaddr;
	return 1;
fail:
	frame_image->data = NULL;
	XDestroyImage(frame_image);
	frame_image = NULL;
	return 0;
}

/* The window's size, and (when `viewable` is not NULL) whether it is on the
 * screen: mapped, with every ancestor mapped. */
static int
window_size(Display *dpy, GLXDrawable d, int *w, int *h, int *viewable)
{
	XWindowAttributes wa;

	if (!XGetWindowAttributes(dpy, d, &wa))
		return 0;
	*w = wa.width;
	*h = wa.height;
	if (viewable)
		*viewable = wa.map_state == IsViewable;
	return 1;
}

/* A GLX pixmap's pixels into its X pixmap. */
static void
present_glxpixmap(void)
{
	hgl_slot a[3];
	int p = glxpixmap_of(cur_draw);
	char *buf;
	XImage *im;
	GC gc;
	int w, h;

	if (p < 0 || cur_dpy == NULL)
		return;
	w = glxpixmaps[p].w;
	h = glxpixmaps[p].h;
	if ((buf = malloc((size_t)w * h * 4)) == NULL)
		return;
	a[0].i = (long)buf;
	a[1].i = w;
	a[2].i = h;
	if (hgl_call(HGL_GL_SWAP, a) == 0) {
		im = XCreateImage(cur_dpy, true_visual(cur_dpy), 24, ZPixmap, 0,
		    buf, w, h, 32, w * 4);
		if (im != NULL) {
			gc = XCreateGC(cur_dpy, cur_draw, 0, NULL);
			XPutImage(cur_dpy, cur_draw, gc, im, 0, 0, 0, 0, w, h);
			XFreeGC(cur_dpy, gc);
			im->data = NULL;
			XDestroyImage(im);
		}
	}
	free(buf);
}

/*
 * A program drawing into the front buffer -- single-buffered, or an Inventor
 * viewer, which draws a frame there with glDrawBuffer(GL_FRONT) after an
 * expose -- expects to be seen as it draws, and says it has finished with
 * glFlush or glFinish. A window reaches the screen only at a swap, which here
 * leaves the buffer as it was, so those present it: without, an Inventor
 * viewer drew its scene and showed a black window.
 */
static void
present_front(const char *by)
{
	static int debug = -1, said;

	if (debug < 0)
		debug = getenv("IRIS_GL_DEBUG") != NULL;
	/* IRIS_GL_DEBUG: the first few, which is enough to see a program's
	 * pattern. */
	if (debug && said < 12) {
		said++;
		fprintf(stderr, "libGL: %s: drawable 0x%lx, front buffer %s\n", by,
		    (unsigned long)cur_draw, hgl_front_drawn(0) ? "drawn" : "untouched");
	}
	/* An overlay has only the one buffer: every flush shows it. A host that
	 * keeps the front buffer shows it as it is; without one, the window's one
	 * buffer is shown by a swap. */
	if (cur_dpy != NULL && cur_draw != None && glxpixmap_of(cur_draw) < 0 &&
	    (hgl_front_drawn(1) || hgl_index_mode)) {
		if (hgl_index_mode || index_window(cur_dpy, cur_draw))
			present_index(cur_dpy, cur_draw, 1);
		else if (hgl_fronts)
			hgl_present_front(cur_dpy, cur_draw);
		else
			glXSwapBuffers(cur_dpy, cur_draw);
	}
}

/*
 * glFlush and glFinish are ours rather than generated: they are where a
 * program says it has finished drawing, which is when a GLX pixmap's pixels
 * have to be in the X pixmap, and a front buffer on the screen. The buffered
 * calls go to the host first.
 *
 * Every GLX operation that makes a host call anyway sends the buffer ahead of
 * it without waiting (hgl_flush_nowait). glFlush sends everything held without waiting for it to run, which
 * is all OpenGL asks of it; glFinish waits.
 */
void
glFlush(void)
{
	hgl_flush_send();
	present_glxpixmap();
	present_front("glFlush");
}

/* In glFinish's present, which must not be left for later (present_flush). */
static int finishing;

void
glFinish(void)
{
	hgl_slot a[1];

	hgl_flush_nowait();
	a[0].i = 0;
	hgl_call(HGL_GL_FINISH, a);
	present_glxpixmap();
	finishing = 1;
	present_front("glFinish");
	finishing = 0;
}

GLXPixmap
glXCreateGLXPixmap(Display *dpy, XVisualInfo *vis, Pixmap pixmap)
{
	Window root;
	unsigned int w, h, bw, depth;
	int x, y, i;

	(void)vis;
	if (!XGetGeometry(dpy, pixmap, &root, &x, &y, &w, &h, &bw, &depth))
		return None;
	for (i = 0; i < HGL_GLXPIXMAPS; i++)
		if (glxpixmaps[i].id == None) {
			/* The pixmap is its own GLX drawable, as X ids are unique. */
			glxpixmaps[i].id = pixmap;
			glxpixmaps[i].w = w;
			glxpixmaps[i].h = h;
			return pixmap;
		}
	return None;
}

/*
 * glXUseXFont: a display list per character, each holding the glyph as a
 * bitmap. The glyphs come from the X server that has the font -- each is
 * drawn into a pixmap and read back -- so a program gets the same letters it
 * would have drawn with X. They are read from a depth 24 pixmap, white on
 * black, rather than a bitmap one: it asks less of the server, and this runs
 * once for a font, not once for a letter.
 */
void
glXUseXFont(Font font, int first, int count, int listBase)
{
	Display *dpy = cur_dpy;
	XFontStruct *fs;
	XCharStruct *cs;
	Pixmap pm;
	GC gc;
	XImage *img;
	unsigned char *bits;
	int i, box_w, box_h, stride, x, y, lbearing, advance;

	if (dpy == NULL || (fs = XQueryFont(dpy, font)) == NULL)
		return;
	box_w = fs->max_bounds.rbearing - fs->min_bounds.lbearing;
	box_h = fs->max_bounds.ascent + fs->max_bounds.descent;
	if (box_w <= 0 || box_h <= 0) {
		XFreeFontInfo(NULL, fs, 1);
		return;
	}
	pm = XCreatePixmap(dpy, RootWindow(dpy, DefaultScreen(dpy)), box_w, box_h, 24);
	gc = XCreateGC(dpy, pm, 0, NULL);
	XSetFont(dpy, gc, font);
	stride = (box_w + 7) / 8;
	bits = malloc((size_t)stride * box_h);
	if (bits == NULL) {
		XFreeGC(dpy, gc);
		XFreePixmap(dpy, pm);
		XFreeFontInfo(NULL, fs, 1);
		return;
	}
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	for (i = 0; i < count; i++) {
		char ch = (char)(first + i);

		cs = &fs->max_bounds;
		if (fs->per_char != NULL && first + i >= (int)fs->min_char_or_byte2
		    && first + i <= (int)fs->max_char_or_byte2)
			cs = &fs->per_char[first + i - fs->min_char_or_byte2];
		lbearing = cs->lbearing;
		advance = cs->width;

		XSetForeground(dpy, gc, 0);
		XFillRectangle(dpy, pm, gc, 0, 0, box_w, box_h);
		XSetForeground(dpy, gc, 0xffffff);
		XDrawString(dpy, pm, gc, -lbearing, cs->ascent, &ch, 1);
		img = XGetImage(dpy, pm, 0, 0, box_w, box_h, AllPlanes, ZPixmap);
		memset(bits, 0, (size_t)stride * box_h);
		if (img != NULL) {
			/* GL reads a bitmap from the bottom up; X gives the top row first. */
			for (y = 0; y < box_h; y++)
				for (x = 0; x < box_w; x++)
					if (XGetPixel(img, x, box_h - 1 - y) != 0)
						bits[y * stride + x / 8] |= 0x80 >> (x % 8);
			XDestroyImage(img);
		}
		glNewList(listBase + i, GL_COMPILE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glBitmap(box_w, box_h, (GLfloat)-lbearing, (GLfloat)(box_h - cs->ascent), (GLfloat)advance, 0, bits);
		glEndList();
	}
	glPopClientAttrib();
	free(bits);
	XFreeGC(dpy, gc);
	XFreePixmap(dpy, pm);
	XFreeFontInfo(NULL, fs, 1);
}

/*
 * An index context's window: an overlay (8 bits) or a colour-index window (12
 * bits). The host hands the frame back as ordinary 32-bit pixels, A R G B,
 * with the index's low 8 bits in R and the next 4 in G (see hgl_index_mode);
 * packed to the window's own pixel size -- one byte, or two -- those are the
 * window's pixels, index 0 an overlay's transparent one. Never composited:
 * the host puts frames into the 24-bit planes only.
 */
static char *index_frame;
static int index_frame_bytes;

static int unobscured(Display *dpy, Window w);

/* What a present needs to know about its window (see window_where). */
struct where {
	int width, height, depth, viewable, unobscured, placed, x, y;
	Visual *visual;
};
static int window_where(Display *dpy, Window w, int cached, struct where *g);
static int overlay_of(Display *dpy, VisualID id);

/*
 * A flush's present of a window the display composites, sent so that the
 * program does not wait for it (HGL_GL_PRESENT_LATER): SGI's IRIS GL flushes
 * after every primitive it draws into the front buffer, and flight's cockpit
 * drew itself over a second, a present a primitive, where the board takes a
 * few hundred milliseconds. The host shows a window's flushes at most once a
 * frame, the last when the frame is up. Only for a window whose last present
 * the display took (`took`): one it hands back has to be put up here. Nonzero
 * when it was sent; zero for an ordinary present, which an IRIS without the
 * call always gets -- and glFinish, which promises the drawing is done: a
 * program may read its window back through X next (`finishing`, set by it).
 */
static int later_ok = 1;

static int
present_flush(hgl_slot *a, GLXDrawable d, GLXDrawable took)
{
	long long wait = 0;

	if (!later_ok || finishing || !(a[6].i & 1) || d != took)
		return 0;
	if (hgl_call2(HGL_GL_PRESENT_LATER, a, &wait) == 1)
		return 1;
	later_ok = 0;
	return 0;
}

/*
 * A colour-index window to the screen. flush: shown because the program
 * flushed or switched contexts, not by a swap -- bit 4, so a swap interval
 * does not pace it.
 */
static GLXDrawable index_took;   /* see present_flush */

static void
present_index(Display *dpy, GLXDrawable d, int flush)
{
	struct where wa;
	hgl_slot a[7];
	long long wait = 0;
	XImage *im;
	GC gc;
	unsigned long mask;
	unsigned char *f;
	int x, y, w, h, bpp;

	if (!window_where(dpy, d, flush, &wa) || wa.depth > 16 || wa.width <= 0 || wa.height <= 0)
		return;
	w = wa.width;
	h = wa.height;
	im = XCreateImage(dpy, wa.visual, wa.depth, ZPixmap, 0, NULL, w, h, 32, 0);
	if (im == NULL)
		return;
	bpp = im->bits_per_pixel;
	if ((bpp != 8 && bpp != 16) || w * h * 4 < im->bytes_per_line * h) {
		XDestroyImage(im);
		return;
	}
	if (w * h * 4 > index_frame_bytes) {
		free(index_frame);
		index_frame = malloc((size_t)w * h * 4);
		index_frame_bytes = index_frame != NULL ? w * h * 4 : 0;
		if (index_frame == NULL) {
			XDestroyImage(im);
			return;
		}
	}
	hgl_flush_nowait();
	a[0].i = (long)index_frame;
	a[1].i = w;
	a[2].i = h;
	a[3].i = 0;
	a[4].i = a[5].i = a[6].i = 0;
	/*
	 * A window on the screen that nothing covers goes to a host that keeps
	 * colour-index pixels as indices, for it to put into the window itself
	 * (bit 2; bit 3 for an overlay), as RGBA frames are composited (see
	 * present_window). It answers nonzero and nothing comes back. A host
	 * without that hands the frame back, and it is put up below.
	 */
	if (getenv("IRIS_HOSTGL_XPUTIMAGE") == NULL && wa.placed && wa.unobscured) {
		a[4].i = wa.x;
		a[5].i = wa.y;
		a[6].i = 1 | 4 | (overlay_of(dpy, XVisualIDFromVisual(wa.visual)) >= 0 ? 8 : 0);
	}
	if (flush)
		a[6].i |= 16;
	if (flush && present_flush(a, d, index_took)) {
		XDestroyImage(im);
		return;
	}
	if (hgl_call2(HGL_GL_SWAP, a, &wait) != 0) {
		index_took = (a[6].i & 1) ? d : None;
		XDestroyImage(im);
		return;
	}
	index_took = None;
	hgl_wait(wait);
	/* In place: row y's pixels land no later than where they were read. */
	f = (unsigned char *)index_frame;
	mask = (1UL << wa.depth) - 1;
	for (y = 0; y < h; y++) {
		unsigned char *src = f + (size_t)y * w * 4, *dst = f + (size_t)y * im->bytes_per_line;

		for (x = 0; x < w; x++) {
			unsigned long p = (src[x * 4 + 1] | (unsigned long)src[x * 4 + 2] << 8) & mask;

			if (bpp == 8) {
				dst[x] = (unsigned char)p;
			} else {
				/* The server's byte order, which XCreateImage chose. */
				if (im->byte_order == MSBFirst) {
					dst[x * 2] = (unsigned char)(p >> 8);
					dst[x * 2 + 1] = (unsigned char)p;
				} else {
					dst[x * 2] = (unsigned char)p;
					dst[x * 2 + 1] = (unsigned char)(p >> 8);
				}
			}
		}
	}
	{
		/* IRIS_GL_DEBUG: each overlay present, and how much it holds. */
		static int debug = -1, said;

		if (debug < 0)
			debug = getenv("IRIS_GL_DEBUG") != NULL;
		if (debug && said < 40) {
			long lit = 0;

			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					if (bpp == 8 ? f[(size_t)y * im->bytes_per_line + x] != 0
					    : (f[(size_t)y * im->bytes_per_line + 2 * x] | f[(size_t)y * im->bytes_per_line + 2 * x + 1]) != 0)
						lit++;
			said++;
			fprintf(stderr, "libGL: index present 0x%lx %dx%d: %ld pixels not 0\n",
			    (unsigned long)d, w, h, lit);
		}
	}
	im->data = index_frame;
	gc = XCreateGC(dpy, d, 0, NULL);
	XPutImage(dpy, d, gc, im, 0, 0, 0, 0, w, h);
	XFreeGC(dpy, gc);
	XFlush(dpy);
	im->data = NULL;
	XDestroyImage(im);
}

Bool
glXQueryExtension(Display *dpy, int *error_base, int *event_base)
{
	(void)dpy;
	if (error_base)
		*error_base = 0;
	if (event_base)
		*event_base = 0;
	/* No host GL service, no GLX: a program says so and stops. */
	return hgl_present() ? True : False;
}

Bool
glXQueryVersion(Display *dpy, int *major, int *minor)
{
	(void)dpy;
	if (major)
		*major = 1;
	if (minor)
		*minor = 3;
	return True;
}

/*
 * Overlay visuals, as the X server lists them on the root window in
 * SERVER_OVERLAY_VISUALS: per visual, its transparent type and value and its
 * layer. On IMPACT, two 8-bit PseudoColor visuals in layer 1, one of them
 * with pixel 0 transparent.
 */
#define HGL_OVERLAYS 8
static struct {
	VisualID id;
	long type, value, layer;
} overlays[HGL_OVERLAYS];
static int noverlays = -1;

static void
overlay_visuals(Display *dpy)
{
	Atom prop, type;
	int format;
	unsigned long n, after, i;
	unsigned char *data = NULL;
	long *v;

	if (noverlays >= 0 || dpy == NULL)
		return;
	noverlays = 0;
	if ((prop = XInternAtom(dpy, "SERVER_OVERLAY_VISUALS", True)) == None)
		return;
	if (XGetWindowProperty(dpy, RootWindow(dpy, DefaultScreen(dpy)), prop, 0, 4 * HGL_OVERLAYS,
	    False, AnyPropertyType, &type, &format, &n, &after, &data) != Success || data == NULL)
		return;
	/* Format 32 comes back as longs, whatever their size. */
	v = (long *)data;
	for (i = 0; format == 32 && i + 4 <= n && noverlays < HGL_OVERLAYS; i += 4) {
		overlays[noverlays].id = (VisualID)v[i];
		overlays[noverlays].type = v[i + 1];
		overlays[noverlays].value = v[i + 2];
		overlays[noverlays].layer = v[i + 3];
		noverlays++;
	}
	XFree(data);
}

/* The overlay entry for visual `id`, or -1 for a visual of the normal planes. */
static int
overlay_of(Display *dpy, VisualID id)
{
	int i;

	overlay_visuals(dpy);
	for (i = 0; i < noverlays; i++)
		if (overlays[i].id == id)
			return i;
	return -1;
}

/*
 * The normal planes' RGBA GL windows are the host's, drawn into a 24-bit
 * TrueColor X window, so that is the visual handed out for GLX_LEVEL 0.
 *
 * A colour-index request at GLX_LEVEL 0 gets IMPACT's 12-bit PseudoColor
 * visual, as it would on SGI's hardware: its contexts draw indices, like an
 * overlay's, and each flush puts them into the window's pixels, shown in
 * the colours of the program's own colormap. 6.5.22's gr_osview asks for
 * just that (GLX_BUFFER_SIZE 12, single buffered, no GLX_RGBA) and drew
 * nothing on the screen when it was given TrueColor: its drawing went to a
 * back buffer that nothing ever showed. A server without such a visual
 * falls back to TrueColor, as before.
 *
 * A single-buffered RGBA request (no GLX_DOUBLEBUFFER) gets a TrueColor
 * visual of its own, the screen's last 24-bit one, and a context made for
 * it starts out drawing into the front buffer, as a single-buffered GLX
 * context does; glFlush then shows each frame (present_front). 6.5.22's
 * ical, mag, colorbars and grid draw that way and never swap: with the
 * double-buffered visual their windows stayed empty. With only one 24-bit
 * TrueColor visual there is nothing to tell the two apart by, and both get
 * it, double buffered, as before.
 *
 * An overlay (a GLX_LEVEL above 0) gets the X server's own overlay visual
 * for that layer, the one with a transparent pixel if there is one: an
 * 8-bit PseudoColor visual, colour index only, as on SGI's hardware. Its
 * contexts draw indices (glshim_rt.c, hgl_index_mode), and what they draw
 * goes into the overlay window's pixels, which the board shows over the
 * picture. An RGBA overlay and any underlay are refused, as there are none.
 * Handing out the TrueColor visual for an overlay, as this once did, left a
 * program storing colours into its read-only colormap, and it died.
 */
static int
glx_attribs(const int *attribs, int *rgba, int *dbl)
{
	int level = 0;

	*rgba = *dbl = 0;
	while (attribs != NULL && *attribs != None) {
		switch (*attribs) {
		/* The attributes that stand alone. */
		case GLX_RGBA:
			*rgba = 1;
			attribs++;
			break;
		case GLX_DOUBLEBUFFER:
			*dbl = 1;
			attribs++;
			break;
		case GLX_USE_GL: case GLX_STEREO:
			attribs++;
			break;
		case GLX_LEVEL:
			level = attribs[1];
			attribs += 2;
			break;
		/* Everything else is followed by its value. */
		default:
			attribs += 2;
			break;
		}
	}
	return level;
}

/* The single-buffered RGBA visual (see above), or 0 when the screen has
 * only the one 24-bit TrueColor visual. */
static VisualID single_vid;
static int single_known;

static VisualID
single_visual(Display *dpy, int screen)
{
	XVisualInfo tmpl, *vi;
	int n;

	if (single_known || dpy == NULL)
		return single_vid;
	single_known = 1;
	tmpl.screen = screen;
	tmpl.depth = 24;
	tmpl.class = TrueColor;
	vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
	if (vi == NULL)
		return 0;
	if (n >= 2)
		single_vid = vi[n - 1].visualid;
	XFree(vi);
	return single_vid;
}

/*
 * glXChooseVisual over the server's table (glshim_glxcfg.c), by GLX 1.2's
 * rules: GLX_RGBA, GLX_DOUBLEBUFFER and GLX_STEREO as asked (one not named
 * means a visual without it), GLX_LEVEL exactly, every size at least what is
 * asked. Among those: the smallest colour-index buffer; for each colour
 * component, the smallest when none is asked for and the largest otherwise;
 * no depth or accumulation buffer when none is asked for and the largest
 * otherwise; the fewest stencil bits and aux buffers. Then, as before, an
 * overlay with a transparent pixel; then the lowest visual id.
 *
 * What the host can show narrows the choice first: RGBA drawing reaches the
 * screen as 24-bit pixels, so an RGBA visual must be a 24-bit TrueColor one
 * (IMPACT's 12-bit RGBA visuals are left out), and colour indices are put
 * into 8- and 12-bit windows.
 */
struct visual_want {
	int rgba, dbl, stereo, level, buffer, aux;
	int red, green, blue, alpha, depth, stencil, accum[4];
};

static void
want_of(const int *a, struct visual_want *w)
{
	memset(w, 0, sizeof *w);
	while (a != NULL && *a != None) {
		switch (*a) {
		case GLX_USE_GL: a++; continue;
		case GLX_RGBA: w->rgba = 1; a++; continue;
		case GLX_DOUBLEBUFFER: w->dbl = 1; a++; continue;
		case GLX_STEREO: w->stereo = 1; a++; continue;
		case GLX_BUFFER_SIZE: w->buffer = a[1]; break;
		case GLX_LEVEL: w->level = a[1]; break;
		case GLX_AUX_BUFFERS: w->aux = a[1]; break;
		case GLX_RED_SIZE: w->red = a[1]; break;
		case GLX_GREEN_SIZE: w->green = a[1]; break;
		case GLX_BLUE_SIZE: w->blue = a[1]; break;
		case GLX_ALPHA_SIZE: w->alpha = a[1]; break;
		case GLX_DEPTH_SIZE: w->depth = a[1]; break;
		case GLX_STENCIL_SIZE: w->stencil = a[1]; break;
		case GLX_ACCUM_RED_SIZE: w->accum[0] = a[1]; break;
		case GLX_ACCUM_GREEN_SIZE: w->accum[1] = a[1]; break;
		case GLX_ACCUM_BLUE_SIZE: w->accum[2] = a[1]; break;
		case GLX_ACCUM_ALPHA_SIZE: w->accum[3] = a[1]; break;
		default: break;
		}
		a += 2;
	}
}

/* A size by its rule: the smallest when none is asked for, else the largest. */
static int
size_cmp(int want, int a, int b)
{
	return want == 0 ? a - b : b - a;
}

static int
cfg_tag(const struct hgl_glxcfg *c, int tag, int *value)
{
	int i;

	for (i = 0; i < c->ntags; i++)
		if (c->tags[i][0] == tag) {
			*value = c->tags[i][1];
			return 1;
		}
	return 0;
}

/* Negative: a is the better choice for w. */
static int
cfg_cmp(const struct visual_want *w, const struct hgl_glxcfg *a, const struct hgl_glxcfg *b)
{
	int d, ta = 0, tb = 0;

	if (w->level != 0) {
		(void)cfg_tag(a, GLX_TRANSPARENT_TYPE_EXT, &ta);
		(void)cfg_tag(b, GLX_TRANSPARENT_TYPE_EXT, &tb);
		if ((ta != GLX_NONE_EXT) != (tb != GLX_NONE_EXT))
			return ta != GLX_NONE_EXT ? -1 : 1;
	}
	if (!w->rgba && (d = a->buffer - b->buffer) != 0)
		return d;
	if ((d = size_cmp(w->red, a->red, b->red)) != 0 ||
	    (d = size_cmp(w->green, a->green, b->green)) != 0 ||
	    (d = size_cmp(w->blue, a->blue, b->blue)) != 0 ||
	    (d = size_cmp(w->alpha, a->alpha, b->alpha)) != 0 ||
	    (d = size_cmp(w->depth, a->depth, b->depth)) != 0 ||
	    (d = a->stencil - b->stencil) != 0 ||
	    (d = size_cmp(w->accum[0], a->accum_red, b->accum_red)) != 0 ||
	    (d = size_cmp(w->accum[3], a->accum_alpha, b->accum_alpha)) != 0 ||
	    (d = a->aux - b->aux) != 0)
		return d;
	return a->id < b->id ? -1 : a->id > b->id;
}

static int
cfg_fits(Display *dpy, int screen, const struct visual_want *w, const struct hgl_glxcfg *c)
{
	XVisualInfo tmpl, *vi;
	int n, ok;

	if (c->rgba != w->rgba || c->dbl != w->dbl || c->stereo != w->stereo || c->level != w->level ||
	    c->aux < w->aux || c->depth < w->depth || c->stencil < w->stencil ||
	    c->accum_red < w->accum[0] || c->accum_green < w->accum[1] ||
	    c->accum_blue < w->accum[2] || c->accum_alpha < w->accum[3])
		return 0;
	if (w->rgba ? (c->red < w->red || c->green < w->green || c->blue < w->blue || c->alpha < w->alpha)
	    : c->buffer < w->buffer)
		return 0;
	/* What the host can show (see above). */
	tmpl.screen = screen;
	tmpl.visualid = c->id;
	vi = XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
	if (vi == NULL)
		return 0;
	if (w->rgba)
		ok = vi->class == TrueColor && vi->depth == 24;
	else
		ok = vi->depth == 8 || vi->depth == 12;
	XFree(vi);
	return ok;
}

static XVisualInfo *choose_visual_own(Display *dpy, int screen, int *attribs);
GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements);

/* An RGBA visual without a back buffer: the server's table says, or, without
 * one, it is the single-buffered visual of the old rules. */
static int
single_rgba(Display *dpy, XVisualInfo *vis)
{
	const struct hgl_glxcfg *c;
	int n;

	if (hgl_glx_configs(dpy, vis->screen, &n) != NULL)
		return (c = hgl_glx_config(dpy, vis->screen, vis->visualid)) != NULL && c->rgba && !c->dbl;
	return vis->visualid == single_visual(dpy, vis->screen);
}

XVisualInfo *
glXChooseVisual(Display *dpy, int screen, int *attribs)
{
	const struct hgl_glxcfg *c, *best = NULL;
	struct visual_want w;
	XVisualInfo tmpl;
	int n, i;

	/* IRIS_GL_DEBUG: what the program asked for, attribute by attribute. */
	if (getenv("IRIS_GL_DEBUG") != NULL) {
		fprintf(stderr, "libGL: glXChooseVisual:");
		for (i = 0; attribs != NULL && attribs[i] != None && i < 64; i++)
			fprintf(stderr, " %d", attribs[i]);
		fprintf(stderr, "\n");
	}
	if ((c = hgl_glx_configs(dpy, screen, &n)) == NULL)
		return choose_visual_own(dpy, screen, attribs);
	want_of(attribs, &w);
	for (i = 0; i < n; i++)
		if (cfg_fits(dpy, screen, &w, &c[i]) && (best == NULL || cfg_cmp(&w, &c[i], best) < 0))
			best = &c[i];
	if (best == NULL)
		return NULL;
	tmpl.screen = screen;
	tmpl.visualid = best->id;
	return XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
}

/* The rules from before the server's table, for a server without GLX. */
static XVisualInfo *
choose_visual_own(Display *dpy, int screen, int *attribs)
{
	XVisualInfo tmpl, *vi;
	int n, i, rgba, dbl, level = glx_attribs(attribs, &rgba, &dbl), pick = -1;

	if (level != 0) {
		if (rgba || level < 0)
			return NULL;
		overlay_visuals(dpy);
		for (i = 0; i < noverlays; i++)
			if (overlays[i].layer == level && (pick < 0 || overlays[i].type != 0))
				pick = i;
		if (pick < 0)
			return NULL;
		tmpl.screen = screen;
		tmpl.visualid = overlays[pick].id;
		return XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
	}
	tmpl.screen = screen;
	if (!rgba) {
		tmpl.depth = 12;
		tmpl.class = PseudoColor;
		vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
		if (vi != NULL)
			return vi;
	}
	if (rgba && !dbl && (tmpl.visualid = single_visual(dpy, screen)) != 0)
		return XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
	tmpl.depth = 24;
	tmpl.class = TrueColor;
	return XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
}

/* An overlay visual's configuration: colour index, single buffered, no
 * ancillary buffers. */
static int
overlay_config(int o, XVisualInfo *vis, int attrib, int *value)
{
	switch (attrib) {
	case GLX_USE_GL: *value = 1; break;
	case GLX_BUFFER_SIZE: *value = vis->depth; break;
	case GLX_LEVEL: *value = (int)overlays[o].layer; break;
#ifdef GLX_EXT_visual_info
	case GLX_X_VISUAL_TYPE_EXT:
		*value = vis->class == StaticColor ? GLX_STATIC_COLOR_EXT : GLX_PSEUDO_COLOR_EXT;
		break;
	case GLX_TRANSPARENT_TYPE_EXT:
		*value = overlays[o].type != 0 ? GLX_TRANSPARENT_INDEX_EXT : GLX_NONE_EXT;
		break;
	case GLX_TRANSPARENT_INDEX_VALUE_EXT: *value = (int)overlays[o].value; break;
	case GLX_VISUAL_CAVEAT_EXT: *value = GLX_NONE_EXT; break;
#endif
	default: *value = 0; break;
	}
	return 0;
}

/* glXGetConfig from the server's table: what SGI's libGL answers. */
static int
config_from_table(const struct hgl_glxcfg *c, int attrib, int *value)
{
	switch (attrib) {
	case GLX_USE_GL: *value = 1; return 0;
	case GLX_BUFFER_SIZE: *value = c->buffer; return 0;
	case GLX_LEVEL: *value = c->level; return 0;
	case GLX_RGBA: *value = c->rgba; return 0;
	case GLX_DOUBLEBUFFER: *value = c->dbl; return 0;
	case GLX_STEREO: *value = c->stereo; return 0;
	case GLX_AUX_BUFFERS: *value = c->aux; return 0;
	case GLX_RED_SIZE: *value = c->red; return 0;
	case GLX_GREEN_SIZE: *value = c->green; return 0;
	case GLX_BLUE_SIZE: *value = c->blue; return 0;
	case GLX_ALPHA_SIZE: *value = c->alpha; return 0;
	case GLX_DEPTH_SIZE: *value = c->depth; return 0;
	case GLX_STENCIL_SIZE: *value = c->stencil; return 0;
	case GLX_ACCUM_RED_SIZE: *value = c->accum_red; return 0;
	case GLX_ACCUM_GREEN_SIZE: *value = c->accum_green; return 0;
	case GLX_ACCUM_BLUE_SIZE: *value = c->accum_blue; return 0;
	case GLX_ACCUM_ALPHA_SIZE: *value = c->accum_alpha; return 0;
	default:
		return cfg_tag(c, attrib, value) ? 0 : GLX_BAD_ATTRIBUTE;
	}
}

int
glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value)
{
	const struct hgl_glxcfg *c;
	int o, n;

	if (vis != NULL && hgl_glx_configs(dpy, vis->screen, &n) != NULL) {
		if ((c = hgl_glx_config(dpy, vis->screen, vis->visualid)) != NULL)
			return config_from_table(c, attrib, value);
		/* A visual GLX does not draw into. */
		if (attrib == GLX_USE_GL) {
			*value = 0;
			return 0;
		}
		return GLX_BAD_VISUAL;
	}

	if (vis != NULL && (o = overlay_of(dpy, vis->visualid)) >= 0)
		return overlay_config(o, vis, attrib, value);
	/* A colour-index visual of the normal planes: indices, shown at each
	 * flush as a single-buffered window is, with the host context's depth
	 * and stencil buffers. */
	if (vis != NULL && (vis->class == PseudoColor || vis->class == StaticColor ||
	    vis->class == GrayScale)) {
		switch (attrib) {
		case GLX_USE_GL: *value = 1; break;
		case GLX_BUFFER_SIZE: *value = vis->depth; break;
		case GLX_DEPTH_SIZE: *value = 24; break;
		case GLX_STENCIL_SIZE: *value = 8; break;
#ifdef GLX_EXT_visual_info
		case GLX_X_VISUAL_TYPE_EXT:
			*value = vis->class == StaticColor ? GLX_STATIC_COLOR_EXT :
			    vis->class == GrayScale ? GLX_GRAY_SCALE_EXT : GLX_PSEUDO_COLOR_EXT;
			break;
		case GLX_TRANSPARENT_TYPE_EXT: *value = GLX_NONE_EXT; break;
		case GLX_VISUAL_CAVEAT_EXT: *value = GLX_NONE_EXT; break;
#endif
		default: *value = 0; break;
		}
		return 0;
	}
	switch (attrib) {
	case GLX_USE_GL:
	case GLX_RGBA:
		*value = 1;
		break;
	case GLX_DOUBLEBUFFER:
		*value = vis == NULL || vis->visualid != single_visual(dpy, vis->screen);
		break;
	case GLX_BUFFER_SIZE:
		*value = 32;
		break;
	case GLX_RED_SIZE:
	case GLX_GREEN_SIZE:
	case GLX_BLUE_SIZE:
	case GLX_ALPHA_SIZE:
		*value = 8;
		break;
	case GLX_DEPTH_SIZE:
		*value = 24;
		break;
	case GLX_STENCIL_SIZE:
		*value = 8;
		break;
	/* The accumulation buffer IRIS keeps for each drawable (its framebuffer
	 * objects can have none): floats, reported as SGI's 16 bits. */
	case GLX_ACCUM_RED_SIZE:
	case GLX_ACCUM_GREEN_SIZE:
	case GLX_ACCUM_BLUE_SIZE:
	case GLX_ACCUM_ALPHA_SIZE:
		*value = 16;
		break;
	default:
		*value = 0;
		break;
	}
	return 0;
}

/* Which contexts draw colour indices: those made for an overlay visual or
 * any other colour-map visual (IRIS GL's colour-index windows, 12 bits). */
#define HGL_INDEX_CTXS 32
static GLXContext index_ctxs[HGL_INDEX_CTXS];

static int
is_index_ctx(GLXContext ctx)
{
	int i;

	for (i = 0; ctx != NULL && i < HGL_INDEX_CTXS; i++)
		if (index_ctxs[i] == ctx)
			return 1;
	return 0;
}

static void
note_index_ctx(GLXContext ctx, int on)
{
	int i;

	for (i = 0; i < HGL_INDEX_CTXS; i++)
		if (index_ctxs[i] == ctx)
			index_ctxs[i] = NULL;
	for (i = 0; on && i < HGL_INDEX_CTXS; i++)
		if (index_ctxs[i] == NULL) {
			index_ctxs[i] = ctx;
			return;
		}
}

/* Contexts made for the single-buffered visual that have not been current
 * yet: the first time they are, they start drawing into the front buffer. */
#define HGL_SINGLE_CTXS 32
static GLXContext single_ctxs[HGL_SINGLE_CTXS];

static void
note_single_ctx(GLXContext ctx, int on)
{
	int i;

	for (i = 0; i < HGL_SINGLE_CTXS; i++)
		if (single_ctxs[i] == ctx)
			single_ctxs[i] = NULL;
	for (i = 0; on && i < HGL_SINGLE_CTXS; i++)
		if (single_ctxs[i] == NULL) {
			single_ctxs[i] = ctx;
			return;
		}
}

static int
take_single_ctx(GLXContext ctx)
{
	int i;

	for (i = 0; ctx != NULL && i < HGL_SINGLE_CTXS; i++)
		if (single_ctxs[i] == ctx) {
			single_ctxs[i] = NULL;
			return 1;
		}
	return 0;
}

/* What GLX's context queries (glXQueryContext, glXQueryContextInfoEXT)
 * ask of a context: the host holds the context, this the facts. */
#define HGL_CTXINFO 64
static struct {
	GLXContext ctx, share;
	VisualID visual;
	int screen;
} ctxinfo[HGL_CTXINFO];

static int
ctxinfo_of(GLXContext ctx)
{
	int i;

	for (i = 0; ctx != NULL && i < HGL_CTXINFO; i++)
		if (ctxinfo[i].ctx == ctx)
			return i;
	return -1;
}

static void
ctxinfo_note(GLXContext ctx, XVisualInfo *vis, GLXContext share)
{
	int i = ctxinfo_of(ctx);

	if (i < 0)
		for (i = 0; i < HGL_CTXINFO && ctxinfo[i].ctx != NULL; i++)
			;
	if (i >= HGL_CTXINFO)
		return;
	ctxinfo[i].ctx = ctx;
	ctxinfo[i].share = share;
	ctxinfo[i].visual = vis != NULL ? vis->visualid : 0;
	ctxinfo[i].screen = vis != NULL ? vis->screen : 0;
}

GLXContext
glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext share, Bool direct)
{
	hgl_slot a[1];
	long long r;

	(void)direct;
	a[0].i = (long)(char *)share;
	r = hgl_call(HGL_GL_CREATE, a);
	if (r <= 0)
		return NULL;
	note_index_ctx((GLXContext)(long)r, vis != NULL && (overlay_of(dpy, vis->visualid) >= 0 ||
	    vis->class == PseudoColor || vis->class == StaticColor || vis->class == GrayScale));
	note_single_ctx((GLXContext)(long)r, vis != NULL && vis->visualid != 0 && single_rgba(dpy, vis));
	ctxinfo_note((GLXContext)(long)r, vis, share);
	return (GLXContext)(long)r;
}

void
glXDestroyGLXPixmap(Display *dpy, GLXPixmap pix)
{
	hgl_slot a[1];
	int p = glxpixmap_of(pix);

	(void)dpy;
	if (cur_draw == pix)
		present_glxpixmap();
	hgl_flush_nowait();
	a[0].i = pix;
	hgl_call(HGL_GL_DRAWABLE_GONE, a);
	if (p >= 0)
		glxpixmaps[p].id = None;
}

void
glXDestroyContext(Display *dpy, GLXContext ctx)
{
	hgl_slot a[1];

	(void)dpy;
	hgl_flush_nowait();
	a[0].i = (long)(char *)ctx;
	hgl_call(HGL_GL_DESTROY, a);
	hgl_client_forget(ctx);
	note_index_ctx(ctx, 0);
	note_single_ctx(ctx, 0);
	if (ctxinfo_of(ctx) >= 0)
		ctxinfo[ctxinfo_of(ctx)].ctx = NULL;
	if (ctx == cur_ctx)
		cur_ctx = NULL;
}

static Bool
make_current(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	hgl_slot a[5];
	int w, h, p;

	present_glxpixmap();
	/* What the context going out drew where it is seen at once on the board
	 * -- a front buffer, an overlay's single buffer -- is shown now, as a
	 * glFlush would: Performer draws Mekton's HUD into its overlay window
	 * and makes the scene's context current again without one. */
	if (cur_ctx != NULL && cur_draw != None && (ctx != cur_ctx || draw != cur_draw))
		present_front("glXMakeCurrent");
	hgl_flush_nowait();
	if (ctx == NULL || draw == None) {
		a[0].i = 0;
		hgl_call(HGL_GL_RELEASE, a);
		hgl_client_select(NULL);
		cur_ctx = NULL;
		cur_draw = None;
		return True;
	}
	if ((p = pbuffer_of(draw)) >= 0) {
		w = pbuffers[p].w;
		h = pbuffers[p].h;
	} else if ((p = glxpixmap_of(draw)) >= 0) {
		w = glxpixmaps[p].w;
		h = glxpixmaps[p].h;
	} else if (!window_size(dpy, draw, &w, &h, NULL)) {
		return False;
	}
	a[0].i = (long)(char *)ctx;
	a[1].i = draw;
	a[2].i = read;
	a[3].i = w;
	a[4].i = h;
	if (hgl_call(HGL_GL_MAKECURRENT, a) != 0)
		return False;
	hgl_client_select(ctx);
	hgl_client_index(is_index_ctx(ctx));
	if (hgl_index_mode) {
		XWindowAttributes wa;

		hgl_index_bits = XGetWindowAttributes(dpy, draw, &wa) && wa.depth > 8 ? 12 : 8;
	}
	/* A single-buffered context, current on a window for the first time.
	 * GLX pixmaps and pbuffers keep the buffer their pixels are read back
	 * from. */
	if (pbuffer_of(draw) < 0 && glxpixmap_of(draw) < 0 && take_single_ctx(ctx)) {
		hgl_client_single();
		glDrawBuffer(GL_FRONT);
		glReadBuffer(GL_FRONT);
	}
	cur_ctx = ctx;
	cur_draw = draw;
	cur_read = read;
	cur_dpy = dpy;
	return True;
}

Bool
glXMakeCurrent(Display *dpy, GLXDrawable d, GLXContext ctx)
{
	return make_current(dpy, d, d, ctx);
}

/* GLX_SGI_make_current_read: read from one drawable while drawing into another. */
Bool
glXMakeCurrentReadSGI(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
{
	return make_current(dpy, draw, read, ctx);
}

GLXDrawable
glXGetCurrentReadDrawableSGI(void)
{
	return cur_read ? cur_read : cur_draw;
}

/* GLX_SGIX_fbconfig. */
/*
 * FBConfigs from the server's table (glshim_glxcfg.c), chosen by GLX 1.3's
 * rules: GLX_DRAWABLE_TYPE and GLX_RENDER_TYPE as masks (by default a
 * window, RGBA), GLX_LEVEL, GLX_DOUBLEBUFFER, GLX_STEREO, GLX_X_RENDERABLE,
 * GLX_X_VISUAL_TYPE, GLX_CONFIG_CAVEAT, GLX_TRANSPARENT_TYPE and
 * GLX_FBCONFIG_ID exactly, every size at least (GLX_DONT_CARE: any); then
 * sorted: no caveat first, the most colour bits asked for, the smallest
 * buffer, single buffered first, the fewest aux buffers, the deepest depth
 * buffer, the fewest stencil bits, the largest accumulation buffer, by id.
 * GLX_SAMPLES_SGIS is what a pbuffer made from the config gets (the host
 * multisamples it); the table's own configs have none.
 */
static const int fb_exact[] = { GLX_LEVEL, GLX_DOUBLEBUFFER, GLX_STEREO, GLX_X_RENDERABLE,
	GLX_X_VISUAL_TYPE, GLX_CONFIG_CAVEAT, GLX_TRANSPARENT_TYPE, GLX_FBCONFIG_ID, 0 };
static const int fb_min[] = { GLX_BUFFER_SIZE, GLX_AUX_BUFFERS, GLX_RED_SIZE, GLX_GREEN_SIZE,
	GLX_BLUE_SIZE, GLX_ALPHA_SIZE, GLX_DEPTH_SIZE, GLX_STENCIL_SIZE, GLX_ACCUM_RED_SIZE,
	GLX_ACCUM_GREEN_SIZE, GLX_ACCUM_BLUE_SIZE, GLX_ACCUM_ALPHA_SIZE, 0 };

static int
fb_get(const struct hgl_fbcfg *c, int attr)
{
	int v = 0;

	(void)hgl_fbcfg_attr(c, attr, &v);
	return v;
}

static int
fb_fits(const struct hgl_fbcfg *c, const int *want)
{
	int i, k, v, a, found;

	/* Defaults: a window, RGBA, stereo off, level 0. */
	if ((fb_get(c, GLX_DRAWABLE_TYPE) & GLX_WINDOW_BIT) == 0 || (fb_get(c, GLX_RENDER_TYPE) & GLX_RGBA_BIT) == 0 ||
	    fb_get(c, GLX_STEREO) != 0 || fb_get(c, GLX_LEVEL) != 0) {
		/* ... unless the list says otherwise: checked below. */
		int dt = 0, rt = 0, st = 0, lv = 0;

		for (i = 0; want != NULL && want[i] != None; i += 2) {
			dt |= want[i] == GLX_DRAWABLE_TYPE;
			rt |= want[i] == GLX_RENDER_TYPE;
			st |= want[i] == GLX_STEREO;
			lv |= want[i] == GLX_LEVEL;
		}
		if ((!dt && (fb_get(c, GLX_DRAWABLE_TYPE) & GLX_WINDOW_BIT) == 0) ||
		    (!rt && (fb_get(c, GLX_RENDER_TYPE) & GLX_RGBA_BIT) == 0) ||
		    (!st && fb_get(c, GLX_STEREO) != 0) || (!lv && fb_get(c, GLX_LEVEL) != 0))
			return 0;
	}
	for (i = 0; want != NULL && want[i] != None; i += 2) {
		a = want[i];
		v = want[i + 1];
		if (v == (int)GLX_DONT_CARE || a == GLX_SAMPLES_SGIS || a == GLX_SAMPLE_BUFFERS_SGIS)
			continue;
		if (a == GLX_DRAWABLE_TYPE || a == GLX_RENDER_TYPE) {
			if ((fb_get(c, a) & v) != v)
				return 0;
			continue;
		}
		for (k = 0, found = 0; fb_exact[k] != 0; k++)
			if (fb_exact[k] == a) {
				if (fb_get(c, a) != v)
					return 0;
				found = 1;
			}
		for (k = 0; !found && fb_min[k] != 0; k++)
			if (fb_min[k] == a && fb_get(c, a) < v)
				return 0;
	}
	return 1;
}

static const int *fb_sort_want;
static Display *fb_sort_dpy;
static int fb_sort_screen;

/*
 * Whether host GL can show a window of this config: RGBA reaches the screen
 * as 24-bit pixels (TrueColor, depth 24), colour indices in 8- and 12-bit
 * windows and overlays. Those sort first: IMPACT's 12-bit RGBA configs have
 * the smallest buffers, so GLX's order put them first, and a program asking
 * for any RGBA config got a window nothing here could show.
 */
static int
fb_showable(const struct hgl_fbcfg *c)
{
	XVisualInfo tmpl, *vi;
	int n, ok, v = 0;

	if (!hgl_fbcfg_attr(c, GLX_VISUAL_ID, &v) || v == 0)
		return 0;
	tmpl.screen = fb_sort_screen;
	tmpl.visualid = (VisualID)v;
	if ((vi = XGetVisualInfo(fb_sort_dpy, VisualScreenMask | VisualIDMask, &tmpl, &n)) == NULL)
		return 0;
	if (fb_get(c, GLX_RENDER_TYPE) & GLX_RGBA_BIT)
		ok = vi->class == TrueColor && vi->depth == 24;
	else
		ok = vi->depth == 8 || vi->depth == 12;
	XFree(vi);
	return ok;
}

static int
fb_cmp(const void *pa, const void *pb)
{
	const struct hgl_fbcfg *a = *(const struct hgl_fbcfg * const *)pa, *b = *(const struct hgl_fbcfg * const *)pb;
	static const int colour[] = { GLX_RED_SIZE, GLX_GREEN_SIZE, GLX_BLUE_SIZE, GLX_ALPHA_SIZE };
	int ca = 0, cb = 0, i, k, d;

	if ((d = fb_showable(b) - fb_showable(a)) != 0)
		return d;
	if ((d = fb_get(a, GLX_CONFIG_CAVEAT) - fb_get(b, GLX_CONFIG_CAVEAT)) != 0)
		return d;
	for (k = 0; k < 4; k++)
		for (i = 0; fb_sort_want != NULL && fb_sort_want[i] != None; i += 2)
			if (fb_sort_want[i] == colour[k] && fb_sort_want[i + 1] != 0 && fb_sort_want[i + 1] != (int)GLX_DONT_CARE) {
				ca += fb_get(a, colour[k]);
				cb += fb_get(b, colour[k]);
			}
	if (ca != cb)
		return cb - ca;
	if ((d = fb_get(a, GLX_BUFFER_SIZE) - fb_get(b, GLX_BUFFER_SIZE)) != 0 ||
	    (d = fb_get(a, GLX_DOUBLEBUFFER) - fb_get(b, GLX_DOUBLEBUFFER)) != 0 ||
	    (d = fb_get(a, GLX_AUX_BUFFERS) - fb_get(b, GLX_AUX_BUFFERS)) != 0 ||
	    (d = fb_get(b, GLX_DEPTH_SIZE) - fb_get(a, GLX_DEPTH_SIZE)) != 0 ||
	    (d = fb_get(a, GLX_STENCIL_SIZE) - fb_get(b, GLX_STENCIL_SIZE)) != 0 ||
	    (d = fb_get(b, GLX_ACCUM_RED_SIZE) - fb_get(a, GLX_ACCUM_RED_SIZE)) != 0)
		return d;
	return fb_get(a, GLX_FBCONFIG_ID) - fb_get(b, GLX_FBCONFIG_ID);
}

/* The record for one of the server's FBConfigs (its visual's). */
static GLXFBConfigSGIX
config_of_fb(Display *dpy, int screen, const struct hgl_fbcfg *f)
{
	XVisualInfo tmpl, *vi;
	GLXFBConfigSGIX c;
	int i, n, v = 0;

	for (i = 0; i < nconfigs; i++)
		if (configs[i].fb == f)
			return &configs[i];
	if (!hgl_fbcfg_attr(f, GLX_VISUAL_ID, &v) || v == 0)
		return NULL;
	tmpl.screen = screen;
	tmpl.visualid = (VisualID)v;
	vi = XGetVisualInfo(dpy, VisualScreenMask | VisualIDMask, &tmpl, &n);
	if (vi == NULL)
		return NULL;
	c = config_for(dpy, vi);
	XFree(vi);
	if (c != NULL)
		c->fb = f;
	return c;
}

GLXFBConfigSGIX *
glXChooseFBConfigSGIX(Display *dpy, int screen, int *attrib_list, int *nitems)
{
	XVisualInfo tmpl, *vis;
	GLXFBConfigSGIX *out;
	const struct hgl_fbcfg *f, **pick;
	int n, i, m;

	int samples = 0;

	for (i = 0; attrib_list != NULL && attrib_list[i] != None; i += 2)
		if (attrib_list[i] == GLX_SAMPLES_SGIS)
			samples = attrib_list[i + 1];
	if ((f = hgl_glx_fbconfigs(dpy, screen, &n)) != NULL) {
		/* No attributes at all: every config, in the server's order, as
		 * SGI's library answers (its glxinfo lists them so). */
		if (attrib_list == NULL || attrib_list[0] == None)
			return (GLXFBConfigSGIX *)glXGetFBConfigs(dpy, screen, nitems);
		pick = malloc(n * sizeof *pick);
		out = malloc((n + 1) * sizeof *out);
		if (pick == NULL || out == NULL) {
			free(pick);
			free(out);
			*nitems = 0;
			return NULL;
		}
		for (i = m = 0; i < n; i++)
			if (fb_fits(&f[i], attrib_list))
				pick[m++] = &f[i];
		fb_sort_want = attrib_list;
		fb_sort_dpy = dpy;
		fb_sort_screen = screen;
		qsort(pick, m, sizeof *pick, fb_cmp);
		if (getenv("IRIS_GL_DEBUG") != NULL) {
			fprintf(stderr, "libGL: glXChooseFBConfig:");
			for (i = 0; attrib_list != NULL && attrib_list[i] != None && i < 64; i += 2)
				fprintf(stderr, " %#x=%#x", attrib_list[i], attrib_list[i + 1]);
			fprintf(stderr, " -> %d of %d\n", m, n);
		}
		for (i = n = 0; i < m; i++)
			if ((out[n] = config_of_fb(dpy, screen, pick[i])) != NULL) {
				out[n]->samples = samples;
				n++;
			}
		free(pick);
		*nitems = n;
		if (n == 0) {
			free(out);
			return NULL;
		}
		return out;
	}
	tmpl.screen = screen;
	tmpl.depth = 24;
	tmpl.class = TrueColor;
	vis = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n);
	if (vis == NULL || n == 0) {
		*nitems = 0;
		return NULL;
	}
	out = malloc(n * sizeof *out);
	for (i = 0; i < n; i++) {
		out[i] = config_for(dpy, &vis[i]);
		if (out[i] != NULL)
			out[i]->samples = samples;
	}
	XFree(vis);
	*nitems = n;
	return out;
}

GLXFBConfigSGIX
glXGetFBConfigFromVisualSGIX(Display *dpy, XVisualInfo *vis)
{
	return config_for(dpy, vis);
}

XVisualInfo *
glXGetVisualFromFBConfigSGIX(Display *dpy, GLXFBConfigSGIX config)
{
	XVisualInfo tmpl;
	int n;

	if (config == NULL)
		return NULL;
	tmpl.visualid = config->vi.visualid;
	return XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
}

int
glXGetFBConfigAttribSGIX(Display *dpy, GLXFBConfigSGIX config, int attribute, int *value)
{
	if (config == NULL)
		return GLX_BAD_VISUAL;
	/* The server's answer, but a pbuffer config's samples are ours. */
	if (config->fb != NULL && attribute != GLX_SAMPLES_SGIS && attribute != GLX_SAMPLE_BUFFERS_SGIS) {
		if (hgl_fbcfg_attr(config->fb, attribute, value))
			return 0;
		if (attribute == GLX_RGBA) {
			*value = (fb_get(config->fb, GLX_RENDER_TYPE) & GLX_RGBA_BIT) != 0;
			return 0;
		}
		if (attribute == GLX_USE_GL) {
			*value = 1;
			return 0;
		}
	}
	switch (attribute) {
	case GLX_FBCONFIG_ID_SGIX:
		*value = (int)config->vi.visualid;
		return 0;
	case GLX_DRAWABLE_TYPE_SGIX:
		*value = GLX_WINDOW_BIT_SGIX | GLX_PIXMAP_BIT_SGIX | GLX_PBUFFER_BIT_SGIX;
		return 0;
	case GLX_RENDER_TYPE_SGIX:
		*value = GLX_RGBA_BIT_SGIX;
		return 0;
	case GLX_MAX_PBUFFER_WIDTH_SGIX:
	case GLX_MAX_PBUFFER_HEIGHT_SGIX:
		*value = 4096;
		return 0;
	case GLX_MAX_PBUFFER_PIXELS_SGIX:
		*value = 4096 * 4096;
		return 0;
	case GLX_SAMPLES_SGIS:
		*value = config->samples;
		return 0;
	case GLX_SAMPLE_BUFFERS_SGIS:
		*value = config->samples > 1;
		return 0;
	}
	return glXGetConfig(dpy, &config->vi, attribute, value);
}

GLXContext
glXCreateContextWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, int render_type, GLXContext share, Bool direct)
{
	(void)render_type;
	if (config == NULL)
		return NULL;
	return glXCreateContext(dpy, &config->vi, share, direct);
}

/* GLX_SGIX_pbuffer: drawing with no window, into memory of the host's. */
GLXPbufferSGIX
glXCreateGLXPbufferSGIX(Display *dpy, GLXFBConfigSGIX config, unsigned int width, unsigned int height, int *attrib_list)
{
	hgl_slot a[3];
	GLXDrawable id;
	long long r;
	int i;

	(void)dpy;
	(void)attrib_list;
	hgl_flush_nowait();
	a[0].i = width;
	a[1].i = height;
	a[2].i = config != NULL ? config->samples : 0;
	r = hgl_call(HGL_GL_PBUFFER, a);
	if (r <= 0)
		return None;
	id = (GLXDrawable)(unsigned)r;
	for (i = 0; i < HGL_PBUFFERS; i++)
		if (pbuffers[i].id == None) {
			pbuffers[i].id = id;
			pbuffers[i].w = width;
			pbuffers[i].h = height;
			return id;
		}
	return None;
}

void
glXDestroyGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf)
{
	hgl_slot a[1];
	int p = pbuffer_of(pbuf);

	(void)dpy;
	if (p < 0)
		return;
	hgl_flush_nowait();
	a[0].i = pbuf;
	hgl_call(HGL_GL_DRAWABLE_GONE, a);
	pbuffers[p].id = None;
}

int
glXQueryGLXPbufferSGIX(Display *dpy, GLXPbufferSGIX pbuf, int attribute, unsigned int *value)
{
	int p = pbuffer_of(pbuf);

	(void)dpy;
	if (p < 0)
		return GLX_BAD_VALUE;
	switch (attribute) {
	case GLX_WIDTH_SGIX: *value = pbuffers[p].w; return 0;
	case GLX_HEIGHT_SGIX: *value = pbuffers[p].h; return 0;
	case GLX_PRESERVED_CONTENTS_SGIX: *value = True; return 0;
	case GLX_LARGEST_PBUFFER_SGIX: *value = False; return 0;
	case GLX_FBCONFIG_ID_SGIX: *value = 0; return 0;
	}
	return GLX_BAD_ATTRIBUTE;
}

/*
 * GLX_SGI_swap_control and GLX_SGI_video_sync.
 *
 * Interval 0 -- "do not wait for the vertical retrace" -- is how a program
 * says it does not want the display to pace it, and IRIS takes it at its
 * word: the host hands the finished frame to a thread of its own and the
 * swap returns without waiting for the frame to reach the screen (at most two
 * frames in flight; see iris-hostgl/src/present.rs). Any other interval waits
 * for the frame, which is what every program written before this expects, so
 * that is also what a program that never calls this gets.
 *
 * IRIS_GL_SWAP overrides both ways for trying it out: `async` makes every
 * swap of this process asynchronous whatever it asks for, `sync` refuses to
 * make any of them asynchronous. The environment wins over the program.
 */
#define SWAP_DEFAULT 0
#define SWAP_ASYNC   1
#define SWAP_SYNC    2
static int swap_override = -1;

static int
swap_mode(void)
{
	const char *e;

	if (swap_override < 0) {
		e = getenv("IRIS_GL_SWAP");
		if (e != NULL && strcmp(e, "async") == 0)
			swap_override = SWAP_ASYNC;
		else if (e != NULL && strcmp(e, "sync") == 0)
			swap_override = SWAP_SYNC;
		else {
			if (e != NULL && *e != 0)
				fprintf(stderr, "libGL: IRIS_GL_SWAP=%s is neither async nor sync; "
				    "leaving the swap to the program\n", e);
			swap_override = SWAP_DEFAULT;
		}
	}
	return swap_override;
}

/* The interval the host is told, once the environment has had its say. */
static int
swap_interval_wanted(int interval)
{
	switch (swap_mode()) {
	case SWAP_ASYNC:
		return 0;
	case SWAP_SYNC:
		return interval < 1 ? 1 : interval;
	default:
		return interval;
	}
}

/*
 * Tell the host what this process's swaps should do, when the environment
 * asked for something other than the default. Called once the host knows this
 * client, before any swap.
 */
void
hgl_swap_setup(void)
{
	hgl_slot a[1];

	if (swap_mode() == SWAP_DEFAULT)
		return;
	a[0].i = swap_interval_wanted(1);
	hgl_call(HGL_GL_SWAP_INTERVAL, a);
}

int
glXSwapIntervalSGI(int interval)
{
	hgl_slot a[1];

	if (interval < 0)
		return GLX_BAD_VALUE;
	a[0].i = swap_interval_wanted(interval);
	hgl_call(HGL_GL_SWAP_INTERVAL, a);
	return 0;
}

int
glXGetVideoSyncSGI(unsigned int *count)
{
	hgl_slot a[3];
	long long r;

	a[0].i = 0;
	a[1].i = 0;
	a[2].i = 0;
	if ((r = hgl_call(HGL_GL_VIDEO_SYNC, a)) < 0)
		return GLX_BAD_CONTEXT;
	*count = (unsigned)r;
	return 0;
}

int
glXWaitVideoSyncSGI(int divisor, int remainder, unsigned int *count)
{
	hgl_slot a[3];
	long long r, wait = 0;

	if (divisor <= 0 || remainder < 0)
		return GLX_BAD_VALUE;
	hgl_flush_nowait();
	a[0].i = 1;
	a[1].i = divisor;
	a[2].i = remainder;
	/*
	 * The host answers at once with the count the wait will reach and how
	 * long to wait for it: the emulator's CPU must not stop while this
	 * process sleeps, so the sleep is here.
	 */
	if ((r = hgl_call2(HGL_GL_VIDEO_SYNC, a, &wait)) < 0)
		return GLX_BAD_CONTEXT;
	hgl_wait(wait);
	*count = (unsigned)r;
	return 0;
}

/* GLX_SGIX_swap_group: one machine, so the group is what the program says. */
void
glXJoinSwapGroupSGIX(Display *dpy, GLXDrawable drawable, GLXDrawable member)
{
	(void)dpy;
	(void)drawable;
	(void)member;
}

static const char *
glx_string(int which)
{
	static char strings[3][1024];
	hgl_slot a[3];

	if (which < 0 || which > 2)
		return "";
	if (strings[which][0] == 0) {
		a[0].i = which;
		a[1].i = (long)strings[which];
		a[2].i = sizeof strings[which];
		if (hgl_call(HGL_GL_GLX_STRING, a) != 0)
			return "";
	}
	return strings[which];
}

const char *
glXQueryExtensionsString(Display *dpy, int screen)
{
	(void)dpy;
	(void)screen;
	return glx_string(0);
}

/*
 * The vendor and version strings are the X server's, as SGI's library gives
 * them (SGI, 1.3 Irix 6.5 on IMPACT): this library implements GLX 1.3. The
 * extension list stays this library's own -- the server's, less
 * GLX_EXT_import_context (a context of another client cannot be imported
 * here), plus GLX_SGIS_multisample (pbuffers are multisampled on the host).
 */
static const char *
glx_client_string(Display *dpy, int screen, int name)
{
	const char *s;

	if (name != GLX_EXTENSIONS && (s = hgl_glx_server_string(dpy, screen, name == GLX_VENDOR ? 1 : 2)) != NULL)
		return s;
	return glx_string(name == GLX_VERSION ? 2 : name == GLX_EXTENSIONS ? 0 : 1);
}

const char *
glXGetClientString(Display *dpy, int name)
{
	return glx_client_string(dpy, dpy != NULL ? DefaultScreen(dpy) : 0, name);
}

const char *
glXQueryServerString(Display *dpy, int screen, int name)
{
	return glx_client_string(dpy, screen, name);
}


GLXContext
glXGetCurrentContext(void)
{
	return cur_ctx;
}

GLXDrawable
glXGetCurrentDrawable(void)
{
	return cur_draw;
}

Display *
glXGetCurrentDisplay(void)
{
	return cur_dpy;
}

Bool
glXIsDirect(Display *dpy, GLXContext ctx)
{
	(void)dpy;
	(void)ctx;
	return True;
}

void
glXWaitGL(void)
{
	hgl_slot a[1];

	hgl_flush_nowait();
	a[0].i = 0;
	hgl_call(HGL_GL_FINISH, a);
	present_glxpixmap();
}

void
glXWaitX(void)
{
}

/*
 * Whether nothing covers a window, for compositing. The host composites a
 * frame wherever the screen shows the window's window ID, and every RGB
 * window of a visual shares one: where another such window lies on top, the
 * frame would be painted over it. So a frame is composited only while the
 * window is unobscured, and otherwise goes through XPutImage, which the X
 * server clips.
 *
 * VisibilityNotify keeps the state, read on a connection of our own so the
 * program's events stay the program's. Selecting it sends nothing for the
 * state the window is already in, so the first answer is worked out from
 * the stacking order: any viewable top-level window above ours that overlaps
 * it.
 */
#define VIS_MAX 16
static Display *vis_dpy;
static struct {
	Window w;
	int state;              /* a Visibility* value, or -1: not known */
	/* Where the window is, for presents made by a flush (see window_where):
	 * valid until the window or its top-level ancestor (a window manager's
	 * frame, watched too) is configured, mapped, unmapped, reparented or
	 * destroyed. */
	Window top;
	int geo_valid, width, height, depth, x, y, geo_viewable, geo_placed;
	Visual *visual;
} vis[VIS_MAX];

static int
overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Worked out from the stacking order, for a window that is viewable. */
static int
visibility_now(Display *d, Window w)
{
	Window root, parent, *kids = NULL, top = w, child;
	unsigned int n, i;
	XWindowAttributes wa, sa;
	int x, y, state = VisibilityUnobscured;

	if (!XGetWindowAttributes(d, w, &wa))
		return -1;
	root = wa.root;
	if (!XTranslateCoordinates(d, w, root, 0, 0, &x, &y, &child))
		return -1;
	/* The window's ancestor among the root's children: a window manager's
	 * frame, or the window itself. */
	for (;;) {
		if (!XQueryTree(d, top, &root, &parent, &kids, &n))
			return -1;
		if (kids)
			XFree(kids);
		if (parent == root)
			break;
		top = parent;
	}
	if (!XQueryTree(d, root, &root, &parent, &kids, &n))
		return -1;
	/* Children come bottom to top: the ones after ours are above it. */
	for (i = 0; i < n && kids[i] != top; i++)
		;
	for (i++; i < n; i++) {
		if (!XGetWindowAttributes(d, kids[i], &sa) || sa.map_state != IsViewable ||
		    sa.class != InputOutput)
			continue;
		if (overlaps(x, y, wa.width, wa.height, sa.x, sa.y,
		    sa.width + 2 * sa.border_width, sa.height + 2 * sa.border_width)) {
			state = VisibilityPartiallyObscured;
			break;
		}
	}
	if (kids)
		XFree(kids);
	return state;
}

static int
unobscured(Display *dpy, Window w)
{
	XEvent e;
	int i, free_slot = -1;

	if (vis_dpy == NULL) {
		vis_dpy = XOpenDisplay(DisplayString(dpy));
		if (vis_dpy == NULL)
			return 0;
	}
	while (XPending(vis_dpy)) {
		XNextEvent(vis_dpy, &e);
		for (i = 0; i < VIS_MAX; i++) {
			if (vis[i].w == 0 || vis[i].w != e.xany.window)
				continue;
			if (e.type == VisibilityNotify)
				vis[i].state = e.xvisibility.state;
			else if (e.type == DestroyNotify)
				vis[i].w = 0;
			else if (e.type == UnmapNotify)
				vis[i].state = -1;
			if (e.type != VisibilityNotify)
				vis[i].geo_valid = 0;
		}
		/* The frame around it: moved, the window is somewhere else. */
		for (i = 0; i < VIS_MAX; i++)
			if (vis[i].w != 0 && vis[i].top != 0 && vis[i].top == e.xany.window)
				vis[i].geo_valid = 0;
	}
	for (i = 0; i < VIS_MAX; i++) {
		if (vis[i].w == w)
			break;
		if (vis[i].w == 0 && free_slot < 0)
			free_slot = i;
	}
	if (i == VIS_MAX) {
		if (free_slot < 0)
			return 0;
		i = free_slot;
		vis[i].w = w;
		vis[i].state = -1;
		vis[i].top = 0;
		vis[i].geo_valid = 0;
		XSelectInput(vis_dpy, w, VisibilityChangeMask | StructureNotifyMask);
		XSync(vis_dpy, False);
	}
	if (vis[i].state < 0)
		vis[i].state = visibility_now(vis_dpy, w);
	return vis[i].state == VisibilityUnobscured;
}

/*
 * A flush's present skips asking the X server where the window is. SGI's IRIS
 * GL flushes after every primitive it draws into the front buffer -- flight's
 * cockpit is thousands of flushes -- and each present asked twice
 * (XGetWindowAttributes, XTranslateCoordinates), round trips to an X server
 * on the emulated machine. What the last present learned stays good until an
 * event on our own connection (see unobscured) says the window or its frame
 * changed. A swap always asks.
 */
static int
window_where(Display *dpy, Window w, int cached, struct where *g)
{
	XWindowAttributes wa;
	Window root, parent, *kids, child, top;
	unsigned int n;
	int i;

	g->unobscured = 0;
	for (i = 0; i < VIS_MAX && vis[i].w != w; i++)
		;
	if (cached && vis_dpy != NULL && i < VIS_MAX) {
		/* Takes the pending events first, which may say it moved. */
		g->unobscured = vis[i].geo_viewable && unobscured(dpy, w);
		if (vis[i].geo_valid && vis[i].w == w) {
			g->width = vis[i].width;
			g->height = vis[i].height;
			g->depth = vis[i].depth;
			g->visual = vis[i].visual;
			g->viewable = vis[i].geo_viewable;
			g->placed = vis[i].geo_placed;
			g->x = vis[i].x;
			g->y = vis[i].y;
			return 1;
		}
	}
	if (!XGetWindowAttributes(dpy, w, &wa))
		return 0;
	g->width = wa.width;
	g->height = wa.height;
	g->depth = wa.depth;
	g->visual = wa.visual;
	g->viewable = wa.map_state == IsViewable;
	g->unobscured = g->viewable && unobscured(dpy, w);
	/*
	 * Only a window that is on the screen now: XTranslateCoordinates also
	 * answers for one not mapped yet (or iconified), and the host would
	 * paint the frame over whatever is really there -- atlantis's first
	 * frames landed at the default 500,500 on top of another window, before
	 * the window manager had placed it, and stayed there.
	 */
	g->placed = g->viewable &&
	    XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &g->x, &g->y, &child) != 0;
	if (!g->placed)
		g->x = g->y = 0;
	for (i = 0; i < VIS_MAX && vis[i].w != w; i++)
		;
	if (vis_dpy == NULL || i == VIS_MAX)
		return 1;       /* nothing watches it: nothing kept */
	if (vis[i].top == 0) {
		/* Its ancestor among the root's children: the window manager's frame. */
		for (top = w;; top = parent) {
			if (!XQueryTree(vis_dpy, top, &root, &parent, &kids, &n))
				return 1;
			if (kids)
				XFree(kids);
			if (parent == root || parent == None)
				break;
		}
		vis[i].top = top;
		if (top != w) {
			XSelectInput(vis_dpy, top, StructureNotifyMask);
			XSync(vis_dpy, False);
		}
	}
	vis[i].width = g->width;
	vis[i].height = g->height;
	vis[i].depth = g->depth;
	vis[i].visual = g->visual;
	vis[i].geo_viewable = g->viewable;
	vis[i].geo_placed = g->placed;
	vis[i].x = g->x;
	vis[i].y = g->y;
	vis[i].geo_valid = 1;
	return 1;
}

static void present_window(Display *dpy, GLXDrawable d, int front);

/*
 * Whether a window shows colour indices: its visual is a colour map one of 8
 * or 12 bits. Asked of the server once a window (a few windows kept).
 */
#define HGL_WINKINDS 16
static struct {
	Window id;
	int index;
} winkind[HGL_WINKINDS];
static int winkind_next;

static int
index_window(Display *dpy, GLXDrawable d)
{
	XWindowAttributes wa;
	int i, index;

	if (d == None || glxpixmap_of(d) >= 0 || pbuffer_of(d) >= 0)
		return 0;
	for (i = 0; i < HGL_WINKINDS; i++)
		if (winkind[i].id == d)
			return winkind[i].index;
	index = XGetWindowAttributes(dpy, d, &wa) && wa.depth <= 12 &&
	    (wa.visual->class == PseudoColor || wa.visual->class == StaticColor ||
	    wa.visual->class == GrayScale);
	winkind[winkind_next].id = d;
	winkind[winkind_next].index = index;
	winkind_next = (winkind_next + 1) % HGL_WINKINDS;
	return index;
}

/*
 * A swap. A single-buffered context's drawing is its front buffer, which a
 * swap leaves as it is: shown as it stands, when the host keeps it apart.
 */
void
glXSwapBuffers(Display *dpy, GLXDrawable d)
{
	static int swap_debug = -1, swaps_said;

	/* What was drawn into the front buffer goes out with this frame. */
	(void)hgl_front_drawn(1);
	if (swap_debug < 0)
		swap_debug = getenv("IRIS_GL_DEBUG") != NULL;
	if (swap_debug && swaps_said < 12) {
		swaps_said++;
		fprintf(stderr, "libGL: glXSwapBuffers(0x%lx)%s%s\n", (unsigned long)d,
		    index_window(dpy, d) ? ", colour index" : "",
		    d != cur_draw ? ", not the current drawable" : "");
	}
	/* The window's kind, not the current context's: SGI's IRIS GL swaps its
	 * colour-index window while an RGBA context is current, and a 24-bit
	 * frame put to it was refused (BadMatch). */
	if (index_window(dpy, d)) {
		present_index(dpy, d, 0);
		return;
	}
	/* A colour-index context's GLX pixmap or pbuffer: nothing to show, as
	 * before. */
	if (hgl_index_mode && (glxpixmap_of(d) >= 0 || pbuffer_of(d) >= 0))
		return;
	present_window(dpy, d, hgl_fronts && hgl_single_now());
}

/*
 * For libirisgl, whose frontbuffer() draws into the front buffer when the
 * host keeps one (and swaps, as before, when it does not).
 */
int
hgl_front_buffers(void)
{
	return hgl_fronts;
}

/* Front-buffer drawing on the screen: the front buffer as it is, no swap. */
void
hgl_present_front(Display *dpy, GLXDrawable d)
{
	static int debug = -1, said;

	if (debug < 0)
		debug = getenv("IRIS_GL_DEBUG") != NULL;
	if (debug && said < 12) {
		said++;
		fprintf(stderr, "libGL: front buffer of 0x%lx shown\n", (unsigned long)d);
	}
	present_window(dpy, d, 1);
}

/* A window's back buffer (front 0) or front buffer (front 1) to the screen. */
static void
present_window(Display *dpy, GLXDrawable d, int front)
{
	static int force_xputimage = -1, said_placed = -1;
	static GLXDrawable said_draw, took;     /* took: see present_flush */
	hgl_slot a[7];
	long long presented, wait = 0;
	int w, h, x, y, placed, viewable;
	struct where g;

	/* A front buffer is shown by flushes: as often as the program draws. */
	if (!window_where(dpy, d, front, &g))
		return;
	w = g.width;
	h = g.height;
	viewable = g.viewable;
	if (frame == NULL || w != frame_w || h != frame_h || d != frame_draw) {
		frame_free();
		if (frame_shm < 0)
			frame_shm = getenv("IRIS_HOSTGL_NOSHM") == NULL && XShmQueryExtension(dpy);
		frame_dpy = dpy;
		if (frame_shm == 1 && !frame_alloc_shm(dpy, w, h))
			frame_shm = 0;
		if (frame_shm != 1) {
			frame = malloc((size_t)w * h * 4);
			frame_image = XCreateImage(dpy, true_visual(dpy), 24, ZPixmap, 0,
			    frame, w, h, 32, w * 4);
		}
		if (frame_gc == NULL)
			frame_gc = XCreateGC(dpy, d, 0, NULL);
		frame_w = w;
		frame_h = h;
		frame_draw = d;
	}
	hgl_flush_nowait();
	a[0].i = (long)frame;
	a[1].i = w;
	a[2].i = h;
	/* The frame's pixel order: the visual's, red high or (IMPACT) low. */
	a[3].i = true_visual(dpy)->red_mask == 0xff;
	/*
	 * Where the window is on the screen, so a host that composites into the
	 * emulated framebuffer can put the frame there itself and skip the
	 * readback and XPutImage below. IRIS_HOSTGL_XPUTIMAGE=1 keeps every frame
	 * on the XPutImage path.
	 */
	if (force_xputimage < 0)
		force_xputimage = getenv("IRIS_HOSTGL_XPUTIMAGE") != NULL;
	/* Only a window on the screen that nothing covers (see window_where). */
	placed = !force_xputimage && g.placed && g.unobscured;
	x = placed ? g.x : 0;
	y = placed ? g.y : 0;
	a[4].i = x;
	a[5].i = y;
	/* Bit 1: show the front buffer as it is (HGL_GL_FRONT_BUFFERS). */
	a[6].i = placed | (front ? 2 : 0);
	if ((d != said_draw || placed != said_placed) && getenv("IRIS_GL_DEBUG") != NULL)
		fprintf(stderr, "libGL: window 0x%lx: %s\n", (unsigned long)d,
		    placed ? "composited" : !viewable ? "not viewable: XPutImage" :
		    force_xputimage ? "XPutImage (IRIS_HOSTGL_XPUTIMAGE)" : "covered: XPutImage");
	said_draw = d;
	said_placed = placed;
	/*
	 * Nonzero: the host put the frame into the window itself, or failed.
	 * `wait` is what GLX_SGI_swap_control asks this swap to wait -- slept
	 * here, not on the host, so the emulated machine keeps running.
	 */
	if (front && present_flush(a, d, took))
		return;
	presented = hgl_call2(HGL_GL_SWAP, a, &wait);
	took = presented != 0 && placed ? d : None;
	hgl_wait(wait);
	if (presented != 0)
		return;
	if (frame_shm == 1)
		XShmPutImage(dpy, d, frame_gc, frame_image, 0, 0, 0, 0, w, h, False);
	else
		XPutImage(dpy, d, frame_gc, frame_image, 0, 0, 0, 0, w, h);
	XFlush(dpy);
}

const GLubyte *
glGetString(GLenum name)
{
	/* The extension list is the long one: hundreds of bytes, not tens. */
	static char strings[4][4096];
	hgl_slot a[3];
	int k = name == GL_VENDOR ? 0 : name == GL_RENDERER ? 1 : name == GL_VERSION ? 2 : 3;

	hgl_flush_nowait();
	a[0].i = name;
	a[1].i = (long)strings[k];
	a[2].i = sizeof strings[k];
	if (hgl_call(HGL_GL_GETSTRING, a) != 0)
		return NULL;
	return (const GLubyte *)strings[k];
}

/* ---- GLX 1.3, and the rest of what SGI's libGL.so exports ----
 *
 * A program that names any entry point SGI's library has and this one
 * lacks is refused by rld before main ("unresolvable symbol"): vrp2DO2
 * died on glXGetCurrentDisplayEXT. GLX 1.3's FBConfigs are the SGIX ones,
 * its pbuffers and pixmaps the SGIX and 1.0 ones. Channels, hyperpipes,
 * swap barriers and video sources are SGI hardware there is none of here;
 * they answer as a machine without them does.
 */
GLXFBConfig *
glXChooseFBConfig(Display *dpy, int screen, int *attribList, int *nitems)
{
	return glXChooseFBConfigSGIX(dpy, screen, attribList, nitems);
}

/* Every FBConfig the server has (glxinfo lists these), in its order. */
GLXFBConfig *
glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
	const struct hgl_fbcfg *f;
	GLXFBConfig *out;
	int i, n, m;

	if ((f = hgl_glx_fbconfigs(dpy, screen, &n)) == NULL)
		return glXChooseFBConfigSGIX(dpy, screen, NULL, nelements);
	if ((out = malloc((n + 1) * sizeof *out)) == NULL) {
		*nelements = 0;
		return NULL;
	}
	for (i = m = 0; i < n; i++)
		if ((out[m] = config_of_fb(dpy, screen, &f[i])) != NULL)
			m++;
	*nelements = m;
	return out;
}

int
glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
	return glXGetFBConfigAttribSGIX(dpy, config, attribute, value);
}

XVisualInfo *
glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
	return glXGetVisualFromFBConfigSGIX(dpy, config);
}

GLXPixmap
glXCreateGLXPixmapWithConfigSGIX(Display *dpy, GLXFBConfigSGIX config, Pixmap pixmap)
{
	return config != NULL ? glXCreateGLXPixmap(dpy, &config->vi, pixmap) : None;
}

GLXPixmap
glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, int *attrib_list)
{
	(void)attrib_list;
	return glXCreateGLXPixmapWithConfigSGIX(dpy, config, pixmap);
}

void
glXDestroyPixmap(Display *dpy, GLXPixmap pix)
{
	glXDestroyGLXPixmap(dpy, pix);
}

/* A GLX window is the X window itself here. */
GLXWindow
glXCreateWindow(Display *dpy, GLXFBConfig config, Window win, int *attrib_list)
{
	(void)dpy;
	(void)config;
	(void)attrib_list;
	return win;
}

void
glXDestroyWindow(Display *dpy, GLXWindow win)
{
	(void)dpy;
	(void)win;
}

GLXPbuffer
glXCreatePbuffer(Display *dpy, GLXFBConfig config, int *attrib_list)
{
	unsigned int w = 0, h = 0;
	int i;

	for (i = 0; attrib_list != NULL && attrib_list[i] != None; i += 2)
		if (attrib_list[i] == GLX_PBUFFER_WIDTH)
			w = (unsigned)attrib_list[i + 1];
		else if (attrib_list[i] == GLX_PBUFFER_HEIGHT)
			h = (unsigned)attrib_list[i + 1];
	return glXCreateGLXPbufferSGIX(dpy, config, w, h, NULL);
}

void
glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf)
{
	glXDestroyGLXPbufferSGIX(dpy, pbuf);
}

void
glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{
	Window root;
	int x, y;
	unsigned int w, h, bw, depth;

	if (value == NULL)
		return;
	if (pbuffer_of(draw) >= 0) {
		glXQueryGLXPbufferSGIX(dpy, draw, attribute == GLX_WIDTH ? GLX_WIDTH_SGIX :
		    attribute == GLX_HEIGHT ? GLX_HEIGHT_SGIX : attribute, value);
		return;
	}
	*value = 0;
	if ((attribute == GLX_WIDTH || attribute == GLX_HEIGHT) &&
	    XGetGeometry(dpy, draw, &root, &x, &y, &w, &h, &bw, &depth))
		*value = attribute == GLX_WIDTH ? w : h;
}

GLXContext
glXCreateNewContext(Display *dpy, GLXFBConfig config, int render_type, GLXContext share_list, Bool direct)
{
	return glXCreateContextWithConfigSGIX(dpy, config, render_type, share_list, direct);
}

Bool
glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext gc)
{
	return make_current(dpy, draw, read, gc);
}

GLXDrawable
glXGetCurrentReadDrawable(void)
{
	return glXGetCurrentReadDrawableSGI();
}

int
glXQueryContext(Display *dpy, GLXContext ctx, int attribute, int *value)
{
	int i = ctxinfo_of(ctx);

	(void)dpy;
	if (i < 0)
		return GLX_BAD_CONTEXT;
	switch (attribute) {
	case GLX_FBCONFIG_ID: *value = (int)ctxinfo[i].visual; return Success;
	case GLX_RENDER_TYPE: *value = is_index_ctx(ctx) ? GLX_COLOR_INDEX_TYPE : GLX_RGBA_TYPE; return Success;
	case GLX_SCREEN: *value = ctxinfo[i].screen; return Success;
	}
	return GLX_BAD_ATTRIBUTE;
}

/* GLX_EXT_import_context: a context can't be shared with another
 * process here, but it can be asked about. */
int
glXQueryContextInfoEXT(Display *dpy, GLXContext ctx, int attribute, int *value)
{
	int i = ctxinfo_of(ctx);

	(void)dpy;
	if (i < 0)
		return GLX_BAD_CONTEXT;
	switch (attribute) {
	case GLX_SHARE_CONTEXT_EXT: *value = (int)(long)(char *)ctxinfo[i].share; return Success;
	case GLX_VISUAL_ID_EXT: *value = (int)ctxinfo[i].visual; return Success;
	case GLX_SCREEN_EXT: *value = ctxinfo[i].screen; return Success;
	}
	return GLX_BAD_ATTRIBUTE;
}

Display *glXGetCurrentDisplayEXT(void) { return glXGetCurrentDisplay(); }
GLXDrawable glXGetCurrentDrawableEXT(void) { return glXGetCurrentDrawable(); }
GLXContextID glXGetContextIDEXT(const GLXContext gc) { return (GLXContextID)(long)(char *)gc; }
GLXContext glXImportContextEXT(Display *dpy, GLXContextID id) { (void)dpy; (void)id; return NULL; }
void glXFreeContextEXT(Display *dpy, GLXContext gc) { (void)dpy; (void)gc; }

/* glXCopyContext: the host copies nothing between contexts yet. */
#ifdef GLX_VERSION_1_3
#define HGL_COPY_MASK unsigned long
#else
#define HGL_COPY_MASK GLuint	/* 6.5.7's glx.h; the same size */
#endif
void
glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, HGL_COPY_MASK mask)
{
	(void)dpy; (void)src; (void)dst; (void)mask;
}

/* GLX events (pbuffer clobber): none are sent, so the masks are kept to
 * answer with. */
static unsigned long glx_event_mask;

void glXSelectEvent(Display *dpy, GLXDrawable d, unsigned long m) { (void)dpy; (void)d; glx_event_mask = m; }
void glXGetSelectedEvent(Display *dpy, GLXDrawable d, unsigned long *m) { (void)dpy; (void)d; if (m) *m = glx_event_mask; }
void glXSelectEventSGIX(Display *dpy, GLXDrawable d, unsigned long m) { glXSelectEvent(dpy, d, m); }
void glXGetSelectedEventSGIX(Display *dpy, GLXDrawable d, unsigned long *m) { glXGetSelectedEvent(dpy, d, m); }

/* glXGetProcAddress: any entry point this library has. */
void (*glXGetProcAddress(const GLubyte *name))(void)
{
	static void *self;

	if (name == NULL)
		return NULL;
	if (self == NULL)
		self = dlopen(NULL, RTLD_LAZY);
	return self != NULL ? (void (*)(void))dlsym(self, (const char *)name) : NULL;
}

void (*glXGetProcAddressARB(const GLubyte *name))(void)
{
	return glXGetProcAddress(name);
}

/* SGI hardware that isn't here. */
int glXBindChannelToWindowSGIX(Display *d, int s, int c, Window w) { (void)d; (void)s; (void)c; (void)w; return 0; }
int glXQueryChannelDeltasSGIX(Display *d, int s, int c, int *x, int *y, int *w, int *h) { (void)d; (void)s; (void)c; (void)x; (void)y; (void)w; (void)h; return 0; }
int glXChannelRectSGIX(Display *d, int s, int c, int x, int y, int w, int h) { (void)d; (void)s; (void)c; (void)x; (void)y; (void)w; (void)h; return 0; }
int glXQueryChannelRectSGIX(Display *d, int s, int c, int *x, int *y, int *w, int *h) { (void)d; (void)s; (void)c; (void)x; (void)y; (void)w; (void)h; return 0; }
int glXChannelRectSyncSGIX(Display *d, int s, int c, GLenum t) { (void)d; (void)s; (void)c; (void)t; return 0; }
GLXHyperpipeNetworkSGIX *glXQueryHyperpipeNetworkSGIX(Display *d, int *n) { (void)d; if (n) *n = 0; return NULL; }
int glXHyperpipeConfigSGIX(Display *d, int net, int n, GLXHyperpipeConfigSGIX *cfg, int *id) { (void)d; (void)net; (void)n; (void)cfg; (void)id; return GLX_BAD_HYPERPIPE_CONFIG_SGIX; }
int glXDestroyHyperpipeConfigSGIX(Display *d, int id) { (void)d; (void)id; return GLX_BAD_HYPERPIPE_SGIX; }
GLXHyperpipeConfigSGIX *glXQueryHyperpipeConfigSGIX(Display *d, int id, int *n) { (void)d; (void)id; if (n) *n = 0; return NULL; }
int glXBindHyperpipeSGIX(Display *d, int id) { (void)d; (void)id; return GLX_BAD_HYPERPIPE_SGIX; }
int glXQueryHyperpipeBestAttribSGIX(Display *d, int t, int a, int s, void *l, void *r) { (void)d; (void)t; (void)a; (void)s; (void)l; (void)r; return GLX_BAD_HYPERPIPE_SGIX; }
int glXQueryHyperpipeAttribSGIX(Display *d, int t, int a, int s, void *r) { (void)d; (void)t; (void)a; (void)s; (void)r; return GLX_BAD_HYPERPIPE_SGIX; }
int glXHyperpipeAttribSGIX(Display *d, int t, int a, int s, void *l) { (void)d; (void)t; (void)a; (void)s; (void)l; return GLX_BAD_HYPERPIPE_SGIX; }
void glXBindSwapBarrierSGIX(Display *d, GLXDrawable dr, int b) { (void)d; (void)dr; (void)b; }
Bool glXQueryMaxSwapBarriersSGIX(Display *d, int s, int *max) { (void)d; (void)s; if (max) *max = 0; return False; }
/* Their prototypes need the video and digital-media headers, which this
 * file doesn't include: a VLServer and DMparams/DMbuffer are pointers,
 * VLPath and VLNode ints. */
GLXVideoSourceSGIX glXCreateGLXVideoSourceSGIX(Display *d, int s, void *svr, int path, int nc, int node) { (void)d; (void)s; (void)svr; (void)path; (void)nc; (void)node; return None; }
void glXDestroyGLXVideoSourceSGIX(Display *d, GLXVideoSourceSGIX v) { (void)d; (void)v; }
/* An O2 digital-media pbuffer shares its pixels with a DMbuffer. Here a
 * pbuffer is the host's, and the DMbuffer stays apart: what the program
 * draws and reads back through GL works (vrp2DO2 does just that), what it
 * reads from the DMbuffer directly does not. */
Bool glXAssociateDMPbufferSGIX(Display *d, GLXPbufferSGIX p, void *params, void *buf) { (void)d; (void)params; (void)buf; return pbuffer_of(p) >= 0; }
/* SGI-internal float variants (no glx.h declares them): the float
 * attributes are ignored, and none are answered. */
GLXFBConfigSGIX *glXChooseFBConfigWithFltSGIX(Display *d, int s, int *a, float *fa, int *n) { (void)fa; return glXChooseFBConfigSGIX(d, s, a, n); }
int glXGetFBConfigFltAttribSGIX(Display *d, GLXFBConfigSGIX c, int a, float *v) { (void)d; (void)c; (void)a; (void)v; return GLX_BAD_ATTRIBUTE; }

/*
 * SGI's private GL_SGIX_igloo_interface, which SGI's IRIS GL (IGLOO) looks up
 * in libGL.so to have SGI's library do the IRIS GL things OpenGL does not.
 * 0x81A3 (its first word: on or off) is lmcolor(LMC_COLOR), done here (see
 * hgl_lmc in glshim_rt.c); the rest are taken and left to IGLOO's own way
 * (IRIS_GL_IGLOO_LOG=1 lists them).
 */
void
glIglooInterfaceSGIX(GLenum pname, const GLvoid *params)
{
	const unsigned int *u = (const unsigned int *)params;
	static int log = -1;

	if (log < 0)
		log = getenv("IRIS_GL_IGLOO_LOG") != NULL;
	if (log)
		fprintf(stderr, "libGL: glIglooInterfaceSGIX(%#x, %#x)\n", (unsigned)pname, u != NULL ? u[0] : 0);
	if (pname == 0x81A3 && u != NULL)
		hgl_lmc_set(u[0] != 0);
}
