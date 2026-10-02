/* IRIS GL's drawing and state, as OpenGL. See irisgl_shim.h. */
#include "irisgl_shim.h"
#include <gl/get.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- framebuffer ---- */

/*
 * IRIS GL's clear fills with the *current colour*; OpenGL's glClear fills with
 * a clear colour of its own that nothing here was setting. So every frame was
 * cleared to the default black no matter what the program asked for.
 */
static float cur_r = 0.0f, cur_g = 0.0f, cur_b = 0.0f, cur_a = 1.0f;
unsigned long hgl_colour_serial, hgl_raster_serial;

/*
 * In lmcolor(LMC_COLOR) a vertex is lit only when a normal was the last of
 * the two sent before it; when the colour was, it is drawn in the colour
 * (lmcolor(3G)). So a program can draw an unlit shape -- a backdrop, a
 * translucent sheet -- in the middle of a lit scene by sending a colour and
 * no normal. This is which came last.
 */
static int colour_last;
long hgl_lmcolor_mode = LMC_COLOR;

/* Every way of setting the colour comes through here, so `clear` -- which in
 * IRIS GL fills with the current colour -- always has the right one. */
void
hgl_set_colour(float r, float g, float b, float a)
{
	if (hgl_lmcolor_mode == LMC_COLOR)
		colour_last = 1;
	cur_r = r;
	cur_g = g;
	cur_b = b;
	cur_a = a;
	glColor4f(r, g, b, a);
	hgl_colour_serial++;
}

void
hgl_current_colour(float *rgba)
{
	rgba[0] = cur_r;
	rgba[1] = cur_g;
	rgba[2] = cur_b;
	rgba[3] = cur_a;
}

/* Whether GL_LIGHTING is on, as this library last set it; see hgl_begin. */
static int prim_lit;

/*
 * The raster position, through the matrices like a vertex but never lit:
 * the colour characters and pixels take is the current colour (cmov(3G)),
 * and OpenGL would light it when GL_LIGHTING is on.
 */
void
hgl_rasterpos(float x, float y, float z)
{
	if (prim_lit)
		hgl_enable(GL_LIGHTING, 0);
	glRasterPos3f(x, y, z);
	if (prim_lit)
		hgl_enable(GL_LIGHTING, 1);
}

/* The index color() last set, for getcolor. */
long hgl_colour_index;

void
hgl_latch_raster_colour(void)
{
	GLboolean valid = GL_FALSE;
	GLfloat p[4];
	int w = hgl_iris.w > 0 ? hgl_iris.w : 1, h = hgl_iris.h > 0 ? hgl_iris.h : 1;

	if (hgl_raster_serial == hgl_colour_serial)
		return;
	hgl_raster_serial = hgl_colour_serial;
	glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
	if (!valid)
		return;
	glGetFloatv(GL_CURRENT_RASTER_POSITION, p);
	/* The same window position (and depth) through a pixel-exact
	 * projection: window z = (1 - z) / 2 for glOrtho(..., -1, 1). */
	glPushAttrib(GL_TRANSFORM_BIT | GL_VIEWPORT_BIT);
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, w, 0, h, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	hgl_rasterpos(p[0], p[1], 1.0f - 2.0f * p[2]);
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopAttrib();
}

void
clear(void)
{
	TRACE("clear");
	hgl_iris_ensure();
	if (hgl_selecting)
		return;
	glClearColor(cur_r, cur_g, cur_b, cur_a);
	glClear(GL_COLOR_BUFFER_BIT);
}

void
zclear(void)
{
	TRACE("zclear");
	hgl_iris_ensure();
	if (hgl_selecting)
		return;
	/* always to the far end (zclear(3G)), whatever czclear last used */
	glClearDepth(1.0);
	glClear(GL_DEPTH_BUFFER_BIT);
}

void
zbuffer(Boolean on)
{
	hgl_irisgl_tracef("zbuffer %d", (int)on);
	hgl_irisgl_trace(on ? "zbuffer on" : "zbuffer off");
	hgl_iris_ensure();
	if (on)
		hgl_enable(GL_DEPTH_TEST, 1);
	else
		hgl_enable(GL_DEPTH_TEST, 0);
}

/*
 * IRIS GL's depth range is in screen z units -- 0 .. 0x7fffff on this class of
 * machine, and a program may hand them over either way round to flip the sense
 * of the depth test. OpenGL's is 0 .. 1, so both ends are scaled by the range
 * getgdesc(GD_ZMAX) reports, which is the same number the program read.
 */
void
lsetdepth(long near, long far)
{
	double scale = (double)0x7fffff;
	char buf[64];

	hgl_iris_ensure();
	sprintf(buf, "lsetdepth %ld %ld", (long)near, (long)far);
	hgl_irisgl_trace(buf);
	glDepthRange((double)near / scale, (double)far / scale);
}


/*
 * cpack packs the components the other way round from anything OpenGL has:
 * the low byte is red and the high byte alpha (0xAABBGGRR).
 */
void
cpack(unsigned long v)
{
	hgl_irisgl_tracef("cpack %08lx", v);
	hgl_iris_ensure();
	hgl_set_colour((float)(v & 0xff) / 255.0f, (float)((v >> 8) & 0xff) / 255.0f,
	    (float)((v >> 16) & 0xff) / 255.0f, (float)((v >> 24) & 0xff) / 255.0f);
}

/* ---- primitives ---- */

/* For the trace: the first vertices of each primitive, with what they carry. */
static int block_verts;
void bgnpolygon(void) { TRACE("bgnpolygon"); hgl_begin(GL_POLYGON); }
void endpolygon(void) { TRACE("endpolygon"); glEnd(); }
/* The current normal and texture coordinate, which swaptmesh's repeated
 * vertices need (see there). */
static float cur_n[3] = { 0.0f, 0.0f, 1.0f };
static float cur_t[2];

void
n3f(const float v[3])
{
	cur_n[0] = v[0]; cur_n[1] = v[1]; cur_n[2] = v[2];
	colour_last = 0;
	glNormal3fv(v);
}

/* ---- matrices ---- */

void
mmode(short m)
{
	TRACE("mmode");
	hgl_iris_ensure();
	/*
	 * MSINGLE keeps its one matrix in the modelview stack. Entering or
	 * leaving it empties the stacks and makes every matrix the identity
	 * (mmode(3G)): a 2-D overlay drawn in MSINGLE after a 3-D view would
	 * otherwise go through the old perspective.
	 */
	if ((hgl_iris.mmode == MSINGLE) != (m == MSINGLE)) {
		GLint depth = 1;

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depth);
		while (depth-- > 1)
			glPopMatrix();
		glLoadIdentity();
	}
	hgl_iris.mmode = m;
	switch (m) {
	case MPROJECTION: glMatrixMode(GL_PROJECTION); break;
	case MTEXTURE:    glMatrixMode(GL_TEXTURE); break;
	/* MSINGLE and MVIEWING are both the modelview stack here: MSINGLE is
	 * IRIS GL's single combined matrix, which a program in that mode uses
	 * for exactly what the modelview does. */
	default:          glMatrixMode(GL_MODELVIEW); break;
	}
}

void
hgl_projection_begin(void)
{
	hgl_iris_ensure();
	/* ortho, perspective and window replace the Projection matrix in
	 * every multi-matrix mode, MTEXTURE included */
	if (hgl_iris.mmode == MVIEWING || hgl_iris.mmode == MTEXTURE)
		glMatrixMode(GL_PROJECTION);
	/* while picking, the projection is restated onto the pick matrix */
	if (hgl_selecting == 1)
		glLoadMatrixf(hgl_pick_matrix);
	else
		glLoadIdentity();
}

void
hgl_projection_end(void)
{
	if (hgl_iris.mmode == MVIEWING)
		glMatrixMode(GL_MODELVIEW);
	else if (hgl_iris.mmode == MTEXTURE)
		glMatrixMode(GL_TEXTURE);
}

/*
 * IRIS GL angles are in *tenths of a degree*, which is the trap in this whole
 * file: perspective(450, ...) is 45 degrees, and reading it as 450 gives a
 * frustum turned inside out.
 */
