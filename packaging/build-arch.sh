#!/usr/bin/env bash
# build-arch.sh — Genera un paquete de Tray Overflow para Arch Linux y
# derivadas (Manjaro, CachyOS, EndeavourOS).
#
# Uso (como usuario normal, no root):  bash packaging/build-arch.sh
# Resultado: xfce4-trayoverflow-plugin-<versión>-1-<arquitectura>.pkg.tar.zst
# en la raíz del proyecto. Se instala con:
#   sudo pacman -U ./xfce4-trayoverflow-plugin-*.pkg.tar.zst
#
# Arma el paquete con makepkg a partir de esta carpeta (no descarga nada).
# El PKGBUILD se genera en build-arch/ con la versión de meson.build.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

NAME=xfce4-trayoverflow-plugin
VERSION=$(sed -n "s/^  version: '\(.*\)',$/\1/p" meson.build | head -1)
BUILD="$ROOT/build-arch"

if [ "$(id -u)" = 0 ]; then
  echo "makepkg no se puede usar como root: corré este script como usuario normal."
  exit 1
fi
if ! command -v makepkg >/dev/null; then
  echo "Falta makepkg: este script es para Arch y derivadas (sudo pacman -S base-devel)."
  exit 1
fi

echo "==> Armando $NAME $VERSION para Arch"
rm -rf "$BUILD"
mkdir -p "$BUILD"

cat > "$BUILD/PKGBUILD" <<EOF
# Generado por packaging/build-arch.sh: no editar a mano
pkgname=$NAME
pkgver=$VERSION
pkgrel=1
pkgdesc='Status tray for xfce4-panel with a popup for hidden icons (like Windows 10/11)'
arch=('x86_64' 'aarch64')
license=('GPL-2.0-or-later')
depends=('glib2' 'gtk3' 'xfce4-panel' 'libxfce4ui' 'libxfce4util' 'xfconf'
         'libdbusmenu-gtk3' 'gtk-layer-shell'
         'libx11' 'libxcomposite' 'libxdamage' 'libxfixes' 'libxtst' 'libxext')
makedepends=('meson' 'gettext')
options=('!debug')
source=()

_src='$ROOT'

build() {
  arch-meson "\$_src" build
  meson compile -C build
}

package() {
  meson install -C build --destdir "\$pkgdir"
  install -Dm644 "\$_src/README.md" "\$pkgdir/usr/share/doc/\$pkgname/README.md"
}
EOF

cd "$BUILD"
# -f: rehacer aunque exista; -c: limpiar al terminar; -s: instalar lo que
# falte para compilar (pide la contraseña si hace falta)
makepkg -fcs --noconfirm

mv -f "$BUILD"/"$NAME"-*.pkg.tar.* "$ROOT/"
PKG=$(ls -t "$ROOT"/"$NAME"-*.pkg.tar.* | head -1)

echo
echo "==> Listo: $(basename "$PKG")"
echo "    Instalalo con:  sudo pacman -U ./$(basename "$PKG")"
echo "    y reiniciá el panel:  xfce4-panel -r"
