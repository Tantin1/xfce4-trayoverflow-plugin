#!/usr/bin/env python3
# App moderna de prueba (StatusNotifierItem, sin menú D-Bus).
# Uso: sni_app.py ARCHIVO_LOG ID
import os, sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

log = open(sys.argv[1], 'a', buffering=1)
app_id = sys.argv[2] if len(sys.argv) > 2 else 'sni-app'

XML = '''<node><interface name="org.kde.StatusNotifierItem">
<property name="Category" type="s" access="read"/>
<property name="Id" type="s" access="read"/>
<property name="Title" type="s" access="read"/>
<property name="Status" type="s" access="read"/>
<property name="IconName" type="s" access="read"/>
<property name="ItemIsMenu" type="b" access="read"/>
<method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
</interface></node>'''

PROPS = {'Category': GLib.Variant('s', 'ApplicationStatus'),
         'Id': GLib.Variant('s', app_id),
         'Title': GLib.Variant('s', app_id),
         'Status': GLib.Variant('s', 'Active'),
         'IconName': GLib.Variant('s', 'dialog-information'),
         'ItemIsMenu': GLib.Variant('b', False)}

node = Gio.DBusNodeInfo.new_for_xml(XML)
name = 'org.kde.StatusNotifierItem-%d-1' % os.getpid()


def call(conn, sender, path, iface, method, args, inv):
    log.write('%s %s\n' % (method.upper(), ' '.join(str(a) for a in args.unpack())))
    inv.return_value(None)


def get_prop(conn, sender, path, iface, prop):
    return PROPS.get(prop)


def register(conn):
    conn.call('org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher',
              'org.kde.StatusNotifierWatcher', 'RegisterStatusNotifierItem',
              GLib.Variant('(s)', (name,)), None, Gio.DBusCallFlags.NONE, -1, None, None)


def acquired(conn, n):
    register(conn)
    # Volver a registrarse si el watcher aparece después (o se reinicia)
    Gio.bus_watch_name_on_connection(conn, 'org.kde.StatusNotifierWatcher',
                                     Gio.BusNameWatcherFlags.NONE,
                                     lambda c, n, o: register(c), None)


bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.register_object('/StatusNotifierItem', node.interfaces[0], call, get_prop, None)
Gio.bus_own_name_on_connection(bus, name, Gio.BusNameOwnerFlags.NONE, acquired, None)
GLib.MainLoop().run()
