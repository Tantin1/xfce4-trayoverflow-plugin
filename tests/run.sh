#!/usr/bin/env bash
# run.sh — Pruebas automáticas de Tray Overflow (sesión X11 aislada).
#
# Uso:  bash tests/run.sh              (todas las pruebas)
#       bash tests/run.sh popup_rueda  (solo las nombradas)
#       bash tests/run.sh --lista      (ver los nombres)
#
# Prueba el plugin INSTALADO (el que deja install.sh). Cada prueba corre en
# su propia pantalla virtual (Xvfb), con su propio D-Bus y su propia
# configuración en una carpeta temporal: no toca tu panel ni tu
# configuración. Wayland no se prueba acá.
#
# Necesita: Xvfb, xfwm4, xfce4-panel, xdotool, ImageMagick y PyGObject.
#   Arch:   sudo pacman -S xorg-server-xvfb xdotool imagemagick python-gobject
#   Debian: sudo apt install xvfb xdotool imagemagick python3-gi gir1.2-gtk-3.0
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
APPS="$HERE/apps"

# Tamaño de la pantalla virtual y del panel (abajo, de lado a lado)
SCREEN_W=1024
SCREEN_H=600
PANEL_SIZE=30
PANEL_Y=$((SCREEN_H - PANEL_SIZE / 2))   # centro vertical del panel

TESTS=(proxy_rueda proxy_wayland_rueda proxy_baja
       panel_app_moderna popup_cierre popup_rueda popup_menu_app_antigua
       arrastre_ocultar arrastre_mostrar atajo teclado apps_nuevas
       iconos_simbolicos)


# ════════════════════════════════════════════════════════════════════════
# Utilidades
# ════════════════════════════════════════════════════════════════════════

say()  { printf '%s\n' "$*"; }
fail() { say "    FALLA: $*"; FAILED=1; }
ok()   { say "    ok: $*"; }

find_python() {
  local p
  for p in python3 python3.13 python3.12 python3.11 python; do
    if command -v "$p" >/dev/null 2>&1 \
       && "$p" -c 'import gi; gi.require_version("Gtk", "3.0"); from gi.repository import Gtk' \
          >/dev/null 2>&1; then
      echo "$p"
      return 0
    fi
  done
  return 1
}

find_proxy() {
  local path=""
  [ -n "${TRAYOVERFLOW_TEST_PROXY:-}" ] && { echo "$TRAYOVERFLOW_TEST_PROXY"; return; }
  [ -f "$ROOT/build/config.h" ] \
    && path=$(sed -n 's/^#define PROXY_PATH "\(.*\)"$/\1/p' "$ROOT/build/config.h")
  [ -x "$path" ] || path=/usr/libexec/xfce4-trayoverflow-plugin/xembed-proxy
  echo "$path"
}

