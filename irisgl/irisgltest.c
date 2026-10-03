/*
 * IRIS GL checks for IRIS's IRIS GL library, run inside the guest with the
 * shim installed as libgl.so, DISPLAY set and a window manager running:
 *
 *   irisgltest
 *
 * Each check sets state or draws, then reads it back -- lrectread,
 * readpixels, getmatrix, getsize, getcpos, the event queue -- and prints ok
 * or FAIL with what it saw. The expected values are what the IRIS GL manual
 * pages specify, not what the shim happens to do, so the same binary checks
 * SGI's own libgl.so on real hardware. The areas are the ones programs on
 * this image lean on: window sizing, colour in both modes, the matrix stack,
 * the z-buffer, backface removal, drawing to the front buffer, the pixel
 * calls, text, objects, the queue, lighting, blending, texturing and NURBS.
 *
 * Colours: IRIS GL packs RGBA as 0xAABBGGRR (cpack, lrectread); pix()
 * returns the low 24 bits, so red is 0x0000ff and blue 0xff0000. (The C_
 * prefix: gl.h already names the colour map's first indices RED, GREEN ...)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
/* X before IRIS GL, whose gl.h would otherwise clash with X over Cursor. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gl/gl.h>
#include <gl/device.h>
#include <gl/glws.h>

/* Bigger than 4Dwm's smallest window (its title bar sets the width), and the
 * coordinates come from getsize anyway: see pixel_ortho. */
#define W 128
#define H 128
#define C_RED     0x0000ffUL
#define C_GREEN   0x00ff00UL
#define C_BLUE    0xff0000UL
#define C_YELLOW  0x00ffffUL
#define C_BLACK   0x000000UL

