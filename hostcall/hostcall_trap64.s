/*
 * The IRIS host call trap for 64-bit programs.
 *
 * int iris_hostcall_trap64(long number, const long *args, long *v0, long *v1);
 *
 * The same indirect system call as hostcall_trap32.s -- v0 = 1000
 * (SYS_syscall), the host call's number in $4 and args[0..6] in $5-$11 --
 * with the arguments loaded whole: under the 64-bit ABI `long` and pointers
 * are eight bytes, and a program's addresses do not fit in 32 bits. The frame
 * is 16 bytes, the stack alignment this ABI keeps.
 *
 * Assembled by MIPSpro cc (cpp, then as): C comments only, no macros.
 */
	.text
	.set	noreorder
	.globl	iris_hostcall_trap64
	.ent	iris_hostcall_trap64
iris_hostcall_trap64:
	.frame	$sp, 16, $31
	daddiu	$sp, $sp, -16
	sd	$6, 0($sp)		/* where v0 goes */
	sd	$7, 8($sp)		/* where v1 goes */
	or	$24, $5, $0		/* args */
	ld	$5, 0($24)
	ld	$6, 8($24)
	ld	$7, 16($24)
	ld	$8, 24($24)
	ld	$9, 32($24)
	ld	$10, 40($24)
	ld	$11, 48($24)
	li	$2, 1000		/* SYS_syscall */
	syscall
	ld	$24, 0($sp)
	sd	$2, 0($24)
	ld	$24, 8($sp)
	sd	$3, 0($24)
	or	$2, $7, $0		/* return a3 */
	jr	$31
	daddiu	$sp, $sp, 16
	.end	iris_hostcall_trap64
