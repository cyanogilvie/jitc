project('jitc', 'c',
  version:          '0.7.1',
  default_options:  ['c_std=c11', 'warning_level=3', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

add_project_arguments('-Wno-missing-field-initializers', '-Wno-unused-parameter', language: 'c')

# --- Configuration data (becomes config.h) ---
conf = configuration_data()
conf.set10('TESTMODE', get_option('testmode'))

# TIP445 shim for Tcl 8.6 compat
tea_want_shims = ['tip445']

# --- Accumulator variables for teabase ---
deps = []
inc = []
teaconf_files = []

# --- Package sources ---
inc += include_directories('generic')
pkg_sources = files(
  'generic/jitc.c',
  'teabase/names.c',
)

teaconf_files += files(
  'loadjitc.tcl.in',
  'library/jitc.tcl',
)

# --- Teabase init (must come first) ---
subdir('teabase/init')

# --- Stubs generation ---
genstubs = custom_target('genstubs',
  output: ['jitcStubInit.c', 'jitcDecls.h'],
  command: [
    build_tclsh,
    meson.project_source_root() / 'teabase/tools/genStubs.tcl',
    meson.project_build_root(),
    meson.project_source_root() / 'generic/jitc.decls',
  ],
  install:     true,
  install_dir: [false, pkg_dir],
)

deps += declare_dependency(sources: genstubs)

# --- Tinycc via interned_deps ---
subdir('interned_deps')
deps += interned_deps_dep

# --- Build tools: packcc, lemon (single C files) ---
# These are shipped with the installed package for end-user JIT use.
packcc_exe = executable('packcc',
  'tools/packcc/src/packcc.c',
  override_options: ['warning_level=1'],
  install:     true,
  install_dir: pkg_dir,
)

lemon_exe = executable('lemon',
  'tools/sqlite/tool/lemon.c',
  override_options: ['warning_level=1'],
  install:     true,
  install_dir: pkg_dir,
)

# --- re2c as meson subproject (always built, never found on system) ---
# Pass pkg_dir so re2c installs alongside the package.
re2c_subproj = subproject('re2c', default_options: ['pkg_install_dir=' + pkg_dir])
re2c_exe = re2c_subproj.get_variable('re2c_exe')

# --- dedup as meson subproject (test dependency) ---
dedup_subproj = subproject('dedup')

# --- Teabase main (builds shared_module, sets up tests) ---
subdir('teabase')

# Add dedup subproject's package dir to TCLLIBPATH so tests can find it
_dedup_devenv = environment()
_dedup_devenv.append('TCLLIBPATH', meson.project_build_root() / 'subprojects' / 'dedup' / 'teabase', separator: ' ')
meson.add_devenv(_dedup_devenv)

# --- Configure the public header from .h.in ---
configure_file(
  configuration:  teaconf,
  input:          'generic/jitc.h.in',
  output:         'jitc.h',
  install_dir:    pkg_dir,
)

# --- Stubs library ---
# Only needs Tcl stubs and generated headers, not libtcc.
stubs_deps = [tcl_dep, declare_dependency(sources: genstubs)]
stubs_lib = static_library(meson.project_name() + 'stub',
  'generic/jitcStubLib.c',
  dependencies: stubs_deps,
  include_directories: inc,
  install:     true,
  install_dir: pkg_dir,
)

# --- Install jitclib Tcl modules ---
subdir('jitclib')

# --- Install tcc runtime into pkg_dir/tcc/ ---
py = import('python').find_installation()
meson.add_install_script(py, '-c',
  '''
import os, shutil, sys
src = sys.argv[1]
destdir = os.environ.get("DESTDIR", "")
dst = destdir + sys.argv[2]
if os.path.isdir(src):
    shutil.copytree(src, dst, dirs_exist_ok=True)
''',
  tcc_runtime_dir,
  pkg_dir,
)

# --- Install include/ (tclstuff.h, chaos-pp, order-pp) ---
subdir('include')

# --- Documentation ---
subdir('doc')
