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
#define FIXED_PROPS 18

typedef struct {
	CARD8 reqType;
	CARD8 glxCode;
	CARD16 length B16;
	CARD32 screen B32;
} xGLXGetVisualConfigsReq;

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
