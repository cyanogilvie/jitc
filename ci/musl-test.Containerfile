# Image for testing the musl build of jitc end-to-end.
# Mirrors the runtime environment used in alpine-tcl.
FROM alpine:3.23.4

RUN apk add --no-cache --update \
		bsd-compat-headers \
		build-base \
		cmake \
		git \
		linux-headers \
		meson \
		musl-obstack-dev \
		ninja \
		pkgconfig \
		python3 \
		valgrind \
		zip

# Match the user namespace so files written in /src land owned by the
# invoking host user (rootless podman maps uid 0 in the container to the
# host user's uid by default).
WORKDIR /src
