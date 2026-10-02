/*
 * Host OpenGL for IRIX programs under IRIS: the host call, the command
 * buffer, and the GL calls the shim answers in the guest rather than on the
 * host.
 *
 * glArrayElement reads the client arrays when it is called. Sending the index
 * to the host would read them when the buffer is flushed -- by which time a
 * program refilling one scratch array per strip has overwritten them -- so it
 * becomes the vertex, normal, colour, index, texture coordinate and edge flag
 * calls it stands for, batched like any other. For that the shim keeps its
 * own record of the arrays, which also answers glGetPointerv.
 *
 * One buffer serves the process. Rendering from more than one thread at a
 * time (sproc share groups, pthreads) needs a buffer per thread; not yet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <GL/gl.h>
#include "glshim.h"
#include "glshim_rt.h"

/* ---- the host call ---- */

/* 1: the host answered HELLO for process hgl_pid; -1: it is not there. */
static int hgl_state;
static pid_t hgl_pid;
static long hgl_client;
static unsigned long hgl_serial;
int hgl_transport;

const char *
hgl_transport_name(void)
{
	return hgl_transport == HGL_TRANSPORT_HOSTCALL ? "hostcall" : "none";
}

static long long hgl_call3(long op, hgl_slot *slots, long long *v1_out, int noreply);

/* IRIS_GL_STATS=1: how this process reached the host, printed at exit. */
static unsigned long st_traps, st_need_pages;

/* ---- round-trip timing (IRIS_GL_STATS) ---- */

int hgl_timing;
static hgl_times t_swap, t_other;
/* What the host's presenting thread had done by the time this process left:
 * asked for before GOODBYE, because afterwards this process has no client. */
static long long st_presented, st_replaced;
static int st_have_present;
/*
 * IRIS_GL_PRESENT: ask the host what its presenting thread did, and print that
 * and nothing else.
 *
 * Separate from IRIS_GL_STATS on purpose. IRIS_GL_STATS times every round trip
 * and every write, which costs about a quarter of the frame rate -- so using it
 * to find out how many frames were dropped changes how many frames were
 * dropped. These counters are four atomics on a thread of the host's and cost
 * the guest one call at exit, so they can be read during a run whose number is
 * going to be quoted.
 */
static int hgl_pstats;
/*
 * Which GL entry point made a command buffer wait: the last command in the
 * buffer being flushed (the encoders put it there -- see glshim.h). Counted
 * per entry point so a workload's round trips can be attributed; the numbers
 * are the HGL_OP_* of glshim_ops.h.
 */
#define HGL_OPS 512
static unsigned long st_waits_by_op[HGL_OPS];
static int last_op = -1;
/* Waits by host operation, HGL_GL_* & 0xff (BATCH is broken out above). */
static unsigned long st_waits_by_hgl[32];

/*
 * Microseconds from a clock that does not jump. CLOCK_SGI_CYCLE is the free
 * running counter, read without a system call where the kernel maps it.
 */
double
hgl_now_us(void)
{
	struct timespec ts;
	struct timeval tv;

	if (clock_gettime(CLOCK_SGI_CYCLE, &ts) == 0)
		return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
		return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1e6 + tv.tv_usec;
}

void
hgl_time_add(hgl_times *t, double us)
{
	int b = us < 100 ? 0 : us < 500 ? 1 : us < 1000 ? 2 : us < 2000 ? 3 : us < 5000 ? 4 : 5;

	/* CLOCK_SGI_CYCLE is a counter that wraps: a "sample" of a second or
	 * more is the wrap, not a wait, and would swamp the mean. */
	if (us < 0 || us > 1e6) {
		t->wrapped++;
		return;
	}
	t->n++;
	t->total_us += us;
	if (us > t->max_us)
		t->max_us = us;
	t->bucket[b]++;
}

void
hgl_time_print(FILE *f, const char *what, const hgl_times *t)
{
	if (t->n == 0)
		return;
	fprintf(f, "libGL: %s: %lu round trips, %.1f ms total, %.0f us mean, %.0f us max; "
	    "<0.1ms %lu  <0.5 %lu  <1 %lu  <2 %lu  <5 %lu  >=5 %lu  (%lu dropped)\n",
	    what, t->n, t->total_us / 1000.0, t->total_us / t->n, t->max_us,
	    t->bucket[0], t->bucket[1], t->bucket[2], t->bucket[3], t->bucket[4], t->bucket[5],
	    t->wrapped);
}