void
perspective(Angle fovy, float aspect, Coord near, Coord far)
{
	double f, m[16];
	char buf[96];

	sprintf(buf, "perspective fovy=%d aspect=%.3f near=%.1f far=%.1f",
	    (int)fovy, (double)aspect, (double)near, (double)far);
	hgl_irisgl_trace(buf);
	/*
	 * IRIS GL's perspective *defines* the projection: it replaces the
	 * matrix, where glFrustum multiplies into it. atlantis calls it
	 * once a frame, so multiplying compounded the projection 2400 times in
	 * forty seconds and squeezed the scene into nothing -- a window that was
	 * black not because nothing drew, but because everything drew far too
	 * small to see.
	 */
	/*
	 * The matrix itself, not glFrustum: a frustum's sides come from near,
	 * and IRIS GL takes near = 0 (amesh does) -- the host GL refuses such a
	 * frustum and would leave no projection at all. From the angle, near = 0
	 * still projects x and y; only depth is lost (it comes out the same
	 * everywhere). For any other near this is the frustum glFrustum would
	 * make.
	 */
	hgl_projection_begin();
	f = 1.0 / tan((double)fovy / 10.0 * M_PI / 360.0);
	memset(m, 0, sizeof m);
	m[0] = f / (aspect != 0.0f ? aspect : 1.0f);
	m[5] = f;
	m[10] = near != far ? (far + near) / (near - far) : -1.0;
	m[11] = -1.0;
	m[14] = near != far ? 2.0 * far * near / (near - far) : 0.0;
	glMultMatrixd(m);
	hgl_projection_end();
}

/* ortho and ortho2 define the projection, as perspective does: they replace
 * it rather than multiplying into it. */
void
ortho(Coord l, Coord r, Coord b, Coord t, Coord n, Coord f)
{
	TRACE("ortho");
	hgl_projection_begin();
	glOrtho(l, r, b, t, n, f);
	hgl_projection_end();
}

void
ortho2(Coord l, Coord r, Coord b, Coord t)
{
	TRACE("ortho2");
	hgl_projection_begin();
	glOrtho(l, r, b, t, -1.0, 1.0);
	hgl_projection_end();
}

void translate(Coord x, Coord y, Coord z) { hgl_irisgl_tracef("translate %g %g %g", x, y, z); glTranslatef(x, y, z); }
void scale(float x, float y, float z) { hgl_irisgl_tracef("scale %g %g %g", x, y, z); glScalef(x, y, z); }

/* rot takes degrees; rotate (not this) takes tenths. */
void
rot(float amount, char axis)
{
	hgl_irisgl_tracef("rot %g %c", amount, axis);
	glRotatef(amount, axis == 'x' || axis == 'X',
	    axis == 'y' || axis == 'Y', axis == 'z' || axis == 'Z');
}

