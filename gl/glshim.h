/*
 * Host OpenGL for IRIX programs under IRIS: what the shim libGL's parts share.
 *
 * GL calls are not made one host call at a time. Each is encoded into a
 * command buffer in this process, and the buffer goes to the host in one
 * call -- HGL_GL_BATCH -- when something needs the host: a call that returns
 * a value or writes through a pointer, one that reads pixel data or client
 * arrays in place, a buffer swap, glFlush/glFinish, or a full buffer. The
 * host (iris-hostgl/src) then runs the calls in order on its own OpenGL.
 *
 * A command is 8-byte aligned: a header word, (entry point << 16) | length
 * in 4-byte words including the header, then the arguments in declaration
 * order. Integers of every width, enums and GLboolean take 4 bytes; GLfloat
 * 4; GLdouble 8, at an offset that is a multiple of 8, so it can be stored
 * directly. A pointer the host reads or writes where it is takes 4 bytes, the
 * guest address. An array copied into the command is a count of elements,
 * then the elements' bytes as they are in memory, padded to 4 -- unless it is
 * bigger than HGL_INLINE_MAX, when the count has HGL_BYREF set and the
 * guest address follows, and the buffer is flushed before the array can
 * change. Everything is in the guest's (big-endian) order; the host swaps.
 *
 * Every command that makes the host read or write this process's memory
 * outside the buffer itself flushes the buffer, so it is always the last
 * command of a batch. The host relies on that (see src/service.rs).
 *
 * The GLX operations that are not GL calls have their own numbers,
 * HGL_GL_CREATE and friends, and go through the same host call with 8-byte
 * argument slots, flushing the buffer first.
 *
 * The host call is IRIS's private system call 3000 (iris-hostcall), made
 * with the indirect `syscall` so that on a real Indy -- or an IRIS without
 * host GL -- it only fails with EINVAL. Its registers:
 *
 *   $5  operation         $6  address of the slots (or, for HELLO, the
 *   $7  client id             protocol version this library speaks)
 *   $8  call serial
 *
 * The client id comes from HGL_GL_HELLO and names this process to the host;
 * a forked child asks for its own. The serial makes every call distinct, so
 * a retry after a need-page answer is recognised as the same call and never
 * runs twice.
 */
#ifndef HGL_GLSHIM_H
#define HGL_GLSHIM_H

#include <stdio.h>
#include <string.h>

/* IRIS's host GL service, and its need-page answer (iris-hostcall). */
#define HGL_HOSTCALL_GL        3000
#define HGL_HOSTCALL_ENEEDPAGE 0x3000

/* The protocol this library speaks; the host answers HELLO with its own. */
#define HGL_PROTOCOL 2

#define HGL_GL_HELLO       0x10000 /* (register) protocol -> client id, v1 = host's protocol */
#define HGL_GL_CREATE      0x10001 /* shareList -> context id */
#define HGL_GL_MAKECURRENT 0x10002 /* ctx, draw, read, width, height */
#define HGL_GL_SWAP        0x10003 /* pixel buffer, width, height -> presented, v1 = ns to wait */
#define HGL_GL_DESTROY     0x10004 /* ctx */
#define HGL_GL_GETSTRING   0x10005 /* name, buffer, length */
#define HGL_GL_RELEASE     0x10006 /* no context current */
#define HGL_GL_FINISH      0x10007
#define HGL_GL_BATCH       0x10008 /* buffer, length -> the last call's value */
#define HGL_GL_PBUFFER     0x10009 /* width, height, samples -> a drawable id */
#define HGL_GL_DRAWABLE_GONE 0x1000A /* drawable */
#define HGL_GL_SWAP_INTERVAL 0x1000B /* frames between swaps */
#define HGL_GL_VIDEO_SYNC  0x1000C /* wait?, divisor, remainder -> frame count, v1 = ns to wait */
#define HGL_GL_GLX_STRING  0x1000D /* which, buffer, length */
#define HGL_GL_GOODBYE     0x1000E /* this client's contexts and drawables go */
#define HGL_GL_STATS       0x1000F /* -> frames presented, v1 = frames replaced */

typedef union {
	long long i;
	double d;
} hgl_slot;

/*
 * The trap itself (guest-tools/hostcall/hostcall_trap32.s): IRIX's indirect system call with
 * `number` in $4 and args[0..6] in $5-$11. 32-bit instructions only, so the
 * same code serves o32 and n32. Returns a3; *v0 and *v1 get the result
 * registers.
 */
extern int iris_hostcall_trap32(long number, const long *args, long *v0, long *v1);
/* The same for 64-bit programs (hostcall_trap64.s): arguments loaded whole. */
extern int iris_hostcall_trap64(long number, const long *args, long *v0, long *v1);
#if defined(_MIPS_SZLONG) && _MIPS_SZLONG == 64
#define HGL_TRAP iris_hostcall_trap64
#else
#define HGL_TRAP iris_hostcall_trap32
#endif

/*
 * A host GL operation with its slots, need-page retries done. The first
 * result register (zero-extended), or -1 if the call failed or the host is
 * not there. *v1, if not null, gets the second result.
 */
extern long long hgl_call2(long op, hgl_slot *slots, long long *v1);
#define hgl_call(op, slots) hgl_call2((op), (slots), (long long *)0)

/*
 * Whether the host GL service answers. The first time it does not, says so
 * once on stderr; everything that needs the host then does nothing.
 */
