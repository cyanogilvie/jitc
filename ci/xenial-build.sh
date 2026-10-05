#!/bin/sh
# Build jitc for an old-glibc x86_64 host (Ubuntu 16.04 / glibc 2.23 and up)
# against that host's Tcl, in the ci/xenial.Containerfile environment, and
# package the install as a tarball to unpack on the host.
#
#   ci/xenial-build.sh <target-tcl-prefix> <build-tcl-prefix> <install-prefix> <out.tar.gz>
#
# target-tcl-prefix  A copy of the target host's Tcl: include/ (tcl.h, ...),
#                    lib/libtclstub*.a and lib/pkgconfig/tcl.pc.  The .pc's
#                    prefix is rewritten to <install-prefix>.
# build-tcl-prefix   A Tcl that runs here and in the container, of the same
#                    major.minor, for build-time scripts (genStubs).  The
#                    target's own tclsh may not run on this CPU.
# install-prefix     Where the target host's Tcl lives (/home/nsadmin, say):
#                    jitc installs into <install-prefix>/lib/jitc<version>.
#
# The git-wrap subprojects are fetched on the host first: the container's CA
# bundle is too old for GitHub.
set -eu

[ $# -eq 4 ] || { sed -n '2,19p' "$0"; exit 1; }
target_tcl=$(realpath "$1")
build_tcl=$(realpath "$2")
prefix=$3
out=$(realpath -m "$4")
src=$(realpath "$(dirname "$0")/..")
builddir=buildxenial
image=jitc-xenial

podman image exists $image || podman build -t $image -f "$src/ci/xenial.Containerfile" "$src/ci"
(cd "$src" && meson subprojects download >/dev/null)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/pc" "$work/stage"
sed -e "s#^prefix=.*#prefix=$prefix#" -e "s#^exec_prefix=.*#exec_prefix=$prefix#" \
	-e "s#^libdir=.*#libdir=$prefix/lib#" -e '/^Requires.private/d' \
	"$target_tcl/lib/pkgconfig/tcl.pc" > "$work/pc/tcl.pc"

podman run --rm \
	-v "$src:/src" \
	-v "$target_tcl/include:$prefix/include:ro" \
	-v "$target_tcl/lib:$prefix/lib:ro" \
	-v "$work/pc:/pc:ro" \
	-v "$build_tcl:/opt/buildtcl:ro" \
	-v "$work/stage:/stage" \
	-e LD_LIBRARY_PATH=/opt/buildtcl/lib \
	-e PATH=/opt/buildtcl/bin:/opt/tc/bin:/usr/bin:/bin \
	-w /src $image sh -euc "
		rm -rf $builddir
		PKG_CONFIG_PATH=/pc meson setup $builddir --buildtype=release \
			--prefix=$prefix --libdir=lib --wrap-mode=nodownload \
			-Dsys_includes=/usr/local/include,/usr/include/x86_64-linux-gnu,/usr/include
		meson compile -C $builddir
		DESTDIR=/stage meson install -C $builddir --skip-subprojects
	"

# Report the newest glibc symbol version the package needs
find "$work/stage" -name '*.so' -exec sh -c 'objdump -T "$1" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1' _ {} \; | sort -uV | tail -1 \
	| sed 's/^/Newest glibc symbol version needed: /'
tar -C "$work/stage$prefix" -czf "$out" lib
echo "Wrote $out:"
tar -tzf "$out" | sed -n '1,3p;$p'
