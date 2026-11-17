# Pruebas rápidas, sin panel ni pantalla:  meson test -C build
unit_inc  = include_directories('../..', '../../src')
unit_deps = [gtk_dep, libxfce4panel, libxfce4ui, libxfce4util, libxfconf,
             dbusmenu_dep, x11_dep, math_dep]
bash      = find_program('bash')
con_dbus  = files('con-dbus.sh')

# Configuración: cada prueba incluye el .c que prueba, para llegar a sus
# funciones internas
test_settings = executable(
  'test-settings',
  files('test-settings.c'),
  include_directories: unit_inc,
  dependencies: unit_deps,
)
test('configuracion', bash, args: [con_dbus, test_settings], timeout: 90)

test_sni = executable(
  'test-sni',
  files('test-sni.c', '../../src/to-item.c', '../../src/to-model.c'),
  include_directories: unit_inc,
  dependencies: unit_deps,
)
test('apps-modernas', test_sni, timeout: 60)

# Proxy de apps antiguas: nombre de apps Java y conversión de la imagen
test_proxy = executable(
  'test-proxy',
  files('test-proxy.c', '../../proxy/xp-logic.c'),
  include_directories: [unit_inc, include_directories('../../proxy')],
  dependencies: [dependency('glib-2.0')],
)
test('proxy', test_proxy, timeout: 60)
