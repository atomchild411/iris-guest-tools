/*
 * IRIS GL's raster fonts: font 0, defrasterfont, and the text calls that
 * draw and measure with the current font. See irisgl_shim.h.
 *
 * Characters are bitmaps drawn at the current character position, which is
 * OpenGL's raster position, one display list per character. The lists are
 * made in whichever context is current when a font is first used; every
 * context shares lists with the first (irisgl_rt.c), so one set serves every
 * window.
 */
#include "irisgl_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Font 0 is IRIS GL's own fixed-pitch font, 9 pixels wide and 15 high with a
 * descender of 2 (as getheight, strwidth and getdescender answer on SGI's
 * library). Its glyphs live inside SGI's libgl.so; the nearest the X server
 * has is misc-fixed 9x15 bold, the same cell and stroke weight, which is
 * drawn here. Metrics are
 * reported as IRIS GL's so programs lay text out the same way.
 */
#define FONT0_WIDTH 9
#define FONT0_HEIGHT 15
#define FONT0_DESCENT 2

#define MAXFONT 64

typedef struct {
	int defined;
	short ht, nc;
	Fontchar *chars;            /* a copy: the program's array may go away */
	unsigned short *raster;
	long nr;
	GLuint lists;               /* 0 until built in a context */
	short descent;
} RasterFont;

static RasterFont fonts[MAXFONT];
static short cur_font;

static XFontStruct *font0;
static GLuint font0_lists;

/*
 * Font 0's lists: each glyph drawn by the X server into a pixmap, read back
 * and kept as a bitmap cut to the glyph's own ink box, so the origin sits
 * exactly on the baseline. (glXUseXFont in the OpenGL shim sizes every
 * bitmap to the font's largest box but places it by the glyph's own descent,
 * which draws most characters several pixels above the character position.)
 */
static void
build_font0(Display *d, XFontStruct *fs, GLuint base)
{
	int bw = fs->max_bounds.rbearing - fs->min_bounds.lbearing;
	int bh = fs->max_bounds.ascent + fs->max_bounds.descent;
	int c, x, y, gw, gh, stride;
	Pixmap pm;
	GC gc;
	XGCValues gv;
	XImage *img;
	unsigned char *bits;

	if (bw <= 0 || bh <= 0)
		return;
	pm = XCreatePixmap(d, RootWindow(d, DefaultScreen(d)), (unsigned)bw, (unsigned)bh,
	    (unsigned)DefaultDepth(d, DefaultScreen(d)));
	gv.font = fs->fid;
	gc = XCreateGC(d, pm, GCFont, &gv);
	bits = (unsigned char *)malloc((size_t)((bw + 7) / 8) * bh);
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	for (c = 0; c < 256; c++) {
		XCharStruct *cs = &fs->max_bounds;
		char ch = (char)c;
		int exists = 1;

		if (fs->per_char != NULL) {
			if (c < (int)fs->min_char_or_byte2 || c > (int)fs->max_char_or_byte2)
				exists = 0;
			else
				cs = &fs->per_char[c - fs->min_char_or_byte2];
		} else if (c < (int)fs->min_char_or_byte2 || c > (int)fs->max_char_or_byte2) {
			exists = 0;
		}
		glNewList(base + c, GL_COMPILE);
		gw = exists ? cs->rbearing - cs->lbearing : 0;
		gh = exists ? cs->ascent + cs->descent : 0;
		if (bits == NULL || gw <= 0 || gh <= 0 || gw > bw || gh > bh) {
			glBitmap(0, 0, 0.0f, 0.0f, exists ? (GLfloat)cs->width : 0.0f, 0.0f, NULL);
			glEndList();
			continue;
		}
		XSetForeground(d, gc, 0);
		XFillRectangle(d, pm, gc, 0, 0, (unsigned)bw, (unsigned)bh);
		XSetForeground(d, gc, 1);
		XDrawString(d, pm, gc, -cs->lbearing, cs->ascent, &ch, 1);
		img = XGetImage(d, pm, 0, 0, (unsigned)gw, (unsigned)gh, AllPlanes, ZPixmap);
		stride = (gw + 7) / 8;
		memset(bits, 0, (size_t)stride * gh);
		if (img != NULL) {
			/* OpenGL's rows run bottom up; X's top down. */
			for (y = 0; y < gh; y++)
				for (x = 0; x < gw; x++)
					if (XGetPixel(img, x, gh - 1 - y) != 0)
						bits[y * stride + x / 8] |= 0x80 >> (x % 8);
			XDestroyImage(img);
		}
		glBitmap(gw, gh, (GLfloat)-cs->lbearing, (GLfloat)cs->descent,
		    (GLfloat)cs->width, 0.0f, bits);
		glEndList();
	}
	glPopClientAttrib();
	free(bits);
	XFreeGC(d, gc);
	XFreePixmap(d, pm);
}

