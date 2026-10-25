#!/usr/bin/env bash
# install.sh — Compila e instala Tray Overflow y reinicia el panel.
#
# Uso:  bash install.sh            (instala en /usr, donde el panel busca plugins)
#       bash install.sh --prefix /usr/local   (solo si tu panel busca ahí)
set -euo pipefail

cd "$(dirname "$0")"

PREFIX=/usr
if [ "${1:-}" = "--prefix" ] && [ -n "${2:-}" ]; then
  PREFIX="$2"
fi

echo "==> Configurando (prefijo: $PREFIX)"
if [ -d build ]; then
  meson setup build --reconfigure --prefix="$PREFIX" >/dev/null
else
  meson setup build --prefix="$PREFIX" >/dev/null
fi

echo "==> Compilando"
ninja -C build

echo "==> Instalando (pide contraseña)"
sudo ninja -C build install

echo "==> Reiniciando el panel"
if pgrep -x xfce4-panel >/dev/null; then
  xfce4-panel -r
fi

cat <<'EOF'

Listo. Para agregarlo:
  clic derecho en el panel → Panel → Agregar nuevos elementos → "Bandeja con desborde"

Si no aparece en la lista, ver la sección "Diagnóstico" del README.
EOF
