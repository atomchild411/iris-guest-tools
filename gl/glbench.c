/*
 * glbench -- host OpenGL through the host call, timed phase by phase.
 *
 *   glbench [-p phases] [-f frames] [-s swaps]
 *           [-u uploads] [-T texsize] [-r readbacks] [-R readsize]
 *           [-q quakeframes] [-n surfaces] [-a appframes] [-W w] [-H h]
 *
 * A child process is forked and runs:
 *
 *   glcheck   glcheck.c's frames at 256x256: draw, glReadPixels, swap; the
 *             exact-pixel checks on frame 0 and the all-frames checksum
 *             (0xa3d786cb on the development Mac's GPU)
 *   swap      the same drawing and glXSwapBuffers only, no readback
 *   upload    a texsize x texsize RGBA glTexImage2D per frame (default
 *             1024: 4 MB), a textured quad and glFinish; checksum of a
 *             read-back 64x64 patch at the end
 *   readback  a frame drawn and read back whole at readsize x readsize
 *             (default 512: 1 MB) with glReadPixels; all-frames checksum
 *   quake     GLQuake's shape of frame at 256x256 (default 300 frames): a
 *             projection set up and glGetFloatv of the modelview matrix, then
 *             `surfaces` (600) textured immediate-mode polygons, each its own
 *             glBindTexture and glBegin/glTexCoord/glVertex/glEnd, two
 *             128x128 RGBA lightmaps uploaded with glTexImage2D and blended
 *             over a quarter of the surfaces, a 2D overlay of quads, and a
 *             swap; checksum of the last frame read back
 *
 *   app       the quake frames again, but as a real program runs them: into a
 *             GLQuake-sized window (640x480, -W/-H) instead of 256x256, and
 *             with the glFinish before the swap that GL_EndRendering does.
 *             That is two things the `quake` phase leaves out and the real
 *             application pays for every frame -- a present of nine times the
 *             pixels, and a third round trip. Not in the default phases: it is
 *             slower than all the others together.
 *
 * -p picks phases by letter (g s u r q a; default gsurq). IRIS_GL_STATS=1
 * makes each child print how many calls waited, writes and round trips it
 * took.
 *
 * Times cover the GL calls only: filling buffers and checksumming are the
 * emulated CPU's work and are left out. The parent prints the numbers and
 * the checksums. Exit status 0 when every check passed.
 *
 * Needs DISPLAY and the shim libGL (build.sh).
 *
 * With SGI's own libGL in place instead (an emulated IMPACT, say) the pass is
 * called "native" and prints the renderer, so the same numbers can be set
 * beside host GL's. The visual is 8/8/8 double-buffered when the screen has
 * one, else any double-buffered RGB visual (a Solid IMPACT's are 12-bit),
 * else 8/8/8 single-buffered; it is printed, since the read-back checksums
 * depend on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

/* The shim's own report of the transport it picked; absent from SGI's libGL. */
#pragma weak hgl_transport_name
extern const char *hgl_transport_name(void);

#define DISC 1024
#define GLCHECK_SIZE 256

struct result {
	int ran;                /* the child got as far as writing this */
	int failures;
	char transport[16];
	int frames, swaps, uploads, texsize, readbacks, readsize, qframes, surfaces;
	int appframes, appw, apph;
	double glcheck_s, swap_s, upload_s, readback_s, quake_s, app_s;
	unsigned long glcheck_sum, upload_sum, readback_sum, quake_sum, app_sum;
	char renderer[48];      /* GL_RENDERER */
	int visdepth, visbits, doublebuffered;
	char phases[16];
};

static struct result res;

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static unsigned long
fnv1a(unsigned long h, const unsigned char *p, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h = (h * 16777619UL) & 0xffffffffUL;
	}
	return h;
}

static unsigned long
fold(unsigned long all, unsigned long sum)
{
	unsigned char b[4];

	b[0] = (unsigned char)(sum >> 24);
	b[1] = (unsigned char)(sum >> 16);
	b[2] = (unsigned char)(sum >> 8);
	b[3] = (unsigned char)sum;
	return fnv1a(all, b, 4);
}

