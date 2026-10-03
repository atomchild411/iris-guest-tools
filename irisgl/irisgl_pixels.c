/*
 * IRIS GL's pixel calls, and the few internal libgl entry points SGI's own
 * libraries import, as OpenGL. See irisgl_shim.h.
 *
 * Pixels (lrectwrite(3G), lrectread(3G), pixmode(3G), writepixels(3G) ...):
 * all coordinates are window pixels, bottom left (0,0), so a transfer puts
 * the raster position there with a pixel-exact projection of its own and
 * puts the program's matrices and viewport back afterwards. lrectwrite and
 * lrectread take 32-bit words, 0xAABBGGRR in RGB mode (OpenGL's ABGR_EXT
 * byte order on this big-endian machine, so the default transfer is passed
 * straight through) and colour indices in colour map mode, which here is a
 * map of our own (the framebuffer is always RGB): indices are looked up on
 * the way in and the nearest index found on the way out. pixmode's packing
 * (PM_SIZE, PM_OFFSET, PM_STRIDE), directions (PM_TTOB, PM_RTOL), PM_SHIFT,
 * PM_EXPAND/PM_C0/PM_C1 and PM_ADD24 are done here in the guest; PM_ZDATA and
 * the format/type modes are not (said once). rectzoom is glPixelZoom, which
 * writepixels and writeRGB, which IRIS GL does not zoom, set aside.
 */
#include "irisgl_shim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_ABGR_EXT
#define GL_ABGR_EXT 0x8000
#endif

static float zoom_x = 1.0f, zoom_y = 1.0f;
static long read_source = SRC_AUTO;

/* pixmode's parameters, by PM_ number. */
static long pm[18] = { 0, 0, 0, 0, 0, 32 };
static int pm_formats_default = 1;

static void
say_once(const char *what)
{
	static const char *said[32];
	static int n;
	int i;

	for (i = 0; i < n; i++)
		if (said[i] == what)
			return;
	if (n < 32)
		said[n++] = what;
	fprintf(stderr, "IRIS GL: %s\n", what);
}

/* The transfer's words need nothing done to them. */
static int
pm_plain(void)
{
	return pm[PM_SHIFT] == 0 && pm[PM_EXPAND] == 0 && pm[PM_ADD24] == 0 && pm[PM_SIZE] == 32
	    && pm[PM_OFFSET] == 0 && pm[PM_STRIDE] == 0 && pm[PM_TTOB] == 0 && pm[PM_RTOL] == 0;
}

void
pixmode(long mode, long value)
{
	TRACE("pixmode");
	switch (mode) {
	case PM_SIZE:
		if (value == 1 || value == 2 || value == 4 || value == 8 || value == 12 || value == 16
		    || value == 24 || value == 32)
			pm[PM_SIZE] = value;
		return;
	case PM_OFFSET:
		if (value >= 0 && value <= 31)
			pm[PM_OFFSET] = value;
		return;
	case PM_SHIFT: case PM_EXPAND: case PM_C0: case PM_C1: case PM_ADD24:
	case PM_STRIDE: case PM_TTOB: case PM_RTOL: case PM_ZDATA:
		pm[mode] = value;
		return;
	case PM_INPUT_FORMAT: case PM_INPUT_TYPE: case PM_OUTPUT_FORMAT: case PM_OUTPUT_TYPE:
		/* PM_ABGR and PM_UNSIGNED_BYTE (both 0) are the defaults. */
		pm_formats_default = value == 0;
		if (value != 0)
			say_once("pixmode's input and output formats and types are not implemented");
		return;
	}
	say_once("pixmode: a mode that is not implemented was ignored");
}

void
rectzoom(float xf, float yf)
{
	TRACE("rectzoom");
	hgl_iris_ensure();
	zoom_x = xf;
	zoom_y = yf;
	glPixelZoom(xf, yf);
}

void
readsource(long src)
{
	TRACE("readsource");
	hgl_iris_ensure();
	read_source = src;
	/* The host's front and back are one drawable; the choice is kept for
	 * the z-buffer and for what a program asks back. */
	/* A single-buffered window draws into the GL back buffer, which is
	 * its front as the program sees it. */
	if ((src == SRC_FRONT || src == SRC_FRONTRIGHT) && hgl_iris.want_double)
		glReadBuffer(GL_FRONT);
	else if (src != SRC_ZBUFFER)
		glReadBuffer(GL_BACK);
}

