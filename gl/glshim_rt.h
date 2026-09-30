/*
 * Host OpenGL for IRIS: the shim's own runtime, shared by the generated encoders
 * (glshim_gen.c) and the hand-written parts (glshim_rt.c, glshim_glx.c).
 */
#ifndef HGL_GLSHIM_RT_H
#define HGL_GLSHIM_RT_H

#include <GL/gl.h>

/* Client arrays, as glArrayElement and glGetPointerv need them. */
#define HGL_ARRAY_VERTEX   0
#define HGL_ARRAY_NORMAL   1
#define HGL_ARRAY_COLOR    2
#define HGL_ARRAY_INDEX    3
#define HGL_ARRAY_TEXCOORD 4
#define HGL_ARRAY_EDGEFLAG 5

/* Texture units with their own recorded coordinate array (the host's count). */
#define HGL_TEX_UNITS 8

extern void hgl_array_pointer(int which, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
extern void hgl_client_unit(GLenum target);
/* glDrawBuffer, per context: has the front buffer been drawn into since the
 * window was last presented? hgl_front_drawn(1) answers and forgets. */
extern void hgl_draw_buffer(GLenum mode);
extern int hgl_front_drawn(int clear);
extern void hgl_client_state(GLenum array, int on);
extern void hgl_interleaved(GLenum format, GLsizei stride, const GLvoid *pointer);
extern GLfloat *hgl_feedback_pointer;
extern GLuint *hgl_select_pointer;

/*
 * What the host will read for an upload or a draw (glshim.h, hgl_ref). The
 * host call reads it itself, so these record nothing.
 */
extern void hgl_ref_image(const GLvoid *pixels, GLenum format, GLenum type, int bitmap, int ndims,
    GLint d0, GLint d1, GLint d2, GLint d3);
extern void hgl_ref_draw_arrays(GLint first, GLsizei count);
extern void hgl_ref_draw_elements(GLsizei count, GLenum type, const GLvoid *indices);
/* glPixelStore and the client attribute stack, reported the same way. */
extern void hgl_pixel_store(GLenum pname, double value);
extern void hgl_push_client_attrib(GLbitfield mask);
extern void hgl_pop_client_attrib(void);
extern void hgl_client_select(void *ctx);
extern void hgl_client_forget(void *ctx);

/* Colour-index contexts (glshim_rt.c): nonzero while one is current, which
 * the encoders of the index calls ask. hgl_client_index says whether the
 * context just selected is one. */
extern int hgl_index_mode;
extern int hgl_index_bits;
extern void hgl_client_index(int on);
extern void hgl_index(double v);
extern void hgl_clear_index(GLfloat c);
extern void hgl_index_mask(GLuint mask);
extern void hgl_index_geti(GLenum pname, GLint *params);
extern void hgl_index_getb(GLenum pname, GLboolean *params);
extern void hgl_index_getf(GLenum pname, GLfloat *params);
extern void hgl_index_getd(GLenum pname, GLdouble *params);

/* Size rules the encoders share. */
extern unsigned hgl_calllists_bytes(GLenum type);
extern unsigned hgl_map_components(GLenum target);

#define hgl_nonneg(v) ((v) > 0 ? (unsigned)(v) : 0u)

#endif
