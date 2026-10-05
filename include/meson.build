foreach h : ['tclstuff.h', 'tip445.h']
  configure_file(
    input:  '../teabase' / h,
    output: h,
    copy: true,
    install_dir: pkg_dir / 'include',
    install_tag: 'runtime',
  )
endforeach

install_subdir('../tools/chaos-pp/chaos',
  install_dir: pkg_dir / 'include',
  install_tag: 'runtime',
)
install_subdir('../tools/order-pp/inc/order',
  install_dir: pkg_dir / 'include',
  install_tag: 'runtime',
)