static void
hgl_stats(void)
{
	if (!hgl_timing) {
		/* IRIS_GL_PRESENT alone: the presenter's numbers, nothing else. */
		if (st_have_present) {
			long long total = st_presented + st_replaced;

			fprintf(stderr, "libGL: host presenter: %lld frames presented, %lld replaced by a "
			    "newer one before they could be (%.1f%% dropped)\n", st_presented, st_replaced,
			    total > 0 ? 100.0 * st_replaced / total : 0.0);
		}
		return;
	}
	fprintf(stderr, "libGL: process %ld, transport %s\n", (long)getpid(), hgl_transport_name());
	if (st_traps)
		fprintf(stderr, "libGL: hostcall: %lu traps, %lu of them need-page retries\n",
		    st_traps, st_need_pages);
	if (st_have_present) {
		long long total = st_presented + st_replaced;

		fprintf(stderr, "libGL: host presenter: %lld frames presented, %lld replaced by a newer "
		    "one before they could be (%.1f%% dropped)\n", st_presented, st_replaced,
		    total > 0 ? 100.0 * st_replaced / total : 0.0);
	}
	hgl_time_print(stderr, "swap", &t_swap);
	hgl_time_print(stderr, "other waits", &t_other);
	{
		int i, j, best;
		unsigned long seen[HGL_OPS];

		memcpy(seen, st_waits_by_op, sizeof seen);
		fprintf(stderr, "libGL: command buffers that waited, by their last call"
		    " (HGL_OP_* of glshim_ops.h):");
		for (j = 0; j < 8; j++) {
			for (i = 0, best = -1; i < HGL_OPS; i++)
				if (seen[i] > 0 && (best < 0 || seen[i] > seen[best]))
					best = i;
			if (best < 0)
				break;
			fprintf(stderr, " op %d x%lu", best, seen[best]);
			seen[best] = 0;
		}
		fprintf(stderr, "\n");
		fprintf(stderr, "libGL: waits by host operation (HGL_GL_* low byte):");
		for (i = 0; i < 32; i++)
			if (st_waits_by_hgl[i] > 0)
				fprintf(stderr, " %#x x%lu", 0x10000 + i, st_waits_by_hgl[i]);
		fprintf(stderr, "\n");
	}
}

static void
hgl_goodbye(void)
{
	hgl_slot a[1];

	/* A forked child that never drew inherits this handler and the
	 * parent's client id: it must not take the parent's contexts with it. */
	if (hgl_state <= 0 || getpid() != hgl_pid)
		return;
	/* Before GOODBYE: after it this process is no longer a client. */
	if (hgl_pstats) {
		long long v1 = 0;

		a[0].i = 0;
		st_presented = hgl_call2(HGL_GL_STATS, a, &v1);
		st_replaced = v1;
		st_have_present = st_presented >= 0;
	}
	a[0].i = 0;
	hgl_call(HGL_GL_GOODBYE, a);
	hgl_state = 0;
}

int
hgl_present(void)
{
	static int registered;
	long args[7], rv0, rv1;
	long long v0 = 0, v1 = 0;

	if (hgl_state > 0 && getpid() == hgl_pid)
		return 1;
	if (hgl_state < 0)
		return 0;
	/* First call in this process, or in a forked child, which asks the
	 * host for a client id of its own. */
	hgl_transport = HGL_TRANSPORT_NONE;
	st_traps = st_need_pages = 0;
	memset(args, 0, sizeof args);
	args[0] = HGL_GL_HELLO;
	args[1] = HGL_PROTOCOL;
	if (HGL_TRAP(HGL_HOSTCALL_GL, args, &rv0, &rv1) != 0) {
		fprintf(stderr, "libGL: host OpenGL is not available (IRIS host call %d "
		    "answered errno %ld): this libGL works only under the IRIS emulator "
		    "with its host GL service; OpenGL calls will do nothing.\n",
		    HGL_HOSTCALL_GL, rv0);
		hgl_state = -1;
		return 0;
	}
	v0 = (unsigned long)rv0;
	v1 = (unsigned long)rv1;
	hgl_transport = HGL_TRANSPORT_HOSTCALL;
	if (v1 != HGL_PROTOCOL || v0 == 0) {
		fprintf(stderr, "libGL: IRIS host OpenGL speaks protocol %ld, this "
		    "library %d: rebuild the library to match; OpenGL calls will do "
		    "nothing.\n", (long)v1, HGL_PROTOCOL);
		hgl_transport = HGL_TRANSPORT_NONE;
		hgl_state = -1;
		return 0;
	}
	hgl_client = (long)v0;
	hgl_pid = getpid();
	hgl_state = 1;
	/* After hgl_state, so the call inside it goes through. */
	hgl_swap_setup();
	if (getenv("IRIS_GL_DEBUG") != NULL)
		fprintf(stderr, "libGL: process %ld uses host OpenGL through %s\n",
		    (long)hgl_pid, hgl_transport_name());
	if (!registered) {
		registered = 1;
		/* Registered first, so it runs after goodbye and counts it. */
		hgl_pstats = getenv("IRIS_GL_PRESENT") != NULL;
		if (getenv("IRIS_GL_STATS") != NULL) {
			hgl_timing = 1;
			hgl_pstats = 1;
		}
		if (hgl_pstats)
			atexit(hgl_stats);
		atexit(hgl_goodbye);
	}
	return 1;
}

