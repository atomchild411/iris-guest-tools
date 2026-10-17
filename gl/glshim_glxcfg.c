/*
 * The X server's GLX visual configurations.
 *
 * SGI's libGL does not decide what its visuals are: it asks the X server,
 * whose GLX extension (SGI's own, loaded from libGLcore.so even when this
 * library stands in for libGL) answers X_GLXGetVisualConfigs with a table,
 * one row per visual: the 18 properties of GLX 1.2 -- visual id, class,
 * RGBA, the colour, accumulation, depth and stencil sizes, double buffer,
 * stereo, buffer size, aux buffers, level -- then tagged pairs (visual
 * caveat, transparency, multisampling, visual select group). Its
 * glXGetConfig answers from that table, and so does this one: every answer
 * SGI's library gave IRIS GL (IGLOO) for every IMPACT visual, 447 of them,
 * is in it. Answers made up here instead told SGI's IRIS GL that colour-index
 * visuals had depth buffers and left out the 12-bit RGBA visuals, and it
 * chose windows the server would not create.
 *
 * Xlibint.h's macros are kept to this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlibint.h>
#include "glshim_glxcfg.h"

#define X_GLXGetVisualConfigs 14
#define X_GLXQueryServerString 19
#define X_GLXGetFBConfigs 21
#define FIXED_PROPS 18

typedef struct {
	CARD8 reqType;
	CARD8 glxCode;
	CARD16 length B16;
	CARD32 screen B32;
} xGLXGetVisualConfigsReq;   /* and X_GLXGetFBConfigs' */

typedef struct {
	CARD8 reqType;
	CARD8 glxCode;
	CARD16 length B16;
	CARD32 screen B32;
	CARD32 name B32;
} xGLXQueryServerStringReq;

/* A request of `size` bytes in the display's buffer; locked by the caller. */
static void *
new_req(Display *dpy, int size)
{
	void *r;

	if (dpy->bufptr + size > dpy->bufmax)
		_XFlush(dpy);
	r = dpy->bufptr;
	dpy->bufptr += size;
	dpy->request++;
	dpy->last_req = (char *)r;
	return r;
}

#define HGL_CFG_MAX 96

static Display *cfg_dpy;
static int cfg_screen = -1;
static int cfg_n = -1;
static struct hgl_glxcfg cfg[HGL_CFG_MAX];

/* The table from the server, into cfg; the number of rows, 0 on failure. */
static int
read_configs(Display *dpy, int screen)
{
	int major, ev, err, i, j, n;
	xGLXGetVisualConfigsReq *req;
	xReply rep;
	unsigned long nvis, nprops, bytes;
	CARD32 *props;

	if (!XQueryExtension(dpy, "GLX", &major, &ev, &err))
		return 0;
	LockDisplay(dpy);
	if (dpy->bufptr + sizeof *req > dpy->bufmax)
		_XFlush(dpy);
	req = (xGLXGetVisualConfigsReq *)dpy->bufptr;
	dpy->bufptr += sizeof *req;
	dpy->request++;
	dpy->last_req = (char *)req;
	req->reqType = major;
	req->glxCode = X_GLXGetVisualConfigs;
	req->length = sizeof *req / 4;
	req->screen = screen;
	if (!_XReply(dpy, &rep, 0, False)) {
		UnlockDisplay(dpy);
		SyncHandle();
		return 0;
	}
	nvis = rep.generic.data00;
	nprops = rep.generic.data01;
	bytes = nvis * nprops * 4;
	if (nprops < FIXED_PROPS || bytes > (unsigned long)rep.generic.length * 4 ||
	    (props = (CARD32 *)malloc(bytes)) == NULL) {
		_XEatData(dpy, (unsigned long)rep.generic.length * 4);
		UnlockDisplay(dpy);
		SyncHandle();
		return 0;
	}
	_XRead(dpy, (char *)props, bytes);
	if ((unsigned long)rep.generic.length * 4 > bytes)
		_XEatData(dpy, (unsigned long)rep.generic.length * 4 - bytes);
	UnlockDisplay(dpy);
	SyncHandle();
	for (i = n = 0; i < (int)nvis && n < HGL_CFG_MAX; i++, n++) {
		const CARD32 *p = props + i * nprops;
		struct hgl_glxcfg *c = &cfg[n];

		c->id = p[0];
		c->class = (int)p[1];
		c->rgba = (int)p[2];
		c->red = (int)p[3];
		c->green = (int)p[4];
		c->blue = (int)p[5];
		c->alpha = (int)p[6];
		c->accum_red = (int)p[7];
		c->accum_green = (int)p[8];
		c->accum_blue = (int)p[9];
		c->accum_alpha = (int)p[10];
		c->dbl = (int)p[11];
		c->stereo = (int)p[12];
		c->buffer = (int)p[13];
		c->depth = (int)p[14];
		c->stencil = (int)p[15];
		c->aux = (int)p[16];
		c->level = (int)p[17];
		c->ntags = 0;
		for (j = FIXED_PROPS; j + 1 < (int)nprops && c->ntags < HGL_CFG_TAGS; j += 2) {
			c->tags[c->ntags][0] = (int)p[j];
			c->tags[c->ntags][1] = (int)p[j + 1];
			c->ntags++;
		}
	}
	free(props);
	return n;
}

const struct hgl_glxcfg *
hgl_glx_configs(Display *dpy, int screen, int *n)
{
	if (dpy == NULL) {
		*n = 0;
		return NULL;
	}
	if (dpy != cfg_dpy || screen != cfg_screen || cfg_n < 0) {
		cfg_dpy = dpy;
		cfg_screen = screen;
		cfg_n = read_configs(dpy, screen);
		if (getenv("IRIS_GL_DEBUG") != NULL)
			fprintf(stderr, "libGL: the X server describes %d GLX visuals\n", cfg_n);
	}
	*n = cfg_n;
	return cfg_n > 0 ? cfg : NULL;
}

