#!/usr/bin/env python3
# App antigua de prueba que imita a ibus 1.5 (ui/gtk3/panel.vala): si el
# ícono nace sin tamaño, espera a "size" y conecta "activate" CADA VEZ que
# el tamaño cambia, sin desconectar el anterior. Con un cambio de tamaño
# después de anclar, un clic "abre" dos menús. Anota en $1: "INIT <tamaño>"
# y "MENU" por cada menú que abriría.
import sys, warnings
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('GdkX11', '3.0')
from gi.repository import GdkX11, GLib, Gtk

warnings.simplefilter('ignore')
GLib.set_prgname('ibus-like-app')   # clave fija para el plugin
log = open(sys.argv[1], 'a', buffering=1)
# Como ibus: el ícono nace vacío; la imagen se pone recién al conocer el
# tamaño (en init)
icon = Gtk.StatusIcon()
# Como ibus: cambiar el nombre recién creado el ícono hace que GTK recree
# su ventana (el proxy ve un anclado, un desanclado y otro anclado)
icon.set_name('ibus-like-app')
icon.set_title('ibus-like-app')


def init(*args):
    log.write('INIT %d\n' % icon.get_size())
    icon.set_from_icon_name('dialog-information')
    icon.connect('activate', lambda i: log.write('MENU\n'))


if icon.get_size() > 0:
    init()
else:
    icon.connect('notify::size', init)


# Como ibus real: ya anclado, la app pide achicar su ventana (a 16x16). Una
# bandeja real no se lo concede; si se le concede, el tamaño que ve la app
# cambia según el momento y al reiniciar el panel conecta otro menú.
def ask_smaller():
    xid = icon.get_x11_window_id()
    if xid:
        display = GdkX11.X11Display.get_default()
        window = GdkX11.X11Window.foreign_new_for_display(display, xid)
        window.resize(16, 16)
        log.write('PIDE 16x16\n')
    return False


GLib.timeout_add(1500, ask_smaller)
Gtk.main()
