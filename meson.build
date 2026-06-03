project('jitc', 'c',
  version:          '0.7.17',
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

tinycc = subproject('tinycc')
deps += dependency('libtcc')

# tinycc and re2c are subprojects, but their *runtime* artifacts are essential
# to jitc's JIT: libtcc1.a + helper objects (to link code tcc compiles at
# runtime), the compiler-provided headers tcc bundles (stddef.h etc., which
# libtcc needs to preprocess user code), and the re2c lexer generator. These
# must land in jitc's versioned pkg_dir and survive the release build's
# `meson install --tags runtime`. We can't lean on the subprojects' own
# install rules for that: subproject default_options that carry pkg_dir are
# pinned at first configure (a build dir reused across a version bump keeps
# installing to the stale dir), and meson tags subproject headers 'devel' /
# custom_target outputs untagged, both of which --tags runtime then drops. So
# install them here, into pkg_dir, explicitly tagged 'runtime'. The subprojects
# skip their own install when consumed as subprojects.
#
# Copies go through an inline Python one-liner rather than `cp`: meson always
# has a Python 3 to hand (it runs on one), whereas `cp` isn't portable to every
# target's build host. shutil.copy preserves the mode bits (keeps re2c +x).
py = import('python').find_installation()
copy_file = [py, '-c', 'import shutil, sys; shutil.copy(sys.argv[1], sys.argv[2])']

# tcc runtime headers (stddef.h etc.) are plain source files.
install_data(tinycc.get_variable('tcc_runtime_headers'),
  install_dir: pkg_dir / 'include',
  install_tag: 'runtime',
)

# tcc runtime library + helper objects are built by tcc (custom_target
# outputs), which install_data/fs.copyfile can't install directly — wrap each
# in a trivial copy target we can install from here. Doubles as the
# `meson test` build dependency that forces tcc to finish building first.
foreach _f : tinycc.get_variable('tcc_runtime_files')
  tea_extra_depends += custom_target('install-' + fs.name(_f.full_path()),
    input:        _f,
    output:       fs.name(_f.full_path()),
    command:      copy_file + ['@INPUT@', '@OUTPUT@'],
    install:      true,
    install_dir:  pkg_dir,
    install_tag:  'runtime',
  )
endforeach

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
# Built by the subproject; installed here (tagged runtime) so it lands in
# pkg_dir robustly regardless of the subproject's pinned install-dir option.
re2c = subproject('re2c')
tea_extra_depends += custom_target('install-re2c',
  input:        re2c.get_variable('re2c_exe'),
  output:       're2c',
  command:      copy_file + ['@INPUT@', '@OUTPUT@'],
  install:      true,
  install_dir:  pkg_dir,
  install_tag:  'runtime',
  install_mode: 'rwxr-xr-x',
)

# --- Test deps ---
tea_test_deps += dependency('dedup', version: '>=0.9.19')

# --- Benchmarking deps (optional, only used if -Dbenchmark != disabled) ---
tea_bench_deps += dependency('tcllib',  required: get_option('benchmark'))
tea_bench_deps += dependency('rl_json', required: get_option('benchmark'))

subdir('teabase')
subdir('tm/jitclib')
subdir('include')
subdir('doc')