static int failures;
static const Matrix identity = {
	{ 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
};

static void
check(const char *what, int good)
{
	printf("%s %s\n", good ? "ok  " : "FAIL", what);
	fflush(stdout);
	if (!good)
		failures++;
}

static unsigned long
pix(int x, int y)
{
	unsigned long p = 0;

	lrectread((Screencoord)x, (Screencoord)y, (Screencoord)x, (Screencoord)y, &p);
	return p & 0xffffffUL;
}

static int
chan(unsigned long p, int shift)
{
	return (int)((p >> shift) & 0xff);
}

/* One unit to a pixel, pixel centres on integer coordinates, from the size
 * the window really has. */
static void
pixel_ortho(int depth)
{
	long w = W, h = H;

	getsize(&w, &h);
	if (depth)
		ortho(-0.5, w - 0.5, -0.5, h - 0.5, -1.0, 1.0);
	else
		ortho2(-0.5, w - 0.5, -0.5, h - 0.5);
}

/* An RGB, single-buffered W x H window with window-pixel coordinates. */
static long
rgb_window(const char *name)
{
	long gid;

	prefsize(W, H);
	gid = winopen((char *)name);
	RGBmode();
	gconfig();
	pixel_ortho(0);
	cpack(C_BLACK);
	clear();
	return gid;
}

static void
t_window(void)
{
	long gid, w = 0, h = 0;
	char what[96];

	/* A window with only a minimum size is made that big by the window
	 * manager before winopen returns (winopen waits for it to map). */
	minsize(200, 150);
	gid = winopen("irisgltest minsize");
	getsize(&w, &h);
	sprintf(what, "getsize after winopen honours minsize (200x150): %ldx%ld", w, h);
	check(what, w >= 200 && h >= 150);
	winclose(gid);

	/* Bigger than 4Dwm's smallest window, which its title bar sets. */
	prefsize(160, 120);
	gid = winopen("irisgltest prefsize");
	getsize(&w, &h);
	sprintf(what, "getsize after prefsize(160, 120): %ldx%ld", w, h);
	check(what, w == 160 && h == 120);
	winclose(gid);
}

static void
t_gdesc(void)
{
	check("getgdesc(GD_XPMAX) is the screen width", getgdesc(GD_XPMAX) > 0);
	check("getgdesc(GD_ZMAX) is nonzero", getgdesc(GD_ZMAX) > 0);
	check("getgdesc(GD_BITS_NORM_DBL_RED) >= 8", getgdesc(GD_BITS_NORM_DBL_RED) >= 8);
	/* UroMan asks for one stencil plane, and turns capping off without it */
	check("getgdesc(GD_BITS_STENCIL) >= 1", getgdesc(GD_BITS_STENCIL) >= 1);
}

/* The stencil planes: a rectangle marks them, and a clear of the whole
 * window drawn where they are marked colours only that rectangle. */
static void
t_stencil(void)
{
	long gid, sbits;

	prefsize(W, H);
	gid = winopen("irisgltest stencil");
	RGBmode();
	stensize(1);
	gconfig();
	pixel_ortho(0);
	sbits = getgconfig(GC_BITS_STENCIL);
	check("getgconfig(GC_BITS_STENCIL) >= 1 after stensize(1)", sbits >= 1);
	cpack(C_BLACK);
	clear();
	sclear(0);

	stencil(TRUE, 1, SF_ALWAYS, 1, ST_KEEP, ST_REPLACE, ST_REPLACE);
	cpack(C_RED);
	rectfi(10, 10, 30, 30);
	stencil(TRUE, 1, SF_EQUAL, 1, ST_KEEP, ST_KEEP, ST_KEEP);
	cpack(C_GREEN);
	rectfi(0, 0, W - 1, H - 1);
	stencil(FALSE, 0, SF_ALWAYS, 0, ST_KEEP, ST_KEEP, ST_KEEP);
	check("stencil: drawn inside the marked rectangle", pix(20, 20) == C_GREEN);
	check("stencil: kept out of the rest", pix(45, 45) == C_BLACK);
	winclose(gid);
}

static void
t_clear_rect(void)
{
	long gid = rgb_window("irisgltest clear");
	unsigned long p;
	char what[96];

	cpack(C_RED);
	clear();
	p = pix(5, 5);
	sprintf(what, "cpack + clear fills the window: %06lx", p);
	check(what, p == C_RED);

	cpack(C_GREEN);
	rectf(10, 10, 20, 20);
	check("rectf fills its rectangle", pix(15, 15) == C_GREEN);
	check("rectf leaves the outside alone", pix(30, 30) == C_RED);
	/* IRIS GL fills the pixels on all four edges of a rectangle; OpenGL's
	 * glRect leaves the right and top ones out. */
	sprintf(what, "rectf includes both corners: %06lx %06lx", pix(10, 10), pix(20, 20));
	check(what, pix(10, 10) == C_GREEN && pix(20, 20) == C_GREEN);

	cpack(C_BLUE);
	bgnpolygon();
	{
		static float v[4][2] = { { 30, 30 }, { 50, 30 }, { 50, 50 }, { 30, 50 } };
		int i;
		for (i = 0; i < 4; i++)
			v2f(v[i]);
	}
	endpolygon();
	check("bgnpolygon/v2f/endpolygon fills the polygon", pix(40, 40) == C_BLUE);
	winclose(gid);
}

static void
t_cmode(void)
{
	long gid;
	short r = 0, g = 0, b = 0;
	Colorindex ci = 99;
	char what[96];

	prefsize(W, H);
	gid = winopen("irisgltest cmode");
	/* colour map mode is the default; gconfig commits it */
	gconfig();
	getmcolor(1, &r, &g, &b);
	sprintf(what, "the default colour map: index 1 is red (%d %d %d)", r, g, b);
	check(what, r == 255 && g == 0 && b == 0);
	/*
	 * A colour no other index has: the library draws colour map windows in
	 * RGB and reads an index back as the nearest colour, so two indices
	 * with the same colour cannot be told apart. A known limit, not what
	 * this checks.
	 */
	mapcolor(9, 10, 200, 30);
	getmcolor(9, &r, &g, &b);
	check("mapcolor then getmcolor", r == 10 && g == 200 && b == 30);
	color(9);
	clear();
	cmov2i(5, 5);
	readpixels(1, &ci);
	sprintf(what, "color + clear in colour map mode, read back as an index: %d", (int)ci);
	check(what, ci == 9);

	/* mapcolor of the current index changes what it draws (mapcolor(3G)) */
	color(40);
	mapcolor(40, 10, 20, 250);
	clear();
	cmov2i(5, 5);
	readpixels(1, &ci);
	sprintf(what, "mapcolor of the current index, then clear: index %d", (int)ci);
	check(what, ci == 40);
	winclose(gid);
}

static void
t_matrix(void)
{
	long gid = rgb_window("irisgltest matrix");
	Matrix m;
	int ident, i, j;

	mmode(MVIEWING);
	loadmatrix(identity);
	pushmatrix();
	translate(1.0f, 2.0f, 3.0f);
	getmatrix(m);
	check("translate is the matrix's last row", m[3][0] == 1.0f && m[3][1] == 2.0f && m[3][2] == 3.0f);
	popmatrix();
	getmatrix(m);
	ident = 1;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			if (m[i][j] != (i == j ? 1.0f : 0.0f))
				ident = 0;
	check("popmatrix restores the matrix", ident);
	scale(2.0f, 3.0f, 4.0f);
	getmatrix(m);
	check("scale is the diagonal", m[0][0] == 2.0f && m[1][1] == 3.0f && m[2][2] == 4.0f);
	mmode(MSINGLE);
	winclose(gid);
}

static void
t_perspective(void)
{
	static float v[4][3] = { { -5, -5, -10 }, { 5, -5, -10 }, { 5, 5, -10 }, { -5, 5, -10 } };
	long gid = rgb_window("irisgltest perspective");
	long w = W, h = H;
	int i;

	getsize(&w, &h);
	mmode(MVIEWING);
	/* perspective takes tenths of a degree; near = 0 is allowed (amesh). */
	perspective(900, (float)w / (float)h, 0.0f, 100.0f);
	loadmatrix(identity);
	cpack(C_GREEN);
	bgnpolygon();
	for (i = 0; i < 4; i++)
		v3f(v[i]);
	endpolygon();
	check("perspective with near = 0 still projects", pix(w / 2, h / 2) == C_GREEN);
	cpack(C_BLACK);
	clear();
	perspective(900, (float)w / (float)h, 1.0f, 100.0f);
	cpack(C_GREEN);
	bgnpolygon();
	for (i = 0; i < 4; i++)
		v3f(v[i]);
	endpolygon();
	check("perspective in MVIEWING sets the projection", pix(w / 2, h / 2) == C_GREEN);
	mmode(MSINGLE);
	winclose(gid);
}

static void
t_zbuffer(void)
{
	long gid;
	unsigned long a, b;

	prefsize(W, H);
	gid = winopen("irisgltest zbuffer");
	RGBmode();
	gconfig();
	pixel_ortho(1);
	zbuffer(TRUE);

	/* Two overlapping rectangles at different depths: whichever is drawn
	 * first, the same one must show. */
	czclear(C_BLACK, getgdesc(GD_ZMAX));
	cpack(C_RED);
	pmv(10, 10, 0.5f); pdr(40, 10, 0.5f); pdr(40, 40, 0.5f); pdr(10, 40, 0.5f); pclos();
	cpack(C_GREEN);
	pmv(20, 20, -0.5f); pdr(50, 20, -0.5f); pdr(50, 50, -0.5f); pdr(20, 50, -0.5f); pclos();
	a = pix(30, 30);

	czclear(C_BLACK, getgdesc(GD_ZMAX));
	cpack(C_GREEN);
	pmv(20, 20, -0.5f); pdr(50, 20, -0.5f); pdr(50, 50, -0.5f); pdr(20, 50, -0.5f); pclos();
	cpack(C_RED);
	pmv(10, 10, 0.5f); pdr(40, 10, 0.5f); pdr(40, 40, 0.5f); pdr(10, 40, 0.5f); pclos();
	b = pix(30, 30);
	check("the z-buffer decides the overlap, whatever the order", a == b && (a == C_RED || a == C_GREEN));

	zbuffer(FALSE);
	winclose(gid);
}

static void
t_backface(void)
{
	long gid = rgb_window("irisgltest backface");

	backface(TRUE);
	cpack(C_GREEN);
	/* counter-clockwise on the screen: kept */
	pmv2i(5, 5); pdr2i(25, 5); pdr2i(25, 25); pdr2i(5, 25); pclos();
	/* clockwise: removed */
	pmv2i(35, 35); pdr2i(35, 55); pdr2i(55, 55); pdr2i(55, 35); pclos();
	check("backface keeps a counter-clockwise polygon", pix(15, 15) == C_GREEN);
	check("backface removes a clockwise polygon", pix(45, 45) == C_BLACK);
	backface(FALSE);
	winclose(gid);
}

static void
t_front(void)
{
	long gid;
	unsigned long f, b;
	char what[96];

	prefsize(W, H);
	gid = winopen("irisgltest frontbuffer");
	doublebuffer();
	RGBmode();
	gconfig();
	pixel_ortho(0);
	cpack(C_BLACK);
	clear();
	swapbuffers();
	clear();

	/* frontbuffer(TRUE) with backbuffer on (the default) draws into both. */
	frontbuffer(TRUE);
	cpack(C_YELLOW);
	rectf(10, 10, 30, 30);
	frontbuffer(FALSE);
	readsource(SRC_FRONT);
	f = pix(20, 20);
	readsource(SRC_BACK);
	b = pix(20, 20);
	readsource(SRC_AUTO);
	sprintf(what, "frontbuffer(TRUE) draws into the front buffer: %06lx", f);
	check(what, f == C_YELLOW);
	sprintf(what, "... and, with backbuffer on, the back buffer too: %06lx", b);
	check(what, b == C_YELLOW);

	/* A swap shows the back buffer. */
	cpack(C_BLUE);
	clear();
	swapbuffers();
	readsource(SRC_FRONT);
	f = pix(5, 5);
	readsource(SRC_AUTO);
	check("swapbuffers makes the back buffer the front", f == C_BLUE);
	winclose(gid);
}

static void
t_pixels(void)
{
	long gid = rgb_window("irisgltest pixels");
	unsigned long in[4] = { C_RED, C_GREEN, C_BLUE, C_YELLOW }, out[4];
	int i, same;

	lrectwrite(10, 10, 11, 11, in);
	memset(out, 0, sizeof out);
	lrectread(10, 10, 11, 11, out);
	same = 1;
	for (i = 0; i < 4; i++)
		if ((out[i] & 0xffffffUL) != in[i])
			same = 0;
	check("lrectwrite then lrectread, 2x2, bottom row first", same);

	cpack(C_GREEN);
	rectf(2, 40, 8, 46);
	rectcopy(2, 40, 8, 46, 40, 2);
	check("rectcopy copies the rectangle", pix(43, 5) == C_GREEN);
	check("... and leaves the source", pix(5, 43) == C_GREEN);
	winclose(gid);
}

static void
t_text(void)
{
	long gid = rgb_window("irisgltest text");
	short x = 0, y = 0;
	unsigned long area[20 * 16];
	int i, lit = 0;
	char what[96];

	cpack(0xffffffUL);
	cmov2i(10, 20);
	charstr("W");
	getcpos(&x, &y);
	sprintf(what, "charstr moves the character position right: %d", (int)x);
	check(what, x > 10);
	lrectread(8, 16, 27, 31, area);
	for (i = 0; i < 20 * 16; i++)
		if ((area[i] & 0xffffffUL) != C_BLACK)
			lit++;
	sprintf(what, "charstr draws the character: %d pixels", lit);
	check(what, lit > 4);
	winclose(gid);
}

static void
t_objects(void)
{
	long gid = rgb_window("irisgltest objects");

	makeobj(1);
	cpack(C_BLUE);
	rectf(20, 20, 40, 40);
	closeobj();
	check("makeobj records rather than draws", pix(30, 30) == C_BLACK);
	callobj(1);
	check("callobj draws what was recorded", pix(30, 30) == C_BLUE);
	delobj(1);
	winclose(gid);
}

static void
t_queue(void)
{
	long gid = rgb_window("irisgltest queue");
	short val = 0;
	long dev;

	qreset();
	qenter(REDRAW, 5);
	check("qtest sees an entered event", qtest() == REDRAW);
	dev = qread(&val);
	check("qread returns its device and value", dev == REDRAW && val == 5);
	winclose(gid);
}

static void
t_lighting(void)
{
	static float mat[] = { DIFFUSE, 1.0f, 0.0f, 0.0f, LMNULL };
	static float light[] = { LCOLOR, 1.0f, 1.0f, 1.0f, POSITION, 0.0f, 0.0f, 1.0f, 0.0f, LMNULL };
	static float model[] = { LMNULL };
	long gid = rgb_window("irisgltest lighting");
	unsigned long p;
	char what[96];

	mmode(MVIEWING);
	pixel_ortho(1);
	loadmatrix(identity);
	/* Any positive short names a definition: Performer's start at 2049. */
	lmdef(DEFMATERIAL, 2049, 5, mat);
	lmdef(DEFLIGHT, 1, 10, light);
	lmdef(DEFLMODEL, 1, 1, model);
	lmbind(MATERIAL, 2049);
	lmbind(LIGHT0, 1);
	lmbind(LMODEL, 1);
	{
		static float n[3] = { 0, 0, 1 };
		static float v[4][3] = { { 10, 10, 0 }, { 50, 10, 0 }, { 50, 50, 0 }, { 10, 50, 0 } };
		int i;
		bgnpolygon();
		for (i = 0; i < 4; i++) {
			n3f(n);
			v3f(v[i]);
		}
		endpolygon();
	}
	p = pix(30, 30);
	sprintf(what, "a red material lit head on is red: %06lx", p);
	check(what, chan(p, 0) > 160 && chan(p, 8) < 40 && chan(p, 16) < 40);

	/* A matrix that squashes the model stretches its normals; lighting uses
	 * them at unit length all the same (nmode NAUTO). demograph draws its map
	 * at scale(1, 1, 1e-8). */
	cpack(C_BLACK);
	clear();
	pushmatrix();
	scale(1.0f, 1.0f, 1e-4f);
	{
		static float n[3] = { 0, 0, 1 };
		static float v[4][3] = { { 10, 10, 0 }, { 50, 10, 0 }, { 50, 50, 0 }, { 10, 50, 0 } };
		int i;
		bgnpolygon();
		for (i = 0; i < 4; i++) {
			n3f(n);
			v3f(v[i]);
		}
		endpolygon();
	}
	popmatrix();
	p = pix(30, 30);
	sprintf(what, "... and still red under a squashing scale: %06lx", p);
	check(what, chan(p, 0) > 160 && chan(p, 8) < 40 && chan(p, 16) < 40);

	/* lmcolor(LMC_COLOR): a vertex is lit when a normal was the last of the
	 * two sent before it, and drawn in the colour when the colour was.
	 * UroMan draws its translucent cutting plane that way, mid-scene. */
	{
		static float n[3] = { 0, 0, 1 };
		static float v[4][3] = { { 10, 10, 0 }, { 50, 10, 0 }, { 50, 50, 0 }, { 10, 50, 0 } };
		int i;

		cpack(C_BLACK);
		clear();
		cpack(0xff00ff00UL);
		bgnpolygon();
		for (i = 0; i < 4; i++)
			v3f(v[i]);
		endpolygon();
		p = pix(30, 30);
		sprintf(what, "a colour and no normal under lighting is drawn unlit: %06lx", p);
		check(what, chan(p, 8) > 200 && chan(p, 0) < 40);

		cpack(0xff00ff00UL);
		bgnpolygon();
		for (i = 0; i < 4; i++) {
			n3f(n);
			v3f(v[i]);
		}
		endpolygon();
		p = pix(30, 30);
		sprintf(what, "... and a normal after the colour is lit again: %06lx", p);
		check(what, chan(p, 0) > 160 && chan(p, 8) < 40);

		/* In any other mode a colour sets the material, and stays lit. */
		lmcolor(LMC_DIFFUSE);
		bgnpolygon();
		for (i = 0; i < 4; i++) {
			n3f(n);
			cpack(0xff00ff00UL);
			v3f(v[i]);
		}
		endpolygon();
		lmcolor(LMC_COLOR);
		p = pix(30, 30);
		sprintf(what, "lmcolor(LMC_DIFFUSE): the colour is the lit material: %06lx", p);
		check(what, chan(p, 8) > 160 && chan(p, 0) < 40);
	}
	lmbind(MATERIAL, 0);
	mmode(MSINGLE);
	winclose(gid);
}

/* A rectangle from (10,10) to (50,50) at depth z, in the colour set before. */
static void
quad_z(float z)
{
	pmv(10, 10, z); pdr(50, 10, z); pdr(50, 50, z); pdr(10, 50, z); pclos();
}

/* zfunction's default, and what czclear and zclear touch. */
static void
t_zclear(void)
{
	long gid;
	unsigned long p;
	char what[96];

	prefsize(W, H);
	gid = winopen("irisgltest zclear");
	RGBmode();
	gconfig();
	pixel_ortho(1);
	zbuffer(TRUE);

	/* ZF_LEQUAL by default (zfunction(3G)): a second pass at the same
	 * depth draws over the first */
	czclear(C_BLACK, getgdesc(GD_ZMAX));
	cpack(C_RED);
	quad_z(0.0f);
	cpack(C_GREEN);
	quad_z(0.0f);
	p = pix(30, 30);
	sprintf(what, "the z test passes equal depths: %06lx", p);
	check(what, p == C_GREEN);

	/* czclear leaves the current colour alone */
	cpack(C_RED);
	czclear(C_BLUE, getgdesc(GD_ZMAX));
	p = pix(5, 5);
	sprintf(what, "czclear clears to its colour: %06lx", p);
	check(what, p == C_BLUE);
	quad_z(0.0f);
	p = pix(30, 30);
	sprintf(what, "... and the current colour is still red: %06lx", p);
	check(what, p == C_RED);

	/* ... and ignores the writemask */
	wmpack(0);
	czclear(C_GREEN, getgdesc(GD_ZMAX));
	wmpack(0xffffffffUL);
	p = pix(5, 5);
	sprintf(what, "czclear ignores wmpack(0): %06lx", p);
	check(what, p == C_GREEN);

	/* zclear clears to the far end, whatever czclear last used */
	czclear(C_BLACK, 0);
	zclear();
	cpack(C_RED);
	quad_z(0.5f);
	p = pix(30, 30);
	sprintf(what, "zclear after czclear(c, 0) clears to the far end: %06lx", p);
	check(what, p == C_RED);

	/* pushattributes does not save the z-buffer's state */
	zbuffer(FALSE);
	pushattributes();
	zbuffer(TRUE);
	popattributes();
	check("pushattributes; zbuffer(TRUE); popattributes leaves it on", getzbuffer() == TRUE);
	zbuffer(FALSE);
	winclose(gid);
}

/* lmdef(3G): a SHININESS of 0 turns specular reflection off. */
static void
t_shininess(void)
{
	static float mat[] = { DIFFUSE, 0.0f, 0.0f, 0.0f, SPECULAR, 1.0f, 1.0f, 1.0f,
	    SHININESS, 0.0f, LMNULL };
	static float light[] = { LCOLOR, 1.0f, 1.0f, 1.0f, POSITION, 0.0f, 0.0f, 1.0f, 0.0f, LMNULL };
	static float model[] = { LMNULL };
	static float n[3] = { 0, 0, 1 };
	static float v[4][3] = { { 10, 10, 0 }, { 50, 10, 0 }, { 50, 50, 0 }, { 10, 50, 0 } };
	long gid = rgb_window("irisgltest shininess");
	unsigned long p;
	char what[96];
	int i;

	mmode(MVIEWING);
	pixel_ortho(1);
	loadmatrix(identity);
	lmdef(DEFMATERIAL, 3, 11, mat);
	lmdef(DEFLIGHT, 3, 10, light);
	lmdef(DEFLMODEL, 3, 1, model);
	lmbind(MATERIAL, 3);
	lmbind(LIGHT0, 3);
	lmbind(LMODEL, 3);
	bgnpolygon();
	for (i = 0; i < 4; i++) {
		n3f(n);
		v3f(v[i]);
	}
	endpolygon();
	p = pix(30, 30);
	sprintf(what, "SHININESS 0 adds no specular: %06lx", p);
	check(what, chan(p, 0) < 80 && chan(p, 8) < 80 && chan(p, 16) < 80);

	/* the colour characters take is the current colour, not lit */
	cpack(C_BLACK);
	clear();
	cpack(C_GREEN);
	cmov2i(10, 20);
	charstr("W");
	{
		unsigned long area[20 * 16];
		int green = 0, other = 0;
		lrectread(8, 16, 27, 31, area);
		for (i = 0; i < 20 * 16; i++) {
			unsigned long c = area[i] & 0xffffffUL;
			if (c == C_GREEN)
				green++;
			else if (c != C_BLACK)
				other++;
		}
		sprintf(what, "text under lighting is the current colour: %d green, %d other", green, other);
		check(what, green > 4 && other == 0);
	}
	lmbind(MATERIAL, 0);
	mmode(MSINGLE);
	winclose(gid);
}

/* A red rectangle with texture coordinates over the texture. */
static void
textured_quad(void)
{
	static float t[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static float v[4][2] = { { 10, 10 }, { 50, 10 }, { 50, 50 }, { 10, 50 } };
	int i;

	cpack(C_RED);
	bgnpolygon();
	for (i = 0; i < 4; i++) {
		t2f(t[i]);
		v2f(v[i]);
	}
	endpolygon();
}

/* Texturing needs both bindings (texbind(3G), tevbind(3G)); tevdef's
 * environments. */
static void
t_texenv(void)
{
	static unsigned long green[4] = { 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL };
	static float tprops[] = { TX_MINFILTER, TX_POINT, TX_NULL };
	static float modulate[] = { TV_MODULATE, TV_NULL };
	static float decal[] = { TV_DECAL, TV_NULL };
	long gid = rgb_window("irisgltest texenv");
	unsigned long p;
	char what[96];

	texdef2d(2, 4, 2, 2, green, 3, tprops);
	tevdef(2, 2, modulate);
	tevdef(3, 2, decal);
	tevbind(TV_ENV0, 0);
	texbind(TX_TEXTURE_0, 2);
	textured_quad();
	p = pix(30, 30);
	sprintf(what, "a texture bound with no environment does not texture: %06lx", p);
	check(what, p == C_RED);

	tevbind(TV_ENV0, 2);
	textured_quad();
	p = pix(30, 30);
	sprintf(what, "... TV_MODULATE: green modulating red is black: %06lx", p);
	check(what, p == C_BLACK);

	tevbind(TV_ENV0, 3);
	textured_quad();
	p = pix(30, 30);
	sprintf(what, "... TV_DECAL: the texture's green: %06lx", p);
	check(what, p == C_GREEN);

	tevbind(TV_ENV0, 0);
	textured_quad();
	p = pix(30, 30);
	sprintf(what, "... tevbind(TV_ENV0, 0) turns texturing off: %06lx", p);
	check(what, p == C_RED);
	texbind(TX_TEXTURE_0, 0);
	winclose(gid);
}

/* mmode(3G): entering MSINGLE makes every matrix the identity. */
static void
t_mmode_single(void)
{
	long gid = rgb_window("irisgltest mmode");
	Matrix m;
	int i, j, same = 1;

	mmode(MVIEWING);
	translate(5.0f, 0.0f, 0.0f);
	mmode(MSINGLE);
	getmatrix(m);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			if (m[i][j] != identity[i][j])
				same = 0;
	check("entering MSINGLE resets the matrix to the identity", same);
	winclose(gid);
}

/* gselect: drawing in the region records the name stack; nothing is
 * drawn. */
static void
t_select(void)
{
	long gid = rgb_window("irisgltest select");
	short buf[50];
	long n;
	unsigned long p;
	char what[96];

	mmode(MVIEWING);
	ortho2(0.0f, 10.0f, 0.0f, 10.0f);	/* the selecting region */
	initnames();
	gselect(buf, 50);
	loadname(7);
	rectfi(2, 2, 4, 4);
	loadname(8);
	rectfi(20, 20, 30, 30);
	n = endselect(buf);
	sprintf(what, "gselect: one hit, named 7: %ld (%d %d)", n, buf[0], buf[1]);
	check(what, n == 1 && buf[0] == 1 && buf[1] == 7);
	mmode(MSINGLE);
	pixel_ortho(0);
	p = pix(3, 3);
	sprintf(what, "... and nothing was drawn: %06lx", p);
	check(what, p == C_BLACK);
	winclose(gid);
}

/* blendfunction's destination factor 2 is the *source* colour (BF_SC), and
 * the last of blendfunction and logicop wins. */
static void
t_blendfactors(void)
{
	long gid = rgb_window("irisgltest blendfactors");
	unsigned long p;
	char what[96];

	cpack(0xffffffUL);
	clear();
	blendfunction(BF_ZERO, BF_SC);
	cpack(C_RED);
	rectfi(10, 10, 50, 50);
	blendfunction(BF_ONE, BF_ZERO);
	p = pix(30, 30);
	sprintf(what, "blendfunction(BF_ZERO, BF_SC) over white is the source: %06lx", p);
	check(what, p == C_RED);

	cpack(C_RED);
	clear();
	logicop(LO_XOR);
	blendfunction(BF_SA, BF_MSA);
	cpack(0xff0000ffUL);
	rectfi(10, 10, 50, 50);
	blendfunction(BF_ONE, BF_ZERO);
	logicop(LO_SRC);
	p = pix(30, 30);
	sprintf(what, "a blendfunction after logicop turns the logic op off: %06lx", p);
	check(what, p == C_RED);
	winclose(gid);
}

/* Old-style polygons (GLPG-I 2-20): filled shapes but bgnpolygon's fill
 * their top and right edge pixels too, unless glcompat(GLC_OLDPOLYGON, 0). */
static void
t_oldpolygon(void)
{
	static Icoord sq[4][2] = { { 10, 10 }, { 19, 10 }, { 19, 19 }, { 10, 19 } };
	/* a U: the notch between x 24..36 above y 30 must stay empty */
	static Coord u[8][2] = {
		{ 10, 10 }, { 50, 10 }, { 50, 50 }, { 40, 50 }, { 40, 30 }, { 20, 30 }, { 20, 50 }, { 10, 50 }
	};
	long gid = rgb_window("irisgltest oldpolygon");
	unsigned long p;
	char what[96];
	int i;

	cpack(C_RED);
	polf2i(4, sq);
	sprintf(what, "polf fills its top right pixel (19,19): %06lx", pix(19, 19));
	check(what, pix(19, 19) == C_RED && pix(20, 20) == C_BLACK);
	cpack(C_BLACK);
	clear();
	glcompat(GLC_OLDPOLYGON, 0);
	cpack(C_RED);
	rectfi(10, 10, 19, 19);
	glcompat(GLC_OLDPOLYGON, 1);
	sprintf(what, "glcompat(GLC_OLDPOLYGON, 0): rectfi point-sampled (%06lx at 19,19)", pix(19, 19));
	check(what, pix(18, 18) == C_RED && pix(19, 19) == C_BLACK);

	cpack(C_BLACK);
	clear();
	concave(TRUE);
	cpack(C_GREEN);
	polf2(8, u);
	p = pix(30, 40);
	sprintf(what, "concave(TRUE): polf leaves a U's notch empty: %06lx", p);
	check(what, p == C_BLACK && pix(15, 40) == C_GREEN && pix(30, 20) == C_GREEN);
	cpack(C_BLACK);
	clear();
	cpack(C_GREEN);
	bgnpolygon();
	for (i = 0; i < 8; i++)
		v2f(u[i]);
	endpolygon();
	concave(FALSE);
	p = pix(30, 40);
	sprintf(what, "... and so does bgnpolygon: %06lx", p);
	check(what, p == C_BLACK && pix(45, 40) == C_GREEN);

	/* lines are drawn closed: both ends (subpixel(3G)) */
	cpack(C_BLACK);
	clear();
	cpack(C_RED);
	{
		static float a[2] = { 60, 10 }, b[2] = { 60, 20 };
		bgnline();
		v2f(a);
		v2f(b);
		endline();
	}
	sprintf(what, "a line from (60,10) to (60,20) draws (60,20): %06lx", pix(60, 20));
	check(what, pix(60, 20) == C_RED && pix(60, 10) == C_RED);
	winclose(gid);
}

/* stencil(3G): with the z-buffer off, pass applies when the stencil test
 * passes (zpass is for z on). */
static void
t_stencil_zoff(void)
{
	long gid;

	prefsize(W, H);
	gid = winopen("irisgltest stencil z off");
	RGBmode();
	stensize(1);
	gconfig();
	pixel_ortho(0);
	cpack(C_BLACK);
	clear();
	sclear(0);
	zbuffer(FALSE);
	stencil(TRUE, 1, SF_ALWAYS, 1, ST_KEEP, ST_REPLACE, ST_KEEP);
	cpack(C_RED);
	rectfi(10, 10, 30, 30);
	stencil(TRUE, 1, SF_EQUAL, 1, ST_KEEP, ST_KEEP, ST_KEEP);
	cpack(C_GREEN);
	rectfi(0, 0, W - 1, H - 1);
	stencil(FALSE, 0, SF_ALWAYS, 0, ST_KEEP, ST_KEEP, ST_KEEP);
	check("stencil with z off: pass marks the rectangle", pix(20, 20) == C_GREEN && pix(45, 45) == C_BLACK);
	winclose(gid);
}

/* depthcue: colour from z alone, max colour near, min colour far. */
static void
t_depthcue(void)
{
	long gid = rgb_window("irisgltest depthcue");
	unsigned long n, f;
	char what[96];

	pixel_ortho(1);
	lRGBrange(0, 0, 0, 255, 255, 255, 0, 0x7fffff);
	depthcue(TRUE);
	cpack(C_RED);		/* ignored while depthcue is on */
	pmv(10, 10, 0.9f); pdr(40, 10, 0.9f); pdr(40, 40, 0.9f); pdr(10, 40, 0.9f); pclos();
	pmv(60, 10, -0.9f); pdr(90, 10, -0.9f); pdr(90, 40, -0.9f); pdr(60, 40, -0.9f); pclos();
	depthcue(FALSE);
	n = pix(25, 25);
	f = pix(75, 25);
	sprintf(what, "depthcue: near is light grey, far dark: %06lx %06lx", n, f);
	check(what, chan(n, 0) > 200 && chan(n, 8) > 200 && chan(f, 0) < 60 && chan(f, 8) < 60);
	winclose(gid);
}

/* Objects: building one changes nothing now; calling it does. Pixel writes
 * are not textured. */
static void
t_objects_state(void)
{
	static unsigned long green[4] = { 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL };
	static unsigned long red[16 * 16];
	static float tprops[] = { TX_MINFILTER, TX_POINT, TX_NULL };
	static float decal[] = { TV_DECAL, TV_NULL };
	long gid = rgb_window("irisgltest object state");
	unsigned long p;
	char what[96];
	int i;

	cpack(C_RED);
	makeobj(5);
	cpack(C_GREEN);
	closeobj();
	clear();
	p = pix(5, 5);
	sprintf(what, "makeobj ... cpack ... closeobj leaves the colour: %06lx", p);
	check(what, p == C_RED);
	callobj(5);
	clear();
	p = pix(5, 5);
	sprintf(what, "... callobj sets it: %06lx", p);
	check(what, p == C_GREEN);
	delobj(5);

	/* objects numbered where the font's lists would be */
	cpack(C_BLACK);
	clear();
	for (i = 1; i <= 300; i++) {
		makeobj(i);
		cpack(C_BLUE);
		closeobj();
	}
	cpack(0xffffffUL);
	cmov2i(10, 20);
	charstr("W");
	{
		unsigned long area[20 * 16];
		int lit = 0;
		lrectread(8, 16, 27, 31, area);
		for (i = 0; i < 20 * 16; i++)
			if ((area[i] & 0xffffffUL) == 0xffffffUL)
				lit++;
		sprintf(what, "text after makeobj(1..300) still draws: %d pixels", lit);
		check(what, lit > 4);
	}
	for (i = 1; i <= 300; i++)
		delobj(i);

	/* a pixel write while a texture is bound */
	for (i = 0; i < 16 * 16; i++)
		red[i] = 0xff0000ffUL;
	texdef2d(4, 4, 2, 2, green, 3, tprops);
	tevdef(4, 2, decal);
	texbind(TX_TEXTURE_0, 4);
	tevbind(TV_ENV0, 4);
	lrectwrite(40, 40, 55, 55, red);
	tevbind(TV_ENV0, 0);
	texbind(TX_TEXTURE_0, 0);
	p = pix(47, 47);
	sprintf(what, "lrectwrite under a bound texture is not textured: %06lx", p);
	check(what, p == C_RED);
	winclose(gid);
}

/* gconfig's resets, getbuffer, lmbind of an undefined name, a patterned
 * clear, per-window lighting. */
static void
t_config_state(void)
{
	static float mat[] = { DIFFUSE, 0.0f, 0.0f, 1.0f, LMNULL };
	static float light[] = { LCOLOR, 1.0f, 1.0f, 1.0f, POSITION, 0.0f, 0.0f, 1.0f, 0.0f, LMNULL };
	static float model[] = { LMNULL };
	static float n[3] = { 0, 0, 1 };
	static float v[4][3] = { { 10, 10, 0 }, { 50, 10, 0 }, { 50, 50, 0 }, { 10, 50, 0 } };
	static unsigned short half[16];
	long gid, gid2;
	short r = 1, g = 1, b = 1;
	unsigned long p;
	char what[96];
	int i, lit;

	prefsize(W, H);
	gid = winopen("irisgltest config");
	RGBmode();
	cpack(C_RED);
	gconfig();
	gRGBcolor(&r, &g, &b);
	sprintf(what, "gconfig sets the colour to 0: %d %d %d", r, g, b);
	check(what, r == 0 && g == 0 && b == 0);
	check("getbuffer is 0 when single-buffered", getbuffer() == 0);
	pixel_ortho(0);

	/* lmbind of a name never defined binds 0: lighting goes off */
	mmode(MVIEWING);
	pixel_ortho(1);
	loadmatrix(identity);
	lmdef(DEFMATERIAL, 6, 5, mat);
	lmdef(DEFLIGHT, 6, 10, light);
	lmdef(DEFLMODEL, 6, 1, model);
	lmbind(MATERIAL, 6);
	lmbind(LIGHT0, 6);
	lmbind(LMODEL, 6);
	lmbind(MATERIAL, 999);
	cpack(C_BLACK);
	clear();
	cpack(C_RED);
	bgnpolygon();
	for (i = 0; i < 4; i++) {
		n3f(n);
		v3f(v[i]);
	}
	endpolygon();
	p = pix(30, 30);
	sprintf(what, "lmbind(MATERIAL, undefined) turns lighting off: %06lx", p);
	check(what, p == C_RED);

	/* lighting bound in one window is not bound in the next */
	lmbind(MATERIAL, 6);
	prefsize(W, H);
	gid2 = winopen("irisgltest config 2");
	RGBmode();
	gconfig();
	mmode(MVIEWING);
	pixel_ortho(1);
	cpack(C_BLACK);
	clear();
	cpack(C_RED);
	bgnpolygon();
	for (i = 0; i < 4; i++) {
		n3f(n);
		v3f(v[i]);
	}
	endpolygon();
	p = pix(30, 30);
	sprintf(what, "a new window has no lighting bound: %06lx", p);
	check(what, p == C_RED);
	winclose(gid2);
	winset(gid);
	lmbind(MATERIAL, 0);
	mmode(MSINGLE);
	pixel_ortho(0);

	/* a patterned clear: every other column */
	for (i = 0; i < 16; i++)
		half[i] = 0xaaaa;
	defpattern(3, 16, half);
	cpack(C_BLACK);
	clear();
	setpattern(3);
	cpack(C_GREEN);
	clear();
	setpattern(0);
	lit = 0;
	for (i = 0; i < 16; i++)
		if (pix(20 + i, 20) == C_GREEN)
			lit++;
	sprintf(what, "clear under a pattern fills half the pixels: %d of 16", lit);
	check(what, lit == 8);
	winclose(gid);
}

/* pixmode(PM_ZDATA): lrectwrite writes z, not colour. */
static void
t_zdata(void)
{
	static unsigned long z[16 * 16];
	long gid;
	unsigned long a, b;
	char what[96];
	int i;

	prefsize(W, H);
	gid = winopen("irisgltest zdata");
	RGBmode();
	gconfig();
	pixel_ortho(1);
	zbuffer(TRUE);
	czclear(C_BLACK, getgdesc(GD_ZMAX));
	for (i = 0; i < 16 * 16; i++)
		z[i] = 0;		/* nearest */
	pixmode(PM_ZDATA, 1);
	lrectwrite(20, 20, 35, 35, z);
	pixmode(PM_ZDATA, 0);
	cpack(C_RED);
	pmv(10, 10, 0.0f); pdr(50, 10, 0.0f); pdr(50, 50, 0.0f); pdr(10, 50, 0.0f); pclos();
	a = pix(27, 27);
	b = pix(15, 15);
	sprintf(what, "z written with PM_ZDATA hides what is behind: %06lx, %06lx beside", a, b);
	check(what, a == C_BLACK && b == C_RED);
	zbuffer(FALSE);
	winclose(gid);
}

/* mapw2 through a viewing object; a texture with the default (mipmap)
 * filter. */
static void
t_mapw_mipmap(void)
{
	static unsigned long green[4] = { 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL, 0xff00ff00UL };
	static float decal[] = { TV_DECAL, TV_NULL };
	long gid = rgb_window("irisgltest mapw");
	long w = W, h = H;
	Coord wx = -1, wy = -1;
	unsigned long p;
	char what[96];

	getsize(&w, &h);
	makeobj(40);
	ortho2(0.0f, 10.0f, 0.0f, 10.0f);
	closeobj();
	mapw2(40, (Screencoord)(w / 2), (Screencoord)(h / 2), &wx, &wy);
	sprintf(what, "mapw2 of the window centre under ortho2(0,10,0,10): %g %g", wx, wy);
	check(what, wx > 4.8f && wx < 5.2f && wy > 4.8f && wy < 5.2f);
	delobj(40);

	pixel_ortho(0);
	texdef2d(6, 4, 2, 2, green, 0, NULL);
	tevdef(6, 2, decal);
	texbind(TX_TEXTURE_0, 6);
	tevbind(TV_ENV0, 6);
	textured_quad();
	tevbind(TV_ENV0, 0);
	texbind(TX_TEXTURE_0, 0);
	p = pix(30, 30);
	sprintf(what, "a texture with the default (mipmapped) filter draws: %06lx", p);
	check(what, p == C_GREEN);
	winclose(gid);
}

/* subtexload: part of a texture, in a mipmapped and a plain one. */
static void
t_subtexload(void)
{
	static unsigned long green[4 * 4], red[2 * 4];
	static float point[] = { TX_MINFILTER, TX_POINT, TX_MAGFILTER, TX_POINT, TX_NULL };
	static float decal[] = { TV_DECAL, TV_NULL };
	long gid = rgb_window("irisgltest subtexload");
	unsigned long a, b;
	char what[96];
	int i, k;

	for (i = 0; i < 16; i++)
		green[i] = 0xff00ff00UL;
	for (i = 0; i < 8; i++)
		red[i] = 0xff0000ffUL;
	tevdef(7, 2, decal);
	for (k = 0; k < 2; k++) {
		texdef2d(7, 4, 4, 4, green, k ? 5 : 0, k ? point : NULL);
		subtexload(TX_TEXTURE_0, 7, 0.0f, 0.5f, 0.0f, 1.0f, 8, red, 0);
		texbind(TX_TEXTURE_0, 7);
		tevbind(TV_ENV0, 7);
		textured_quad();
		tevbind(TV_ENV0, 0);
		texbind(TX_TEXTURE_0, 0);
		a = pix(15, 30);
		b = pix(45, 30);
		sprintf(what, "subtexload the left half red (%s): %06lx | %06lx", k ? "plain" : "mipmapped", a, b);
		check(what, a == C_RED && b == C_GREEN);
	}
	winclose(gid);
}

/* The accumulation buffer: half of red, returned. */
static void
t_acbuf(void)
{
	long gid;
	unsigned long p;
	char what[96];

	prefsize(W, H);
	gid = winopen("irisgltest acbuf");
	RGBmode();
	acsize(16);
	gconfig();
	pixel_ortho(0);
	cpack(C_RED);
	clear();
	acbuf(AC_CLEAR_ACCUMULATE, 0.5f);
	cpack(C_BLACK);
	clear();
	acbuf(AC_RETURN, 1.0f);
	p = pix(30, 30);
	sprintf(what, "acbuf: half of red accumulated and returned: %06lx", p);
	check(what, chan(p, 0) > 110 && chan(p, 0) < 145 && chan(p, 8) == 0);
	winclose(gid);
}

/* Old-style curves and patches on a Bezier basis (GLPG-II 14-35). */
static void
t_oldcurves(void)
{
	static Matrix bezier = {
		{ -1.0f, 3.0f, -3.0f, 1.0f }, { 3.0f, -6.0f, 3.0f, 0.0f },
		{ -3.0f, 3.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f }
	};
	/* a straight Bezier from (10,10) to (50,10), and an arch above it */
	static Coord line[4][3] = { { 10, 10, 0 }, { 23, 10, 0 }, { 37, 10, 0 }, { 50, 10, 0 } };
	/* a flat patch over 60..90: x varies with v (columns), y with u (rows) */
	static Matrix gx = {
		{ 60, 70, 80, 90 }, { 60, 70, 80, 90 }, { 60, 70, 80, 90 }, { 60, 70, 80, 90 }
	};
	static Matrix gy = {
		{ 60, 60, 60, 60 }, { 70, 70, 70, 70 }, { 80, 80, 80, 80 }, { 90, 90, 90, 90 }
	};
	static Matrix gz = { { 0 } };
	long gid = rgb_window("irisgltest curves");
	char what[96];

	defbasis(1, bezier);
	curvebasis(1);
	curveprecision(20);
	cpack(C_RED);
	crv(line);
	sprintf(what, "crv draws a Bezier segment (30,10): %06lx", pix(30, 10));
	check(what, pix(30, 10) == C_RED && pix(50, 10) == C_RED);

	patchbasis(1, 1);
	patchcurves(4, 4);
	patchprecision(12, 12);
	cpack(C_GREEN);
	patch(gx, gy, gz);
	sprintf(what, "patch draws its wireframe edges (60,75) (75,60): %06lx %06lx", pix(60, 75), pix(75, 60));
	check(what, pix(60, 75) == C_GREEN && pix(75, 60) == C_GREEN && pix(65, 65) == C_BLACK);
	winclose(gid);
}

static void
t_blend(void)
{
	long gid = rgb_window("irisgltest blend");
	unsigned long p;
	char what[96];

	cpack(C_BLUE);
	clear();
	blendfunction(BF_SA, BF_MSA);
	cpack(0x800000ffUL);            /* red at half alpha */
	rectf(10, 10, 30, 30);
	blendfunction(BF_ONE, BF_ZERO);
	p = pix(20, 20);
	sprintf(what, "blendfunction(BF_SA, BF_MSA) mixes half and half: %06lx", p);
	check(what, chan(p, 0) > 100 && chan(p, 0) < 156 && chan(p, 16) > 100 && chan(p, 16) < 156);
	winclose(gid);
}

static void
t_texture(void)
{
	static unsigned long white[4] = { 0xffffffffUL, 0xffffffffUL, 0xffffffffUL, 0xffffffffUL };
	static float tprops[] = { TX_MINFILTER, TX_POINT, TX_NULL };
	static float tevprops[] = { TV_MODULATE, TV_NULL };
	long gid = rgb_window("irisgltest texture");
	unsigned long p;
	char what[96];

	texdef2d(1, 4, 2, 2, white, 3, tprops);
	tevdef(1, 2, tevprops);
	texbind(TX_TEXTURE_0, 1);
	tevbind(TV_ENV0, 1);
	cpack(C_GREEN);
	{
		static float t[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		static float v[4][2] = { { 10, 10 }, { 50, 10 }, { 50, 50 }, { 10, 50 } };
		int i;
		bgnpolygon();
		for (i = 0; i < 4; i++) {
			t2f(t[i]);
			v2f(v[i]);
		}
		endpolygon();
	}
	texbind(TX_TEXTURE_0, 0);
	p = pix(30, 30);
	sprintf(what, "a white texture modulating green is green: %06lx", p);
	check(what, p == C_GREEN);
	winclose(gid);
}

static void
t_nurbs(void)
{
	/* A bilinear patch: order 2 both ways, a 2x2 grid of control points. */
	static double knots[4] = { 0, 0, 1, 1 };
	static double ctl[2][2][3] = {
		{ { 10, 10, 0 }, { 10, 50, 0 } },
		{ { 50, 10, 0 }, { 50, 50, 0 } },
	};
	long gid = rgb_window("irisgltest nurbs");
	unsigned long p;
	char what[96];

	cpack(C_GREEN);
	bgnsurface();
	nurbssurface(4, knots, 4, knots, sizeof ctl[0], sizeof ctl[0][0],
	    &ctl[0][0][0], 2, 2, N_XYZ);
	endsurface();
	p = pix(30, 30);
	sprintf(what, "a NURBS surface is drawn: %06lx", p);
	check(what, p == C_GREEN);
	check("... and only where it is", pix(5, 5) == C_BLACK && pix(58, 58) == C_BLACK);
	winclose(gid);
}

/* The X window named `name`, searched for from `w` down. */
static Window
named_window(Display *d, Window w, const char *name)
{
	Window root, parent, *kids, found = None;
	unsigned int n, i;
	char *wn = NULL;

	if (XFetchName(d, w, &wn) && wn != NULL) {
		int same = strcmp(wn, name) == 0;

		XFree(wn);
		if (same)
			return w;
	}
	if (!XQueryTree(d, w, &root, &parent, &kids, &n))
		return None;
	for (i = 0; i < n && found == None; i++)
		found = named_window(d, kids[i], name);
	if (kids)
		XFree(kids);
	return found;
}

/* A square over (40..50, 40..50) at depth z. */
static void
square(float z)
{
	float v[4][3];
	int i;

	for (i = 0; i < 4; i++) {
		v[i][0] = i == 1 || i == 2 ? 50.0f : 40.0f;
		v[i][1] = i >= 2 ? 50.0f : 40.0f;
		v[i][2] = z;
	}
	bgnpolygon();
	for (i = 0; i < 4; i++)
		v3f(v[i]);
	endpolygon();
}

/*
 * The overlay and popup planes, where the server has an overlay visual: what
 * is drawn in OVERDRAW is an index in the overlay (a child window, 8 bits,
 * read back here through X), leaves the normal planes as they were, and
 * index 0 is transparent. The z-buffer is the window's, not the draw mode's:
 * turned off in the overlay, it is off for normal drawing.
 */
static void
t_layers(void)
{
	static const char name[] = "irisgltest layers";
	Display *d;
	Window w, over = None, root, parent, *kids;
	unsigned int n, i;
	XWindowAttributes wa;
	XImage *im;
	short r, g, b;
	long gid, bits = getgdesc(GD_BITS_OVER_SNG_CMODE);
	char what[96];

	printf("=    getgdesc: overlay %ld bits, popup %ld\n", bits, getgdesc(GD_BITS_PUP_SNG_CMODE));
	if (bits <= 0) {
		printf("=    no overlay planes: the layer checks are skipped\n");
		return;
	}
	check("getgdesc(GD_BITS_OVER_SNG_CMODE) is 8", bits == 8);
	gid = rgb_window(name);
	zbuffer(TRUE);
	cpack(C_RED);
	clear();
	drawmode(OVERDRAW);
	mapcolor(5, 0, 255, 0);
	color(0);
	clear();
	color(5);
	rectf(8, 8, 23, 23);
	/* Off here, off for the normal planes too. */
	zbuffer(FALSE);
	drawmode(NORMALDRAW);
	gflush();
	check("drawing in the overlay leaves the normal planes alone", pix(15, 15) == C_RED);
	/* A near square, then a far one over it: with the z-buffer on the near
	 * one would stay. */
	pixel_ortho(1);
	zclear();
	cpack(C_BLUE);
	square(0.5f);
	cpack(C_GREEN);
	square(-0.5f);
	gflush();
	check("the z-buffer turned off in the overlay is off for normal drawing", pix(45, 45) == C_GREEN);
	drawmode(OVERDRAW);
	getmcolor(5, &r, &g, &b);
	check("the overlay's own colour map", r == 0 && g == 255 && b == 0);
	drawmode(NORMALDRAW);

	if ((d = XOpenDisplay(NULL)) == NULL) {
		check("an X connection to look at the overlay", 0);
		winclose(gid);
		return;
	}
	w = named_window(d, DefaultRootWindow(d), name);
	if (w != None && XQueryTree(d, w, &root, &parent, &kids, &n)) {
		for (i = 0; i < n; i++)
			if (XGetWindowAttributes(d, kids[i], &wa) && wa.depth == 8)
				over = kids[i];
		if (kids)
			XFree(kids);
	}
	check("the window has an 8-bit overlay window over it", over != None);
	if (over != None && (im = XGetImage(d, over, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap)) != NULL) {
		unsigned long in = XGetPixel(im, 15, wa.height - 1 - 15);
		unsigned long out = XGetPixel(im, 40, wa.height - 1 - 40);

		sprintf(what, "the overlay holds index 5 in the rectangle, 0 around it (%lu, %lu)", in, out);
		check(what, in == 5 && out == 0);
		XDestroyImage(im);
	}
	XCloseDisplay(d);
	winclose(gid);
}

/*
 * The GLX mixed model with an overlay: the program asks GLXgetconfig for the
 * normal planes and an 8-bit overlay, makes a window for each from the
 * answer (the overlay's a child of the normal one), links them and draws in
 * both with GLXwinset. The overlay is a window in the overlay visual whose
 * pixels are the indices drawn, in the colours of the colormap the answer
 * named.
 */
static void
t_glx_mixed(void)
{
	GLXconfig want[] = {
		{ GLX_NORMAL, GLX_RGB, TRUE },
		{ GLX_NORMAL, GLX_DOUBLE, FALSE },
		{ GLX_OVERLAY, GLX_BUFSIZE, 8 },
		{ 0, 0, 0 }
	};
	GLXconfig *conf, *c;
	Display *d;
	XSetWindowAttributes swa;
	XVisualInfo tmpl, *nvi = NULL, *ovi = NULL;
	Colormap ncmap = None, ocmap = None;
	Window nwin, owin;
	XImage *im;
	short r, g, b;
	int n;
	char what[96];

	if (getgdesc(GD_BITS_OVER_SNG_CMODE) <= 0 || (d = XOpenDisplay(NULL)) == NULL) {
		printf("=    no overlay planes: the mixed-model checks are skipped\n");
		return;
	}
	conf = GLXgetconfig(d, DefaultScreen(d), want);
	check("GLXgetconfig answers", conf != NULL);
	if (conf == NULL)
		return;
	for (c = conf; c->buffer; c++) {
		if (c->mode == GLX_VISUAL) {
			tmpl.visualid = (VisualID)c->arg;
			if (c->buffer == GLX_NORMAL)
				nvi = XGetVisualInfo(d, VisualIDMask, &tmpl, &n);
			else if (c->buffer == GLX_OVERLAY && c->arg)
				ovi = XGetVisualInfo(d, VisualIDMask, &tmpl, &n);
		}
		if (c->mode == GLX_COLORMAP && c->buffer == GLX_NORMAL)
			ncmap = (Colormap)c->arg;
		if (c->mode == GLX_COLORMAP && c->buffer == GLX_OVERLAY)
			ocmap = (Colormap)c->arg;
	}
	check("the overlay's visual is 8 bits of colour index", ovi != NULL && ovi->depth == 8 && ovi->class == PseudoColor);
	if (nvi == NULL || ovi == NULL) {
		XCloseDisplay(d);
		return;
	}
	swa.colormap = ncmap;
	swa.border_pixel = 0;
	nwin = XCreateWindow(d, RootWindow(d, nvi->screen), 0, 0, W, H, 0, nvi->depth, InputOutput,
	    nvi->visual, CWColormap | CWBorderPixel, &swa);
	XStoreName(d, nwin, "irisgltest mixed");
	swa.colormap = ocmap;
	swa.background_pixel = 0;
	owin = XCreateWindow(d, nwin, 0, 0, W, H, 0, ovi->depth, InputOutput, ovi->visual,
	    CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XMapWindow(d, owin);
	XMapWindow(d, nwin);
	XSync(d, False);
	sleep(1);
	for (c = conf; c->buffer; c++)
		if (c->mode == GLX_WINDOW)
			c->arg = c->buffer == GLX_NORMAL ? (int)nwin : c->buffer == GLX_OVERLAY ? (int)owin : 0;
	check("GLXlink", GLXlink(d, conf) == GLWS_NOERROR);

	check("GLXwinset the normal window", GLXwinset(d, nwin) == GLWS_NOERROR);
	pixel_ortho(0);
	cpack(C_RED);
	clear();
	gflush();

	check("GLXwinset the overlay window", GLXwinset(d, owin) == GLWS_NOERROR);
	pixel_ortho(0);
	mapcolor(3, 0, 0, 255);
	getmcolor(3, &r, &g, &b);
	sprintf(what, "mapcolor in the overlay, read back (%d %d %d)", r, g, b);
	check(what, r == 0 && g == 0 && b == 255);
	color(0);
	clear();
	color(3);
	rectf(8, 8, 23, 23);
	gflush();
	XSync(d, False);
	im = XGetImage(d, owin, 0, 0, W, H, AllPlanes, ZPixmap);
	if (im != NULL) {
		unsigned long in = XGetPixel(im, 15, H - 1 - 15), out = XGetPixel(im, 40, H - 1 - 40);

		sprintf(what, "the overlay window holds index 3 in the rectangle, 0 around it (%lu, %lu)", in, out);
		check(what, in == 3 && out == 0);
		XDestroyImage(im);
	} else {
		check("XGetImage of the overlay window", 0);
	}
	GLXwinset(d, nwin);
	check("the normal window keeps its own pixels", pix(15, 15) == C_RED);
	GLXunlink(d, owin);
	GLXunlink(d, nwin);
	XDestroyWindow(d, nwin);
	XCloseDisplay(d);
}

/*
 * A colour-index program's normal planes in the GLX mixed model: IMPACT's
 * 12-bit colour-index visual, whose pixels are the indices drawn -- all 12
 * bits of them -- in the colours of its colormap.
 */
static void
t_glx_cmode(void)
{
	GLXconfig want[] = {
		{ GLX_NORMAL, GLX_DOUBLE, FALSE },
		{ 0, 0, 0 }
	};
	GLXconfig *conf, *c;
	Display *d;
	XSetWindowAttributes swa;
	XVisualInfo tmpl, *vi = NULL;
	Colormap cmap = None;
	Window win;
	XImage *im;
	short r, g, b;
	int n;
	char what[96];

	if ((d = XOpenDisplay(NULL)) == NULL)
		return;
	conf = GLXgetconfig(d, DefaultScreen(d), want);
	for (c = conf; c != NULL && c->buffer; c++) {
		if (c->buffer == GLX_NORMAL && c->mode == GLX_VISUAL) {
			tmpl.visualid = (VisualID)c->arg;
			vi = XGetVisualInfo(d, VisualIDMask, &tmpl, &n);
		}
		if (c->buffer == GLX_NORMAL && c->mode == GLX_COLORMAP)
			cmap = (Colormap)c->arg;
	}
	if (vi == NULL || vi->class != PseudoColor) {
		printf("=    colour-index GLX windows are not colour-map windows here: skipped\n");
		XCloseDisplay(d);
		return;
	}
	check("a colour-index GLX window is 12 bits deep", vi->depth == 12);
	swa.colormap = cmap;
	swa.border_pixel = 0;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput,
	    vi->visual, CWColormap | CWBorderPixel, &swa);
	XStoreName(d, win, "irisgltest cmode");
	XMapWindow(d, win);
	XSync(d, False);
	sleep(1);
	for (c = conf; c->buffer; c++)
		if (c->mode == GLX_WINDOW)
			c->arg = c->buffer == GLX_NORMAL ? (int)win : 0;
	check("GLXlink the colour-index window", GLXlink(d, conf) == GLWS_NOERROR);
	GLXwinset(d, win);
	pixel_ortho(0);
	mapcolor(1000, 255, 128, 0);
	getmcolor(1000, &r, &g, &b);
	sprintf(what, "mapcolor 1000 in its colormap, read back (%d %d %d)", r, g, b);
	check(what, r == 255 && g == 128 && b == 0);
	color(7);
	clear();
	color(1000);
	rectf(8, 8, 23, 23);
	gflush();
	XSync(d, False);
	im = XGetImage(d, win, 0, 0, W, H, AllPlanes, ZPixmap);
	if (im != NULL) {
		unsigned long in = XGetPixel(im, 15, H - 1 - 15), out = XGetPixel(im, 40, H - 1 - 40);

		sprintf(what, "its pixels are the indices: 1000 in the rectangle, 7 around it (%lu, %lu)", in, out);
		check(what, in == 1000 && out == 7);
		XDestroyImage(im);
	} else {
		check("XGetImage of the colour-index window", 0);
	}
	GLXunlink(d, win);
	XDestroyWindow(d, win);
	XCloseDisplay(d);
}

int
main(void)
{
	foreground();
	t_window();
	t_gdesc();
	t_clear_rect();
	t_cmode();
	t_matrix();
	t_perspective();
	t_zbuffer();
	t_stencil();
	t_backface();
	t_front();
	t_pixels();
	t_text();
	t_objects();
	t_queue();
	t_lighting();
	t_blend();
	t_texture();
	t_zclear();
	t_shininess();
	t_texenv();
	t_mmode_single();
	t_select();
	t_blendfactors();
	t_oldpolygon();
	t_stencil_zoff();
	t_depthcue();
	t_objects_state();
	t_config_state();
	t_zdata();
	t_mapw_mipmap();
	t_subtexload();
	t_acbuf();
	t_oldcurves();
	t_nurbs();
	t_layers();
	t_glx_mixed();
	t_glx_cmode();
	printf("%s\n", failures ? "irisgltest: FAILED" : "irisgltest: all ok");
	return failures != 0;
}