static int
font0_ready(void)
{
	Display *d;
	static const char *names[] = {
		/* The same 9x15 cell with IRIS GL font 0's heavy strokes. */
		"-misc-fixed-bold-r-normal--15-140-75-75-c-90-iso8859-1",
		"9x15bold",
		"-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1",
		"9x15",
		"fixed",
		NULL
	};
	int i;

	hgl_iris_ensure();
	if (font0_lists)
		return 1;
	d = hgl_display();
	for (i = 0; names[i] != NULL && font0 == NULL; i++)
		font0 = XLoadQueryFont(d, names[i]);
	if (!font0)
		return 0;
	font0_lists = glGenLists(256);
	build_font0(d, font0, font0_lists);
	return 1;
}

/* A defined font's lists, built on first use. */
static int
font_ready(RasterFont *f)
{
	int i;

	hgl_iris_ensure();
	if (f->lists)
		return 1;
	if (f->nc <= 0)
		return 0;
	f->lists = glGenLists(f->nc);
	if (f->lists == 0)
		return 0;
	glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
	/* Each row is whole 16-bit words, most significant bit leftmost,
	 * bottom row first -- glBitmap's own layout at an alignment of 2. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	for (i = 0; i < f->nc; i++) {
		const Fontchar *c = &f->chars[i];
		long words = (c->w + 15) / 16;

		glNewList(f->lists + i, GL_COMPILE);
		if (c->w > 0 && c->h > 0 && c->offset + words * c->h <= f->nr) {
			/* xoff/yoff place the bitmap relative to the character
			 * position; glBitmap's origin is the opposite offset. */
			glBitmap(c->w, c->h, (GLfloat)-c->xoff, (GLfloat)-c->yoff,
			    (GLfloat)c->width, 0.0f, (const GLubyte *)&f->raster[c->offset]);
		} else {
			glBitmap(0, 0, 0.0f, 0.0f, (GLfloat)c->width, 0.0f, NULL);
		}
		glEndList();
	}
	glPopClientAttrib();
	return 1;
}

void
defrasterfont(short n, short ht, short nc, const Fontchar chars[], short nr,
    const unsigned short raster[])
{
	RasterFont *f;
	int i;
	char what[64];

	sprintf(what, "defrasterfont %d ht %d nc %d nr %d", n, ht, nc, nr);
	TRACE(what);
	/* "Font 0 cannot be redefined." */
	if (n <= 0 || n >= MAXFONT)
		return;
	f = &fonts[n];
	if (f->lists && hgl_iris.ctx)
		glDeleteLists(f->lists, f->nc);
	free(f->chars);
	free(f->raster);
	memset(f, 0, sizeof *f);
	/* A font with no characters deletes it. */
	if (nc <= 0 || chars == NULL)
		return;
	f->chars = (Fontchar *)malloc(sizeof(Fontchar) * nc);
	f->raster = (unsigned short *)malloc(sizeof(unsigned short) * (nr > 0 ? nr : 1));
	if (f->chars == NULL || f->raster == NULL) {
		free(f->chars);
		free(f->raster);
		memset(f, 0, sizeof *f);
		return;
	}
	memcpy(f->chars, chars, sizeof(Fontchar) * nc);
	if (nr > 0 && raster != NULL)
		memcpy(f->raster, raster, sizeof(unsigned short) * nr);
	f->nr = nr > 0 ? nr : 0;
	f->ht = ht;
	f->nc = nc;
	for (i = 0; i < nc; i++)
		if (chars[i].yoff < 0 && -chars[i].yoff > f->descent)
			f->descent = -chars[i].yoff;
	f->defined = 1;
}

void
font(short n)
{
	/* "If you specify a font number that is not defined, the system
	 * selects font 0." */
	cur_font = n > 0 && n < MAXFONT && fonts[n].defined ? n : 0;
}

long getfont(void) { return cur_font; }

static int
defined_char(const RasterFont *f, unsigned long c)
{
	return c < (unsigned long)f->nc && (f->chars[c].width != 0 || f->chars[c].w != 0);
}

