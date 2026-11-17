#!/usr/bin/env bash
# con-dbus.sh — Corre una prueba con un D-Bus y una configuración propios
# (carpeta temporal): no toca la configuración ni el panel del usuario.
# Si falta dbus-run-session, la prueba se saltea (código 77).
set -u

command -v dbus-run-session >/dev/null 2>&1 || { echo "falta dbus-run-session"; exit 77; }

work=$(mktemp -d "${TMPDIR:-/tmp}/trayoverflow-unit.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/config" "$work/cache" "$work/data"

# Las variables van antes de dbus-run-session para que xfconfd (activado
# por D-Bus) use la configuración temporal
env -u WAYLAND_DISPLAY -u DISPLAY -u SESSION_MANAGER \
    XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache" \
    XDG_DATA_HOME="$work/data" TRAYOVERFLOW_TEST_DIR="$work" \
    timeout 60 dbus-run-session -- "$@"
