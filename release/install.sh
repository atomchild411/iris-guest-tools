#!/bin/sh
#
# install.sh [-u | -s] [n32 | o32 ...]  -- switch an IRIX machine's OpenGL
# and IRIS GL libraries to the IRIS host-GL ones in this release, or back.
# Run on the IRIX machine, as root, from the unpacked release directory.
#
#   install.sh        copy the libraries to /usr/local/iris-tools/<lib dir>
#                     and point IRIX's GL links at them (recording SGI's
#                     targets first)
#   install.sh -u     put SGI's targets back, from that record
#   install.sh -s     show where everything points; change nothing
#
# With no ABI named, every ABI this release carries is done (n32: lib32,
# o32: lib). The test programs go to /usr/local/iris-tools/bin/<abi>.
#
# How IRIX finds these libraries: programs name libGL.so (OpenGL) and
# libgl.so (IRIS GL) in DT_NEEDED; /usr/<lib dir>/libGL.so and libgl.so are
# symlinks to ../../var/arch/<lib dir>/<name>, and those point at the
# graphics board's own library under /usr/gfx/arch/. Only the /var/arch
# links change here: /usr/<lib dir> and /usr/gfx are left as SGI installed
# them, and preloading is not used (rld binds these libraries directly).
#
# The record: the first run writes `ls -l` of the /var/arch links to
# /usr/local/iris-tools/sgi-links-<lib dir>.txt and never overwrites it, so
# -u always restores SGI's original targets. If a name there is a regular
# file rather than a link, it is moved (not copied) to
# /usr/local/iris-tools/sgi-orig-<lib dir>/ and -u moves it back. A link that
# already points at an IRIS GL library (this install, or the pkgsrc
# package's in /opt/pkgsrc/lib/iris-tools) is not recorded as SGI's: undo
# the other install first to keep SGI's targets.
#
# Each file and link is made under a temporary name and renamed into place,
# so a program starting meanwhile sees the old library or the new one; `sync`
# runs at the end.
#
# Bourne shell (IRIX /bin/sh): backquotes, no $(...).
set -e
HERE=`dirname "$0"`
R=${IRIS_TOOLS_TESTROOT:-}	# empty on a real machine; set only to test the script elsewhere
TOOLS=$R/usr/local/iris-tools
NAMES="libGL.so libgl.so"

mode=install
case "$1" in
-s) mode=show; shift ;;
-u) mode=undo; shift ;;
-*) echo "usage: $0 [-u | -s] [n32 | o32 ...]" >&2; exit 2 ;;
esac
abis="$*"
if [ -z "$abis" ]; then
	for a in n32 o32; do
		if [ $mode = install ]; then
			[ -f "$HERE/$a/libglshim.so" ] && abis="$abis $a"
		else
			abis="$abis $a"
		fi
	done
fi

libdir() {
	case "$1" in
	n32) echo lib32 ;;
	o32) echo lib ;;
	*) echo "install.sh: unknown ABI $1 (n32 or o32)" >&2; exit 2 ;;
	esac
}

target() {	# target LINK: what a symlink points at (from ls -l)
	ls -l "$1" | awk '{ print $NF }'
}

ours() {	# ours TARGET: is this an IRIS GL library (ours or the pkgsrc package's)?
	case "$1" in
	$TOOLS/*|/usr/local/iris-tools/*|/opt/pkgsrc/lib/iris-tools/*) return 0 ;;
	esac
	return 1
}

relink() {	# relink TARGET LINK
	rm -f "$2.new"
	ln -s "$1" "$2.new"
	mv -f "$2.new" "$2"
	echo "$2 -> $1"
}

put() {	# put SRC DST: copy under a temporary name, then rename over DST
	cp "$1" "$2.new"
	chmod 755 "$2.new"
	mv -f "$2.new" "$2"
}

if [ $mode != show ] && [ -z "$R" ]; then
	case "`id`" in
	"uid=0("*) ;;
	*) echo "install.sh: run as root" >&2; exit 1 ;;
	esac
fi

for abi in $abis; do
	ld=`libdir $abi`
	LIBS=$TOOLS/$ld
	ARCH=$R/var/arch/$ld
	USRLIB=$R/usr/$ld
	RECORD=$TOOLS/sgi-links-$ld.txt
	ORIG=$TOOLS/sgi-orig-$ld
	echo "== $abi ($ld)"

	if [ $mode = show ]; then
		for n in $NAMES; do
			for p in $USRLIB/$n $ARCH/$n; do
				if [ -h $p ]; then
					echo "$p -> `target $p`"
				elif [ -f $p ]; then
					echo "$p: regular file"
				else
					echo "$p: absent"
				fi
			done
		done
		[ -f $RECORD ] && { echo "record ($RECORD):"; cat $RECORD; }
		continue
	fi

	if [ $mode = undo ]; then
		[ -f $RECORD ] || { echo "no record ($RECORD): nothing to undo"; continue; }
		for n in $NAMES; do
			if [ -f $ORIG/$n ]; then
				rm -f $ARCH/$n
				mv $ORIG/$n $ARCH/$n
				echo "$ARCH/$n: SGI's file moved back"
				continue
			fi
			t=`awk -v n=$n '$(NF-2) == n { print $NF }' $RECORD`
			if [ -n "$t" ]; then
				relink "$t" $ARCH/$n
			else
				echo "$ARCH/$n: not in the record; left alone"
			fi
		done
		continue
	fi

	[ -f "$HERE/$abi/libglshim.so" ] && [ -f "$HERE/$abi/libirisgl.so" ] || {
		echo "install.sh: this release has no $abi libraries ($HERE/$abi)" >&2; exit 1; }
	# /usr/<lib dir>/<name> must lead to /var/arch/<lib dir>/<name>, or
	# changing that link would not change what programs load.
	for n in $NAMES; do
		if [ -h $USRLIB/$n ]; then
			case "`target $USRLIB/$n`" in
			*var/arch/$ld/$n) ;;
			*) echo "install.sh: $USRLIB/$n -> `target $USRLIB/$n`, not /var/arch/$ld/$n: stopping; nothing changed" >&2
			   exit 1 ;;
			esac
		else
			echo "install.sh: $USRLIB/$n is not a symlink (a library copied over it earlier?): stopping; nothing changed" >&2
			exit 1
		fi
	done

	mkdir -p $LIBS $ARCH
	put "$HERE/$abi/libglshim.so" $LIBS/libGL.so
	put "$HERE/$abi/libirisgl.so" $LIBS/libgl.so
	if [ -d "$HERE/bin/$abi" ]; then
		mkdir -p $TOOLS/bin/$abi
		for f in "$HERE/bin/$abi"/*; do
			put "$f" $TOOLS/bin/$abi/`basename "$f"`
		done
	fi

	if [ ! -f $RECORD ]; then
		: > $RECORD.new
		for n in $NAMES; do
			if [ -h $ARCH/$n ]; then
				t=`target $ARCH/$n`
				if ours "$t"; then
					echo "install.sh: $ARCH/$n already points at an IRIS GL library ($t); SGI's target is unknown: not recorded" >&2
				else
					(cd $ARCH && ls -l $n) >> $RECORD.new
				fi
			elif [ -f $ARCH/$n ]; then
				mkdir -p $ORIG
				mv $ARCH/$n $ORIG/$n
				echo "$ARCH/$n: regular file, moved to $ORIG/$n"
			fi
		done
		mv $RECORD.new $RECORD
		echo "SGI's links recorded in $RECORD:"
		cat $RECORD
	fi

	for n in $NAMES; do
		relink $LIBS/$n $ARCH/$n
	done
done
sync
