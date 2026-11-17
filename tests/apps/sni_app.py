#!/usr/bin/env python3
# App moderna de prueba (StatusNotifierItem, sin menú D-Bus).
# Uso: sni_app.py ARCHIVO_LOG ID [MODO]
# Modos (apps "rebeldes"):
#   reregistra  se vuelve a registrar cada vez que el watcher avisa de un
#               registro (libappindicator viejo, p. ej. indicator-keylock)
#   tarde       se registra antes de estar lista: responde 1,2 s después
#   ruidosa     avisa NewIcon sin parar (cada 3 ms) sin cambiar nada
#   etiqueta    a los 4 s pone un texto junto al ícono (XAyatanaLabel)
# El log anota REGISTER en cada registro y GETALL en cada lectura completa.
# También cada llamada (ACTIVATE x y, PROVIDEXDGACTIVATIONTOKEN token, …).
import os, sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

log = open(sys.argv[1], 'a', buffering=1)
app_id = sys.argv[2] if len(sys.argv) > 2 else 'sni-app'
mode = sys.argv[3] if len(sys.argv) > 3 else ''

XML = '''<node><interface name="org.kde.StatusNotifierItem">
<property name="Category" type="s" access="read"/>
<property name="Id" type="s" access="read"/>
<property name="Title" type="s" access="read"/>
<property name="Status" type="s" access="read"/>
<property name="IconName" type="s" access="read"/>
<property name="ItemIsMenu" type="b" access="read"/>
<property name="XAyatanaLabel" type="s" access="read"/>
<property name="XAyatanaLabelGuide" type="s" access="read"/>
<method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
<method name="ProvideXdgActivationToken"><arg type="s" direction="in"/></method>
<signal name="NewIcon"/>
<signal name="XAyatanaNewLabel"><arg type="s"/><arg type="s"/></signal>
</interface></node>'''

PROPS = {'Category': GLib.Variant('s', 'ApplicationStatus'),
         'Id': GLib.Variant('s', app_id),
         'Title': GLib.Variant('s', app_id),
         'Status': GLib.Variant('s', 'Active'),
         'IconName': GLib.Variant('s', 'dialog-information'),
         'ItemIsMenu': GLib.Variant('b', False),
         'XAyatanaLabel': GLib.Variant('s', ''),
         'XAyatanaLabelGuide': GLib.Variant('s', '')}

node = Gio.DBusNodeInfo.new_for_xml(XML)
name = 'org.kde.StatusNotifierItem-%d-1' % os.getpid()


def call(conn, sender, path, iface, method, args, inv):
    log.write('%s %s\n' % (method.upper(), ' '.join(str(a) for a in args.unpack())))
    inv.return_value(None)


def get_prop(conn, sender, path, iface, prop):
    if prop == 'Id':   # una vez por lectura completa (GetAll)
        log.write('GETALL\n')
    return PROPS.get(prop)


def register(conn):
    log.write('REGISTER\n')
    conn.call('org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher',
              'org.kde.StatusNotifierWatcher', 'RegisterStatusNotifierItem',
              GLib.Variant('(s)', (name,)), None, Gio.DBusCallFlags.NONE, -1, None, None)


def acquired(conn, n):
    register(conn)
    # Volver a registrarse si el watcher aparece después (o se reinicia)
    Gio.bus_watch_name_on_connection(conn, 'org.kde.StatusNotifierWatcher',
                                     Gio.BusNameWatcherFlags.NONE,
                                     lambda c, n, o: register(c), None)


def export(conn):
    conn.register_object('/StatusNotifierItem', node.interfaces[0], call, get_prop, None)
    return False


def noise(conn):
    conn.emit_signal(None, '/StatusNotifierItem', 'org.kde.StatusNotifierItem',
                     'NewIcon', None)
    return True


def set_label(conn):
    PROPS['XAyatanaLabel'] = GLib.Variant('s', '42 °C')
    PROPS['XAyatanaLabelGuide'] = GLib.Variant('s', '100 °C')
    conn.emit_signal(None, '/StatusNotifierItem', 'org.kde.StatusNotifierItem',
                     'XAyatanaNewLabel', GLib.Variant('(ss)', ('42 °C', '100 °C')))
    log.write('LABEL\n')
    return False


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
if mode == 'tarde':
    GLib.timeout_add(1200, export, bus)
else:
    export(bus)
if mode == 'reregistra':
    bus.signal_subscribe('org.kde.StatusNotifierWatcher', 'org.kde.StatusNotifierWatcher',
                         'StatusNotifierItemRegistered', '/StatusNotifierWatcher', None,
                         Gio.DBusSignalFlags.NONE, lambda *a: register(bus))
if mode == 'ruidosa':
    GLib.timeout_add(3, noise, bus)
if mode == 'etiqueta':
    GLib.timeout_add(4000, set_label, bus)
Gio.bus_own_name_on_connection(bus, name, Gio.BusNameOwnerFlags.NONE, acquired, None)
GLib.MainLoop().run()