void pushmatrix(void) { TRACE("pushmatrix"); hgl_iris_ensure(); glPushMatrix(); }
void popmatrix(void) { glPopMatrix(); }
void loadmatrix(const Matrix m) { hgl_irisgl_tracef("loadmatrix %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]); hgl_iris_ensure(); glLoadMatrixf((const GLfloat *)m); }

void
getmatrix(Matrix m)
{
	hgl_iris_ensure();
	glGetFloatv(hgl_iris.mmode == MPROJECTION ? GL_PROJECTION_MATRIX
	    : hgl_iris.mmode == MTEXTURE ? GL_TEXTURE_MATRIX
	    : GL_MODELVIEW_MATRIX, (GLfloat *)m);
}

/* ---- lighting ----
 *
 * IRIS GL defines a material, light or lighting model as a list of property
 * tokens, each followed by its values, ending in LMNULL, and binds a definition
 * to a target. Definitions are kept here as complete sets of values -- the
 * defaults of lmdef(3G), overwritten by each property given -- and turned into
 * glMaterial/glLight/glLightModel calls when bound. A property list is walked
 * token by token: a value of 0.0 is LMNULL only where a token is expected (a
 * light with AMBIENT 0 0 0 used to end right there, losing its LCOLOR).
 * Redefining a bound definition takes effect at once, as it does on SGI.
 */
#define MAXDEF 256

typedef struct {
	short used;
	float emission[4], ambient[4], diffuse[4], specular[4];
	float shininess, alpha;
} Material;

typedef struct {
	short used;
	float ambient[4], colour[4], position[4], spotdir[3];
	float spot_exp, spot_cutoff;
} Light;

typedef struct {
	short used;
	float ambient[4];
	float localviewer, att_const, att_linear, att_quad, twoside;
} LModel;

static Material materials[MAXDEF];
static Light lights[MAXDEF];
static LModel lmodels[MAXDEF];
static short bound_material, bound_backmaterial, bound_lmodel, bound_light[MAXLIGHTS];

/* Whether a back material is bound: without one, the front material (and
 * what lmcolor tracks into it) lights back faces too. */
int
hgl_back_material_bound(void)
{
	return bound_backmaterial != 0;
}

/*
 * IRIS GL numbers a definition with any positive short -- Performer's start
 * at 2049 -- so each kind keeps MAXDEF of them in slots found by number, and
 * everything past lmdef and lmbind works on the slot. The ids of the slots
 * in use, 0 for a free one; slot 0 is never used, as index 0 means none.
 */
static long material_ids[MAXDEF], light_ids[MAXDEF], lmodel_ids[MAXDEF];

static long *
def_ids(short deftype)
{
	switch (deftype) {
	case DEFMATERIAL: return material_ids;
	case DEFLIGHT: return light_ids;
	case DEFLMODEL: return lmodel_ids;
	}
	return NULL;
}

/* The slot of definition `index` in a table of MAXDEF, taking a free one for
 * a new definition when `create`; 0 if there is none. Textures use it too. */
static int
def_slot(long *ids, long index, int create)
{
	int i, free_slot = 0;

	if (ids == NULL || index <= 0)
		return 0;
	for (i = 1; i < MAXDEF; i++) {
		if (ids[i] == index)
			return i;
		if (ids[i] == 0 && free_slot == 0)
			free_slot = i;
	}
	if (!create)
		return 0;
	if (free_slot)
		ids[free_slot] = index;
	return free_slot;
}

static void
set4(float *d, float a, float b, float c, float e)
{
	d[0] = a; d[1] = b; d[2] = c; d[3] = e;
}

/* How many values follow a property token; -1 for a token we do not know,
 * whose values cannot be told from the next token. */
static int
prop_count(int tok)
{
	switch (tok) {
	case EMISSION: case AMBIENT: case DIFFUSE: case SPECULAR:
	case COLORINDEXES: case LCOLOR: case SPOTDIRECTION:
		return 3;
	case SHININESS: case ALPHA: case LOCALVIEWER: case ATTENUATION2: case TWOSIDE:
		return 1;
	case SPOTLIGHT: case ATTENUATION:
		return 2;
	case POSITION:
		return 4;
	default:
		return -1;
	}
}

static void apply_material(GLenum face, short index);
static void apply_light(int n, short index);
static void apply_lmodel(short index);

/* Lighting is on while both a material and a lighting model are bound, and
 * the last of a colour and a normal was a normal (see colour_last). */
static int
lighting_wanted(void)
{
	return bound_material && bound_lmodel && !(colour_last && hgl_lmcolor_mode == LMC_COLOR);
}

/* What the primitive being drawn was begun as. */
static GLenum prim_mode;

static void
update_lighting_enable(void)
{
	prim_lit = lighting_wanted();
	hgl_enable(GL_LIGHTING, prim_lit);
}

/*
 * Every primitive starts here. OpenGL lights a primitive whole or not at
 * all, and lighting can change only between primitives: it is set at the
 * start, and once more at the first vertex (v3f) for a program that sends
 * its normal or colour after the bgn. A change after that is not followed.
 */
void
hgl_begin(GLenum mode)
{
	hgl_iris_ensure();
	update_lighting_enable();
	prim_mode = mode;
	block_verts = 0;
	glBegin(mode);
}

void
lmdef(short deftype, short index, short np, const float props[])
{
	int i = 0, k, tok, n, reset;
	const float *v;

	hgl_irisgl_tracef("lmdef %d %d np %d", deftype, index, np);
	for (i = 0; props != NULL && i < (np > 0 ? np : 16); i++)
		hgl_irisgl_tracef("  lmdef prop[%d] %g", i, props[i]);
	i = 0;
	if ((index = def_slot(def_ids(deftype), index, 1)) == 0)
		return;
	/* The first definition of an index starts from the defaults, and so
	 * does an empty property list; any other only modifies what it names
	 * (lmdef(3G)). */
	reset = props == NULL || props[0] == LMNULL;
	switch (deftype) {
	case DEFMATERIAL: {
		Material *m = &materials[index];
		if (m->used && !reset)
			break;
		set4(m->emission, 0, 0, 0, 1);
		set4(m->ambient, 0.2f, 0.2f, 0.2f, 1);
		set4(m->diffuse, 0.8f, 0.8f, 0.8f, 1);
		set4(m->specular, 0, 0, 0, 1);
		m->shininess = 0;
		m->alpha = 1;
		m->used = 1;
		break;
	}
	case DEFLIGHT: {
		Light *l = &lights[index];
		if (l->used && !reset)
			break;
		set4(l->ambient, 0, 0, 0, 1);
		set4(l->colour, 1, 1, 1, 1);
		set4(l->position, 0, 0, 1, 0);
		l->spotdir[0] = 0; l->spotdir[1] = 0; l->spotdir[2] = -1;
		l->spot_exp = 0;
		l->spot_cutoff = 180;
		l->used = 1;
		break;
	}
	case DEFLMODEL: {
		LModel *m = &lmodels[index];
		if (m->used && !reset)
			break;
		set4(m->ambient, 0.2f, 0.2f, 0.2f, 1);
		m->localviewer = 0;
		m->att_const = 1;
		m->att_linear = 0;
		m->att_quad = 0;
		m->twoside = 0;
		m->used = 1;
		break;
	}
	default:
		return;
	}
	while (props != NULL && (np <= 0 || i < np)) {
		tok = (int)props[i];
		if (props[i] == LMNULL)
			break;
		n = prop_count(tok);
		if (n < 0 || (np > 0 && i + n >= np))
			break;
		v = &props[i + 1];
		switch (deftype) {
		case DEFMATERIAL: {
			Material *m = &materials[index];
			switch (tok) {
			case EMISSION: set4(m->emission, v[0], v[1], v[2], 1); break;
			case AMBIENT: set4(m->ambient, v[0], v[1], v[2], 1); break;
			case DIFFUSE: set4(m->diffuse, v[0], v[1], v[2], 1); break;
			case SPECULAR: set4(m->specular, v[0], v[1], v[2], 1); break;
			case SHININESS: m->shininess = v[0]; break;
			case ALPHA: m->alpha = v[0]; break;
			default: break;
			}
			break;
		}
		case DEFLIGHT: {
			Light *l = &lights[index];
			switch (tok) {
			case AMBIENT: set4(l->ambient, v[0], v[1], v[2], 1); break;
			case LCOLOR: set4(l->colour, v[0], v[1], v[2], 1); break;
			case POSITION: set4(l->position, v[0], v[1], v[2], v[3]); break;
			case SPOTDIRECTION: for (k = 0; k < 3; k++) l->spotdir[k] = v[k]; break;
			case SPOTLIGHT: l->spot_exp = v[0]; l->spot_cutoff = v[1]; break;
			default: break;
			}
			break;
		}
		default: {
			LModel *m = &lmodels[index];
			switch (tok) {
			case AMBIENT: set4(m->ambient, v[0], v[1], v[2], 1); break;
			case LOCALVIEWER: m->localviewer = v[0]; break;
			case ATTENUATION: m->att_const = v[0]; m->att_linear = v[1]; break;
			case ATTENUATION2: m->att_quad = v[0]; break;
			case TWOSIDE: m->twoside = v[0]; break;
			default: break;
			}
			break;
		}
		}
		i += 1 + n;
	}
	if (!hgl_iris.opened)
		return;
	/* A definition in use changes what is drawn from now on. */
	if (deftype == DEFMATERIAL) {
		if (bound_material == index)
			apply_material(bound_backmaterial ? GL_FRONT : GL_FRONT_AND_BACK, index);
		if (bound_backmaterial == index)
			apply_material(GL_BACK, index);
	} else if (deftype == DEFLMODEL) {
		if (bound_lmodel == index)
			apply_lmodel(index);
	} else {
		for (k = 0; k < MAXLIGHTS; k++)
			if (bound_light[k] == index)
				apply_light(k, index);
	}
}

static void
apply_material(GLenum face, short index)
{
	Material *m = &materials[index];
	float d[4];

	glMaterialfv(face, GL_EMISSION, m->emission);
	glMaterialfv(face, GL_AMBIENT, m->ambient);
	/* ALPHA is the alpha of the lit colour: OpenGL takes it from diffuse. */
	set4(d, m->diffuse[0], m->diffuse[1], m->diffuse[2], m->alpha);
	glMaterialfv(face, GL_DIFFUSE, d);
	/* A SHININESS of 0 turns the specular term off in IRIS GL (lmdef(3G));
	 * in OpenGL it is the widest highlight there is. */
	if (m->shininess <= 0.0f) {
		set4(d, 0.0f, 0.0f, 0.0f, m->specular[3]);
		glMaterialfv(face, GL_SPECULAR, d);
	} else
		glMaterialfv(face, GL_SPECULAR, m->specular);
	glMaterialf(face, GL_SHININESS, m->shininess > 128 ? 128 : m->shininess);
}

static void
apply_attenuation(int n)
{
	LModel *m = bound_lmodel ? &lmodels[bound_lmodel] : NULL;

	glLightf(GL_LIGHT0 + n, GL_CONSTANT_ATTENUATION, m ? m->att_const : 1.0f);
	glLightf(GL_LIGHT0 + n, GL_LINEAR_ATTENUATION, m ? m->att_linear : 0.0f);
	glLightf(GL_LIGHT0 + n, GL_QUADRATIC_ATTENUATION, m ? m->att_quad : 0.0f);
}

/* Position and direction are taken through the matrix current at lmbind,
 * exactly as glLightfv does. */
static void
apply_light(int n, short index)
{
	Light *l = &lights[index];
	GLenum gl = GL_LIGHT0 + n;

	glLightfv(gl, GL_AMBIENT, l->ambient);
	glLightfv(gl, GL_DIFFUSE, l->colour);
	glLightfv(gl, GL_SPECULAR, l->colour);
	glLightfv(gl, GL_POSITION, l->position);
	glLightfv(gl, GL_SPOT_DIRECTION, l->spotdir);
	glLightf(gl, GL_SPOT_EXPONENT, l->spot_exp);
	glLightf(gl, GL_SPOT_CUTOFF, l->spot_cutoff > 90 ? 180 : l->spot_cutoff);
	apply_attenuation(n);
	glEnable(gl);
}

static void
apply_lmodel(short index)
{
	LModel *m = &lmodels[index];
	int n;

	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, m->ambient);
	glLightModelf(GL_LIGHT_MODEL_LOCAL_VIEWER, m->localviewer);
	glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, m->twoside);
	for (n = 0; n < MAXLIGHTS; n++)
		if (bound_light[n])
			apply_attenuation(n);
}

void
lmbind(short target, short index)
{
	short id = index;

	hgl_irisgl_tracef("lmbind %d %d", target, index);
	hgl_iris_ensure();
	if (index < 0)
		return;
	if (target == MATERIAL || target == BACKMATERIAL) {
		if (id && (index = def_slot(material_ids, id, 0)) == 0)
			return;
		if (index && !materials[index].used)
			return;
		if (target == MATERIAL)
			bound_material = index;
		else
			bound_backmaterial = index;
		if (index) {
			if (target == BACKMATERIAL)
				apply_material(GL_BACK, index);
			else
				apply_material(bound_backmaterial ? GL_FRONT : GL_FRONT_AND_BACK, index);
		}
		update_lighting_enable();
	} else if (target >= LIGHT0 && target <= LIGHT7) {
		int n = target - LIGHT0;
		if (id && (index = def_slot(light_ids, id, 0)) == 0)
			return;
		if (index && !lights[index].used)
			return;
		bound_light[n] = index;
		if (index)
			apply_light(n, index);
		else
			glDisable(GL_LIGHT0 + n);
	} else if (target == LMODEL) {
		if (id && (index = def_slot(lmodel_ids, id, 0)) == 0)
			return;
		if (index && !lmodels[index].used)
			return;
		bound_lmodel = index;
		if (index)
			apply_lmodel(index);
		update_lighting_enable();
	}
}

