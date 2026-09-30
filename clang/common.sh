# Sourced by build.sh: the toolchain, and the SGI files the build links
# against that an LLVM IRIX sysroot does not have.  Every path comes from the
# environment:
#
#   LLVMBIN  the LLVM IRIX cross toolchain's bin directory (clang, llvm-nm,
#            llvm-readobj, ...)
#   SYSROOT  an IRIX 6.5.7 root: libc, libm and the headers, including IRIS
#            GL's usr/include/gl/gl.h.  6.5.7 is the floor: what is built
#            here must run on 6.5.7 to 6.5.30.
#   SGI      SGI's files the sysroot lacks, laid out as on IRIX, from the
#            same release (and the same files a MIPSpro build host has):
#              usr/lib32/libX11.so, libXext.so   x_eoe.sw.eoe
#              usr/lib32/libGLU.so               eoe.sw.gfx
#              usr/include/GL/gl.h               gl_dev.sw.gldev (OpenGL's)
#            They are SGI's: link-time inputs only.  Nothing from $SGI goes
#            into a tarball (the package step copies named build outputs).
#   WORK     build output and packages (default build/clang here, which git
#            ignores)
#
# (GL/glx.h, glxtokens.h and glu.h are the sysroot's, already 6.5.7m.  The
# sysroot's usr/include/GL and usr/include/gl are one directory on a
# case-folding filesystem, holding IRIS GL's gl.h: see build.sh.)

GT=$(cd "$HERE/.." && pwd)
: "${LLVMBIN:?set LLVMBIN to the bin directory of the LLVM IRIX cross toolchain}"
: "${SYSROOT:?set SYSROOT to an IRIX 6.5.7 root}"
: "${SGI:?set SGI to a directory holding SGI libX11, libXext, libGLU and GL/gl.h (see clang/common.sh)}"
QC=${WORK:-$GT/build/clang}

CC=$LLVMBIN/clang
NM=$LLVMBIN/llvm-nm
READOBJ=$LLVMBIN/llvm-readobj
OBJDUMP=$LLVMBIN/llvm-objdump
STRIP=$LLVMBIN/llvm-strip

# Plain char is unsigned under MIPSpro (and IRIX's <ctype.h> macros index a
# table with it); clang's MIPS default is signed.
# No type-based alias analysis: MIPSpro -O2 does not do it, and this code
# (the GL command encoders) was only ever built without it.
COMMON_CFLAGS="-funsigned-char -fno-strict-aliasing -fcommon"

# sgi_inputs: fail early, and say what is missing, when $SGI lacks a file.
sgi_inputs() {
	for f in usr/lib32/libX11.so usr/lib32/libXext.so usr/lib32/libGLU.so usr/include/GL/gl.h; do
		[ -e "$SGI/$f" ] || { echo "clang/build.sh: $SGI/$f is missing (see clang/common.sh)" >&2; exit 1; }
	done
}

# check_elf FILE...: what the report needs for every binary and library.
# Fails on: DT_RUNPATH (IRIX 6.5 rld ignores it), __stack_chk_* (IRIX libc has
# no stack protector), binary128 long double helpers (IRIX has none), and any
# undefined symbol matching $CHECK_DENY.
check_elf() {
	bad=0
	for f in "$@"; do
		echo "== $(basename "$f")"
		file "$f" | sed 's/^[^:]*: /   /'
		"$READOBJ" -d "$f" 2>/dev/null | grep -E 'NEEDED|SONAME|RPATH|RUNPATH|BASE_ADDRESS|MIPS_FLAGS' | sed 's/^ */   /'
		if "$READOBJ" -d "$f" 2>/dev/null | grep -q RUNPATH; then
			echo "   BAD: DT_RUNPATH"; bad=1
		fi
		u=$("$NM" -D -u "$f" 2>/dev/null | awk '{print $NF}')
		x=$(echo "$u" | grep -E '^__stack_chk|^__(add|sub|mul|div|neg|eq|ne|lt|le|gt|ge|unord|cmp)tf[23]|^__(extend|trunc)[a-z]*tf|^__fix[a-z]*tf|^__float[a-z]*tf|^__emutls' || true)
		if [ -n "$x" ]; then
			echo "   BAD undefined:" $x; bad=1
		fi
		echo "   undefined: $(echo "$u" | grep -c .)  defined (dynamic): $("$NM" -D --defined-only "$f" 2>/dev/null | wc -l | tr -d ' ')"
	done
	return $bad
}
