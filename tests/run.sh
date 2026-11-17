#!/usr/bin/env bash
# run.sh — Pruebas automáticas de Tray Overflow (sesión X11 aislada).
#
# Uso:  bash tests/run.sh              (todas las pruebas)
#       bash tests/run.sh popup_rueda  (solo las nombradas)
#       bash tests/run.sh --lista      (ver los nombres)
#       TRAYOVERFLOW_TEST_KEEP=1 bash tests/run.sh …   (no borrar las
#                                       carpetas con registros y capturas)
#
# Prueba el plugin INSTALADO (el que deja install.sh). Cada prueba corre en
# su propia pantalla virtual (Xvfb), con su propio D-Bus y su propia
# configuración en una carpeta temporal: no toca tu panel ni tu
# configuración. Wayland no se prueba acá.
#
# Necesita: Xvfb, xfwm4, xfce4-panel, xdotool, xwininfo, ImageMagick y
# PyGObject.
#   Arch:   sudo pacman -S xorg-server-xvfb xdotool xorg-xwininfo imagemagick python-gobject
#   Debian: sudo apt install xvfb xdotool x11-utils imagemagick python3-gi gir1.2-gtk-3.0
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
       iconos_simbolicos un_menu_por_clic un_menu_tras_reiniciar
       un_menu_en_popup un_menu_al_reiniciar_app
       registro_repetido registro_temprano avisos_vacios guardado_minimo
       texto_junto_al_icono nombres_accesibles icono_sin_alfa icono_con_forma
       proxy_wayland_wine proxy_wayland_menu)


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
  for cmd in Xvfb xfwm4 xfce4-panel xfconf-query xdotool xwininfo import convert gdbus dbus-run-session; do
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

# Tamaño (AxB) de la ventana de una app antigua, por su título
app_window_size() {
  xwininfo -name "$1" 2>/dev/null | awk '/Width:/{w=$2} /Height:/{h=$2} END{if (w) print w "x" h}'
}