static void
check(const char *what, int good, unsigned char *p)
{
	printf("glbench: %-8s %s %s = %d %d %d %d\n", res.transport, good ? "ok  " : "FAIL", what, p[0], p[1], p[2], p[3]);
	if (!good)
		res.failures++;
}

/* glcheck.c's frame, call for call (the host's reference replays these). */
static void
draw_frame(int i, int size, GLfloat *disc_xy, GLubyte *disc_rgba)
{
	glViewport(0, 0, size, size);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1, 1, -1, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	if (i == 0) {
		glShadeModel(GL_FLAT);
	} else {
		glShadeModel(GL_SMOOTH);
		glRotatef((GLfloat)(i * 6 % 360), 0, 0, 1);
	}
	glBegin(GL_TRIANGLES);
	glColor3ub(255, 0, 0);
	glVertex2f(0.0f, 0.8f);
	glColor3ub(0, 255, 0);
	glVertex2f(-0.7f, -0.5f);
	glColor3ub(0, 0, 255);
	glVertex2f(0.7f, -0.5f);
	glEnd();
	if (i > 0) {
		glLoadIdentity();
		glTranslatef(-0.6f, 0.6f, 0);
		glScalef(0.3f, 0.3f, 1);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, disc_xy);
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, disc_rgba);
		glDrawArrays(GL_TRIANGLE_FAN, 0, DISC + 2);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
	}
}

#define QTEX 16
#define LM 128

/* One GLQuake-shaped frame (see the top of the file). */
static void
quake_frame(int f, int surfaces, GLuint *tex, GLubyte **lightmaps)
{
	GLfloat m[16];
	int s, k;

	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -1, 1, 1, 64);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatef((GLfloat)(f % 360), 0, 0, 1);
	glTranslatef(0, 0, -4);
	glGetFloatv(GL_MODELVIEW_MATRIX, m);    /* R_SetupGL does */

	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	for (s = 0; s < surfaces; s++) {
		GLfloat x = (GLfloat)((s * 37) % 64) / 16.0f - 2.0f;
		GLfloat y = (GLfloat)((s * 23) % 64) / 16.0f - 2.0f;
		GLfloat z = (GLfloat)((s * 11) % 16) / 8.0f - 1.0f;

		glBindTexture(GL_TEXTURE_2D, tex[s % QTEX]);
		glBegin(GL_POLYGON);
		glTexCoord2f(0, 0); glVertex3f(x, y, z);
		glTexCoord2f(1, 0); glVertex3f(x + 0.25f, y, z);
		glTexCoord2f(1, 1); glVertex3f(x + 0.25f, y + 0.25f, z);
		glTexCoord2f(0, 1); glVertex3f(x, y + 0.25f, z);
		glEnd();
	}

	/* The lightmap pass: two dirty lightmaps uploaded, then blended. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	for (k = 0; k < 2; k++) {
		glBindTexture(GL_TEXTURE_2D, tex[QTEX + k]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LM, LM, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		    lightmaps[(f + k) % 4]);
		for (s = k; s < surfaces; s += 8) {
			GLfloat x = (GLfloat)((s * 37) % 64) / 16.0f - 2.0f;
			GLfloat y = (GLfloat)((s * 23) % 64) / 16.0f - 2.0f;
			GLfloat z = (GLfloat)((s * 11) % 16) / 8.0f - 1.0f;

			glBegin(GL_POLYGON);
			glTexCoord2f(0, 0); glVertex3f(x, y, z);
			glTexCoord2f(1, 0); glVertex3f(x + 0.25f, y, z);
			glTexCoord2f(1, 1); glVertex3f(x + 0.25f, y + 0.25f, z);
			glTexCoord2f(0, 1); glVertex3f(x, y + 0.25f, z);
			glEnd();
		}
	}
	glDisable(GL_BLEND);

	/* The 2D overlay: status bar and console characters. */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 256, 0, 256, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f(1, 1, 1, 1);
	for (s = 0; s < 40; s++) {
		glBindTexture(GL_TEXTURE_2D, tex[s % QTEX]);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0); glVertex2f((GLfloat)(s * 6), 0);
		glTexCoord2f(1, 0); glVertex2f((GLfloat)(s * 6 + 6), 0);
		glTexCoord2f(1, 1); glVertex2f((GLfloat)(s * 6 + 6), 8);
		glTexCoord2f(0, 1); glVertex2f((GLfloat)(s * 6), 8);
		glEnd();
	}
	glDisable(GL_TEXTURE_2D);
}