check_requirements() {
  local missing=() cmd
  for cmd in Xvfb xfwm4 xfce4-panel xfconf-query xdotool import convert gdbus dbus-run-session; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
  done
  PYTHON=$(find_python) || missing+=("PyGObject con GTK 3")
  if [ ${#missing[@]} -gt 0 ]; then
    say "Faltan: ${missing[*]}"
    say "Ver la cabecera de tests/run.sh para instalarlos."
    exit 2
  fi
  PROXY=$(find_proxy)
  export PYTHON PROXY
}

# Espera hasta $2 segundos a que el comando $1 tenga éxito
wait_for() {
  local i
  for i in $(seq $(( $2 * 10 ))); do
    eval "$1" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

count() { grep -c -- "$1" "$2" 2>/dev/null || true; }

# Arrastre "humano": GTK necesita movimientos graduales para empezarlo
drag() {   # x1 y1 x2 y2
  local i
  xdotool mousemove "$1" "$2" sleep 0.2 mousedown 1 sleep 0.2
  # Primero unos píxeles, para pasar el umbral de arrastre de GTK
  for i in 3 6 9 12; do
    xdotool mousemove $(( $1 + i )) $(( $2 - i )); sleep 0.05
  done
  for i in $(seq 1 14); do
    xdotool mousemove $(( $1 + ($3 - $1) * i / 14 )) $(( $2 + ($4 - $2) * i / 14 ))
    sleep 0.05
  done
  sleep 0.3
  xdotool mouseup 1
}

# ¿La app (clave $1) está entre las ocultas? (lo guardado en xfconf)
is_hidden() {
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/hidden-items 2>/dev/null | grep -qx "$1"
}

click() {   # x y botón
  xdotool mousemove "$1" "$2" sleep 0.2 mousedown "$3" sleep 0.05 mouseup "$3"
}

shot() { import -window root "$WORK/s.png"; }

# Rectángulo de lo que no es fondo (negro) por encima del panel: "w h x y"
above_panel_rect() {
  shot
  convert "$WORK/s.png" -crop "${SCREEN_W}x$((SCREEN_H - PANEL_SIZE - 3))+0+0" +repage \
          -trim -format '%w %h %X %Y' info: 2>/dev/null | tr -d '+'
}

# Color de un píxel de la última captura
pixel() { convert "$WORK/s.png" -format "%[pixel:p{$1,$2}]" info: 2>/dev/null; }

# ¿El popup está abierto? Mira un píxel de su borde inferior izquierdo
# (fuera de donde salen los menús de las apps)
popup_is_open() {
  shot
  [ "$(pixel "$POPUP_PX" "$POPUP_PY")" != "srgb(0,0,0)" ]
}

# Abre el popup con un clic en la flecha y toma su posición
open_popup() {
  local w h x y
  click "$ARROW_X" "$PANEL_Y" 1
  sleep 1
  read -r w h x y <<<"$(above_panel_rect)"
  if [ "${w:-0}" -lt 10 ] || [ "${h:-0}" -lt 10 ]; then
    return 1
  fi
  POPUP_PX=$((x + 3)); POPUP_PY=$((y + h - 4))
  POPUP_CX=$((x + w / 2)); POPUP_CY=$((y + h / 2))
  return 0
}


# ════════════════════════════════════════════════════════════════════════
# Sesión aislada (se ejecuta dentro de dbus-run-session)
# ════════════════════════════════════════════════════════════════════════

PIDS=()

start_x() {
  local n
  for n in $(seq 91 120); do
    [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ] || break
  done
  Xvfb ":$n" -screen 0 "${SCREEN_W}x${SCREEN_H}x24" -nolisten tcp >/dev/null 2>&1 &
  PIDS+=($!)
  export DISPLAY=":$n"
  wait_for 'xdotool getdisplaygeometry' 5 || { say "    Xvfb no arrancó"; exit 1; }
}

start_app() {   # nombre-de-script args...
  local script=$1; shift
  "$PYTHON" "$APPS/$script" "$@" >>"$WORK/apps.err" 2>&1 &
  PIDS+=($!)
  LAST_PID=$!
}

# Panel con una sola instancia del plugin; $1 = apps ocultas (claves)
start_panel() {
  local q="xfconf-query -c xfce4-panel"
  $q -p /panels -n -a -t int -s 1
  $q -p /panels/panel-1/position -n -t string -s "p=8;x=$((SCREEN_W / 2));y=$PANEL_Y"
  $q -p /panels/panel-1/size -n -t uint -s "$PANEL_SIZE"
  $q -p /panels/panel-1/length -n -t uint -s 100
  $q -p /panels/panel-1/position-locked -n -t bool -s true
  $q -p /panels/panel-1/plugin-ids -n -a -t int -s 1
  $q -p /plugins/plugin-1 -n -t string -s trayoverflow
  $q -p /plugins/plugin-1/legacy-tray -n -t bool -s true
  $q -p /plugins/plugin-1/arrow-position -n -t int -s 0
  if [ -n "${1:-}" ]; then
    local key args=()
    for key in $1; do args+=(-t string -s "$key"); done
    $q -p /plugins/plugin-1/hidden-items -n -a "${args[@]}"
  fi

  xfwm4 --compositor=off >/dev/null 2>&1 &
  PIDS+=($!)
  sleep 1
  TRAYOVERFLOW_PROXY_DEBUG=1 xfce4-panel >"$WORK/panel.log" 2>&1 &
  PANEL_PID=$!
  PIDS+=($PANEL_PID)
  sleep 4
  ARROW_X=10
}

cleanup() {
  local pid
  # Solo nuestros procesos, por PID: nunca por nombre (mataría tu panel)
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
  wait 2>/dev/null
}


# ════════════════════════════════════════════════════════════════════════
# Pruebas
# ════════════════════════════════════════════════════════════════════════

# Proxy solo (sin panel): giros encolados y Scroll(n) = n pasos
t_proxy_rueda() {
  local item svc i pids=()
  start_app watcher.py "$WORK/watcher.log"
  sleep 0.5
  "$PROXY" --verbose >"$WORK/proxy.log" 2>&1 & PIDS+=($!)
  sleep 0.5
  start_app legacy_app.py "$WORK/app.log"
  wait_for "grep -q ITEM '$WORK/watcher.log'" 10 || { fail "el proxy no publicó la app"; return; }
  sleep 1
  svc=$(awk '/ITEM/{print $2; exit}' "$WORK/watcher.log")
  item="--session --dest $svc --object-path /StatusNotifierItem --method org.kde.StatusNotifierItem"
  gdbus call $item.Scroll 3 vertical >/dev/null
  sleep 0.5
  for i in 1 2 3 4 5; do gdbus call $item.Scroll -- -1 vertical >/dev/null & pids+=($!); done
  wait "${pids[@]}"
  sleep 1
  [ "$(count 'SCROLL down' "$WORK/app.log")" = 3 ] && ok "Scroll(3) = 3 pasos" \
    || fail "Scroll(3): $(count 'SCROLL down' "$WORK/app.log") pasos (esperado 3)"
  [ "$(count 'SCROLL up' "$WORK/app.log")" = 5 ] && ok "5 giros rápidos = 5 pasos" \
    || fail "5 giros rápidos: $(count 'SCROLL up' "$WORK/app.log") pasos (esperado 5)"
}

# Proxy en modo Wayland: la rueda usa la posición de SetPointer
t_proxy_wayland_rueda() {
  local item svc
  start_app watcher.py "$WORK/watcher.log"
  sleep 0.5
  "$PROXY" --verbose --wayland >"$WORK/proxy.log" 2>&1 & PIDS+=($!)
  sleep 0.5
  start_app legacy_app.py "$WORK/app.log"
  wait_for "grep -q ITEM '$WORK/watcher.log'" 10 || { fail "el proxy no publicó la app"; return; }
  sleep 1
  svc=$(awk '/ITEM/{print $2; exit}' "$WORK/watcher.log")
  item="--session --dest $svc --object-path /StatusNotifierItem"
  gdbus call $item --method org.xfce.TrayOverflow.XEmbedItem.SetPointer \
    $((SCREEN_W / 2)) $((SCREEN_H - 20)) >/dev/null
  gdbus call $item --method org.kde.StatusNotifierItem.Scroll 2 vertical >/dev/null
  sleep 1.5
  [ "$(count 'SCROLL down' "$WORK/app.log")" = 2 ] && ok "rueda sin clic previo" \
    || fail "rueda sin clic previo: $(count 'SCROLL down' "$WORK/app.log") pasos (esperado 2)"
}

# Proxy: no gasta CPU en reposo y nota enseguida que una app se cerró
t_proxy_baja() {
  local proxy_pid app_pid a b t0 ms
  start_app watcher.py "$WORK/watcher.log"
  sleep 0.5
  "$PROXY" --verbose >"$WORK/proxy.log" 2>&1 & proxy_pid=$!; PIDS+=($proxy_pid)
  sleep 0.5
  start_app legacy_app.py "$WORK/app.log"; app_pid=$LAST_PID
  wait_for "grep -q ITEM '$WORK/watcher.log'" 10 || { fail "el proxy no publicó la app"; return; }
  sleep 1
  a=$(awk '{print $14 + $15}' "/proc/$proxy_pid/stat")
  sleep 3
  b=$(awk '{print $14 + $15}' "/proc/$proxy_pid/stat")
  [ $((b - a)) -le 1 ] && ok "sin CPU en reposo" || fail "usó $((b - a)) ticks de CPU en reposo"
  kill "$app_pid"
  t0=$(date +%s%N)
  if wait_for "grep -q desanclado '$WORK/proxy.log'" 3; then
    ms=$(( ($(date +%s%N) - t0) / 1000000 ))
    [ "$ms" -lt 1000 ] && ok "baja detectada en $ms ms" || fail "baja tardó $ms ms"
  else
    fail "no notó que la app se cerró"
  fi
}

# App moderna: aparece en el panel y un clic la activa
t_panel_app_moderna() {
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna
  sleep 2
  shot
  # Sin apps ocultas no hay flecha: el ícono queda al principio del panel.
  # Probar unas posiciones hasta que la app registre el clic.
  local x
  for x in 15 25 35 45; do
    click "$x" "$PANEL_Y" 1
    sleep 0.5
    grep -q ACTIVATE "$WORK/app.log" && break
  done
  grep -q ACTIVATE "$WORK/app.log" && ok "clic en el panel activa la app" \
    || fail "la app no recibió Activate"
}

# El popup se abre con la flecha y se cierra con Esc, con la flecha y con
# un clic afuera
t_popup_cierre() {
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  sleep 3
  open_popup || { fail "el popup no se abrió"; return; }
  ok "la flecha abre el popup"
  xdotool key Escape; sleep 0.7
  popup_is_open && fail "Esc no cerró el popup" || ok "Esc cierra"
  open_popup || { fail "el popup no se volvió a abrir"; return; }
  click "$ARROW_X" "$PANEL_Y" 1; sleep 0.7
  popup_is_open && fail "la flecha no cerró el popup" || ok "la flecha cierra"
  open_popup || { fail "el popup no se volvió a abrir"; return; }
  click $((SCREEN_W / 2)) $((SCREEN_H / 3)) 1; sleep 0.7
  popup_is_open && fail "un clic afuera no cerró el popup" || ok "un clic afuera cierra"
}

# Rueda sobre una app antigua del popup: llega cada giro y el popup sigue
t_popup_rueda() {
  local i
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  sleep 3
  open_popup || { fail "el popup no se abrió"; return; }
  xdotool mousemove "$POPUP_CX" "$POPUP_CY"; sleep 0.3
  for i in 1 2 3; do xdotool click 5; sleep 0.15; done
  sleep 1
  [ "$(count 'SCROLL down' "$WORK/app.log")" = 3 ] && ok "llegan los 3 giros" \
    || fail "llegaron $(count 'SCROLL down' "$WORK/app.log") giros (esperado 3)"
  popup_is_open && ok "el popup sigue abierto" || fail "el popup se cerró"
}

# Menú propio de una app antigua (X11): el popup queda abierto, recupera
# el puntero al cerrarse el menú y después un clic afuera lo cierra
t_popup_menu_app_antigua() {
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  sleep 3
  open_popup || { fail "el popup no se abrió"; return; }
  xdotool mousemove "$POPUP_CX" "$POPUP_CY"; sleep 0.3
  xdotool mousedown 3 sleep 0.05 mouseup 3; sleep 2
  grep -q MENU-ABIERTO "$WORK/app.log" && ok "se abre el menú de la app" \
    || fail "no se abrió el menú de la app"
  popup_is_open && ok "el popup sigue abierto con el menú" || fail "el menú cerró el popup"
  xdotool key Escape; sleep 1.5
  popup_is_open && ok "sigue abierto tras cerrar el menú" || fail "se cerró al cerrar el menú"
  click $((SCREEN_W / 2)) $((SCREEN_H / 3)) 1; sleep 1
  popup_is_open && fail "un clic afuera no lo cerró (¿no recuperó el puntero?)" \
    || ok "un clic afuera lo cierra"
}


# Arrastrar una app del panel a la flecha la oculta
t_arrastre_ocultar() {
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 3
  # Panel: [flecha][app moderna]
  drag 33 "$PANEL_Y" "$ARROW_X" "$PANEL_Y"
  sleep 3   # el guardado en xfconf se agrupa (hasta 2 s)
  is_hidden prueba-moderna && ok "arrastrar a la flecha la oculta" \
    || fail "arrastrar a la flecha no la ocultó"
}

# Arrastrar una app del popup a un ícono del panel la vuelve a mostrar
t_arrastre_mostrar() {
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 3
  open_popup || { fail "el popup no se abrió"; return; }
  # Soltar sobre la app moderna, que está en el panel junto a la flecha
  drag "$POPUP_CX" "$POPUP_CY" 33 "$PANEL_Y"
  sleep 3
  is_hidden legacy-app && fail "arrastrar al panel no la mostró" \
    || ok "arrastrar del popup al panel la muestra"
}

# Atajo de teclado (evento del panel): abre y cierra el popup
t_atajo() {
  start_panel legacy-app
  start_app legacy_app.py "$WORK/app.log"
  sleep 3
  # Tomar la posición del popup con un clic y cerrarlo
  open_popup || { fail "el popup no se abrió"; return; }
  xdotool key Escape; sleep 0.7
  xfce4-panel --plugin-event=trayoverflow:popup; sleep 1
  popup_is_open && ok "el atajo abre el popup" || fail "el atajo no abrió el popup"
  xfce4-panel --plugin-event=trayoverflow:popup; sleep 1
  popup_is_open && fail "el atajo no cerró el popup" || ok "el atajo lo cierra"
  xfce4-panel --plugin-event=trayoverflow:popup:bool:true; sleep 1
  xfce4-panel --plugin-event=trayoverflow:popup:bool:true; sleep 1
  popup_is_open && ok ":bool:true solo abre" || fail ":bool:true lo cerró"
}

# Teclado dentro del popup: Enter activa el ícono elegido, las flechas
# eligen otro y la tecla de menú abre el menú de la app
t_teclado() {
  start_panel "legacy-app prueba-moderna"
  start_app legacy_app.py "$WORK/app.log"
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 3
  # Primer ícono (el atajo lo elige al abrir) y después el segundo
  xfce4-panel --plugin-event=trayoverflow:popup:bool:true; sleep 1
  xdotool key Return; sleep 1.5
  xfce4-panel --plugin-event=trayoverflow:popup:bool:true; sleep 1
  xdotool key Right; sleep 0.3
  xdotool key Return; sleep 1.5
  if grep -q ACTIVATE "$WORK/app.log" && grep -q ACTIVATE "$WORK/sni.log"; then
    ok "Enter y las flechas activan cada ícono"
  else
    fail "Enter/flechas: antigua=$(count ACTIVATE "$WORK/app.log") moderna=$(count ACTIVATE "$WORK/sni.log") (esperado 1 y 1)"
  fi
  # Tecla de menú sobre la app antigua (su menú propio)
  xfce4-panel --plugin-event=trayoverflow:popup:bool:false; sleep 0.5
  xfce4-panel --plugin-event=trayoverflow:popup:bool:true; sleep 1
  xdotool key Menu; sleep 1.5
  grep -q "CONTEXTMENU" "$WORK/sni.log" || grep -q MENU-ABIERTO "$WORK/app.log" \
    && ok "la tecla de menú abre el menú de la app" \
    || fail "la tecla de menú no abrió ningún menú"
}

# Opción "apps nuevas en el popup": la nueva queda oculta; la que ya era
# conocida y visible no cambia
t_apps_nuevas() {
  local q="xfconf-query -c xfce4-panel -p /plugins/plugin-1"
  $q/new-in-popup -n -t bool -s true
  $q/known-items -n -a -t string -s legacy-app
  $q/visible-items -n -a -t string -s legacy-app
  start_panel
  start_app legacy_app.py "$WORK/app.log"
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 5   # registro de las apps + guardado agrupado (hasta 2 s)
  is_hidden prueba-moderna && ok "la app nueva queda oculta" \
    || fail "la app nueva no quedó oculta"
  is_hidden legacy-app && fail "la app ya conocida se ocultó" \
    || ok "la app ya conocida sigue visible"
}

# Saturación máxima (0 a 1) del cuadrado de 24 px centrado en x,y de la
# última captura: un ícono en color da mucho más que uno monocromo
saturation() {
  convert "$WORK/s.png" -crop "24x24+$(( $1 - 12 ))+$(( $2 - 12 ))" +repage \
          -colorspace HSL -channel G -separate +channel -format '%[fx:maxima]' info:
}

# Íconos simbólicos: la app moderna (ícono del tema) pasa a monocroma al
# activar la opción, sin reiniciar el panel
t_iconos_simbolicos() {
  local before after
  start_panel
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 3
  shot; before=$(saturation 15 "$PANEL_Y")
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/symbolic-icons -n -t bool -s true
  sleep 1.5
  shot; after=$(saturation 15 "$PANEL_Y")
  if awk -v b="$before" -v a="$after" 'BEGIN { exit !(b > 0.3 && a < 0.2) }'; then
    ok "el ícono pasa a monocromo (saturación $before → $after)"
  else
    fail "saturación $before → $after (esperado: color → monocromo)"
  fi
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/symbolic-icons -s false
  sleep 1.5
  shot; after=$(saturation 15 "$PANEL_Y")
  awk -v a="$after" 'BEGIN { exit !(a > 0.3) }' && ok "al desactivarla vuelve el color" \
    || fail "al desactivarla no volvió el color (saturación $after)"
}

# ════════════════════════════════════════════════════════════════════════
# Principal
# ════════════════════════════════════════════════════════════════════════

if [ "${1:-}" = "--interno" ]; then
  # Dentro de la sesión aislada: una sola prueba
  FAILED=0
  WORK=$2
  trap cleanup EXIT
  start_x
  "t_$3"
  exit $FAILED
fi

if [ "${1:-}" = "--lista" ]; then
  printf '%s\n' "${TESTS[@]}"
  exit 0
fi

check_requirements
[ $# -gt 0 ] && TESTS=("$@")

desktop=$(ls /usr/share/xfce4/panel/plugins/trayoverflow.desktop 2>/dev/null)
[ -n "$desktop" ] || { say "No encuentro el plugin instalado: ejecutá install.sh primero."; exit 2; }
say "Proxy: $PROXY"
TOTAL=0; BAD=0
for t in "${TESTS[@]}"; do
  if ! declare -F "t_$t" >/dev/null; then
    say "No existe la prueba '$t' (ver --lista)"; BAD=$((BAD + 1)); continue
  fi
  TOTAL=$((TOTAL + 1))
  say "• $t"
  work=$(mktemp -d "${TMPDIR:-/tmp}/trayoverflow-test.XXXXXX")
  mkdir -p "$work/config" "$work/cache" "$work/data"
  # Todo aislado: configuración, caché y D-Bus propios. Las variables se
  # fijan antes de dbus-run-session para que xfconfd (activado por D-Bus)
  # use la configuración temporal.
  if ! env -u WAYLAND_DISPLAY -u SESSION_MANAGER \
       XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache" \
       XDG_DATA_HOME="$work/data" GDK_BACKEND=x11 GTK_THEME=Adwaita NO_AT_BRIDGE=1 \
       PYTHON="$PYTHON" PROXY="$PROXY" \
       timeout 120 dbus-run-session -- bash "$0" --interno "$work" "$t" 2>/dev/null; then
    BAD=$((BAD + 1))
    say "    (registros en $work)"
  else
    rm -rf "$work"
  fi
done

say ""
if [ $BAD -eq 0 ]; then
  say "Todas las pruebas pasaron ($TOTAL)."
else
  say "$BAD de $TOTAL pruebas fallaron."
fi
[ $BAD -eq 0 ]
