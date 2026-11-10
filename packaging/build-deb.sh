#!/usr/bin/env bash
# build-deb.sh — Genera un paquete .deb de Tray Overflow para Debian/Ubuntu/Mint.
#
# Uso: bash packaging/build-deb.sh
# Resultado: xfce4-trayoverflow-plugin_<versión>_<arquitectura>_<distro>.deb
# en la raíz (p. ej. ..._<versión>_amd64_debian12.deb). Las dependencias se
# toman de la distro donde se arma: usá el .deb de tu distro.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

NAME=xfce4-trayoverflow-plugin
VERSION=$(sed -n "s/^  version: '\(.*\)',$/\1/p" meson.build | head -1)
ARCH=$(dpkg --print-architecture)
# Distro donde se arma (debian12, debian13, ubuntu24.04...): va en el
# nombre del archivo para no confundir paquetes de distintas distros
DISTRO=$(. /etc/os-release && echo "${ID:-linux}${VERSION_ID:-}")
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
Copyright: 2026 Juan Sebastian Perez <juanperez33840@gmail.com>
License: GPL-2+
 On Debian systems, the full text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
EOF

SIZE=$(du -sk "$STAGE/usr" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $NAME
Version: $VERSION
Architecture: $ARCH
Maintainer: Tantin1 <juanperez33840@gmail.com>
Installed-Size: $SIZE
Depends: $DEPENDS_LINE
Recommends: xfce4-panel
Section: xfce
Priority: optional
Description: Status notifier tray with a popup for hidden icons
 Xfce panel plugin displaying StatusNotifierItem (AppIndicator) icons
 and allowing less-used icons to be hidden in a popup, similar to
 the notification area in Windows 10/11.
 .
 Supports dragging icons between the panel and popup, reordering,
 and independent icon size and spacing preferences.
 .
 An optional XEmbed proxy also displays classic X11 tray applications.
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

OUT="$ROOT/${NAME}_${VERSION}_${ARCH}_${DISTRO}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo "==> Paquete: $OUT"
echo "    Depends: $DEPENDS_LINE"
echo "    Instalar: sudo apt install ./$(basename "$OUT")"
