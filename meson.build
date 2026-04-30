project('jitc', 'c',
  version:          '0.7.11',
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
conf.set10('TESTMODE',  get_option('testmode'))
conf.set10('STUBSMODE', get_option('stubsmode'))

inc = [
  include_directories('generic'),
]
pkg_sources = files(
  'generic/jitc.c',
  'teabase/names.c',
)
teaconf_files = files(
  'loadjitc.tcl.in',
  'library/jitc.tcl',
  'generic/jitc.h.in',
)
tea_genstubs = files('generic/jitc.decls', 'generic/jitcStubLib.c')

# --- Teabase init (must come first) ---
tea_want_shims = ['tip445']
tea_want_defer = true
subdir('teabase/init')

tinycc = subproject('tinycc', default_options: ['runtime_install_dir=' + pkg_dir])
deps += dependency('libtcc')
tea_extra_depends += tinycc.get_variable('tcc_runtime_files')   # Force `meson test` to build tcc and wait for it to finish

# --- Build tools: packcc, lemon (single C files), re2c ---
# These are shipped with the installed package for end-user JIT use,
tea_extra_depends += executable('packcc',
  'tools/packcc/src/packcc.c',
  override_options: ['warning_level=1'],
  install:     true,
  install_dir: pkg_dir,
)

tea_extra_depends += executable('lemon',
  'tools/sqlite/tool/lemon.c',
  override_options: ['warning_level=1'],
  install:     true,
  install_dir: pkg_dir,
)

# --- re2c as meson subproject (always built, never found on system) ---
# Pass pkg_dir so re2c installs alongside the package.
tea_extra_depends += subproject('re2c', default_options: ['pkg_install_dir='+pkg_dir]).get_variable('re2c_exe')

# --- Test deps ---
tea_test_deps += dependency('dedup', version: '>=0.9.19')

# --- Benchmarking deps (optional, only used if -Dbenchmark != disabled) ---
tea_bench_deps += dependency('tcllib',  required: get_option('benchmark'))
tea_bench_deps += dependency('rl_json', required: get_option('benchmark'))

subdir('teabase')
subdir('tm/jitclib')
subdir('include')
subdir('doc')