/*
 * The GLQuake-shaped frames, into `win` at w x h, `frames` of them, with a
 * glFinish before each swap if `finish`. Both the `quake` phase (256x256, no
 * finish: the GL work alone) and the `app` phase (a real GLQuake window, with
 * the finish) are this.
 */
static int
quake_phase(Display *dpy, GLXDrawable win, int w, int h, int frames, int finish,
    double *secs, unsigned long *sum)
{
	double t;
	int i;

	GLuint tex[QTEX + 2];
	GLubyte *lightmaps[4], texels[64 * 64 * 4], last[64 * 64 * 4];
	int k, x, y;

	for (k = 0; k < 4; k++) {
		if ((lightmaps[k] = malloc(LM * LM * 4)) == NULL) {
			printf("glbench: out of memory\n");
			while (k-- > 0)
				free(lightmaps[k]);
			return -1;
		}
		for (y = 0; y < LM; y++)
			for (x = 0; x < LM; x++) {
				GLubyte *p = lightmaps[k] + 4 * (y * LM + x);

				p[0] = p[1] = p[2] = (GLubyte)(128 + ((x * (k + 1) + y * 3) & 127));
				p[3] = 255;
			}
	}
	glViewport(0, 0, w, h);
	for (k = 0; k < QTEX + 2; k++) {
		tex[k] = 100 + k;
		for (y = 0; y < 64; y++)
			for (x = 0; x < 64; x++) {
				GLubyte *p = texels + 4 * (y * 64 + x);

				p[0] = (GLubyte)(x * 4 + k * 16);
				p[1] = (GLubyte)(y * 4 + k * 8);
				p[2] = (GLubyte)(((x ^ y) & 8) ? 255 : k * 12);
				p[3] = 255;
			}
		glBindTexture(GL_TEXTURE_2D, tex[k]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	}
	glFinish();
	t = now();
	for (i = 0; i < frames; i++) {
		quake_frame(i, res.surfaces, tex, lightmaps);
		if (i == frames - 1) {
			glReadPixels(96, 96, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, last);
			*sum = fnv1a(2166136261UL, last, sizeof last);
		}
		/* GLQuake's GL_EndRendering: a glFinish, then the swap. */
		if (finish)
			glFinish();
		glXSwapBuffers(dpy, win);
	}
	glFinish();
	*secs = now() - t;
	for (k = 0; k < 4; k++)
		free(lightmaps[k]);
	return 0;
}

static Window
make_window(Display *dpy, XVisualInfo *vi, int x, int w, int h, const char *name)
{
	XSetWindowAttributes swa;
	Window win;

	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), x, 60, w, h, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XStoreName(dpy, win, name);
	XMapWindow(dpy, win);
	XSync(dpy, False);
	return win;
}

/*
 * The best RGB visual for the benchmark: 8/8/8 double-buffered, else any
 * double-buffered RGB visual, else 8/8/8 single-buffered.
 */
