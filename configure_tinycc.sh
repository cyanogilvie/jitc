#!/bin/sh -e

cd tinycc
if test -e /etc/alpine-release
then
    ./configure --config-musl "$@"
else
    ./configure "$@"
fi

