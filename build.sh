#!/bin/sh
#
# Build everything IRIS runs inside IRIX, into build/:
#
#   build/gl/n32/         libglshim.so (OpenGL + GLX, SONAME libGL.so,
#                         -> /usr/lib32/libGL.so), libirisgl.so (IRIS GL,
#                         SONAME libgl.so, -> /usr/lib32/libgl.so),
#                         libGLcore.so (a stand-in for SGI's, which holds
#                         its GL functions: no functions, needs libGL.so;
#                         see gl/glcore_stub.c), the
#                         tests glcheck, glbench, gltest, the
#                         overlay demo gloverlay, and the IRIS GL test irisgltest
#   build/gl/install-gl.sh  puts them in place in a guest (see the script)
#   build/gl/o32/         the same for o32 programs (-> /usr/lib/...)
#   build/gl/64/          libglshim.so, libGLcore.so and the tests for 64-bit
#                         programs (-> /usr/lib64/libGL.so); IRIX has no
#                         64-bit IRIS GL
#   build/gl/n32-cross/   libglshim.so and glcheck from the Mac cross compiler
#   build/hostcall/       hostcall_test (n32, cross compiler)
#   build/*/mipspro.log   compiler output of each MIPSpro step
#
#   ./build.sh [gl|cross|all]      (default: all)
#
# Sources:
#   include/     the wire contract shared with IRIS's host crates:
#                hostcall.h (iris-hostcall)
#   hostcall/    the host call trap: hostcall_trap.c (64-bit arguments, n32
#                only), hostcall_trap32.s (32-bit, o32 and n32; libGL's),
#                hostcall_trap64.s (the 64-bit ABI; libGL's), and
#                hostcall_test.c
#   gl/          the replacement libGL and its tests. glshim_ops.h and
#                glshim_gen.c are generated in IRIS, with its
#                iris-hostgl/src/calls.rs, by iris-hostgl/tools/glshim.py, and
#                copied here: regenerate there, never edit
#   irisgl/      the replacement IRIS GL; its stubs are made at build time
#                (irisgl_generate, below)
#   tools/       irisapi_from_gl_h.py and irisglshim.py, which make them
#
# MIPSpro step (gl): MIPSpro 7.4 `cc` on a real IRIX 6.5 host, reached
# with $IRIX_SSH -- a command that runs a shell there, e.g.
#     IRIX_SSH="ssh -o Port=2232 irix" ./build.sh
# Each step's sources travel flat, as a ustar+gzip stream on ssh's stdin, into
# $IRIS_GUEST_REMOTE/<step> (default /var/tmp/iris-guest-build), which is
# removed afterwards. Nothing is loaded or installed on that host. Without
# $IRIX_SSH, `all` skips these steps and naming one is an error.
#
# gl: libglshim.so reaches the host through host call 3000. The libraries replace SGI's
# files outright, under SGI's SONAMEs: IRIX binds libGL.so and libgl.so
# directly, so preloading (_RLD_LIST) does not intercept them, and a symbol
# this library did not define would pull SGI's own library (and its
# graphics-board probing) back in. Install by copying over the originals (keep
# those somewhere to go back to); nothing else is needed. The two names differ
# only in case, so do not copy both through a filesystem that folds case
# (macOS's does). The gl step also copies that host's OpenGL headers to
# build/gl/cross-include for the cross step.
#
# Load addresses: IRIX shared libraries are linked at fixed addresses from a
# so_locations registry, and rld only relocates a library whose range is taken.
#   - Both libraries must be linked with the *same* registry
#     (-update_registry): two linked separately both take the default base,
#     and the second then faults before main with no message at all.
#   - n32: a fresh registry, which bases them at 0x400000 -- below where n32
#     programs are linked (0x10000000).
#   - o32: a copy of the build host's /usr/lib/so_locations. With an empty
#     registry the library lands at 0x400000 -- exactly where o32 *programs*
#     are linked -- and rld's attempt to move it ends in a memory fault.
#     Built against IRIX 6.5.7's registry; a system whose SGI libraries sit
#     elsewhere may need the build redone against its own copy
#     (IRIX_SO_LOCATIONS_O32=path-on-the-build-host). SGI's range for
#     libgl.so is 0x160000 bytes at 0x0f000000, text, data and bss
#     together: large static tables in the IRIS GL library can outgrow it.
#
# cross: $IRIX_GCC (default tools/irix-cross/irix-gcc in the workspace, two
# directories up),
# clang/zig + GNU ld for n32. Builds the hostcall test, and libglshim.so and
# glcheck when build/gl/cross-include holds the OpenGL headers (the gl step
# fetches them; the cross sysroot has only IRIS GL's gl.h, which shares the
# path GL/gl.h on a case-folding filesystem). IRIS GL is not cross-built: it
# needs both <GL/gl.h> and <gl/gl.h>, which such a filesystem cannot hold side
# by side. NOTE: GNU ld's dynamic sections are not SGI's quickstart layout;
# the cross glcheck runs only under minirld, and whether SGI's rld loads the
# cross libglshim.so into SGI programs is untested. Use the MIPSpro libraries
# for SGI programs. hostcall_trap.c is GNU-style file-scope asm, which is why
# the hostcall test is built here and not with MIPSpro.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$HERE/build
REMOTE_BASE=${IRIS_GUEST_REMOTE:-/var/tmp/iris-guest-build}
TARGET=${1:-all}

