#!/bin/sh
#
# Build the n32 IRIS guest tools with our LLVM 21
# cross toolchain instead of MIPSpro on a guest, and package them for
# /opt/pkgsrc.
#
#   clang/build.sh [build|check|package|all]     (default: all)
#
# Paths come from the environment: LLVMBIN, SYSROOT and SGI, and WORK for
# the output (see common.sh).  Output: $WORK/build/ and
# $WORK/dist/iris-tools-irix65-n32.tgz.
#
# What is built (the n32 half of iris-guest-tools/build.sh's gl step, plus the
# host call test its cross step makes):
#
#   build/iris-tools/lib/libglshim.so  OpenGL + GLX, SONAME libGL.so, based at
#                                      0x00400000 (lld's IRIX default for PIC)
#   build/iris-tools/lib/libirisgl.so  IRIS GL, SONAME libgl.so, based at
#                                      0x00500000.  MIPSpro's registry put it
#                                      at 0x00480000, but lld pads segments to
#                                      64 KB and libglshim.so (with its 290 KB
#                                      bss) now ends at ~0x004aa000; the two
#                                      must not overlap (rld would have to
#                                      move one of them), and the check step
#                                      verifies they do not.
#   build/iris-tools/lib/libGLcore.so  the stand-in for SGI's (no functions,
#                                      needs libGL.so; gl/glcore_stub.c),
#                                      based at 0x00600000
#   build/iris-tools/bin/{glcheck,glbench,gltest,gloverlay,
#                         irisgltest,hostcall_test}
#
# Same SONAMEs, same NEEDED lists and the same exported symbol sets as the
# MIPSpro build (the check step compares them with this repository's
# build/gl/n32 when it is there).  Code: n32, MIPS III (like MIPSpro's -mips3:
# these libraries are loaded by every GL program, and must still load on an
# R4400), scheduled for the R10000 (-mtune=r10000).
#
# IRIS GL's stubs are made from the sysroot's usr/include/gl/gl.h (IRIX
# 6.5.7; identical to the MIPSpro build host's copy) into build/, as build.sh
# does from its build host.  Sources are read from this repository in
# place; nothing is written there.
#
# GL/gl.h vs gl/gl.h: IRIS GL's sources include both.  On this case-folding
# filesystem they are one path, so the IRIS GL compile goes through a clang
# VFS overlay (case-sensitive) that maps exactly those two names:
#   GL/gl.h -> $SGI/usr/include/GL/gl.h            (OpenGL, 6.5.7 gl_dev)
#   gl/gl.h -> $SYSROOT/usr/include/gl/gl.h        (IRIS GL)

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"

SRC=${IRIS_GUEST_TOOLS:-$GT}
OUT=$QC/build/iris-tools
DIST=$QC/dist
MIPSPRO=$SRC/build/gl/n32		# the MIPSpro build, for comparison

CFLAGS="-O2 -mtune=r10000 $COMMON_CFLAGS"
INCS="-I$SRC/include -I$SRC/gl -I$SRC/irisgl -I$SGI/usr/include"
LIBDIRS="-L$SGI/usr/lib32"

GLSHIM_BASE=0x00400000
IRISGL_BASE=0x00500000
GLCORE_BASE=0x00600000