static XVisualInfo *
choose_visual(Display *dpy, int *doublebuffered)
{
	static int deep_db[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
	    GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
	static int any_db[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1,
	    GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1, None };
	static int deep_sb[] = { GLX_RGBA, GLX_RED_SIZE, 8,
	    GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
	XVisualInfo *vi;

	*doublebuffered = 1;
	if ((vi = glXChooseVisual(dpy, DefaultScreen(dpy), deep_db)) != NULL)
		return vi;
	if ((vi = glXChooseVisual(dpy, DefaultScreen(dpy), any_db)) != NULL)
		return vi;
	*doublebuffered = 0;
	return glXChooseVisual(dpy, DefaultScreen(dpy), deep_sb);
}

/* The child's work: every phase on one transport. 0 when it could run. */
static int
run(void)
{
	Display *dpy;
	XVisualInfo *vi;
	Window small, big, app;
	GLXContext ctx;
	GLfloat *disc_xy;
	GLubyte *disc_rgba, *pixels, *tex, patch[64 * 64 * 4];
	unsigned char *p;
	unsigned long all;
	double t;
	int i, k, c, n;

	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		printf("glbench: %s: cannot open the display\n", res.transport);
		return -1;
	}
	if (!glXQueryExtension(dpy, NULL, NULL)) {
		printf("glbench: %s: no GLX through this transport\n", res.transport);
		return -1;
	}
	if (&hgl_transport_name != NULL && strcmp(hgl_transport_name(), res.transport) != 0) {
		printf("glbench: asked for %s, the library uses %s\n", res.transport, hgl_transport_name());
		return -1;
	}
	if ((vi = choose_visual(dpy, &res.doublebuffered)) == NULL) {
		printf("glbench: %s: no visual\n", res.transport);
		return -1;
	}
	small = make_window(dpy, vi, 40, GLCHECK_SIZE, GLCHECK_SIZE, "glbench 256");
	big = make_window(dpy, vi, 60 + GLCHECK_SIZE, res.readsize, res.readsize, "glbench");
	app = strchr(res.phases, 'a')
	    ? make_window(dpy, vi, 40, res.appw, res.apph, "glbench app") : None;
	if ((ctx = glXCreateContext(dpy, vi, NULL, True)) == NULL || !glXMakeCurrent(dpy, small, ctx)) {
		printf("glbench: %s: no GL context\n", res.transport);
		return -1;
	}
	{
		GLint bits = 0;

		strncpy(res.renderer, (char *)glGetString(GL_RENDERER), sizeof res.renderer - 1);
		glGetIntegerv(GL_RED_BITS, &bits);
		res.visbits = bits;
		res.visdepth = vi->depth;
	}

	disc_xy = malloc(sizeof(GLfloat) * 2 * (DISC + 2));
	disc_rgba = malloc(4 * (DISC + 2));
	n = res.readsize > GLCHECK_SIZE ? res.readsize : GLCHECK_SIZE;
	pixels = malloc((size_t)n * n * 4);
	tex = malloc((size_t)res.texsize * res.texsize * 4);
	if (disc_xy == NULL || disc_rgba == NULL || pixels == NULL || tex == NULL) {
		printf("glbench: out of memory\n");
		return -1;
	}
	disc_xy[0] = disc_xy[1] = 0;
	disc_rgba[0] = disc_rgba[1] = disc_rgba[2] = disc_rgba[3] = 255;
	for (k = 0; k <= DISC; k++) {
		int tx = k % DISC, ty = (k + DISC / 4) % DISC;

		disc_xy[2 * (k + 1)] = (GLfloat)((tx > DISC / 2 ? tx - DISC / 2 : DISC / 2 - tx) - DISC / 4) / (DISC / 4);
		disc_xy[2 * (k + 1) + 1] = (GLfloat)((ty > DISC / 2 ? ty - DISC / 2 : DISC / 2 - ty) - DISC / 4) / (DISC / 4);
		disc_rgba[4 * (k + 1)] = (GLubyte)(k * 255 / DISC);
		disc_rgba[4 * (k + 1) + 1] = (GLubyte)(255 - k * 255 / DISC);
		disc_rgba[4 * (k + 1) + 2] = 128;
		disc_rgba[4 * (k + 1) + 3] = 255;
	}

	/* glcheck */
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	all = 2166136261UL;
	res.glcheck_s = 0;
	for (i = 0; i < (strchr(res.phases, 'g') ? res.frames : 0); i++) {
		for (k = 0; k < GLCHECK_SIZE * GLCHECK_SIZE * 4; k++)
			pixels[k] = (unsigned char)(k * 7 + i);
		t = now();
		draw_frame(i, GLCHECK_SIZE, disc_xy, disc_rgba);
		glReadPixels(0, 0, GLCHECK_SIZE, GLCHECK_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		res.glcheck_s += now() - t;
		if (i == 0) {
			p = pixels + 4 * (1 * GLCHECK_SIZE + 1);
			check("clear colour at (1,1)", p[0] == 51 && p[1] == 102 && p[2] == 153 && p[3] == 255, p);
			p = pixels + 4 * ((GLCHECK_SIZE / 2) * GLCHECK_SIZE + GLCHECK_SIZE / 2);
			check("flat triangle at the centre", p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255, p);
			c = glGetError();
			printf("glbench: %-8s %s glGetError after frame 0 is NO_ERROR (%#x)\n", res.transport,
			    c == GL_NO_ERROR ? "ok  " : "FAIL", c);
			if (c != GL_NO_ERROR)
				res.failures++;
		}
		all = fold(all, fnv1a(2166136261UL, pixels, (unsigned long)GLCHECK_SIZE * GLCHECK_SIZE * 4));
		t = now();
		glXSwapBuffers(dpy, small);
		res.glcheck_s += now() - t;
	}
	res.glcheck_sum = all;
	if (strchr(res.phases, 'g'))
		printf("glbench: %-8s glcheck  %d frames of %dx%d with readback: %.2f ms/frame, %.1f fps, checksum %#010lx\n",
	    res.transport, res.frames, GLCHECK_SIZE, GLCHECK_SIZE, 1e3 * res.glcheck_s / res.frames,
	    res.frames / res.glcheck_s, res.glcheck_sum);
	fflush(stdout);

	/* swap: drawing and presenting only */
	t = now();
	for (i = 0; i < (strchr(res.phases, 's') ? res.swaps : 0); i++) {
		draw_frame(i + 1, GLCHECK_SIZE, disc_xy, disc_rgba);
		glXSwapBuffers(dpy, small);
	}
	glFinish();
	res.swap_s = strchr(res.phases, 's') ? now() - t : 0;
	if (strchr(res.phases, 's'))
		printf("glbench: %-8s swap     %d frames of %dx%d, no readback: %.2f ms/frame, %.1f fps\n",
	    res.transport, res.swaps, GLCHECK_SIZE, GLCHECK_SIZE, 1e3 * res.swap_s / res.swaps, res.swaps / res.swap_s);
	fflush(stdout);

	/* quake */
	if (strchr(res.phases, 'q')) {
		if (quake_phase(dpy, small, GLCHECK_SIZE, GLCHECK_SIZE, res.qframes, 0,
		    &res.quake_s, &res.quake_sum) != 0)
			return -1;
		printf("glbench: %-8s quake    %d frames, %d surfaces each: %.2f ms/frame, %.1f fps, checksum %#010lx\n",
		    res.transport, res.qframes, res.surfaces, 1e3 * res.quake_s / res.qframes,
		    res.qframes / res.quake_s, res.quake_sum);
		fflush(stdout);
	}

	/* app: the same frames as a real program runs them */
	if (strchr(res.phases, 'a')) {
		if (!glXMakeCurrent(dpy, app, ctx)) {
			printf("glbench: glXMakeCurrent on the app window failed\n");
			return -1;
		}
		if (quake_phase(dpy, app, res.appw, res.apph, res.appframes, 1,
		    &res.app_s, &res.app_sum) != 0)
			return -1;
		if (!glXMakeCurrent(dpy, small, ctx)) {
			printf("glbench: glXMakeCurrent back to the small window failed\n");
			return -1;
		}
		printf("glbench: %-8s app      %d frames of %dx%d with glFinish: %.2f ms/frame, %.1f fps, checksum %#010lx\n",
		    res.transport, res.appframes, res.appw, res.apph, 1e3 * res.app_s / res.appframes,
		    res.appframes / res.app_s, res.app_sum);
		fflush(stdout);
	}

	/* upload */
	if (!glXMakeCurrent(dpy, big, ctx)) {
		printf("glbench: %s: cannot draw into the big window\n", res.transport);
		return -1;
	}
	glViewport(0, 0, res.readsize, res.readsize);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1, 1, -1, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_TEXTURE_2D);
	res.upload_s = 0;
	for (i = 0; i < (strchr(res.phases, 'u') ? res.uploads : 0); i++) {
		for (k = 0; k < res.texsize * res.texsize * 4; k++)
			tex[k] = (GLubyte)(k * 31 + (k >> 12) * 3 + i * 7);
		t = now();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, res.texsize, res.texsize, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0);
		glVertex2f(-1, -1);
		glTexCoord2f(1, 0);
		glVertex2f(1, -1);
		glTexCoord2f(1, 1);
		glVertex2f(1, 1);
		glTexCoord2f(0, 1);
		glVertex2f(-1, 1);
		glEnd();
		glFinish();
		res.upload_s += now() - t;
	}
	glDisable(GL_TEXTURE_2D);
	glReadPixels(res.readsize / 2 - 32, res.readsize / 2 - 32, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, patch);
	res.upload_sum = fnv1a(2166136261UL, patch, sizeof patch);
	if (strchr(res.phases, 'u'))
		printf("glbench: %-8s upload   %d x %dx%d RGBA glTexImage2D: %.1f ms each, %.2f MB/s, checksum %#010lx\n",
	    res.transport, res.uploads, res.texsize, res.texsize, 1e3 * res.upload_s / res.uploads,
	    res.uploads * (res.texsize * (double)res.texsize * 4 / 1048576.0) / res.upload_s, res.upload_sum);
	fflush(stdout);

	/* readback */
	all = 2166136261UL;
	res.readback_s = 0;
	for (i = 0; i < (strchr(res.phases, 'r') ? res.readbacks : 0); i++) {
		t = now();
		draw_frame(i + 1, res.readsize, disc_xy, disc_rgba);
		glReadPixels(0, 0, res.readsize, res.readsize, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		res.readback_s += now() - t;
		all = fold(all, fnv1a(2166136261UL, pixels, (unsigned long)res.readsize * res.readsize * 4));
	}
	res.readback_sum = all;
	if (strchr(res.phases, 'r'))
		printf("glbench: %-8s readback %d x %dx%d RGBA glReadPixels: %.1f ms each, %.2f MB/s, checksum %#010lx\n",
	    res.transport, res.readbacks, res.readsize, res.readsize, 1e3 * res.readback_s / res.readbacks,
	    res.readbacks * (res.readsize * (double)res.readsize * 4 / 1048576.0) / res.readback_s, res.readback_sum);
	fflush(stdout);

	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, ctx);
	XCloseDisplay(dpy);
	return 0;
}

