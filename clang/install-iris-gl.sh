#!/bin/sh
#
# install-iris-gl.sh [-u | -s]  -- switch an IRIX machine's n32 GL libraries
# to the IRIS host-GL shims in /opt/pkgsrc/lib/iris-tools/lib32, or back.
# Run on the guest, as root, after unpacking iris-tools-irix65-n32.tgz at /.
#
#   install-iris-gl.sh      point /var/arch/lib32/libGL.so and libgl.so at
#                           the shims (recording SGI's targets first)
#   install-iris-gl.sh -u   put SGI's targets back, from that record
#   install-iris-gl.sh -s   show where everything points; change nothing
#
# How IRIX finds these libraries: programs name libGL.so (OpenGL) and
# libgl.so (IRIS GL) in DT_NEEDED; /usr/lib32/libGL.so and libgl.so are
# symlinks to ../../var/arch/lib32/<name>, and those point at the graphics
# board's own library under /usr/gfx/arch/.  Only the /var/arch/lib32 links
# change here: /usr/lib32 and /usr/gfx are left as SGI installed them, and
# preloading is not used (rld binds these libraries directly).
#
# libGLcore.so too, where IRIX has one: SGI's libGL.so is GLX alone and its
# GL functions are in libGLcore.so, which drives the board itself, and a
# program that lists libGLcore.so before libGL.so would take them from there.
# The package's libGLcore.so has no functions and needs libGL.so, so they all
# come from the shim whatever a program lists.
#
# The X server: its GLX extension (glx.so, which Xsgi loads) needs SGI's
# libGLcore.so, and the link no longer leads there, so it would fail to load
# ("unresolvable symbol ... __gl_adapters") and the server would answer
# every GLX request with BadImplementation. So each local X server in
# /var/X11/xdm/Xservers is started as
#     /sbin/env LD_LIBRARYN32_PATH=<SGI's libGLcore.so directory> <command>
# which puts SGI's library first for the X server alone. The original file
# is kept as /opt/pkgsrc/lib/iris-tools/sgi-Xservers and the directory added
# in xservers-path.txt; -u takes out exactly that prefix. An Xservers that
# sets a library path already is left alone. The X server reads it when it
# next starts: reboot.
#
# The record: each /var/arch/lib32 link is written, as `ls -l`, to
# /opt/pkgsrc/lib/iris-tools/sgi-links-lib32.txt the first time it is
# switched, and never rewritten, so -u always restores SGI's original
# targets.  If a name there is a regular
# file rather than a link, it is moved (not copied) to
# /opt/pkgsrc/lib/iris-tools/sgi-orig-lib32/ and -u moves it back.  A link
# that already points into /opt/pkgsrc/lib/iris-tools, or into an older
# MIPSpro install under /usr/local/iris-tools, is not recorded as SGI's.
#
# Each link is made under a temporary name and renamed into place, so a
# program starting meanwhile sees the old library or the new one; `sync`
# runs at the end.  o32 (/var/arch/lib) and 64-bit (/var/arch/lib64) are not
# touched: this package holds n32 libraries only.
#
# Bourne shell (IRIX /bin/sh): backquotes, no $(...).
set -e
R=${IRIS_TOOLS_TESTROOT:-}	# empty on a real machine; set only to test the script elsewhere
TOOLS=$R/opt/pkgsrc/lib/iris-tools
LIBS=$TOOLS/lib32
ARCH=$R/var/arch/lib32
USRLIB=$R/usr/lib32
RECORD=$TOOLS/sgi-links-lib32.txt
ORIG=$TOOLS/sgi-orig-lib32
NAMES="libGL.so libgl.so"
# libGLcore.so only where this IRIX has one
if [ -h $USRLIB/libGLcore.so ] || [ -f $USRLIB/libGLcore.so ]; then
	NAMES="$NAMES libGLcore.so"
fi

target() {	# target LINK: what a symlink points at (from ls -l)
	ls -l "$1" | awk '{ print $NF }'
}

ours() {	# ours TARGET: is this one of our shims (this package or MIPSpro's)?
	case "$1" in
	$TOOLS/*|$R/usr/local/iris-tools/*) return 0 ;;
	esac
	return 1
}

show() {
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
	if [ -f $XSERVERS ]; then
		echo "X servers ($XSERVERS):"
		grep '^:' $XSERVERS || true
	fi
	return 0
}

XSERVERS=$R/var/X11/xdm/Xservers
XMARK=$TOOLS/xservers-path.txt

sgi_core_dir() {	# where SGI's libGLcore.so is (as the guest sees it), from the record
	if [ -f $ORIG/libGLcore.so ]; then
		d=$ORIG
	else
		t=`awk '$(NF-2) == "libGLcore.so" { print $NF }' $RECORD 2>/dev/null`
		[ -n "$t" ] || return 1
		d=`cd $ARCH && cd \`dirname $t\` && pwd` || return 1
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

relink() {	# relink TARGET LINK
	rm -f "$2.new"
	ln -s "$1" "$2.new"
	mv -f "$2.new" "$2"
	echo "$2 -> $1"
}

case "$1" in
-s)
	show
	exit 0
	;;
-u)
	[ -f $RECORD ] || { echo "install-iris-gl.sh: no record ($RECORD): nothing to undo" >&2; exit 1; }
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
	xservers_off
	sync
	exit 0
	;;
"")
	;;
*)
	echo "usage: $0 [-u | -s]" >&2
	exit 2
	;;
esac

if [ -z "$R" ]; then
	case "`id`" in
	"uid=0("*) ;;
	*) echo "install-iris-gl.sh: run as root" >&2; exit 1 ;;
	esac
fi
for n in $NAMES; do
	[ -f $LIBS/$n ] || { echo "install-iris-gl.sh: $LIBS/$n missing (unpack the tarball at / first)" >&2; exit 1; }
	# /usr/lib32/<name> must lead to /var/arch/lib32/<name>, or changing that
	# link would not change what programs load.
	if [ -h $USRLIB/$n ]; then
		case "`target $USRLIB/$n`" in
		*var/arch/lib32/$n) ;;
		*) echo "install-iris-gl.sh: $USRLIB/$n -> `target $USRLIB/$n`, not /var/arch/lib32/$n: stopping; nothing changed" >&2
		   exit 1 ;;
		esac
	else
		echo "install-iris-gl.sh: $USRLIB/$n is not a symlink (a shim copied over it earlier?): stopping; nothing changed" >&2
		exit 1
	fi
done

mkdir -p $ARCH
for n in $NAMES; do
	# already recorded (as a link, or moved aside as a file)?
	[ -f $ORIG/$n ] && continue
	if [ -f $RECORD ] && awk -v n=$n '$(NF-2) == n { f = 1 } END { exit !f }' $RECORD; then
		continue
	fi
	if [ -h $ARCH/$n ]; then
		t=`target $ARCH/$n`
		if ours "$t"; then
			echo "install-iris-gl.sh: $ARCH/$n already points at a shim ($t); SGI's target is unknown: not recorded" >&2
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

for n in $NAMES; do
	relink $LIBS/$n $ARCH/$n
done

# the X server is n32: its GLX needs SGI's libGLcore.so back
case "$NAMES" in
*libGLcore.so*)
	if d=`sgi_core_dir`; then
		xservers_on $d
	else
		echo "install-iris-gl.sh: SGI's libGLcore.so not found from the record: X server left as it is (its GLX will not load)" >&2
	fi ;;
esac
sync
