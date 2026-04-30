foreach f : [
  'asciilower-0.1.tm',
  'intrep_registry-0.1.tm',
  'json-0.1.tm',
  'load_cdef-0.1.tm',
  #'mediatype-0.1.tm',
  'mtag-0.1.tm',
  'obstackpool-0.2.tm',
  'parse_tcc_errors-0.1.tm',
  #'re2tcl-0.1.tm',
  #'reswitch-0.1.tm',
  'tcltime-0.2.tm',
  'tempdir-0.1.tm',
  'tip445-0.1.tm',
  'unicode-0.1.tm',
]
  configure_file(
    input:        f,
    output:       f,
    copy:         true,
    install_dir:  pkg_dir / 'tm/jitclib',
    install_tag: 'runtime',
  )
endforeach
