#!/usr/bin/env python3
# App antigua de prueba (bandeja clásica XEmbed, GtkStatusIcon).
# Anota en el archivo $1 los giros, clics y su menú propio (clic derecho).
import sys, warnings
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import GLib, Gtk

# Clave fija para el plugin (WM_CLASS): "legacy-app"
GLib.set_prgname('legacy-app')

warnings.simplefilter('ignore')
log = open(sys.argv[1], 'a', buffering=1)

icon = Gtk.StatusIcon.new_from_icon_name('dialog-information')
icon.set_title('legacy-app')
icon.set_name('legacy-app')
icon.connect('scroll-event', lambda w, e: log.write('SCROLL %s\n' % e.direction.value_nick))
icon.connect('activate', lambda w: log.write('ACTIVATE\n'))

menu = Gtk.Menu()
item = Gtk.MenuItem(label='Opcion')
item.connect('activate', lambda *a: log.write('ELEGIDA\n'))
menu.append(item)
menu.show_all()
menu.connect('show', lambda *a: log.write('MENU-ABIERTO\n'))
menu.connect('hide', lambda *a: log.write('MENU-CERRADO\n'))
icon.connect('popup-menu', lambda i, b, t: menu.popup(None, None, None, None, b, t))

Gtk.main()
