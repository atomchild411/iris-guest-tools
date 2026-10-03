/*
 * IRIS GL's NURBS curves and surfaces, drawn by GLU's NURBS renderer.
 *
 * bgnsurface/nurbssurface/endsurface, the trimming loops (bgntrim, pwlcurve,
 * nurbscurve, endtrim), bgncurve/endcurve and the properties are the model
 * GLU took over under newer names, so each call becomes its GLU counterpart
 * on one renderer the process shares. SGI's libGLU tessellates in the guest
 * and draws with OpenGL, which here is the OpenGL shim.
 *
 * What does not carry over as it is:
 *   - IRIS GL gives knots and control points as doubles, with the distance
 *     between control points in bytes; GLU takes floats, with strides in
 *     floats. Every array is copied, and the copies are kept until the end
 *     of the surface or curve in case the renderer reads them only then.
 *   - IRIS GL lights a NURBS surface with the normals of the surface itself.
 *     GLU draws with OpenGL evaluators, which make normals only when
 *     GL_AUTO_NORMAL is on, so it is on for the length of each surface.
 *   - Rational texture and colour data (N_TEXW, N_RGBAW) have no OpenGL
 *     evaluator map; they are drawn from the coordinates without the weight.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/glu.h>
#include "irisgl_shim.h"

static GLUnurbsObj *nurbs;
static float props[N_TMP6 + 1];
static int props_set;

/* The copies handed to GLU since the last bgnsurface or bgncurve. */
static void **copies;
static int ncopies, maxcopies;

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

static void
nurbs_error(GLenum code)
{
	fprintf(stderr, "IRIS GL: NURBS: %s\n", (const char *)gluErrorString(code));
}

static void
defaults(void)
{
	/* The IRIS GL defaults (nurbsproperty(3G)). */
	props[N_PIXEL_TOLERANCE] = 50.0f;
	props[N_CULLING] = 0.0f;
	props[N_DISPLAY] = N_FILL;
	props[N_ERRORCHECKING] = 0.0f;
	props_set = 1;
}

/* N_ISOLINE_S (lines of constant s) has no GLU mode: the patch outlines
 * are the nearest. */
static GLfloat
display_mode(float value)
{
	return value == N_OUTLINE_POLY ? GLU_OUTLINE_POLYGON :
	    value == N_OUTLINE_PATCH || value == N_ISOLINE_S ? GLU_OUTLINE_PATCH : GLU_FILL;
}

static GLUnurbsObj *
renderer(void)
{
	if (!props_set)
		defaults();
	if (nurbs == NULL) {
		hgl_iris_ensure();
		nurbs = gluNewNurbsRenderer();
		if (nurbs == NULL)
			return NULL;
		gluNurbsCallback(nurbs, GLU_ERROR, (void (*)())nurbs_error);
		gluNurbsProperty(nurbs, GLU_SAMPLING_TOLERANCE, props[N_PIXEL_TOLERANCE]);
		gluNurbsProperty(nurbs, GLU_CULLING, props[N_CULLING] != 0.0f ? GL_TRUE : GL_FALSE);
		gluNurbsProperty(nurbs, GLU_DISPLAY_MODE, display_mode(props[N_DISPLAY]));
	}
	return nurbs;
}

static void *
keep(size_t bytes)
{
	void *p = malloc(bytes ? bytes : 1);

	if (p == NULL)
		return NULL;
	if (ncopies == maxcopies) {
		int m = maxcopies ? maxcopies * 2 : 16;
		void **c = realloc(copies, m * sizeof *c);
		if (c == NULL) {
			free(p);
			return NULL;
		}
		copies = c;
		maxcopies = m;
	}
	copies[ncopies++] = p;
	return p;
}

static void
release(void)
{
	while (ncopies > 0)
		free(copies[--ncopies]);
}

