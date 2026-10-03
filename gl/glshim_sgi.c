/*
 * Host OpenGL for IRIS: the rest of the gl* entry points SGI's libGLcore.so
 * exports.
 *
 * SGI's libGL.so is GLX alone; the GL functions are in libGLcore.so, which
 * drives the graphics board itself. A program linked against both binds its
 * gl* calls to whichever comes first in its library list, so with
 * libGLcore.so first it would draw through SGI's driver while GLX is ours.
 * The install puts a libGLcore.so in place that has no functions of its own
 * and brings this library in (glcore_stub.c), so every gl* call ends here:
 * this file has the ones libGLcore.so exports that the rest of the shim does
 * not.
 *
 * None of these extensions is in the extension string (that is the host's),
 * so a program that asks before using one does not use it. For the others:
 *
 *   SGIS_multitexture       the ARB_multitexture calls, with SGIS's unit names
 *   EXT_point_parameters    SGIS_point_parameters, which has the same enums
 *   SGIX_async              markers kept here; everything is finished at once,
 *                           since nothing here runs asynchronously
 *   SGIX_fragment_lighting, the parameters kept here, so that what is set
 *   SGIS_pixel_texture,     reads back, with no effect on drawing (fragment
 *   SGI_pixel_transform     lighting would need per-pixel lighting the host's
 *                           fixed-function pipeline does not have)
 *
 * and the entry points no SGI header declares (coordinate frames, NURBS and
 * subdivision evaluation, texture lighting, the IGLOO interface, ...) accept
 * whatever they are given and do nothing.
 */
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include "glshim.h"
#include "glshim_rt.h"

/* ARB_multitexture and SGIS_point_parameters, which this image's gl.h may
 * not declare (the rest of the shim defines them). */
extern void glClientActiveTextureARB(GLenum texture);
extern void glMultiTexCoord1dARB(GLenum target, GLdouble s);
extern void glMultiTexCoord1fARB(GLenum target, GLfloat s);
extern void glMultiTexCoord1iARB(GLenum target, GLint s);
extern void glMultiTexCoord1sARB(GLenum target, GLshort s);
extern void glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t);
extern void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t);
extern void glMultiTexCoord2iARB(GLenum target, GLint s, GLint t);
extern void glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t);
extern void glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r);
extern void glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r);
extern void glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r);
extern void glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r);
extern void glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q);
extern void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
extern void glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q);
extern void glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q);
extern void glPointParameterfSGIS(GLenum pname, GLfloat param);
extern void glPointParameterfvSGIS(GLenum pname, const GLfloat *params);

#define HGL_TEXTURE0_ARB 0x84C0
#define HGL_TEXTURE0_SGIS 0x835E
#define HGL_TEXTURE1_SGIS 0x835F

/* SGIS_multitexture's unit names as ARB_multitexture's. */
static GLenum
unit(GLenum target)
{
	if (target == HGL_TEXTURE0_SGIS || target == HGL_TEXTURE1_SGIS)
		return HGL_TEXTURE0_ARB + (target - HGL_TEXTURE0_SGIS);
	return target;
}

/* SGIS_multitexture */