/*
 * The raster position at window pixel (x, y), with a projection, modelview
 * and viewport that make that exact; raster_done puts the program's back. A
 * position outside the window is reached by moving a valid one with a bitmap
 * of no pixels, which OpenGL allows and which keeps the position valid.
 */
/* What raster_at turned off: IRIS GL's pixel writes are not textured,
 * fogged or lit, and OpenGL would do all three to glDrawPixels's fragments
 * (an image written while a texture is bound came out as one texel). */
static int raster_tex, raster_fog, raster_lit;

static void
raster_at(int x, int y)
{
	int w = hgl_iris.w > 0 ? hgl_iris.w : 1, h = hgl_iris.h > 0 ? hgl_iris.h : 1;

	raster_tex = hgl_suspend(GL_TEXTURE_2D);
	raster_fog = hgl_suspend(GL_FOG);
	raster_lit = hgl_suspend(GL_LIGHTING);

	glPushAttrib(GL_TRANSFORM_BIT | GL_VIEWPORT_BIT);
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, w, 0, h, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	if (x >= 0 && y >= 0 && x < w && y < h) {
		glRasterPos2i(x, y);
	} else {
		glRasterPos2i(0, 0);
		glBitmap(0, 0, 0, 0, (GLfloat)x, (GLfloat)y, NULL);
	}
	hgl_raster_serial = hgl_colour_serial;
}

static void
raster_done(void)
{
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopAttrib();
	hgl_resume(GL_LIGHTING, raster_lit);
	hgl_resume(GL_FOG, raster_fog);
	hgl_resume(GL_TEXTURE_2D, raster_tex);
}

/* Corners in either order: the lower left, and the size. */
static int
span(Screencoord a, Screencoord b, int *lo, int *n)
{
	*lo = a < b ? a : b;
	*n = (a < b ? b - a : a - b) + 1;
	return *n > 0;
}

/* ---- packing ----
 *
 * pixmode(3G): pixels are packed big-endian into 32-bit words with no padding
 * between them, PM_OFFSET bits skipped at the start of each scanline, and a
 * scanline of PM_STRIDE words (0: exactly the words it needs).
 */
static unsigned long
row_words(int w)
{
	unsigned long bits = (unsigned long)pm[PM_OFFSET] + (unsigned long)w * pm[PM_SIZE];

	return pm[PM_STRIDE] > 0 ? (unsigned long)pm[PM_STRIDE] : (bits + 31) / 32;
}

static unsigned long
get_bits(const unsigned char *p, unsigned long bit, int size)
{
	unsigned long v = 0;
	int k;

	if (bit % 8 == 0 && size % 8 == 0) {
		for (k = 0; k < size; k += 8)
			v = v << 8 | p[(bit + k) / 8];
		return v;
	}
	for (k = 0; k < size; k++, bit++)
		v = v << 1 | ((p[bit / 8] >> (7 - bit % 8)) & 1);
	return v;
}

static void
put_bits(unsigned char *p, unsigned long bit, int size, unsigned long v)
{
	int k;

	for (k = size - 1; k >= 0; k--, bit++) {
		unsigned char m = (unsigned char)(0x80 >> (bit % 8));

		if ((v >> k) & 1)
			p[bit / 8] |= m;
		else
			p[bit / 8] &= (unsigned char)~m;
	}
}

static unsigned long
shift_add(unsigned long v, int reading)
{
	long s = pm[PM_SHIFT], low;

	if (reading)
		s = -s;
	if (s > 0)
		v = s >= 32 ? 0 : v << s;
	else if (s < 0)
		v = -s >= 32 ? 0 : v >> -s;
	v &= 0xffffffffUL;
	if (pm[PM_EXPAND])
		v = (v & 1) ? (unsigned long)pm[PM_C1] : (unsigned long)pm[PM_C0];
	if (pm[PM_ADD24]) {
		low = (long)(v & 0xffffff);
		if (low & 0x800000)
			low -= 0x1000000;
		low += pm[PM_ADD24];
		if (low < -0x800000)
			low = -0x800000;
		if (low > 0x7fffff)
			low = 0x7fffff;
		v = (v & 0xff000000UL) | ((unsigned long)low & 0xffffff);
	}
	return v & 0xffffffffUL;
}