/* ---- per-context client state ----
 *
 * The library keeps a record per context of what the host has but cannot be
 * asked for cheaply: the client arrays, as the host records them (set by the
 * pointer calls, glEnable/DisableClientState and glInterleavedArrays), the
 * client texture unit, which buffer glDrawBuffer names, and a colour-index
 * context's index state. glArrayElement and glGetPointerv answer from it.
 */
#define HGL_CLIENTS 64

struct hgl_array {
	int enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	const GLvoid *pointer;
};

struct hgl_client_state {
	void *ctx;              /* the context; NULL: a free slot */
	int tracked;            /* this is a current context's own record */
	struct hgl_array arrays[6];
	/* The client texture unit, from 0, and each unit's coordinate array
	 * while it is not the current one: arrays[HGL_ARRAY_TEXCOORD] is the
	 * current unit's, and its entry here is stale. */
	int client_unit;
	struct hgl_array tex_arrays[HGL_TEX_UNITS];
	/* glDrawBuffer names the front buffer now, and has named it since the
	 * window was last presented (see hgl_front_drawn). */
	int front_now, front_drawn;
	/* A colour-index context's index state (see hgl_index_mode): the host
	 * has none of it. index_ready: its initial state has been set. */
	int index_mode, index_ready;
	long cur_index, clear_index;
	unsigned long index_mask;
};

static struct hgl_client_state clients[HGL_CLIENTS];
/* No context current, or more contexts than slots: nothing is known. */
static struct hgl_client_state untracked;
static struct hgl_client_state *cur_client = &untracked;

static void
client_init(struct hgl_client_state *c, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->ctx = ctx;
	c->tracked = ctx != NULL;
}