# Reiniciar el panel (las apps siguen corriendo y se vuelven a anclar)
restart_panel() {
  kill "$PANEL_PID" 2>/dev/null
  sleep 2
  TRAYOVERFLOW_PROXY_DEBUG=1 xfce4-panel >>"$WORK/panel.log" 2>&1 &
  PANEL_PID=$!
  PIDS+=($PANEL_PID)
  sleep 5
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

# Proxy en modo Wayland con una app de Wine (0068): antes ningún clic le
# llegaba. Necesita wine y x86_64-w64-mingw32-gcc; sin ellos, se saltea
t_proxy_wayland_wine() {
  local exe="$WORK/wine_tray.exe" item svc i
  if ! command -v wine >/dev/null 2>&1 \
     || ! x86_64-w64-mingw32-gcc "$APPS/wine_tray.c" -o "$exe" -mwindows 2>>"$WORK/apps.err"; then
    ok "sin wine o sin mingw: prueba salteada"
    return
  fi
  export WINEPREFIX="$WORK/wine" WINEDEBUG=-all
  wineboot -i >>"$WORK/apps.err" 2>&1
  start_app watcher.py "$WORK/watcher.log"
  sleep 0.5
  "$PROXY" --verbose --wayland >"$WORK/proxy.log" 2>&1 & PIDS+=($!)
  sleep 0.5
  wine "$exe" "Z:$(printf '%s' "$WORK/app.log" | tr / '\\')" >>"$WORK/apps.err" 2>&1 & PIDS+=($!)
  wait_for "grep -q ITEM '$WORK/watcher.log'" 40 || { fail "el proxy no publicó la app de Wine"; wineserver -k; return; }
  sleep 2
  svc=$(awk '/ITEM/{print $2; exit}' "$WORK/watcher.log")
  item="--session --dest $svc --object-path /StatusNotifierItem"
  gdbus call $item --method org.xfce.TrayOverflow.XEmbedItem.SetPointer \
    $((SCREEN_W / 2)) $((SCREEN_H - 30)) >/dev/null
  gdbus call $item --method org.kde.StatusNotifierItem.Activate \
    $((SCREEN_W / 2)) $((SCREEN_H - 30)) >/dev/null
  sleep 1.5
  gdbus call $item --method org.kde.StatusNotifierItem.ContextMenu \
    $((SCREEN_W / 2)) $((SCREEN_H - 30)) >/dev/null
  sleep 1.5
  grep -q LUP "$WORK/app.log" && ok "clic izquierdo a la app de Wine" \
    || fail "la app de Wine no recibió el clic izquierdo"
  grep -q RUP "$WORK/app.log" && ok "clic derecho a la app de Wine" \
    || fail "la app de Wine no recibió el clic derecho"
  # El clic derecho abrió su menú (0069). El puntero, fuera de las
  # ventanas X: sin usarlo, se cierra solo a los 5 s
  xdotool mousemove 5 5
  wait_for "grep -q MENU-CERRADO '$WORK/app.log'" 8 \
    && ok "el menú de Wine sin usar se cierra solo" \
    || fail "el menú de Wine quedó abierto"
  wineserver -k 2>/dev/null
}

# Proxy en modo Wayland: menús de apps antiguas (0069). En Wayland un
# menú X no ve los clics fuera de X; el proxy lo cierra si el puntero no
# está en él (Xvfb hace de XWayland: con el puntero fuera de las ventanas
# X, X no informa ninguna ventana debajo, igual que con Wayland)
legacy_menu_window() {   # la ventana visible de la app (el menú)
  local w
  for w in $(xwininfo -root -children 2>/dev/null \
               | awk '/legacy-app/ && !/1x1/ && !/-512/ {print $1}'); do
    xwininfo -id "$w" 2>/dev/null | grep -q IsViewable && { echo "$w"; return; }
  done
}

t_proxy_wayland_menu() {
  local item svc menu mx my n
  start_app watcher.py "$WORK/watcher.log"
  sleep 0.5
  "$PROXY" --verbose --wayland >"$WORK/proxy.log" 2>&1 & PIDS+=($!)
  sleep 0.5
  start_app legacy_app.py "$WORK/app.log"
  wait_for "grep -q ITEM '$WORK/watcher.log'" 10 || { fail "el proxy no publicó la app"; return; }
  sleep 1
  svc=$(awk '/ITEM/{print $2; exit}' "$WORK/watcher.log")
  item="--session --dest $svc --object-path /StatusNotifierItem"
  open_menu() {
    gdbus call $item --method org.xfce.TrayOverflow.XEmbedItem.SetPointer \
      $((SCREEN_W / 2)) $((SCREEN_H - 30)) >/dev/null
    gdbus call $item --method org.kde.StatusNotifierItem.ContextMenu \
      $((SCREEN_W / 2)) $((SCREEN_H - 30)) >/dev/null
    sleep 1
    xdotool mousemove 5 5
  }

  # 1. Sin usar: se cierra a los 5 s
  open_menu
  [ "$(count MENU-ABIERTO "$WORK/app.log")" = 1 ] || { fail "no se abrió el menú"; return; }
  sleep 2.5
  [ "$(count MENU-CERRADO "$WORK/app.log")" = 0 ] && ok "no se cierra enseguida" \
    || fail "el menú se cerró antes de tiempo"
  wait_for "[ \$(grep -c MENU-CERRADO '$WORK/app.log') = 1 ]" 5 \
    && ok "el menú sin usar se cierra solo" || fail "el menú sin usar quedó abierto"

  # 2. Con el puntero encima no se cierra; al salir, sí
  open_menu
  menu=$(legacy_menu_window)
  [ -n "$menu" ] || { fail "no encuentro la ventana del menú"; return; }
  read -r mx my < <(xwininfo -id "$menu" | awk '/Absolute upper-left X/{x=$4} /Absolute upper-left Y/{y=$4} END{print x, y}')
  xdotool mousemove $((mx + 10)) $((my + 10))
  sleep 7
  n=$(count MENU-CERRADO "$WORK/app.log")
  [ "$n" = 1 ] && ok "con el puntero encima sigue abierto" \
    || fail "se cerró con el puntero encima ($n cierres)"
  xdotool mousemove 5 5
  wait_for "[ \$(grep -c MENU-CERRADO '$WORK/app.log') = 2 ]" 3 \
    && ok "al salir el puntero se cierra" || fail "al salir el puntero quedó abierto"

  # 3. Elegir una opción sigue andando
  open_menu
  menu=$(legacy_menu_window)
  read -r mx my < <(xwininfo -id "$menu" | awk '/Absolute upper-left X/{x=$4} /Absolute upper-left Y/{y=$4} END{print x, y}')
  xdotool mousemove $((mx + 15)) $((my + 12)) sleep 0.5 click 1
  wait_for "grep -q ELEGIDA '$WORK/app.log'" 3 && ok "elegir una opción" \
    || fail "no se pudo elegir la opción"
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
  # El permiso de activación es solo para Wayland
  grep -q PROVIDEXDG "$WORK/app.log" && fail "en X11 se mandó un token de activación" \
    || ok "en X11 no se manda token de activación"
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
  # Desde 0058 la opción viene activada: la prueba parte de apagada
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/symbolic-icons -n -t bool -s false
  start_panel
  start_app sni_app.py "$WORK/sni.log" prueba-moderna
  sleep 3
  shot; before=$(saturation 15 "$PANEL_Y")
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/symbolic-icons -s true
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

# App antigua que imita a ibus (ver apps/ibus_like_app.py). Con el panel en
# 22 px: el ícono aparece (el proxy le avisa el tamaño final cuando recorta
# su pedido; sin eso GTK no dibujaba), nace al tamaño justo y un clic abre
# un solo menú (ibus conecta su menú en cada cambio de tamaño)
t_un_menu_por_clic() {
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/panel-icon-size -n -t int -s 22
  start_panel
  start_app ibus_like_app.py "$WORK/app.log"
  sleep 3
  click 15 "$PANEL_Y" 1; sleep 1.5
  [ "$(count INIT "$WORK/app.log")" = 1 ] && ok "un solo tamaño al aparecer" \
    || fail "$(count INIT "$WORK/app.log") tamaños al aparecer: $(grep INIT "$WORK/app.log" | tr '\n' ' ')"
  [ "$(count MENU "$WORK/app.log")" = 1 ] && ok "un clic abre un solo menú" \
    || fail "un clic abrió $(count MENU "$WORK/app.log") menús (esperado 1)"
}

# Con la app tipo ibus ya corriendo, reiniciar el panel: el ícono se
# vuelve a anclar al mismo tamaño (el proxy no deja que la app lo cambie)
# y sigue abriendo un solo menú por clic
t_un_menu_tras_reiniciar() {
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/panel-icon-size -n -t int -s 22
  start_panel
  start_app ibus_like_app.py "$WORK/app.log"
  sleep 3
  local size
  size=$(app_window_size ibus-like-app)
  [ "$size" = 22x22 ] && ok "la ventana de la app mide lo que el ícono (22x22)" \
    || fail "la ventana de la app mide $size (esperado 22x22)"
  restart_panel
  restart_panel
  size=$(app_window_size ibus-like-app)
  [ "$size" = 22x22 ] && ok "y sigue igual tras dos reinicios" \
    || fail "tras reiniciar, la ventana de la app mide $size (esperado 22x22)"
  click 15 "$PANEL_Y" 1; sleep 1.5
  [ "$(count INIT "$WORK/app.log")" = 1 ] && ok "mismo tamaño tras dos reinicios" \
    || fail "tamaños vistos: $(grep INIT "$WORK/app.log" | tr '\n' ' ')"
  [ "$(count MENU "$WORK/app.log")" = 1 ] && ok "un clic sigue abriendo un solo menú" \
    || fail "un clic abrió $(count MENU "$WORK/app.log") menús (esperado 1)"
}

# App tipo ibus oculta en el popup (24 px) con el panel en 22: nace
# directamente en 24 (el plugin le dice al proxy qué apps están ocultas) y
# un clic en el popup abre un solo menú, también tras reiniciar el panel
t_un_menu_en_popup() {
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/panel-icon-size -n -t int -s 22
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/popup-icon-size -n -t int -s 24
  start_panel ibus-like-app
  start_app ibus_like_app.py "$WORK/app.log"
  sleep 3
  [ "$(count INIT "$WORK/app.log")" = 1 ] && ok "en el popup nace con su tamaño" \
    || fail "tamaños vistos al aparecer: $(grep INIT "$WORK/app.log" | tr '\n' ' ')"
  restart_panel
  [ "$(count INIT "$WORK/app.log")" = 1 ] && ok "y lo mantiene tras reiniciar el panel" \
    || fail "tamaños vistos tras reiniciar: $(grep INIT "$WORK/app.log" | tr '\n' ' ')"
  open_popup || { fail "el popup no se abrió"; return; }
  click "$POPUP_CX" "$POPUP_CY" 1; sleep 1.5
  [ "$(count MENU "$WORK/app.log")" = 1 ] && ok "un clic en el popup abre un solo menú" \
    || fail "un clic en el popup abrió $(count MENU "$WORK/app.log") menús (esperado 1)"
}

# Panel en 22 y popup en 24, una app antigua oculta (24) y la tipo ibus en
# el panel (22). Al reiniciar la tipo ibus, nace en 22: el proxy recuerda
# el tamaño de cada app, sin mezclarlo con el de las demás
t_un_menu_al_reiniciar_app() {
  local pid
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/panel-icon-size -n -t int -s 22
  xfconf-query -c xfce4-panel -p /plugins/plugin-1/popup-icon-size -n -t int -s 24
  start_panel legacy-app
  start_app legacy_app.py "$WORK/legacy.log"
  start_app ibus_like_app.py "$WORK/app1.log"; pid=$LAST_PID
  sleep 4
  kill "$pid"; sleep 1
  start_app ibus_like_app.py "$WORK/app.log"
  sleep 4
  [ "$(count INIT "$WORK/app.log")" = 1 ] && ok "al reiniciarla nace con su tamaño" \
    || fail "tamaños vistos al reiniciarla: $(grep INIT "$WORK/app.log" | tr '\n' ' ')"
  # Panel: [flecha][app tipo ibus]
  click 33 "$PANEL_Y" 1; sleep 1.5
  [ "$(count MENU "$WORK/app.log")" = 1 ] && ok "un clic abre un solo menú" \
    || fail "un clic abrió $(count MENU "$WORK/app.log") menús (esperado 1)"
}

# Un clic en el primer ícono del panel (sin flecha) activa la app
click_first_icon() {   # archivo-log
  local x
  for x in 15 25 35 45; do
    click "$x" "$PANEL_Y" 1
    sleep 0.5
    grep -q ACTIVATE "$1" && return 0
  done
  return 1
}

# App "rebelde" que se vuelve a registrar cada vez que el watcher avisa de
# un registro (0051): antes eran miles de vueltas por segundo
t_registro_repetido() {
  local n
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna reregistra
  sleep 4
  n=$(count REGISTER "$WORK/app.log")
  [ "$n" -lt 10 ] && ok "sin bucle de registros ($n en 4 s)" \
    || fail "la app se registró $n veces en 4 s (bucle)"
  click_first_icon "$WORK/app.log" && ok "y su ícono funciona" \
    || fail "la app no recibió Activate"
}

# App que se registra antes de poder responder (0052): se reintenta y
# aparece igual
t_registro_temprano() {
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna tarde
  sleep 5
  click_first_icon "$WORK/app.log" && ok "aparece aunque se registró antes de estar lista" \
    || fail "la app que se registró temprano no apareció"
}

# App que avisa sin parar sin cambiar nada (0054): tras unas lecturas, el
# plugin pasa a leerla una vez por segundo
t_avisos_vacios() {
  local a b
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna ruidosa
  sleep 4
  a=$(count GETALL "$WORK/app.log")
  sleep 4
  b=$(count GETALL "$WORK/app.log")
  [ "$a" -gt 0 ] || fail "el plugin nunca leyó la app"
  [ $((b - a)) -le 8 ] && ok "lecturas frenadas ($((b - a)) en 4 s)" \
    || fail "$((b - a)) lecturas en 4 s con avisos vacíos (esperado: unas 4)"
  click_first_icon "$WORK/app.log" && ok "y su ícono funciona" \
    || fail "la app no recibió Activate"
}

# Guardar solo lo modificado (0060): lo que nadie tocó no queda escrito y
# un cambio hecho desde afuera no se pisa
t_guardado_minimo() {
  local q="xfconf-query -c xfce4-panel -p /plugins/plugin-1"
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna
  sleep 5   # registro de la app + guardado agrupado (hasta 2 s)
  $q/known-items 2>/dev/null | grep -qx prueba-moderna && ok "la app nueva se guardó" \
    || fail "la app nueva no se guardó"
  if $q/popup-columns >/dev/null 2>&1 || $q/popup-icon-size >/dev/null 2>&1; then
    fail "se escribieron opciones que nadie tocó"
  else
    ok "lo que nadie tocó no queda escrito"
  fi
  # Una app nueva deja un guardado pendiente; mientras, un cambio de afuera
  start_app sni_app.py "$WORK/app2.log" prueba-otra
  sleep 1
  $q/popup-spacing -n -t int -s 9
  sleep 4
  [ "$($q/popup-spacing 2>/dev/null)" = 9 ] && ok "el cambio de afuera no se pisó" \
    || fail "el cambio de afuera se perdió (popup-spacing: $($q/popup-spacing 2>&1))"
  $q/known-items 2>/dev/null | grep -qx prueba-otra && ok "y lo nuestro también se guardó" \
    || fail "la segunda app no se guardó"
}

# Ancho (en píxeles) de lo que el plugin ocupa en el panel: columnas del
# panel que cambian entre la captura $1 y la última
changed_width() {   # captura-de-referencia
  shot
  convert "$1" "$WORK/s.png" -crop "${SCREEN_W}x${PANEL_SIZE}+0+$((SCREEN_H - PANEL_SIZE))" \
          +repage -compose difference -composite -threshold 5% -trim \
          -format '%w' info: 2>/dev/null
}

# Texto junto al ícono (XAyatanaLabel, 0065): aparece cuando la app lo
# pone, se va al apagar la opción y el ícono sigue respondiendo
t_texto_junto_al_icono() {
  local q="xfconf-query -c xfce4-panel -p /plugins/plugin-1" w
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna etiqueta
  sleep 2.5
  xdotool mousemove 500 300
  sleep 0.5
  shot; cp "$WORK/s.png" "$WORK/antes.png"
  wait_for 'grep -q LABEL "$WORK/app.log"' 6 || fail "la app no puso su texto"
  sleep 1.5
  w=$(changed_width "$WORK/antes.png")
  cp "$WORK/s.png" "$WORK/con-texto.png"
  [ "${w:-0}" -ge 15 ] && ok "aparece el texto junto al ícono (${w} px)" \
    || fail "no apareció el texto (cambiaron ${w:-0} px)"
  click_first_icon "$WORK/app.log" && ok "el ícono sigue respondiendo" \
    || fail "la app no recibió Activate"
  $q/show-labels -n -t bool -s false
  xdotool mousemove 500 300   # fuera del ícono: sin resaltado
  sleep 1.5
  w=$(changed_width "$WORK/antes.png")
  [ "${w:-0}" -le 2 ] && ok "con la opción apagada no se muestra" \
    || fail "con la opción apagada sigue el texto (cambiaron ${w} px)"
}

# Nombres para lectores de pantalla (0066): la flecha y cada ícono tienen
# un nombre, y el texto de la app se suma al nombre. Necesita AT-SPI
# (at-spi2-core y el puente de GTK); sin eso, se saltea
t_nombres_accesibles() {
  local names rc
  unset NO_AT_BRIDGE
  start_panel
  start_app sni_app.py "$WORK/app.log" prueba-moderna etiqueta
  sleep 7   # registro + el texto que la app pone a los 4 s
  names=$("$PYTHON" "$APPS/a11y_names.py" 2>>"$WORK/apps.err"); rc=$?
  if [ "$rc" = 77 ]; then
    ok "sin AT-SPI en esta máquina: prueba salteada"
    return
  fi
  [ "$rc" = 0 ] || fail "el panel no aparece para los lectores de pantalla"
  printf '%s\n' "$names" >"$WORK/a11y.txt"
  grep -qx $'interruptor\tShow hidden icons' "$WORK/a11y.txt" \
    && ok "la flecha tiene nombre" || fail "la flecha no tiene nombre"
  grep -q $'^boton\tprueba-moderna 42 °C$' "$WORK/a11y.txt" \
    && ok "el ícono tiene nombre, con su texto" \
    || fail "el ícono no tiene el nombre esperado ($(grep -c . "$WORK/a11y.txt") botones)"
}

# Íconos antiguos sin alfa (0067). La app pinta un fondo blanco y un
# cuadro negro con un "ojo" blanco. Antes todo el blanco se volvía
# transparente (el ojo quedaba agujereado); con forma (Wine), el negro.
# Se mira el ícono en la captura: tiene que haber negro y el centro del
# negro tiene que ser blanco puro
legacy_noalpha() {   # [shape]
  local bin="$WORK/xembed_noalpha" box cx cy center left
  if ! cc "$APPS/xembed_noalpha.c" -o "$bin" -lX11 -lXext 2>>"$WORK/apps.err"; then
    ok "no se pudo compilar la app de prueba (falta cc o libX11): prueba salteada"
    return
  fi
  start_panel
  sleep 1
  "$bin" "$@" & PIDS+=($!)
  sleep 5
  xdotool mousemove 500 300
  sleep 1
  shot
  # Caja de los píxeles negros dentro del panel: "ancho alto x y"
  box=$(convert "$WORK/s.png" -crop "${SCREEN_W}x${PANEL_SIZE}+0+$((SCREEN_H - PANEL_SIZE))" \
          +repage -fill white +opaque black -negate -trim -format '%w %h %X %Y' info: 2>/dev/null)
  set -- $box
  if [ "${1:-0}" -lt 4 ] || [ "${2:-0}" -lt 4 ] || [ "${1:-0}" -gt 60 ]; then
    fail "no se ve el cuadro negro del ícono (${box:-nada})"
    return
  fi
  cx=$(( ${3#+} + $1 / 2 )); cy=$(( SCREEN_H - PANEL_SIZE + ${4#+} + $2 / 2 ))
  center=$(convert "$WORK/s.png" -format '%[hex:u.p{'"$cx,$cy"'}]' info: 2>/dev/null)
  left=$(convert "$WORK/s.png" -format '%[hex:u.p{'"$(( ${3#+} - 2 )),$cy"'}]' info: 2>/dev/null)
  echo "caja $box centro $center izquierda $left" >>"$WORK/apps.err"
  case "$center" in FFFFFF*) ok "el interior blanco del ícono se conserva" ;;
    *) fail "el interior del ícono quedó transparente (centro: $center)" ;; esac
  LEGACY_LEFT=$left
}

t_icono_sin_alfa() {
  LEGACY_LEFT=
  legacy_noalpha
  [ -n "$LEGACY_LEFT" ] || return 0
  case "$LEGACY_LEFT" in FFFFFF*) fail "el fondo blanco del ícono no se quitó" ;;
    *) ok "el fondo del ícono se quitó" ;; esac
}

t_icono_con_forma() {
  legacy_noalpha shape
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
    [ -n "${TRAYOVERFLOW_TEST_KEEP:-}" ] || rm -rf "$work"
  fi
done

say ""
if [ $BAD -eq 0 ]; then
  say "Todas las pruebas pasaron ($TOTAL)."
else
  say "$BAD de $TOTAL pruebas fallaron."
fi
[ $BAD -eq 0 ]
