/*
 * IRIS GL for IRIS: SGI's pre-OpenGL graphics API, on top of OpenGL.
 *
 * IRIS GL programs on the IRIX image -- clock, buttonfly, atlantis, blast,
 * flight -- link SGI's libgl.so, which drives the graphics board itself. On a
 * display that is not the emulated Newport (the host X server) there is no
 * board to find ("Can't determine board count (old kernel?)"), and even on
 * the Newport the drawing is emulated in software.
 *
 * So this library takes libgl.so's place. It translates to OpenGL *in the
 * guest* and calls the OpenGL shim (glshim_*.c, SONAME libGL.so), which means
 * the whole host side -- the batched command buffer, the executor, GLX, the
 * frame handoff -- is shared, and it makes no host calls of its own.
 *
 * It defines every entry point the real library exports (523 of them; see
 * tools/irisglshim.py), because a symbol left out would be resolved from
 * SGI's libgl.so, which would then load and hit exactly the wall this avoids.
 */
#ifndef HGL_IRISGL_SHIM_H
#define HGL_IRISGL_SHIM_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <gl/gl.h>
#include <gl/device.h>

/* Said once per entry point, so a program that walks off the implemented set
 * names the call instead of failing somewhere else later. */
void hgl_irisgl_missing(const char *name);

/* IRIS_IRISGL_TRACE=1 logs every implemented call as it happens. A program
 * that stops does so inside one of these, and the last line says which. */
void hgl_irisgl_trace(const char *name);
#define TRACE(n) hgl_irisgl_trace(n)
void hgl_irisgl_tracef(const char *fmt, ...);

/*
 * The current window, its GLX context and the queue, shared by the halves.
 *
 * IRIS GL has many windows and one current one (winset). The per-window
 * fields below are the *current* window's; irisgl_rt.c keeps every window in
 * a table and copies a window's fields in and out of here when winset (or
 * GLXwinset) changes which one is current, so the drawing code only ever
 * looks at hgl_iris.
 */
/* A window's popup and overlay layers, as windows in the overlay planes
 * (irisgl_extra.c). */
struct hgl_layers;

typedef struct {
	Display *dpy;            /* the library's own connection: events, fonts */
	/* --- the current window --- */
	long gid;                /* its IRIS GL identifier, 0 before winopen */
	Display *gldpy;          /* the connection GLX calls use for it */
	Window win;
	GLXContext ctx;
	XVisualInfo *vi;
	int w, h;
	int opened;              /* the X window exists */
	int glx;                 /* an X window the program made (GLXlink) */
	int layer;               /* ... for an overlay, underlay or popup layer */
	struct hgl_layers *layers; /* its own layers' windows, or NULL */
	unsigned enables;        /* the states hgl_enable keeps, as the program set them */
	/* A GLX mixed-model layer window in the overlay visual: its pixels are
	 * colour indices, its colours those of index_cmap (see GLXlink). */
	int index_win;
	Colormap index_cmap;
	int want_rgb, want_double, want_zbuf, want_ms;
	/* frontbuffer(TRUE) in a double-buffered program: what it draws has to
	 * be seen without a swap. */
	int front;
	/* Blending is on (blendfunction, linesmooth, pntsmooth), so pixels
	 * must not be drawn twice. */
	int blend;
	int mmode;               /* MSINGLE / MPROJECTION / MVIEWING / MTEXTURE */
	/* scrmask, inclusive, in window coordinates */
	int mask_l, mask_r, mask_b, mask_t;
	char title[128];
	/* --- process-wide --- */
	/* The devices qdevice() asked for, and the event queue we fill from X. */
	unsigned char queued[1024];
	struct { short dev; short val; } q[256];
	int qhead, qtail;
	int mousex, mousey;
	unsigned char button[1024];
	/* TIMER0..3: ticks between events (noise), and when each is next due,
	 * in seconds. A tick is a vertical retrace, a sixtieth of a second. */
	short timer_ticks[4];
	double timer_due[4];
} HglIris;

extern HglIris hgl_iris;

/* Make the window and the context if they are not there yet: IRIS GL lets a
 * program configure before or after winopen, so neither call can be the one
 * that commits. */
void hgl_iris_ensure(void);
/* The projection calls -- perspective, window, ortho, ortho2 -- replace the
 * projection matrix in MVIEWING mode, where every other matrix call works on
 * the viewing matrix, and the current matrix in MSINGLE and MPROJECTION
 * (mmode(3G)). hgl_projection_begin makes the matrix they replace current and
 * clears it; hgl_projection_end makes the viewing matrix current again. */
void hgl_projection_begin(void);
/* Picking (1) or selecting (2): nothing is drawn; see pick in irisgl_extra.c. */
extern int hgl_selecting;
extern float hgl_pick_matrix[16];
void hgl_projection_end(void);
void hgl_iris_pump(int block);
/* An X event someone else read (a popup menu), into the program's queue. */
void hgl_iris_event(XEvent *e);
void hgl_set_colour(float r, float g, float b, float a);
Cursor hgl_blank_cursor(void);
void hgl_iris_swapinterval(int n);
void hgl_iris_present_if_single(void);
/* Something was drawn into the normal planes for a layer that has no window
 * of its own (see drawmode) since the last present. Those planes are
 * single-buffered and on the screen as soon as they are drawn, but this is
 * the window's own buffer: the next point where the program waits
 * (hgl_iris_present_if_single) presents it. */