static void
client_pick(void *ctx)
{
	int i, free_slot = -1;

	if (ctx == NULL) {
		cur_client = &untracked;
		return;
	}
	for (i = 0; i < HGL_CLIENTS; i++) {
		if (clients[i].ctx == ctx) {
			cur_client = &clients[i];
			return;
		}
		if (clients[i].ctx == NULL && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		client_init(&untracked, NULL);
		cur_client = &untracked;
		return;
	}
	client_init(&clients[free_slot], ctx);
	cur_client = &clients[free_slot];
}

void
hgl_client_select(void *ctx)
{
	client_pick(ctx);
	hgl_index_mode = cur_client->index_mode;
}

void
hgl_client_forget(void *ctx)
{
	int i;

	for (i = 0; i < HGL_CLIENTS; i++)
		if (clients[i].ctx == ctx && ctx != NULL) {
			if (cur_client == &clients[i]) {
				cur_client = &untracked;
				hgl_index_mode = 0;
			}
			clients[i].ctx = NULL;
		}
}

/*
 * The encoders report pixel stores, client attribute pushes and pops, and the
 * memory each command will read. The host call reads this process's memory
 * itself and keeps its own pixel store, so there is nothing to record.
 */
void
hgl_pixel_store(GLenum pname, double value)
{
	(void)pname;
	(void)value;
}

void
hgl_push_client_attrib(GLbitfield mask)
{
	(void)mask;
}

void
hgl_pop_client_attrib(void)
{
}

void
hgl_ref(const char *addr, unsigned long len)
{
	(void)addr;
	(void)len;
}

void
hgl_ref_image(const GLvoid *pixels, GLenum format, GLenum type, int bitmap, int ndims,
    GLint d0, GLint d1, GLint d2, GLint d3)
{
	(void)pixels; (void)format; (void)type; (void)bitmap; (void)ndims;
	(void)d0; (void)d1; (void)d2; (void)d3;
}

/* ---- the call ---- */

static long long
hgl_call3_raw(long op, hgl_slot *slots, long long *v1_out, int noreply)
{
	long args[7], v0, v1, last = -1;
	int again = 0;

	(void)noreply;          /* the host call always answers */
	if (!hgl_present())
		return -1;
	memset(args, 0, sizeof args);
	args[0] = op;
	args[1] = (long)(char *)slots;
	args[2] = hgl_client;
	args[3] = (long)++hgl_serial;
	for (;;) {
		st_traps++;
		if (HGL_TRAP(HGL_HOSTCALL_GL, args, &v0, &v1) == 0) {
			if (v1_out != 0)
				*v1_out = (unsigned long)v1;
			return (unsigned long)v0;
		}
		if (v0 != HGL_HOSTCALL_ENEEDPAGE)
			return -1;
		st_need_pages++;
		/*
		 * Touch the page the host could not use, the way any access
		 * would, and make the same call again. A page asked for over and
		 * over is one IRIX will map but the host still cannot use (not
		 * RAM): give up rather than spin.
		 */
		if (v1 == last) {
			if (++again > 16) {
				fprintf(stderr, "libGL: host OpenGL cannot use page %#lx; "
				    "call %#lx abandoned\n", (unsigned long)v1 & ~1ul, op);
				return -1;
			}
		} else {
			last = v1;
			again = 0;
		}
		{
			/* volatile: a read whose value is unused, or a byte written
			 * back to itself, must still happen. */
			volatile char *p = (char *)((unsigned long)v1 & ~1ul);

			if (v1 & 1)
				*p = *p;
			else
				(void)*p;
		}
	}
}

/* The timed wrapper: only a call that waits for the host is a round trip. */
static long long
hgl_call3(long op, hgl_slot *slots, long long *v1_out, int noreply)
{
	double t0;
	long long r;

	if (!hgl_timing || noreply)
		return hgl_call3_raw(op, slots, v1_out, noreply);
	t0 = hgl_now_us();
	r = hgl_call3_raw(op, slots, v1_out, noreply);
	hgl_time_add(op == HGL_GL_SWAP ? &t_swap : &t_other, hgl_now_us() - t0);
	if (op == HGL_GL_BATCH && last_op >= 0 && last_op < HGL_OPS)
		st_waits_by_op[last_op]++;
	if ((unsigned long)(op & 0xff) < 32)
		st_waits_by_hgl[op & 0xff]++;
	return r;
}

long long
hgl_call2(long op, hgl_slot *slots, long long *v1_out)
{
	return hgl_call3(op, slots, v1_out, 0);
}

void
hgl_wait(long long ns)
{
	struct timeval tv;

	if (ns <= 0)
		return;
	tv.tv_sec = (long)(ns / 1000000000);
	tv.tv_usec = (long)((ns % 1000000000) / 1000);
	select(0, NULL, NULL, NULL, &tv);
}

/* ---- the command buffer ---- */

unsigned char *hgl_cur;
unsigned char *hgl_end;
static unsigned char *hgl_base;
/* doubles, so the buffer is 8-byte aligned, as the command format needs. */
static double hgl_store[HGL_BATCH_BYTES / 8];

void
hgl_batch_init(void)
{
	if (hgl_base != NULL)
		return;
	hgl_base = (unsigned char *)hgl_store;
	hgl_cur = hgl_base;
	hgl_end = hgl_base + HGL_BATCH_BYTES;
}

static unsigned long long
flush(int noreply)
{
	hgl_slot a[2];
	long len;
	unsigned long long r;

	if (hgl_base == NULL)
		hgl_batch_init();
	len = hgl_cur - hgl_base;
	if (len == 0)
		return 0;
	if (hgl_timing && !noreply) {
		const unsigned char *p = hgl_base;

		last_op = -1;
		while (p + 4 <= hgl_cur) {
			unsigned int h = *(const unsigned int *)p;
			unsigned int words = h & 0xffff;

			if (words == 0)
				break;
			last_op = (int)(h >> 16);
			p += words * 4;
		}
	}
	hgl_cur = hgl_base;
	if (!hgl_present())
		return 0;
	a[0].i = (long)(char *)hgl_base;
	a[1].i = len;
	r = (unsigned)hgl_call3(HGL_GL_BATCH, a, 0, noreply);
	return r;
}

unsigned long long
hgl_flush(void)
{
	return flush(0);
}

void
hgl_flush_nowait(void)
{
	flush(1);
}

void
hgl_flush_upload(void)
{
	flush(1);
}

void
hgl_flush_send(void)
{
	flush(1);
}

/* ---- client arrays ---- */

/* The current context's arrays (see "per-context client state"). */
#define arrays (cur_client->arrays)

GLfloat *hgl_feedback_pointer;
GLuint *hgl_select_pointer;

static int
array_of(GLenum cap)
{
	switch (cap) {
	case GL_VERTEX_ARRAY: return HGL_ARRAY_VERTEX;
	case GL_NORMAL_ARRAY: return HGL_ARRAY_NORMAL;
	case GL_COLOR_ARRAY: return HGL_ARRAY_COLOR;
	case GL_INDEX_ARRAY: return HGL_ARRAY_INDEX;
	case GL_TEXTURE_COORD_ARRAY: return HGL_ARRAY_TEXCOORD;
	case GL_EDGE_FLAG_ARRAY: return HGL_ARRAY_EDGEFLAG;
	}
	return -1;
}

void
hgl_array_pointer(int which, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	arrays[which].size = size;
	arrays[which].type = type;
	arrays[which].stride = stride;
	arrays[which].pointer = pointer;
}

void
hgl_client_state(GLenum cap, int on)
{
	int a = array_of(cap);

	if (a >= 0)
		arrays[a].enabled = on;
}

static unsigned
type_bytes(GLenum type)
{
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
	case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
	case GL_DOUBLE: return 8;
	}
	return 0;
}

/*
 * glInterleavedArrays (1.1 section 2.8, table 2.5): which arrays a format
 * holds, their sizes and offsets. Enables those and disables the others.
 */
