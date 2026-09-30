/*
 * glcheck -- render N frames through IRIS's host OpenGL and print checksums
 * of what glReadPixels gives back.
 *
 *   glcheck [frames] [size]          (default 60 frames, 256x256)
 *
 * Needs DISPLAY and the shim installed as libGL.so (see build.sh). Frame 0 is
 * drawn flat-shaded and checked pixel by pixel against values OpenGL fixes
 * exactly; every frame is then read back whole -- at 256x256 that is 64
 * pages, more than the R4400's TLB holds, so the host's need-page retries on
 * a write are exercised every frame -- and checksummed with FNV-1a over the
 * RGBA bytes. Frames after the first rotate a smooth-shaded triangle and draw
 * a diamond from client arrays with glDrawArrays, which the host reads from this
 * process's memory at the draw.
 *
 * The checksums are the host GPU's rasterisation, so they are stable on one
 * machine: `cargo test -p iris-hostgl --release -- --nocapture glcheck_reference`
 * replays the same frames on the host directly and prints the numbers to
 * compare with. The exact-pixel checks hold everywhere.
 *
 * Output: "ok"/"FAIL" lines, one checksum line per frame, a summary, and
 * "glcheck: PASSED" or "glcheck: FAILED". Exit status 0 when every check
 * passed. Without IRIS's host GL service it says so and exits 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

/*
 * The rim of the "disc" -- a diamond, so every coordinate is an exact binary
 * fraction and the host's reference computes the very same floats.
 */
#define DISC 1024

static int failures;

static void
check(const char *what, int good, unsigned char *p)
{
	printf("%s %s = %d %d %d %d\n", good ? "ok  " : "FAIL", what, p[0], p[1], p[2], p[3]);
	if (!good)
		failures++;
}

static unsigned long
fnv1a(const unsigned char *p, unsigned long n)
{
	unsigned long h = 2166136261UL;
	unsigned long i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h = (h * 16777619UL) & 0xffffffffUL;
	}
	return h;
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/*
 * One frame. The host's reference (src/tests.rs, glcheck_reference) makes
 * exactly these calls in this order: change one, change both.
 */
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

int
main(int argc, char **argv)
{
	static int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
	    GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
	int frames = argc > 1 ? atoi(argv[1]) : 60;
	int size = argc > 2 ? atoi(argv[2]) : 256;
	Display *dpy;
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	GLfloat *disc_xy;
	GLubyte *disc_rgba, *pixels;
	unsigned long sum, all = 2166136261UL;
	unsigned char *p, sums[4];
	double t0, t1;
	int i, k, c;

	if (frames < 1 || size < 16 || size > 2048) {
		printf("usage: glcheck [frames >= 1] [16 <= size <= 2048]\n");
		return 2;
	}
	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		printf("glcheck: cannot open the display\n");
		return 2;
	}
	if (!glXQueryExtension(dpy, NULL, NULL)) {
		printf("glcheck: no GLX (is this running under IRIS with host GL?)\n");
		return 2;
	}
	if ((vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs)) == NULL) {
		printf("glcheck: no visual\n");
		return 2;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.background_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 100, 100, size, size, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel | CWBackPixel, &swa);
	XStoreName(dpy, win, "glcheck");
	XMapWindow(dpy, win);
	XSync(dpy, False);
	if ((ctx = glXCreateContext(dpy, vi, NULL, True)) == NULL || !glXMakeCurrent(dpy, win, ctx)) {
		printf("glcheck: no GL context\n");
		return 2;
	}
	printf("glcheck: GL_VENDOR %s, GL_RENDERER %s, GL_VERSION %s\n", (char *)glGetString(GL_VENDOR),
	    (char *)glGetString(GL_RENDERER), (char *)glGetString(GL_VERSION));

	/* The disc: a fan around the centre, the colour turning along the rim. */
	disc_xy = malloc(sizeof(GLfloat) * 2 * (DISC + 2));
	disc_rgba = malloc(4 * (DISC + 2));
	pixels = malloc((size_t)size * size * 4);
	if (disc_xy == NULL || disc_rgba == NULL || pixels == NULL) {
		printf("glcheck: out of memory\n");
		return 2;
	}
	disc_xy[0] = disc_xy[1] = 0;
	disc_rgba[0] = disc_rgba[1] = disc_rgba[2] = disc_rgba[3] = 255;
	for (k = 0; k <= DISC; k++) {
		/* A triangle wave in x, the same a quarter turn on in y. */
		int tx = k % DISC, ty = (k + DISC / 4) % DISC;

		disc_xy[2 * (k + 1)] = (GLfloat)((tx > DISC / 2 ? tx - DISC / 2 : DISC / 2 - tx) - DISC / 4) / (DISC / 4);
		disc_xy[2 * (k + 1) + 1] = (GLfloat)((ty > DISC / 2 ? ty - DISC / 2 : DISC / 2 - ty) - DISC / 4) / (DISC / 4);
		disc_rgba[4 * (k + 1)] = (GLubyte)(k * 255 / DISC);
		disc_rgba[4 * (k + 1) + 1] = (GLubyte)(255 - k * 255 / DISC);
		disc_rgba[4 * (k + 1) + 2] = 128;
		disc_rgba[4 * (k + 1) + 3] = 255;
	}

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	t0 = now();
	for (i = 0; i < frames; i++) {
		draw_frame(i, size, disc_xy, disc_rgba);
		/* Every byte of the buffer is overwritten, so fill it with a
		 * pattern first: a read that stored nothing cannot pass. */
		for (k = 0; k < size * size * 4; k++)
			pixels[k] = (unsigned char)(k * 7 + i);
		glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		if (i == 0) {
			p = pixels + 4 * (1 * size + 1);
			check("clear colour at (1,1) is 51 102 153 255", p[0] == 51 && p[1] == 102 && p[2] == 153 && p[3] == 255, p);
			p = pixels + 4 * (size - 2) * size + 4 * (size - 2);
			check("clear colour at the top right is 51 102 153 255", p[0] == 51 && p[1] == 102 && p[2] == 153 && p[3] == 255, p);
			p = pixels + 4 * ((size / 2) * size + size / 2);
			check("flat triangle at the centre is blue 0 0 255 255", p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255, p);
			c = glGetError();
			printf("%s glGetError after frame 0 is NO_ERROR (%#x)\n", c == GL_NO_ERROR ? "ok  " : "FAIL", c);
			if (c != GL_NO_ERROR)
				failures++;
		}
		sum = fnv1a(pixels, (unsigned long)size * size * 4);
		printf("glcheck: frame %d checksum %#010lx\n", i, sum);
		sums[0] = (unsigned char)(sum >> 24);
		sums[1] = (unsigned char)(sum >> 16);
		sums[2] = (unsigned char)(sum >> 8);
		sums[3] = (unsigned char)sum;
		for (k = 0; k < 4; k++) {
			all ^= sums[k];
			all = (all * 16777619UL) & 0xffffffffUL;
		}
		glXSwapBuffers(dpy, win);
	}
	t1 = now();
	printf("glcheck: %d frames of %dx%d, each read back, in %.2fs = %.1f fps\n", frames, size, size,
	    t1 - t0, frames / (t1 - t0));
	printf("glcheck: all frames checksum %#010lx\n", all);
	printf("glcheck: %s\n", failures ? "FAILED" : "PASSED");
	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, ctx);
	XCloseDisplay(dpy);
	return failures != 0;
}
