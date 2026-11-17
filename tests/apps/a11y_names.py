#!/usr/bin/env python3
# Lista los botones que un lector de pantalla ve en el panel (AT-SPI):
# una línea por botón, "rol<TAB>nombre". Uso: a11y_names.py
# Sale con 77 si no hay AT-SPI (la prueba se saltea).
import sys

try:
    import gi
    gi.require_version('Atspi', '2.0')
    from gi.repository import Atspi
except (ImportError, ValueError):
    sys.exit(77)

ROLES = {Atspi.Role.PUSH_BUTTON: 'boton', Atspi.Role.TOGGLE_BUTTON: 'interruptor'}


def walk(node, depth=0):
    if node is None or depth > 40:
        return
    try:
        role = node.get_role()
        if role in ROLES:
            print('%s\t%s' % (ROLES[role], node.get_name()))
        for i in range(node.get_child_count()):
            walk(node.get_child_at_index(i), depth + 1)
    except Exception:
        pass


desktop = Atspi.get_desktop(0)
if desktop is None or desktop.get_child_count() == 0:
    sys.exit(77)   # no hay registro de AT-SPI: nadie puede aparecer
found = False
for i in range(desktop.get_child_count()):
    app = desktop.get_child_at_index(i)
    if app is not None and 'panel' in (app.get_name() or '').lower():
        found = True
        walk(app)
sys.exit(0 if found else 3)