static int
read_all(int fd, char *buf, size_t n)
{
	size_t done = 0;
	ssize_t r;

	while (done < n) {
		r = read(fd, buf + done, n - done);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		done += r;
	}
	return 0;
}

static int
one(const char *transport, struct result *out)
{
	int fds[2], status;
	pid_t pid;

	memset(out, 0, sizeof *out);
	fflush(stdout);
	if (pipe(fds) < 0) {
		perror("glbench: pipe");
		return -1;
	}
	if ((pid = fork()) < 0) {
		perror("glbench: fork");
		return -1;
	}
	if (pid == 0) {
		close(fds[0]);
		strncpy(res.transport, transport, sizeof res.transport - 1);
		if (run() == 0) {
			res.ran = 1;
			write(fds[1], (char *)&res, sizeof res);
		}
		close(fds[1]);
		exit(0);
	}
	close(fds[1]);
	if (read_all(fds[0], (char *)out, sizeof *out) < 0)
		out->ran = 0;
	close(fds[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return out->ran ? 0 : -1;
}

static void
usage(void)
{
	printf("usage: glbench [-p phases gsurqa] [-f frames] [-s swaps] [-u uploads]\n"
	    "               [-T texsize] [-r readbacks] [-R readsize] [-q quakeframes] [-n surfaces]\n"
	    "               [-a appframes] [-W appwidth] [-H appheight]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct result r;
	/* SGI's libGL (or another without the shim's transport) runs as itself */
	const char *transport = &hgl_transport_name != NULL ? "hostcall" : "native";
	int c, failed = 0;

	res.frames = 60;
	res.swaps = 120;
	res.uploads = 10;
	res.texsize = 1024;
	res.readbacks = 30;
	res.readsize = 512;
	res.qframes = 300;
	res.surfaces = 600;
	res.appframes = 300;
	res.appw = 640;
	res.apph = 480;
	/* `a` is not in the default set: it is slower than the rest together,
	 * and it is a different question from "how fast is the host call". */
	strcpy(res.phases, "gsurq");
	while ((c = getopt(argc, argv, "p:f:s:u:T:r:R:q:n:a:W:H:")) != -1) {
		switch (c) {
		case 'p': strncpy(res.phases, optarg, sizeof res.phases - 1); break;
		case 'q': res.qframes = atoi(optarg); break;
		case 'n': res.surfaces = atoi(optarg); break;
		case 'a': res.appframes = atoi(optarg); break;
		case 'W': res.appw = atoi(optarg); break;
		case 'H': res.apph = atoi(optarg); break;
		case 'f': res.frames = atoi(optarg); break;
		case 's': res.swaps = atoi(optarg); break;
		case 'u': res.uploads = atoi(optarg); break;
		case 'T': res.texsize = atoi(optarg); break;
		case 'r': res.readbacks = atoi(optarg); break;
		case 'R': res.readsize = atoi(optarg); break;
		default: usage();
		}
	}
	if (res.appframes < 1 || res.appw < 64 || res.apph < 64 || res.appw > 4096 || res.apph > 4096)
		usage();
	if (res.frames < 1 || res.swaps < 1 || res.uploads < 1 || res.readbacks < 1 || res.qframes < 1
	    || res.surfaces < 8 || res.surfaces > 100000
	    || res.texsize < 16 || res.texsize > 4096 || res.readsize < 16 || res.readsize > 2048)
		usage();
	if (one(transport, &r) != 0) {
		printf("glbench: did not run\n");
		failed++;
	} else if (r.failures) {
		printf("glbench: failed %d checks\n", r.failures);
		failed++;
	}
	if (r.ran) {
		printf("glbench: glcheck ms/frame   swap fps   upload ms   readback ms   quake ms/frame  quake fps"
		    "   app ms/frame   app fps\n");
		printf("glbench: %16.2f  %9.1f  %10.1f  %12.1f  %15.2f  %9.1f  %13.2f  %8.1f\n",
		    1e3 * r.glcheck_s / r.frames, r.swap_s > 0 ? r.swaps / r.swap_s : 0.0,
		    1e3 * r.upload_s / r.uploads, 1e3 * r.readback_s / r.readbacks,
		    1e3 * r.quake_s / r.qframes, r.quake_s > 0 ? r.qframes / r.quake_s : 0.0,
		    1e3 * r.app_s / r.appframes, r.app_s > 0 ? r.appframes / r.app_s : 0.0);
		printf("glbench: renderer %s, visual depth %d (%d bits a channel), %s-buffered\n",
		    r.renderer, r.visdepth, r.visbits, r.doublebuffered ? "double" : "single");
		printf("glbench: checksums: glcheck %#010lx, upload %#010lx, readback %#010lx, quake %#010lx\n",
		    r.glcheck_sum, r.upload_sum, r.readback_sum, r.quake_sum);
	}
	printf("glbench: %s\n", failed ? "FAILED" : "PASSED");
	return failed != 0;
}