/* ---- texture ---- */

/* By number, as the lighting definitions are (def_slot). */
static long tex_ids[MAXDEF];
static GLuint texnames[MAXDEF];
/*
 * Texturing is on only while both a texture and a texture environment are
 * bound (texbind(3G), tevbind(3G)): binding 0 to either turns it off, and
 * both start unbound.
 */
static int tex_bound, tev_bound;

/*
 * What props says about the texture: wrap modes, filters, and whether the
 * image's components are 8 or 16 bits. Each symbol is followed by as many
 * values as texdef(3G) gives it; a symbol this does not know ends the walk,
 * since its values cannot be told from the next symbol.
 */
struct texprops {
	GLint wrap_s, wrap_t, minf, magf;
	int pack16;
};

static GLint
tx_wrap(float v)
{
	return (long)v == TX_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

static void
tex_props(long np, const float props[], struct texprops *tp)
{
	long i = 0;

	tp->wrap_s = tp->wrap_t = GL_REPEAT;
	tp->minf = tp->magf = GL_LINEAR;
	tp->pack16 = 0;
	if (props == NULL)
		return;
	while ((np <= 0 || i < np) && (long)props[i] != TX_NULL) {
		long sym = (long)props[i++];
		float v = props[i];
		switch (sym) {
		case TX_MINFILTER:
			tp->minf = (long)v == TX_POINT ? GL_NEAREST : GL_LINEAR;
			i++;
			break;
		case TX_MAGFILTER:
		case TX_MAGFILTER_COLOR:
		case TX_MAGFILTER_ALPHA:
			tp->magf = (long)v == TX_POINT ? GL_NEAREST : GL_LINEAR;
			i++;
			break;
		case TX_WRAP:
			tp->wrap_s = tp->wrap_t = tx_wrap(v);
			i++;
			break;
		case TX_WRAP_S: tp->wrap_s = tx_wrap(v); i++; break;
		case TX_WRAP_T: tp->wrap_t = tx_wrap(v); i++; break;
		case TX_WRAP_R: i++; break;
		case TX_EXTERNAL_FORMAT:
			tp->pack16 = (long)v == TX_PACK_16;
			i++;
			break;
		case TX_INTERNAL_FORMAT:
		case TX_CONTROL_CLAMP:
			i++;
			break;
		case TX_FAST_DEFINE:
			break;
		case TX_CONTROL_POINT: i += 2; break;
		case TX_TILE: i += 4; break;
		case TX_DETAIL: i += 5; break;
		case TX_MIPMAP_FILTER_KERNEL: i += 8; break;
		default:
			return;
		}
	}
}

/*
 * texdef2d's image, per texdef(3G): the pixels are *packed bytes* -- nc
 * components each, 8 or 16 bits (TX_EXTERNAL_FORMAT) -- and each row starts
 * on a long-word boundary. A four-component texel is ABGR, and fewer
 * components drop from the front: BGR, then alpha and luminance, then
 * luminance alone.
 *
 * This used to read one whole word per texel. That is four times the image
 * for a one-component texture, and flight, whose first texture is exactly
 * that, died reading past the end of it.
 */
void
texdef2d(long index, long nc, long width, long height,
    const unsigned long *image, long np, const float props[])
{
	struct texprops tp;
	const unsigned char *src = (const unsigned char *)image;
	unsigned char *buf;
	long x, y, bpc, row;

	{
		char what[80];
		sprintf(what, "texdef2d index %ld nc %ld %ldx%ld np %ld", index, nc, width, height, np);
		TRACE(what);
	}
	hgl_iris_ensure();
	if (width <= 0 || height <= 0 || nc < 1 || nc > 4 || (index = def_slot(tex_ids, index, 1)) == 0)
		return;
	tex_props(np, props, &tp);
	bpc = tp.pack16 ? 2 : 1;
	row = (width * nc * bpc + 3) & ~3L;
	buf = (unsigned char *)malloc(width * height * 4);
	if (!buf)
		return;
	for (y = 0; y < height; y++) {
		const unsigned char *in = src + y * row;
		unsigned char *out = buf + y * width * 4;
		for (x = 0; x < width; x++, out += 4) {
			/* The top byte of each component: a 16-bit one is big-endian. */
			const unsigned char *t = in + x * nc * bpc;
#define C(k) t[(k) * bpc]
			switch (nc) {
			case 1:
				out[0] = out[1] = out[2] = C(0);
				out[3] = 255;
				break;
			case 2:
				out[0] = out[1] = out[2] = C(1);
				out[3] = C(0);
				break;
			case 3:
				out[0] = C(2); out[1] = C(1); out[2] = C(0);
				out[3] = 255;
				break;
			default:
				out[0] = C(3); out[1] = C(2); out[2] = C(1);
				out[3] = C(0);
				break;
			}
#undef C
		}
	}
	if (!texnames[index])
		glGenTextures(1, &texnames[index]);
	glBindTexture(GL_TEXTURE_2D, texnames[index]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tp.minf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tp.magf);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, tp.wrap_s);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, tp.wrap_t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
	    GL_RGBA, GL_UNSIGNED_BYTE, buf);
	free(buf);
	/* defining a texture does not bind it */
	glBindTexture(GL_TEXTURE_2D, tex_bound ? texnames[tex_bound] : 0);
}


/* The environments tevdef made: how texture values meet the incoming colour,
 * and TV_BLEND's constant colour, which starts white (tevdef(3G)). */
static long tev_ids[MAXDEF];
static struct {
	GLint mode;
	float colour[4];
} tevs[MAXDEF];

static void
update_texturing(void)
{
	hgl_enable(GL_TEXTURE_2D, tex_bound && texnames[tex_bound] && tev_bound);
}

void
texbind(long target, long index)
{
	hgl_irisgl_tracef("texbind %ld %ld", target, index);
	hgl_iris_ensure();
	if (target != TX_TEXTURE_0)
		return;
	tex_bound = def_slot(tex_ids, index, 0);
	if (tex_bound && texnames[tex_bound])
		glBindTexture(GL_TEXTURE_2D, texnames[tex_bound]);
	update_texturing();
}

/*
 * The arithmetic of TV_MODULATE, TV_DECAL and TV_BLEND is OpenGL's
 * GL_MODULATE, GL_DECAL and GL_BLEND exactly, given that every texture is
 * kept as RGBA with an alpha of 1 where the image has none (see texdef2d).
 * TV_ALPHA, TV_SHADOW and the component selects are not on IMPACT; they
 * modulate.
 */
void
tevdef(long index, long np, const float props[])
{
	long i = 0;
	int slot;

	hgl_irisgl_tracef("tevdef %ld np %ld", index, np);
	if ((slot = def_slot(tev_ids, index, 1)) == 0)
		return;
	tevs[slot].mode = GL_MODULATE;
	set4(tevs[slot].colour, 1.0f, 1.0f, 1.0f, 1.0f);
	while (props != NULL && (np <= 0 || i < np) && (long)props[i] != TV_NULL) {
		switch ((long)props[i++]) {
		case TV_MODULATE: tevs[slot].mode = GL_MODULATE; break;
		case TV_DECAL: tevs[slot].mode = GL_DECAL; break;
		case TV_BLEND: tevs[slot].mode = GL_BLEND; break;
		case TV_COLOR:
			set4(tevs[slot].colour, props[i], props[i + 1], props[i + 2], props[i + 3]);
			i += 4;
			break;
		case TV_ALPHA: case TV_SHADOW:
		case TV_I_GETS_R: case TV_I_GETS_G: case TV_I_GETS_B: case TV_I_GETS_A:
		case TV_IA_GETS_RG: case TV_IA_GETS_BA: case TV_I_GETS_I:
			break;
		case TV_COMPONENT_SELECT:
			i++;
			break;
		default:
			i = np > 0 ? np : i;
			props = NULL;	/* a symbol whose values are unknown ends the walk */
			break;
		}
	}
}

