project('jitc', 'c',
  version:          '0.7.7',
  default_options:  ['c_std=gnu2y,gnu23,gnu2x', 'warning_level=3', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

add_project_arguments(cc.get_supported_arguments([
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-deprecated-octal-literals',
]), language: 'c')

# --- Configuration data (becomes config.h) ---
conf = configuration_data()
conf.set10('TESTMODE', get_option('testmode'))

if get_option('stubsmode')
  conf.set10('STUBSMODE', get_option('stubsmode'))
endif


# TIP445 shim for Tcl 8.6 compat
tea_want_shims = ['tip445']
tea_want_defer = true

# --- Accumulator variables for teabase ---
deps          = []
inc           = []
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
  'generic/jitc.h.in',
)

tea_genstubs = files('generic/jitc.decls', 'generic/jitcStubLib.c')

# --- Teabase init (must come first) ---
subdir('teabase/init')

tinycc = subproject('tinycc', default_options: ['runtime_install_dir=' + pkg_dir])
libtcc_dep = dependency('libtcc')
deps += libtcc_dep

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

# For packcc and lemon:
tea_host_env.prepend('PATH', meson.project_build_root())

# --- re2c as meson subproject (always built, never found on system) ---
# Pass pkg_dir so re2c installs alongside the package.
re2c_subproj = subproject('re2c', default_options: ['pkg_install_dir=' + pkg_dir])
re2c_exe = re2c_subproj.get_variable('re2c_exe')
tea_host_env.prepend('PATH', fs.parent(re2c_exe.full_path()))

# --- dedup as meson subproject (test dependency) ---
dedup_dep = dependency('dedup', version: '>=0.9.19', fallback: ['dedup', 'dedup_dep'])
tea_host_env.prepend('TCLLIBPATH', dedup_dep.get_variable('tcl_pkg_path'), separator: ' ')

# --- Teabase main (builds shared_module, sets up tests) ---
subdir('teabase')

# --- Install jitclib Tcl modules ---
subdir('tm/jitclib')

# --- Install include/ (tclstuff.h, chaos-pp, order-pp) ---
subdir('include')

# --- Documentation ---
subdir('doc')