# Whether a MIPSpro step can run; under `all` a missing $IRIX_SSH skips it.
have_irix() {
	[ -n "${IRIX_SSH:-}" ] && return 0
	if [ "$TARGET" = all ]; then
		echo "build.sh: IRIX_SSH is not set: skipping the MIPSpro $1 build"
		return 1
	fi
	echo "build.sh: set IRIX_SSH to a command that runs a shell on an IRIX host with MIPSpro" >&2
	exit 2
}

# pack OUT FILE...: FILEs (paths in this project, or absolute for what the
# build made in build/) as one flat ustar+gzip stream,
# OUT/src.tar.gz. Flat because the sources include each other by bare name.
pack() {
	out=$1
	shift
	stage=$(mktemp -d)
	for f in "$@"; do
		case $f in
		/*) cp "$f" "$stage/" ;;         # made in build/ (see irisgl_generate)
		*) cp "$HERE/$f" "$stage/" ;;
		esac
	done
	(cd "$stage" && COPYFILE_DISABLE=1 tar --format ustar -czf "$out/src.tar.gz" *)
	rm -rf "$stage"
}

# remote OUT STEP ARG...: ship OUT/src.tar.gz to the IRIX host, run
# OUT/remote.sh there as `sh -s <dir> ARG...`, and unpack what it left in
# out.tar.gz into OUT/out. Compiler output goes to OUT/mipspro.log.
remote() {
	out=$1
	step=$2
	shift 2
	dir=$REMOTE_BASE/$step
	echo "build.sh: $step: sending sources to the IRIX host ($dir)"
	# Transfers over ssh to an emulated guest now and then arrive short, so
	# each is checked by its size and made again.
	for try in 1 2 3; do
		$IRIX_SSH "rm -rf $dir && mkdir -p $dir && cat > $dir/src.tar.gz" < "$out/src.tar.gz"
		[ "$($IRIX_SSH "wc -c < $dir/src.tar.gz" | tr -d ' \r')" = "$(wc -c < "$out/src.tar.gz" | tr -d ' ')" ] && break
		echo "build.sh: $step: the sources arrived short; sending them again"
	done
	echo "build.sh: $step: compiling there"
	if ! $IRIX_SSH "sh -s $dir $*" < "$out/remote.sh" > "$out/mipspro.log" 2>&1; then
		cat "$out/mipspro.log"
		echo "build.sh: $step: the MIPSpro build failed (log: $out/mipspro.log)"
		$IRIX_SSH "rm -rf $dir" || true
		exit 1
	fi
	rm -rf "$out/out"
	for try in 1 2 3; do
		$IRIX_SSH "cat $dir/out.tar.gz" > "$out/out.tar.gz"
		[ "$($IRIX_SSH "wc -c < $dir/out.tar.gz" | tr -d ' \r')" = "$(wc -c < "$out/out.tar.gz" | tr -d ' ')" ] && break
		echo "build.sh: $step: the build came back short; fetching it again"
	done
	(cd "$out" && tar xzf out.tar.gz)
	rm -f "$out/out.tar.gz"
	$IRIX_SSH "rm -rf $dir"
	rm -f "$out/src.tar.gz" "$out/remote.sh"
}

# irisgl_generate OUT: IRIS GL's stubs, made from the build host's own
# /usr/include/gl/gl.h into OUT/gen: tools/irisapi_from_gl_h.py lists its
# entry points, tools/irisglshim.py writes a stub for each one not written by
# hand. Neither the header nor anything made from it goes into the
# repository; they are rebuilt from the IRIX host every build.
irisgl_generate() {
	gen=$1/gen
	mkdir -p "$gen"
	for try in 1 2 3; do
		$IRIX_SSH "cat /usr/include/gl/gl.h" > "$gen/gl.h"
		[ "$($IRIX_SSH "wc -c < /usr/include/gl/gl.h" | tr -d ' \r')" = "$(wc -c < "$gen/gl.h" | tr -d ' ')" ] && break
		echo "build.sh: gl: gl.h arrived short; fetching it again"
	done
	[ -s "$gen/gl.h" ] || { echo "build.sh: gl: no /usr/include/gl/gl.h on the IRIX host (gl_dev.sw.gldev)" >&2; exit 1; }
	python3 "$HERE/tools/irisapi_from_gl_h.py" "$gen/gl.h" "$gen/irisapi.json" >/dev/null
	python3 "$HERE/tools/irisglshim.py" "$gen/irisapi.json" "$gen/irisgl_stubs.c"
}

gl() {
	have_irix gl || return 0
	out=$BUILD/gl
	mkdir -p "$out"
	irisgl_generate "$out"
	pack "$out" hostcall/hostcall_trap32.s hostcall/hostcall_trap64.s \
	    gl/glshim.h gl/glshim_ops.h gl/glshim_rt.h gl/glshim_rt.c gl/glshim_glx.c \
	    gl/glshim_gen.c gl/glshim_sgi.c gl/glcore_stub.c \
	    irisgl/irisgl_shim.h irisgl/irisgl_rt.c irisgl/irisgl_draw.c irisgl/irisgl_pixels.c \
	    irisgl/irisgl_font.c irisgl/irisgl_extra.c irisgl/irisgl_nurbs.c irisgl/irisgl_pup.c "$out/gen/irisgl_stubs.c" irisgl/irisgltest.c \
	    gl/glcheck.c gl/glbench.c gl/gltest.c gl/gloverlay.c
	cat > "$out/remote.sh" <<'EOF'
set -e
cd "$1"
gzcat src.tar.gz | tar xf -
rm -rf out
mkdir -p out/n32 out/o32 out/64

echo "== n32"
rm -f so_locations.n32
cc -n32 -mips3 -O2 -shared -Wl,-update_registry,./so_locations.n32 -Wl,-soname,libGL.so \
    -o out/n32/libglshim.so glshim_rt.c glshim_glx.c glshim_gen.c glshim_sgi.c hostcall_trap32.s -lXext -lX11 -lm
cc -n32 -mips3 -O2 -shared -Wl,-update_registry,./so_locations.n32 -Wl,-soname,libgl.so \
    -o out/n32/libirisgl.so irisgl_rt.c irisgl_draw.c irisgl_pixels.c irisgl_font.c irisgl_extra.c irisgl_nurbs.c irisgl_pup.c irisgl_stubs.c out/n32/libglshim.so -lGLU -lXext -lX11 -lm
cc -n32 -mips3 -O2 -shared -Wl,-update_registry,./so_locations.n32 -Wl,-soname,libGLcore.so \
    -o out/n32/libGLcore.so glcore_stub.c out/n32/libglshim.so
cc -n32 -mips3 -O2 -o out/n32/glcheck glcheck.c -lGL -lX11
cc -n32 -mips3 -O2 -o out/n32/glbench glbench.c -lGL -lX11
cc -n32 -mips3 -O2 -o out/n32/gltest gltest.c -lGL -lX11 -lm
cc -n32 -mips3 -O2 -o out/n32/gloverlay gloverlay.c -lGL -lX11
cc -n32 -mips3 -O2 -o out/n32/irisgltest irisgltest.c out/n32/libirisgl.so -lX11 -lm

echo "== o32"
cp "$2" so_locations.o32
chmod u+w so_locations.o32
cc -o32 -O2 -shared -Wl,-update_registry,./so_locations.o32 -Wl,-soname,libGL.so \
    -o out/o32/libglshim.so glshim_rt.c glshim_glx.c glshim_gen.c glshim_sgi.c hostcall_trap32.s -lXext -lX11 -lm
cc -o32 -O2 -shared -Wl,-update_registry,./so_locations.o32 -Wl,-soname,libgl.so \
    -o out/o32/libirisgl.so irisgl_rt.c irisgl_draw.c irisgl_pixels.c irisgl_font.c irisgl_extra.c irisgl_nurbs.c irisgl_pup.c irisgl_stubs.c out/o32/libglshim.so -lGLU -lXext -lX11 -lm
cc -o32 -O2 -shared -Wl,-update_registry,./so_locations.o32 -Wl,-soname,libGLcore.so \
    -o out/o32/libGLcore.so glcore_stub.c out/o32/libglshim.so
cc -o32 -O2 -o out/o32/glcheck glcheck.c -lGL -lX11
cc -o32 -O2 -o out/o32/glbench glbench.c -lGL -lX11
cc -o32 -O2 -o out/o32/gltest gltest.c -lGL -lX11 -lm

echo "== 64"
# OpenGL only: IRIX has no 64-bit IRIS GL. A fresh registry, as for n32.
rm -f so_locations.64
cc -64 -mips3 -O2 -shared -Wl,-update_registry,./so_locations.64 -Wl,-soname,libGL.so \
    -o out/64/libglshim.so glshim_rt.c glshim_glx.c glshim_gen.c glshim_sgi.c hostcall_trap64.s -lXext -lX11 -lm
cc -64 -mips3 -O2 -shared -Wl,-update_registry,./so_locations.64 -Wl,-soname,libGLcore.so \
    -o out/64/libGLcore.so glcore_stub.c out/64/libglshim.so
cc -64 -mips3 -O2 -o out/64/glcheck glcheck.c -lGL -lX11
cc -64 -mips3 -O2 -o out/64/glbench glbench.c -lGL -lX11
cc -64 -mips3 -O2 -o out/64/gltest gltest.c -lGL -lX11 -lm

tar cf - out | gzip -c > out.tar.gz
echo "== built"
EOF
	remote "$out" gl "${IRIX_SO_LOCATIONS_O32:-/usr/lib/so_locations}"
	rm -rf "$out/n32" "$out/o32" "$out/64"
	mv "$out/out/n32" "$out/out/o32" "$out/out/64" "$out/"
	rmdir "$out/out"
	cp "$HERE/gl/install-gl.sh" "$out/"
	rm -rf "$out/cross-include"
	mkdir -p "$out/cross-include"
	$IRIX_SSH "cd /usr/include && tar cf - GL/gl.h GL/glx.h GL/glxtokens.h | gzip -c" | (cd "$out/cross-include" && tar xzf -)
	echo "build.sh: gl: MIPSpro builds in $out/n32, $out/o32 and $out/64 (log: $out/mipspro.log)"
}

cross() {
	gcc=${IRIX_GCC:-$HERE/../../tools/irix-cross/irix-gcc}
	if [ ! -x "$gcc" ]; then
		echo "build.sh: no irix-gcc at $gcc (set IRIX_GCC): skipping the cross build"
		return 0
	fi
	d=$BUILD/hostcall
	mkdir -p "$d"
	"$gcc" -O2 -I"$HERE/include" -o "$d/hostcall_test" \
	    "$HERE/hostcall/hostcall_test.c" "$HERE/hostcall/hostcall_trap.c"
	echo "build.sh: cross: $d/hostcall_test"
	inc=$BUILD/gl/cross-include
	if [ ! -f "$inc/GL/gl.h" ]; then
		echo "build.sh: no OpenGL headers in $inc (run the gl step first): skipping the cross libGL"
		return 0
	fi
	d=$BUILD/gl/n32-cross
	rm -rf "$d"
	mkdir -p "$d"
	g=$HERE/gl
	"$gcc" -O2 -I"$inc" -I"$HERE/include" -shared -Wl,-soname,libGL.so -o "$d/libglshim.so" \
	    "$g/glshim_rt.c" "$g/glshim_glx.c" "$g/glshim_gen.c" \
	    "$HERE/hostcall/hostcall_trap32.s" -lXext -lX11 -lm
	"$gcc" -O2 -I"$inc" -o "$d/glcheck" "$g/glcheck.c" "$d/libglshim.so" -lX11
	echo "build.sh: cross: $d"
}

case "$TARGET" in
gl) gl ;;
cross) cross ;;
all)
	gl
	cross
	;;
*)
	echo "usage: $0 [gl|cross|all]" >&2
	exit 2
	;;
esac
