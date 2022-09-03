#!/bin/sh

test -x /src/parse_args || (echo "Should only be run from a container, like \"docker-compose run --rm benchmark\""; exit 1)

apt install -y libtool python3 pandoc

git config --global --add safe.directory /build/jitc
git config --global --add safe.directory /build/jitc/tinycc
git config --global --add safe.directory /build/jitc/tools/re2c
git config --global --add safe.directory /build/jitc/tools/packcc
git config --global --add safe.directory /build/jitc/tools/sqlite

cp -a /src/jitc .
cd jitc
rm -r tclconfig
ln -s /src/tclconfig

export CFLAGS="-O3 -march=haswell -flto"
#export CFLAGS="-O3 -march=native -flto"
#export CFLAGS="-O3 -flto"
#export CFLAGS="-O2 -flto"

autoconf
./configure CFLAGS="${CFLAGS}" --with-tcl=/usr/local/lib --enable-symbols --enable-64bit
make clean
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" binaries
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" test
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" benchmark
#make -C tinycc test
make clean
make CFLAGS="${CFLAGS} -fprofile-use=prof -fprofile-partial-training -Wno-coverage-mismatch -Wno-missing-profile" binaries
make CFLAGS="${CFLAGS} -fprofile-use=prof -fprofile-partial-training -Wno-coverage-mismatch -Wno-missing-profile" benchmark
ls -la
make install
tclsh bench/run.tcl
