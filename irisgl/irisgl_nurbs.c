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

/* ---- old-style curves and patches ----
 *
 * GLPG-II 14-28 to 14-37: cubic segments on a basis matrix. A point is
 * [t^3 t^2 t 1] Mb G (with w for the rational forms); a patch's is
 * [u^3 u^2 u 1] Mu G Mv' [v^3 v^2 v 1]' for each coordinate, drawn as a
 * web of u and v curves. SGI iterates forward differences in the pipe;
 * evaluating each point directly draws the same lines.
 */
#define MAXBASIS 64
static struct {
	short id;
	float m[4][4];
} bases[MAXBASIS];
static int nbases;
static short cur_curvebasis, cur_ubasis, cur_vbasis;
static long curve_segs, patch_ucurves, patch_vcurves, patch_usegs, patch_vsegs;

static const float (*basis_of(short id))[4]
{
	int i;

	for (i = 0; i < nbases; i++)
		if (bases[i].id == id)
			return (const float (*)[4])bases[i].m;
	return NULL;
}

void
defbasis(short id, const Matrix mat)
{
	int i;

	for (i = 0; i < nbases && bases[i].id != id; i++)
		;
	if (i == nbases) {
		if (nbases == MAXBASIS)
			return;
		nbases++;
	}
	bases[i].id = id;
	memcpy(bases[i].m, mat, sizeof bases[i].m);
}

void curvebasis(short id) { cur_curvebasis = id; }
void curveprecision(short n) { curve_segs = n; }
void patchbasis(long u, long v) { cur_ubasis = (short)u; cur_vbasis = (short)v; }
void patchcurves(long u, long v) { patch_ucurves = u; patch_vcurves = v; }
void patchprecision(long u, long v) { patch_usegs = u; patch_vsegs = v; }

/* c = [t^3 t^2 t 1] Mb: the weights of the four control points at t. */
static void
weights(const float (*mb)[4], float t, float c[4])
{
	float tv[4];
	int j;

	tv[0] = t * t * t;
	tv[1] = t * t;
	tv[2] = t;
	tv[3] = 1.0f;
	for (j = 0; j < 4; j++)
		c[j] = tv[0] * mb[0][j] + tv[1] * mb[1][j] + tv[2] * mb[2][j] + tv[3] * mb[3][j];
}

/* One segment of four control points of dim coordinates (3, or 4 with w),
 * stride floats apart. */
static void
curve_segment(const float *g, int dim, int stride)
{
	const float (*mb)[4] = basis_of(cur_curvebasis);
	float c[4], p[4], end[3] = { 0.0f, 0.0f, 0.0f };
	long i;
	int k, j;

	if (mb == NULL || curve_segs <= 0)
		return;
	hgl_begin(GL_LINE_STRIP);
	for (i = 0; i <= curve_segs; i++) {
		weights(mb, (float)i / (float)curve_segs, c);
		for (k = 0; k < 4; k++) {
			p[k] = k < dim ? 0.0f : 1.0f;
			if (k < dim)
				for (j = 0; j < 4; j++)
					p[k] += c[j] * g[j * stride + k];
		}
		if (p[3] == 0.0f)
			continue;
		end[0] = p[0] / p[3];
		end[1] = p[1] / p[3];
		end[2] = p[2] / p[3];
		glVertex3fv(end);
	}
	glEnd();
	hgl_line_end(end);
}

void crv(const Coord g[4][3]) { hgl_iris_ensure(); curve_segment(&g[0][0], 3, 3); }
void rcrv(const Coord g[4][4]) { hgl_iris_ensure(); curve_segment(&g[0][0], 4, 4); }

void
crvn(long n, const Coord g[][3])
{
	long i;

	hgl_iris_ensure();
	for (i = 0; i + 3 < n; i++)
		curve_segment(&g[i][0], 3, 3);
}

void
rcrvn(long n, const Coord g[][4])
{
	long i;

	hgl_iris_ensure();
	for (i = 0; i + 3 < n; i++)
		curve_segment(&g[i][0], 4, 4);
}

/* (Mu G Mv') for one coordinate's geometry: the patch is then
 * [u^3 u^2 u 1] H [v^3 v^2 v 1]'. */
static void
patch_coeffs(const float (*mu)[4], const float (*g)[4], const float (*mv)[4], float h[4][4])
{
	float t[4][4];
	int i, j, k;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			t[i][j] = 0.0f;
			for (k = 0; k < 4; k++)
				t[i][j] += mu[i][k] * g[k][j];
		}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			h[i][j] = 0.0f;
			for (k = 0; k < 4; k++)
				h[i][j] += t[i][k] * mv[j][k];
		}
}

static float
patch_at(float h[4][4], float u, float v)
{
	float uv[4], vv[4], s = 0.0f;
	int i, j;

	uv[0] = u * u * u; uv[1] = u * u; uv[2] = u; uv[3] = 1.0f;
	vv[0] = v * v * v; vv[1] = v * v; vv[2] = v; vv[3] = 1.0f;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			s += uv[i] * h[i][j] * vv[j];
	return s;
}

/* The number of line segments along one direction: at least the
 * precision, and a multiple of the curves across it so the web meets. */
static long
patch_steps(long precision, long across)
{
	long gaps = across > 1 ? across - 1 : 1, n = precision > 0 ? precision : 1;

	return (n + gaps - 1) / gaps * gaps;
}

static void
patch_draw(const Matrix gx, const Matrix gy, const Matrix gz, const Matrix gw)
{
	const float (*mu)[4] = basis_of(cur_ubasis), (*mv)[4] = basis_of(cur_vbasis);
	float h[4][4][4], p[4], end[3] = { 0.0f, 0.0f, 0.0f };
	long a, b, steps;
	int dir, k;

	hgl_iris_ensure();
	if (mu == NULL || mv == NULL || patch_ucurves <= 0 || patch_vcurves <= 0)
		return;
	patch_coeffs(mu, (const float (*)[4])gx, mv, h[0]);
	patch_coeffs(mu, (const float (*)[4])gy, mv, h[1]);
	patch_coeffs(mu, (const float (*)[4])gz, mv, h[2]);
	if (gw != NULL)
		patch_coeffs(mu, (const float (*)[4])gw, mv, h[3]);
	/* dir 0: curves of constant u, along v; dir 1: constant v, along u */
	for (dir = 0; dir < 2; dir++) {
		long curves = dir ? patch_vcurves : patch_ucurves;

		steps = dir ? patch_steps(patch_usegs, patch_vcurves) : patch_steps(patch_vsegs, patch_ucurves);
		for (a = 0; a < curves; a++) {
			float fixed = curves > 1 ? (float)a / (float)(curves - 1) : 0.0f;

			hgl_begin(GL_LINE_STRIP);
			for (b = 0; b <= steps; b++) {
				float run = (float)b / (float)steps;
				float u = dir ? run : fixed, v = dir ? fixed : run;

				for (k = 0; k < 3; k++)
					p[k] = patch_at(h[k], u, v);
				p[3] = gw != NULL ? patch_at(h[3], u, v) : 1.0f;
				if (p[3] == 0.0f)
					continue;
				end[0] = p[0] / p[3];
				end[1] = p[1] / p[3];
				end[2] = p[2] / p[3];
				glVertex3fv(end);
			}
			glEnd();
			hgl_line_end(end);
		}
	}
}

void patch(const Matrix gx, const Matrix gy, const Matrix gz) { patch_draw(gx, gy, gz, NULL); }
void rpatch(const Matrix gx, const Matrix gy, const Matrix gz, const Matrix gw) { patch_draw(gx, gy, gz, gw); }