/* A 32-bit pixel (0xAABBGGRR, or an index in colour map mode) as RGBA bytes. */
static void
to_rgba(unsigned long v, unsigned char *out, int index)
{
	if (index) {
		hgl_cmap_rgb(v, out);
		out[3] = 0xff;
		return;
	}
	out[0] = (unsigned char)v;
	out[1] = (unsigned char)(v >> 8);
	out[2] = (unsigned char)(v >> 16);
	out[3] = (unsigned char)(v >> 24);
}

static unsigned long
from_rgba(const unsigned char *in, int index)
{
	if (index)
		return hgl_cmap_index(in[0], in[1], in[2]);
	return (unsigned long)in[0] | (unsigned long)in[1] << 8 | (unsigned long)in[2] << 16
	    | (unsigned long)in[3] << 24;
}

/* RGBA bytes of w x h pixels into the window at (x, y), zoomed. */
static void
draw_rgba(int x, int y, int w, int h, const unsigned char *rgba)
{
	raster_at(x, y);
	glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	raster_done();
}

static unsigned char *
scratch(unsigned long bytes)
{
	static unsigned char *buf;
	static unsigned long have;

	if (bytes > have) {
		free(buf);
		buf = malloc(bytes);
		have = buf != NULL ? bytes : 0;
	}
	return buf;
}

/*
 * pixmode(PM_ZDATA, 1): the pixels are z values, in the screen z units
 * getgdesc(GD_ZMAX) reports, and colour is never written (pixmode(3G)).
 * With zbuffer on they are compared as usual; with it off they are written
 * as they are -- and OpenGL writes depth only while its depth test is on,
 * so that is an always-passing test.
 */
static void
z_write(int x, int y, int w, int h, const unsigned long *parray)
{
	float *z;
	unsigned long words, bit;
	int r, c;

	if ((z = (float *)malloc((size_t)w * h * sizeof *z)) == NULL)
		return;
	words = row_words(w);
	for (r = 0; r < h; r++) {
		int dr = pm[PM_TTOB] ? h - 1 - r : r;
		const unsigned char *row = (const unsigned char *)parray + (unsigned long)r * words * 4;

		bit = (unsigned long)pm[PM_OFFSET];
		for (c = 0; c < w; c++, bit += pm[PM_SIZE]) {
			int dc = pm[PM_RTOL] ? w - 1 - c : c;
			unsigned long v = shift_add(get_bits(row, bit, (int)pm[PM_SIZE]), 0) & 0xffffff;
			float d = (float)v / (float)0x7fffff;

			z[(unsigned long)dr * w + dc] = d > 1.0f ? 1.0f : d;
		}
	}
	glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_TRUE);
	if (!(hgl_iris.enables & 1u)) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
	}
	raster_at(x, y);
	glDrawPixels(w, h, GL_DEPTH_COMPONENT, GL_FLOAT, z);
	raster_done();
	glPopAttrib();
	free(z);
}

