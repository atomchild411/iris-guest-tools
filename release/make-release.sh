#!/bin/sh
#
# make-release.sh [MIPSPRO_GL_DIR]  -- build and package a binary release.
#
# Builds the n32 libraries and tests with the LLVM cross toolchain
# (clang/build.sh build + check: LLVMBIN, SYSROOT and SGI must be set, see
# clang/common.sh), and adds o32 libraries from a MIPSpro build when there is
# one: MIPSPRO_GL_DIR is build.sh's gl output (default build/gl, if it holds
# o32/). With COMPILER=mipspro, n32 comes from that MIPSpro build too, and
# the cross toolchain is not used: build.sh gl first. Output, in
# $WORK/release (WORK defaults to build/clang):
#
#   iris-guest-tools-DATE-protoN.tgz         the release, rooted at its own
#                                            directory: install.sh, README.txt,
#                                            VERSION, LICENSE, n32/ [o32/], bin/
#   iris-guest-tools-DATE-protoN.tgz.sha256
#
# DATE is the date of the commit built; N is the host GL protocol the
# libraries speak (HGL_PROTOCOL in gl/glshim.h), which is what decides the
# IRIS it works with. A tree with uncommitted changes is refused
# (ALLOW_DIRTY=1 to build one anyway, marked -dirty).
#
# The two library names differ only in case, which a Mac cannot keep in one
# directory, so the tarball carries them as libglshim.so (OpenGL, SONAME
# libGL.so) and libirisgl.so (IRIS GL, SONAME libgl.so); install.sh gives
# them their IRIX names. libGLcore.so, the stand-in for SGI's, keeps its own.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
GT=$(cd "$HERE/.." && pwd)
WORK=${WORK:-$GT/build/clang}
export WORK
MIPSPRO=${1:-$GT/build/gl}

# The IRIS each protocol needs. A new protocol gets a line here.
iris_for() {
	case "$1" in
	2) echo "techomancer/iris from commit 1a93808 (2026-09-30) on, built with --features hostgl, on macOS" ;;
	*) echo "an IRIS whose host GL speaks protocol $1" ;;
	esac
}

cd "$GT"
rev=$(git rev-parse --short HEAD)
date=$(git log -1 --format=%cd --date=format:%Y%m%d)
dirty=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	[ "${ALLOW_DIRTY:-}" = 1 ] || { echo "make-release.sh: uncommitted changes; commit them or set ALLOW_DIRTY=1" >&2; exit 1; }
	dirty=-dirty
fi
proto=$(awk '$1 == "#define" && $2 == "HGL_PROTOCOL" { print $3 }' gl/glshim.h)
[ -n "$proto" ] || { echo "make-release.sh: no HGL_PROTOCOL in gl/glshim.h" >&2; exit 1; }
name=iris-guest-tools-$date-proto$proto$dirty

out=$WORK/release
stage=$out/$name
rm -rf "$stage" "$out/$name.tgz" "$out/$name.tgz.sha256"
mkdir -p "$stage/n32" "$stage/bin/n32"
case "${COMPILER:-clang}" in
clang)
	sh "$GT/clang/build.sh" build
	sh "$GT/clang/build.sh" check
	B=$WORK/build/iris-tools
	cp "$B/lib/libglshim.so" "$B/lib/libirisgl.so" "$B/lib/libGLcore.so" "$stage/n32/"
	for t in glcheck gltest glbench gloverlay irisgltest hostcall_test; do
		cp "$B/bin/$t" "$stage/bin/n32/"
	done
	compiled="clang (LLVM cross toolchain) for n32"
	;;
mipspro)
	[ -f "$MIPSPRO/n32/libglshim.so" ] && [ -f "$MIPSPRO/o32/libglshim.so" ] || {
		echo "make-release.sh: no MIPSpro build in $MIPSPRO (./build.sh gl)" >&2; exit 1; }
	cp "$MIPSPRO/n32/libglshim.so" "$MIPSPRO/n32/libirisgl.so" "$MIPSPRO/n32/libGLcore.so" "$stage/n32/"
	for t in glcheck gltest glbench gloverlay irisgltest; do
		cp "$MIPSPRO/n32/$t" "$stage/bin/n32/"
	done
	compiled="MIPSpro 7.4 for n32"
	;;
*) echo "make-release.sh: COMPILER is clang or mipspro" >&2; exit 1 ;;
esac
abis=n32
if [ -f "$MIPSPRO/o32/libglshim.so" ] && [ -f "$MIPSPRO/o32/libirisgl.so" ] && [ -f "$MIPSPRO/o32/libGLcore.so" ]; then
	mkdir -p "$stage/o32" "$stage/bin/o32"
	cp "$MIPSPRO/o32/libglshim.so" "$MIPSPRO/o32/libirisgl.so" "$MIPSPRO/o32/libGLcore.so" "$stage/o32/"
	for t in glcheck gltest glbench gloverlay irisgltest; do
		[ -f "$MIPSPRO/o32/$t" ] && cp "$MIPSPRO/o32/$t" "$stage/bin/o32/"
	done
	abis="n32 o32"
	compiled="$compiled, MIPSpro 7.4 for o32"
fi
cp "$HERE/install.sh" "$HERE/README.txt" "$GT/LICENSE" "$stage/"
cat > "$stage/VERSION" <<EOF
iris-guest-tools $date (git $rev$dirty), https://github.com/atomchild411/iris-guest-tools
ABIs:              $abis
compiled with:     $compiled
host GL protocol:  $proto
needs:             $(iris_for "$proto")
EOF
chmod 755 "$stage/install.sh" "$stage"/n32/* "$stage"/bin/*/*
[ -d "$stage/o32" ] && chmod 755 "$stage"/o32/*

(cd "$out" && COPYFILE_DISABLE=1 tar --format ustar --uid 0 --gid 0 --uname root --gname sys \
    -czf "$name.tgz" "$name")
(cd "$out" && shasum -a 256 "$name.tgz" > "$name.tgz.sha256")
echo "make-release.sh: $out/$name.tgz"
cat "$stage/VERSION"
tar tzf "$out/$name.tgz"