void glMultiTexCoord1dSGIS(GLenum t, GLdouble s) { glMultiTexCoord1dARB(unit(t), s); }
void glMultiTexCoord1fSGIS(GLenum t, GLfloat s) { glMultiTexCoord1fARB(unit(t), s); }
void glMultiTexCoord1iSGIS(GLenum t, GLint s) { glMultiTexCoord1iARB(unit(t), s); }
void glMultiTexCoord1sSGIS(GLenum t, GLshort s) { glMultiTexCoord1sARB(unit(t), s); }
void glMultiTexCoord2dSGIS(GLenum t, GLdouble s, GLdouble u) { glMultiTexCoord2dARB(unit(t), s, u); }
void glMultiTexCoord2fSGIS(GLenum t, GLfloat s, GLfloat u) { glMultiTexCoord2fARB(unit(t), s, u); }
void glMultiTexCoord2iSGIS(GLenum t, GLint s, GLint u) { glMultiTexCoord2iARB(unit(t), s, u); }
void glMultiTexCoord2sSGIS(GLenum t, GLshort s, GLshort u) { glMultiTexCoord2sARB(unit(t), s, u); }
void glMultiTexCoord3dSGIS(GLenum t, GLdouble s, GLdouble u, GLdouble r) { glMultiTexCoord3dARB(unit(t), s, u, r); }
void glMultiTexCoord3fSGIS(GLenum t, GLfloat s, GLfloat u, GLfloat r) { glMultiTexCoord3fARB(unit(t), s, u, r); }
void glMultiTexCoord3iSGIS(GLenum t, GLint s, GLint u, GLint r) { glMultiTexCoord3iARB(unit(t), s, u, r); }
void glMultiTexCoord3sSGIS(GLenum t, GLshort s, GLshort u, GLshort r) { glMultiTexCoord3sARB(unit(t), s, u, r); }
void glMultiTexCoord4dSGIS(GLenum t, GLdouble s, GLdouble u, GLdouble r, GLdouble q) { glMultiTexCoord4dARB(unit(t), s, u, r, q); }
void glMultiTexCoord4fSGIS(GLenum t, GLfloat s, GLfloat u, GLfloat r, GLfloat q) { glMultiTexCoord4fARB(unit(t), s, u, r, q); }
void glMultiTexCoord4iSGIS(GLenum t, GLint s, GLint u, GLint r, GLint q) { glMultiTexCoord4iARB(unit(t), s, u, r, q); }
void glMultiTexCoord4sSGIS(GLenum t, GLshort s, GLshort u, GLshort r, GLshort q) { glMultiTexCoord4sARB(unit(t), s, u, r, q); }

/* The coordinate array of one unit, leaving the client unit as it was. */
void
glMultiTexCoordPointerSGIS(GLenum target, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	int was = hgl_client_unit_now();
	GLenum u = unit(target);

	if (u == HGL_TEXTURE0_ARB + was) {
		glTexCoordPointer(size, type, stride, pointer);
		return;
	}
	glClientActiveTextureARB(u);
	glTexCoordPointer(size, type, stride, pointer);
	glClientActiveTextureARB(HGL_TEXTURE0_ARB + was);
}

/* EXT_point_parameters: SGIS_point_parameters' calls, enums and all. */

void
glPointParameterfEXT(GLenum pname, GLfloat param)
{
	glPointParameterfSGIS(pname, param);
}

void
glPointParameterfvEXT(GLenum pname, const GLfloat *params)
{
	glPointParameterfvSGIS(pname, params);
}

/*
 * SGIX_async. Markers are names, handed out in ranges as display lists are.
 * A marker set with glAsyncMarkerSGIX names the asynchronous commands that
 * follow; none is asynchronous here, so each is finished as soon as it is
 * set, and the next glFinishAsyncSGIX or glPollAsyncSGIX reports it.
 */
#define HGL_ASYNC_MAX 256
static GLuint async_done[HGL_ASYNC_MAX];
static int async_ndone;
static GLuint async_next = 1;
static struct { GLuint first; GLsizei n; } async_ranges[HGL_ASYNC_MAX];
static int async_nranges;

GLuint
glGenAsyncMarkersSGIX(GLsizei range)
{
	GLuint first = async_next;

	if (range <= 0 || async_nranges == HGL_ASYNC_MAX)
		return 0;
	async_ranges[async_nranges].first = first;
	async_ranges[async_nranges].n = range;
	async_nranges++;
	async_next += range;
	return first;
}

