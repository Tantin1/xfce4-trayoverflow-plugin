#!/usr/bin/env bash
# build-deb.sh — Genera un paquete .deb de Tray Overflow para Debian/Ubuntu/Mint.
#
# Uso: bash packaging/build-deb.sh
# Resultado: xfce4-trayoverflow-plugin_<versión>_<arquitectura>.deb en la raíz.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

NAME=xfce4-trayoverflow-plugin
VERSION=$(sed -n "s/^  version: '\(.*\)',$/\1/p" meson.build | head -1)
ARCH=$(dpkg --print-architecture)
BUILD="$ROOT/build-deb"
STAGE="$BUILD/stage"

echo "==> Compilando $NAME $VERSION ($ARCH)"
rm -rf "$BUILD"
meson setup "$BUILD" --prefix=/usr -Dbuildtype=release -Db_ndebug=true >/dev/null
ninja -C "$BUILD"
DESTDIR="$STAGE" ninja -C "$BUILD" install >/dev/null

SO=$(find "$STAGE" -name 'libtrayoverflow.so' | head -1)
PROXY=$(find "$STAGE" -name 'xembed-proxy' -type f | head -1)
BINARIES=("$SO")
[ -n "$PROXY" ] && BINARIES+=("$PROXY")
strip --strip-unneeded "${BINARIES[@]}"

# ── Dependencias: el paquete dueño de cada biblioteca enlazada ───────────
echo "==> Calculando dependencias"
declare -A seen=()
DEPENDS=()
for lib in $(objdump -p "${BINARIES[@]}" | awk '/NEEDED/{print $2}' | sort -u); do
  path=$(ldconfig -p | awk -v l="$lib" '$1 == l {print $NF; exit}')
  [ -z "$path" ] && continue
  pkg=$(dpkg -S "$(readlink -f "$path")" 2>/dev/null | head -1 | cut -d: -f1) || true
  [ -z "$pkg" ] && pkg=$(dpkg -S "$path" 2>/dev/null | head -1 | cut -d: -f1) || true
  [ -z "$pkg" ] || [ -n "${seen[$pkg]:-}" ] && continue
  seen[$pkg]=1
  ver=$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null | sed 's/-[^-]*$//')
  case "$pkg" in
    libc6) DEPENDS+=("$pkg") ;;
    *)     DEPENDS+=("$pkg (>= ${ver%%+*})") ;;
  esac
done
DEPENDS_LINE=$(IFS=,; echo "${DEPENDS[*]}" | sed 's/,/, /g')

mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/share/doc/$NAME"
cp README.md "$STAGE/usr/share/doc/$NAME/"
cat > "$STAGE/usr/share/doc/$NAME/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: $NAME
Files: *
Copyright: 2026 Juan Pérez
License: GPL-2+
 On Debian systems, the full text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
EOF

SIZE=$(du -sk "$STAGE/usr" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $NAME
Version: $VERSION
Architecture: $ARCH
Maintainer: Juan Pérez <juanchiperez97@gmail.com>
Installed-Size: $SIZE
Depends: $DEPENDS_LINE
Recommends: xfce4-panel
Section: xfce
Priority: optional
Description: Bandeja de estado con ventana emergente para íconos ocultos
 Plugin para xfce4-panel que muestra los íconos StatusNotifierItem
 (AppIndicator) y permite ocultar los que no se usan a diario en una
 ventana emergente, como el área de notificación de Windows 10/11.
 .
 Permite arrastrar íconos entre el panel y la ventana emergente,
 reordenarlos y ajustar el tamaño y la separación de los íconos.
 .
 Incluye un proxy opcional que muestra también las aplicaciones antiguas
 que usan la bandeja clásica de X11 (XEmbed).
EOF

# Reiniciar el panel del usuario no es tarea del paquete; solo avisar
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
  echo "Tray Overflow instalado. Reiniciá el panel con: xfce4-panel -r"
fi
exit 0
EOF
chmod 0755 "$STAGE/DEBIAN/postinst"

OUT="$ROOT/${NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo "==> Paquete: $OUT"
echo "    Depends: $DEPENDS_LINE"
echo "    Instalar: sudo apt install ./$(basename "$OUT")"