build() {
	sgi_inputs
	rm -rf "$OUT"
	mkdir -p "$OUT/gen" "$OUT/lib" "$OUT/bin" "$OUT/obj" "$OUT/link"

	# --- IRIS GL stubs from the sysroot's gl.h (never kept) ---
	python3 "$SRC/tools/irisapi_from_gl_h.py" "$SYSROOT/usr/include/gl/gl.h" "$OUT/gen/irisapi.json" >/dev/null
	python3 "$SRC/tools/irisglshim.py" "$OUT/gen/irisapi.json" "$OUT/gen/irisgl_stubs.c"

	# --- the VFS overlay separating GL/gl.h from gl/gl.h ---
	cat > "$OUT/gen/gl-case.yaml" <<EOF
{ 'version': 0, 'case-sensitive': 'true', 'overlay-relative': false,
  'roots': [
    { 'name': '$OUT/gen/vfsinc/GL', 'type': 'directory',
      'contents': [ { 'name': 'gl.h', 'type': 'file',
                      'external-contents': '$SGI/usr/include/GL/gl.h' } ] },
    { 'name': '$OUT/gen/vfsinc/gl', 'type': 'directory',
      'contents': [ { 'name': 'gl.h', 'type': 'file',
                      'external-contents': '$SYSROOT/usr/include/gl/gl.h' } ] }
  ] }
EOF
	VFS="-ivfsoverlay $OUT/gen/gl-case.yaml -I$OUT/gen/vfsinc"

	echo "build-iris-tools: libglshim.so (libGL.so)"
	# shellcheck disable=SC2086
	"$CC" $CFLAGS $INCS -shared -Wl,-soname,libGL.so -Wl,--image-base=$GLSHIM_BASE \
	    -o "$OUT/lib/libglshim.so" \
	    "$SRC/gl/glshim_rt.c" "$SRC/gl/glshim_glx.c" "$SRC/gl/glshim_gen.c" \
	    "$SRC/gl/glshim_sgi.c" "$SRC/hostcall/hostcall_trap32.s" $LIBDIRS -lXext -lX11 -lm
	# for -lGL: a directory holding only libGL.so (libgl.so would be the same
	# name on this filesystem)
	ln -sf ../lib/libglshim.so "$OUT/link/libGL.so"

	echo "build-iris-tools: libGLcore.so (stand-in)"
	# shellcheck disable=SC2086
	"$CC" $CFLAGS -shared -Wl,-soname,libGLcore.so -Wl,--image-base=$GLCORE_BASE \
	    -Wl,--no-as-needed -o "$OUT/lib/libGLcore.so" "$SRC/gl/glcore_stub.c" -L"$OUT/link" -lGL

	echo "build-iris-tools: libirisgl.so (libgl.so)"
	# shellcheck disable=SC2086
	"$CC" $CFLAGS $VFS $INCS -shared -Wl,-soname,libgl.so -Wl,--image-base=$IRISGL_BASE \
	    -o "$OUT/lib/libirisgl.so" \
	    "$SRC/irisgl/irisgl_rt.c" "$SRC/irisgl/irisgl_draw.c" "$SRC/irisgl/irisgl_pixels.c" \
	    "$SRC/irisgl/irisgl_font.c" "$SRC/irisgl/irisgl_extra.c" "$SRC/irisgl/irisgl_nurbs.c" \
	    "$SRC/irisgl/irisgl_pup.c" "$OUT/gen/irisgl_stubs.c" \
	    "$OUT/lib/libglshim.so" $LIBDIRS -lGLU -lXext -lX11 -lm

	echo "build-iris-tools: tests"
	for t in glcheck glbench gltest gloverlay; do
		# shellcheck disable=SC2086
		"$CC" $CFLAGS $INCS -o "$OUT/bin/$t" "$SRC/gl/$t.c" -L"$OUT/link" $LIBDIRS -lGL -lX11 -lm
	done
	# shellcheck disable=SC2086
	"$CC" $CFLAGS $VFS $INCS -o "$OUT/bin/irisgltest" "$SRC/irisgl/irisgltest.c" \
	    "$OUT/lib/libirisgl.so" $LIBDIRS -lX11 -lm
	# the 64-bit-argument trap (ld/sd): n32 only, as in build.sh's cross step
	"$CC" $CFLAGS -I"$SRC/include" -o "$OUT/bin/hostcall_test" \
	    "$SRC/hostcall/hostcall_test.c" "$SRC/hostcall/hostcall_trap.c"
	# the traps on their own, for the disassembly comparison
	"$CC" $CFLAGS -c -o "$OUT/obj/hostcall_trap32.o" "$SRC/hostcall/hostcall_trap32.s"
	"$CC" $CFLAGS -c -o "$OUT/obj/hostcall_trap.o" "$SRC/hostcall/hostcall_trap.c"
	echo "build-iris-tools: built in $OUT"
}

