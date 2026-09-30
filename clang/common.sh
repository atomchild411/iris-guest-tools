# Sourced by build.sh: the toolchain, and the
# SGI files the builds link against that the LLVM sysroot does not have.
#
# The sysroot (scratch/llvm-irix/root, IRIX 6.5.7) has libc, libm, libgen,
# libdmedia and most headers, but not libX11, libXext, libGLU or libaudio,
# nor dmedia/audio.h; and its usr/include/GL and usr/include/gl are one
# directory on this case-folding filesystem, holding IRIS GL's gl.h, not
# OpenGL's.  The rest comes from the IRIX 6.5.7 CDs in media/ (read-only),
# taken with tools/sgidist.py into $SGI -- the same
# release as the sysroot (our floor: binaries must run on 6.5.7 to 6.5.30),
# and the same files the MIPSpro build hosts had:
#
#   usr/lib32/libX11.so.1    x_eoe.sw.eoe         Overlays (2-2), x_eoe_657m
#   usr/lib32/libXext.so     x_eoe.sw.eoe         Overlays (2-2), x_eoe_657m
#   usr/lib32/libGLU.so      eoe.sw.gfx           Installation Tools & Overlays (1-2), eoe_657m
#   usr/lib32/libaudio.so.1  dmedia_eoe.sw.audio  Foundations (1-2), dmedia_eoe
#   usr/include/GL/gl.h      gl_dev.sw.gldev      Development Libraries, gl_dev
#   usr/include/dmedia/{audio,dmedia,cdaudio}.h  dmedia_dev.sw.base, Development Libraries
#
# (GL/glx.h, glxtokens.h and glu.h are the sysroot's, already 6.5.7m.)
# They are SGI's: link-time inputs only.  Nothing from $SGI goes into a
# tarball (the package steps copy named build outputs, never this tree).

# This repository, the workspace it sits in (two levels up), and the
# work directory: build output, packages and the extracted SGI inputs, never
# inside the repository.
GT=$(cd "$HERE/.." && pwd)
WS=${WS:-$(cd "$GT/../.." && pwd)}
QC=${WORK:-$WS/scratch/q-clang}
LLVMBIN=${LLVMBIN:-$WS/scratch/llvm-irix/cross21/bin}
SYSROOT=${SYSROOT:-$WS/scratch/llvm-irix/root}
SGI=$QC/sgi-657
SGIDIST=$GT/tools/sgidist.py
MEDIA=${MEDIA:-$WS/media/irix-6.5.7}

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

sgi_get() {	# sgi_get ISO IDB PATH
	[ -s "$SGI/$3" ] && return 0
	python3 "$SGIDIST" get "$MEDIA/$1" "$2" "$3" '' "$SGI/$3" >/dev/null
}

sgi_inputs() {
	mkdir -p "$SGI/usr/lib32" "$SGI/usr/include/GL" "$SGI/usr/include/dmedia"
	sgi_get "Overlays (2-2).iso" /dist/x_eoe_657m.idb usr/lib32/libX11.so.1
	sgi_get "Overlays (2-2).iso" /dist/x_eoe_657m.idb usr/lib32/libXext.so
	sgi_get "Installation Tools & Overlays (1-2).iso" /dist/eoe_657m.idb usr/lib32/libGLU.so
	sgi_get "Foundations (1-2).iso" /dist/dmedia_eoe.idb usr/lib32/libaudio.so.1
	sgi_get "Development Libraries.iso" /dist/gl_dev.idb usr/include/GL/gl.h
	for h in audio dmedia cdaudio; do
		sgi_get "Development Libraries.iso" /dist/dmedia_dev.idb usr/include/dmedia/$h.h
	done
	ln -sf libX11.so.1 "$SGI/usr/lib32/libX11.so"
	ln -sf libaudio.so.1 "$SGI/usr/lib32/libaudio.so"
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