static float *
knots(long n, const double *k)
{
	float *f = keep((size_t)(n > 0 ? n : 0) * sizeof *f);
	long i;

	if (f != NULL)
		for (i = 0; i < n; i++)
			f[i] = (float)k[i];
	return f;
}

/* The coordinates a point of `type` has, and the map GLU is given for it, a
 * surface's (surface != 0) or a curve's. 0: not a surface or curve type. */
static int
coords(long type, int surface, GLenum *map)
{
	switch (type) {
	case N_XYZ:     /* also N_V3D */
		*map = surface ? GL_MAP2_VERTEX_3 : GL_MAP1_VERTEX_3;
		return 3;
	case N_XYZW:    /* N_V3DR */
		*map = surface ? GL_MAP2_VERTEX_4 : GL_MAP1_VERTEX_4;
		return 4;
	case N_TEX:     /* N_T2D */
		*map = surface ? GL_MAP2_TEXTURE_COORD_2 : GL_MAP1_TEXTURE_COORD_2;
		return 2;
	case N_TEXW:    /* N_T2DR */
		say_once("NURBS: rational texture coordinates are drawn without their weight");
		*map = surface ? GL_MAP2_TEXTURE_COORD_3 : GL_MAP1_TEXTURE_COORD_3;
		return 3;
	case N_RGBA:    /* N_C4D */
		*map = surface ? GL_MAP2_COLOR_4 : GL_MAP1_COLOR_4;
		return 4;
	case N_RGBAW:   /* N_C4DR */
		say_once("NURBS: rational colours are drawn without their weight");
		*map = surface ? GL_MAP2_COLOR_4 : GL_MAP1_COLOR_4;
		return 5;
	}
	return 0;
}

/* A trimming curve's point: N_ST (N_P2D) or, rational, N_STW (N_P2DR). */
static int
trim_coords(long type, GLenum *map)
{
	if (type == N_ST) {
		*map = GLU_MAP1_TRIM_2;
		return 2;
	}
	if (type == N_STW) {
		*map = GLU_MAP1_TRIM_3;
		return 3;
	}
	return 0;
}

static int in_surface, in_curve, in_trim;

void
bgnsurface(void)
{
	GLUnurbsObj *n;

	TRACE("bgnsurface");
	if ((n = renderer()) == NULL)
		return;
	glPushAttrib(GL_EVAL_BIT);
	glEnable(GL_AUTO_NORMAL);
	gluBeginSurface(n);
	in_surface = 1;
}

void
endsurface(void)
{
	TRACE("endsurface");
	if (!in_surface || nurbs == NULL)
		return;
	/* A surface is lit by the bound material (GLPG-II 14-14): its
	 * normals come from the evaluators, so it is normal-last, and it
	 * never passes through hgl_begin, which sets lighting. */
	hgl_lighting_normal_last();
	gluEndSurface(nurbs);
	glPopAttrib();
	in_surface = 0;
	release();
}

void
nurbssurface(long scount, const double *sknot, long tcount, const double *tknot,
    long soffset, long toffset, const double *ctl, long sorder, long torder, long type)
{
	GLenum map;
	int n = coords(type, 1, &map), c;
	long ns = scount - sorder, nt = tcount - torder, i, j;
	float *sk, *tk, *pts;

	TRACE("nurbssurface");
	if (!in_surface || nurbs == NULL || n == 0 || ns <= 0 || nt <= 0 || ctl == NULL)
		return;
	sk = knots(scount, sknot);
	tk = knots(tcount, tknot);
	pts = keep((size_t)ns * nt * 4 * sizeof *pts);
	if (sk == NULL || tk == NULL || pts == NULL)
		return;
	/* Point (i, j) is soffset*i + toffset*j bytes into ctl; the copy has
	 * them in rows of t, 4 floats apart at most. */
	for (i = 0; i < ns; i++)
		for (j = 0; j < nt; j++) {
			const double *p = (const double *)((const char *)ctl + i * soffset + j * toffset);
			float *q = pts + (i * nt + j) * 4;
			for (c = 0; c < n && c < 4; c++)
				q[c] = (float)p[c];
		}
	gluNurbsSurface(nurbs, scount, sk, tcount, tk, (GLint)(nt * 4), 4, pts,
	    (GLint)sorder, (GLint)torder, map);
}