extern int hgl_present(void);

/* Wait `ns` nanoseconds, as a host answer asked (swap interval, video sync). */
extern void hgl_wait(long long ns);

/*
 * IRIS_GL_SWAP: tell the host what this process's swaps should do when the
 * environment asked for something other than what the program asks for
 * (glshim_glx.c). Called once, after the host knows this client.
 */
extern void hgl_swap_setup(void);

/* The transport this process uses, once hgl_present has answered 1. */
#define HGL_TRANSPORT_NONE     0
#define HGL_TRANSPORT_HOSTCALL 1
extern int hgl_transport;
/* "hostcall" or "none". */
extern const char *hgl_transport_name(void);

/*
 * IRIS_GL_STATS: time the round trips (a call that waits for the host). Only
 * such calls are timed, so the clock is read a handful of times a frame.
 */
extern int hgl_timing;
extern double hgl_now_us(void);
/* One op class's round trips: see hgl_time_add / hgl_time_print. */
typedef struct {
	unsigned long n;
	double total_us, max_us;
	unsigned long bucket[6];        /* <0.1 <0.5 <1 <2 <5 >=5 ms */
	unsigned long wrapped;          /* samples the cycle counter wrapped */
} hgl_times;
extern void hgl_time_add(hgl_times *t, double us);
extern void hgl_time_print(FILE *f, const char *what, const hgl_times *t);
/*
 * Memory the host will read when the buffer runs, reported by the encoders
 * after they place a command: `len` bytes at `addr`. The host call reads
 * this process's memory itself, so nothing is kept.
 */
extern void hgl_ref(const char *addr, unsigned long len);

/*
 * The command buffer. Small enough that a whole batch's pages fit in the
 * R4400's 48 TLB entries (the host reads it a page at a time through the TLB,
 * and a page that is not there costs a need-page retry); big enough that a
 * frame of immediate-mode calls is a handful of flushes.
 */
#define HGL_BATCH_BYTES (128 << 10)
#define HGL_INLINE_MAX (16 << 10)
#define HGL_BYREF 0x80000000u

extern unsigned char *hgl_cur;
extern unsigned char *hgl_end;

/* Run the buffered calls; the value the last of them returned. */
extern unsigned long long hgl_flush(void);
/*
 * Send the buffered calls without waiting for them to run, where nothing
 * needs their result: when the buffer is full, after by-reference arrays
 * (all recorded with hgl_ref), and right before a call that waits anyway.
 * The same as hgl_flush over the host call.
 */
extern void hgl_flush_nowait(void);
/*
 * After a command that reads memory but gives nothing back (an upload, a
 * draw, a by-reference array): without waiting if everything it reads was
 * recorded exactly, else as hgl_flush.
 */
extern void hgl_flush_upload(void);
/* hgl_flush_nowait, and nothing held back in this process either (glFlush). */
extern void hgl_flush_send(void);
/* Make sure the buffer exists; called before the first command. */
extern void hgl_batch_init(void);

/*
 * Room for a command of `bytes` (a multiple of 8), with its header written.
 * A command bigger than the whole buffer cannot happen: arrays past
 * HGL_INLINE_MAX go by reference.
 */
#define HGL_CMD(p, op, bytes) do { \
	if (hgl_cur == NULL) \
		hgl_batch_init(); \
	if ((unsigned long)(hgl_end - hgl_cur) < (unsigned long)(bytes)) \
		hgl_flush_nowait(); \
	(p) = hgl_cur; \
	hgl_cur += (bytes); \
	*(unsigned int *)(void *)(p) = ((unsigned)(op) << 16) | ((unsigned)(bytes) >> 2); \
} while (0)

/* Native order is the wire order: the guest is big-endian. */
#define HGL_PUT_I(p, off, v) (*(int *)(void *)((p) + (off)) = (int)(v))
#define HGL_PUT_F(p, off, v) (*(float *)(void *)((p) + (off)) = (float)(v))
#define HGL_PUT_D(p, off, v) (*(double *)(void *)((p) + (off)) = (double)(v))
#define HGL_ROUND8(n) (((n) + 7) & ~7)
/*
 * A guest address, as eight bytes, high word first (protocol 2). A 64-bit
 * program's addresses do not fit in 32 bits (its heap and stack sit above
 * 4 GB); a 32-bit program's are sent zero-extended. Also used for an inline
 * array's offset, which shares the field.
 */
#define HGL_PUT_A(p, off, v) do { \
	unsigned long long _a = (unsigned long long)(unsigned long)(v); \
	HGL_PUT_I(p, off, (unsigned)(_a >> 32)); \
	HGL_PUT_I(p, (off) + 4, (unsigned)_a); \
} while (0)

/*
 * An array argument: in the command if it fits, else by reference. `at` is
 * where the count goes; returns the bytes the array took after the count.
 * `room` is what the command reserved for it (the inline size, rounded).
 */
#define HGL_PUT_ARRAY(p, at, ptr, count, elem_bytes, byref) do { \
	unsigned _n = (unsigned)(count); \
	if (byref) { \
		HGL_PUT_I(p, at, _n | HGL_BYREF); \
		HGL_PUT_A(p, (at) + 4, (char *)(ptr)); \
	} else { \
		HGL_PUT_I(p, at, _n); \
		if (_n && (ptr)) \
			memcpy((p) + (at) + 4, (ptr), _n * (elem_bytes)); \
	} \
} while (0)

#endif
