/*
 * OpenGL conformance checks for IRIS's host GL, run inside the guest with
 * the shim installed as libGL.so (see build.sh), and DISPLAY set:
 *
 *   gltest
 *
 * Each check sets state or draws, then reads it back -- glGet, glReadPixels,
 * the selection and feedback buffers -- and prints ok or FAIL with what it
 * saw. Together they cover every way an argument crosses to the host: counts
 * in names, SGI's pname tables, glGet counts, arrays copied into the command
 * buffer and arrays too big for it, pixel data read and written in place in
 * both byte orders, client arrays read at draw time, the deferred feedback and
 * selection buffers, evaluator maps, and the EXT names. The expected values
 * are what the OpenGL 1.1 specification requires, so the same binary checks
 * SGI's own libGL too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#define W 64
#define H 64

static int failures;

static void
check(const char *what, int good)
{
	printf("%s %s\n", good ? "ok  " : "FAIL", what);
	if (!good)
		failures++;
}

static int
near(double a, double b)
{
	return fabs(a - b) < 1e-4;
}

/* The colour at (x, y) of what was just drawn, 0xRRGGBB. */
static unsigned long
pixel(int x, int y)
{
	GLubyte p[4];

	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	return ((unsigned long)p[0] << 16) | (p[1] << 8) | p[2];
}

static void
reset_view(void)
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void
t_state(void)
{
	GLfloat f[16];
	GLint v[4];
	GLboolean b[4];
	GLdouble d[16];
	GLfloat light[4];
	GLdouble plane[4];

	glColor4f(0.25f, 0.5f, 0.75f, 1.0f);
	glGetFloatv(GL_CURRENT_COLOR, f);
	check("glGetFloatv CURRENT_COLOR (4 values)", near(f[0], 0.25) && near(f[1], 0.5) && near(f[2], 0.75) && near(f[3], 1));

	glViewport(0, 0, W, H);
	glGetIntegerv(GL_VIEWPORT, v);
	check("glGetIntegerv VIEWPORT (4 values)", v[0] == 0 && v[1] == 0 && v[2] == W && v[3] == H);

	glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
	glGetBooleanv(GL_COLOR_WRITEMASK, b);
	check("glGetBooleanv COLOR_WRITEMASK", b[0] && !b[1] && b[2] && !b[3]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslated(1.5, -2.0, 3.25);
	glGetDoublev(GL_MODELVIEW_MATRIX, d);
	check("glGetDoublev MODELVIEW_MATRIX (16 doubles)", near(d[12], 1.5) && near(d[13], -2) && near(d[14], 3.25) && near(d[15], 1));
	glLoadIdentity();

	light[0] = 1; light[1] = 2; light[2] = 3; light[3] = 0;
	glLightfv(GL_LIGHT1, GL_POSITION, light);
	glGetLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, f);
	check("glGetLightfv SPOT_DIRECTION default (SGI table: 3)", near(f[0], 0) && near(f[1], 0) && near(f[2], -1));
	light[0] = 0.1f; light[1] = 0.2f; light[2] = 0.3f; light[3] = 0.4f;
	glLightfv(GL_LIGHT1, GL_DIFFUSE, light);
	glGetLightfv(GL_LIGHT1, GL_DIFFUSE, f);
	check("glLightfv/glGetLightfv DIFFUSE (SGI table: 4)", near(f[0], 0.1) && near(f[3], 0.4));

	plane[0] = 1; plane[1] = -1; plane[2] = 0.5; plane[3] = 7;
	glClipPlane(GL_CLIP_PLANE2, plane);
	glGetClipPlane(GL_CLIP_PLANE2, plane);
	check("glClipPlane/glGetClipPlane (4 doubles)", near(plane[0], 1) && near(plane[1], -1) && near(plane[2], 0.5) && near(plane[3], 7));

	glEnable(GL_CULL_FACE);
	check("glIsEnabled", glIsEnabled(GL_CULL_FACE) == GL_TRUE);
	glDisable(GL_CULL_FACE);

	/* Anything left over from earlier, so this asks about its own error. */
	while (glGetError() != GL_NO_ERROR)
		;
	glEnable(0x7777);
	check("glGetError after a bad enum is INVALID_ENUM", glGetError() == GL_INVALID_ENUM);
	check("glGetError is then NO_ERROR", glGetError() == GL_NO_ERROR);

	check("glGetString VENDOR", glGetString(GL_VENDOR) != NULL && glGetString(GL_VENDOR)[0] != 0);

	{
		/*
		 * The extensions must be the ones this implementation provides:
		 * the SGI and EXT ones an IRIX program looks for, and none of the
		 * host's own, whose entry points are not here to call.
		 */
		const char *e = (const char *)glGetString(GL_EXTENSIONS);
		int n = 0;
		const char *p;

		if (e == NULL)
			e = "";
		for (p = e; *p; p++)
			if (*p == ' ')
				n++;
		printf("=    %d extensions: %.200s\n", e[0] ? n + 1 : 0, e);
		check("extensions include GL_EXT_texture_object", strstr(e, "GL_EXT_texture_object") != NULL);
		check("extensions include GL_SGIS_texture_edge_clamp", strstr(e, "GL_SGIS_texture_edge_clamp") != NULL);
		check("extensions include GL_SGI_color_table", strstr(e, "GL_SGI_color_table") != NULL);
		check("extensions carry none of the host's own", strstr(e, "GL_APPLE") == NULL && strstr(e, "GL_NV_") == NULL);
		/* SGIX_sprite is emulated now; clipmap is the one still missing,
		 * and the string must keep saying so. */
		check("extensions include GL_SGIX_sprite", strstr(e, "GL_SGIX_sprite") != NULL);
		check("extensions include GL_SGIX_clipmap", strstr(e, "GL_SGIX_clipmap") != NULL);
		/* SGIX_instruments stays deliberately quiet: the calls are accepted
		 * so a program runs, but nothing measures anything, and a program
		 * would believe the numbers it read back. */
		check("extensions leave out what only pretends to work", strstr(e, "GL_SGIX_instruments") == NULL);
	}

	glBlendColorEXT(0.5f, 0.25f, 0.125f, 1.0f);
	glGetFloatv(GL_BLEND_COLOR_EXT, f);
	check("glBlendColorEXT (an EXT name) and GL_BLEND_COLOR_EXT", near(f[0], 0.5) && near(f[1], 0.25) && near(f[2], 0.125));
}

static void
t_draw(void)
{
	reset_view();
	glColor3f(1, 0, 0);
	glBegin(GL_QUADS);
	glVertex2i(8, 8);
	glVertex2i(24, 8);
	glVertex2i(24, 24);
	glVertex2i(8, 24);
	glEnd();
	check("immediate mode quad is drawn", pixel(16, 16) == 0xff0000 && pixel(40, 40) == 0);
}

static void
t_lists(void)
{
	GLuint base = glGenLists(3);
	GLubyte names1[2];
	GLubyte names2[4];
	GLuint *many;
	int i;

	check("glGenLists", base != 0);
	glNewList(base, GL_COMPILE);
	glColor3f(0, 1, 0);
	glRecti(0, 0, 8, 8);
	glEndList();
	glNewList(base + 1, GL_COMPILE);
	glColor3f(0, 0, 1);
	glRecti(8, 0, 16, 8);
	glEndList();
	glNewList(base + 2, GL_COMPILE);
	glEndList();
	check("glIsList", glIsList(base) && !glIsList(base + 100));

	reset_view();
	glCallList(base);
	check("glCallList", pixel(4, 4) == 0x00ff00 && pixel(12, 4) == 0);

	reset_view();
	glListBase(base);
	names1[0] = 0;
	names1[1] = 1;
	glCallLists(2, GL_UNSIGNED_BYTE, names1);
	check("glCallLists GL_UNSIGNED_BYTE", pixel(4, 4) == 0x00ff00 && pixel(12, 4) == 0x0000ff);

	reset_view();
	names2[0] = 0; names2[1] = 1; /* 1 */
	names2[2] = 0; names2[3] = 0; /* 0 */
	glCallLists(2, GL_2_BYTES, names2);
	check("glCallLists GL_2_BYTES (big-endian by definition)", pixel(4, 4) == 0x00ff00 && pixel(12, 4) == 0x0000ff);

	/* 20000 names: past what fits in the command buffer, so by reference. */
	reset_view();
	many = malloc(20000 * sizeof *many);
	for (i = 0; i < 20000; i++)
		many[i] = 2;
	many[19999] = 1;
	glCallLists(20000, GL_UNSIGNED_INT, many);
	check("glCallLists 20000 GL_UNSIGNED_INT (by reference)", pixel(12, 4) == 0x0000ff && pixel(4, 4) == 0);
	free(many);
	glListBase(0);
	glDeleteLists(base, 3);
}

static void
t_arrays(void)
{
	static GLfloat verts[] = { 32, 32, 48, 32, 48, 48, 32, 48 };
	static GLubyte colors[] = { 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255 };
	static GLushort idx[] = { 0, 1, 2, 3 };
	static struct {
		GLubyte c[4];
		GLfloat v[3];
	} inter[4] = {
		{ { 0, 255, 255, 255 }, { 4, 40, 0 } },
		{ { 0, 255, 255, 255 }, { 20, 40, 0 } },
		{ { 0, 255, 255, 255 }, { 20, 56, 0 } },
		{ { 0, 255, 255, 255 }, { 4, 56, 0 } },
	};
	GLvoid *got;
	int i;

	reset_view();
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glDrawArrays(GL_QUADS, 0, 4);
	check("glDrawArrays with float vertices and ubyte colours", pixel(40, 40) == 0xffff00);

	glGetPointerv(GL_VERTEX_ARRAY_POINTER, &got);
	check("glGetPointerv VERTEX_ARRAY_POINTER", got == (GLvoid *)verts);

	reset_view();
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, idx);
	check("glDrawElements with short indices", pixel(40, 40) == 0xffff00);

	/* glArrayElement reads the arrays when called: changing them after must
	 * not change what was drawn. */
	reset_view();
	glBegin(GL_QUADS);
	for (i = 0; i < 4; i++)
		glArrayElement(i);
	for (i = 0; i < 16; i += 4)
		colors[i + 1] = 0;
	glEnd();
	check("glArrayElement reads the arrays at the call", pixel(40, 40) == 0xffff00);
	for (i = 0; i < 16; i += 4)
		colors[i + 1] = 255;
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

	reset_view();
	glInterleavedArrays(GL_C4UB_V3F, 0, inter);
	glDrawArrays(GL_QUADS, 0, 4);
	check("glInterleavedArrays C4UB_V3F", pixel(12, 48) == 0x00ffff);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

