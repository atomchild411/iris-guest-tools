/*
 * A GL window with an overlay over it, left up to be looked at:
 *
 *   gloverlay [seconds]      (default 20)
 *
 * The window's normal planes hold a shaded quad, red to blue, swapped once a
 * second. Its overlay -- a child window in the server's GLX_LEVEL 1 visual,
 * drawn by a colour index context -- holds a yellow cross, a white frame and
 * a magenta triangle on index 0, the transparent pixel: where the overlay has
 * nothing, the quad shows through. gltest checks the overlay's pixels; this
 * is for the eye, and for a screenshot of what the board composites.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#define W 400
#define H 300

static void
shaded_quad(void)
{
	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glShadeModel(GL_SMOOTH);
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0); glVertex2i(20, 20);
	glColor3f(0, 0, 1); glVertex2i(W - 20, 20);
	glColor3f(0, 0, 1); glVertex2i(W - 20, H - 20);
	glColor3f(1, 0, 0); glVertex2i(20, H - 20);
	glEnd();
}

static void
ortho(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

int
main(int argc, char **argv)
{
	static int normal[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, None };
	static int over[] = { GLX_LEVEL, 1, GLX_BUFFER_SIZE, 1, None };
	int seconds = argc > 1 ? atoi(argv[1]) : 20, i;
	Display *dpy;
	XVisualInfo *vi, *ovi;
	XSetWindowAttributes swa;
	Colormap ocmap;
	unsigned long px[3];
	XColor c;
	Window win, ov;
	GLXContext ctx, octx;

	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		fprintf(stderr, "gloverlay: no display\n");
		return 1;
	}
	vi = glXChooseVisual(dpy, DefaultScreen(dpy), normal);
	ovi = glXChooseVisual(dpy, DefaultScreen(dpy), over);
	if (vi == NULL || ovi == NULL) {
		fprintf(stderr, "gloverlay: no %s visual\n", vi == NULL ? "RGBA" : "overlay");
		return 1;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, W, H, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XStoreName(dpy, win, "gloverlay");

	/* The overlay's colours: yellow, white and magenta, in cells of its own
	 * colormap. Pixel 0 is the server's, and transparent. */
	ocmap = XCreateColormap(dpy, RootWindow(dpy, ovi->screen), ovi->visual, AllocNone);
	if (!XAllocColorCells(dpy, ocmap, False, NULL, 0, px, 3)) {
		fprintf(stderr, "gloverlay: no overlay colour cells\n");
		return 1;
	}
	c.flags = DoRed | DoGreen | DoBlue;
	c.pixel = px[0]; c.red = 0xffff; c.green = 0xffff; c.blue = 0;      XStoreColor(dpy, ocmap, &c);
	c.pixel = px[1]; c.red = 0xffff; c.green = 0xffff; c.blue = 0xffff; XStoreColor(dpy, ocmap, &c);
	c.pixel = px[2]; c.red = 0xffff; c.green = 0;      c.blue = 0xffff; XStoreColor(dpy, ocmap, &c);
	swa.colormap = ocmap;
	swa.background_pixel = 0;
	ov = XCreateWindow(dpy, win, 0, 0, W, H, 0, ovi->depth, InputOutput, ovi->visual,
	    CWColormap | CWBorderPixel | CWBackPixel, &swa);
	/* The window manager installs the overlay's colormap with the window's. */
	XSetWMColormapWindows(dpy, win, &ov, 1);
	XMapWindow(dpy, ov);
	XMapWindow(dpy, win);
	XSync(dpy, False);
	sleep(1);

	ctx = glXCreateContext(dpy, vi, NULL, True);
	octx = glXCreateContext(dpy, ovi, NULL, True);
	if (ctx == NULL || octx == NULL) {
		fprintf(stderr, "gloverlay: no context\n");
		return 1;
	}

	glXMakeCurrent(dpy, ov, octx);
	ortho();
	glClearIndex(0);
	glClear(GL_COLOR_BUFFER_BIT);
	glIndexi((GLint)px[0]);
	glLineWidth(3);
	glBegin(GL_LINES);
	glVertex2i(W / 2, 10); glVertex2i(W / 2, H - 10);
	glVertex2i(10, H / 2); glVertex2i(W - 10, H / 2);
	glEnd();
	glIndexi((GLint)px[1]);
	glBegin(GL_LINE_LOOP);
	glVertex2i(60, 60); glVertex2i(W - 60, 60); glVertex2i(W - 60, H - 60); glVertex2i(60, H - 60);
	glEnd();
	glIndexi((GLint)px[2]);
	glBegin(GL_TRIANGLES);
	glVertex2i(80, 80); glVertex2i(160, 80); glVertex2i(120, 140);
	glEnd();
	glFlush();

	glXMakeCurrent(dpy, win, ctx);
	ortho();
	for (i = 0; i < seconds; i++) {
		shaded_quad();
		glXSwapBuffers(dpy, win);
		sleep(1);
	}
	glXMakeCurrent(dpy, None, NULL);
	return 0;
}
