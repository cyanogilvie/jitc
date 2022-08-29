#!/bin/sh

git config --global --add safe.directory /build/tinycc
git config --global --add safe.directory /build/tinycc/tinycc

cp -a /src/tinycc .
cd tinycc

#export CFLAGS="-O3 -march=haswell -flto"
#export CFLAGS="-O3 -march=native -flto"
export CFLAGS="-O3 -flto"

autoconf
./configure CFLAGS="${CFLAGS}" --with-tcl=/usr/local/lib --enable-symbols --enable-64bit
make clean
#make CFLAGS="${CFLAGS} -fprofile-generate=prof -lgcov" tcc
#make CFLAGS="${CFLAGS} -fprofile-generate=prof -lgcov" -C tinycc test
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" binaries
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" test
make CFLAGS="${CFLAGS} -fprofile-generate=prof" LDFLAGS="-lgcov" benchmark
make -C tinycc test
make clean
make CFLAGS="${CFLAGS} -fprofile-use=prof -fprofile-partial-training -Wno-coverage-mismatch -Wno-missing-profile" binaries
make CFLAGS="${CFLAGS} -fprofile-use=prof -fprofile-partial-training -Wno-coverage-mismatch -Wno-missing-profile" benchmark
