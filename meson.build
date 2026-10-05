project('jitc', 'c',
  version:          '0.8.3',
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

# libslimcc (slimcc reworked as a C23+defer -> MIR JIT frontend) replaces
# libtcc as the in-process compiler backend; the 'slimcc' dependency links the
# static library and pulls 'mir' (the JIT codegen substrate) in via the wraps.
deps += dependency('slimcc')

# libtcc bundled libc headers (stddef.h etc.) and preprocessed user code with a
# built-in system include search path. libslimcc, in library mode, ships only
# the freestanding headers (stdarg.h, stddef.h, _BitInt builtins, ...) and
# configures NO system include paths — so the host's libc header directories
# (where <stdio.h>, <tcl.h>'s dependencies, ... live) must be supplied by the
# consumer. Detect the build host C compiler's <...> search list at configure
# time and bake it into the build so jitc can hand it to slimcc_compile.
# -Dsys_includes overrides the probe: a build with a non-system toolchain
# (a conda gcc for an old-glibc host, say) would otherwise bake in that
# toolchain's private dirs rather than the target host's.
_sysinc = get_option('sys_includes')
if _sysinc.length() == 0
  _probe = run_command(cc.cmd_array() + ['-E', '-Wp,-v', '-xc', '/dev/null'],
    check: false)
  _in_list = false
  foreach _line : _probe.stderr().split('\n')
    _t = _line.strip()
    if _t == '#include <...> search starts here:'
      _in_list = true
    elif _t == 'End of search list.'
      _in_list = false
    elif _in_list and _t != ''
      # gcc annotates framework dirs with " (framework directory)"; drop that.
      _sysinc += _t.split(' ')[0]
    endif
  endforeach
endif
if _sysinc.length() == 0
  warning('Could not detect system include paths from the C compiler; ' +
    'JIT compiles that #include <stdio.h> etc. may fail to find libc headers')
endif
add_project_arguments(
  '-DJITC_SYS_INCLUDES="@0@"'.format(':'.join(_sysinc)),
  language: 'c')

# Copies (for the re2c install below) go through an inline Python one-liner
# rather than `cp`: meson always has a Python 3 to hand (it runs on one),
# whereas `cp` isn't portable to every target's build host. shutil.copy
# preserves the mode bits (keeps re2c +x).
py = import('python').find_installation()
copy_file = [py, '-c', 'import shutil, sys; shutil.copy(sys.argv[1], sys.argv[2])']

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