void
bgntrim(void)
{
	TRACE("bgntrim");
	if (!in_surface || nurbs == NULL)
		return;
	gluBeginTrim(nurbs);
	in_trim = 1;
}

void
endtrim(void)
{
	TRACE("endtrim");
	if (!in_trim || nurbs == NULL)
		return;
	gluEndTrim(nurbs);
	in_trim = 0;
}

void
pwlcurve(long count, const double *data, long offset, long type)
{
	GLenum map;
	int n = trim_coords(type, &map), c;
	float *pts;
	long i;

	TRACE("pwlcurve");
	if (!in_trim || nurbs == NULL || n == 0 || count <= 0 || data == NULL)
		return;
	if ((pts = keep((size_t)count * n * sizeof *pts)) == NULL)
		return;
	for (i = 0; i < count; i++) {
		const double *p = (const double *)((const char *)data + i * offset);
		for (c = 0; c < n; c++)
			pts[i * n + c] = (float)p[c];
	}
	gluPwlCurve(nurbs, (GLint)count, pts, n, map);
}

void
nurbscurve(long count, const double *knot, long offset, const double *ctl, long order, long type)
{
	GLenum map;
	int n, c;
	long np = count - order, i;
	float *k, *pts;

	TRACE("nurbscurve");
	if (nurbs == NULL || (!in_trim && !in_curve) || np <= 0 || ctl == NULL)
		return;
	n = in_trim ? trim_coords(type, &map) : coords(type, 0, &map);
	if (n == 0)
		return;
	k = knots(count, knot);
	pts = keep((size_t)np * 4 * sizeof *pts);
	if (k == NULL || pts == NULL)
		return;
	for (i = 0; i < np; i++) {
		const double *p = (const double *)((const char *)ctl + i * offset);
		for (c = 0; c < n && c < 4; c++)
			pts[i * 4 + c] = (float)p[c];
	}
	gluNurbsCurve(nurbs, (GLint)count, k, 4, pts, (GLint)order, map);
}

void
bgncurve(void)
{
	GLUnurbsObj *n;

	TRACE("bgncurve");
	if ((n = renderer()) == NULL)
		return;
	gluBeginCurve(n);
	in_curve = 1;
}

void
endcurve(void)
{
	TRACE("endcurve");
	if (!in_curve || nurbs == NULL)
		return;
	gluEndCurve(nurbs);
	in_curve = 0;
	release();
}

void
setnurbsproperty(long property, float value)
{
	GLUnurbsObj *n;

	if (!props_set)
		defaults();
	if (property < 1 || property > N_TMP6)
		return;
	props[property] = value;
	/* Before the renderer exists (before winopen, perhaps) the value is
	 * only kept: renderer() applies it when it makes one. */
	if (nurbs == NULL || (n = renderer()) == NULL)
		return;
	switch (property) {
	case N_PIXEL_TOLERANCE:
		gluNurbsProperty(n, GLU_SAMPLING_TOLERANCE, value);
		break;
	case N_CULLING:
		gluNurbsProperty(n, GLU_CULLING, value != 0.0f ? GL_TRUE : GL_FALSE);
		break;
	case N_DISPLAY:
		gluNurbsProperty(n, GLU_DISPLAY_MODE, display_mode(value));
		break;
	default:
		/* N_ERRORCHECKING, N_SUBDIVISIONS, N_S_STEPS, N_T_STEPS, N_TILES
		 * and the N_TMPs are kept for getnurbsproperty only. */
		break;
	}
}

void
getnurbsproperty(long property, float *value)
{
	if (!props_set)
		defaults();
	if (value != NULL && property >= 1 && property <= N_TMP6)
		*value = props[property];
}
