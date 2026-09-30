/*
 * The host call trap, in assembly, as IRIX's *indirect* system call: v0 =
 * 1000 (SYS_syscall), the host call's number in $4 and seven arguments in
 * $5-$11; then `syscall`, v0/v1 out, a3 returned.
 *
 * Indirect because of what a real kernel does when no IRIS is there to answer:
 * a direct call to an unknown number sends SIGSYS as well as failing with
 * EINVAL, while the indirect form only fails. So a library can probe for IRIS
 * without a signal handler.
 *
 * libc's syscall() cannot be used -- it drops v1, which carries the page
 * address of a need-page answer.
 *
 * int iris_hostcall_trap(long number, const long long *args,
 *                        long long *v0, long long *v1);
 * Built for n32 (32-bit pointers, 64-bit registers).
 */
__asm__(
"	.text\n"
"	.set	noreorder\n"
"	.globl	iris_hostcall_trap\n"
"	.type	iris_hostcall_trap, @function\n"
"iris_hostcall_trap:\n"
"	addiu	$sp, $sp, -16\n"
"	sw	$6, 0($sp)\n"		/* where v0 goes */
"	sw	$7, 4($sp)\n"		/* where v1 goes */
"	li	$2, 1000\n"		/* SYS_syscall */
"	move	$24, $5\n"		/* args */
"	ld	$5, 0($24)\n"
"	ld	$6, 8($24)\n"
"	ld	$7, 16($24)\n"
"	ld	$8, 24($24)\n"
"	ld	$9, 32($24)\n"
"	ld	$10, 40($24)\n"
"	ld	$11, 48($24)\n"
"	syscall\n"
"	lw	$24, 0($sp)\n"
"	sd	$2, 0($24)\n"
"	lw	$24, 4($sp)\n"
"	sd	$3, 0($24)\n"
"	move	$2, $7\n"		/* return a3 */
"	jr	$31\n"
"	addiu	$sp, $sp, 16\n"
"	.size	iris_hostcall_trap, .-iris_hostcall_trap\n"
);