/* Draw character values through the current font's lists. */
static void
draw_chars(const unsigned long *cv, long n)
{
	GLuint buf[256];
	long i, k = 0;
	RasterFont *f = cur_font ? &fonts[cur_font] : NULL;

	if (n <= 0)
		return;
	if (f != NULL ? !font_ready(f) : !font0_ready())
		return;
	hgl_latch_raster_colour();
	for (i = 0; i < n; i++) {
		/* Characters the font does not define are ignored, and never
		 * reach a list number that belongs to something else. */
		if (f != NULL) {
			if (!defined_char(f, cv[i]))
				continue;
			buf[k++] = f->lists + (GLuint)cv[i];
		} else {
			if (cv[i] > 255)
				continue;
			buf[k++] = font0_lists + (GLuint)cv[i];
		}
		if (k == 256) {
			glCallLists(k, GL_UNSIGNED_INT, buf);
			k = 0;
		}
	}
	if (k)
		glCallLists(k, GL_UNSIGNED_INT, buf);
}

/* A string's character values, as lcharstr(3G) spells them out. */
static long
decode(long type, void *str, unsigned long *out, long max)
{
	const unsigned char *s = (const unsigned char *)str;
	long n = 0;
	unsigned long v;

	if (str == NULL)
		return 0;
	for (;;) {
		switch (type) {
		case STR_B:  v = s[n]; break;
		case STR_2B: v = (unsigned long)s[2 * n] << 8 | s[2 * n + 1]; break;
		case STR_3B: v = (unsigned long)s[3 * n] << 16 | (unsigned long)s[3 * n + 1] << 8 | s[3 * n + 2]; break;
		case STR_4B: v = (unsigned long)s[4 * n] << 24 | (unsigned long)s[4 * n + 1] << 16 |
		    (unsigned long)s[4 * n + 2] << 8 | s[4 * n + 3]; break;
		case STR_16: v = ((const unsigned short *)str)[n]; break;
		case STR_32: v = ((const unsigned long *)str)[n]; break;
		default: return 0;
		}
		if (v == 0 || n >= max)
			return n;
		out[n++] = v;
	}
}

#define MAXSTR 4096

void
lcharstr(long type, void *str)
{
	static unsigned long cv[MAXSTR];

	TRACE("lcharstr");
	draw_chars(cv, decode(type, str, cv, MAXSTR));
}

void
charstr(String str)
{
	TRACE("charstr");
	lcharstr(STR_B, (void *)str);
}

long
lstrwidth(long type, void *str)
{
	static unsigned long cv[MAXSTR];
	long n = decode(type, str, cv, MAXSTR), i, w = 0;
	RasterFont *f = cur_font ? &fonts[cur_font] : NULL;

	for (i = 0; i < n; i++) {
		if (f == NULL)
			w += cv[i] < 256 ? FONT0_WIDTH : 0;
		else if (defined_char(f, cv[i]))
			w += f->chars[cv[i]].width;
	}
	return w;
}

long strwidth(String str) { return lstrwidth(STR_B, (void *)str); }

long
getheight(void)
{
	return cur_font ? fonts[cur_font].ht : FONT0_HEIGHT;
}

long
getdescender(void)
{
	return cur_font ? fonts[cur_font].descent : FONT0_DESCENT;
}

/* getlwidth(3G): the width of the current line, which is a whole pixel count
 * here. */
long getlwidth(void)
{
	GLfloat w = 1.0f;

	hgl_iris_ensure();
	glGetFloatv(GL_LINE_WIDTH, &w);
	return (long)(w + 0.5f);
}

void cmov(Coord x, Coord y, Coord z) { hgl_iris_ensure(); hgl_rasterpos(x, y, z); hgl_raster_serial = hgl_colour_serial; }
void cmov2(Coord x, Coord y) { hgl_iris_ensure(); hgl_rasterpos(x, y, 0.0f); hgl_raster_serial = hgl_colour_serial; }
void cmov2i(Icoord x, Icoord y) { hgl_iris_ensure(); hgl_rasterpos((float)x, (float)y, 0.0f); hgl_raster_serial = hgl_colour_serial; }
void cmov2s(Scoord x, Scoord y) { hgl_iris_ensure(); hgl_rasterpos((float)x, (float)y, 0.0f); hgl_raster_serial = hgl_colour_serial; }

/* In screen coordinates: "for purely historical reasons" (getcpos(3G)) the
 * window origin is added. */
void
getcpos(Screencoord *ix, Screencoord *iy)
{
	GLfloat p[4];
	long ox = 0, oy = 0;

	hgl_iris_ensure();
	glGetFloatv(GL_CURRENT_RASTER_POSITION, p);
	getorigin(&ox, &oy);
	*ix = (Screencoord)(p[0] + ox);
	*iy = (Screencoord)(p[1] + oy);
}
