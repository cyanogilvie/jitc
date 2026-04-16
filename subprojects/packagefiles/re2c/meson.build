# Minimal re2c build from pre-generated bootstrap sources.
# Only builds the C-language re2c executable needed for lexer generation.

project(
  're2c',
  'cpp',
  version: '4.3',
  license: 'Unlicense',
  default_options: [
    'cpp_std=c++11',
    'warning_level=0',
  ],
  meson_version: '>=1.3.0',
)

cpp = meson.get_compiler('cpp')

conf_data = configuration_data()
conf_data.set_quoted('PROJECT_VERSION', meson.project_version())

defines = ['-DPACKAGE_VERSION=' + conf_data.get('PROJECT_VERSION')]

foreach header : ['sys/types.h', 'sys/stat.h', 'fcntl.h', 'unistd.h', 'io.h']
  name = 'HAVE_' + header.underscorify().to_upper()
  conf_data.set10(name, cpp.has_header(header))
  defines += ['-D@0@=@1@'.format(name, conf_data.get(name))]
endforeach

defines += [
  '-DRE2C_STDLIB_DIR=""',
]

configure_file(
  output: 'config.h',
  configuration: conf_data,
)

# Copy bootstrap generated files to build dir so that includes like
# "src/parse/lexer.h" resolve via the build dir include path
python = find_program('python3')
run_command(python, '-c',
  'import shutil; shutil.copytree("bootstrap/src", "src", dirs_exist_ok=True)',
  check: true,
)

_pkg_install_dir = get_option('pkg_install_dir')

re2c_exe = executable('re2c',
  [
    'bootstrap/src/default_syntax_c.h',
    'bootstrap/src/msg/help_re2c.cc',
    'bootstrap/src/msg/ver_to_vernum.cc',
    'bootstrap/src/options/parse_opts.cc',
    'bootstrap/src/parse/conf_lexer.cc',
    'bootstrap/src/parse/conf_parser.cc',
    'bootstrap/src/parse/conf_parser.h',
    'bootstrap/src/parse/lexer.cc',
    'bootstrap/src/parse/lexer.h',
    'bootstrap/src/parse/parser.cc',
    'bootstrap/src/parse/parser.h',
    'src/adfa/adfa.cc',
    'src/cfg/cfg.cc',
    'src/cfg/compact.cc',
    'src/cfg/dce.cc',
    'src/cfg/freeze.cc',
    'src/cfg/interfere.cc',
    'src/cfg/liveanal.cc',
    'src/cfg/normalize.cc',
    'src/cfg/optimize.cc',
    'src/cfg/rename.cc',
    'src/cfg/varalloc.cc',
    'src/codegen/helpers.cc',
    'src/codegen/output.cc',
    'src/codegen/pass1_analyze.cc',
    'src/codegen/pass2_generate.cc',
    'src/codegen/pass3_fixup.cc',
    'src/codegen/pass4_render.cc',
    'src/debug/dump_adfa.cc',
    'src/debug/dump_cfg.cc',
    'src/debug/dump_dfa.cc',
    'src/debug/dump_dfa_tree.cc',
    'src/debug/dump_interf.cc',
    'src/debug/dump_nfa.cc',
    'src/dfa/closure.cc',
    'src/dfa/dead_rules.cc',
    'src/dfa/determinization.cc',
    'src/dfa/fallback_tags.cc',
    'src/dfa/fillpoints.cc',
    'src/dfa/find_state.cc',
    'src/dfa/minimization.cc',
    'src/dfa/tagver_table.cc',
    'src/dfa/tcmd.cc',
    'src/encoding/ebcdic.cc',
    'src/encoding/enc.cc',
    'src/encoding/range_suffix.cc',
    'src/encoding/utf16.cc',
    'src/encoding/utf8.cc',
    'src/main.cc',
    'src/msg/msg.cc',
    'src/msg/warn.cc',
    'src/nfa/re_to_nfa.cc',
    'src/options/opt.cc',
    'src/options/symtab.cc',
    'src/parse/ast.cc',
    'src/parse/input.cc',
    'src/regexp/ast_to_re.cc',
    'src/regexp/default_tags.cc',
    'src/regexp/fixed_tags.cc',
    'src/regexp/nullable.cc',
    'src/regexp/regexp.cc',
    'src/regexp/split_charset.cc',
    'src/skeleton/control_flow.cc',
    'src/skeleton/generate_code.cc',
    'src/skeleton/generate_data.cc',
    'src/skeleton/maxpath.cc',
    'src/skeleton/skeleton.cc',
    'src/util/file_utils.cc',
    'src/util/range.cc',
    'src/util/string_utils.cc',
  ],
  include_directories: include_directories('.'),
  cpp_args: defines,
  install:     _pkg_install_dir != '',
  install_dir: _pkg_install_dir != '' ? _pkg_install_dir : [],
)

meson.override_dependency(meson.project_name(), declare_dependency(
  variables: {
    'prog': re2c_exe.full_path(),
  },
))

meson.override_find_program('re2c', re2c_exe)