void
lrectwrite(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, const unsigned long *parray)
{
	int x, y, w, h, r, c, index = !hgl_iris.want_rgb;
	unsigned long words, bit;
	unsigned char *rgba;
	const unsigned char *row;

	TRACE("lrectwrite");
	if (parray == NULL || !span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return;
	hgl_iris_ensure();
	/* x1, y1 is the lower left of what is filled, whatever the zoom. */
	x = x1;
	y = y1;
	if (pm[PM_ZDATA]) {
		z_write(x, y, w, h, parray);
		return;
	}
	if (!index && pm_plain() && pm_formats_default) {
		/* 0xAABBGGRR in memory is the bytes A, B, G, R. */
		raster_at(x, y);
		glDrawPixels(w, h, GL_ABGR_EXT, GL_UNSIGNED_BYTE, parray);
		raster_done();
		return;
	}
	if ((rgba = scratch((unsigned long)w * h * 4)) == NULL)
		return;
	words = row_words(w);
	for (r = 0; r < h; r++) {
		int dr = pm[PM_TTOB] ? h - 1 - r : r;

		row = (const unsigned char *)parray + (unsigned long)r * words * 4;
		bit = (unsigned long)pm[PM_OFFSET];
		for (c = 0; c < w; c++, bit += pm[PM_SIZE]) {
			int dc = pm[PM_RTOL] ? w - 1 - c : c;
			unsigned long v = get_bits(row, bit, (int)pm[PM_SIZE]);

			to_rgba(shift_add(v, 0), rgba + ((unsigned long)dr * w + dc) * 4, index);
		}
	}
	draw_rgba(x, y, w, h, rgba);
}

void
rectwrite(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, const Colorindex *parray)
{
	int x, y, w, h;
	unsigned long i;
	unsigned char *rgba;

	TRACE("rectwrite");
	if (parray == NULL || !span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return;
	hgl_iris_ensure();
	if ((rgba = scratch((unsigned long)w * h * 4)) == NULL)
		return;
	for (i = 0; i < (unsigned long)w * h; i++)
		to_rgba(parray[i], rgba + i * 4, 1);
	draw_rgba(x1, y1, w, h, rgba);
}

/* RGBA bytes (or depth) of w x h window pixels at (x, y). */
static unsigned char *
read_rgba(int x, int y, int w, int h, int depth)
{
	unsigned char *buf = scratch((unsigned long)w * h * 4);

	if (buf == NULL)
		return NULL;
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	if (depth)
		glReadPixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, buf);
	else
		glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	return buf;
}

long
lrectread(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, unsigned long *parray)
{
	int x, y, w, h, r, c, depth = read_source == SRC_ZBUFFER, index = !hgl_iris.want_rgb && !depth;
	unsigned long words, bit;
	unsigned char *px, *row;

	TRACE("lrectread");
	if (!span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return 0;
	if (parray == NULL)
		return (long)w * h;
	hgl_iris_ensure();
	if (!depth && !index && pm_plain() && pm_formats_default) {
		/* RGBA bytes turned round are A, B, G, R: 0xAABBGGRR. Read as RGBA
		 * rather than ABGR_EXT, which the host can only do by first reading
		 * the whole buffer from this process (the bytes between pixels are
		 * the program's). */
		unsigned char *p = (unsigned char *)parray, t;
		unsigned long i, n = (unsigned long)w * h;

		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(x1, y1, w, h, GL_RGBA, GL_UNSIGNED_BYTE, parray);
		for (i = 0; i < n; i++, p += 4) {
			t = p[0]; p[0] = p[3]; p[3] = t;
			t = p[1]; p[1] = p[2]; p[2] = t;
		}
		return (long)n;
	}
	if ((px = read_rgba(x1, y1, w, h, depth)) == NULL)
		return (long)w * h;
	words = row_words(w);
	for (r = 0; r < h; r++) {
		int sr = pm[PM_TTOB] ? h - 1 - r : r;

		row = (unsigned char *)parray + (unsigned long)r * words * 4;
		bit = (unsigned long)pm[PM_OFFSET];
		for (c = 0; c < w; c++, bit += pm[PM_SIZE]) {
			int sc = pm[PM_RTOL] ? w - 1 - c : c;
			const unsigned char *p = px + ((unsigned long)sr * w + sc) * 4;
			unsigned long v;

			if (depth)  /* screen z: 24 bits (getgdesc GD_ZMAX 0x7fffff, signed) */
				v = ((unsigned long)p[0] << 24 | (unsigned long)p[1] << 16
				    | (unsigned long)p[2] << 8 | p[3]) >> 9;
			else
				v = from_rgba(p, index);
			put_bits(row, bit, (int)pm[PM_SIZE], shift_add(v, 1));
		}
	}
	return (long)w * h;
}

long
rectread(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, Colorindex *parray)
{
	int x, y, w, h;
	unsigned long i;
	unsigned char *px;

	TRACE("rectread");
	if (!span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return 0;
	if (parray == NULL)
		return (long)w * h;
	hgl_iris_ensure();
	if ((px = read_rgba(x1, y1, w, h, 0)) == NULL)
		return (long)w * h;
	for (i = 0; i < (unsigned long)w * h; i++)
		parray[i] = (Colorindex)hgl_cmap_index(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]);
	return (long)w * h;
}

void
rectcopy(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, Screencoord newx, Screencoord newy)
{
	int x, y, w, h;

	TRACE("rectcopy");
	if (!span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return;
	hgl_iris_ensure();
	raster_at(newx, newy);
	glCopyPixels(x, y, w, h, read_source == SRC_ZBUFFER && pm[PM_ZDATA] ? GL_DEPTH : GL_COLOR);
	raster_done();
}

/* ---- rows at the character position ----
 *
 * writepixels, writeRGB, readpixels and readRGB work along one scanline from
 * the current character position -- OpenGL's raster position -- and leave it
 * one pixel past the last. IRIS GL does not zoom them.
 */

static void
row_draw(int n, const unsigned char *rgba)
{
	int zoomed = zoom_x != 1.0f || zoom_y != 1.0f;

	if (zoomed)
		glPixelZoom(1.0f, 1.0f);
	glDrawPixels(n, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (zoomed)
		glPixelZoom(zoom_x, zoom_y);
	glBitmap(0, 0, 0, 0, (GLfloat)n, 0, NULL);
}

void
writepixels(short n, const Colorindex *colors)
{
	unsigned char *rgba;
	int i;

	TRACE("writepixels");
	if (n <= 0 || colors == NULL)
		return;
	hgl_iris_ensure();
	if ((rgba = scratch((unsigned long)n * 4)) == NULL)
		return;
	for (i = 0; i < n; i++)
		to_rgba(colors[i], rgba + i * 4, 1);
	row_draw(n, rgba);
}

void
writeRGB(short n, const RGBvalue *r, const RGBvalue *g, const RGBvalue *b)
{
	unsigned char *rgba;
	int i;

	TRACE("writeRGB");
	if (n <= 0 || r == NULL || g == NULL || b == NULL)
		return;
	hgl_iris_ensure();
	if ((rgba = scratch((unsigned long)n * 4)) == NULL)
		return;
	for (i = 0; i < n; i++) {
		rgba[i * 4] = r[i];
		rgba[i * 4 + 1] = g[i];
		rgba[i * 4 + 2] = b[i];
		rgba[i * 4 + 3] = 0xff;
	}
	row_draw(n, rgba);
}

/* n pixels from the character position, which is then moved past them; 0 if
 * the position is not valid. */
static unsigned char *
row_read(int n)
{
	GLfloat pos[4];
	GLboolean valid = GL_FALSE;
	unsigned char *px;

	hgl_iris_ensure();
	glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
	if (!valid)
		return NULL;
	glGetFloatv(GL_CURRENT_RASTER_POSITION, pos);
	px = read_rgba((int)pos[0], (int)pos[1], n, 1, 0);
	glBitmap(0, 0, 0, 0, (GLfloat)n, 0, NULL);
	return px;
}

long
readpixels(short n, Colorindex *colors)
{
	unsigned char *px;
	int i;

	TRACE("readpixels");
	if (n <= 0 || colors == NULL || (px = row_read(n)) == NULL)
		return 0;
	for (i = 0; i < n; i++)
		colors[i] = (Colorindex)hgl_cmap_index(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]);
	return n;
}

long
readRGB(short n, RGBvalue *r, RGBvalue *g, RGBvalue *b)
{
	unsigned char *px;
	int i;

	TRACE("readRGB");
	if (n <= 0 || r == NULL || g == NULL || b == NULL || (px = row_read(n)) == NULL)
		return 0;
	for (i = 0; i < n; i++) {
		r[i] = px[i * 4];
		g[i] = px[i * 4 + 1];
		b[i] = px[i * 4 + 2];
	}
	return n;
}

/*
 * readdisplay(3G) reads the screen, not a window. Only this program's window
 * is ours to read; the pixels of the rest of the screen come back black.
 */
long
readdisplay(Screencoord x1, Screencoord y1, Screencoord x2, Screencoord y2, unsigned long *parray,
    unsigned long hints)
{
	long ox = 0, oy = 0;
	int x, y, w, h, wx, wy, r, c;
	unsigned char *px;

	TRACE("readdisplay");
	(void)hints;
	if (!span(x1, x2, &x, &w) || !span(y1, y2, &y, &h))
		return 0;
	if (parray == NULL)
		return (long)w * h;
	hgl_iris_ensure();
	say_once("readdisplay reads only this program's own window; the rest of the screen reads black");
	/* The window's bottom left, in IRIS GL's screen coordinates (y up). */
	getorigin(&ox, &oy);
	wx = x - (int)ox;
	wy = y - (int)oy;
	for (r = 0; r < h * w; r++)
		parray[r] = 0xff000000UL;
	{
		int cx0 = wx < 0 ? 0 : wx, cy0 = wy < 0 ? 0 : wy;
		int cx1 = wx + w > hgl_iris.w ? hgl_iris.w : wx + w;
		int cy1 = wy + h > hgl_iris.h ? hgl_iris.h : wy + h;

		if (cx1 <= cx0 || cy1 <= cy0 || (px = read_rgba(cx0, cy0, cx1 - cx0, cy1 - cy0, 0)) == NULL)
			return (long)w * h;
		for (r = cy0; r < cy1; r++)
			for (c = cx0; c < cx1; c++) {
				const unsigned char *p = px + ((unsigned long)(r - cy0) * (cx1 - cx0) + (c - cx0)) * 4;

				parray[(unsigned long)(r - wy) * w + (c - wx)] = (unsigned long)p[0]
				    | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | 0xff000000UL;
			}
	}
	return (long)w * h;
}

/* ---- libgl's internal entry points, imported by SGI's own libraries ----
 *
 * Not in <gl/gl.h>, but exported by the real libgl.so, which has local ("g")
 * and remote DGL ("d") versions of each. SGI's libraries import them, and
 * fail to load without them.
 */

/* Whether graphics are local rather than through DGL. libfm, libil and
 * showcase ask; here the drawing is always local to the program. */
long
gl_islocal(void)
{
	return 1;
}

/* A sine and cosine of an angle in tenths of a degree (gl_g_gl_sincos is a
 * 0.1-degree table); either pointer may be null. buttonfly and jello use it. */
void
gl_sincos(long angle, float *s, float *c)
{
	long a = angle % 3600;
	double rad;

	if (a < 0)
		a += 3600;
	rad = (double)a * M_PI / 1800.0;
	if (s != NULL)
		*s = (float)sin(rad);
	if (c != NULL)
		*c = (float)cos(rad);
}

/* readRGB by another name: gl_g_gl_readscreen reads n pixels from the
 * character position with lrectread and splits them into r, g, b. imged. */
long
gl_readscreen(short n, RGBvalue *r, RGBvalue *g, RGBvalue *b)
{
	return readRGB(n, r, g, b);
}

/* Performer after a multisample clear; the DGL form sends an opcode with no
 * arguments. Nothing to do on a host that resolves its own samples. */
void
gl_ms_init_pixels(void)
{
}

/* libfm brackets a string with these; SGI's are empty functions. */
void gl_beginstring(void) { }
void gl_endstring(void) { }

/*
 * libfm's glyph (__fm_bmnew in libfm.so): the rows of the bitmap bottom to
 * top, each `words` 16-bit words, leftmost pixel in the top bit, as IRIS GL's
 * raster fonts are (defrasterfont(3G)); drawn at the character position less
 * the origin, which then moves by (xmove, ymove) -- exactly glBitmap.
 */
struct hgl_fm_bitmap {
	unsigned short *bits;
	short w, h, xorig, yorig, xmove, ymove, words;
};

void
gl_drawbitmap(const struct hgl_fm_bitmap *b)
{
	unsigned char *copy = NULL;
	const unsigned char *bits;
	int row, align = 2, r;

	TRACE("gl_drawbitmap");
	if (b == NULL)
		return;
	hgl_iris_ensure();
	hgl_latch_raster_colour();
	if (b->w <= 0 || b->h <= 0 || b->bits == NULL) {
		glBitmap(0, 0, 0, 0, b->xmove, b->ymove, NULL);
		return;
	}
	bits = (const unsigned char *)b->bits;
	row = (b->w + 7) / 8;
	/* Rows of exactly the 16-bit words the width needs are glBitmap's rows
	 * at an alignment of 2; any other word count is copied down to rows of
	 * whole bytes. */
	if (b->words != (b->w + 15) / 16) {
		if (b->words < (b->w + 15) / 16 || (copy = malloc((size_t)row * b->h)) == NULL)
			return;
		for (r = 0; r < b->h; r++)
			memcpy(copy + (size_t)r * row, bits + (size_t)r * b->words * 2, (size_t)row);
		bits = copy;
		align = 1;
	}
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glBitmap(b->w, b->h, b->xorig, b->yorig, b->xmove, b->ymove, bits);
	glPopClientAttrib();
	free(copy);
}

/* ---- small calls the demos on this image import ---- */

void
winposition(long x1, long x2, long y1, long y2)
{
	int x = (int)(x1 < x2 ? x1 : x2), w = (int)(x1 < x2 ? x2 - x1 : x1 - x2) + 1;
	int y = (int)(y1 < y2 ? y1 : y2), h = (int)(y1 < y2 ? y2 - y1 : y1 - y2) + 1;

	TRACE("winposition");
	if (!hgl_iris.opened) {
		prefposition(x, x + w - 1, y, y + h - 1);
		return;
	}
	/* IRIS GL's screen y grows upwards from the bottom. */
	hgl_origin_known();
	XMoveResizeWindow(hgl_display(), hgl_iris.win, x,
	    HeightOfScreen(DefaultScreenOfDisplay(hgl_display())) - y - h, (unsigned)w, (unsigned)h);
	XFlush(hgl_display());
}

void
winpop(void)
{
	TRACE("winpop");
	if (hgl_iris.opened) {
		XRaiseWindow(hgl_display(), hgl_iris.win);
		XFlush(hgl_display());
	}
}

/* Drawing into the back buffer is what a double-buffered program does
 * anyway; turning it off leaves frontbuffer to say where drawing goes. */
void
backbuffer(Boolean b)
{
	TRACE("backbuffer");
	if (b)
		hgl_iris.front = 0;
}

void
gexit(void)
{
	TRACE("gexit");
	if (hgl_iris.opened) {
		glFinish();
		XFlush(hgl_display());
	}
}

/* Colour commands while lighting is on: which material property they set. */
void
lmcolor(long mode)
{
	static const GLenum what[6] = { 0, GL_EMISSION, GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR,
	    GL_AMBIENT_AND_DIFFUSE };

	hgl_irisgl_tracef("lmcolor %ld", mode);
	hgl_iris_ensure();
	hgl_lmcolor_mode = mode;
	if (mode > LMC_COLOR && mode <= LMC_AD) {
		glColorMaterial(hgl_back_material_bound() ? GL_FRONT : GL_FRONT_AND_BACK, what[mode]);
		glEnable(GL_COLOR_MATERIAL);
	} else {
		/* LMC_COLOR, and LMC_NULL: colour commands leave materials alone. */
		glDisable(GL_COLOR_MATERIAL);
	}
}

void cmovi(Icoord x, Icoord y, Icoord z) { hgl_iris_ensure(); hgl_rasterpos((float)x, (float)y, (float)z); hgl_raster_serial = hgl_colour_serial; }
void cmovs(Scoord x, Scoord y, Scoord z) { hgl_iris_ensure(); hgl_rasterpos((float)x, (float)y, (float)z); hgl_raster_serial = hgl_colour_serial; }

/* ---- accumulation (acbuf(3G)) ----
 *
 * IRIS GL counts accumulation values in 0..255 per component, OpenGL in
 * 0..1; the operations are glAccum's, one for one, where the context has
 * an accumulation buffer. The host's has none, so the buffer is kept here:
 * floats per component of the window, filled from the read buffer and
 * written back with glDrawPixels -- slower, but acbuf runs a few times a
 * frame (jittered antialiasing, motion blur).
 */
static long ac_planes;
static float *acc;
static int acc_w, acc_h, acc_host = -1;

void
acsize(long planes)
{
	ac_planes = planes;
}

static int
acc_ready(void)
{
	int w = hgl_iris.w, h = hgl_iris.h;

	if (w <= 0 || h <= 0)
		return 0;
	if (acc == NULL || acc_w != w || acc_h != h) {
		free(acc);
		acc = (float *)calloc((size_t)w * h * 4, sizeof *acc);
		acc_w = acc == NULL ? 0 : w;
		acc_h = acc == NULL ? 0 : h;
	}
	return acc != NULL;
}

/* The read buffer's pixels times v, into the buffer (or over it). */
static void
acc_take(float v, int load)
{
	unsigned char *px;
	size_t i, n;

	if (!acc_ready() || (px = scratch((unsigned long)acc_w * acc_h * 4)) == NULL)
		return;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, acc_w, acc_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	n = (size_t)acc_w * acc_h * 4;
	for (i = 0; i < n; i++)
		acc[i] = (load ? 0.0f : acc[i]) + v * (px[i] / 255.0f);
}

/* The buffer times v, clamped, written over the window: nothing but the
 * screenmask and writemask applies (acbuf(3G)). */
static void
acc_return(float v)
{
	static const GLenum off[] = {
		GL_DEPTH_TEST, GL_BLEND, GL_COLOR_LOGIC_OP, GL_ALPHA_TEST, GL_STENCIL_TEST, GL_DITHER
	};
	unsigned char *px;
	size_t i, n;
	int k;

	if (!acc_ready() || (px = scratch((unsigned long)acc_w * acc_h * 4)) == NULL)
		return;
	n = (size_t)acc_w * acc_h * 4;
	for (i = 0; i < n; i++) {
		float c = acc[i] * v;
		px[i] = (unsigned char)(c <= 0.0f ? 0 : c >= 1.0f ? 255 : c * 255.0f + 0.5f);
	}
	glPushAttrib(GL_ENABLE_BIT | GL_PIXEL_MODE_BIT);
	for (k = 0; k < (int)(sizeof off / sizeof off[0]); k++)
		glDisable(off[k]);
	glPixelZoom(1.0f, 1.0f);
	raster_at(0, 0);
	glDrawPixels(acc_w, acc_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	raster_done();
	glPopAttrib();
}

void
acbuf(long op, float value)
{
	float v;
	size_t i, n;

	hgl_irisgl_tracef("acbuf %ld %g", op, value);
	hgl_iris_ensure();
	if (hgl_selecting)
		return;
	if (acc_host < 0) {
		GLint bits = 0;
		glGetIntegerv(GL_ACCUM_RED_BITS, &bits);
		acc_host = bits > 0;
	}
	if (op == AC_ACCUMULATE || op == AC_CLEAR_ACCUMULATE)
		value = value < -256.0f ? -256.0f : value > 256.0f ? 256.0f : value;
	if (op == AC_RETURN)
		value = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	if (acc_host) {
		switch (op) {
		case AC_CLEAR:
			v = value / 255.0f;
			glClearAccum(v, v, v, v);
			glClear(GL_ACCUM_BUFFER_BIT);
			break;
		case AC_ACCUMULATE: glAccum(GL_ACCUM, value); break;
		case AC_CLEAR_ACCUMULATE: glAccum(GL_LOAD, value); break;
		case AC_RETURN: glAccum(GL_RETURN, value); break;
		case AC_MULT: glAccum(GL_MULT, value); break;
		case AC_ADD: glAccum(GL_ADD, value / 255.0f); break;
		}
		return;
	}
	switch (op) {
	case AC_CLEAR:
	case AC_ADD:
		if (!acc_ready())
			return;
		n = (size_t)acc_w * acc_h * 4;
		for (i = 0; i < n; i++)
			acc[i] = (op == AC_ADD ? acc[i] : 0.0f) + value / 255.0f;
		break;
	case AC_ACCUMULATE: acc_take(value, 0); break;
	case AC_CLEAR_ACCUMULATE: acc_take(value, 1); break;
	case AC_RETURN: acc_return(value); break;
	case AC_MULT:
		if (!acc_ready())
			return;
		n = (size_t)acc_w * acc_h * 4;
		for (i = 0; i < n; i++)
			acc[i] *= value;
		break;
	}
}
