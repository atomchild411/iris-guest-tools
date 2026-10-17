#!/bin/sh
#
# make-tardist.sh RELEASE_DIR VERSION [OUTDIR]  -- package an unpacked release
# (make-release.sh's tarball, gzcat | tar xf) as a tardist: an inst(1M)
# distribution in a tar file, product iristools. Runs on IRIX, with
# gendist(1M) (inst_dev, the Software Packager).
#
# VERSION is the image version, a number that grows with each release: the
# release's date and a two-digit serial (2026101201 for the first one of
# 2026-10-12). Output: OUTDIR/iris-guest-tools-<release>.tardist.
#
# The tardist puts the release in /usr/local/iris-tools/release; inst then
# runs its install.sh (exitop), exactly as a manual install would, and keeps
# a copy as /usr/local/iris-tools/install.sh. `versions remove iristools`
# runs that copy with -u (removeop), which puts SGI's libraries back.
# Reboot after either for the X server to follow.
#
# Bourne shell (IRIX /bin/sh): backquotes, no $(...).
set -eu
here=`dirname "$0"`
here=`cd "$here" && pwd`
rel=`cd "$1" && pwd` version=$2 out=${3:-`pwd`}
case $version in *[!0-9]*|"") echo "VERSION must be a number" >&2; exit 1 ;; esac
[ -f "$rel/install.sh" ] && [ -f "$rel/VERSION" ] || { echo "$rel is not an unpacked release" >&2; exit 1; }
name=`basename "$rel"`

dest=usr/local/iris-tools/release
# inst runs a removeop after it has deleted the files, so the one that puts
# SGI's libraries back runs a copy of install.sh that inst does not own (-u
# needs only the record in /usr/local/iris-tools, not the release).
keep=usr/local/iris-tools/install.sh
tag=IRISTOOLS_BASE
work=/usr/tmp/iristools-dist.$$
trap 'rm -rf "$work"' 0
mkdir -p "$work/src/usr/local/iris-tools" "$work/dist"
cp -r "$rel" "$work/src/$dest"

# The idb: type mode owner group destination source tags, sorted on the
# destination and source (gendist(1M)). install.sh carries the exit and
# remove ops; inst runs both at the end of its session, after every file
# is in place or taken away.
(
	cd "$work/src"
	find $dest -type d -print | while read d; do
		echo "d 0755 root sys $d $d $tag"
	done
	find $dest -type f -print | while read f; do
		case $f in
		$dest/install.sh)
			echo "f 0755 root sys $f $f $tag nostrip exitop(\"sh /$dest/install.sh && cp /$dest/install.sh /$keep\") removeop(\"sh /$keep -u\")" ;;
		$dest/n32/*|$dest/o32/*|$dest/bin/*|$dest/hostgl|$dest/sgigl)
			echo "f 0755 root sys $f $f $tag nostrip" ;;
		*)
			echo "f 0644 root sys $f $f $tag" ;;
		esac
	done
) | sort +4u -6 > "$work/idb"

sed "s/VERSION/$version/" "$here/iristools.spec" > "$work/spec"

/usr/sbin/gendist -rbase / -sbase "$work/src" -idb "$work/idb" \
	-spec "$work/spec" -dist "$work/dist" -nostrip -all

file=$name.tardist
(cd "$work/dist" && tar cf - iristools*) > "$out/$file"
ls -l "$out/$file"
