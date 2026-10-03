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
# libGLcore.so too, where IRIX has one: SGI's libGL.so is GLX alone and its
# GL functions are in libGLcore.so, which drives the board itself, and a
# program that lists libGLcore.so before libGL.so would take them from there.
# The release's libGLcore.so has no functions and needs libGL.so, so they all
# come from this release's libGL.so whatever a program lists.
#
# The X server: its GLX extension (glx.so, which Xsgi loads) needs SGI's
# libGLcore.so, and the n32 link no longer leads there, so it would fail to
# load ("unresolvable symbol ... __gl_adapters") and the server would answer
# every GLX request with BadImplementation. So each local X server in
# /var/X11/xdm/Xservers is started as
#     /sbin/env LD_LIBRARYN32_PATH=<SGI's libGLcore.so directory> <command>
# which puts SGI's library first for the X server alone. The original file
# is kept as /usr/local/iris-tools/sgi-Xservers and the directory added in
# xservers-path.txt; -u takes out exactly that prefix. An Xservers that sets
# a library path already is left alone. The X server reads it when it next
# starts: reboot.
#
# The record: each library's /var/arch link is written, as `ls -l`, to
# /usr/local/iris-tools/sgi-links-<lib dir>.txt the first time it is
# switched, and never rewritten, so -u always restores SGI's original
# targets (an install over an older release adds the links that release did
# not switch). If a name there is a regular
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
NAMES="libGL.so libgl.so libGLcore.so"

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

XSERVERS=$R/var/X11/xdm/Xservers
XMARK=$TOOLS/xservers-path.txt

sgi_core_dir() {	# sgi_core_dir ARCH RECORD ORIG: where SGI's libGLcore.so is (as the guest sees it)
	if [ -f $3/libGLcore.so ]; then
		d=$3
	else
		t=`awk '$(NF-2) == "libGLcore.so" { print $NF }' $2 2>/dev/null`
		[ -n "$t" ] || return 1
		d=`cd $1 && cd \`dirname $t\` && pwd` || return 1
	fi
	[ -f $d/libGLcore.so ] || return 1
	echo "$d" | sed "s|^$R||"
}

xservers_on() {	# xservers_on DIR: start the local X servers with DIR on their n32 library path
	[ -f $XSERVERS ] || return 0
	if grep LD_LIBRARYN32_PATH= $XSERVERS > /dev/null; then
		echo "$XSERVERS: already sets a library path; left alone"
		return 0
	fi
	[ -f $TOOLS/sgi-Xservers ] || cp -p $XSERVERS $TOOLS/sgi-Xservers
	# local servers: "<display> <class> /<command> ...", and not comments
	cp -p $XSERVERS $XSERVERS.new
	sed "s|^\(:[^ 	#]*[ 	][ 	]*[^ 	]*[ 	][ 	]*\)/|\1/sbin/env LD_LIBRARYN32_PATH=$1 /|" $XSERVERS > $XSERVERS.new
	mv -f $XSERVERS.new $XSERVERS
	echo "$1" > $XMARK
	echo "$XSERVERS: X server's GLX gets SGI's libGLcore.so from $1 (from the next X start: reboot)"
}

xservers_off() {	# take out what xservers_on added
	[ -f $XMARK ] && [ -f $XSERVERS ] || return 0
	d=`cat $XMARK`
	cp -p $XSERVERS $XSERVERS.new
	sed "s|/sbin/env LD_LIBRARYN32_PATH=$d /|/|" $XSERVERS > $XSERVERS.new
	mv -f $XSERVERS.new $XSERVERS
	rm -f $XMARK
	echo "$XSERVERS: X server back to IRIX's library path (from the next X start: reboot)"
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
	names=
	for n in $NAMES; do
		# libGLcore.so only where this IRIX has one
		[ $n = libGLcore.so ] && [ ! -h $USRLIB/$n ] && [ ! -f $USRLIB/$n ] && continue
		names="$names $n"
	done

	if [ $mode = show ]; then
		for n in $names; do
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
		if [ $abi = n32 ] && [ -f $XSERVERS ]; then
			echo "X servers ($XSERVERS):"
			grep '^:' $XSERVERS || true
		fi
		continue
	fi

	if [ $mode = undo ]; then
		[ -f $RECORD ] || { echo "no record ($RECORD): nothing to undo"; continue; }
		for n in $names; do
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
		[ $abi = n32 ] && xservers_off
		continue
	fi

	[ -f "$HERE/$abi/libglshim.so" ] && [ -f "$HERE/$abi/libirisgl.so" ] || {
		echo "install.sh: this release has no $abi libraries ($HERE/$abi)" >&2; exit 1; }
	# /usr/<lib dir>/<name> must lead to /var/arch/<lib dir>/<name>, or
	# changing that link would not change what programs load.
	for n in $names; do
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
	case "$names" in *libGLcore.so*) put "$HERE/$abi/libGLcore.so" $LIBS/libGLcore.so ;; esac
	if [ -d "$HERE/bin/$abi" ]; then
		mkdir -p $TOOLS/bin/$abi
		for f in "$HERE/bin/$abi"/*; do
			put "$f" $TOOLS/bin/$abi/`basename "$f"`
		done
	fi

	for n in $names; do
		# already recorded (as a link, or moved aside as a file)?
		[ -f $ORIG/$n ] && continue
		if [ -f $RECORD ] && awk -v n=$n '$(NF-2) == n { f = 1 } END { exit !f }' $RECORD; then
			continue
		fi
		if [ -h $ARCH/$n ]; then
			t=`target $ARCH/$n`
			if ours "$t"; then
				echo "install.sh: $ARCH/$n already points at an IRIS GL library ($t); SGI's target is unknown: not recorded" >&2
			else
				(cd $ARCH && ls -l $n) >> $RECORD
				echo "$ARCH/$n: SGI's target recorded"
			fi
		elif [ -f $ARCH/$n ]; then
			mkdir -p $ORIG
			mv $ARCH/$n $ORIG/$n
			echo "$ARCH/$n: regular file, moved to $ORIG/$n"
		fi
	done
	[ -f $RECORD ] && { echo "SGI's links ($RECORD):"; cat $RECORD; }

	for n in $names; do
		relink $LIBS/$n $ARCH/$n
	done

	# the X server is n32: its GLX needs SGI's libGLcore.so back
	if [ $abi = n32 ]; then
		case "$names" in
		*libGLcore.so*)
			if d=`sgi_core_dir $ARCH $RECORD $ORIG`; then
				xservers_on $d
			else
				echo "install.sh: SGI's n32 libGLcore.so not found from the record: X server left as it is (its GLX will not load)" >&2
			fi ;;
		esac
	fi
done
sync
