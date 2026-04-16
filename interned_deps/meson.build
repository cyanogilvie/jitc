py = import('python').find_installation()

interned_prefix = meson.current_build_dir() / 'prefix'
interned_build  = meson.current_build_dir() / 'build'

interned = custom_target('interned-deps',
  output: [
    'libtcc.a',
    'interned-deps.stamp',
  ],
  command: [py,
    meson.project_source_root() / 'teabase/tools/interned_deps.py',
    '--manifest',    meson.project_source_root() / 'tools/interned_deps.json',
    '--source-root', meson.project_source_root(),
    '--prefix',      interned_prefix,
    '--build',       interned_build,
    '--copy-artifact', 'lib/libtcc.a:' + meson.current_build_dir() / 'libtcc.a',
    '--stamp',       meson.current_build_dir() / 'interned-deps.stamp',
  ],
  depend_files: files(
    meson.project_source_root() / 'tools/interned_deps.json',
    meson.project_source_root() / 'teabase/tools/interned_deps.py',
  ),
  console:          true,
  build_by_default: true,
)

interned_deps_dep = declare_dependency(
  sources:      interned,
  compile_args: ['-I' + interned_prefix / 'include'],
  link_args: [
    interned[0].full_path(),  # libtcc.a
  ],
)

# The tcc runtime directory (includes, libtcc1.a, etc.) lives at
# interned_prefix / 'lib/tcc' after the build.  We expose the path
# so the root meson.build can set up install rules.
tcc_runtime_dir = interned_prefix / 'lib' / 'tcc'
tcc_include_dir = interned_prefix / 'include'

run_target('deps-clean',
  command: [py, '-c',
    'import os, shutil, sys\n' +
    'for p in sys.argv[1:]:\n' +
    '    if os.path.isdir(p): shutil.rmtree(p, ignore_errors=True)\n' +
    '    else:\n' +
    '        try: os.remove(p)\n' +
    '        except FileNotFoundError: pass',
    interned_prefix,
    interned_build,
    meson.current_build_dir() / 'interned-deps.stamp',
    meson.current_build_dir() / 'libtcc.a',
  ],
)