static void
t_pixels(void)
{
	GLubyte rgba[4 * 4 * 4];
	GLubyte abgr[4 * 4 * 4];
	GLushort lum[4 * 4];
	GLubyte back[4 * 4 * 4];
	GLushort sback[4 * 4];
	GLubyte stipple[32 * 4];
	GLubyte stipple_back[32 * 4];
	static GLubyte bits[] = { 0xff, 0x80, 0x80, 0xff };
	GLuint tex;
	int i, same;

	for (i = 0; i < 16; i++) {
		rgba[i * 4] = i * 16;
		rgba[i * 4 + 1] = 255 - i * 16;
		rgba[i * 4 + 2] = 128;
		rgba[i * 4 + 3] = 255;
		lum[i] = (GLushort)(i * 4096 + 0x0102);
	}

	reset_view();
	glRasterPos2i(10, 10);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glReadPixels(10, 10, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, back);
	check("glDrawPixels/glReadPixels RGBA ubyte", memcmp(rgba, back, sizeof rgba) == 0);

	/*
	 * EXT_abgr: SGI's component order, reversed from RGBA. The Mac's GL has
	 * no such format at all, so the components are turned round on the way
	 * through -- once on the way in and once on the way out, which is why
	 * both directions are checked rather than a round trip that would pass
	 * just as well if neither happened.
	 */
	for (i = 0; i < 16; i++) {
		abgr[i * 4] = rgba[i * 4 + 3];
		abgr[i * 4 + 1] = rgba[i * 4 + 2];
		abgr[i * 4 + 2] = rgba[i * 4 + 1];
		abgr[i * 4 + 3] = rgba[i * 4];
	}
	reset_view();
	glRasterPos2i(10, 10);
	glDrawPixels(4, 4, GL_ABGR_EXT, GL_UNSIGNED_BYTE, abgr);
	memset(back, 0, sizeof back);
	glReadPixels(10, 10, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, back);
	check("glDrawPixels GL_ABGR_EXT", memcmp(rgba, back, sizeof rgba) == 0);

	memset(back, 0, sizeof back);
	glReadPixels(10, 10, 4, 4, GL_ABGR_EXT, GL_UNSIGNED_BYTE, back);
	check("glReadPixels GL_ABGR_EXT", memcmp(abgr, back, sizeof abgr) == 0);

	/*
	 * EXT_packed_pixels' later types. This header numbers 5_6_5 and
	 * 2_3_3_REV the other way round from the Mac's GL, so each is drawn in
	 * a colour the other meaning would not give, and read back.
	 */
	for (i = 0; i < 16; i++) {
		sback[i] = 0;
		lum[i] = 0xF800;                        /* 5_6_5 red */
		stipple[i] = 0x38;                      /* 2_3_3_REV green */
	}
	reset_view();
	glRasterPos2i(10, 10);
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, lum);
	glRasterPos2i(20, 10);
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_BYTE_2_3_3_REV, stipple);
	check("glDrawPixels UNSIGNED_SHORT_5_6_5", pixel(10, 10) == 0xff0000 && pixel(13, 13) == 0xff0000 &&
	    pixel(14, 10) == 0);
	check("glDrawPixels UNSIGNED_BYTE_2_3_3_REV", pixel(20, 10) == 0x00ff00 && pixel(23, 13) == 0x00ff00);
	glReadPixels(10, 10, 4, 4, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, sback);
	for (same = 1, i = 0; i < 16; i++)
		same &= sback[i] == 0xF800;
	check("glReadPixels UNSIGNED_SHORT_5_6_5", same);
	glReadPixels(10, 10, 4, 4, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, back);
	check("glReadPixels UNSIGNED_INT_8_8_8_8_REV", ((GLuint *)back)[0] == 0xff0000ffu);

	/* 16-bit components: the byte order matters, and the host's is not ours. */
	reset_view();
	glRasterPos2i(10, 10);
	glDrawPixels(4, 4, GL_LUMINANCE, GL_UNSIGNED_SHORT, lum);
	glReadPixels(10, 10, 4, 4, GL_RED, GL_UNSIGNED_SHORT, sback);
	/*
	 * A 16-bit component becomes the framebuffer's 8 bits as v/65535 x 255
	 * (1.1 section 2.13), not as its top byte: 0x9102 is 144, where the top
	 * byte says 145. Reading back as 16 bits expands those 8 bits again. So
	 * the check is against that conversion, to a bit either way -- and it is
	 * still a check of byte order, because bytes the wrong way round are
	 * nowhere near.
	 */
	same = 1;
	for (i = 0; i < 16; i++) {
		int want = (int)(lum[i] / 65535.0 * 255.0 + 0.5);
		int got = sback[i] >> 8;
		if (got < want - 1 || got > want + 1) {
			printf("=    pixel %d: wrote %04x, wanted about %02x, read %04x\n", i, lum[i], want, sback[i]);
			same = 0;
		}
	}
	check("glDrawPixels/glReadPixels 16-bit in guest byte order", same);

	glGenTextures(1, &tex);
	check("glGenTextures", tex != 0);
	glBindTextureEXT(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	memset(back, 0, sizeof back);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
	check("glTexImage2D/glGetTexImage (dimensions asked of the host)", memcmp(rgba, back, sizeof rgba) == 0);

	reset_view();
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2i(0, 0);
	glTexCoord2f(1, 0); glVertex2i(64, 0);
	glTexCoord2f(1, 1); glVertex2i(64, 64);
	glTexCoord2f(0, 1); glVertex2i(0, 64);
	glEnd();
	glDisable(GL_TEXTURE_2D);
	check("textured quad shows texel (0,0)", pixel(4, 4) == 0x00ff80);
	glDeleteTextures(1, &tex);

	/*
	 * SGI_texture_color_table: a table between the texel and the fragment.
	 * The table turns red into its opposite, so the textured quad comes out
	 * inverted while it is on and plain when it is off.
	 */
	{
		GLubyte table[256 * 4];
		GLuint t2;
		unsigned long with, without;

		for (i = 0; i < 256; i++) {
			table[i * 4] = 255 - i;
			table[i * 4 + 1] = i;
			table[i * 4 + 2] = i;
			table[i * 4 + 3] = 255;
		}
		glGenTextures(1, &t2);
		glBindTexture(GL_TEXTURE_2D, t2);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glColorTableSGI(GL_TEXTURE_COLOR_TABLE_SGI, GL_RGBA, 256, GL_RGBA, GL_UNSIGNED_BYTE, table);
		glEnable(GL_TEXTURE_2D);
		glEnable(GL_TEXTURE_COLOR_TABLE_SGI);
		check("glIsEnabled of the texture colour table", glIsEnabled(GL_TEXTURE_COLOR_TABLE_SGI) == GL_TRUE);
		reset_view();
		glColor3f(1, 1, 1);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0); glVertex2i(0, 0);
		glTexCoord2f(0, 0); glVertex2i(32, 0);
		glTexCoord2f(0, 0); glVertex2i(32, 32);
		glTexCoord2f(0, 0); glVertex2i(0, 32);
		glEnd();
		with = pixel(4, 4);
		glDisable(GL_TEXTURE_COLOR_TABLE_SGI);
		reset_view();
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0); glVertex2i(0, 0);
		glTexCoord2f(0, 0); glVertex2i(32, 0);
		glTexCoord2f(0, 0); glVertex2i(32, 32);
		glTexCoord2f(0, 0); glVertex2i(0, 32);
		glEnd();
		without = pixel(4, 4);
		printf("=    texel %02x%02x%02x: with the table %#06lx, without %#06lx\n",
		    rgba[0], rgba[1], rgba[2], with, without);
		check("the colour table inverts the red it is given",
		    ((with >> 16) & 0xff) == (255 - ((without >> 16) & 0xff)));
		check("the colour table leaves the other components", ((with >> 8) & 0xff) == ((without >> 8) & 0xff));
		/* SGIX_texture_scale_bias: the texel halved after the filter. */
		{
			GLfloat half[4], none[4];
			unsigned long scaled;

			half[0] = half[1] = half[2] = half[3] = 0.5f;
			none[0] = none[1] = none[2] = none[3] = 0.0f;
			glTexParameterfv(GL_TEXTURE_2D, GL_POST_TEXTURE_FILTER_SCALE_SGIX, half);
			glTexParameterfv(GL_TEXTURE_2D, GL_POST_TEXTURE_FILTER_BIAS_SGIX, none);
			reset_view();
			glBegin(GL_QUADS);
			glTexCoord2f(0, 0); glVertex2i(0, 0);
			glTexCoord2f(0, 0); glVertex2i(32, 0);
			glTexCoord2f(0, 0); glVertex2i(32, 32);
			glTexCoord2f(0, 0); glVertex2i(0, 32);
			glEnd();
			scaled = pixel(4, 4);
			printf("=    scale 0.5: %#06lx from %#06lx\n", scaled, without);
			check("the texel is scaled after the filter",
			    ((scaled >> 8) & 0xff) > 0 && ((scaled >> 8) & 0xff) < ((without >> 8) & 0xff));
			half[0] = half[1] = half[2] = half[3] = 1.0f;
			glTexParameterfv(GL_TEXTURE_2D, GL_POST_TEXTURE_FILTER_SCALE_SGIX, half);
		}
		glDisable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glDeleteTextures(1, &t2);
	}

	for (i = 0; i < (int)sizeof stipple; i++)
		stipple[i] = (GLubyte)(i * 37);
	glPolygonStipple(stipple);
	glGetPolygonStipple(stipple_back);
	check("glPolygonStipple/glGetPolygonStipple", memcmp(stipple, stipple_back, sizeof stipple) == 0);

	reset_view();
	glColor3f(1, 1, 1);
	glRasterPos2i(20, 20);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glBitmap(8, 4, 0, 0, 0, 0, bits);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	check("glBitmap", pixel(20, 20) == 0xffffff && pixel(21, 21) == 0 && pixel(20, 21) == 0xffffff);
}

/*
 * SGIS_fog_function and SGIX_fog_offset: fog by the program's own table, and
 * fog brought nearer the eye. Both are read back as the colour of a fogged
 * quad, against the same quad under ordinary linear fog.
 */