void
tevbind(long target, long index)
{
	hgl_irisgl_tracef("tevbind %ld %ld", target, index);
	hgl_iris_ensure();
	if (target != TV_ENV0)
		return;
	tev_bound = def_slot(tev_ids, index, 0);
	if (tev_bound) {
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, tevs[tev_bound].mode);
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tevs[tev_bound].colour);
	}
	update_texturing();
}

/*
 * texgen(3G): TG_LINEAR (an object-space plane), TG_CONTOUR (an eye-space
 * plane, through the matrix current now) and TG_SPHEREMAP define a
 * coordinate's function; TG_ON and TG_OFF turn it on and off, and defining
 * does not turn it on.
 */
void
texgen(long coord, long mode, const float params[])
{
	static const GLenum coords[4] = { GL_S, GL_T, GL_R, GL_Q };
	static const GLenum gens[4] = {
		GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q
	};
	GLenum c;

	hgl_irisgl_tracef("texgen %ld %ld", coord, mode);
	hgl_iris_ensure();
	if (coord < TX_S || coord > TX_Q)
		return;
	c = coords[coord];
	switch (mode) {
	case TG_OFF:
		glDisable(gens[coord]);
		break;
	case TG_ON:
		glEnable(gens[coord]);
		break;
	case TG_LINEAR:
		glTexGeni(c, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
		if (params)
			glTexGenfv(c, GL_OBJECT_PLANE, params);
		break;
	case TG_CONTOUR:
		glTexGeni(c, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
		if (params)
			glTexGenfv(c, GL_EYE_PLANE, params);
		break;
	case TG_SPHEREMAP:
		if (c == GL_S || c == GL_T)
			glTexGeni(c, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
		break;
	}
}

/* ---- the rest of the drawing set ----
 *
 * These are the entry points the demos on this image actually reach, found by
 * running them under tools-irisdemo.py and reading what the generated stubs
 * said they wanted. Nearly all are OpenGL under another name.
 */

/* IRIS GL's primitives, each an OpenGL one. */
void bgnline(void) { hgl_begin(GL_LINE_STRIP); }
void endline(void) { glEnd(); }
void bgnclosedline(void) { hgl_begin(GL_LINE_LOOP); }
void endclosedline(void) { glEnd(); }
void bgnpoint(void) { hgl_begin(GL_POINTS); }
void endpoint(void) { glEnd(); }
static int tmesh_n;
/* The facing of the next triangle: 0 as (older, newer, new vertex), 1 the
 * other way round. It alternates with each triangle, as in a strip, and
 * swaptmesh flips it too -- which is what keeps a fan made with swaptmesh,
 * or any mesh that swaps, facing one way. Two-sided lighting and backface
 * removal both see it. */
static int tmesh_odd;
void bgntmesh(void) { hgl_irisgl_tracef("bgntmesh"); tmesh_n = 0; tmesh_odd = 0; hgl_begin(GL_TRIANGLE_STRIP); }
void endtmesh(void) { glEnd(); }
void bgnqstrip(void) { hgl_irisgl_tracef("bgnqstrip"); hgl_begin(GL_QUAD_STRIP); }
void endqstrip(void) { glEnd(); }

/*
 * A triangle mesh keeps two vertices, and each new one makes a triangle with
 * them and then replaces the older; swaptmesh swaps which of the two is the
 * older, so the next vertex replaces the other one. GL_TRIANGLE_STRIP has no
 * such control, and a strip that swaps is a different strip -- so swaptmesh
 * ends the run and starts another with the two kept vertices in their new
 * order, which draws the same triangles at the cost of repeating two
 * vertices. They are repeated whole: position, normal, colour and texture
 * coordinate, as they were given, or the repeated ones would be lit and
 * coloured with whatever came last.
 */
struct tmesh_vertex {
	float v[3], n[3], t[2], c[4];
};
static struct tmesh_vertex tmesh_a, tmesh_b;	/* the older, the newer */

static void
tmesh_put(const struct tmesh_vertex *p)
{
	glColor4fv(p->c);
	glNormal3fv(p->n);
	glTexCoord2fv(p->t);
	glVertex3fv(p->v);
}

void
swaptmesh(void)
{
	struct tmesh_vertex t = tmesh_a;
	float c[4];

	tmesh_a = tmesh_b;
	tmesh_b = t;
	tmesh_odd ^= 1;
	glEnd();
	glBegin(GL_TRIANGLE_STRIP);
	if (tmesh_n >= 2) {
		/* A strip's next triangle faces as the count of its vertices so
		 * far is even or odd: the older vertex twice (a triangle with no
		 * area) makes it odd. */
		if (tmesh_odd)
			tmesh_put(&tmesh_a);
		tmesh_put(&tmesh_a);
		tmesh_put(&tmesh_b);
		/* what is current again what it was */
		hgl_current_colour(c);
		glColor4fv(c);
		glNormal3fv(cur_n);
		glTexCoord2fv(cur_t);
	}
}

void
v3f(const float v[3])
{
	tmesh_a = tmesh_b;
	tmesh_b.v[0] = v[0]; tmesh_b.v[1] = v[1]; tmesh_b.v[2] = v[2];
	tmesh_b.n[0] = cur_n[0]; tmesh_b.n[1] = cur_n[1]; tmesh_b.n[2] = cur_n[2];
	tmesh_b.t[0] = cur_t[0]; tmesh_b.t[1] = cur_t[1];
	hgl_current_colour(tmesh_b.c);
	if (tmesh_n < 2)
		tmesh_n++;
	else
		tmesh_odd ^= 1;		/* a triangle was made */
	if (block_verts == 0 && lighting_wanted() != prim_lit) {
		/* nothing drawn yet: begin again, lit or not as it now is */
		glEnd();
		update_lighting_enable();
		glBegin(prim_mode);
	}
	if (block_verts++ < 3)
		hgl_irisgl_tracef("  v %g %g %g n %g %g %g c %g %g %g %g t %g %g", v[0], v[1], v[2],
		    cur_n[0], cur_n[1], cur_n[2], tmesh_b.c[0], tmesh_b.c[1], tmesh_b.c[2], tmesh_b.c[3],
		    cur_t[0], cur_t[1]);
	glVertex3fv(v);
}

void v2f(const float v[2]) { float f[3]; f[0] = v[0]; f[1] = v[1]; f[2] = 0.0f; v3f(f); }
void v2i(const long v[2]) { float f[3]; f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = 0.0f; v3f(f); }
void v2s(const short v[2]) { float f[3]; f[0] = v[0]; f[1] = v[1]; f[2] = 0.0f; v3f(f); }
void v4f(const float v[4]) { glVertex4fv(v); }
void c3f(const float v[3]) { hgl_irisgl_tracef("c3f %g %g %g", v[0], v[1], v[2]); hgl_set_colour(v[0], v[1], v[2], 1.0f); }
void c4f(const float v[4]) { hgl_set_colour(v[0], v[1], v[2], v[3]); }
void t2f(const float v[2]) { cur_t[0] = v[0]; cur_t[1] = v[1]; glTexCoord2fv(v); }
void t2i(const long v[2]) { cur_t[0] = (float)v[0]; cur_t[1] = (float)v[1]; glTexCoord2fv(cur_t); }
void t2s(const short v[2]) { cur_t[0] = v[0]; cur_t[1] = v[1]; glTexCoord2fv(cur_t); }

void
RGBcolor(short r, short g, short b)
{
	hgl_irisgl_tracef("RGBcolor %d %d %d", r, g, b);
	hgl_iris_ensure();
	hgl_set_colour(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

/*
 * Colour-index mode. Everything here is RGB, so an index is looked up in a
 * colour map of our own. It starts as the map an IRIS GL window has on an
 * Indy: SGI's library draws colour-map windows in the X server's default
 * colormap, which Xsgi preloads with its "4sight" map -- the eight base
 * colours, IRIS GL's eight secondary ones at 8-15, sgilightblue at 16 and a
 * 24-step grey ramp at 32-55 -- and leaves every other cell black. Programs
 * use those cells without a mapcolor: clock draws its whole face from the
 * grey ramp, which with any other ramp is a dim smudge.
 *
 * The values were read from SGI's libgl.so on Xsgi (the colormap of clock's
 * and cedit's windows, identical); Xsgi computes the map in the server
 * (init4sight), and no file on the system holds it.
 */
static float cmap[HGL_CMAP_SIZE][3];
static int cmap_ready;
/* The highest index a program has defined (the preloaded ones always
 * count): a search for a colour need not look past it. */
static unsigned long cmap_top = 55;

static void
cmap_set(int i, unsigned long rgb)
{
	cmap[i][0] = (float)((rgb >> 16) & 0xff) / 255.0f;
	cmap[i][1] = (float)((rgb >> 8) & 0xff) / 255.0f;
	cmap[i][2] = (float)(rgb & 0xff) / 255.0f;
}

/*
 * What the X server's default colormap holds now, over the table: SGI's
 * library reads its cells, and they are not only the 4sight map -- the
 * desktop's scheme colours sit past it (from 56 on), and a program written
 * with FORMS reads the whole map with getmcolor and draws its panels in
 * those cells' colours. Only for a colormap visual: a TrueColor one has no
 * cells to read.
 */
static void
cmap_from_server(void)
{
	Display *d = hgl_display();
	Visual *v;
	XColor *x;
	int i, n;

	if (d == NULL)
		return;
	v = DefaultVisual(d, DefaultScreen(d));
	if (v->class != PseudoColor && v->class != GrayScale && v->class != StaticColor &&
	    v->class != StaticGray)
		return;
	n = DisplayCells(d, DefaultScreen(d));
	if (n > HGL_CMAP_SIZE)
		n = HGL_CMAP_SIZE;
	if (n <= 0 || (x = malloc(n * sizeof *x)) == NULL)
		return;
	for (i = 0; i < n; i++)
		x[i].pixel = (unsigned long)i;
	XQueryColors(d, DefaultColormap(d, DefaultScreen(d)), x, n);
	for (i = 0; i < n; i++) {
		cmap[i][0] = (float)x[i].red / 65535.0f;
		cmap[i][1] = (float)x[i].green / 65535.0f;
		cmap[i][2] = (float)x[i].blue / 65535.0f;
		if ((x[i].red | x[i].green | x[i].blue) && (unsigned long)i > cmap_top)
			cmap_top = i;
	}
	free(x);
}

static void
cmap_init(void)
{
	static const unsigned long base[17] = {
		0x000000, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
		0x555555, 0xc67171, 0x71c671, 0x8e8e38, 0x7171c6, 0x8e388e, 0x388e8e, 0xaaaaaa,
		0x7d9ec0
	};
	static const unsigned long ramp[24] = {
		0x0a, 0x14, 0x1e, 0x28, 0x33, 0x3d, 0x47, 0x51, 0x5b, 0x66, 0x70, 0x7a,
		0x84, 0x8e, 0x99, 0xa3, 0xad, 0xb7, 0xc1, 0xcc, 0xd6, 0xe0, 0xea, 0xf4
	};
	int i;

	/* Everything else is black, as an unallocated cell reads on Xsgi. */
	memset(cmap, 0, sizeof cmap);
	for (i = 0; i < 17; i++)
		cmap_set(i, base[i]);
	for (i = 0; i < 24; i++)
		cmap_set(32 + i, ramp[i] << 16 | ramp[i] << 8 | ramp[i]);
	cmap_ready = 1;
	cmap_from_server();
}

void
hgl_cmap_rgb(unsigned long index, unsigned char *rgb)
{
	float *c;

	if (!cmap_ready)
		cmap_init();
	c = cmap[index & (HGL_CMAP_SIZE - 1)];
	rgb[0] = (unsigned char)(c[0] * 255.0f + 0.5f);
	rgb[1] = (unsigned char)(c[1] * 255.0f + 0.5f);
	rgb[2] = (unsigned char)(c[2] * 255.0f + 0.5f);
}

/*
 * The index a pixel read back stands for. The framebuffer holds colours, not
 * indices, so this is the lowest index whose colour is nearest -- exact for
 * anything written through the map, as long as two indices do not share a
 * colour. Remembered for the last colour asked, which is most of an image.
 */
unsigned long
hgl_cmap_index(unsigned char r, unsigned char g, unsigned char b)
{
	static unsigned long last_rgb = 0xffffffffUL, last_index;
	unsigned long key = (unsigned long)r << 16 | (unsigned long)g << 8 | b, best = 0;
	long d, best_d = 0x7fffffffL;
	unsigned char c[3];
	unsigned long i;

	if (key == last_rgb)
		return last_index;
	for (i = 0; i <= cmap_top; i++) {
		hgl_cmap_rgb(i, c);
		d = ((long)c[0] - r) * ((long)c[0] - r) + ((long)c[1] - g) * ((long)c[1] - g)
		    + ((long)c[2] - b) * ((long)c[2] - b);
		if (d < best_d) {
			best_d = d;
			best = i;
			if (d == 0)
				break;
		}
	}
	last_rgb = key;
	last_index = best;
	return best;
}

void
mapcolor(Colorindex i, short r, short g, short b)
{
	hgl_irisgl_tracef("mapcolor %d %d %d %d", i, r, g, b);
	if (hgl_index_mapcolor(i, r, g, b) || hgl_layer_mapcolor(i, r, g, b))
		return;
	if (!cmap_ready)
		cmap_init();
	if (i < HGL_CMAP_SIZE) {
		if (i > cmap_top)
			cmap_top = i;
		cmap[i][0] = r / 255.0f;
		cmap[i][1] = g / 255.0f;
		cmap[i][2] = b / 255.0f;
	}
}

void
color(Colorindex i)
{
	hgl_irisgl_tracef("color %d", i);
	hgl_colour_index = i;
	if (hgl_iris.index_win) {
		/* The index itself, in the OpenGL shim's index mode (see GLXlink):
		 * its low 8 bits as red, the next 4 as green. */
		hgl_set_colour((float)(i & 255) / 255.0f, (float)(i >> 8 & 15) / 255.0f, 0.0f, 1.0f);
		return;
	}
	if (hgl_layer_color(i))
		return;
	if (!cmap_ready)
		cmap_init();
	hgl_iris_ensure();
	i &= HGL_CMAP_SIZE - 1;
	hgl_set_colour(cmap[i][0], cmap[i][1], cmap[i][2], 1.0f);
}

void
czclear(unsigned long c, long z)
{
	float was[4];
	long was_index = hgl_colour_index;
	int was_last = colour_last;

	hgl_irisgl_tracef("czclear %08lx %ld", c, z);
	hgl_iris_ensure();
	if (hgl_selecting)
		return;
	/*
	 * czclear(3G): c is a packed colour in RGB mode and an index in colour
	 * map mode; the current colour does not change; and the writemasks,
	 * like everything else but the screenmask, do not apply.
	 */
	hgl_current_colour(was);
	if (hgl_iris.want_rgb)
		cpack(c);
	else
		color((Colorindex)c);
	glClearColor(cur_r, cur_g, cur_b, cur_a);
	hgl_set_colour(was[0], was[1], was[2], was[3]);
	hgl_colour_index = was_index;
	colour_last = was_last;
	glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	/* z in the screen z units getgdesc(GD_ZMAX) reports. */
	glClearDepth((double)z / (double)0x7fffff);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glPopAttrib();
}

/* In SGI's libGL.so, not declared in IRIX's GL/gl.h. */
extern void glBlendEquationEXT(GLenum mode);
extern void glBlendColorEXT(GLfloat r, GLfloat g, GLfloat b, GLfloat a);

/*
 * blendfunction(3G). The factors are numbered in OpenGL's order, but 2 and 3
 * are the *destination* colour as source factors (BF_DC, BF_MDC) and the
 * *source* colour as destination factors (BF_SC, BF_MSC); 8 is OpenGL's
 * SRC_ALPHA_SATURATE; 9..12 are blendcolor's constant; BF_MIN and BF_MAX
 * (13, 14, as the source factor) are the min and max equations.
 * (BF_ONE, BF_ZERO) is no blending, and any other function turns logicop
 * back to LO_SRC.
 */
void
blendfunction(long src, long dst)
{
	static const GLenum sf[13] = {
		GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
		GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
		GL_SRC_ALPHA_SATURATE, 0x8003 /* CONSTANT_ALPHA */, 0x8004 /* 1 - */,
		0x8001 /* CONSTANT_COLOR */, 0x8002 /* 1 - */
	};
	static const GLenum df[13] = {
		GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
		GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
		GL_ONE, 0x8003, 0x8004, 0x8001, 0x8002
	};

	hgl_irisgl_tracef("blendfunction %ld %ld", src, dst);
	hgl_iris_ensure();
	if (src == BF_ONE && dst == BF_ZERO) {
		hgl_enable(GL_BLEND, 0);
		hgl_iris.blend = 0;
		return;
	}
	if (src == BF_MIN || src == BF_MAX) {
		glBlendEquationEXT(src == BF_MIN ? 0x8007 /* MIN */ : 0x8008 /* MAX */);
		glBlendFunc(GL_ONE, GL_ONE);
	} else {
		glBlendEquationEXT(0x8006 /* FUNC_ADD */);
		glBlendFunc(src >= 0 && src < 13 ? sf[src] : GL_ONE,
		    dst >= 0 && dst < 13 ? df[dst] : GL_ZERO);
	}
	glDisable(GL_COLOR_LOGIC_OP);
	hgl_enable(GL_BLEND, 1);
	hgl_iris.blend = 1;
}

void
blendcolor(float r, float g, float b, float a)
{
	hgl_iris_ensure();
#define CLAMP01(v) ((v) < 0.0f ? 0.0f : (v) > 1.0f ? 1.0f : (v))
	glBlendColorEXT(CLAMP01(r), CLAMP01(g), CLAMP01(b), CLAMP01(a));
#undef CLAMP01
}

void
zfunction(long f)
{
	/* ZF_NEVER..ZF_ALWAYS run in the same order as GL_NEVER..GL_ALWAYS. */
	hgl_iris_ensure();
	glDepthFunc(GL_NEVER + (GLenum)(f & 7));
}

void
shademodel(long m)
{
	hgl_iris_ensure();
	hgl_shade_model = m;
	glShadeModel(m ? GL_SMOOTH : GL_FLAT);
}

void dither(long on) { hgl_iris_ensure(); if (on) glEnable(GL_DITHER); else glDisable(GL_DITHER); }
/* The line width and shade model, as last set, for pushattributes. */
float hgl_line_width = 1.0f;
long hgl_shade_model = GOURAUD;

void linewidth(short w) { hgl_iris_ensure(); hgl_line_width = w > 0 ? w : 1; glLineWidth(hgl_line_width); }
void pntsize(short s) { hgl_iris_ensure(); glPointSize(s > 0 ? s : 1); }

void
linesmooth(unsigned long on)
{
	hgl_irisgl_tracef("linesmooth %lu", on);
	hgl_iris_ensure();
	if (on) {
		/* the program sets the blend function to go with it
		 * (linesmooth(3G)); turning blending on here brought back
		 * whatever function was set last */
		glEnable(GL_LINE_SMOOTH);
	} else {
		glDisable(GL_LINE_SMOOTH);
	}
}

void
pntsmooth(unsigned long on)
{
	hgl_iris_ensure();
	if (on) {
		glEnable(GL_POINT_SMOOTH);
	} else {
		glDisable(GL_POINT_SMOOTH);
	}
}

void polysmooth(long on) { hgl_iris_ensure(); if (on) glEnable(GL_POLYGON_SMOOTH); else glDisable(GL_POLYGON_SMOOTH); }

void multmatrix(const Matrix m) { hgl_iris_ensure(); glMultMatrixf((const GLfloat *)m); }

/* rotate takes tenths of a degree where rot takes degrees. */
void
rotate(Angle a, char axis)
{
	hgl_irisgl_tracef("rotate %d %c", a, axis);
	glRotatef((float)a / 10.0f, axis == 'x' || axis == 'X',
	    axis == 'y' || axis == 'Y', axis == 'z' || axis == 'Z');
}

/*
 * IRIS GL fills every pixel on a rectangle's boundary. OpenGL's fill rule
 * leaves out the ones on two of its edges, which a program drawing in window
 * pixels -- bars, buttons, panels -- shows as a missing row and column. A
 * one-pixel outline over the filled rectangle puts them back, whatever the
 * transformation. Not while blending, where the pixels the fill covered
 * would be blended twice.
 */
void
rectf(Coord x1, Coord y1, Coord x2, Coord y2)
{
	hgl_iris_ensure();
	update_lighting_enable();
	glRectf(x1, y1, x2, y2);
	if (hgl_iris.blend)
		return;
	glPushAttrib(GL_LINE_BIT | GL_POINT_BIT);
	glLineWidth(1.0f);
	glDisable(GL_LINE_STIPPLE);
	glDisable(GL_LINE_SMOOTH);
	glBegin(GL_LINE_LOOP);
	glVertex2f(x1, y1);
	glVertex2f(x2, y1);
	glVertex2f(x2, y2);
	glVertex2f(x1, y2);
	glEnd();
	/* A line leaves out the pixel it ends on, and the host's the one it
	 * starts on too: the corners are points of their own. */
	glPointSize(1.0f);
	glDisable(GL_POINT_SMOOTH);
	glBegin(GL_POINTS);
	glVertex2f(x1, y1);
	glVertex2f(x2, y1);
	glVertex2f(x2, y2);
	glVertex2f(x1, y2);
	glEnd();
	glPopAttrib();
}

void
rect(Coord x1, Coord y1, Coord x2, Coord y2)
{
	hgl_iris_ensure();
	hgl_begin(GL_LINE_LOOP);
	glVertex2f(x1, y1);
	glVertex2f(x2, y1);
	glVertex2f(x2, y2);
	glVertex2f(x1, y2);
	glEnd();
}

/*
 * The integer and short spellings. IRIS GL gives almost every drawing call
 * four of these -- f, i, s and, for some, a double -- and a program that uses
 * pixel coordinates naturally reaches for the integer one. `jot` draws its
 * text area with recti and stopped at it.
 */
void
recti(long x1, long y1, long x2, long y2)
{
	rect((Coord)x1, (Coord)y1, (Coord)x2, (Coord)y2);
}

void
rects(short x1, short y1, short x2, short y2)
{
	rect((Coord)x1, (Coord)y1, (Coord)x2, (Coord)y2);
}

void
rectfi(long x1, long y1, long x2, long y2)
{
	rectf((Coord)x1, (Coord)y1, (Coord)x2, (Coord)y2);
}

void
rectfs(short x1, short y1, short x2, short y2)
{
	rectf((Coord)x1, (Coord)y1, (Coord)x2, (Coord)y2);
}


/*
 * Display lists. IRIS GL names an object with a number the program either
 * chooses or asks genobj for, which is what glGenLists does.
 */
Object genobj(void) { hgl_iris_ensure(); return (Object)glGenLists(1); }
void makeobj(Object o) { hgl_iris_ensure(); glNewList((GLuint)o, GL_COMPILE); }
void closeobj(void) { glEndList(); }
void callobj(Object o) { hgl_irisgl_tracef("callobj %ld", (long)o); hgl_iris_ensure(); glCallList((GLuint)o); }
void delobj(Object o) { hgl_iris_ensure(); glDeleteLists((GLuint)o, 1); }
long isobj(Object o) { hgl_iris_ensure(); return glIsList((GLuint)o); }

/* ---- viewport ---- */

/* The viewport and the screen mask, saved together as pushviewport(3G) says. */
#define VPSTACK 16
static struct { int l, r, b, t; } vpstack[VPSTACK];
static int vpdepth;

void
pushviewport(void)
{
	hgl_iris_ensure();
	glPushAttrib(GL_VIEWPORT_BIT);
	if (vpdepth < VPSTACK) {
		vpstack[vpdepth].l = hgl_iris.mask_l;
		vpstack[vpdepth].r = hgl_iris.mask_r;
		vpstack[vpdepth].b = hgl_iris.mask_b;
		vpstack[vpdepth].t = hgl_iris.mask_t;
	}
	vpdepth++;
}

void
popviewport(void)
{
	hgl_iris_ensure();
	glPopAttrib();
	if (vpdepth > 0 && --vpdepth < VPSTACK) {
		hgl_iris.mask_l = vpstack[vpdepth].l;
		hgl_iris.mask_r = vpstack[vpdepth].r;
		hgl_iris.mask_b = vpstack[vpdepth].b;
		hgl_iris.mask_t = vpstack[vpdepth].t;
		hgl_apply_scrmask();
	}
}

void
getviewport(Screencoord *left, Screencoord *right, Screencoord *bottom, Screencoord *top)
{
	GLint v[4];

	hgl_iris_ensure();
	glGetIntegerv(GL_VIEWPORT, v);
	*left = (Screencoord)v[0];
	*right = (Screencoord)(v[0] + v[2] - 1);
	*bottom = (Screencoord)v[1];
	*top = (Screencoord)(v[1] + v[3] - 1);
}

/* ---- line styles and fill patterns ----
 *
 * Both are defined under a number and selected by it, 0 being solid. A line
 * style is a 16-bit mask, which is exactly OpenGL's stipple. A pattern is a
 * 16x16 or 32x32 mask, bit 15 of each word leftmost, rows bottom to top
 * (defpattern(3G)) -- OpenGL's polygon stipple is 32x32, rows bottom to top,
 * MSB leftmost with LSB-first off, so a 16x16 pattern is tiled into it.
 */
#define MAXSTYLE 64
static unsigned short linestyles[MAXSTYLE];
static short cur_linestyle, cur_pattern;
static long ls_repeat = 1;
static GLubyte patterns[MAXSTYLE][32 * 4];
static unsigned char pattern_set[MAXSTYLE];

void
deflinestyle(short n, Linestyle ls)
{
	if (n > 0 && n < MAXSTYLE)
		linestyles[n] = ls;
}

void
setlinestyle(short n)
{
	hgl_iris_ensure();
	cur_linestyle = n > 0 && n < MAXSTYLE ? n : 0;
	if (n <= 0 || n >= MAXSTYLE || linestyles[n] == 0xffff) {
		glDisable(GL_LINE_STIPPLE);
		return;
	}
	glLineStipple((GLint)ls_repeat, linestyles[n]);
	glEnable(GL_LINE_STIPPLE);
}

void
defpattern(short n, short size, const unsigned short *mask)
{
	int row, col;

	if (n <= 0 || n >= MAXSTYLE || mask == NULL || (size != 16 && size != 32))
		return;
	memset(patterns[n], 0, sizeof patterns[n]);
	for (row = 0; row < 32; row++) {
		for (col = 0; col < 32; col++) {
			int on;
			if (size == 16)
				on = mask[row % 16] >> (15 - col % 16) & 1;
			else
				on = mask[row * 2 + col / 16] >> (15 - col % 16) & 1;
			if (on)
				patterns[n][row * 4 + col / 8] |= 0x80 >> (col % 8);
		}
	}
	pattern_set[n] = 1;
}

void
setpattern(short n)
{
	hgl_iris_ensure();
	cur_pattern = n > 0 && n < MAXSTYLE && pattern_set[n] ? n : 0;
	if (n <= 0 || n >= MAXSTYLE || !pattern_set[n]) {
		glDisable(GL_POLYGON_STIPPLE);
		return;
	}
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glPolygonStipple(patterns[n]);
	glEnable(GL_POLYGON_STIPPLE);
}

long getlstyle(void) { return cur_linestyle; }
long getpattern(void) { return cur_pattern; }
long getlsrepeat(void) { return ls_repeat; }

/* lsrepeat(3G): each bit of the line style covers this many pixels. */
void
lsrepeat(long factor)
{
	ls_repeat = factor < 1 ? 1 : factor > 255 ? 255 : factor;
	if (cur_linestyle)
		setlinestyle(cur_linestyle);
}

/* ---- colour map, read back ---- */

void
getmcolor(Colorindex i, short *r, short *g, short *b)
{
	if (hgl_index_getmcolor(i, r, g, b) || hgl_layer_getmcolor(i, r, g, b))
		return;
	if (!cmap_ready)
		cmap_init();
	i &= HGL_CMAP_SIZE - 1;
	*r = (short)(cmap[i][0] * 255.0f + 0.5f);
	*g = (short)(cmap[i][1] * 255.0f + 0.5f);
	*b = (short)(cmap[i][2] * 255.0f + 0.5f);
	hgl_irisgl_tracef("getmcolor %d -> %d %d %d", i, *r, *g, *b);
}

/* ---- fog ----
 *
 * fogvertex(3G): exponential fog is e^(5.5 * density * Zeye), squared
 * exponential e^(-5.5 * (density * Zeye)^2), linear from start to end, all in
 * eye space -- OpenGL's own fog with the 5.5 folded into the density. The
 * parameters are the density (or start and end) followed by the fog colour.
 */
void
fogvertex(long mode, const float *params)
{
	hgl_iris_ensure();
	switch (mode) {
	case FG_OFF:
		hgl_enable(GL_FOG, 0);
		return;
	case FG_ON:
		hgl_enable(GL_FOG, 1);
		return;
	case FG_VTX_EXP:
	case FG_PIX_EXP:
		glFogi(GL_FOG_MODE, GL_EXP);
		glFogf(GL_FOG_DENSITY, 5.5f * params[0]);
		params += 1;
		break;
	case FG_VTX_EXP2:
	case FG_PIX_EXP2:
		glFogi(GL_FOG_MODE, GL_EXP2);
		glFogf(GL_FOG_DENSITY, (float)sqrt(5.5) * params[0]);
		params += 1;
		break;
	case FG_VTX_LIN:
	case FG_PIX_LIN:
		glFogi(GL_FOG_MODE, GL_LINEAR);
		glFogf(GL_FOG_START, params[0]);
		glFogf(GL_FOG_END, params[1]);
		params += 2;
		break;
	default:
		return;
	}
	{
		GLfloat c[4];
		c[0] = params[0];
		c[1] = params[1];
		c[2] = params[2];
		c[3] = 1.0f;
		glFogfv(GL_FOG_COLOR, c);
	}
	glHint(GL_FOG_HINT, mode == FG_PIX_EXP || mode == FG_PIX_EXP2 || mode == FG_PIX_LIN
	    ? GL_NICEST : GL_FASTEST);
}

/* ---- buffers, and what a window reports ---- */

void
frontbuffer(Boolean b)
{
	hgl_iris.front = b != 0;
	if (b && hgl_iris.opened && hgl_iris.ctx)
		glXSwapBuffers(hgl_iris.gldpy, hgl_iris.win);
}

long getplanes(void) { return hgl_iris.want_rgb ? 8 : 12; }

long
getdisplaymode(void)
{
	if (hgl_iris.want_rgb)
		return hgl_iris.want_double ? DMRGBDOUBLE : DMRGB;
	return hgl_iris.want_double ? DMDOUBLE : DMSINGLE;
}

void
afunction(long ref, long func)
{
	static const GLenum f[8] = {
		GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
	};
	hgl_iris_ensure();
	if (func < 0 || func > 7)
		return;
	glAlphaFunc(f[func], ref / 255.0f);
	if (func == AF_ALWAYS)
		hgl_enable(GL_ALPHA_TEST, 0);
	else
		hgl_enable(GL_ALPHA_TEST, 1);
}

void pntsizef(float n) { hgl_iris_ensure(); glPointSize(n); }

/* ---- the window, from the program's side ---- */

void
wintitle(String name)
{
	strncpy(hgl_iris.title, name ? name : "", sizeof hgl_iris.title - 1);
	if (hgl_iris.opened && !hgl_iris.glx) {
		XStoreName(hgl_iris.gldpy, hgl_iris.win, hgl_iris.title);
		XFlush(hgl_iris.gldpy);
	}
}

long qgetfd(void) { return ConnectionNumber(hgl_display()); }

void
qreset(void)
{
	hgl_iris.qhead = hgl_iris.qtail;
}
