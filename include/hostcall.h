/*
 * hostcall.h -- calling IRIS host services from an IRIX program.
 *
 * A host call is a `syscall` with a number from 3000 to 3009, answered by IRIS
 * itself (iris-hostcall/src/lib.rs has the full contract). This header gives
 * the trap and the one piece of protocol a caller must implement: when IRIS
 * answers HOSTCALL_ENEEDPAGE, touch the page it names and call again.
 *
 * The trap uses IRIX's indirect system call (see hostcall_trap.c), so seven
 * arguments at most, and they always travel in registers, whatever the
 * program's ABI.
 */
#ifndef IRIS_HOSTCALL_H
#define IRIS_HOSTCALL_H

#define HOSTCALL_GL        3000
#define HOSTCALL_SELFTEST  3009
#define HOSTCALL_ENEEDPAGE 0x3000

/*
 * One trap; args[0..6] are used. Returns a3 (0 = success); *v0 and *v1 receive the result
 * registers. On a real Indy, or an IRIS without host calls, this returns 1
 * with *v0 = EINVAL.
 */
int iris_hostcall_trap(long number, const long long *args, long long *v0, long long *v1);

/*
 * A call, with need-page retries done. Returns 0 on success with the results
 * in *v0 and *v1, or -1 with the errno in *v0. *retries, if given, counts the
 * pages that had to be faulted in.
 */
static int
iris_hostcall(long number, const long long *args, long long *v0, long long *v1, long *retries)
{
	for (;;) {
		if (iris_hostcall_trap(number, args, v0, v1) == 0)
			return 0;
		if (*v0 != HOSTCALL_ENEEDPAGE)
			return -1;
		{
			/*
			 * Fault the page in the way any access would. volatile keeps
			 * the compiler from dropping a read whose value is unused or
			 * a write of a byte to itself.
			 */
			volatile char *p = (char *)(unsigned long)(*v1 & ~1LL);

			if (*v1 & 1)
				*p = *p;
			else
				(void)*p;
		}
		if (retries != 0)
			(*retries)++;
	}
}

#endif