static void
t_fog(void)
{
	static GLfloat points[] = { 0.0f, 1.0f, 4.0f, 0.5f, 8.0f, 0.0f };
	GLfloat offset[4], blue[4];
	unsigned long linear, table, shifted;

	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0f);
	glFogf(GL_FOG_END, 8.0f);
	blue[0] = 0.0f; blue[1] = 0.0f; blue[2] = 1.0f; blue[3] = 1.0f;
	glFogfv(GL_FOG_COLOR, blue);
	glEnable(GL_FOG);
	glColor3f(1, 0, 0);

	/* A quad four units from the eye, where the fog is half way. */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1, 1, -1, 1, 1, 16);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0, 0, -4);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glRectf(-1, -1, 1, 1);
	linear = pixel(W / 2, H / 2);

	while (glGetError() != GL_NO_ERROR)
		;               /* anything left by an earlier check */
	glFogFuncSGIS(3, points);
	glFogi(GL_FOG_MODE, GL_FOG_FUNC_SGIS);
	glClear(GL_COLOR_BUFFER_BIT);
	glRectf(-1, -1, 1, 1);
	table = pixel(W / 2, H / 2);
	check("a fog table is used", glGetError() == GL_NO_ERROR);

	offset[0] = 0; offset[1] = 0; offset[2] = 0; offset[3] = 2.0f;
	glFogfv(GL_FOG_OFFSET_VALUE_SGIX, offset);
	glEnable(GL_FOG_OFFSET_SGIX);
	check("glIsEnabled of the fog offset", glIsEnabled(GL_FOG_OFFSET_SGIX) == GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT);
	glRectf(-1, -1, 1, 1);
	shifted = pixel(W / 2, H / 2);
	printf("=    linear %#06lx, table %#06lx, table with a 2-unit offset %#06lx\n", linear, table, shifted);
	check("the fog offset brings the fog nearer", (shifted >> 16) > (table >> 16));
	glDisable(GL_FOG_OFFSET_SGIX);
	glDisable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_EXP);
	reset_view();
}

/*
 * SGIX_reference_plane: a plane, not the geometry, gives the fragment its
 * depth -- so a decal drawn on a wall meets the depth test exactly rather
 * than fighting it.
 *
 * The check calibrates itself: it first reads the depth a quad at z = -0.5
 * really produces, then draws a quad at z = 0 with the reference plane set to
 * the plane z = -0.5 and asks for the same answer. Nothing here depends on
 * the projection, the depth range or the buffer's precision.
 */
/* The quad t_filter4 draws: a 2x2 texture magnified, sampled where the four
 * texels meet so every tap of the filter has something to say. */
static void
filter4_quad(void)
{
	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2i(0, 0);
	glTexCoord2f(1, 0); glVertex2i(48, 0);
	glTexCoord2f(1, 1); glVertex2i(48, 48);
	glTexCoord2f(0, 1); glVertex2i(0, 48);
	glEnd();
}

/* Two packed RGB colours within `slack` of each other on every component. */
static int
close_rgb(unsigned long a, unsigned long b, int slack)
{
	int i;

	for (i = 0; i < 3; i++) {
		int x = (int)((a >> (i * 8)) & 0xff) - (int)((b >> (i * 8)) & 0xff);
		if (x < -slack || x > slack)
			return 0;
	}
	return 1;
}

static void
t_reference_plane(void)
{
	static GLdouble eq[4] = { 0.0, 0.0, 1.0, 0.5 };  /* z = -0.5 */
	GLfloat want, got, flat;

	reset_view();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glColor3f(1, 1, 1);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_QUADS);
	glVertex3f(0, 0, -0.5f); glVertex3f(W, 0, -0.5f);
	glVertex3f(W, H, -0.5f); glVertex3f(0, H, -0.5f);
	glEnd();
	glReadPixels(W / 2, H / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &want);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glRectf(0, 0, (GLfloat)W, (GLfloat)H);
	glReadPixels(W / 2, H / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &flat);

	while (glGetError() != GL_NO_ERROR)
		;
	glReferencePlaneSGIX(eq);
	glEnable(GL_REFERENCE_PLANE_SGIX);
	check("glIsEnabled of the reference plane", glIsEnabled(GL_REFERENCE_PLANE_SGIX) == GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glRectf(0, 0, (GLfloat)W, (GLfloat)H);
	glReadPixels(W / 2, H / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &got);
	glDisable(GL_REFERENCE_PLANE_SGIX);

	printf("=    depth: quad at z=-0.5 %.4f, flat quad %.4f, flat quad on the plane %.4f\n",
	    want, flat, got);
	check("the reference plane gives the fragment its depth",
	    got > want - 0.01f && got < want + 0.01f && (flat > want + 0.01f || flat < want - 0.01f));

	glDepthFunc(GL_LESS);
	glDisable(GL_DEPTH_TEST);
	reset_view();
}

/*
 * SGIS_texture_filter4: four taps in each direction, weighted by a function
 * the program supplies.
 *
 * The check calibrates itself against the host's own LINEAR. A tent -- f(0)=1
 * falling to f(1)=0 -- makes the specification's four-tap sum collapse to
 * exactly linear interpolation, because the two outer taps get weight zero and
 * the inner two get 1-A and A. So the same quad filtered both ways must come
 * out the same colour. A second pass with a box function, which keeps the
 * nearer texel whole, must then differ: without it a filter4 that quietly did
 * nothing at all would pass the first check.
 */
static void
t_filter4(void)
{
	static GLubyte texels[2 * 2 * 4] = {
		0, 0, 0, 255,       255, 255, 255, 255,
		255, 255, 255, 255, 0, 0, 0, 255
	};
	GLfloat tent[33], box[33];
	GLuint tex;
	unsigned long linear, tented, boxed;
	int i;

	for (i = 0; i < 33; i++) {
		GLfloat d = 2.0f * (GLfloat)i / 32.0f;
		tent[i] = d < 1.0f ? 1.0f - d : 0.0f;
		box[i] = d < 0.5f ? 1.0f : 0.0f;
	}

	reset_view();
	while (glGetError() != GL_NO_ERROR)
		;
	/*
	 * A bare primitive, before any of the filter work: the pixel store is
	 * reset after every command in a batch, and doing that between glBegin
	 * and glEnd is illegal, so this once reported INVALID_OPERATION for a
	 * draw that had nothing wrong with it.
	 */
	glBegin(GL_QUADS);
	glVertex2i(0, 0); glVertex2i(4, 0); glVertex2i(4, 4); glVertex2i(0, 4);
	glEnd();
	check("a plain primitive raises no error", glGetError() == GL_NO_ERROR);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	while (glGetError() != GL_NO_ERROR)
		;               /* anything an earlier check left behind */
	filter4_quad();
	linear = pixel(24, 24);

	while (glGetError() != GL_NO_ERROR)
		;
	glTexFilterFuncSGIS(GL_TEXTURE_2D, GL_FILTER4_SGIS, 33, tent);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_FILTER4_SGIS);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_FILTER4_SGIS);
	filter4_quad();
	tented = pixel(24, 24);
	check("glTexFilterFuncSGIS accepted", glGetError() == GL_NO_ERROR);

	glTexFilterFuncSGIS(GL_TEXTURE_2D, GL_FILTER4_SGIS, 33, box);
	filter4_quad();
	boxed = pixel(24, 24);

	printf("=    filter4 at the texel corner: LINEAR %#06lx, tent %#06lx, box %#06lx\n",
	    linear, tented, boxed);
	check("a tent filter4 is linear interpolation", close_rgb(tented, linear, 4));
	check("a box filter4 is not", !close_rgb(boxed, linear, 4));

	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDeleteTextures(1, &tex);
	reset_view();
}

/*
 * SGIS_sharpen_texture: magnification that extrapolates away from what the
 * coarser mipmap level would have given, T' = (1 + F) T0 - F T1, which puts
 * back the contrast magnification takes out.
 *
 * Level 0 is two greys either side of 0x80 and level 1 is 0x80 itself, so at a
 * texel's own centre T0 is that grey and T1 is 0x80 exactly. With F = 1 the
 * answer is 2 T0 - T1, and the arithmetic is checked rather than just the
 * direction.
 */
static void
t_sharpen(void)
{
	static GLubyte lvl0[2 * 2 * 4] = {
		102, 102, 102, 255,  153, 153, 153, 255,
		153, 153, 153, 255,  102, 102, 102, 255
	};
	static GLubyte lvl1[4] = { 128, 128, 128, 255 };
	static GLfloat pts[4] = { -8.0f, 1.0f, 0.0f, 1.0f };   /* F = 1 throughout */
	GLuint tex;
	unsigned long plain, sharp;
	int want;

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	/*
	 * CLAMP_TO_EDGE, not CLAMP: the 1x1 level 1 is one texel wide, so with
	 * a border the linear filter blends it towards black on both axes and
	 * T1 comes out 0.76^2 of what was put there rather than the value
	 * itself. That is GL behaving correctly and the check reading it wrong.
	 */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, lvl0);
	glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lvl1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);

	filter4_quad();
	plain = pixel(12, 12);

	while (glGetError() != GL_NO_ERROR)
		;
	glSharpenTexFuncSGIS(GL_TEXTURE_2D, 2, pts);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_SHARPEN_SGIS);
	filter4_quad();
	sharp = pixel(12, 12);
	check("glSharpenTexFuncSGIS accepted", glGetError() == GL_NO_ERROR);

	want = 2 * (int)(plain & 0xff) - 128;
	if (want < 0)
		want = 0;
	printf("=    sharpen: plain %#06lx, sharpened %#06lx, 2*T0-T1 wanted %#04x\n",
	    plain, sharp, want);
	check("the sharpened texel is 2*T0 - T1",
	    (int)(sharp & 0xff) > want - 6 && (int)(sharp & 0xff) < want + 6);

	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDeleteTextures(1, &tex);
	reset_view();
}

/*
 * SGIX_interlace: "all of the groups which belong to a row m are treated as
 * if they belonged to the row 2 * m", which spreads an h-row image over 2h-1
 * rows and leaves the odd ones undefined. A video field, drawn into every
 * other scanline.
 *
 * Four rows of distinct colours are drawn interlaced, and the even
 * destination rows must carry them in order. The odd rows are deliberately not
 * checked: the specification says only every other row is defined.
 */
static void
t_interlace(void)
{
	GLubyte src[4 * 4 * 4];
	unsigned long got[7];
	int i, ok;

	for (i = 0; i < 16; i++) {
		src[i * 4] = (GLubyte)(40 * (i / 4) + 20);   /* one red per row */
		src[i * 4 + 1] = 0;
		src[i * 4 + 2] = 0;
		src[i * 4 + 3] = 255;
	}
	reset_view();
	while (glGetError() != GL_NO_ERROR)
		;
	glEnable(GL_INTERLACE_SGIX);
	check("glIsEnabled of interlace", glIsEnabled(GL_INTERLACE_SGIX) == GL_TRUE);
	glRasterPos2i(10, 10);
	glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, src);
	for (i = 0; i < 7; i++)
		got[i] = pixel(10, 10 + i);
	glDisable(GL_INTERLACE_SGIX);
	check("interlace raised no error", glGetError() == GL_NO_ERROR);

	printf("=    interlace rows:");
	for (i = 0; i < 7; i++)
		printf(" %#06lx", got[i]);
	printf("\n");
	ok = 1;
	for (i = 0; i < 4; i++)
		if ((got[i * 2] >> 16) != (unsigned long)(40 * i + 20))
			ok = 0;
	check("each source row lands on an even destination row", ok);
	reset_view();
}

