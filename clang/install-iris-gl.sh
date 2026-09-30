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
# The record: the first run writes `ls -l` of both /var/arch/lib32 links to
# /opt/pkgsrc/lib/iris-tools/sgi-links-lib32.txt and never overwrites it, so
# -u always restores SGI's original targets.  If a name there is a regular
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
	return 0
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
if [ ! -f $RECORD ]; then
	: > $RECORD.new
	for n in $NAMES; do
		if [ -h $ARCH/$n ]; then
			t=`target $ARCH/$n`
			if ours "$t"; then
				echo "install-iris-gl.sh: $ARCH/$n already points at a shim ($t); SGI's target is unknown: not recorded" >&2
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
sync