const struct hgl_glxcfg *
hgl_glx_config(Display *dpy, int screen, VisualID id)
{
	const struct hgl_glxcfg *c;
	int i, n;

	c = hgl_glx_configs(dpy, screen, &n);
	for (i = 0; c != NULL && i < n; i++)
		if (c[i].id == id)
			return &c[i];
	return NULL;
}

/*
 * The server's GLX 1.3 FBConfigs and its strings, read once per display and
 * screen like the visual table. SGI's glxinfo lists FBConfigs, not visuals:
 * this library made one of each 24-bit TrueColor visual, so it listed 8 of
 * IMPACT's 26 configurations.
 */
#define HGL_FB_MAX 96

static Display *fb_dpy;
static int fb_screen = -1;
static int fb_n = -1;
static struct hgl_fbcfg fb[HGL_FB_MAX];
static char *srv_str[4];

static int
read_fbconfigs(Display *dpy, int screen)
{
	int major, ev, err, i, j, n;
	xGLXGetVisualConfigsReq *req;
	xReply rep;
	unsigned long nfb, nattr, bytes;
	CARD32 *a;

	if (!XQueryExtension(dpy, "GLX", &major, &ev, &err))
		return 0;
	LockDisplay(dpy);
	req = (xGLXGetVisualConfigsReq *)new_req(dpy, sizeof *req);
	req->reqType = major;
	req->glxCode = X_GLXGetFBConfigs;
	req->length = sizeof *req / 4;
	req->screen = screen;
	if (!_XReply(dpy, &rep, 0, False)) {
		UnlockDisplay(dpy);
		SyncHandle();
		return 0;
	}
	nfb = rep.generic.data00;
	nattr = rep.generic.data01;
	bytes = nfb * nattr * 8;
	if (bytes > (unsigned long)rep.generic.length * 4 || (a = (CARD32 *)malloc(bytes + 4)) == NULL) {
		_XEatData(dpy, (unsigned long)rep.generic.length * 4);
		UnlockDisplay(dpy);
		SyncHandle();
		return 0;
	}
	_XRead(dpy, (char *)a, bytes);
	if ((unsigned long)rep.generic.length * 4 > bytes)
		_XEatData(dpy, (unsigned long)rep.generic.length * 4 - bytes);
	UnlockDisplay(dpy);
	SyncHandle();
	for (i = n = 0; i < (int)nfb && n < HGL_FB_MAX; i++, n++) {
		fb[n].nattr = 0;
		for (j = 0; j < (int)nattr && fb[n].nattr < HGL_FB_ATTRS; j++) {
			fb[n].attr[fb[n].nattr][0] = (int)a[(i * nattr + j) * 2];
			fb[n].attr[fb[n].nattr][1] = (int)a[(i * nattr + j) * 2 + 1];
			fb[n].nattr++;
		}
	}
	free(a);
	return n;
}

static char *
read_string(Display *dpy, int screen, int name)
{
	int major, ev, err;
	xGLXQueryServerStringReq *req;
	xReply rep;
	unsigned long bytes;
	char *s;

	if (!XQueryExtension(dpy, "GLX", &major, &ev, &err))
		return NULL;
	LockDisplay(dpy);
	req = (xGLXQueryServerStringReq *)new_req(dpy, sizeof *req);
	req->reqType = major;
	req->glxCode = X_GLXQueryServerString;
	req->length = sizeof *req / 4;
	req->screen = screen;
	req->name = name;
	if (!_XReply(dpy, &rep, 0, False)) {
		UnlockDisplay(dpy);
		SyncHandle();
		return NULL;
	}
	bytes = (unsigned long)rep.generic.length * 4;
	if ((s = (char *)malloc(bytes + 1)) == NULL) {
		_XEatData(dpy, bytes);
		UnlockDisplay(dpy);
		SyncHandle();
		return NULL;
	}
	_XRead(dpy, s, bytes);
	s[bytes] = 0;
	UnlockDisplay(dpy);
	SyncHandle();
	return s;
}

static void
fb_load(Display *dpy, int screen)
{
	int i;

	if (dpy == fb_dpy && screen == fb_screen && fb_n >= 0)
		return;
	fb_dpy = dpy;
	fb_screen = screen;
	fb_n = read_fbconfigs(dpy, screen);
	for (i = 1; i <= 3; i++) {
		free(srv_str[i]);
		srv_str[i] = read_string(dpy, screen, i);
	}
	if (getenv("IRIS_GL_DEBUG") != NULL)
		fprintf(stderr, "libGL: the X server describes %d GLX FBConfigs, GLX %s\n", fb_n,
		    srv_str[2] != NULL ? srv_str[2] : "(no version)");
}

const struct hgl_fbcfg *
hgl_glx_fbconfigs(Display *dpy, int screen, int *n)
{
	if (dpy == NULL) {
		*n = 0;
		return NULL;
	}
	fb_load(dpy, screen);
	*n = fb_n > 0 ? fb_n : 0;
	return fb_n > 0 ? fb : NULL;
}

int
hgl_fbcfg_attr(const struct hgl_fbcfg *c, int attr, int *value)
{
	int i;

	for (i = 0; c != NULL && i < c->nattr; i++)
		if (c->attr[i][0] == attr) {
			*value = c->attr[i][1];
			return 1;
		}
	return 0;
}

const char *
hgl_glx_server_string(Display *dpy, int screen, int name)
{
	if (dpy == NULL || name < 1 || name > 3)
		return NULL;
	fb_load(dpy, screen);
	return srv_str[name];
}
