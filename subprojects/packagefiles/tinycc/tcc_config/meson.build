# Generated config.h lives here, not in the build root, so that parent
# meson projects consuming the libtcc dep don't get tinycc's config.h
# silently injected onto their include search path (where it would
# clash with their own config.h).
configure_file(
  output:         'config.h',
  configuration:  conf,
)

# Append the source+build versions of this directory to the include path
# tinycc itself uses for its compiles. Not part of the public dep.
tinycc_inc += include_directories('.')