void
hgl_interleaved(GLenum format, GLsizei stride, const GLvoid *pointer)
{
	/* et ec en st sc sv tc pc pn pv s */
	static const struct {
		GLenum format;
		int et, ec, en, st, sc, sv;
		GLenum tc;
	} f[] = {
		{ GL_V2F, 0, 0, 0, 0, 0, 2, 0 },
		{ GL_V3F, 0, 0, 0, 0, 0, 3, 0 },
		{ GL_C4UB_V2F, 0, 1, 0, 0, 4, 2, GL_UNSIGNED_BYTE },
		{ GL_C4UB_V3F, 0, 1, 0, 0, 4, 3, GL_UNSIGNED_BYTE },
		{ GL_C3F_V3F, 0, 1, 0, 0, 3, 3, GL_FLOAT },
		{ GL_N3F_V3F, 0, 0, 1, 0, 0, 3, 0 },
		{ GL_C4F_N3F_V3F, 0, 1, 1, 0, 4, 3, GL_FLOAT },
		{ GL_T2F_V3F, 1, 0, 0, 2, 0, 3, 0 },
		{ GL_T4F_V4F, 1, 0, 0, 4, 0, 4, 0 },
		{ GL_T2F_C4UB_V3F, 1, 1, 0, 2, 4, 3, GL_UNSIGNED_BYTE },
		{ GL_T2F_C3F_V3F, 1, 1, 0, 2, 3, 3, GL_FLOAT },
		{ GL_T2F_N3F_V3F, 1, 0, 1, 2, 0, 3, 0 },
		{ GL_T2F_C4F_N3F_V3F, 1, 1, 1, 2, 4, 3, GL_FLOAT },
		{ GL_T4F_C4F_N3F_V4F, 1, 1, 1, 4, 4, 4, GL_FLOAT },
	};
	const unsigned char *p = pointer;
	unsigned i, pc, pn, pv, s;

	for (i = 0; i < sizeof f / sizeof f[0]; i++)
		if (f[i].format == format)
			break;
	if (i == sizeof f / sizeof f[0])
		return;
	/* A C4UB colour is 4 bytes; other components are floats. */
	pc = f[i].st * 4;
	pn = pc + (f[i].tc == GL_UNSIGNED_BYTE ? 4 : f[i].sc * 4);
	pv = pn + (f[i].en ? 12 : 0);
	s = pv + f[i].sv * 4;
	if (stride == 0)
		stride = s;
	arrays[HGL_ARRAY_EDGEFLAG].enabled = 0;
	arrays[HGL_ARRAY_INDEX].enabled = 0;
	arrays[HGL_ARRAY_TEXCOORD].enabled = f[i].et;
	if (f[i].et)
		hgl_array_pointer(HGL_ARRAY_TEXCOORD, f[i].st, GL_FLOAT, stride, p);
	arrays[HGL_ARRAY_COLOR].enabled = f[i].ec;
	if (f[i].ec)
		hgl_array_pointer(HGL_ARRAY_COLOR, f[i].sc, f[i].tc, stride, p + pc);
	arrays[HGL_ARRAY_NORMAL].enabled = f[i].en;
	if (f[i].en)
		hgl_array_pointer(HGL_ARRAY_NORMAL, 3, GL_FLOAT, stride, p + pn);
	arrays[HGL_ARRAY_VERTEX].enabled = 1;
	hgl_array_pointer(HGL_ARRAY_VERTEX, f[i].sv, GL_FLOAT, stride, p + pv);
}

/* ARB_multitexture, which this image's gl.h does not declare. */
extern void glMultiTexCoord4fvARB(GLenum target, const GLfloat *v);

/* SGIS_multitexture's units, for programs that still use its names. */
#define HGL_TEXTURE0_ARB 0x84C0
#define HGL_TEXTURE0_SGIS 0x835E
#define HGL_TEXTURE1_SGIS 0x835F

/*
 * glClientActiveTextureARB, glSelectTextureSGIS: the unit the texture
 * coordinate calls act on. The units are numbered as the host numbers them
 * (src/exec.rs, texture_unit); one past HGL_TEX_UNITS is not recorded.
 */
void
hgl_client_unit(GLenum target)
{
	int u;

	if (target >= HGL_TEXTURE0_ARB && target < HGL_TEXTURE0_ARB + 32)
		u = target - HGL_TEXTURE0_ARB;
	else if (target == HGL_TEXTURE0_SGIS || target == HGL_TEXTURE1_SGIS)
		u = target - HGL_TEXTURE0_SGIS;
	else if (target < 32)
		u = target;
	else
		return;
	if (u >= HGL_TEX_UNITS || u == cur_client->client_unit)
		return;
	cur_client->tex_arrays[cur_client->client_unit] = arrays[HGL_ARRAY_TEXCOORD];
	arrays[HGL_ARRAY_TEXCOORD] = cur_client->tex_arrays[u];
	cur_client->client_unit = u;
}

int
hgl_client_unit_now(void)
{
	return cur_client->client_unit;
}

