project('tinycc', 'c',
  version:          '0.9.28rc',
  default_options:  ['warning_level=1', 'werror=false'],
  meson_version:    '>=1.3.0',
)

fs = import('fs')
cc = meson.get_compiler('c')

# === Target architecture ==================================================
arch = host_machine.cpu_family()
arch_to_target = {
  'x86':     'TCC_TARGET_I386',
  'x86_64':  'TCC_TARGET_X86_64',
  'arm':     'TCC_TARGET_ARM',
  'aarch64': 'TCC_TARGET_ARM64',
  'riscv64': 'TCC_TARGET_RISCV64',
}
if arch not in arch_to_target
  error('tinycc: unsupported CPU family: @0@'.format(arch))
endif
tcc_target_define = arch_to_target[arch]

host_os  = host_machine.system()
is_pe    = host_os == 'windows' or host_os == 'cygwin'
is_macho = host_os == 'darwin'
bsd_targetos = {
  'freebsd':   'FreeBSD',
  'netbsd':    'NetBSD',
  'openbsd':   'OpenBSD',
  'dragonfly': 'DragonFly',
}
is_bsd = host_os in bsd_targetos

# === Triplet ==============================================================
arch_t = {
  'x86':     'i386',
  'x86_64':  'x86_64',
  'arm':     'arm',
  'aarch64': 'aarch64',
  'riscv64': 'riscv64',
}[arch]
os_t = {
  'linux':   'linux',
  'freebsd': 'kfreebsd',
  'netbsd':  'netbsd',
  'openbsd': 'openbsd',
  'darwin':  'darwin',
  'windows': 'win32',
  'cygwin':  'win32',
}.get(host_os, host_os)
abi_t = 'gnu'
if arch == 'arm'
  abi_t = 'gnueabihf'
endif
if is_pe or is_macho
  triplet = '@0@-@1@'.format(arch_t, os_t)
else
  triplet = '@0@-@1@-@2@'.format(arch_t, os_t, abi_t)
endif

# === Install directories ==================================================
prefix              = get_option('prefix')
libdir              = get_option('libdir')
runtime_install_dir = get_option('runtime_install_dir')
if runtime_install_dir == ''
  runtime_install_dir = libdir / 'tcc'
endif
runtime_include_dir = runtime_install_dir / 'include'

# === Generated config.h ===================================================
known_cc = ['gcc', 'clang', 'tcc', 'msvc']
cc_short = 'unknown'
if cc.get_id() in known_cc
  cc_short = cc.get_id()
endif

ver_parts = cc.version().split('.')
gcc_major = ver_parts[0]
gcc_minor = '0'
if ver_parts.length() > 1
  gcc_minor = ver_parts[1]
endif

conf = configuration_data()
conf.set_quoted('TCC_VERSION',          meson.project_version())
conf.set       ('CC_NAME',              'CC_' + cc_short)
conf.set       ('GCC_MAJOR',            gcc_major)
conf.set       ('GCC_MINOR',            gcc_minor)
conf.set       (tcc_target_define,      1)
conf.set_quoted('CONFIG_TRIPLET',       triplet)
conf.set_quoted('CONFIG_TCCDIR',        prefix / runtime_install_dir)
conf.set       ('CONFIG_TCC_PREDEFS',   1)

if is_macho
  conf.set('TCC_TARGET_MACHO', 1)
endif
if is_pe
  conf.set('TCC_TARGET_PE', 1)
endif
if is_bsd
  conf.set('TARGETOS_' + bsd_targetos[host_os], 1)
endif

dwarf = get_option('dwarf_version')
if dwarf > 0
  conf.set('CONFIG_DWARF_VERSION', dwarf)
endif

# === Internal include path ================================================
# Used to compile tinycc's own translation units. The public libtcc_dep
# only exposes `include_directories('.')` so consumers can locate libtcc.h
# without inheriting tinycc's private build artefacts (config.h in
# particular, which would shadow their own config.h).
tinycc_inc = [include_directories('.')]

# Generates <build>/tcc_config/config.h from `conf` and appends that dir
# to tinycc_inc.
subdir('tcc_config')

# === c2str helper -> tccdefs_.h ==========================================
# c2str converts include/tccdefs.h to a string literal blob the parser
# splices in at runtime when CONFIG_TCC_PREDEFS=1.
c2str = executable('c2str',
  'conftest.c',
  c_args:           ['-DC2STR'],
  native:           true,
  build_by_default: false,
)
tccdefs_h = custom_target('tccdefs_h',
  input:    'include/tccdefs.h',
  output:   'tccdefs_.h',
  command:  [c2str, '@INPUT@', '@OUTPUT@'],
)

# === libtcc.a (single-source build) ======================================

libtcc = static_library('tcc',
  'libtcc.c',
  tccdefs_h,
  c_args:               ['-DONE_SOURCE=1'],
  include_directories:  tinycc_inc,
  pic:                  true,
  install:              not meson.is_subproject(),
)

libtcc_dep = declare_dependency(
  link_with:            libtcc,
  include_directories:  [include_directories('.'), include_directories('include')],
)
meson.override_dependency('libtcc', libtcc_dep)

if not meson.is_subproject()
  install_headers('libtcc.h')
endif