/*
 * SGIS_detail_texture: a second, finer texture blended in by level of detail,
 * so magnification has something to show instead of a blur.
 *
 *   ADD: T' = T + F(LOD) * (2 Td - 1)
 *
 * The base texture is mid-grey and the detail texture is white, so with F = 1
 * the sum is 0.5 + 1 * (2*1 - 1) = 1.5, clamped to white -- and with a black
 * detail texture it is 0.5 - 1 = -0.5, clamped to black. Both directions are
 * checked because a detail texture that is quietly ignored sits at 0.5.
 */
static void
t_detail(void)
{
	static GLubyte base[4] = { 128, 128, 128, 255 };
	static GLubyte white[4] = { 255, 255, 255, 255 };
	static GLubyte black[4] = { 0, 0, 0, 255 };
	static GLfloat pts[4] = { -8.0f, 1.0f, 0.0f, 1.0f };   /* F = 1 throughout */
	GLuint tex;
	unsigned long plain, lighter, darker;

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, base);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	filter4_quad();
	plain = pixel(24, 24);

	while (glGetError() != GL_NO_ERROR)
		;
	glDetailTexFuncSGIS(GL_TEXTURE_2D, 2, pts);
	glTexParameteri(GL_TEXTURE_2D, GL_DETAIL_TEXTURE_MODE_SGIS, GL_ADD);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_DETAIL_SGIS);
	glTexImage2D(GL_DETAIL_TEXTURE_2D_SGIS, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	filter4_quad();
	lighter = pixel(24, 24);
	check("detail texture raised no error", glGetError() == GL_NO_ERROR);

	glTexImage2D(GL_DETAIL_TEXTURE_2D_SGIS, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
	filter4_quad();
	darker = pixel(24, 24);

	printf("=    detail: plain %#06lx, white detail %#06lx, black detail %#06lx\n",
	    plain, lighter, darker);
	check("a white detail texture adds", (lighter & 0xff) > (plain & 0xff) + 32);
	check("a black detail texture subtracts", (darker & 0xff) + 32 < (plain & 0xff));

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDeleteTextures(1, &tex);
	reset_view();
}

/*
 * SGIS_texture_select: one image holding several textures' worth of
 * components, with a parameter choosing which group a fragment sees.
 *
 * A QUAD_INTENSITY8 texture is loaded with a different value in each of the
 * four components, so selecting group 0..3 in turn must show each of them.
 * The four are distinct values, so a select that is ignored -- or that always
 * reads the same component -- cannot pass.
 */
static void
t_texture_select(void)
{
	static GLubyte quad[4] = { 32, 96, 160, 224 };
	GLuint tex;
	unsigned long got[4];
	int i, ok;

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	while (glGetError() != GL_NO_ERROR)
		;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_QUAD_INTENSITY8_SGIS, 1, 1, 0,
	    GL_RGBA, GL_UNSIGNED_BYTE, quad);
	check("a QUAD_INTENSITY8 texture is accepted", glGetError() == GL_NO_ERROR);

	for (i = 0; i < 4; i++) {
		glTexParameteri(GL_TEXTURE_2D, GL_QUAD_TEXTURE_SELECT_SGIS, i);
		filter4_quad();
		got[i] = pixel(24, 24);
	}
	printf("=    texture select groups:");
	for (i = 0; i < 4; i++)
		printf(" %#06lx", got[i]);
	printf("\n");
	ok = 1;
	for (i = 0; i < 4; i++) {
		int want = quad[i];
		int have = (int)(got[i] & 0xff);
		if (have < want - 4 || have > want + 4)
			ok = 0;
	}
	check("each group shows its own component", ok);

	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDeleteTextures(1, &tex);
	reset_view();
}

/*
 * ARB_multitexture with client arrays: each texture unit has its own
 * coordinate array, chosen by glClientActiveTextureARB, and a draw reads
 * every enabled one -- how Quake III lays a lightmap over a wall.
 *
 * Unit 0 replaces with a red|green texture at s = 0.75 (green); unit 1
 * modulates by a white|black texture at s = 0.25 (white). Green is the only
 * right answer: unit 1's coordinates on unit 0 give red, and unit 0's
 * coordinates on unit 1 give black. Each draw path is checked, with the
 * client unit left at 1 (as a program leaves it after setting the lightmap
 * coordinates) and then at 0.
 *
 * The ARB names are looked up when the test runs, as a program gets them
 * from glXGetProcAddress: this image's libGL, which the binary links
 * against, has only SGIS_multitexture.
 */
#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#endif

static void
t_multitexture_arrays(void)
{
	static GLubyte base[2 * 3] = { 255, 0, 0, 0, 255, 0 };
	static GLubyte light[2 * 3] = { 255, 255, 255, 0, 0, 0 };
	static GLfloat verts[4 * 2] = { 8, 8, 56, 8, 56, 56, 8, 56 };
	static GLfloat st0[4 * 2] = { 0.75f, 0.5f, 0.75f, 0.5f, 0.75f, 0.5f, 0.75f, 0.5f };
	static GLfloat st1[4 * 2] = { 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f };
	static GLushort idx[] = { 0, 1, 2, 3 };
	static const char *path[3] = { "glDrawElements", "glDrawArrays", "glArrayElement" };
	GLuint tex[2];
	unsigned long got;
	char what[96];
	int unit, p, i;
	void *self = dlopen(NULL, RTLD_LAZY);
	void (*glActiveTextureARB)(GLenum) = self ? (void (*)(GLenum))dlsym(self, "glActiveTextureARB") : NULL;
	void (*glClientActiveTextureARB)(GLenum) = self ? (void (*)(GLenum))dlsym(self, "glClientActiveTextureARB") : NULL;

	if (glActiveTextureARB == NULL || glClientActiveTextureARB == NULL) {
		printf("=    no ARB_multitexture entry points: multitexture arrays not checked\n");
		return;
	}
	reset_view();
	glGenTextures(2, tex);
	for (unit = 0; unit < 2; unit++) {
		glActiveTextureARB(GL_TEXTURE0_ARB + unit);
		glBindTexture(GL_TEXTURE_2D, tex[unit]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE,
		    unit == 0 ? base : light);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, unit == 0 ? GL_REPLACE : GL_MODULATE);
		glEnable(GL_TEXTURE_2D);
	}
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glEnableClientState(GL_VERTEX_ARRAY);
	glClientActiveTextureARB(GL_TEXTURE0_ARB);
	glTexCoordPointer(2, GL_FLOAT, 0, st0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTextureARB(GL_TEXTURE1_ARB);
	glTexCoordPointer(2, GL_FLOAT, 0, st1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	check("two units' coordinate arrays accepted", glGetError() == GL_NO_ERROR);

	for (unit = 1; unit >= 0; unit--) {
		glClientActiveTextureARB(GL_TEXTURE0_ARB + unit);
		for (p = 0; p < 3; p++) {
			glClear(GL_COLOR_BUFFER_BIT);
			if (p == 0)
				glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, idx);
			else if (p == 1)
				glDrawArrays(GL_QUADS, 0, 4);
			else {
				glBegin(GL_QUADS);
				for (i = 0; i < 4; i++)
					glArrayElement(i);
				glEnd();
			}
			got = pixel(32, 32);
			sprintf(what, "%s reads each unit's coordinates (client unit %d): %#08lx",
			    path[p], unit, got);
			check(what, got == 0x00ff00);
		}
	}

	for (unit = 1; unit >= 0; unit--) {
		glClientActiveTextureARB(GL_TEXTURE0_ARB + unit);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glActiveTextureARB(GL_TEXTURE0_ARB + unit);
		glDisable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDeleteTextures(2, tex);
	reset_view();
}

/*
 * SGIS_texture4D: a fourth texture axis. Two slabs of a 1x1x1 image hold
 * black and white, so moving the q coordinate from one end to the other must
 * sweep the fragment from black to white -- and the midpoint must be grey,
 * which is the part that shows the fourth axis is *filtered* and not merely
 * indexed.
 */
static void
t_texture4d(void)
{
	static GLubyte slabs[2 * 4] = {
		0, 0, 0, 255,           /* q = 0 */
		255, 255, 255, 255      /* q = 1 */
	};
	unsigned long lo, mid, hi;
	GLuint tex;

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	while (glGetError() != GL_NO_ERROR)
		;
	glTexImage4DSGIS(GL_TEXTURE_4D_SGIS, 0, GL_RGBA, 1, 1, 1, 2, 0,
	    GL_RGBA, GL_UNSIGNED_BYTE, slabs);
	check("glTexImage4DSGIS is accepted", glGetError() == GL_NO_ERROR);
	glEnable(GL_TEXTURE_4D_SGIS);
	check("glIsEnabled of TEXTURE_4D", glIsEnabled(GL_TEXTURE_4D_SGIS) == GL_TRUE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord4f(0.5f, 0.5f, 0.5f, 0.0f); glVertex2i(0, 0);
	glTexCoord4f(0.5f, 0.5f, 0.5f, 0.0f); glVertex2i(48, 0);
	glTexCoord4f(0.5f, 0.5f, 0.5f, 1.0f); glVertex2i(48, 48);
	glTexCoord4f(0.5f, 0.5f, 0.5f, 1.0f); glVertex2i(0, 48);
	glEnd();
	lo = pixel(24, 2);
	mid = pixel(24, 24);
	hi = pixel(24, 45);
	glDisable(GL_TEXTURE_4D_SGIS);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	printf("=    texture4D along q: %#06lx %#06lx %#06lx\n", lo, mid, hi);
	check("the fourth coordinate sweeps between the slabs",
	    (lo & 0xff) < 64 && (hi & 0xff) > 192);
	check("and is filtered between them",
	    (mid & 0xff) > 80 && (mid & 0xff) < 176);
	glDeleteTextures(1, &tex);
	reset_view();
}

/*
 * SGIX_sprite: geometry turned to face the eye before the modelview is
 * applied, for trees and puffs of smoke that are really one flat quad.
 *
 * A quad lying in the xy plane is drawn at an angle where, untransformed, it
 * is edge-on and covers almost nothing. Turned to face the eye it covers the
 * view. Counting lit pixels is the check: it does not depend on the exact
 * rotation, only on the quad having been turned towards the viewer.
 */
static void
t_sprite(void)
{
	static GLfloat axis[3] = { 0.0f, 1.0f, 0.0f };
	int plain, turned, x, y;

	reset_view();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-2, 2, -2, 2, -10, 10);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	/*
	 * The object has to stand away from the eye, or its position in object
	 * coordinates *is* the eye and there is no direction to turn towards --
	 * the first version of this check put the quad at the origin and the
	 * rotation was degenerate for both passes.
	 *
	 * Then a quarter turn about y, which leaves the quad in the xy plane
	 * edge-on and covering nothing.
	 */
	glTranslatef(0.0f, 0.0f, -4.0f);
	glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
	glColor3f(1, 1, 1);

	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_QUADS);
	glVertex3f(-1, -1, 0); glVertex3f(1, -1, 0);
	glVertex3f(1, 1, 0); glVertex3f(-1, 1, 0);
	glEnd();
	plain = 0;
	for (y = 8; y < 56; y += 4)
		for (x = 8; x < 56; x += 4)
			if (pixel(x, y) != 0)
				plain++;

	while (glGetError() != GL_NO_ERROR)
		;
	glSpriteParameteriSGIX(GL_SPRITE_MODE_SGIX, GL_SPRITE_AXIAL_SGIX);
	glSpriteParameterfvSGIX(GL_SPRITE_AXIS_SGIX, axis);
	glEnable(GL_SPRITE_SGIX);
	check("glIsEnabled of the sprite", glIsEnabled(GL_SPRITE_SGIX) == GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_QUADS);
	glVertex3f(-1, -1, 0); glVertex3f(1, -1, 0);
	glVertex3f(1, 1, 0); glVertex3f(-1, 1, 0);
	glEnd();
	turned = 0;
	for (y = 8; y < 56; y += 4)
		for (x = 8; x < 56; x += 4)
			if (pixel(x, y) != 0)
				turned++;
	glDisable(GL_SPRITE_SGIX);
	check("the sprite raised no error", glGetError() == GL_NO_ERROR);

	printf("=    sprite: edge-on %d lit samples, turned %d\n", plain, turned);
	check("the sprite turns to face the eye", turned > plain + 40);
	reset_view();
}