/* glDrawBuffer: whether drawing now goes to the front buffer. */
void
hgl_draw_buffer(GLenum mode)
{
	static int debug = -1, said;

	if (debug < 0)
		debug = getenv("IRIS_GL_DEBUG") != NULL;
	if (debug && said < 12) {
		said++;
		fprintf(stderr, "libGL: glDrawBuffer(0x%x), context %p\n", (unsigned)mode, cur_client->ctx);
	}
	/* Naming the front buffer is taken as drawing into it: a program asks for
	 * it to draw there. Inventor's viewers draw a frame into the front
	 * buffer and name the back one again before their glFlush. */
	cur_client->front_now = mode == GL_FRONT || mode == GL_FRONT_LEFT ||
	    mode == GL_FRONT_RIGHT || mode == GL_FRONT_AND_BACK || mode == GL_LEFT ||
	    mode == GL_RIGHT;
	if (cur_client->front_now)
		cur_client->front_drawn = 1;
}

/*
 * Colour-index contexts. The host draws RGBA only, so while one is current
 * an index is drawn as a colour: its low 8 bits the red component, the next
 * 4 the green -- exact, since n/255 converts back to n -- and the window's
 * pixels come back as indices (glXSwapBuffers). Logic ops then work on the
 * index's bits, as they should. 8-bit overlays use the red alone; IMPACT's
 * 12-bit colour-index windows use both. Shading between two indices is right
 * where their top 4 bits agree. The index state OpenGL keeps is kept here,
 * per context.
 */
int hgl_index_mode;
/* The index bits of the current context's windows: 8 for an overlay, 12 for
 * a colour-index window (glXMakeCurrent says). */
int hgl_index_bits = 8;

