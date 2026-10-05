# Build environment for jitc on old glibc hosts (Ubuntu 16.04 / glibc 2.23,
# e.g. legacy NaviServer servers).  jitc needs gcc >= 13 (C23, the defer
# polyfill) and meson >= 1.3, neither of which xenial packages.  conda-forge's
# toolchain fills the gap without compiling gcc: its gcc 14 builds against a
# glibc 2.17 sysroot, so everything it links (jitc's .so, the static re2c and
# its libstdc++) needs nothing newer than 2.17 at runtime, and its binaries run
# on xenial's 2.23.
FROM ubuntu:16.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
		bzip2 ca-certificates curl file libc6-dev pkg-config \
 && rm -rf /var/lib/apt/lists/*

# xenial's CA bundle is too old for some TLS endpoints: fetch micromamba with
# --insecure only to bootstrap; conda itself then uses its own certifi bundle.
ARG MAMBA_VER=2.3.2-0
RUN curl -fsSLk https://github.com/mamba-org/micromamba-releases/releases/download/${MAMBA_VER}/micromamba-linux-64 \
		-o /usr/local/bin/micromamba \
 && chmod +x /usr/local/bin/micromamba

ENV MAMBA_ROOT_PREFIX=/opt/conda
RUN micromamba create -y -p /opt/tc -c conda-forge \
		gcc_linux-64=14 gxx_linux-64=14 sysroot_linux-64=2.17 \
		meson ninja python=3.12 \
 && micromamba clean -a -y

# The conda compiler wrappers are named x86_64-conda-linux-gnu-*; meson picks
# them up through CC/CXX.  The toolchain is used for building only: the
# system include dirs jitc bakes in (-Dsys_includes) are the target host's,
# not the conda sysroot's.
ENV PATH=/opt/tc/bin:$PATH \
	CC=x86_64-conda-linux-gnu-gcc \
	CXX=x86_64-conda-linux-gnu-g++ \
	AR=x86_64-conda-linux-gnu-ar

WORKDIR /src