void
glDeleteAsyncMarkersSGIX(GLuint marker, GLsizei range)
{
	int i, j;

	for (i = 0; i < async_nranges; i++) {
		GLuint f = async_ranges[i].first, l = f + async_ranges[i].n;
		if (marker >= l || marker + range <= f)
			continue;
		/* Deleting part of a range forgets all of it: names are cheap. */
		async_ranges[i] = async_ranges[--async_nranges];
		i--;
	}
	for (i = j = 0; i < async_ndone; i++)
		if (async_done[i] < marker || async_done[i] >= marker + range)
			async_done[j++] = async_done[i];
	async_ndone = j;
}

GLboolean
glIsAsyncMarkerSGIX(GLuint marker)
{
	int i;

	for (i = 0; i < async_nranges; i++)
		if (marker >= async_ranges[i].first &&
		    marker < async_ranges[i].first + async_ranges[i].n)
			return GL_TRUE;
	return GL_FALSE;
}

void
glAsyncMarkerSGIX(GLuint marker)
{
	if (marker != 0 && async_ndone < HGL_ASYNC_MAX)
		async_done[async_ndone++] = marker;
}

GLint
glFinishAsyncSGIX(GLuint *markerp)
{
	int i;

	if (async_ndone == 0)
		return 0;
	*markerp = async_done[0];
	for (i = 1; i < async_ndone; i++)
		async_done[i - 1] = async_done[i];
	async_ndone--;
	return 1;
}

GLint
glPollAsyncSGIX(GLuint *markerp)
{
	return glFinishAsyncSGIX(markerp);
}

/*
 * Parameters kept so that they read back: by object (a light, a face, or
 * 0) and parameter name, up to four values each.
 */
#define HGL_PARAMS 128
static struct param {
	GLenum kind, obj, pname;
	GLfloat v[4];
} params[HGL_PARAMS];
static int nparams;

/* How many values a parameter has: four for colours and positions, three
 * for directions, one otherwise. */
static int
param_count(GLenum pname)
{
	switch (pname) {
	case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION:
	case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
	case 0x840A:	/* FRAGMENT_LIGHT_MODEL_AMBIENT_SGIX */
		return 4;
	case GL_SPOT_DIRECTION:
		return 3;
	case GL_COLOR_INDEXES:
		return 3;
	}
	return 1;
}

static struct param *
param_find(GLenum kind, GLenum obj, GLenum pname, int make)
{
	int i;

	for (i = 0; i < nparams; i++)
		if (params[i].kind == kind && params[i].obj == obj && params[i].pname == pname)
			return &params[i];
	if (!make || nparams == HGL_PARAMS)
		return NULL;
	params[nparams].kind = kind;
	params[nparams].obj = obj;
	params[nparams].pname = pname;
	memset(params[nparams].v, 0, sizeof params[nparams].v);
	return &params[nparams++];
}

static void
param_setf(GLenum kind, GLenum obj, GLenum pname, const GLfloat *v, int n)
{
	struct param *p = param_find(kind, obj, pname, 1);
	int i;

	if (p == NULL)
		return;
	for (i = 0; i < n && i < 4; i++)
		p->v[i] = v[i];
}

static void
param_seti(GLenum kind, GLenum obj, GLenum pname, const GLint *v, int n)
{
	GLfloat f[4];
	int i;

	for (i = 0; i < n && i < 4; i++)
		f[i] = (GLfloat)v[i];
	param_setf(kind, obj, pname, f, n);
}

static void
param_getf(GLenum kind, GLenum obj, GLenum pname, GLfloat *v)
{
	struct param *p = param_find(kind, obj, pname, 0);
	int i, n = param_count(pname);

	for (i = 0; i < n; i++)
		v[i] = p ? p->v[i] : 0.0f;
}

static void
param_geti(GLenum kind, GLenum obj, GLenum pname, GLint *v)
{
	GLfloat f[4];
	int i, n = param_count(pname);

	param_getf(kind, obj, pname, f);
	for (i = 0; i < n; i++)
		v[i] = (GLint)f[i];
}

