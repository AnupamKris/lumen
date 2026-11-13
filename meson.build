project('lumen', 'c', version: '0.1.0', license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=2', 'buildtype=release', 'b_lto=true'])

add_project_arguments('-DWLR_USE_UNSTABLE', '-Wno-unused-parameter',
  '-Wno-missing-field-initializers', language: 'c')

wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.32')
wlroots = dependency('wlroots-0.18')
scenefx = dependency('scenefx-0.2')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
cairo = dependency('cairo')
pango = dependency('pangocairo')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
pixman = dependency('pixman-1')
math = meson.get_compiler('c').find_library('m')

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))

server_protos = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
  wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
]
client_protos = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
]

server_src = []
foreach p : server_protos
  name = p.split('/')[-1].replace('.xml', '')
  server_src += custom_target(name + '-server-h', input: p,
    output: name + '-protocol.h', command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
endforeach

client_src = []
foreach p : client_protos
  name = p.split('/')[-1].replace('.xml', '')
  client_src += custom_target(name + '-client-h', input: p,
    output: name + '-client-protocol.h', command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  client_src += custom_target(name + '-code', input: p,
    output: name + '-protocol.c', command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

executable('lumen',
  files('src/main.c', 'src/view.c', 'src/layout.c', 'src/layer.c', 'src/output.c',
        'src/input.c', 'src/deco.c', 'src/config.c', 'src/actions.c', 'src/ipc.c', 'src/lock.c'),
  server_src,
  # scenefx MUST come before wlroots: both export wlr_scene_*, and the first
  # DT_NEEDED library wins symbol resolution. Wrong order = mixed struct layouts.
  dependencies: [scenefx, wayland_server, wlroots, xkbcommon, libinput, cairo, pango, libdrm,
                 pixman, math],
  install: true)

if get_option('panel')
  executable('lumen-panel', files('panel/lumen-panel.c'), client_src,
    dependencies: [wayland_client, cairo, pango, math],
    install: true)
endif

install_data('data/lumen.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
install_data('data/lumen-session', 'data/lumen-lock', 'data/lumen-power', 'data/lumen-screenshot',
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('data/lumen.conf', 'data/fuzzel.ini', 'data/mako.conf',
  install_dir: get_option('datadir') / 'lumen')