/*
 * SGIX_pixel_texture: a transferred pixel's colour becomes the fragment's
 * texture coordinates, so an image can index a table held as a texture --
 * false colour, or a lookup of arbitrary width.
 *
 * The texture is a horizontal ramp from black to red. The image drawn is a
 * row of increasing red values, which as S coordinates must pick out
 * increasing points along that ramp. Without the extension the same
 * glDrawPixels would simply show the image, so the two are told apart by the
 * green channel, which the ramp has and the image has not.
 */
static void
t_pixel_texture(void)
{
	GLubyte ramp[16 * 4];
	GLubyte img[8 * 4];
	GLuint tex;
	unsigned long a, b;
	int i;

	for (i = 0; i < 16; i++) {
		ramp[i * 4] = (GLubyte)(i * 17);        /* red climbs */
		ramp[i * 4 + 1] = 200;                  /* green marks the ramp */
		ramp[i * 4 + 2] = 0;
		ramp[i * 4 + 3] = 255;
	}
	for (i = 0; i < 8; i++) {
		img[i * 4] = (GLubyte)(i * 32);         /* S climbs across the row */
		img[i * 4 + 1] = 0;
		img[i * 4 + 2] = 0;
		img[i * 4 + 3] = 255;
	}

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp);
	glEnable(GL_TEXTURE_2D);

	while (glGetError() != GL_NO_ERROR)
		;
	/*
	 * RGBA mode: the fragment's colour comes from the raster position --
	 * white here -- so the default MODULATE environment lets the texel
	 * through unchanged and the green the ramp carries is visible. With
	 * mode NONE the colour would be the pixel itself, whose green is zero,
	 * and MODULATE would correctly multiply the ramp's green away.
	 */
	glPixelTexGenSGIX(GL_RGBA);
	glEnable(GL_PIXEL_TEX_GEN_SGIX);
	check("glIsEnabled of pixel texture", glIsEnabled(GL_PIXEL_TEX_GEN_SGIX) == GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glRasterPos2i(4, 4);
	glDrawPixels(8, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
	a = pixel(5, 4);
	b = pixel(10, 4);
	glDisable(GL_PIXEL_TEX_GEN_SGIX);
	check("pixel texture raised no error", glGetError() == GL_NO_ERROR);

	printf("=    pixel texture: low S %#06lx, high S %#06lx\n", a, b);
	check("the pixel indexed the texture", ((a >> 8) & 0xff) > 100 && ((b >> 8) & 0xff) > 100);
	check("and a larger pixel value reached further along it",
	    ((b >> 16) & 0xff) > ((a >> 16) & 0xff) + 32);

	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	reset_view();
}

/* A small quad, textured from one point of the virtual clipmap, magnified so
 * the whole quad samples close to that point. */
static void
clipmap_quad(GLfloat s, GLfloat t)
{
	GLfloat e = 1.0f / 256.0f;

	glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(s - e, t - e); glVertex2i(0, 0);
	glTexCoord2f(s + e, t - e); glVertex2i(48, 0);
	glTexCoord2f(s + e, t + e); glVertex2i(48, 48);
	glTexCoord2f(s - e, t + e); glVertex2i(0, 48);
	glEnd();
}

/*
 * SGIX_clipmap: a virtual texture far larger than memory, of which a window
 * around a moving centre is resident.
 *
 * The resident levels are all the same size, which an OpenGL mipmap chain
 * cannot hold -- every level of one is half the last -- so they are kept as
 * bands of a single image and the level is chosen, wrapped and filtered in the
 * shader instead.
 *
 * Two properties are checked, and they are the two that make it a clipmap
 * rather than a mipmap:
 *
 *   the window wraps -- sliding the offset by half the window moves what a
 *   fixed coordinate sees, toroidally, which is how a program scrolls new
 *   terrain in at one edge;
 *
 *   the window is finite -- a coordinate far from the centre is not resident
 *   at the finest level, so the level of detail rises to one that has it,
 *   and the colour that comes back is the coarser level's, not the finer.
 */
static void
t_clipmap(void)
{
	GLubyte level0[8 * 8 * 4], level1[8 * 8 * 4], level2[8 * 8 * 4];
	GLfloat depth[3], centre[2], offset[2], back[4];
	GLuint tex;
	unsigned long home, slid, far_away;
	int i;

	/* Level 0 a horizontal ramp, the coarser levels flat and distinct. */
	for (i = 0; i < 64; i++) {
		level0[i * 4] = (GLubyte)((i % 8) * 32);
		level0[i * 4 + 1] = 0;
		level0[i * 4 + 2] = 0;
		level0[i * 4 + 3] = 255;
		level1[i * 4] = 0;   level1[i * 4 + 1] = 255; level1[i * 4 + 2] = 0;   level1[i * 4 + 3] = 255;
		level2[i * 4] = 0;   level2[i * 4 + 1] = 0;   level2[i * 4 + 2] = 255; level2[i * 4 + 3] = 255;
	}

	reset_view();
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	while (glGetError() != GL_NO_ERROR)
		;
	/* (D, N+1, V+1): finest level 0, three resident levels, a virtual
	 * pyramid six deep -- so the finest virtual texture is 32x32 and the
	 * 8-texel window really is a window. */
	depth[0] = 0.0f; depth[1] = 3.0f; depth[2] = 6.0f;
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_VIRTUAL_DEPTH_SGIX, depth);
	centre[0] = 4.0f; centre[1] = 4.0f;
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_CENTER_SGIX, centre);
	offset[0] = 0.0f; offset[1] = 0.0f;
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_OFFSET_SGIX, offset);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_CLIPMAP_LINEAR_SGIX);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, level0);
	glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, level1);
	glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, level2);
	check("a clipmap is accepted", glGetError() == GL_NO_ERROR);

	glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_CENTER_SGIX, back);
	check("the clipmap centre reads back", back[0] == 4.0f && back[1] == 4.0f);

	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);

	/*
	 * A small quad at the centre of texel 4, which is 4.5 texels along --
	 * a texel *centre*, so the filter returns that texel exactly and the
	 * answer is arithmetic rather than a blend. The first version of this
	 * sampled a texel boundary, where the average either side of the wrap
	 * came to the same number both times and a shift of four texels showed
	 * up as a difference of one part in 255.
	 */
	clipmap_quad(4.5f / 32.0f, 4.5f / 32.0f);
	home = pixel(24, 24);

	/* Slide the window by half its width: the same coordinate now lands on
	 * texel (4 + 4) mod 8 = 0, the dark end of the ramp. */
	offset[0] = 4.0f; offset[1] = 0.0f;
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_OFFSET_SGIX, offset);
	clipmap_quad(4.5f / 32.0f, 4.5f / 32.0f);
	slid = pixel(24, 24);

	/* Far from the centre, the finest level has nothing: the level of
	 * detail rises and a coarser, differently coloured level answers. */
	offset[0] = 0.0f;
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_CLIPMAP_OFFSET_SGIX, offset);
	clipmap_quad(28.5f / 32.0f, 4.5f / 32.0f);
	far_away = pixel(24, 24);

	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	printf("=    clipmap: at the centre %#06lx, window slid %#06lx, far out %#06lx\n",
	    home, slid, far_away);
	check("the centre texel reads its own ramp value", ((home >> 16) & 0xff) > 112);
	check("sliding the window wraps it round to the far edge",
	    ((slid >> 16) & 0xff) < 32);
	check("outside the window the level of detail rises",
	    ((far_away >> 8) & 0xff) > 100 || (far_away & 0xff) > 100);
	glDeleteTextures(1, &tex);
	reset_view();
}

static void
t_select_feedback(void)
{
	GLuint sel[64];
	GLfloat fb[64];
	GLint hits, n;

	reset_view();
	glSelectBuffer(64, sel);
	glRenderMode(GL_SELECT);
	glInitNames();
	glPushName(7);
	glRecti(0, 0, 10, 10);
	glLoadName(9);
	glRecti(20, 20, 30, 30);
	hits = glRenderMode(GL_RENDER);
	check("selection: two hits", hits == 2);
	check("selection: records carry names 7 and 9", sel[0] == 1 && sel[3] == 7 && sel[4] == 1 && sel[7] == 9);

	glFeedbackBuffer(64, GL_2D, fb);
	glRenderMode(GL_FEEDBACK);
	glBegin(GL_POINTS);
	glVertex2i(5, 6);
	glEnd();
	n = glRenderMode(GL_RENDER);
	check("feedback: one point token and its x, y", n == 3 && fb[0] == GL_POINT_TOKEN && near(fb[1], 5.5) == near(fb[1], 5.5) && fb[1] >= 4.5 && fb[1] <= 6.5);
}

