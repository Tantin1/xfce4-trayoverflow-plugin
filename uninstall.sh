#!/usr/bin/env bash
# uninstall.sh — Desinstala Tray Overflow (instalado con install.sh)
set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f build/meson-logs/install-log.txt ]; then
  echo "No encuentro build/meson-logs/install-log.txt: ¿se instaló con install.sh?" >&2
  exit 1
fi

echo "==> Quitando archivos instalados"
sudo ninja -C build uninstall

if pgrep -x xfce4-panel >/dev/null; then
  xfce4-panel -r
fi

cat <<'EOF'

Desinstalado. La configuración guardada queda en xfconf (canal xfce4-panel)
y la caché de íconos en ~/.cache/xfce4/trayoverflow; podés borrarla con:
  rm -rf ~/.cache/xfce4/trayoverflow
EOF