void
hgl_client_index(int on)
{
	struct hgl_client_state *c = cur_client;

	c->index_mode = on;
	hgl_index_mode = on;
	if (!on || c->index_ready)
		return;
	/* OpenGL's initial index state: current index 1, clear index 0, every
	 * bit writable. Dithering would move an index. */
	c->index_ready = 1;
	c->cur_index = 1;
	c->clear_index = 0;
	c->index_mask = ~0UL;
	glDisable(GL_DITHER);
	glColor4ub(1, 0, 0, 255);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void
hgl_index(double v)
{
	long i = (long)v;

	cur_client->cur_index = i;
	glColor4ub((GLubyte)(i & 0xff), (GLubyte)(i >> 8 & 0x0f), 0, 255);
}

void
hgl_clear_index(GLfloat c)
{
	long i = (long)c;

	cur_client->clear_index = i;
	glClearColor((GLfloat)(i & 0xff) / 255.0f, (GLfloat)(i >> 8 & 0x0f) / 255.0f, 0.0f, 1.0f);
}

/* Whole planes only: the host masks a component or not. */
void
hgl_index_mask(GLuint mask)
{
	GLboolean on = (mask & 0xfff) != 0;

	cur_client->index_mask = mask;
	glColorMask(on, on, on, on);
}

/* An index context's answer for `pname`, when it is index state. */
static int
index_state(GLenum pname, double *v)
{
	switch (pname) {
	case GL_INDEX_MODE: *v = 1; return 1;
	case GL_RGBA_MODE: *v = 0; return 1;
	case GL_INDEX_BITS: *v = hgl_index_bits; return 1;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
		*v = 0; return 1;
	case GL_CURRENT_INDEX: *v = (double)cur_client->cur_index; return 1;
	case GL_INDEX_CLEAR_VALUE: *v = (double)cur_client->clear_index; return 1;
	case GL_INDEX_WRITEMASK: *v = (double)(cur_client->index_mask & ((1UL << hgl_index_bits) - 1)); return 1;
	}
	return 0;
}

void hgl_index_geti(GLenum p, GLint *out) { double v; if (out && index_state(p, &v)) *out = (GLint)v; }
void hgl_index_getb(GLenum p, GLboolean *out) { double v; if (out && index_state(p, &v)) *out = v != 0; }
void hgl_index_getf(GLenum p, GLfloat *out) { double v; if (out && index_state(p, &v)) *out = (GLfloat)v; }
void hgl_index_getd(GLenum p, GLdouble *out) { double v; if (out && index_state(p, &v)) *out = v; }

int
hgl_front_drawn(int clear)
{
	int was = cur_client->front_now || cur_client->front_drawn;

	if (clear)
		cur_client->front_drawn = 0;
	return was;
}

/* Unit u's texture coordinate array. */
static const struct hgl_array *
tex_array(int u)
{
	if (u == cur_client->client_unit)
		return &arrays[HGL_ARRAY_TEXCOORD];
	return &cur_client->tex_arrays[u];
}

static const GLvoid *
element_of(const struct hgl_array *a, int edgeflag, GLint i)
{
	unsigned step = a->stride;

	if (step == 0)
		step = a->size * (edgeflag ? 1 : type_bytes(a->type));
	return (const char *)a->pointer + step * i;
}

static const GLvoid *
element(int which, GLint i)
{
	return element_of(&arrays[which], which == HGL_ARRAY_EDGEFLAG, i);
}

/* Nothing to record for a draw either (see hgl_ref). */
void
hgl_ref_draw_arrays(GLint first, GLsizei count)
{
	(void)first;
	(void)count;
}

void
hgl_ref_draw_elements(GLsizei count, GLenum type, const GLvoid *indices)
{
	(void)count;
	(void)type;
	(void)indices;
}

/*
 * Unit u's coordinates for element i, as glMultiTexCoord: glTexCoord is unit
 * 0's whichever unit is the client's. Missing components are 0, 0, 1 as the
 * short forms give them.
 */
static void
multi_tex_coord(int u, GLint i)
{
	const struct hgl_array *t = tex_array(u);
	const GLvoid *e;
	GLfloat v[4] = { 0, 0, 0, 1 };
	int c;

	if (!t->enabled || t->pointer == NULL || t->size < 1 || t->size > 4)
		return;
	e = element_of(t, 0, i);
	for (c = 0; c < t->size; c++) {
		switch (t->type) {
		case GL_SHORT: v[c] = ((const GLshort *)e)[c]; break;
		case GL_INT: v[c] = ((const GLint *)e)[c]; break;
		case GL_FLOAT: v[c] = ((const GLfloat *)e)[c]; break;
		case GL_DOUBLE: v[c] = ((const GLdouble *)e)[c]; break;
		default: return;
		}
	}
	glMultiTexCoord4fvARB(HGL_TEXTURE0_ARB + u, v);
}

/* glArrayElement: the per-vertex calls it stands for, in the order 2.8 gives. */
static void
array_element(GLint i)
{
	const struct hgl_array *t;
	const GLvoid *e;
	int u;

	if (arrays[HGL_ARRAY_EDGEFLAG].enabled && arrays[HGL_ARRAY_EDGEFLAG].pointer)
		glEdgeFlagv((const GLboolean *)element(HGL_ARRAY_EDGEFLAG, i));
	for (u = 1; u < HGL_TEX_UNITS; u++)
		multi_tex_coord(u, i);
	t = tex_array(0);
	if (t->enabled && t->pointer) {
		e = element_of(t, 0, i);
		switch (t->type * 8 + t->size) {
		case GL_SHORT * 8 + 1: glTexCoord1sv(e); break;
		case GL_SHORT * 8 + 2: glTexCoord2sv(e); break;
		case GL_SHORT * 8 + 3: glTexCoord3sv(e); break;
		case GL_SHORT * 8 + 4: glTexCoord4sv(e); break;
		case GL_INT * 8 + 1: glTexCoord1iv(e); break;
		case GL_INT * 8 + 2: glTexCoord2iv(e); break;
		case GL_INT * 8 + 3: glTexCoord3iv(e); break;
		case GL_INT * 8 + 4: glTexCoord4iv(e); break;
		case GL_FLOAT * 8 + 1: glTexCoord1fv(e); break;
		case GL_FLOAT * 8 + 2: glTexCoord2fv(e); break;
		case GL_FLOAT * 8 + 3: glTexCoord3fv(e); break;
		case GL_FLOAT * 8 + 4: glTexCoord4fv(e); break;
		case GL_DOUBLE * 8 + 1: glTexCoord1dv(e); break;
		case GL_DOUBLE * 8 + 2: glTexCoord2dv(e); break;
		case GL_DOUBLE * 8 + 3: glTexCoord3dv(e); break;
		case GL_DOUBLE * 8 + 4: glTexCoord4dv(e); break;
		}
	}
	if (arrays[HGL_ARRAY_COLOR].enabled && arrays[HGL_ARRAY_COLOR].pointer) {
		e = element(HGL_ARRAY_COLOR, i);
		switch (arrays[HGL_ARRAY_COLOR].type * 8 + arrays[HGL_ARRAY_COLOR].size) {
		case GL_BYTE * 8 + 3: glColor3bv(e); break;
		case GL_BYTE * 8 + 4: glColor4bv(e); break;
		case GL_UNSIGNED_BYTE * 8 + 3: glColor3ubv(e); break;
		case GL_UNSIGNED_BYTE * 8 + 4: glColor4ubv(e); break;
		case GL_SHORT * 8 + 3: glColor3sv(e); break;
		case GL_SHORT * 8 + 4: glColor4sv(e); break;
		case GL_UNSIGNED_SHORT * 8 + 3: glColor3usv(e); break;
		case GL_UNSIGNED_SHORT * 8 + 4: glColor4usv(e); break;
		case GL_INT * 8 + 3: glColor3iv(e); break;
		case GL_INT * 8 + 4: glColor4iv(e); break;
		case GL_UNSIGNED_INT * 8 + 3: glColor3uiv(e); break;
		case GL_UNSIGNED_INT * 8 + 4: glColor4uiv(e); break;
		case GL_FLOAT * 8 + 3: glColor3fv(e); break;
		case GL_FLOAT * 8 + 4: glColor4fv(e); break;
		case GL_DOUBLE * 8 + 3: glColor3dv(e); break;
		case GL_DOUBLE * 8 + 4: glColor4dv(e); break;
		}
	}
	if (arrays[HGL_ARRAY_INDEX].enabled && arrays[HGL_ARRAY_INDEX].pointer) {
		e = element(HGL_ARRAY_INDEX, i);
		switch (arrays[HGL_ARRAY_INDEX].type) {
		case GL_UNSIGNED_BYTE: glIndexubv(e); break;
		case GL_SHORT: glIndexsv(e); break;
		case GL_INT: glIndexiv(e); break;
		case GL_FLOAT: glIndexfv(e); break;
		case GL_DOUBLE: glIndexdv(e); break;
		}
	}
	if (arrays[HGL_ARRAY_NORMAL].enabled && arrays[HGL_ARRAY_NORMAL].pointer) {
		e = element(HGL_ARRAY_NORMAL, i);
		switch (arrays[HGL_ARRAY_NORMAL].type) {
		case GL_BYTE: glNormal3bv(e); break;
		case GL_SHORT: glNormal3sv(e); break;
		case GL_INT: glNormal3iv(e); break;
		case GL_FLOAT: glNormal3fv(e); break;
		case GL_DOUBLE: glNormal3dv(e); break;
		}
	}
	if (arrays[HGL_ARRAY_VERTEX].enabled && arrays[HGL_ARRAY_VERTEX].pointer) {
		e = element(HGL_ARRAY_VERTEX, i);
		switch (arrays[HGL_ARRAY_VERTEX].type * 8 + arrays[HGL_ARRAY_VERTEX].size) {
		case GL_SHORT * 8 + 2: glVertex2sv(e); break;
		case GL_SHORT * 8 + 3: glVertex3sv(e); break;
		case GL_SHORT * 8 + 4: glVertex4sv(e); break;
		case GL_INT * 8 + 2: glVertex2iv(e); break;
		case GL_INT * 8 + 3: glVertex3iv(e); break;
		case GL_INT * 8 + 4: glVertex4iv(e); break;
		case GL_FLOAT * 8 + 2: glVertex2fv(e); break;
		case GL_FLOAT * 8 + 3: glVertex3fv(e); break;
		case GL_FLOAT * 8 + 4: glVertex4fv(e); break;
		case GL_DOUBLE * 8 + 2: glVertex2dv(e); break;
		case GL_DOUBLE * 8 + 3: glVertex3dv(e); break;
		case GL_DOUBLE * 8 + 4: glVertex4dv(e); break;
		}
	}
}

void
glArrayElement(GLint i)
{
	array_element(i);
}

void
glArrayElementEXT(GLint i)
{
	array_element(i);
}

static void
get_pointer(GLenum pname, GLvoid **params)
{
	switch (pname) {
	case GL_VERTEX_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_VERTEX].pointer; break;
	case GL_NORMAL_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_NORMAL].pointer; break;
	case GL_COLOR_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_COLOR].pointer; break;
	case GL_INDEX_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_INDEX].pointer; break;
	case GL_TEXTURE_COORD_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_TEXCOORD].pointer; break;
	case GL_EDGE_FLAG_ARRAY_POINTER: *params = (GLvoid *)arrays[HGL_ARRAY_EDGEFLAG].pointer; break;
	case GL_FEEDBACK_BUFFER_POINTER: *params = hgl_feedback_pointer; break;
	case GL_SELECTION_BUFFER_POINTER: *params = hgl_select_pointer; break;
	default: *params = NULL; break;
	}
}