static void
t_maps(void)
{
	GLfloat pts[] = { 0, 0, 0, 1, 2, 3, 4, 5, 6 };
	GLfloat coeff[16];
	GLint order;
	GLfloat pm[4] = { 0.1f, 0.5f, 0.75f, 1.0f };
	GLfloat pm_back[16];

	glMap1f(GL_MAP1_VERTEX_3, 0, 1, 3, 3, pts);
	glGetMapiv(GL_MAP1_VERTEX_3, GL_ORDER, &order);
	glGetMapfv(GL_MAP1_VERTEX_3, GL_COEFF, coeff);
	check("glMap1f/glGetMapiv ORDER", order == 3);
	check("glGetMapfv COEFF (order x components, asked of the host)", near(coeff[3], 1) && near(coeff[8], 6));

	glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 4, pm);
	glGetPixelMapfv(GL_PIXEL_MAP_R_TO_R, pm_back);
	check("glPixelMapfv/glGetPixelMapfv (size asked of the host)", near(pm_back[1], 0.5) && near(pm_back[3], 1));
	pm[0] = 0; pm[1] = 1;
	glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 2, pm);
}

/*
 * GLX as a high-end SGI has it: configs, a pbuffer drawn into with no window,
 * a read drawable of its own, the swap interval, and the frame counter.
 */