enum { K_FLIGHT = 1, K_FMATERIAL, K_FMODEL, K_LIGHTENV, K_PIXTEX, K_PIXXFORM };

/* SGIX_fragment_lighting */

void glFragmentLightfSGIX(GLenum l, GLenum p, GLfloat v) { param_setf(K_FLIGHT, l, p, &v, 1); }
void glFragmentLightfvSGIX(GLenum l, GLenum p, const GLfloat *v) { param_setf(K_FLIGHT, l, p, v, param_count(p)); }
void glFragmentLightiSGIX(GLenum l, GLenum p, GLint v) { param_seti(K_FLIGHT, l, p, &v, 1); }
void glFragmentLightivSGIX(GLenum l, GLenum p, const GLint *v) { param_seti(K_FLIGHT, l, p, v, param_count(p)); }
void glGetFragmentLightfvSGIX(GLenum l, GLenum p, GLfloat *v) { param_getf(K_FLIGHT, l, p, v); }
void glGetFragmentLightivSGIX(GLenum l, GLenum p, GLint *v) { param_geti(K_FLIGHT, l, p, v); }
void glFragmentMaterialfSGIX(GLenum f, GLenum p, GLfloat v) { param_setf(K_FMATERIAL, f, p, &v, 1); }
void glFragmentMaterialfvSGIX(GLenum f, GLenum p, const GLfloat *v) { param_setf(K_FMATERIAL, f, p, v, param_count(p)); }
void glFragmentMaterialiSGIX(GLenum f, GLenum p, GLint v) { param_seti(K_FMATERIAL, f, p, &v, 1); }
void glFragmentMaterialivSGIX(GLenum f, GLenum p, const GLint *v) { param_seti(K_FMATERIAL, f, p, v, param_count(p)); }
void glGetFragmentMaterialfvSGIX(GLenum f, GLenum p, GLfloat *v) { param_getf(K_FMATERIAL, f, p, v); }
void glGetFragmentMaterialivSGIX(GLenum f, GLenum p, GLint *v) { param_geti(K_FMATERIAL, f, p, v); }
void glFragmentLightModelfSGIX(GLenum p, GLfloat v) { param_setf(K_FMODEL, 0, p, &v, 1); }
void glFragmentLightModelfvSGIX(GLenum p, const GLfloat *v) { param_setf(K_FMODEL, 0, p, v, param_count(p)); }
void glFragmentLightModeliSGIX(GLenum p, GLint v) { param_seti(K_FMODEL, 0, p, &v, 1); }
void glFragmentLightModelivSGIX(GLenum p, const GLint *v) { param_seti(K_FMODEL, 0, p, v, param_count(p)); }
void glFragmentColorMaterialSGIX(GLenum face, GLenum mode) { GLint m = (GLint)mode; param_seti(K_FMATERIAL, face, 0, &m, 1); }
void glLightEnviSGIX(GLenum p, GLint v) { param_seti(K_LIGHTENV, 0, p, &v, 1); }

/* SGIS_pixel_texture */

void glPixelTexGenParameterfSGIS(GLenum p, GLfloat v) { param_setf(K_PIXTEX, 0, p, &v, 1); }
void glPixelTexGenParameterfvSGIS(GLenum p, const GLfloat *v) { param_setf(K_PIXTEX, 0, p, v, 1); }
void glPixelTexGenParameteriSGIS(GLenum p, GLint v) { param_seti(K_PIXTEX, 0, p, &v, 1); }
void glPixelTexGenParameterivSGIS(GLenum p, const GLint *v) { param_seti(K_PIXTEX, 0, p, v, 1); }
void glGetPixelTexGenParameterfvSGIS(GLenum p, GLfloat *v) { param_getf(K_PIXTEX, 0, p, v); }
void glGetPixelTexGenParameterivSGIS(GLenum p, GLint *v) { param_geti(K_PIXTEX, 0, p, v); }

/* SGI_pixel_transform: kept by target and parameter, one value each. */