extern int hgl_layer_drawn;
/* The layers that are windows (irisgl_extra.c). hgl_layer_suspend makes the
 * normal planes current -- for a swap, or before another window is -- and
 * says for hgl_layer_resume where drawing was. */
int hgl_layer_suspend(void);
void hgl_layer_resume(int was);
void hgl_layers_present(void);
void hgl_layers_resize(struct hgl_layers *L, Display *d, int w, int h);
void hgl_layers_free(struct hgl_layers *L, Display *d);
/* glEnable/glDisable for the states a layer is drawn without (z-buffer,
 * lighting, texture, fog, blending, alpha test): see irisgl_extra.c. */
void hgl_enable(GLenum cap, int on);
/* Turn a cap off for a moment if the program has it on (hgl_resume turns it
 * back on): old-style polygons are never textured or fogged. */
int hgl_suspend(GLenum cap);
void hgl_resume(GLenum cap, int was);

/* A polygon's vertex with what it carries. */
struct hgl_vtx {
	float v[3], n[3], t[2], c[4];
};
#define HGL_VTX_COLOUR 1	/* send each vertex's colour */
#define HGL_VTX_NT 2		/* ... and its normal and texture coordinate */
/* Fill a polygon, tessellated when concave; lighting is already set and no
 * glBegin is open. */
void hgl_polygon_fill(const struct hgl_vtx *p, int n, int what, int concave);
/* Draw an old-style polygon (polf, rectf, circf, pmv ...): see irisgl_extra.c. */
void hgl_old_polygon(const struct hgl_vtx *p, int n, int what);
extern int hgl_concave;
/* Set lighting for drawing that does not go through hgl_begin. */
void hgl_lighting_sync(void);
/* depthcue (irisgl_draw.c) and the stencil ops that follow zbuffer. */
extern int hgl_depthcue;
void hgl_depthcue_update(void);
void hgl_stencil_ops(void);
/* An object's list is being compiled (makeobj); fonts build their lists
 * beforehand (hgl_fonts_prepare). */
extern int hgl_compiling;
void hgl_fonts_prepare(void);
/* The last pixel of an open line (subpixel FALSE draws lines closed). */
void hgl_line_end(const float v[3]);
/* The X server's overlay visual (GLX_LEVEL 1) on the library's own
 * connection, or NULL: then the layers are drawn into the normal planes. */
XVisualInfo *hgl_overlay_visual(void);
/* An index window's colour map (irisgl_rt.c): store an index's colour, or
 * read it back. Nonzero when the current window is one and it was done. */
int hgl_index_mapcolor(Colorindex i, short r, short g, short b);
int hgl_index_getmcolor(Colorindex i, short *r, short *g, short *b);
/* getgdesc's GD_BITS_OVER_SNG_CMODE (0) and GD_BITS_PUP_SNG_CMODE (1). */
long hgl_overlay_bits(int popup);
/* The OpenGL shim's: the current context draws colour indices, or not. */
extern void hgl_client_index(int on);
/* The library's own X connection, opened on first use. */
Display *hgl_display(void);
/* viewport and scrmask together, as IRIS GL's viewport resets the mask:
 * a rectangle in window pixels, inclusive of both edges. */
void hgl_set_viewport(int l, int r, int b, int t);
void hgl_apply_scrmask(void);
/* A new context's IRIS GL defaults (winopen(3G)): the viewport and screen
 * mask cover the window and the single matrix is ortho2 onto its pixels. */
void hgl_window_defaults(void);
/* Colour writes on or off: off in a draw mode or window that has no planes. */
void hgl_apply_colormask(void);
/* Colour-map calls made in a draw mode other than NORMALDRAW (irisgl_extra.c):
 * nonzero when the call was for that layer and has been handled. */
int hgl_layer_mapcolor(Colorindex i, short r, short g, short b);
int hgl_layer_color(Colorindex i);
int hgl_layer_getmcolor(Colorindex i, short *r, short *g, short *b);
/* The window was placed by the program (winposition): getorigin asks the server. */
void hgl_origin_known(void);

/* The colour map (irisgl_draw.c): an index's colour, and the index whose
 * colour is nearest a pixel's, for the pixel calls (irisgl_pixels.c). */
#define HGL_CMAP_SIZE 4096

/*
 * IRIS GL draws characters in the colour current when they are drawn; OpenGL
 * uses the colour current when the raster position was last set. Every colour
 * change counts (hgl_colour_serial), every raster position set records the
 * count it was made with (hgl_raster_serial), and text calls
 * hgl_latch_raster_colour first, which sets the raster position again where it
 * is -- asking the host where that is -- only if the colour changed since.
 */
extern unsigned long hgl_colour_serial, hgl_raster_serial;
void hgl_latch_raster_colour(void);
/* glRasterPos3f, never lit (cmov). */
void hgl_rasterpos(float x, float y, float z);
/* linewidth and shademodel as last set (pushattributes saves them). */
extern float hgl_line_width;
extern long hgl_shade_model;
void hgl_cmap_rgb(unsigned long index, unsigned char *rgb);
/* The current colour, and the index color() last set. */
void hgl_current_colour(float *rgba);
extern long hgl_colour_index;
unsigned long hgl_cmap_index(unsigned char r, unsigned char g, unsigned char b);

int hgl_back_material_bound(void);
/* Every primitive begins here, so it is lit or not as IRIS GL would light it. */
void hgl_begin(GLenum mode);
/* The lmcolor mode, which decides what colour commands do under lighting. */
extern long hgl_lmcolor_mode;
#endif
