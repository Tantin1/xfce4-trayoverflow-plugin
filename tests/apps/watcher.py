#!/usr/bin/env python3
# Watcher StatusNotifier mínimo, para probar el proxy sin el panel.
# Anota en el archivo $1 cada ítem que se registra: "ITEM <emisor> <servicio>".
import sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

log = open(sys.argv[1], 'a', buffering=1)
XML = '''<node><interface name="org.kde.StatusNotifierWatcher">
<method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
<method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
<property name="RegisteredStatusNotifierItems" type="as" access="read"/>
<property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
<property name="ProtocolVersion" type="i" access="read"/>
</interface></node>'''
node = Gio.DBusNodeInfo.new_for_xml(XML)


def call(conn, sender, path, iface, method, args, inv):
    if method == 'RegisterStatusNotifierItem':
        log.write('ITEM %s %s\n' % (sender, args.unpack()[0]))
    inv.return_value(None)


def get_prop(conn, sender, path, iface, prop):
    return {'IsStatusNotifierHostRegistered': GLib.Variant('b', True),
            'ProtocolVersion': GLib.Variant('i', 0),
            'RegisteredStatusNotifierItems': GLib.Variant('as', [])}[prop]


def acquired(conn, name):
    conn.register_object('/StatusNotifierWatcher', node.interfaces[0], call, get_prop, None)


Gio.bus_own_name(Gio.BusType.SESSION, 'org.kde.StatusNotifierWatcher',
                 Gio.BusNameOwnerFlags.NONE, acquired, None, None)
GLib.MainLoop().run()
