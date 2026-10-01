project('soft-tempest', 'c',
  version: '0.1.0',
  meson_version: '>= 0.61.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=release',
  ])

fs = import('fs')
cc = meson.get_compiler('c')

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DSOFT_TEMPEST_VERSION="@0@"'.format(meson.project_version()),
  language: 'c')

wl_client = dependency('wayland-client', version: '>= 1.20')
wl_egl    = dependency('wayland-egl')
egl       = dependency('egl')
gles      = dependency('glesv2')
libm      = cc.find_library('m', required: false)

# ---------------------------------------------------------------------------
# Protocol code generation (wayland-scanner)
# ---------------------------------------------------------------------------
scanner = find_program('wayland-scanner', native: true)

# wlr-layer-shell is not part of wayland-protocols; it is vendored in protocol/.
# The three standard protocols are taken from the system wayland-protocols when
# a new enough version is installed, otherwise from the vendored copies.
wl_protocols = dependency('wayland-protocols', version: '>= 1.31', required: false)
if wl_protocols.found()
  wl_protocol_dir = wl_protocols.get_variable(pkgconfig: 'pkgdatadir')
  xdg_shell_xml        = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'
  viewporter_xml       = wl_protocol_dir / 'stable/viewporter/viewporter.xml'
  fractional_scale_xml = wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml'
else
  xdg_shell_xml        = 'protocol/xdg-shell.xml'
  viewporter_xml       = 'protocol/viewporter.xml'
  fractional_scale_xml = 'protocol/fractional-scale-v1.xml'
endif

protocol_xmls = [
  'protocol/wlr-layer-shell-unstable-v1.xml',
  xdg_shell_xml,
  viewporter_xml,
  fractional_scale_xml,
]

protocol_sources = []
foreach xml : protocol_xmls
  base = fs.stem(xml)
  protocol_sources += custom_target(base + '-client-header',
    input: xml,
    output: base + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  protocol_sources += custom_target(base + '-private-code',
    input: xml,
    output: base + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
sources = files(
  'src/main.c',
  'src/overlay.c',
  'src/render.c',
  'src/config.c',
)

executable('soft-tempest',
  sources, protocol_sources,
  dependencies: [wl_client, wl_egl, egl, gles, libm],
  install: true)

install_data('soft-tempest.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('soft-tempest.service',
  install_dir: get_option('libdir') / 'systemd/user')