# === tcc executable (always built; needed to compile libtcc1.a) ==========
tcc_exe_deps = []
if not is_pe
  foreach name : ['m', 'dl']
    d = cc.find_library(name, required: false)
    if d.found()
      tcc_exe_deps += d
    endif
  endforeach
  thread_dep = dependency('threads', required: false)
  if thread_dep.found()
    tcc_exe_deps += thread_dep
  endif
endif

tcc_exe = executable('tcc',
  'tcc.c',
  tccdefs_h,
  c_args:               ['-DONE_SOURCE=0'],
  include_directories:  tinycc_inc,
  link_with:            libtcc,
  dependencies:         tcc_exe_deps,
  install:              not meson.is_subproject() and get_option('install_tcc'),
)

# In a cross build, tcc_exe is a host-machine binary and meson can't run
# it during the build (without an exe wrapper). To compile libtcc1.a we
# need a build-machine binary that targets host_machine — same trick the
# autoconf flow uses with --enable-cross. Build a self-contained native
# tcc from tcc.c (ONE_SOURCE=1, no link to libtcc) for that purpose.
if meson.is_cross_build()
  tcc_for_libtcc1 = executable('tcc-bootstrap',
    'tcc.c',
    tccdefs_h,
    c_args:               ['-DONE_SOURCE=1'],
    include_directories:  tinycc_inc,
    native:               true,
    install:              false,
    build_by_default:     false,
  )
else
  tcc_for_libtcc1 = tcc_exe
endif

# === libtcc1.a runtime ===================================================
# libtcc1 sources are compiled by the freshly-built tcc itself, then
# combined into an archive via tcc's built-in -ar mode.
src_root  = meson.current_source_dir()
build_dir = meson.current_build_dir()
# tcc needs config.h on its -I path for libtcc1 sources (lib/bcheck.c,
# lib/armeabi.c). After the tcc_config subdir, config.h lives there.
tcc_xflags = ['-B' + src_root, '-I' + build_dir / 'tcc_config']

# Files that go into libtcc1.a.
archive_srcs = []
if arch == 'aarch64' or arch == 'riscv64'
  archive_srcs += 'lib/lib-arm64.c'
else
  archive_srcs += 'lib/libtcc1.c'
endif

archive_srcs += [
  'lib/stdatomic.c',
  'lib/atomic.S',
  'lib/builtin.c',
  'lib/alloca.S',
  'lib/alloca-bt.S',
  'lib/tcov.c',
]

if arch == 'arm'
  archive_srcs += ['lib/armeabi.c', 'lib/armflush.c']
elif arch == 'x86'
  archive_srcs += 'lib/pic86.S'
endif

if not is_pe and not is_macho
  archive_srcs += 'lib/dsohandle.c'
endif

# Helper objects installed alongside libtcc1.a, NOT in the archive.
extra_srcs = ['lib/runmain.c', 'lib/bt-exe.c', 'lib/bt-log.c']

compile_with_tcc = [tcc_for_libtcc1, '-c', '@INPUT@', '-o', '@OUTPUT@'] + tcc_xflags

archive_objs = []
foreach s : archive_srcs
  archive_objs += custom_target(fs.stem(s) + '.o',
    input:   s,
    output:  fs.stem(s) + '.o',
    command: compile_with_tcc,
  )
endforeach

libtcc1_a = custom_target('libtcc1_a',
  input:        archive_objs,
  output:       'libtcc1.a',
  command:      [tcc_for_libtcc1, '-ar', 'rcs', '@OUTPUT@', '@INPUT@'],
  install:      true,
  install_dir:  runtime_install_dir,
)

extra_objs = []
foreach s : extra_srcs
  extra_objs += custom_target(fs.stem(s) + '.o',
    input:        s,
    output:       fs.stem(s) + '.o',
    command:      compile_with_tcc,
    install:      true,
    install_dir:  runtime_install_dir,
  )
endforeach

# bcheck.o needs -bt for backtrace support.
bcheck_o = custom_target('bcheck_o',
  input:        'lib/bcheck.c',
  output:       'bcheck.o',
  command:      [tcc_for_libtcc1, '-c', '@INPUT@', '-o', '@OUTPUT@', '-bt'] + tcc_xflags,
  install:      true,
  install_dir:  runtime_install_dir,
)

# === Runtime headers =====================================================
install_headers(
  files(
    'include/float.h',
    'include/stdalign.h',
    'include/stdarg.h',
    'include/stdatomic.h',
    'include/stdbool.h',
    'include/stddef.h',
    'include/stdnoreturn.h',
    'include/tccdefs.h',
    'include/tgmath.h',
    'include/varargs.h',
    'tcclib.h',
  ),
  install_dir:  runtime_include_dir,
)

# === Variables for parent meson projects =================================
# Parent projects consuming tinycc as a subproject can:
#   tinycc      = subproject('tinycc')
#   libtcc_dep  = dependency('libtcc')
#   tcc_runtime = tinycc.get_variable('tcc_runtime_dir')   # build dir holding libtcc1.a
#   tcc_runtime_files = tinycc.get_variable('tcc_runtime_files')
tcc_runtime_dir   = build_dir
tcc_runtime_files = [libtcc1_a, bcheck_o] + extra_objs
