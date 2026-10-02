/*
 * The rest of IRIS GL that programs on the image reach: draw modes, arcs and
 * circles, screen boxes, the short/int/double spellings, write masks and
 * the window queries. See irisgl_shim.h.
 */
#include "irisgl_shim.h"
#include <gl/get.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ---- draw modes ----
 *
 * The popup and overlay planes are the board's own: each IRIS GL window gets
 * a child window per layer, in the X server's overlay visual (GLX_LEVEL 1),
 * made the first time the program draws in that mode. drawmode makes the
 * window's context current on the layer's window -- the same context, so
 * the matrices, viewport and screen mask stay the ones the program set, as
 * IRIS GL shares them between draw modes -- with depth, lighting, texture,
 * fog and blending off, and draws colour indices (see hgl_client_index in
 * the OpenGL shim: an index is the red component of an 8-bit colour). What a
 * layer holds goes into its window at the points a single-buffered window
 * is presented, and the board shows it over the picture; index 0 is the
 * transparent pixel. flight draws its whole instrument panel in the popup
 * planes this way.
 *
 * Without an overlay visual (a server other than IMPACT's), in the underlay,
 * or in a window of the GLX mixed model, a layer is drawn into the normal
 * planes instead, through the layer's own colour map; colour 0 -- erasing,
 * in a layer -- leaves the normal planes alone there.
 *
 * A layer window of the GLX mixed model (hgl_iris.layer) is another matter:
 * it is a separate, invisible X window, and nothing drawn into it is kept.
 */
static long draw_mode = NORMALDRAW;
static int layer_transparent;

#define NLAYERS 3
static float layer_map[NLAYERS][256][3];
static int layer_map_ready;
static long layer_index[NLAYERS];
static float normal_colour[4];
static long normal_index;

/* The layers that are windows of their own: the popup planes and the
 * overlay. */
#define NREAL 2

struct hgl_layers {
	Window win[NREAL];
	Colormap cmap[NREAL];
	/* The colormap cells this window owns: every one it could get, which
	 * is all but the server's transparent pixel. */
	unsigned char owned[NREAL][256];
	/* Drawn into since its window was last given the pixels. */
	int drawn[NREAL];
};

/* Where drawing goes now: -1 the normal planes, else a real layer of the
 * current window. */
static int target = -1;

static int
layer_of(long m)
{
	switch (m) {
	case PUPDRAW: case CURSORDRAW: return 0;
	case OVERDRAW: return 1;
	case UNDERDRAW: return 2;
	default: return -1;
	}
}

/* The X server's overlay visual, or NULL. IRIS_IRISGL_NO_OVERLAY=1 draws
 * the layers into the normal planes, as a server without one does. */
XVisualInfo *
hgl_overlay_visual(void)
{
	static int known;
	static XVisualInfo *vi;
	static int want[] = { GLX_LEVEL, 1, GLX_BUFFER_SIZE, 1, None };
	Display *d;

	/* The library's own connection: a program asks getgdesc before it has
	 * a window, and asking must not make one. Its layers are windows on
	 * that connection too. */
	if (!known && (d = hgl_display()) != NULL) {
		known = 1;
		if (getenv("IRIS_IRISGL_NO_OVERLAY") == NULL)
			vi = glXChooseVisual(d, DefaultScreen(d), want);
	}
	return vi;
}

/* The real layer draw mode `m` draws into here, or -1: the normal planes. */
static int
real_layer(long m)
{
	int l = layer_of(m);

	if (l < 0 || l >= NREAL || hgl_iris.glx || !hgl_iris.opened || hgl_overlay_visual() == NULL)
		return -1;
	return l;
}

long
hgl_overlay_bits(int popup)
{
	if (hgl_overlay_visual() == NULL)
		return 0;
	return popup ? 2 : hgl_overlay_visual()->depth;
}

static void
layer_map_init(void)
{
	int l, i;
	unsigned char rgb[3];

	/* Until a program maps them, a layer's colours are the normal map's
	 * first ones. */
	for (l = 0; l < NLAYERS; l++)
		for (i = 0; i < 256; i++) {
			hgl_cmap_rgb(i, rgb);
			layer_map[l][i][0] = rgb[0] / 255.0f;
			layer_map[l][i][1] = rgb[1] / 255.0f;
			layer_map[l][i][2] = rgb[2] / 255.0f;
		}
	layer_map_ready = 1;
}

static void
store_cell(struct hgl_layers *L, int l, int i)
{
	XColor c;

	if (L == NULL || L->cmap[l] == None || !L->owned[l][i])
		return;
	c.pixel = (unsigned long)i;
	c.red = (unsigned short)(layer_map[l][i][0] * 65535.0f + 0.5f);
	c.green = (unsigned short)(layer_map[l][i][1] * 65535.0f + 0.5f);
	c.blue = (unsigned short)(layer_map[l][i][2] * 65535.0f + 0.5f);
	c.flags = DoRed | DoGreen | DoBlue;
	XStoreColor(hgl_iris.gldpy, L->cmap[l], &c);
}

/* Layer l's window of the current window, made on first use: a child
 * covering it, in the overlay visual, with a colormap of its own. */
static Window
layer_window(int l)
{
	HglIris *g = &hgl_iris;
	struct hgl_layers *L;
	XVisualInfo *vi = hgl_overlay_visual();
	XSetWindowAttributes swa;
	unsigned long px[256];
	Window list[NREAL + 1];
	int i, n, k;

	if (g->layers == NULL && (g->layers = calloc(1, sizeof *g->layers)) == NULL)
		return None;
	L = g->layers;
	if (L->win[l] != None)
		return L->win[l];
	if (!layer_map_ready)
		layer_map_init();
	L->cmap[l] = XCreateColormap(g->gldpy, RootWindow(g->gldpy, vi->screen), vi->visual, AllocNone);
	/* Every cell there is: the server keeps the transparent one. */
	for (n = 255; n > 0; n--)
		if (XAllocColorCells(g->gldpy, L->cmap[l], False, NULL, 0, px, (unsigned)n))
			break;
	for (i = 0; i < n; i++)
		if (px[i] < 256)
			L->owned[l][px[i]] = 1;
	for (i = 1; i < 256; i++)
		store_cell(L, l, i);
	memset(&swa, 0, sizeof swa);
	swa.colormap = L->cmap[l];
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	L->win[l] = XCreateWindow(g->gldpy, g->win, 0, 0, (unsigned)g->w, (unsigned)g->h, 0,
	    vi->depth, InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XMapWindow(g->gldpy, L->win[l]);
	/* The popup planes are over the overlay. */
	if (L->win[0] != None && l != 0)
		XRaiseWindow(g->gldpy, L->win[0]);
	/* The window manager installs the layers' colormaps with the window's. */
	for (k = 0, i = 0; i < NREAL; i++)
		if (L->win[i] != None)
			list[k++] = L->win[i];
	list[k++] = g->win;
	XSetWMColormapWindows(g->gldpy, g->win, list, k);
	XSync(g->gldpy, False);
	hgl_irisgl_tracef("layer %d: window %lx, %d colormap cells", l, (unsigned long)L->win[l], n);
	return L->win[l];
}

/*
 * The states a layer is drawn without -- it holds indices, not lit, textured,
 * fogged or blended colours, and has no z-buffer -- and which belong to the
 * window, not the draw mode: a program that turns the z-buffer off while in
 * the popup planes has turned it off for its normal drawing too. So the
 * library sets them through hgl_enable, which keeps what the program asked
 * for (per window, in hgl_iris.enables) and gives it to GL only while the
 * normal planes are current; leaving a layer gives it back. Saving and
 * restoring GL's own state around a layer (glPushAttrib) undid every such
 * change the program made in the layer -- flight's dial faces came out
 * white and its view flashed.
 */
static const GLenum layer_off[] = {
	GL_DEPTH_TEST, GL_LIGHTING, GL_TEXTURE_2D, GL_FOG, GL_BLEND, GL_ALPHA_TEST
};
#define NOFF ((int)(sizeof layer_off / sizeof layer_off[0]))

void
hgl_enable(GLenum cap, int on)
{
	int i;

	for (i = 0; i < NOFF; i++)
		if (layer_off[i] == cap) {
			if (on)
				hgl_iris.enables |= 1u << i;
			else
				hgl_iris.enables &= ~(1u << i);
		}
	if (target >= 0)
		return;
	if (on)
		glEnable(cap);
	else
		glDisable(cap);
}

/* Draw into layer l of the current window, or (l < 0) its normal planes. */
static void
set_target(int l)
{
	HglIris *g = &hgl_iris;
	Window w = None;
	int i, from = target;

	if (l == target)
		return;
	/* What was drawn in the layer being left is due on the screen: a
	 * program that erases its menu and goes back to the normal planes
	 * left it showing otherwise. */
	if (target >= 0 && g->layers != NULL)
		g->layers->drawn[target] = 1;
	target = -1;
	if (l >= 0)
		w = layer_window(l);
	if (w == None) {
		glXMakeCurrent(g->gldpy, g->win, g->ctx);
		/* Back from a layer: the window's own state again. */
		for (i = 0; from >= 0 && i < NOFF; i++)
			if (g->enables & (1u << i))
				glEnable(layer_off[i]);
		return;
	}
	glXMakeCurrent(g->gldpy, w, g->ctx);
	for (i = 0; i < NOFF; i++)
		glDisable(layer_off[i]);
	hgl_client_index(1);
	glDisable(GL_DITHER);
	g->layers->drawn[l] = 1;
	target = l;
}

/* Back to the normal planes for a while (a swap, another window): what
 * hgl_layer_resume takes to return. */
int
hgl_layer_suspend(void)
{
	int was = target;

	if (target >= 0)
		set_target(-1);
	return was;
}

void
hgl_layer_resume(int was)
{
	if (was >= 0 && real_layer(draw_mode) == was)
		set_target(was);
}

/*
 * The layers' pixels into their windows: those drawn in since last time, and
 * the one drawing is going on in -- nothing marks each primitive, so while a
 * program stays in a layer it is shown again at most every LAYER_PERIOD
 * seconds. jot enters the popup planes, flushes, draws its pull-down menu
 * and then polls the mouse without leaving them: marking a layer only on the
 * way in, its menu was never shown.
 */
#define LAYER_PERIOD 0.066

static double
now_s(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

void
hgl_layers_present(void)
{
	static double last[NREAL];
	HglIris *g = &hgl_iris;
	struct hgl_layers *L = g->layers;
	double t;
	int l, was, due[NREAL];

	if (L == NULL)
		return;
	t = now_s();
	for (l = 0; l < NREAL; l++)
		due[l] = L->win[l] != None && (L->drawn[l] || (l == target && t - last[l] >= LAYER_PERIOD));
	if (!due[0] && !due[1])
		return;
	was = target;
	for (l = 0; l < NREAL; l++) {
		if (!due[l])
			continue;
		set_target(l);
		glXSwapBuffers(g->gldpy, L->win[l]);
		last[l] = t;
	}
	set_target(was);
	/* Switching between them marked them drawn; they were presented just
	 * now, and nothing has been drawn since. */
	for (l = 0; l < NREAL; l++)
		if (due[l])
			L->drawn[l] = 0;
}

/* The window became w x h: its layers follow. */
void
hgl_layers_resize(struct hgl_layers *L, Display *d, int w, int h)
{
	int l;

	for (l = 0; L != NULL && l < NREAL; l++)
		if (L->win[l] != None)
			XResizeWindow(d, L->win[l], (unsigned)w, (unsigned)h);
}

/* The window is closing: its layers' colormaps go (the windows go with it). */
void
hgl_layers_free(struct hgl_layers *L, Display *d)
{
	int l;

	if (L == NULL)
		return;
	if (L == hgl_iris.layers && target >= 0)
		set_target(-1);
	for (l = 0; l < NREAL; l++)
		if (L->cmap[l] != None)
			XFreeColormap(d, L->cmap[l]);
	free(L);
}

int
hgl_layer_mapcolor(Colorindex i, short r, short g, short b)
{
	int l = layer_of(draw_mode);

	if (l < 0)
		return 0;
	if (!layer_map_ready)
		layer_map_init();
	i &= 255;
	layer_map[l][i][0] = r / 255.0f;
	layer_map[l][i][1] = g / 255.0f;
	layer_map[l][i][2] = b / 255.0f;
	if (target == l)
		store_cell(hgl_iris.layers, l, i);
	/* The colour in use changes with its map entry. */
	if (layer_index[l] == i)
		hgl_layer_color(i);
	return 1;
}

int
hgl_layer_color(Colorindex i)
{
	int l = layer_of(draw_mode);

	if (l < 0)
		return 0;
	if (!layer_map_ready)
		layer_map_init();
	i &= 255;
	layer_index[l] = i;
	hgl_iris_ensure();
	if (target == l) {
		/* A real layer draws the index itself (see set_target). */
		layer_transparent = 0;
		hgl_set_colour((float)i / 255.0f, 0.0f, 0.0f, 1.0f);
	} else {
		layer_transparent = i == 0;
		hgl_set_colour(layer_map[l][i][0], layer_map[l][i][1], layer_map[l][i][2], 1.0f);
	}
	hgl_apply_colormask();
	return 1;
}

int
hgl_layer_getmcolor(Colorindex i, short *r, short *g, short *b)
{
	int l = layer_of(draw_mode);

	if (l < 0)
		return 0;
	if (!layer_map_ready)
		layer_map_init();
	i &= 255;
	*r = (short)(layer_map[l][i][0] * 255.0f + 0.5f);
	*g = (short)(layer_map[l][i][1] * 255.0f + 0.5f);
	*b = (short)(layer_map[l][i][2] * 255.0f + 0.5f);
	return 1;
}

void
drawmode(long m)
{
	int was = layer_of(draw_mode), now = layer_of(m);

	hgl_irisgl_tracef("drawmode %ld", m);
	hgl_iris_ensure();
	if (m == draw_mode)
		return;
	if (was < 0) {
		hgl_current_colour(normal_colour);
		normal_index = hgl_colour_index;
	}
	draw_mode = m;
	set_target(real_layer(m));
	if (now >= 0) {
		if (target < 0)
			hgl_layer_drawn = 1;
		/* Each draw mode keeps its own current colour. */
		hgl_layer_color((Colorindex)layer_index[now]);
		hgl_colour_index = layer_index[now];
	} else {
		layer_transparent = 0;
		hgl_set_colour(normal_colour[0], normal_colour[1], normal_colour[2], normal_colour[3]);
		hgl_colour_index = normal_index;
		hgl_apply_colormask();
	}
}

/* The program's own write mask (wmpack, RGBwritemask, writemask), whole
 * components only: OpenGL masks a component or not, as Impact does. */
static GLboolean wm[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

void
hgl_apply_colormask(void)
{
	GLboolean on = !hgl_iris.layer && (layer_of(draw_mode) < 0 || target >= 0 || !layer_transparent);

	glColorMask(on && wm[0], on && wm[1], on && wm[2], on && wm[3]);
}

void
wmpack(unsigned long m)
{
	hgl_irisgl_tracef("wmpack %#lx", m);
	hgl_iris_ensure();
	wm[0] = (m & 0xff) != 0;
	wm[1] = (m >> 8 & 0xff) != 0;
	wm[2] = (m >> 16 & 0xff) != 0;
	wm[3] = (m >> 24 & 0xff) != 0;
	hgl_apply_colormask();
}

void
RGBwritemask(short r, short g, short b)
{
	hgl_iris_ensure();
	wm[0] = r != 0;
	wm[1] = g != 0;
	wm[2] = b != 0;
	hgl_apply_colormask();
}

/* Colour-map mode's mask is over index bits, which are colours here: any bit
 * writes, none protects. */
void
writemask(Colorindex m)
{
	hgl_iris_ensure();
	wm[0] = wm[1] = wm[2] = wm[3] = m != 0;
	hgl_apply_colormask();
}

void
gRGBmask(short *r, short *g, short *b)
{
	*r = wm[0] ? 0xff : 0;
	*g = wm[1] ? 0xff : 0;
	*b = wm[2] ? 0xff : 0;
}

long getwritemask(void) { return wm[0] ? 0xfff : 0; }

void zwritemask(unsigned long m) { hgl_iris_ensure(); glDepthMask(m != 0); }

long getdrawmode(void) { return draw_mode; }

long getmmode(void) { return hgl_iris.mmode; }

/* ---- the pen ----
 *
 * move/draw keep a current graphics position in object coordinates and draw
 * lines from it; pmv/pdr/pclos build a filled polygon a point at a time; the
 * r- spellings are relative to the position. One position serves both, as in
 * IRIS GL.
 */
static float pen[3];
static int in_poly;

static void
pen_line(float x, float y, float z)
{
	hgl_iris_ensure();
	hgl_begin(GL_LINES);
	glVertex3f(pen[0], pen[1], pen[2]);
	glVertex3f(x, y, z);
	glEnd();
	pen[0] = x;
	pen[1] = y;
	pen[2] = z;
}

static void
pen_move(float x, float y, float z)
{
	pen[0] = x;
	pen[1] = y;
	pen[2] = z;
}

static void
poly_move(float x, float y, float z)
{
	hgl_iris_ensure();
	if (in_poly)
		glEnd();
	hgl_begin(GL_POLYGON);
	glVertex3f(x, y, z);
	in_poly = 1;
	pen_move(x, y, z);
}

static void
poly_draw(float x, float y, float z)
{
	if (in_poly)
		glVertex3f(x, y, z);
	pen_move(x, y, z);
}

void move(Coord x, Coord y, Coord z) { pen_move(x, y, z); }
void movei(Icoord x, Icoord y, Icoord z) { pen_move((float)x, (float)y, (float)z); }
void moves(Scoord x, Scoord y, Scoord z) { pen_move(x, y, z); }
void move2(Coord x, Coord y) { pen_move(x, y, 0.0f); }
void move2i(Icoord x, Icoord y) { pen_move((float)x, (float)y, 0.0f); }
void move2s(Scoord x, Scoord y) { pen_move(x, y, 0.0f); }
void rmv(Coord x, Coord y, Coord z) { pen_move(pen[0] + x, pen[1] + y, pen[2] + z); }
void rmvi(Icoord x, Icoord y, Icoord z) { rmv((float)x, (float)y, (float)z); }
void rmvs(Scoord x, Scoord y, Scoord z) { rmv(x, y, z); }
void rmv2(Coord x, Coord y) { pen_move(pen[0] + x, pen[1] + y, pen[2]); }
void rmv2i(Icoord x, Icoord y) { rmv2((float)x, (float)y); }
void rmv2s(Scoord x, Scoord y) { rmv2(x, y); }

void draw(Coord x, Coord y, Coord z) { pen_line(x, y, z); }
void drawi(Icoord x, Icoord y, Icoord z) { pen_line((float)x, (float)y, (float)z); }
void draws(Scoord x, Scoord y, Scoord z) { pen_line(x, y, z); }
void draw2(Coord x, Coord y) { pen_line(x, y, 0.0f); }
void draw2i(Icoord x, Icoord y) { pen_line((float)x, (float)y, 0.0f); }
void draw2s(Scoord x, Scoord y) { pen_line(x, y, 0.0f); }
void rdr(Coord x, Coord y, Coord z) { pen_line(pen[0] + x, pen[1] + y, pen[2] + z); }
void rdri(Icoord x, Icoord y, Icoord z) { rdr((float)x, (float)y, (float)z); }
void rdrs(Scoord x, Scoord y, Scoord z) { rdr(x, y, z); }
void rdr2(Coord x, Coord y) { pen_line(pen[0] + x, pen[1] + y, pen[2]); }
void rdr2i(Icoord x, Icoord y) { rdr2((float)x, (float)y); }
void rdr2s(Scoord x, Scoord y) { rdr2(x, y); }

void pmv(Coord x, Coord y, Coord z) { poly_move(x, y, z); }
void pmvi(Icoord x, Icoord y, Icoord z) { poly_move((float)x, (float)y, (float)z); }
void pmvs(Scoord x, Scoord y, Scoord z) { poly_move(x, y, z); }
void pmv2(Coord x, Coord y) { poly_move(x, y, 0.0f); }
void pmv2i(Icoord x, Icoord y) { poly_move((float)x, (float)y, 0.0f); }
void pmv2s(Scoord x, Scoord y) { poly_move(x, y, 0.0f); }
void rpmv(Coord x, Coord y, Coord z) { poly_move(pen[0] + x, pen[1] + y, pen[2] + z); }
void rpmvi(Icoord x, Icoord y, Icoord z) { rpmv((float)x, (float)y, (float)z); }
void rpmvs(Scoord x, Scoord y, Scoord z) { rpmv(x, y, z); }
void rpmv2(Coord x, Coord y) { poly_move(pen[0] + x, pen[1] + y, pen[2]); }
void rpmv2i(Icoord x, Icoord y) { rpmv2((float)x, (float)y); }
void rpmv2s(Scoord x, Scoord y) { rpmv2(x, y); }

void pdr(Coord x, Coord y, Coord z) { poly_draw(x, y, z); }
void pdri(Icoord x, Icoord y, Icoord z) { poly_draw((float)x, (float)y, (float)z); }
void pdrs(Scoord x, Scoord y, Scoord z) { poly_draw(x, y, z); }
void pdr2(Coord x, Coord y) { poly_draw(x, y, 0.0f); }
void pdr2i(Icoord x, Icoord y) { poly_draw((float)x, (float)y, 0.0f); }
void pdr2s(Scoord x, Scoord y) { poly_draw(x, y, 0.0f); }
void rpdr(Coord x, Coord y, Coord z) { poly_draw(pen[0] + x, pen[1] + y, pen[2] + z); }
void rpdri(Icoord x, Icoord y, Icoord z) { rpdr((float)x, (float)y, (float)z); }
void rpdrs(Scoord x, Scoord y, Scoord z) { rpdr(x, y, z); }
void rpdr2(Coord x, Coord y) { poly_draw(pen[0] + x, pen[1] + y, pen[2]); }
void rpdr2i(Icoord x, Icoord y) { rpdr2((float)x, (float)y); }
void rpdr2s(Scoord x, Scoord y) { rpdr2(x, y); }

void
pclos(void)
{
	TRACE("pclos");
	if (in_poly)
		glEnd();
	in_poly = 0;
}
void spclos(void) { pclos(); }

void
getgpos(Coord *x, Coord *y, Coord *z, Coord *w)
{
	if (x) *x = pen[0];
	if (y) *y = pen[1];
	if (z) *z = pen[2];
	if (w) *w = 1.0f;
}

/* ---- points ---- */

void
pnt(Coord x, Coord y, Coord z)
{
	hgl_iris_ensure();
	hgl_begin(GL_POINTS);
	glVertex3f(x, y, z);
	glEnd();
	pen_move(x, y, z);
}
void pnti(Icoord x, Icoord y, Icoord z) { pnt((float)x, (float)y, (float)z); }
void pnts(Scoord x, Scoord y, Scoord z) { pnt(x, y, z); }
void pnt2(Coord x, Coord y) { pnt(x, y, 0.0f); }
void pnt2i(Icoord x, Icoord y) { pnt((float)x, (float)y, 0.0f); }
void pnt2s(Scoord x, Scoord y) { pnt(x, y, 0.0f); }

/* ---- polygons from arrays: poly outlines, polf fills, splf shades ---- */

#define POLYS(name, T, dim, mode) \
void name(long n, const T parray[][dim]) \
{ \
	long i; \
	if (n <= 0 || parray == NULL) \
		return; \
	hgl_iris_ensure(); \
	hgl_begin(mode); \
	for (i = 0; i < n; i++) \
		glVertex3f((float)parray[i][0], (float)parray[i][1], dim > 2 ? (float)parray[i][dim > 2 ? 2 : 0] : 0.0f); \
	glEnd(); \
}

POLYS(polf, Coord, 3, GL_POLYGON)
POLYS(polfi, Icoord, 3, GL_POLYGON)
POLYS(polfs, Scoord, 3, GL_POLYGON)
POLYS(polf2, Coord, 2, GL_POLYGON)
POLYS(polf2i, Icoord, 2, GL_POLYGON)
POLYS(polf2s, Scoord, 2, GL_POLYGON)
POLYS(poly, Coord, 3, GL_LINE_LOOP)
POLYS(polyi, Icoord, 3, GL_LINE_LOOP)
POLYS(polys, Scoord, 3, GL_LINE_LOOP)
POLYS(poly2, Coord, 2, GL_LINE_LOOP)
POLYS(poly2i, Icoord, 2, GL_LINE_LOOP)
POLYS(poly2s, Scoord, 2, GL_LINE_LOOP)

#define SPLF(name, T, dim) \
void name(long n, const T parray[][dim], const Colorindex iarray[]) \
{ \
	long i; \
	unsigned char rgb[3]; \
	if (n <= 0 || parray == NULL || iarray == NULL) \
		return; \
	hgl_iris_ensure(); \
	hgl_begin(GL_POLYGON); \
	for (i = 0; i < n; i++) { \
		hgl_cmap_rgb(iarray[i], rgb); \
		glColor3ub(rgb[0], rgb[1], rgb[2]); \
		glVertex3f((float)parray[i][0], (float)parray[i][1], dim > 2 ? (float)parray[i][dim > 2 ? 2 : 0] : 0.0f); \
	} \
	glEnd(); \
	hgl_colour_serial++; \
}

SPLF(splf, Coord, 3)
SPLF(splfi, Icoord, 3)
SPLF(splfs, Scoord, 3)
SPLF(splf2, Coord, 2)
SPLF(splf2i, Icoord, 2)
SPLF(splf2s, Scoord, 2)

/* concave(3G) promises that polygons may be concave. GL_POLYGON draws convex
 * ones only, and a concave one comes out with its notches filled. */
void concave(Boolean b) { (void)b; }

/* ---- vertices, normals, colours and texture coordinates ---- */

/* Through v3f, which a triangle mesh's swaptmesh needs to see every vertex. */
void v2d(const double v[2]) { float f[3]; f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = 0.0f; v3f(f); }
void v3d(const double v[3]) { float f[3]; f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = (float)v[2]; v3f(f); }
void v4d(const double v[4]) { glVertex4d(v[0], v[1], v[2], v[3]); }
void v3i(const long v[3]) { float f[3]; f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = (float)v[2]; v3f(f); }
void v4i(const long v[4]) { glVertex4i((GLint)v[0], (GLint)v[1], (GLint)v[2], (GLint)v[3]); }
void v3s(const short v[3]) { float f[3]; f[0] = v[0]; f[1] = v[1]; f[2] = v[2]; v3f(f); }
void v4s(const short v[4]) { glVertex4s(v[0], v[1], v[2], v[3]); }
void normal(const Coord v[3]) { n3f(v); }
void t2d(const double v[2]) { glTexCoord2d(v[0], v[1]); }
void t3d(const double v[3]) { glTexCoord3d(v[0], v[1], v[2]); }
void t3f(const float v[3]) { glTexCoord3fv(v); }
void t3i(const long v[3]) { glTexCoord3f((float)v[0], (float)v[1], (float)v[2]); }
void t3s(const short v[3]) { glTexCoord3s(v[0], v[1], v[2]); }
void t4d(const double v[4]) { glTexCoord4d(v[0], v[1], v[2], v[3]); }
void t4f(const float v[4]) { glTexCoord4fv(v); }
void t4i(const long v[4]) { glTexCoord4f((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
void t4s(const short v[4]) { glTexCoord4s(v[0], v[1], v[2], v[3]); }

/* Integer colour components run 0..255, and values above are clamped. */
static float
c255(long v)
{
	return v <= 0 ? 0.0f : v >= 255 ? 1.0f : (float)v / 255.0f;
}

void c3i(const long c[3]) { hgl_iris_ensure(); hgl_set_colour(c255(c[0]), c255(c[1]), c255(c[2]), 1.0f); }
void c3s(const short c[3]) { hgl_iris_ensure(); hgl_set_colour(c255(c[0]), c255(c[1]), c255(c[2]), 1.0f); }
void c4i(const long c[4]) { hgl_iris_ensure(); hgl_set_colour(c255(c[0]), c255(c[1]), c255(c[2]), c255(c[3])); }
void c4s(const short c[4]) { hgl_iris_ensure(); hgl_set_colour(c255(c[0]), c255(c[1]), c255(c[2]), c255(c[3])); }

void
gRGBcolor(short *r, short *g, short *b)
{
	float c[4];

	hgl_current_colour(c);
	*r = (short)(c[0] * 255.0f + 0.5f);
	*g = (short)(c[1] * 255.0f + 0.5f);
	*b = (short)(c[2] * 255.0f + 0.5f);
}

long getcolor(void) { return hgl_colour_index; }
long getshade(void) { return hgl_colour_index; }
void setshade(Colorindex i) { color(i); }

/* One colour map, always: IRIS GL's multimap mode has nothing to switch. */
Boolean getcmmode(void) { return TRUE; }
long getmap(void) { return 0; }
void onemap(void) { }
void multimap(void) { }
void setmap(short n) { (void)n; }

/* ---- circles, arcs and screen boxes ----
 *
 * All in the x-y plane of object space, as line loops or fans. Angles are
 * tenths of a degree, counter-clockwise from the positive x axis, and an arc
 * runs from its start angle counter-clockwise to its end.
 */
#define CIRCLE_SEGMENTS 80

static void
arc_path(float x, float y, float r, long a0, long a1, int filled, int closed)
{
	double s, e, t;
	int i, n;

	hgl_iris_ensure();
	s = a0 * M_PI / 1800.0;
	e = a1 * M_PI / 1800.0;
	while (e <= s)
		e += 2.0 * M_PI;
	if (closed)
		e = s + 2.0 * M_PI;
	n = (int)(CIRCLE_SEGMENTS * (e - s) / (2.0 * M_PI) + 0.999);
	if (n < 2)
		n = 2;
	if (filled) {
		hgl_begin(GL_TRIANGLE_FAN);
		glVertex2f(x, y);
	} else {
		hgl_begin(closed ? GL_LINE_LOOP : GL_LINE_STRIP);
	}
	for (i = 0; i <= n; i++) {
		if (closed && !filled && i == n)
			break;
		t = s + (e - s) * i / n;
		glVertex2f(x + r * (float)cos(t), y + r * (float)sin(t));
	}
	glEnd();
}

void arc(Coord x, Coord y, Coord r, Angle a, Angle b) { arc_path(x, y, r, a, b, 0, 0); }
void arci(Icoord x, Icoord y, Icoord r, Angle a, Angle b) { arc_path((float)x, (float)y, (float)r, a, b, 0, 0); }
void arcs(Scoord x, Scoord y, Scoord r, Angle a, Angle b) { arc_path(x, y, r, a, b, 0, 0); }
void arcf(Coord x, Coord y, Coord r, Angle a, Angle b) { arc_path(x, y, r, a, b, 1, 0); }
void arcfi(Icoord x, Icoord y, Icoord r, Angle a, Angle b) { arc_path((float)x, (float)y, (float)r, a, b, 1, 0); }
void arcfs(Scoord x, Scoord y, Scoord r, Angle a, Angle b) { arc_path(x, y, r, a, b, 1, 0); }
void circ(Coord x, Coord y, Coord r) { arc_path(x, y, r, 0, 0, 0, 1); }
void circi(Icoord x, Icoord y, Icoord r) { arc_path((float)x, (float)y, (float)r, 0, 0, 0, 1); }
void circs(Scoord x, Scoord y, Scoord r) { arc_path(x, y, r, 0, 0, 0, 1); }
void circf(Coord x, Coord y, Coord r) { arc_path(x, y, r, 0, 0, 1, 1); }
void circfi(Icoord x, Icoord y, Icoord r) { arc_path((float)x, (float)y, (float)r, 0, 0, 1, 1); }
void circfs(Scoord x, Scoord y, Scoord r) { arc_path(x, y, r, 0, 0, 1, 1); }

void sbox(Coord a, Coord b, Coord c, Coord d) { rect(a, b, c, d); }
void sboxi(Icoord a, Icoord b, Icoord c, Icoord d) { rect((float)a, (float)b, (float)c, (float)d); }
void sboxs(Scoord a, Scoord b, Scoord c, Scoord d) { rect(a, b, c, d); }
void sboxf(Coord a, Coord b, Coord c, Coord d) { rectf(a, b, c, d); }
void sboxfi(Icoord a, Icoord b, Icoord c, Icoord d) { rectf((float)a, (float)b, (float)c, (float)d); }
void sboxfs(Scoord a, Scoord b, Scoord c, Scoord d) { rectf(a, b, c, d); }

/* bbox2: a culling hint. Drawing everything is what happens without it. */
void bbox2(Screencoord a, Screencoord b, Coord c, Coord d, Coord e, Coord f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void bbox2i(Screencoord a, Screencoord b, Icoord c, Icoord d, Icoord e, Icoord f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void bbox2s(Screencoord a, Screencoord b, Scoord c, Scoord d, Scoord e, Scoord f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }

/* ---- viewing ---- */

/* window(3G): a perspective frustum that, like perspective, replaces the
 * projection. */
void
window(Coord l, Coord r, Coord b, Coord t, Coord n, Coord f)
{
	hgl_projection_begin();
	glFrustum(l, r, b, t, n, f);
	hgl_projection_end();
}

/* polarview(3G): the viewer at dist, looking at the origin from azimuth and
 * incidence, twisted about the line of sight -- all tenths of a degree. */
void
polarview(Coord dist, Angle azim, Angle inc, Angle twist)
{
	hgl_iris_ensure();
	glTranslatef(0.0f, 0.0f, -dist);
	glRotatef(-twist / 10.0f, 0.0f, 0.0f, 1.0f);
	glRotatef(-inc / 10.0f, 1.0f, 0.0f, 0.0f);
	glRotatef(-azim / 10.0f, 0.0f, 0.0f, 1.0f);
}

/* lookat(3G): from (vx,vy,vz) towards (px,py,pz), y up, then twisted. */
void
lookat(Coord vx, Coord vy, Coord vz, Coord px, Coord py, Coord pz, Angle twist)
{
	float f[3], up[3], sd[3], u[3], m[16], len;
	int i;

	hgl_iris_ensure();
	glRotatef(-twist / 10.0f, 0.0f, 0.0f, 1.0f);
	f[0] = px - vx; f[1] = py - vy; f[2] = pz - vz;
	len = (float)sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (len == 0.0f)
		return;
	for (i = 0; i < 3; i++)
		f[i] /= len;
	up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
	/* Looking straight up or down the y axis: -z is up on the screen. */
	if (f[0] == 0.0f && f[2] == 0.0f) {
		up[1] = 0.0f;
		up[2] = f[1] > 0 ? 1.0f : -1.0f;
	}
	sd[0] = f[1] * up[2] - f[2] * up[1];
	sd[1] = f[2] * up[0] - f[0] * up[2];
	sd[2] = f[0] * up[1] - f[1] * up[0];
	len = (float)sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
	for (i = 0; i < 3; i++)
		sd[i] /= len;
	u[0] = sd[1] * f[2] - sd[2] * f[1];
	u[1] = sd[2] * f[0] - sd[0] * f[2];
	u[2] = sd[0] * f[1] - sd[1] * f[0];
	memset(m, 0, sizeof m);
	m[0] = sd[0]; m[4] = sd[1]; m[8] = sd[2];
	m[1] = u[0];  m[5] = u[1];  m[9] = u[2];
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
	m[15] = 1.0f;
	glMultMatrixf(m);
	glTranslatef(-vx, -vy, -vz);
}

/*
 * screenspace(3G): screen coordinates for drawing and reading -- a viewport
 * and matrix covering the whole screen, placed so the window's pixels keep
 * their screen positions. Drawing still lands only in the window.
 */
void
screenspace(void)
{
	long ox, oy;
	int sw, sh;

	hgl_iris_ensure();
	getorigin(&ox, &oy);
	sw = (int)getgdesc(GD_XPMAX);
	sh = (int)getgdesc(GD_YPMAX);
	glViewport(-(int)ox, -(int)oy, sw, sh);
	hgl_iris.mask_l = 0;
	hgl_iris.mask_r = hgl_iris.w - 1;
	hgl_iris.mask_b = 0;
	hgl_iris.mask_t = hgl_iris.h - 1;
	hgl_apply_scrmask();
	glLoadIdentity();
	glOrtho(-0.5, sw - 0.5, -0.5, sh - 0.5, -1.0, 1.0);
}

/* fullscrn draws across the whole screen; here the window is all there is
 * to draw into, in screen coordinates. */
void fullscrn(void) { screenspace(); }
void endfullscrn(void) { reshapeviewport(); }

/* ---- rendering state ---- */

static int cull_back, cull_front;

static void
apply_cull(void)
{
	/*
	 * Front faces are counter-clockwise on the screen, as in OpenGL.
	 * buttonfly's button faces are counter-clockwise and it culls back
	 * faces: with clockwise here every face vanished and only the bevels
	 * were left. atlantis, which once seemed to need clockwise, culls per
	 * part and draws the same either way.
	 */
	glFrontFace(GL_CCW);
	if (cull_back && cull_front)
		glCullFace(GL_FRONT_AND_BACK);
	else
		glCullFace(cull_front ? GL_FRONT : GL_BACK);
	if (cull_back || cull_front)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
}

void backface(Boolean b) { hgl_irisgl_tracef("backface %d", (int)b); hgl_iris_ensure(); cull_back = b != 0; apply_cull(); }
void frontface(Boolean b) { hgl_irisgl_tracef("frontface %d", (int)b); hgl_iris_ensure(); cull_front = b != 0; apply_cull(); }
long getbackface(void) { return cull_back; }

Boolean getzbuffer(void) { hgl_iris_ensure(); return (Boolean)glIsEnabled(GL_DEPTH_TEST); }

long
getbuffer(void)
{
	long b = 0;

	if (hgl_iris.want_double && !hgl_iris.front)
		b |= BCKBUFFER;
	if (!hgl_iris.want_double || hgl_iris.front)
		b |= FRNTBUFFER;
	return b;
}

void
logicop(long op)
{
	hgl_iris_ensure();
	op &= 0xf;
	if (op == LO_SRC) {
		glDisable(GL_COLOR_LOGIC_OP);
		return;
	}
	/* LO_ZERO..LO_ONE run in the order of GL_CLEAR..GL_SET. */
	glLogicOp(GL_CLEAR + (GLenum)op);
	glEnable(GL_COLOR_LOGIC_OP);
}

void
polymode(long m)
{
	hgl_iris_ensure();
	glPolygonMode(GL_FRONT_AND_BACK, m == PYM_POINT ? GL_POINT
	    : m == PYM_LINE || m == PYM_HOLLOW || m == PYM_LINE_FAST ? GL_LINE : GL_FILL);
}

void linewidthf(float w) { hgl_iris_ensure(); glLineWidth(w > 0.0f ? w : 1.0f); }
void smoothline(long on) { linesmooth((unsigned long)on); }
void gsync(void) { hgl_iris_ensure(); glFinish(); }

void
displacepolygon(float s)
{
	hgl_iris_ensure();
	if (s == 0.0f) {
		glDisable(GL_POLYGON_OFFSET_FILL);
		return;
	}
	glPolygonOffset(0.0f, s);
	glEnable(GL_POLYGON_OFFSET_FILL);
}

void
clipplane(long index, long mode, const float params[])
{
	GLdouble eq[4];
	hgl_irisgl_tracef("clipplane %ld mode %ld", index, mode);

	hgl_iris_ensure();
	if (index < 0 || index > 5)
		return;
	switch (mode) {
	case CP_DEFINE:
		eq[0] = params[0]; eq[1] = params[1]; eq[2] = params[2]; eq[3] = params[3];
		glClipPlane(GL_CLIP_PLANE0 + (GLenum)index, eq);
		break;
	case CP_ON:
		glEnable(GL_CLIP_PLANE0 + (GLenum)index);
		break;
	default:
		glDisable(GL_CLIP_PLANE0 + (GLenum)index);
		break;
	}
}

/* ---- stencil ----
 * SF_ functions run in the order of GL_NEVER..GL_ALWAYS; ST_ operations are
 * mapped one by one. */
static GLenum
st_op(long op)
{
	static const GLenum ops[6] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT };
	return op >= 0 && op < 6 ? ops[op] : GL_KEEP;
}

void
stencil(long enable, unsigned long ref, long func, unsigned long mask, long fail, long pass, long zpass)
{
	hgl_irisgl_tracef("stencil %ld ref %lu func %ld mask %#lx ops %ld %ld %ld", enable, ref, func, mask, fail, pass, zpass);
	hgl_iris_ensure();
	if (!enable) {
		glDisable(GL_STENCIL_TEST);
		return;
	}
	glStencilFunc(GL_NEVER + (GLenum)(func & 7), (GLint)ref, (GLuint)mask);
	glStencilOp(st_op(fail), st_op(pass), st_op(zpass));
	glEnable(GL_STENCIL_TEST);
}

void swritemask(unsigned long m) { hgl_iris_ensure(); glStencilMask((GLuint)m); }
void stensize(long planes) { (void)planes; /* always 8 here */ }

void
sclear(unsigned long v)
{
	hgl_irisgl_tracef("sclear %lu", v);
	hgl_iris_ensure();
	glClearStencil((GLint)v);
	glClear(GL_STENCIL_BUFFER_BIT);
}

/* ---- attributes ----
 *
 * pushattributes(3G) saves the colour, write masks, line style, pattern,
 * font and buffer choice; OpenGL's attribute stack holds most of it, and
 * what only this library knows goes on a stack beside it.
 */
#define ATTRDEPTH 16
static struct {
	float colour[4];
	long index, lstyle, pattern, font, lsrepeat;
	GLboolean wm[4];
	int front;
} attrs[ATTRDEPTH];
static int attrdepth;

void
pushattributes(void)
{
	hgl_iris_ensure();
	glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_POLYGON_STIPPLE_BIT | GL_POLYGON_BIT |
	    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_ENABLE_BIT);
	if (attrdepth < ATTRDEPTH) {
		hgl_current_colour(attrs[attrdepth].colour);
		attrs[attrdepth].index = hgl_colour_index;
		attrs[attrdepth].lstyle = getlstyle();
		attrs[attrdepth].pattern = getpattern();
		attrs[attrdepth].font = getfont();
		attrs[attrdepth].lsrepeat = getlsrepeat();
		memcpy(attrs[attrdepth].wm, wm, sizeof wm);
		attrs[attrdepth].front = hgl_iris.front;
	}
	attrdepth++;
}

void
popattributes(void)
{
	hgl_iris_ensure();
	if (attrdepth == 0)
		return;
	glPopAttrib();
	if (--attrdepth < ATTRDEPTH) {
		const float *c = attrs[attrdepth].colour;
		hgl_set_colour(c[0], c[1], c[2], c[3]);
		hgl_colour_index = attrs[attrdepth].index;
		lsrepeat(attrs[attrdepth].lsrepeat);
		setlinestyle((short)attrs[attrdepth].lstyle);
		setpattern((short)attrs[attrdepth].pattern);
		font((short)attrs[attrdepth].font);
		memcpy(wm, attrs[attrdepth].wm, sizeof wm);
		hgl_iris.front = attrs[attrdepth].front;
		hgl_apply_colormask();
	}
}

/* ---- windows and screens ---- */

void
winmove(long x, long y)
{
	Display *d = hgl_display();

	hgl_iris_ensure();
	hgl_origin_known();
	/* IRIS GL's screen origin is the bottom left. */
	XMoveWindow(d, hgl_iris.win, (int)x, HeightOfScreen(DefaultScreenOfDisplay(d)) - (int)y - hgl_iris.h);
	XFlush(d);
}

void
winpush(void)
{
	hgl_iris_ensure();
	XLowerWindow(hgl_display(), hgl_iris.win);
	XFlush(hgl_display());
}

/* getport(3G): winopen's old name. */
void getport(String name) { (void)winopen(name); }

void
ginit(void)
{
	prefposition(0, getgdesc(GD_XPMAX) - 1, 0, getgdesc(GD_YPMAX) - 1);
	(void)winopen("");
}
void gbegin(void) { ginit(); }

void
greset(void)
{
	hgl_iris_ensure();
	hgl_window_defaults();
	color(0);
	font(0);
	setlinestyle(0);
	setpattern(0);
	lsrepeat(1);
	drawmode(NORMALDRAW);
	wmpack(0xffffffffUL);
}

long getwscrn(void) { return 0; }
long scrnselect(long s) { return s == 0 ? 0 : -1; }
long scrnattach(long s) { return s == 0 ? 0 : -1; }
Boolean ismex(void) { return FALSE; }
long dglopen(String name, long type) { (void)name; (void)type; return -1; }
void dglclose(long id) { (void)id; }

void
getdepth(Screencoord *n, Screencoord *f)
{
	*n = 0;
	*f = 0x7fff;
}

/* A device's value, whichever kind of device it is. */
void
getdev(long n, const Device devs[], short vals[])
{
	long i;

	for (i = 0; i < n; i++) {
		if (devs[i] == MOUSEX || devs[i] == MOUSEY)
			vals[i] = (short)getvaluator(devs[i]);
		else
			vals[i] = (short)getbutton(devs[i]);
	}
}

/* The cursor is the server's default: glyph 0, visible. */
void
getcursor(short *index, Colorindex *colour, Colorindex *wtm, Boolean *b)
{
	if (index) *index = 0;
	if (colour) *colour = 1;
	if (wtm) *wtm = 0xfff;
	if (b) *b = TRUE;
}

long getmonitor(void) { return HZ60; }
long getothermonitor(void) { return HZ60; }
Boolean getmultisample(void) { return FALSE; }
Boolean getresetls(void) { return TRUE; }
Boolean getlsbackup(void) { return FALSE; }
Boolean getdcm(void) { return FALSE; }
void mswapbuffers(long fbuf) { (void)fbuf; swapbuffers(); }

/*
 * Accepted and ignored: hardware this display does not have (the keyboard's
 * lamps and click, video formats and monitor timing, blanking, stereo,
 * cursor planes, gamma tables, depth-cue and multisample controls, DGL
 * lights), and the old text-port calls.
 */
void setbell(Byte b) { (void)b; }
void clkon(void) { }
void clkoff(void) { }
void lampon(Byte b) { (void)b; }
void lampoff(Byte b) { (void)b; }
void blanktime(long n) { (void)n; }
void blankscreen(Boolean b) { (void)b; }
void setmonitor(short m) { (void)m; }
void setvideo(long r, long v) { (void)r; (void)v; }
long getvideo(long r) { (void)r; return -1; }
void videocmd(long c) { (void)c; }
void monobuffer(void) { }
void stereobuffer(void) { }
void leftbuffer(Boolean b) { (void)b; }
void rightbuffer(Boolean b) { (void)b; }
void RGBsize(unsigned long planes) { (void)planes; }
void gammaramp(const short r[256], const short g[256], const short b[256]) { (void)r; (void)g; (void)b; }
void attachcursor(Device a, Device b) { (void)a; (void)b; }
void RGBcursor(short a, short b, short c, short d, short e, short f, short g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; }
void blink(short a, Colorindex b, short c, short d, short e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void cyclemap(short a, short b, short c) { (void)a; (void)b; (void)c; }
void depthcue(Boolean b) { (void)b; }
void multisample(Boolean b) { (void)b; }
void msalpha(long m) { (void)m; }
void msmask(float v, Boolean b) { (void)v; (void)b; }
void mspattern(long p) { (void)p; }
void mssample(long s) { (void)s; }
/*
 * nmode(3G): NAUTO, the default, renormalizes normals when the matrix would
 * change their length, NNORMALIZE always does; either way lighting sees unit
 * normals, which GL_NORMALIZE gives. Without it a program that squashes its
 * model -- demograph starts its map at scale(1, 1, 1e-8) -- has its normals
 * stretched as much, and every lit surface comes out white.
 */
void
nmode(long m)
{
	(void)m;
	hgl_iris_ensure();
	glEnable(GL_NORMALIZE);
}
void pagecolor(Colorindex c) { (void)c; }
void textcolor(Colorindex c) { (void)c; }
void textinit(void) { }
void textport(Screencoord a, Screencoord b, Screencoord c, Screencoord d) { (void)a; (void)b; (void)c; (void)d; }
void tpon(void) { }
void tpoff(void) { }
void setdblights(unsigned long m) { (void)m; }
void lsbackup(Boolean b) { (void)b; }
void resetls(Boolean b) { (void)b; }
void chunksize(long n) { (void)n; }