void
glGetPointerv(GLenum pname, GLvoid **params)
{
	get_pointer(pname, params);
}

void
glGetPointervEXT(GLenum pname, GLvoid **params)
{
	get_pointer(pname, params);
}

/* ---- size rules ---- */

/* glCallLists (1.1 section 5.4): bytes per list name by type. */
unsigned
hgl_calllists_bytes(GLenum type)
{
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
	case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: return 2;
	case GL_3_BYTES: return 3;
	case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: return 4;
	}
	return 0;
}

/*
 * Components of one evaluator control point (1.1 table 5.1), and of
 * SGIX_polynomial_ffd's deformations, which map points to points.
 */
unsigned
hgl_map_components(GLenum target)
{
	switch (target) {
	case GL_MAP1_INDEX: case GL_MAP2_INDEX:
	case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1:
		return 1;
	case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2:
		return 2;
	case GL_MAP1_NORMAL: case GL_MAP2_NORMAL:
	case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3:
	case GL_MAP1_VERTEX_3: case GL_MAP2_VERTEX_3:
	case GL_GEOMETRY_DEFORMATION_SGIX: case GL_TEXTURE_DEFORMATION_SGIX:
		return 3;
	case GL_MAP1_COLOR_4: case GL_MAP2_COLOR_4:
	case GL_MAP1_TEXTURE_COORD_4: case GL_MAP2_TEXTURE_COORD_4:
	case GL_MAP1_VERTEX_4: case GL_MAP2_VERTEX_4:
		return 4;
	}
	return 0;
}
