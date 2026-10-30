project(
  'xfce4-trayoverflow-plugin',
  'c',
  version: '0.2.21',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.56.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

gettext_package = meson.project_name()
i18n = import('i18n')
cc = meson.get_compiler('c')

# ── Dependencias ──────────────────────────────────────────────────────────
gtk_dep          = dependency('gtk+-3.0',          version: '>= 3.22')
libxfce4panel    = dependency('libxfce4panel-2.0', version: '>= 4.16')
libxfce4ui       = dependency('libxfce4ui-2',      version: '>= 4.16')
libxfce4util     = dependency('libxfce4util-1.0',  version: '>= 4.16')
libxfconf        = dependency('libxfconf-0',       version: '>= 4.16')
dbusmenu_dep     = dependency('dbusmenu-gtk3-0.4')
x11_dep          = dependency('x11', required: false)
# Wayland: el popup se muestra como capa (como los popups del propio panel)
layer_shell_dep  = dependency('gtk-layer-shell-0', version: '>= 0.6.0',
                              required: get_option('gtk-layer-shell'))
math_dep         = cc.find_library('m', required: false)

# Proxy XEmbed → SNI (programa aparte; solo Xlib y GLib)
build_proxy      = get_option('xembed-proxy')
gio_dep          = dependency('gio-2.0',     required: build_proxy)
gio_unix_dep     = dependency('gio-unix-2.0', required: build_proxy)
xcomposite_dep   = dependency('xcomposite',  required: build_proxy)
xdamage_dep      = dependency('xdamage',     required: build_proxy)
xfixes_dep       = dependency('xfixes',      required: build_proxy)
xext_dep         = dependency('xext',        required: build_proxy)
xtst_dep         = dependency('xtst',        required: build_proxy)
x11_proxy_dep    = dependency('x11',         required: build_proxy)
have_proxy = gio_dep.found() and gio_unix_dep.found() and xcomposite_dep.found() \
             and xdamage_dep.found() and xfixes_dep.found() and xext_dep.found() \
             and xtst_dep.found() and x11_proxy_dep.found()

# ── Directorios de instalación ────────────────────────────────────────────
# Los plugins internos se buscan en el libdir del propio panel
# (Debian/Ubuntu: /usr/lib/<multiarch>; Arch/Fedora: /usr/lib o /usr/lib64).
plugin_libdir = get_option('plugin-libdir')
if plugin_libdir == ''
  plugin_libdir = libxfce4panel.get_variable(pkgconfig: 'libdir') / 'xfce4' / 'panel' / 'plugins'
endif

prefix         = get_option('prefix')
datadir        = prefix / get_option('datadir')
localedir      = prefix / get_option('localedir')
plugin_datadir = datadir / 'xfce4' / 'panel' / 'plugins'
proxy_dir      = prefix / get_option('libexecdir') / meson.project_name()
proxy_path     = proxy_dir / 'xembed-proxy'
pkg_datadir    = datadir / meson.project_name()

# ── config.h ──────────────────────────────────────────────────────────────
conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('PACKAGE_LOCALE_DIR', localedir)
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('PROXY_PATH', proxy_path)
conf.set_quoted('DEFAULT_ICONS_PATH', pkg_datadir / 'icon-defaults.conf')
conf.set10('HAVE_XEMBED_PROXY', have_proxy)
if layer_shell_dep.found()
  conf.set('HAVE_GTK_LAYER_SHELL', 1)
endif
configure_file(output: 'config.h', configuration: conf)

add_project_arguments(
  '-DG_LOG_DOMAIN="trayoverflow"',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c',
)

# ── Plugin (.so) ──────────────────────────────────────────────────────────
sources = files(
  'src/to-item.c',
  'src/to-model.c',
  'src/to-sni.c',
  'src/to-settings.c',
  'src/to-button.c',
  'src/to-dialog.c',
  'src/to-legacy.c',
  'src/to-plugin.c',
)

shared_module(
  'trayoverflow',
  sources,
  name_prefix: 'lib',
  include_directories: include_directories('.'),
  dependencies: [gtk_dep, libxfce4panel, libxfce4ui, libxfce4util, libxfconf,
                 dbusmenu_dep, x11_dep, layer_shell_dep, math_dep],
  link_args: ['-Wl,--no-undefined'],
  install: true,
  install_dir: plugin_libdir,
)

# ── Proxy XEmbed → SNI ────────────────────────────────────────────────────
if have_proxy
  executable(
    'xembed-proxy',
    files('proxy/xembed-proxy.c'),
    include_directories: include_directories('.'),
    dependencies: [gio_dep, gio_unix_dep, x11_proxy_dep, xcomposite_dep,
                   xdamage_dep, xfixes_dep, xext_dep, xtst_dep],
    install: true,
    install_dir: proxy_dir,
  )
endif

# ── Archivo .desktop del plugin ───────────────────────────────────────────
desktop_conf = configuration_data()
desktop_conf.set('PLUGIN_LIBDIR', plugin_libdir)

configure_file(
  input: 'data/trayoverflow.desktop.in',
  output: 'trayoverflow.desktop',
  configuration: desktop_conf,
  install: true,
  install_dir: plugin_datadir,
)

# ── Íconos por defecto para apps que se ven mal ───────────────────────────
install_data('data/icon-defaults.conf', install_dir: pkg_datadir)

# ── Traducciones ──────────────────────────────────────────────────────────
subdir('po')

summary({
  'Plugin (.so)': plugin_libdir,
  '.desktop':     plugin_datadir,
  'Traducciones': localedir,
  'Proxy XEmbed': have_proxy ? proxy_path : 'no (faltan bibliotecas X11)',
}, section: 'Instalación')

summary({
  'Popup en Wayland': layer_shell_dep.found() ? 'sí (gtk-layer-shell)' : 'no (falta gtk-layer-shell)',
}, section: 'Wayland')