static void
t_glx(Display *dpy, Window win, GLXContext ctx)
{
	const char *e = glXQueryExtensionsString(dpy, DefaultScreen(dpy));
	GLXFBConfigSGIX *cfgs;
	GLXPbufferSGIX pbuf;
	XVisualInfo *vi;
	unsigned int count, later, value;
	int n = 0, attrib = 0;

	if (e == NULL)
		e = "";
	printf("=    GLX extensions: %.200s\n", e);
	check("GLX extensions include SGIX_pbuffer", strstr(e, "GLX_SGIX_pbuffer") != NULL);
	check("GLX extensions include SGI_make_current_read", strstr(e, "GLX_SGI_make_current_read") != NULL);
	check("GLX extensions leave out what we cannot do", strstr(e, "hyperpipe") == NULL);

	cfgs = glXChooseFBConfigSGIX(dpy, DefaultScreen(dpy), &attrib, &n);
	check("glXChooseFBConfigSGIX", cfgs != NULL && n > 0);
	if (cfgs == NULL || n == 0)
		return;
	check("fbconfig draws to windows, pixmaps and pbuffers",
	    glXGetFBConfigAttribSGIX(dpy, cfgs[0], GLX_DRAWABLE_TYPE_SGIX, &attrib) == 0
	    && (attrib & GLX_PBUFFER_BIT_SGIX) != 0);
	vi = glXGetVisualFromFBConfigSGIX(dpy, cfgs[0]);
	check("glXGetVisualFromFBConfigSGIX", vi != NULL && vi->depth == 24);

	/* A pbuffer keeps its own pixels, and so does the window. */
	pbuf = glXCreateGLXPbufferSGIX(dpy, cfgs[0], 32, 32, NULL);
	check("glXCreateGLXPbufferSGIX", pbuf != None);
	if (pbuf != None) {
		check("glXQueryGLXPbufferSGIX width",
		    glXQueryGLXPbufferSGIX(dpy, pbuf, GLX_WIDTH_SGIX, &value) == 0 && value == 32);
		reset_view();
		glColor3f(1, 0, 0);
		glRecti(0, 0, W, H);            /* the window: red */
		check("the window was drawn", pixel(4, 4) == 0xff0000);

		check("glXMakeCurrent to a pbuffer", glXMakeCurrent(dpy, pbuf, ctx));
		glViewport(0, 0, 32, 32);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, 32, 0, 32, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glClearColor(0, 0, 1, 0);
		glClear(GL_COLOR_BUFFER_BIT);   /* the pbuffer: blue */
		check("the pbuffer was drawn", pixel(4, 4) == 0x0000ff);

		check("glXMakeCurrent back to the window", glXMakeCurrent(dpy, win, ctx));
		glViewport(0, 0, W, H);
		check("the window kept its pixels", pixel(4, 4) == 0xff0000);

		/* Read from the pbuffer while drawing into the window. */
		check("glXMakeCurrentReadSGI", glXMakeCurrentReadSGI(dpy, win, pbuf, ctx));
		check("reading takes the pbuffer's pixels", pixel(4, 4) == 0x0000ff);
		check("glXGetCurrentReadDrawableSGI", glXGetCurrentReadDrawableSGI() == pbuf);
		glXMakeCurrent(dpy, win, ctx);
		glXDestroyGLXPbufferSGIX(dpy, pbuf);
	}
	XFree(cfgs);

	/* A GLX pixmap: drawn into with GL, then read back as an X pixmap. */
	if (vi != NULL) {
		Pixmap pm = XCreatePixmap(dpy, RootWindow(dpy, vi->screen), 16, 16, 24);
		GLXPixmap gpm = glXCreateGLXPixmap(dpy, vi, pm);
		XImage *img;

		check("glXCreateGLXPixmap", gpm != None);
		if (gpm != None) {
			check("glXMakeCurrent to a GLX pixmap", glXMakeCurrent(dpy, gpm, ctx));
			glViewport(0, 0, 16, 16);
			glClearColor(0, 1, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glFinish();     /* the pixels reach the X pixmap here */
			glXMakeCurrent(dpy, win, ctx);
			glViewport(0, 0, W, H);
			img = XGetImage(dpy, pm, 0, 0, 16, 16, AllPlanes, ZPixmap);
			check("the X pixmap holds what GL drew",
			    img != NULL && (XGetPixel(img, 4, 4) & 0xffffff) == 0x00ff00);
			if (img != NULL)
				XDestroyImage(img);
			glXDestroyGLXPixmap(dpy, gpm);
		}
		XFreePixmap(dpy, pm);
	}

	/*
	 * glXUseXFont: a letter of the server's own font, as a display list.
	 * What the glyph looks like is the font's business, so the check is
	 * that something was drawn inside its box and not everything.
	 */
	{
		Font font = XLoadFont(dpy, "fixed");
		GLuint base = glGenLists(4);
		int lit = 0, x, y;

		glXUseXFont(font, 'A', 4, base);
		check("glXUseXFont made the lists", glIsList(base) && glIsList(base + 3));
		reset_view();
		glColor3f(1, 1, 1);
		glRasterPos2i(8, 8);
		glCallList(base);       /* 'A' */
		glFinish();
		for (y = 0; y < 24; y++)
			for (x = 0; x < 24; x++)
				if (pixel(x, y) != 0)
					lit++;
		printf("=    the glyph lit %d pixels in a 24x24 corner\n", lit);
		check("glXUseXFont drew a letter", lit > 0 && lit < 24 * 24);
		glDeleteLists(base, 4);
		XUnloadFont(dpy, font);
	}

	/*
	 * A multisampled pbuffer: a slanted edge should leave pixels part way
	 * between the two colours, which is what multisampling is for.
	 */
	{
		int ms_attribs[] = { GLX_SAMPLES_SGIS, 4, None };
		GLXFBConfigSGIX *ms;
		int count = 0, between = 0, x, y, got = 0;
		GLXPbufferSGIX mspb;

		ms = glXChooseFBConfigSGIX(dpy, DefaultScreen(dpy), ms_attribs, &count);
		if (ms != NULL && count > 0
		    && glXGetFBConfigAttribSGIX(dpy, ms[0], GLX_SAMPLES_SGIS, &got) == 0) {
			check("the fbconfig kept its sample count", got == 4);
			mspb = glXCreateGLXPbufferSGIX(dpy, ms[0], 32, 32, NULL);
			if (mspb != None && glXMakeCurrent(dpy, mspb, ctx)) {
				glViewport(0, 0, 32, 32);
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrtho(0, 32, 0, 32, -1, 1);
				glMatrixMode(GL_MODELVIEW);
				glLoadIdentity();
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				glColor3f(1, 1, 1);
				glBegin(GL_TRIANGLES);
				glVertex2i(0, 0);
				glVertex2i(32, 0);
				glVertex2i(0, 32);
				glEnd();
				glFinish();
				for (y = 0; y < 32; y++)
					for (x = 0; x < 32; x++) {
						unsigned long p = pixel(x, y) & 0xff;
						if (p != 0 && p != 0xff)
							between++;
					}
				printf("=    the slanted edge left %d part-way pixels\n", between);
				check("a multisampled pbuffer antialiases", between > 0);
				glXMakeCurrent(dpy, win, ctx);
				glViewport(0, 0, W, H);
				glXDestroyGLXPbufferSGIX(dpy, mspb);
			}
			XFree(ms);
		}
	}

	check("glXSwapIntervalSGI", glXSwapIntervalSGI(1) == 0);
	check("glXGetVideoSyncSGI", glXGetVideoSyncSGI(&count) == 0);
	check("glXWaitVideoSyncSGI advances the counter",
	    glXWaitVideoSyncSGI(2, 0, &later) == 0 && later >= count);
	glXJoinSwapGroupSGIX(dpy, win, win);
	reset_view();
}

/*
 * An overlay. GLX_LEVEL 1 gives the X server's overlay visual, colour index
 * only; what a context for it draws are indices, and they are the overlay
 * window's pixels -- read back here through X, from the board's overlay
 * planes -- index 0 the transparent one. Logic ops work on an index's bits.
 */
static void
t_overlay(Display *dpy, Window win, GLXContext ctx)
{
	static int want[] = { GLX_LEVEL, 1, GLX_BUFFER_SIZE, 1, None };
	static int want_rgba[] = { GLX_LEVEL, 1, GLX_RGBA, None };
	static const struct { int x, y; } at[] = {
		{ 4, 4 }, { 12, 12 }, { 20, 20 }, { 32, 32 }, { 44, 44 }, { 52, 52 },
	};
	unsigned long px[3], expect[6];
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	XColor c;
	Window ov;
	GLXContext octx;
	XImage *im;
	GLint iv = -1, cur = -1;
	GLboolean mode = GL_FALSE;
	int v = -1, t = -1, i, good;
	char what[80];

	check("no RGBA overlay", glXChooseVisual(dpy, DefaultScreen(dpy), want_rgba) == NULL);
	vi = glXChooseVisual(dpy, DefaultScreen(dpy), want);
	check("glXChooseVisual GLX_LEVEL 1: an 8-bit colour index visual",
	    vi != NULL && vi->depth == 8 && vi->class == PseudoColor);
	if (vi == NULL)
		return;
	glXGetConfig(dpy, vi, GLX_LEVEL, &v);
	glXGetConfig(dpy, vi, GLX_RGBA, &t);
	check("glXGetConfig of the overlay: level 1, colour index", v == 1 && t == 0);
#ifdef GLX_EXT_visual_info
	glXGetConfig(dpy, vi, GLX_TRANSPARENT_TYPE_EXT, &t);
	glXGetConfig(dpy, vi, GLX_TRANSPARENT_INDEX_VALUE_EXT, &v);
	check("glXGetConfig of the overlay: index 0 is transparent", t == GLX_TRANSPARENT_INDEX_EXT && v == 0);
#endif
	/* The server keeps the transparent pixel, so an overlay colormap cannot
	 * be AllocAll: three cells are allocated, red, green and blue. */
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	good = XAllocColorCells(dpy, swa.colormap, False, NULL, 0, px, 3) != 0;
	check("three read-write cells in an overlay colormap", good);
	if (!good) {
		px[0] = 1;
		px[1] = 2;
		px[2] = 3;
	}
	c.flags = DoRed | DoGreen | DoBlue;
	for (i = 0; good && i < 3; i++) {
		c.pixel = px[i];
		c.red = i == 0 ? 0xffff : 0;
		c.green = i == 1 ? 0xffff : 0;
		c.blue = i == 2 ? 0xffff : 0;
		XStoreColor(dpy, swa.colormap, &c);
	}
	/* What each probe should read: outside everything, the first square,
	 * it under the XOR square, the XOR square alone, the second square
	 * under it, the second square. */
	expect[0] = 0;
	expect[1] = px[0];
	expect[2] = px[0] ^ px[2];
	expect[3] = px[2];
	expect[4] = px[1] ^ px[2];
	expect[5] = px[1];
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	ov = XCreateWindow(dpy, win, 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual,
	    CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XMapWindow(dpy, ov);
	XSync(dpy, False);
	octx = glXCreateContext(dpy, vi, NULL, True);
	good = octx != NULL && glXMakeCurrent(dpy, ov, octx);
	check("an overlay context, current on the overlay window", good);
	if (good) {
		glGetBooleanv(GL_INDEX_MODE, &mode);
		glGetIntegerv(GL_INDEX_BITS, &iv);
		check("the overlay context: INDEX_MODE, 8 INDEX_BITS", mode && iv == 8);
		glViewport(0, 0, W, H);
		reset_view();
		glClearIndex(0);
		glClear(GL_COLOR_BUFFER_BIT);
		glIndexi((GLint)px[0]);
		glRecti(8, 8, 24, 24);
		glIndexi((GLint)px[1]);
		glRecti(40, 40, 56, 56);
		glGetIntegerv(GL_CURRENT_INDEX, &cur);
		check("glGetIntegerv CURRENT_INDEX", cur == (GLint)px[1]);
		glEnable(GL_LOGIC_OP);
		glLogicOp(GL_XOR);
		glIndexi((GLint)px[2]);
		glRecti(16, 16, 48, 48);
		glDisable(GL_LOGIC_OP);
		glFlush();
		XSync(dpy, False);
		im = XGetImage(dpy, ov, 0, 0, W, H, AllPlanes, ZPixmap);
		check("XGetImage of the overlay window", im != NULL);
		for (i = 0; im != NULL && i < 6; i++) {
			/* GL counts rows up from the bottom, X down from the top. */
			unsigned long p = XGetPixel(im, at[i].x, H - 1 - at[i].y);

			sprintf(what, "overlay pixel (%d,%d) is index %lu (saw %lu)", at[i].x, at[i].y, expect[i], p);
			check(what, p == expect[i]);
		}
		if (im != NULL)
			XDestroyImage(im);
	}
	glXMakeCurrent(dpy, win, ctx);
	glGetBooleanv(GL_INDEX_MODE, &mode);
	check("back in the RGBA context: not INDEX_MODE", !mode);
	if (octx != NULL)
		glXDestroyContext(dpy, octx);
	XDestroyWindow(dpy, ov);
	XFreeColormap(dpy, swa.colormap);
	XFree(vi);
	reset_view();
}

/*
 * A colour-index window in the normal planes, as 6.5.22's gr_osview asks for
 * one: no GLX_RGBA, GLX_BUFFER_SIZE 12, single buffered. It gets IMPACT's
 * 12-bit PseudoColor visual, and what a context for it draws -- all 12 bits
 * of each index -- are the window's pixels at the next glFlush, with no
 * swap. On a server without that visual it is skipped.
 */
static void
t_cmode(Display *dpy, Window win, GLXContext ctx)
{
	static int want[] = { GLX_USE_GL, GLX_BUFFER_SIZE, 12, None };
	XVisualInfo tmpl, *vi;
	XSetWindowAttributes swa;
	Window cw;
	GLXContext cctx;
	XImage *im;
	GLint iv = -1;
	GLboolean mode = GL_FALSE;
	int rgba = -1, dbl = -1, size = -1, n, good;
	char what[80];

	tmpl.screen = DefaultScreen(dpy);
	tmpl.depth = 12;
	tmpl.class = PseudoColor;
	if ((vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tmpl, &n)) == NULL) {
		printf("=    no 12-bit colour-index visual here: skipped\n");
		return;
	}
	XFree(vi);
	vi = glXChooseVisual(dpy, DefaultScreen(dpy), want);
	check("glXChooseVisual GLX_BUFFER_SIZE 12: a 12-bit colour index visual",
	    vi != NULL && vi->depth == 12 && vi->class == PseudoColor);
	if (vi == NULL || vi->class != PseudoColor) {
		if (vi != NULL)
			XFree(vi);
		return;
	}
	glXGetConfig(dpy, vi, GLX_RGBA, &rgba);
	glXGetConfig(dpy, vi, GLX_DOUBLEBUFFER, &dbl);
	glXGetConfig(dpy, vi, GLX_BUFFER_SIZE, &size);
	check("glXGetConfig of it: colour index, single buffered, 12 bits",
	    rgba == 0 && dbl == 0 && size == 12);
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocAll);
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	cw = XCreateWindow(dpy, win, 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual,
	    CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XMapWindow(dpy, cw);
	XSync(dpy, False);
	cctx = glXCreateContext(dpy, vi, NULL, True);
	good = cctx != NULL && glXMakeCurrent(dpy, cw, cctx);
	check("a colour-index context, current on its window", good);
	if (good) {
		glGetBooleanv(GL_INDEX_MODE, &mode);
		glGetIntegerv(GL_INDEX_BITS, &iv);
		check("the colour-index context: INDEX_MODE, 12 INDEX_BITS", mode && iv == 12);
		glViewport(0, 0, W, H);
		reset_view();
		glClearIndex(7);
		glClear(GL_COLOR_BUFFER_BIT);
		glIndexi(1000);
		glRecti(8, 8, 24, 24);
		/* No swap: a single-buffered window shows at the flush. */
		glFlush();
		XSync(dpy, False);
		im = XGetImage(dpy, cw, 0, 0, W, H, AllPlanes, ZPixmap);
		check("XGetImage of the colour-index window", im != NULL);
		if (im != NULL) {
			unsigned long in = XGetPixel(im, 16, H - 1 - 16), out = XGetPixel(im, 40, H - 1 - 40);

			sprintf(what, "its pixels are the indices: 1000 drawn, 7 cleared (%lu, %lu)", in, out);
			check(what, in == 1000 && out == 7);
			XDestroyImage(im);
		}
	}
	glXMakeCurrent(dpy, win, ctx);
	if (cctx != NULL)
		glXDestroyContext(dpy, cctx);
	XDestroyWindow(dpy, cw);
	XFreeColormap(dpy, swa.colormap);
	XFree(vi);
	reset_view();
}

/*
 * A single-buffered RGBA window, as 6.5.22's ical, mag, colorbars and grid
 * ask for one: GLX_RGBA with no GLX_DOUBLEBUFFER. Its visual says it is
 * single buffered, and what a context for it draws is in the window at the
 * next glFlush, with no swap. That last part is read back through X, so it
 * is checked only where frames go through the X server: on SGI's hardware,
 * or with IRIS_HOSTGL_XPUTIMAGE=1 (composited frames never reach X).
 *
 * Not checked: glGet of GL_DRAW_BUFFER. It should say GL_FRONT, but the host
 * answers with the framebuffer object's attachment, whatever was named.
 */
static void
t_single(Display *dpy, Window win, GLXContext ctx)
{
	static int want[] = { GLX_RGBA, GLX_RED_SIZE, 1, None };
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window sw;
	GLXContext sctx;
	XImage *im;
	int dbl = -1, good;
	char what[80];

	vi = glXChooseVisual(dpy, DefaultScreen(dpy), want);
	check("glXChooseVisual GLX_RGBA, single buffered", vi != NULL);
	if (vi == NULL)
		return;
	glXGetConfig(dpy, vi, GLX_DOUBLEBUFFER, &dbl);
	check("glXGetConfig of it: single buffered", dbl == 0);
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	sw = XCreateWindow(dpy, win, 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual,
	    CWColormap | CWBorderPixel, &swa);
	XMapWindow(dpy, sw);
	XSync(dpy, False);
	sctx = glXCreateContext(dpy, vi, NULL, True);
	good = sctx != NULL && glXMakeCurrent(dpy, sw, sctx);
	check("a single-buffered context, current on its window", good);
	if (good) {
		glViewport(0, 0, W, H);
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFlush();
		XSync(dpy, False);
		if (getenv("IRIS_HOSTGL_XPUTIMAGE") == NULL) {
			printf("=    single-buffered window through X: needs IRIS_HOSTGL_XPUTIMAGE=1, skipped\n");
		} else if ((im = XGetImage(dpy, sw, 0, 0, W, H, AllPlanes, ZPixmap)) != NULL) {
			unsigned long p = XGetPixel(im, W / 2, H / 2);
			Visual *v = vi->visual;

			sprintf(what, "glFlush puts it in the window, no swap (pixel %06lx)", p);
			check(what, (p & v->green_mask) == v->green_mask &&
			    (p & (v->red_mask | v->blue_mask)) == 0);
			XDestroyImage(im);
		} else {
			check("XGetImage of the single-buffered window", 0);
		}
	}
	glXMakeCurrent(dpy, win, ctx);
	if (sctx != NULL)
		glXDestroyContext(dpy, sctx);
	XDestroyWindow(dpy, sw);
	XFreeColormap(dpy, swa.colormap);
	XFree(vi);
	reset_view();
}

/*
 * The gl* entry points of SGI's libGLcore.so that the rest of the shim did
 * not have (glshim_sgi.c). Looked up by name, as a program would find them.
 */
#define SGI_FN(type, name, args) type (*name) args = (type (*) args)(self ? dlsym(self, #name) : NULL)

/* SGIS_multitexture under its later names, glMultiTexCoord*SGIS: the same
 * two textures as t_multitexture_arrays, unit 1's coordinate through them. */
static void
t_sgis_multitexcoord(void)
{
	static GLubyte base[2 * 3] = { 255, 0, 0, 0, 255, 0 };
	static GLubyte light[2 * 3] = { 255, 255, 255, 0, 0, 0 };
	static GLfloat verts[4 * 2] = { 8, 8, 56, 8, 56, 56, 8, 56 };
	static GLfloat st1[4 * 2] = { 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f };
	GLuint tex[2];
	GLint client;
	int unit, i;
	unsigned long got;
	void *self = dlopen(NULL, RTLD_LAZY);
	SGI_FN(void, glActiveTextureARB, (GLenum));
	SGI_FN(void, glClientActiveTextureARB, (GLenum));
	SGI_FN(void, glMultiTexCoord2fSGIS, (GLenum, GLfloat, GLfloat));
	SGI_FN(void, glMultiTexCoordPointerSGIS, (GLenum, GLint, GLenum, GLsizei, const GLvoid *));

	if (glMultiTexCoord2fSGIS == NULL || glMultiTexCoordPointerSGIS == NULL || glActiveTextureARB == NULL) {
		check("glMultiTexCoord2fSGIS and glMultiTexCoordPointerSGIS are there", 0);
		return;
	}
	reset_view();
	glGenTextures(2, tex);
	for (unit = 0; unit < 2; unit++) {
		glActiveTextureARB(GL_TEXTURE0_ARB + unit);
		glBindTexture(GL_TEXTURE_2D, tex[unit]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE,
		    unit == 0 ? base : light);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, unit == 0 ? GL_REPLACE : GL_MODULATE);
		glEnable(GL_TEXTURE_2D);
	}

	/* green from unit 0, times white (0.25) or black (0.75) from unit 1 */
	for (i = 0; i < 2; i++) {
		GLfloat s1 = i == 0 ? 0.25f : 0.75f;
		glClear(GL_COLOR_BUFFER_BIT);
		glBegin(GL_QUADS);
		glMultiTexCoord2fSGIS(0x835E, 0.75f, 0.5f);	/* TEXTURE0_SGIS */
		glMultiTexCoord2fSGIS(0x835F, s1, 0.5f);	/* TEXTURE1_SGIS */
		glVertex2f(8, 8); glVertex2f(56, 8); glVertex2f(56, 56); glVertex2f(8, 56);
		glEnd();
		got = pixel(32, 32);
		check(i == 0 ? "glMultiTexCoord2fSGIS sets each unit (white on unit 1)" :
		    "glMultiTexCoord2fSGIS sets each unit (black on unit 1)",
		    got == (i == 0 ? 0x00ff00UL : 0UL));
	}

	/* unit 1's array by glMultiTexCoordPointerSGIS, unit 0 a constant */
	glClientActiveTextureARB(GL_TEXTURE0_ARB);
	glMultiTexCoordPointerSGIS(0x835F, 2, GL_FLOAT, 0, st1);
	glGetIntegerv(0x84E1, &client);			/* CLIENT_ACTIVE_TEXTURE_ARB */
	check("glMultiTexCoordPointerSGIS leaves the client unit as it was", client == GL_TEXTURE0_ARB);
	glClientActiveTextureARB(GL_TEXTURE1_ARB);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTextureARB(GL_TEXTURE0_ARB);
	glMultiTexCoord2fSGIS(0x835E, 0.75f, 0.5f);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glEnableClientState(GL_VERTEX_ARRAY);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_QUADS, 0, 4);
	check("glMultiTexCoordPointerSGIS gives unit 1 its array", pixel(32, 32) == 0x00ff00);
	glDisableClientState(GL_VERTEX_ARRAY);
	glClientActiveTextureARB(GL_TEXTURE1_ARB);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTextureARB(GL_TEXTURE0_ARB);

	for (unit = 1; unit >= 0; unit--) {
		glActiveTextureARB(GL_TEXTURE0_ARB + unit);
		glDisable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
	glDeleteTextures(2, tex);
	check("no GL error from SGIS_multitexture's calls", glGetError() == GL_NO_ERROR);
	reset_view();
}