void glPixelTransformParameterfSGI(GLenum t, GLenum p, GLfloat v) { param_setf(K_PIXXFORM, t, p, &v, 1); }
void glPixelTransformParameterfvSGI(GLenum t, GLenum p, const GLfloat *v) { param_setf(K_PIXXFORM, t, p, v, 1); }
void glPixelTransformParameteriSGI(GLenum t, GLenum p, GLint v) { param_seti(K_PIXXFORM, t, p, &v, 1); }
void glPixelTransformParameterivSGI(GLenum t, GLenum p, const GLint *v) { param_seti(K_PIXXFORM, t, p, v, 1); }
void glGetPixelTransformParameterfvSGI(GLenum t, GLenum p, GLfloat *v) { param_getf(K_PIXXFORM, t, p, v); }
void glGetPixelTransformParameterivSGI(GLenum t, GLenum p, GLint *v) { param_geti(K_PIXXFORM, t, p, v); }
void glPixelTransformSGI(GLenum t) { (void)t; }

/* SGIS_fog_function: the points are the host's, which has no such fog;
 * none come back. */
/* (const: 6.5.22's gl.h declares it so) */
void glGetFogFuncSGIS(const GLfloat *points) { (void)points; }

/* SGIS_texture_color_mask: the host masks colours as a whole. */
void glTextureColorMaskSGIS(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; }

/* SGIX_igloo_interface: SGI's IRIS GL (IGLOO) talking to its own OpenGL;
 * this shim's IRIS GL does not use it. */
void glIglooInterfaceSGIX(GLenum pname, const GLvoid *p) { (void)pname; (void)p; }

/*
 * Entry points no SGI header declares, whose arguments are therefore not
 * known here: each takes whatever it is given and does nothing. A MIPS
 * callee that ignores its arguments is right whatever they were.
 */
#define HGL_IGNORED(name) void name(void) {}
HGL_IGNORED(glApplyTextureSGIX)
HGL_IGNORED(glFogLayersSGIX)
HGL_IGNORED(glFragmentLightSpaceSGIX)
HGL_IGNORED(glNurbsKnots1dSGIX)
HGL_IGNORED(glNurbsKnots1fSGIX)
HGL_IGNORED(glNurbsKnots2dSGIX)
HGL_IGNORED(glNurbsKnots2fSGIX)
HGL_IGNORED(glSubdivPatchParameterfSGIX)
HGL_IGNORED(glSubdivPatchParameteriSGIX)
HGL_IGNORED(glSubdivPatchSGIX)
HGL_IGNORED(glTextureLightSGIX)
HGL_IGNORED(glTextureMaterialSGIX)
/* The coordinate frame (tangent and binormal per vertex): only bump mapping
 * and fragment lighting would read them. */
HGL_IGNORED(glBinormal3bSGIX)
HGL_IGNORED(glBinormal3bvSGIX)
HGL_IGNORED(glBinormal3dSGIX)
HGL_IGNORED(glBinormal3dvSGIX)
HGL_IGNORED(glBinormal3fSGIX)
HGL_IGNORED(glBinormal3fvSGIX)
HGL_IGNORED(glBinormal3iSGIX)
HGL_IGNORED(glBinormal3ivSGIX)
HGL_IGNORED(glBinormal3sSGIX)
HGL_IGNORED(glBinormal3svSGIX)
HGL_IGNORED(glTangent3bSGIX)
HGL_IGNORED(glTangent3bvSGIX)
HGL_IGNORED(glTangent3dSGIX)
HGL_IGNORED(glTangent3dvSGIX)
HGL_IGNORED(glTangent3fSGIX)
HGL_IGNORED(glTangent3fvSGIX)
HGL_IGNORED(glTangent3iSGIX)
HGL_IGNORED(glTangent3ivSGIX)
HGL_IGNORED(glTangent3sSGIX)
HGL_IGNORED(glTangent3svSGIX)
