/*
 * The X server's GLX visual configurations, as SGI's libGL reads them
 * (glshim_glxcfg.c): what glXGetConfig answers and glXChooseVisual chooses
 * from.
 */
#ifndef GLSHIM_GLXCFG_H
#define GLSHIM_GLXCFG_H

#include <X11/Xlib.h>

#define HGL_CFG_TAGS 16

struct hgl_glxcfg {
	VisualID id;
	int class;              /* the X visual class the server gives */
	int rgba, red, green, blue, alpha;
	int accum_red, accum_green, accum_blue, accum_alpha;
	int dbl, stereo, buffer, depth, stencil, aux, level;
	int ntags;              /* the tagged properties: GLX_*_EXT, SGIS ... */
	int tags[HGL_CFG_TAGS][2];
};

/*
 * The configurations of `screen`, read from the server once per display and
 * screen; *n is their number. NULL when the server answers none (no GLX):
 * then the callers keep their own rules.
 */
extern const struct hgl_glxcfg *hgl_glx_configs(Display *dpy, int screen, int *n);

/* The configuration of one visual, or NULL. */
extern const struct hgl_glxcfg *hgl_glx_config(Display *dpy, int screen, VisualID id);

/*
 * The server's GLX 1.3 FBConfigs (X_GLXGetFBConfigs): each an attribute list,
 * GLX_FBCONFIG_ID, GLX_VISUAL_ID, GLX_RENDER_TYPE, GLX_DRAWABLE_TYPE and
 * the rest, as the server gives them. NULL when it gives none.
 */
#define HGL_FB_ATTRS 48

struct hgl_fbcfg {
	int nattr;
	int attr[HGL_FB_ATTRS][2];
};

extern const struct hgl_fbcfg *hgl_glx_fbconfigs(Display *dpy, int screen, int *n);

/* One attribute of an FBConfig: 1 and *value, or 0 when it has none. */
extern int hgl_fbcfg_attr(const struct hgl_fbcfg *c, int attr, int *value);

/* The server's GLX strings (1 vendor, 2 version, 3 extensions), or NULL. */
extern const char *hgl_glx_server_string(Display *dpy, int screen, int name);

#endif