/* The rest: what they keep reads back, and none of them is an error. */
static void
t_libglcore_extras(void)
{
	void *self = dlopen(NULL, RTLD_LAZY);
	SGI_FN(void, glPointParameterfEXT, (GLenum, GLfloat));
	SGI_FN(GLuint, glGenAsyncMarkersSGIX, (GLsizei));
	SGI_FN(void, glAsyncMarkerSGIX, (GLuint));
	SGI_FN(GLint, glFinishAsyncSGIX, (GLuint *));
	SGI_FN(GLint, glPollAsyncSGIX, (GLuint *));
	SGI_FN(GLboolean, glIsAsyncMarkerSGIX, (GLuint));
	SGI_FN(void, glDeleteAsyncMarkersSGIX, (GLuint, GLsizei));
	SGI_FN(void, glFragmentLightfvSGIX, (GLenum, GLenum, const GLfloat *));
	SGI_FN(void, glGetFragmentLightfvSGIX, (GLenum, GLenum, GLfloat *));
	SGI_FN(void, glFragmentMaterialiSGIX, (GLenum, GLenum, GLint));
	SGI_FN(void, glGetFragmentMaterialivSGIX, (GLenum, GLenum, GLint *));
	SGI_FN(void, glPixelTexGenParameteriSGIS, (GLenum, GLint));
	SGI_FN(void, glGetPixelTexGenParameterivSGIS, (GLenum, GLint *));
	SGI_FN(void, glTangent3fSGIX, (GLfloat, GLfloat, GLfloat));
	SGI_FN(void, glBinormal3fSGIX, (GLfloat, GLfloat, GLfloat));
	SGI_FN(void, glIglooInterfaceSGIX, (GLenum, const GLvoid *));
	GLfloat f[4], d[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	GLint iv;
	GLuint m, done;

	while (glGetError() != GL_NO_ERROR)
		;
	if (glPointParameterfEXT == NULL) {
		check("glPointParameterfEXT is there", 0);
	} else {
		glPointParameterfEXT(0x8126, 2.0f);		/* POINT_SIZE_MIN_EXT */
		glGetFloatv(0x8126, f);
		check("glPointParameterfEXT reads back as POINT_SIZE_MIN", near(f[0], 2.0));
		glPointParameterfEXT(0x8126, 0.0f);
	}

	if (glGenAsyncMarkersSGIX == NULL || glFinishAsyncSGIX == NULL || glPollAsyncSGIX == NULL ||
	    glAsyncMarkerSGIX == NULL || glIsAsyncMarkerSGIX == NULL || glDeleteAsyncMarkersSGIX == NULL) {
		check("SGIX_async's calls are there", 0);
	} else {
		m = glGenAsyncMarkersSGIX(2);
		check("glGenAsyncMarkersSGIX/glIsAsyncMarkerSGIX", m != 0 && glIsAsyncMarkerSGIX(m + 1) && !glIsAsyncMarkerSGIX(m + 2));
		glAsyncMarkerSGIX(m + 1);
		done = 0;
		check("glFinishAsyncSGIX reports the marker", glFinishAsyncSGIX(&done) == 1 && done == m + 1);
		check("glPollAsyncSGIX then has nothing", glPollAsyncSGIX(&done) == 0);
		glDeleteAsyncMarkersSGIX(m, 2);
		check("glDeleteAsyncMarkersSGIX", !glIsAsyncMarkerSGIX(m));
	}

	if (glFragmentLightfvSGIX == NULL || glGetFragmentLightfvSGIX == NULL ||
	    glFragmentMaterialiSGIX == NULL || glGetFragmentMaterialivSGIX == NULL) {
		check("SGIX_fragment_lighting's calls are there", 0);
	} else {
		glFragmentLightfvSGIX(0x840D, GL_DIFFUSE, d);	/* FRAGMENT_LIGHT1_SGIX */
		glGetFragmentLightfvSGIX(0x840D, GL_DIFFUSE, f);
		check("glFragmentLightfvSGIX reads back", near(f[0], 0.25) && near(f[3], 1.0));
		glFragmentMaterialiSGIX(GL_FRONT, GL_SHININESS, 17);
		glGetFragmentMaterialivSGIX(GL_FRONT, GL_SHININESS, &iv);
		check("glFragmentMaterialiSGIX reads back", iv == 17);
	}

	if (glPixelTexGenParameteriSGIS == NULL || glGetPixelTexGenParameterivSGIS == NULL) {
		check("SGIS_pixel_texture's calls are there", 0);
	} else {
		glPixelTexGenParameteriSGIS(0x8354, GL_CURRENT_RASTER_COLOR);	/* PIXEL_FRAGMENT_RGB_SOURCE_SGIS */
		glGetPixelTexGenParameterivSGIS(0x8354, &iv);
		check("glPixelTexGenParameteriSGIS reads back", iv == GL_CURRENT_RASTER_COLOR);
	}

	if (glTangent3fSGIX == NULL || glBinormal3fSGIX == NULL || glIglooInterfaceSGIX == NULL) {
		check("the entry points with no effect are there", 0);
	} else {
		glBegin(GL_POINTS);
		glTangent3fSGIX(1, 0, 0);
		glBinormal3fSGIX(0, 1, 0);
		glVertex2f(1, 1);
		glEnd();
		glIglooInterfaceSGIX(0, NULL);
		check("the entry points with no effect take their arguments", 1);
	}
	check("no GL error from libGLcore's extras", glGetError() == GL_NO_ERROR);
}

int
main(void)
{
	static int attribs[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_DEPTH_SIZE, 16, None };
	Display *dpy;
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;

	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		printf("FAIL open display\n");
		return 1;
	}
	vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs);
	if (vi == NULL) {
		printf("FAIL no visual\n");
		return 1;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput,
	    vi->visual, CWColormap | CWBorderPixel, &swa);
	XMapWindow(dpy, win);
	XSync(dpy, False);
	ctx = glXCreateContext(dpy, vi, NULL, True);
	if (ctx == NULL || !glXMakeCurrent(dpy, win, ctx)) {
		printf("FAIL no context\n");
		return 1;
	}
	printf("=    renderer %s, version %s\n", (char *)glGetString(GL_RENDERER), (char *)glGetString(GL_VERSION));
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	check("glDrawBuffer/glReadBuffer GL_BACK accepted", glGetError() == GL_NO_ERROR);

	t_glx(dpy, win, ctx);
	t_state();
	t_draw();
	t_lists();
	t_arrays();
	t_pixels();
	t_fog();
	t_reference_plane();
	t_filter4();
	t_sharpen();
	t_interlace();
	t_detail();
	t_texture_select();
	t_texture4d();
	t_multitexture_arrays();
	t_sgis_multitexcoord();
	t_libglcore_extras();
	t_sprite();
	t_pixel_texture();
	t_clipmap();
	t_select_feedback();
	t_maps();
	t_overlay(dpy, win, ctx);
	t_cmode(dpy, win, ctx);
	t_single(dpy, win, ctx);

	printf("%s\n", failures ? "gltest: FAILED" : "gltest: all ok");
	return failures != 0;
}
