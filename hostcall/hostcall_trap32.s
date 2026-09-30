/*
 * The IRIS host call trap, for the shim libGL, in 32-bit instructions only.
 *
 * int iris_hostcall_trap32(long number, const long *args, long *v0, long *v1);
 *
 * IRIX's *indirect* system call: v0 = 1000 (SYS_syscall), the host call's
 * number in $4 and args[0..6] in $5-$11; then `syscall`, v0/v1 out, a3
 * returned. Indirect because a real kernel answers an unknown number given
 * that way with EINVAL alone, where a direct one also raises SIGSYS
 * (iris-hostcall/src/lib.rs has the whole contract).
 *
 * hostcall_trap.c, beside this file, is the same trap with 64-bit arguments
 * (`ld`/`sd`), which is right for n32 but a Reserved Instruction in an o32
 * process: IRIX runs o32 programs with 32-bit user mode. Every value this
 * library passes -- operations, 32-bit pointers, ids -- fits in 32 bits, so
 * this one version, `lw`/`sw` only, serves both ABIs. On n32 `lw` sign-extends
 * into the 64-bit register, which is how n32 keeps 32-bit values anyway.
 *
 * Both calling conventions put the four arguments in $4-$7, and neither needs
 * the caller's argument save area for a leaf like this. `li $2` stays right
 * before `syscall`, where the kernel's restart convention expects it.
 *
 * Assembled by MIPSpro cc (cpp, then as) and by GNU as, so the syntax is the
 * common subset: C comments only, no macros.
 */
	.text
	.set	noreorder
	.globl	iris_hostcall_trap32
	.ent	iris_hostcall_trap32
iris_hostcall_trap32:
	.frame	$sp, 16, $31
	addiu	$sp, $sp, -16
	sw	$6, 0($sp)		/* where v0 goes */
	sw	$7, 4($sp)		/* where v1 goes */
	or	$24, $5, $0		/* args */
	lw	$5, 0($24)
	lw	$6, 4($24)
	lw	$7, 8($24)
	lw	$8, 12($24)
	lw	$9, 16($24)
	lw	$10, 20($24)
	lw	$11, 24($24)
	li	$2, 1000		/* SYS_syscall */
	syscall
	lw	$24, 0($sp)
	sw	$2, 0($24)
	lw	$24, 4($sp)
	sw	$3, 0($24)
	or	$2, $7, $0		/* return a3 */
	jr	$31
	addiu	$sp, $sp, 16
	.end	iris_hostcall_trap32