# Address range of a library (every PT_LOAD, text through bss).
span() {
	SYSROOT="$SYSROOT" SGI="$SGI" python3 "$HERE/undef-check.py" span "$1"
}

# trapdis LIB: the instructions of iris_hostcall_trap32, from its dynamic
# symbol's address and size (MIPSpro's .symtab does not carry it).
trapdis() {
	set -- "$1" $("$READOBJ" --dyn-syms "$1" 2>/dev/null | awk '
		/Name: iris_hostcall_trap32 / {f=1} f && /Value:/ {v=$2} f && /Size:/ {print v, $2; exit}')
	"$OBJDUMP" -d --start-address=$(($2)) --stop-address=$(($2 + $3)) "$1" 2>/dev/null |
	    grep -E '^ +[0-9a-f]+:' | sed 's/^ *[0-9a-f]*:[[:space:]]*//'	# encoding + mnemonic
}

check() {
	status=0
	echo "== ELF checks"
	check_elf "$OUT"/lib/*.so "$OUT"/bin/* || status=1
	SYSROOT="$SYSROOT" SGI="$SGI" python3 "$HERE/undef-check.py" --shim "$OUT/lib/libglshim.so" --lib libgl.so="$OUT/lib/libirisgl.so" "$OUT"/lib/*.so "$OUT"/bin/* || status=1

	echo "== load ranges"
	for l in libglshim.so libirisgl.so libGLcore.so; do echo "   $l: $(span "$OUT/lib/$l")"; done
	g_hi=$(span "$OUT/lib/libglshim.so" | awk '{print $2}')
	i_lo=$(span "$OUT/lib/libirisgl.so" | awk '{print $1}')
	if [ $((g_hi)) -gt $((i_lo)) ]; then
		echo "   BAD: libglshim.so ends past libirisgl.so's base"; status=1
	fi

	if [ -d "$MIPSPRO" ]; then
		echo "== symbol sets vs MIPSpro ($MIPSPRO)"
		for l in libglshim.so libirisgl.so; do
			"$NM" -D --defined-only "$MIPSPRO/$l" 2>/dev/null | awk '{print $NF}' | sort -u > "$OUT/obj/$l.mipspro.syms"
			"$NM" -D --defined-only "$OUT/lib/$l" | awk '{print $NF}' | sort -u > "$OUT/obj/$l.clang.syms"
			only_m=$(comm -23 "$OUT/obj/$l.mipspro.syms" "$OUT/obj/$l.clang.syms")
			only_c=$(comm -13 "$OUT/obj/$l.mipspro.syms" "$OUT/obj/$l.clang.syms")
			echo "   $l: MIPSpro $(wc -l < "$OUT/obj/$l.mipspro.syms" | tr -d ' '), clang $(wc -l < "$OUT/obj/$l.clang.syms" | tr -d ' ')"
			[ -n "$only_m" ] && echo "     only in MIPSpro:" $only_m
			[ -n "$only_c" ] && echo "     only in clang:" $only_c
			for which in mipspro clang; do
				if [ $which = mipspro ]; then f=$MIPSPRO/$l; else f=$OUT/lib/$l; fi
				"$READOBJ" -d "$f" 2>/dev/null | awk '/NEEDED|SONAME/ {print $NF}' | tr '\n' ' ' > "$OUT/obj/$l.$which.dyn"
			done
			if ! cmp -s "$OUT/obj/$l.mipspro.dyn" "$OUT/obj/$l.clang.dyn"; then
				echo "     NEEDED/SONAME differ: MIPSpro $(cat "$OUT/obj/$l.mipspro.dyn") / clang $(cat "$OUT/obj/$l.clang.dyn")"
			fi
		done
		echo "== iris_hostcall_trap32: MIPSpro libglshim.so vs clang libglshim.so"
		trapdis "$MIPSPRO/libglshim.so" > "$OUT/obj/trap32.mipspro.dis"
		trapdis "$OUT/lib/libglshim.so" > "$OUT/obj/trap32.clang.dis"
		if cmp -s "$OUT/obj/trap32.mipspro.dis" "$OUT/obj/trap32.clang.dis"; then
			echo "   identical ($(wc -l < "$OUT/obj/trap32.clang.dis" | tr -d ' ') instructions):"
			sed 's/^/     /' "$OUT/obj/trap32.clang.dis"
		else
			echo "   DIFFERENT:"; diff "$OUT/obj/trap32.mipspro.dis" "$OUT/obj/trap32.clang.dis" | sed 's/^/     /'
			status=1
		fi
	fi
	return $status
}

# The tarball, rooted at /:
#   opt/pkgsrc/lib/iris-tools/lib32/libGL.so   (libglshim.so)
#   opt/pkgsrc/lib/iris-tools/lib32/libgl.so   (libirisgl.so)
#   opt/pkgsrc/lib/iris-tools/lib32/libGLcore.so
#   opt/pkgsrc/bin/{glcheck,...,hostcall_test}
#   opt/pkgsrc/share/iris-tools/install-iris-gl.sh
# libGL.so and libgl.so cannot sit side by side in a staging directory here,
# so they are staged under other names and renamed inside the archive.
package() {
	stage=$QC/build/stage-iris-tools
	rm -rf "$stage"
	mkdir -p "$stage/opt/pkgsrc/lib/iris-tools/lib32" "$stage/opt/pkgsrc/bin" "$stage/opt/pkgsrc/share/iris-tools"
	cp "$OUT/lib/libglshim.so" "$stage/opt/pkgsrc/lib/iris-tools/lib32/opengl.stage"
	cp "$OUT/lib/libirisgl.so" "$stage/opt/pkgsrc/lib/iris-tools/lib32/irisgl.stage"
	cp "$OUT/lib/libGLcore.so" "$stage/opt/pkgsrc/lib/iris-tools/lib32/libGLcore.so"
	for b in "$OUT"/bin/*; do cp "$b" "$stage/opt/pkgsrc/bin/"; done
	cp "$HERE/install-iris-gl.sh" "$stage/opt/pkgsrc/share/iris-tools/"
	chmod 755 "$stage"/opt/pkgsrc/lib/iris-tools/lib32/* "$stage"/opt/pkgsrc/bin/* \
	    "$stage/opt/pkgsrc/share/iris-tools/install-iris-gl.sh"
	mkdir -p "$DIST"
	(cd "$stage" && COPYFILE_DISABLE=1 tar --format ustar --uid 0 --gid 0 --uname root --gname sys \
	    -s ',lib32/opengl\.stage$,lib32/libGL.so,' -s ',lib32/irisgl\.stage$,lib32/libgl.so,' -czf "$DIST/iris-tools-irix65-n32.tgz" opt)
	echo "build-iris-tools: $DIST/iris-tools-irix65-n32.tgz"
	tar tvzf "$DIST/iris-tools-irix65-n32.tgz"
	# the archive must hold both libraries, each the right one
	for pair in libGL.so:libglshim.so libgl.so:libirisgl.so; do
		a=${pair%%:*}; b=${pair#*:}
		got=$(tar xOzf "$DIST/iris-tools-irix65-n32.tgz" "opt/pkgsrc/lib/iris-tools/lib32/$a" | shasum | cut -c1-40)
		want=$(shasum < "$OUT/lib/$b" | cut -c1-40)
		[ "$got" = "$want" ] || { echo "build-iris-tools: $a in the archive is not $b" >&2; exit 1; }
		echo "build-iris-tools: archive $a = $b ($want)"
	done
}

case "${1:-all}" in
build) build ;;
check) check ;;
package) package ;;
all) build; check; package ;;
*) echo "usage: $0 [build|check|package|all]" >&2; exit 2 ;;
esac
