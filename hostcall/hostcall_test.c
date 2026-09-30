/*
 * hostcall_test -- prove IRIS host calls work from a real IRIX process.
 *
 * Uses the self-test service (3009). Checks, in order:
 *   ping      the trap is answered at all, and the registers come back
 *   sum       a 16 MB buffer IRIS must read, most pages not in the TLB
 *   fill      a fresh (never touched) buffer IRIS must write
 *   cow       a write in a forked child lands in the child's copy only
 *   stack     a read of the caller's own stack
 * On a real Indy or an IRIS without host calls it prints "absent" and exits 0.
 *
 * Output lines: "ok NAME ...", "FAIL NAME ...", and a summary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "hostcall.h"

static int failures;

static void
result(int good, const char *name, const char *fmt, long a, long b)
{
	printf("%s %s ", good ? "ok" : "FAIL", name);
	printf(fmt, a, b);
	printf("\n");
	if (!good)
		failures++;
}

static int
selftest(long long op, long long a1, long long a2, long long a3, long long *v0, long *retries)
{
	long long args[8], v1;

	memset(args, 0, sizeof args);
	args[0] = op;
	args[1] = a1;
	args[2] = a2;
	args[3] = a3;
	return iris_hostcall(HOSTCALL_SELFTEST, args, v0, &v1, retries);
}

int
main(void)
{
	long long v0, v1, args[8];
	long retries;
	unsigned long len, i;
	unsigned char *buf;
	unsigned long long want;
	pid_t pid;
	int status;

	memset(args, 0, sizeof args);
	args[1] = 5;
	args[2] = 6;
	if (iris_hostcall_trap(HOSTCALL_SELFTEST, args, &v0, &v1) != 0) {
		printf("absent (errno %lld): not running on IRIS with host calls\n", v0);
		return 0;
	}
	result(v0 == 0x1215 && v1 == 2, "ping", "v0=%#lx v1=%ld", (long)v0, (long)v1);

	/* sum: 16 MB with a known pattern */
	len = 16UL << 20;
	buf = malloc(len);
	for (i = 0, want = 0; i < len; i++) {
		buf[i] = (unsigned char)(i * 7 + 3);
		want += buf[i];
	}
	retries = 0;
	if (selftest(1, (long)buf, len, 0, &v0, &retries) != 0)
		result(0, "sum", "errno %ld%s", (long)v0, 0);
	else
		result((unsigned long long)v0 == want, "sum", "matches, %ld page retries%.0ld", retries, 0);

	/* fill: a buffer nothing has touched yet */
	len = 1UL << 20;
	buf = malloc(len);
	retries = 0;
	if (selftest(2, (long)buf, len, 0x5a, &v0, &retries) != 0) {
		result(0, "fill", "errno %ld%s", (long)v0, 0);
	} else {
		for (i = 0; i < len && buf[i] == 0x5a; i++)
			;
		result(i == len, "fill", "all bytes stored, %ld page retries%.0ld", retries, 0);
	}

	/* cow: parent's copy must not see the child's host-side write */
	len = 64UL << 10;
	buf = malloc(len);
	memset(buf, 1, len);
	pid = fork();
	if (pid == 0) {
		retries = 0;
		if (selftest(2, (long)buf, len, 7, &v0, &retries) != 0)
			_exit(2);
		for (i = 0; i < len; i++)
			if (buf[i] != 7)
				_exit(3);
		/* a copy-on-write page must have needed at least one write retry */
		_exit(retries > 0 ? 0 : 4);
	}
	waitpid(pid, &status, 0);
	for (i = 0; i < len && buf[i] == 1; i++)
		;
	result(WIFEXITED(status) && WEXITSTATUS(status) == 0 && i == len, "cow",
	    "child status %ld, parent intact bytes %ld", (long)(WIFEXITED(status) ? WEXITSTATUS(status) : -1), (long)i);

	/* stack */
	{
		unsigned char local[256];

		for (i = 0, want = 0; i < sizeof local; i++) {
			local[i] = (unsigned char)i;
			want += i;
		}
		result(selftest(1, (long)local, sizeof local, 0, &v0, 0) == 0 && (unsigned long long)v0 == want,
		    "stack", "sum %ld%.0ld", (long)v0, 0);
	}

	/* a kernel address must be refused as a page fault the caller cannot fix,
	 * not read: use the raw trap so the retry loop does not spin */
	memset(args, 0, sizeof args);
	args[0] = 1;
	args[1] = (long long)(long)0x88000000UL;
	args[2] = 16;
	result(iris_hostcall_trap(HOSTCALL_SELFTEST, args, &v0, &v1) == 1 && v0 == HOSTCALL_ENEEDPAGE,
	    "kernel-address", "refused (v0=%#lx)%.0ld", (long)v0, 0);

	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
