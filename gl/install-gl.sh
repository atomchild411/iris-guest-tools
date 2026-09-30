#!/bin/sh
#
# install-gl.sh [n32|o32|64 ...] -- put the host GL libraries in place, in an
# IRIX guest, as root, from a copy of guest-tools/build/gl (the directory
# holding n32/, o32/ and 64/). With no ABI named, every one present is done.
#
# For each ABI the libraries go to /usr/local/iris-tools/<lib dir>:
#
#   n32   lib32   libGL.so (libglshim.so), libgl.so (libirisgl.so)
#   o32   lib     the same
#   64    lib64   libGL.so only: IRIX has no 64-bit IRIS GL
#
# and /var/arch/<lib dir>/{libGL,libgl}.so -- where /usr/<lib dir>/libGL.so
# and libgl.so point on this image -- are made to point at them. SGI's own
# targets are written to /usr/local/iris-tools/sgi-links-<lib dir>.txt the
# first time, never overwritten, so `install-gl.sh -u` can put them back.
#
#   install-gl.sh -u [n32|o32|64 ...]   restore SGI's links from that record
#
# Each library is written to a new file and renamed into place, so a program
# starting meanwhile sees the old library or the new one, never half of one,
# and `sync` runs at the end: a guest that dies soon after an install keeps
# what it was given.
set -e
HERE=`dirname "$0"`
TOOLS=/usr/local/iris-tools
undo=0
if [ "$1" = "-u" ]; then
	undo=1
	shift
fi
abis="$*"
[ -n "$abis" ] || abis="n32 o32 64"

libdir() {
	case "$1" in
	n32) echo lib32 ;;
	o32) echo lib ;;
	64) echo lib64 ;;
	*) echo "install-gl.sh: unknown ABI $1" >&2; exit 1 ;;
	esac
}

# install SRC DST: copy under a temporary name, then rename over DST.
install_file() {
	cp "$1" "$2.new"
	chmod 755 "$2.new"
	mv "$2.new" "$2"
}

for abi in $abis; do
	ld=`libdir $abi`
	record=$TOOLS/sgi-links-$ld.txt
	arch=/var/arch/$ld
	names="libGL.so libgl.so"
	[ $abi = 64 ] && names="libGL.so"
	if [ $undo = 1 ]; then
		[ -f $record ] || { echo "$abi: no record of SGI's links ($record)"; continue; }
		# The record is `ls -l` lines: name -> target.
		for n in $names; do
			t=`awk -v n=$n '$(NF-2) == n { print $NF }' $record`
			[ -n "$t" ] && { rm -f $arch/$n; ln -s $t $arch/$n; echo "$arch/$n -> $t"; }
		done
		continue
	fi
	[ -d "$HERE/$abi" ] || { echo "$abi: nothing built ($HERE/$abi)"; continue; }
	mkdir -p $TOOLS/$ld
	if [ ! -f $record ]; then
		(cd $arch && for n in $names; do [ -h $n ] && ls -l $n; done) > $record
	fi
	install_file "$HERE/$abi/libglshim.so" $TOOLS/$ld/libGL.so
	[ $abi != 64 ] && install_file "$HERE/$abi/libirisgl.so" $TOOLS/$ld/libgl.so
	for n in $names; do
		rm -f $arch/$n
		ln -s $TOOLS/$ld/$n $arch/$n
		echo "$arch/$n -> $TOOLS/$ld/$n"
	done
done
sync
