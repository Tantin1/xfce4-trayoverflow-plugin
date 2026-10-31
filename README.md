# Tray Overflow — Bandeja con desborde para xfce4-panel

Plugin para el panel de XFCE que muestra los íconos de las aplicaciones
(StatusNotifierItem / AppIndicator) y permite **ocultar los que no usás a
diario en una ventana emergente**, como el área de notificación de
Windows 10/11.

```
  [▲][✉][📁][■]          ← panel: íconos visibles
   │
   └─► ┌───────────┐
       │ ⚙  💬  ■  │      ← ventana emergente: íconos ocultos
       └───────────┘
```

## Funciones

- **Ocultar y mostrar** íconos arrastrándolos entre el panel y la ventana
  emergente, o desde la lista de la configuración.
- **Reordenar** arrastrando, en el panel o en la ventana emergente.
- Quedarse con un ícono sobre la flecha mientras se arrastra **abre la
  ventana emergente**, como en Windows.
- **Apps cerradas recordadas**: se pueden ocultar de antemano; al abrirse
  van directo a la ventana emergente.
- **Menú contextual** de cada app, también desde la ventana emergente.
- **Punto de aviso** en la flecha cuando una app oculta pide atención.
- Íconos **inactivos** (estado *Passive*) a la ventana emergente, opcional.
- Tamaño de ícono y separación **independientes** para el panel y la
  ventana emergente; columnas de la ventana emergente configurables.
- Paneles horizontales, verticales y de varias filas; HiDPI.
- X11 y **Wayland** (sesión Wayland de XFCE 4.20).
- Varias instancias (p. ej. una por panel), cada una con su configuración.
- **Apps antiguas (bandeja clásica XEmbed)**, opcional: se muestran junto a
  las demás, con ventana emergente y arrastre incluidos. Incluye apps Java
  y Wine.
- **Otro ícono para cualquier app**: del tema de íconos o de un archivo.
- **Arreglos incluidos** para apps cuyo ícono se ve mal (por ejemplo
  JDownloader o Clipman): se usa el de tu tema sin hacer nada.
- Traducción al español incluida.

## Requisitos

- xfce4-panel 4.16 o superior (probado con 4.18; pensado para 4.20).
- Paquetes de desarrollo para compilar:

| Distribución | Comando |
|---|---|
| Debian / Ubuntu / Mint | `sudo apt install build-essential meson ninja-build gettext libgtk-3-dev libxfce4panel-2.0-dev libxfce4ui-2-dev libxfconf-0-dev libdbusmenu-gtk3-dev libgtk-layer-shell-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxtst-dev libxext-dev` |
| Arch / Manjaro / CachyOS | `sudo pacman -S base-devel meson gettext gtk3 xfce4-panel libxfce4ui xfconf libdbusmenu-gtk3 gtk-layer-shell libxcomposite libxdamage libxfixes libxtst libxext` |
| Fedora | `sudo dnf install gcc meson gettext gtk3-devel xfce4-panel-devel libxfce4ui-devel xfconf-devel libdbusmenu-gtk3-devel gtk-layer-shell-devel libXcomposite-devel libXdamage-devel libXfixes-devel libXtst-devel libXext-devel` |

Las bibliotecas `libX*` son solo para el soporte de apps antiguas, y
gtk-layer-shell solo para Wayland. Si faltan, el plugin se compila igual,
sin esa opción.

## Instalación

```bash
tar xzf xfce4-trayoverflow-plugin.tar.gz
cd xfce4-trayoverflow-plugin
bash install.sh
```

Después: clic derecho en el panel → **Panel → Agregar nuevos elementos** →
**Bandeja con desborde** (en inglés, *Tray Overflow*).

`install.sh` instala con prefijo `/usr`, que es donde el panel busca los
plugins. Con `/usr/local` el panel no lo encuentra en muchas distribuciones.

### Paquete .deb (Debian, Ubuntu, Mint)

En la página de **Releases** del repositorio hay paquetes listos para
**Debian 12**, **Debian 13** y **Ubuntu 24.04** (también Linux Mint 22 y
Xubuntu 24.04). Bajá el de tu distro y:

```bash
sudo apt install ./xfce4-trayoverflow-plugin_*_debian12.deb   # o el que corresponda
xfce4-panel -r
```

Para armarlo vos (queda con el nombre de tu distro al final):

```bash
bash packaging/build-deb.sh
sudo apt install ./xfce4-trayoverflow-plugin_*.deb
xfce4-panel -r
```

Cada cambio que se sube a GitHub se compila y se prueba automáticamente en
Debian 12, Debian 13, Ubuntu 24.04 y Arch (ver
`.github/workflows/build.yml`).

### Desinstalar

```bash
bash uninstall.sh          # si se instaló con install.sh
sudo apt remove xfce4-trayoverflow-plugin   # si se instaló el .deb
```

## Uso

| Acción | Resultado |
|---|---|
| Clic en la flecha | Abre o cierra la ventana emergente |
| Clic izquierdo en un ícono | Activa la app (normalmente abre su ventana) |
| Clic central | Acción secundaria de la app |
| Clic derecho | Menú de la app |
| Rueda del ratón | Scroll en la app (p. ej. volumen) |
| Arrastrar un ícono a la flecha o a la ventana emergente | Lo oculta |
| Arrastrar desde la ventana emergente al panel | Lo vuelve a mostrar |
| Clic derecho en la flecha | Menú del plugin (Propiedades, Acerca de...) |
| Atajo de teclado (ver abajo) | Abre o cierra la ventana emergente |
| Flechas, Inicio, Fin, Tab (en la ventana emergente) | Eligen un ícono |
| Enter o espacio | Activa el ícono elegido, como un clic |
| Tecla de menú o Mayús+F10 | Menú del ícono elegido, como el clic derecho |

La ventana emergente se cierra con un clic fuera o con Escape. Al hacer
clic en uno de sus íconos, se cierra solo si la app abre o activa una
ventana, o muestra su menú; si la app no hace nada, queda abierta. Las apps
que solo tienen menú (muchas que usan AppIndicator) abren su menú también
con el clic izquierdo.

### Atajo de teclado

Como Win+B en Windows. En **Configuración → Teclado → Atajos de
aplicaciones**, agregá un atajo con el comando:

```
xfce4-panel --plugin-event=trayoverflow:popup
```

Abre la ventana emergente con el primer ícono elegido (seguí con las
flechas y Enter) o la cierra si ya está abierta. Con
`trayoverflow:popup:bool:true` solo abre y con `:bool:false` solo cierra.
Si tenés varias instancias del plugin, responde la primera que tenga íconos
ocultos.

## Configuración

**Apariencia**: tamaño de ícono del panel (automático o fijo), separación
entre íconos, **íconos simbólicos** (monocromos, del color del texto del
panel; solo en apps cuyo ícono viene del tema, como en la bandeja de XFCE), tamaño y separación en la ventana emergente, número de
columnas, y si los íconos inactivos van a la ventana emergente.

**Elementos**: lista de todas las apps vistas alguna vez, con una casilla
para ocultarlas, botones para reordenar, **Ícono…** para elegir otro ícono
y **Olvidar** para quitar apps que ya no usás. Con **Poner las apps nuevas en la
ventana emergente**, cada app que aparece por primera vez empieza oculta,
como en Windows 11; las que ya estaban en la lista no cambian.

### Elegir otro ícono

Seleccioná una app en **Elementos** y tocá **Ícono…**. Se sugieren los
íconos de tu tema con un nombre parecido; también podés escribir otro
nombre o elegir un archivo PNG o SVG. **Usar el ícono propio de la
aplicación** vuelve al original.

Mientras una app tenga otro ícono, no se ven los cambios de su ícono propio
(por ejemplo, un aviso). El tooltip sí sigue siendo el de la app.

### Apps cuyo ícono se ve mal

Algunas apps mandan un ícono roto, borroso o que falla en ciertos tamaños.
Para esas, el plugin usa por defecto el ícono de tu tema o, si el tema no
lo tiene, el que instaló el paquete de la app. Si no existe ninguno, se ve
el de la app, como siempre. Lo que elijas con **Ícono…** tiene prioridad.

La lista se instala con el plugin, en
`/usr/share/xfce4-trayoverflow-plugin/icon-defaults.conf`. Tiene una línea
por app:

```
xfce4-clipman=xfce4-clipman-plugin;org.xfce.clipman
org-jdownloader-*=jdownloader
```

- A la izquierda, la clave de la app. Para verla:
  `xfconf-query -c xfce4-panel -lv | grep -A20 known-items`. Con `*` al
  final vale para todas las claves que empiezan así.
- A la derecha, uno o más íconos (nombre del tema o ruta de archivo),
  separados por `;`. Se usa el primero que exista.

Para sumar apps sin esperar una versión nueva, creá
`~/.config/xfce4/trayoverflow/icon-defaults.conf` con el mismo formato:
tiene prioridad sobre la lista del plugin. Reiniciá el panel para
aplicarlo. Si encontrás una app que se ve mal, avisá para sumarla a la
lista.

La configuración se guarda en xfconf, en el canal `xfce4-panel`, bajo
`/plugins/plugin-N/`. Los íconos de apps cerradas se guardan en
`~/.cache/xfce4/trayoverflow/`.

## Limitaciones conocidas

### Apps antiguas (XEmbed)

Casi todas las apps actuales (Qt, Electron, Telegram, Discord, Steam,
AppIndicator...) usan el protocolo moderno StatusNotifier y se ven sin
hacer nada. Algunas apps antiguas (GTK con `GtkStatusIcon`, Java, Wine)
solo usan la bandeja clásica de X11 (XEmbed). Para verlas:

**Propiedades → Elementos → "Mostrar también las aplicaciones antiguas"**.

Eso arranca un pequeño programa aparte, `xembed-proxy`, que hace de bandeja
clásica y convierte cada ícono antiguo en uno moderno. El plugin lo inicia
y lo detiene solo, y lo cierra si el panel se cae. En reposo no usa CPU y
ocupa unos 7 MB de RAM.

- Solo puede haber **una bandeja clásica** a la vez. Si la bandeja nativa
  ("Bandeja de estado") está en el panel, las apps antiguas siguen allí y
  el proxy espera; al quitarla, pasan aquí solas.
- Si usás la bandeja nativa para las apps antiguas, también muestra las
  modernas, que aparecerían **dos veces**: ocultalas en sus propiedades.
- El ícono es una captura de la ventana de la app y los clics se simulan.
  Funciona con la gran mayoría de las apps, pero alguna muy particular
  podría no responder bien. Para diagnosticar:
  `/usr/libexec/xfce4-trayoverflow-plugin/xembed-proxy --verbose` (con la
  opción desactivada y sin otra bandeja clásica activa).
- Las apps antiguas no informan su tooltip; se muestra su nombre. En las
  apps Java, el nombre sale de su línea de comandos (por ejemplo
  `java -jar JDownloader.jar` → *JDownloader*). Las apps que dibujan su
  propio tooltip al pasar el ratón (como el resumen de descargas de
  JDownloader) no lo muestran aquí.
- Cada app antigua se redibuja al tamaño exacto en que se muestra (panel o
  ventana emergente), así que se ve nítida como en la bandeja nativa. Las
  apps GTK dibujan a tamaños estándar: con **16, 24, 32 o 48 px** ocupan
  todo el espacio; con otros valores (por ejemplo 22) usan el tamaño
  estándar inmediatamente menor, nítido pero algo más chico.

El proxy también sirve con otros escritorios que muestren StatusNotifier
(por ejemplo, otros paneles): basta con ejecutarlo al iniciar sesión.

La pestaña **Elementos** indica en cada momento qué bandeja clásica está
activa.

### Wayland

Funciona en la sesión Wayland de XFCE 4.20 con compositores que tengan
*layer-shell* (labwc, Wayfire, xfwl4…). Hace falta compilar con
**gtk-layer-shell** (la misma biblioteca que usa el panel); `meson` indica
al final si quedó incluido. El mismo plugin sirve para X11 y Wayland: elige
solo al arrancar.

Diferencias con X11:

- La ventana emergente se cierra cuando otra app pasa a primer plano (al
  hacer clic en ella o al abrir una ventana). Un clic sobre el **escritorio
  vacío** puede no cerrarla, según el compositor: usá Escape o la flecha.
  En xfwl4 (todavía en desarrollo) tampoco la cierra un clic en otra
  ventana; lo mismo pasa con el calendario del reloj del panel.
- Las apps antiguas (XEmbed) funcionan a través de **XWayland**, que el
  compositor tiene que ofrecer (labwc lo hace). Al hacer clic en una de
  ellas dentro de la ventana emergente, esta se cierra siempre, para no
  tapar el menú de la app. Sus menús se cierran con Escape o eligiendo una
  opción: un clic fuera de ellos no los cierra (es una limitación de
  XWayland).

### Apps Flatpak con ID genérico

Para reconocer una app entre reinicios se usa su ID; si es genérico (como
el de las apps Electron), el nombre de su ejecutable. En apps Flatpak el
ejecutable no es accesible y se usa el título; si una app cambia de título,
puede aparecer como una app nueva en la lista.

## Diagnóstico

```bash
# ¿El panel ve el plugin?
ls /usr/share/xfce4/panel/plugins/trayoverflow.desktop
xfce4-panel -q; PANEL_DEBUG=1 xfce4-panel 2>&1 | grep -i trayoverflow

# Mensajes del plugin
xfce4-panel -q; G_MESSAGES_DEBUG=trayoverflow xfce4-panel

# ¿Qué apps están registradas por StatusNotifier?
busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
  org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems

# Ver la configuración guardada
xfconf-query -c xfce4-panel -lv | grep -A1 trayoverflow
```

Los cambios hechos con `xfconf-query` (o desde otro programa) se aplican
al instante, también en la ventana de propiedades si está abierta. Si
algo no se refleja, `xfce4-panel -r` reinicia el panel sin cerrar la
sesión.

## Aplicar parches

Las modificaciones se distribuyen como archivos `.patch`. Desde la carpeta
del proyecto:

```bash
patch -p1 --dry-run < cambio.patch   # comprobar que aplica
patch -p1 < cambio.patch
bash install.sh
```

## Pruebas automáticas

Después de instalar con `install.sh`:

```bash
bash tests/run.sh           # todas (alrededor de un minuto)
bash tests/run.sh --lista   # ver los nombres
bash tests/run.sh popup_rueda
```

Cada prueba levanta su propia pantalla virtual (Xvfb), con su propio panel,
D-Bus y configuración en una carpeta temporal: no toca tu panel ni tu
configuración. Usan apps de prueba (una antigua y una moderna) y prueban el
popup, la rueda, los menús de apps antiguas y el proxy. Solo cubren X11;
Wayland se prueba a mano.

Necesitan `Xvfb`, `xfwm4`, `xdotool`, ImageMagick y PyGObject (la cabecera
de `tests/run.sh` dice qué paquetes instalar). Si una prueba falla, se
indica la carpeta con sus registros y la última captura de pantalla.

## Estructura del código

| Archivo | Contenido |
|---|---|
| `src/to-item.[ch]` | Ícono genérico, independiente del protocolo |
| `src/to-sni.[ch]` | Backend StatusNotifier: host y watcher de respaldo |
| `src/to-model.[ch]` | Lista compartida de íconos (una por proceso) |
| `src/to-settings.[ch]` | Configuración y registro de apps (xfconf) |
| `src/to-button.[ch]` | Botón de cada ícono: clics, scroll, tooltip |
| `src/to-plugin.[ch]` | Panel, flecha, distribución de los íconos, construcción |
| `src/to-popup.[ch]` | Ventana emergente: mostrar, ubicar, cerrar (X11 y Wayland), menús, esquinas |
| `src/to-dnd.[ch]` | Arrastrar y soltar entre el panel y la ventana emergente |
| `src/to-dialog.[ch]` | Diálogo de configuración |
| `src/to-legacy.[ch]` | Arranque y parada del proxy de apps antiguas |
| `proxy/xembed-proxy.c` | Proxy XEmbed → StatusNotifier (programa aparte) |
| `tests/` | Pruebas automáticas (`run.sh`) y apps de prueba (`apps/`) |

El proxy es un programa independiente a propósito: si una app antigua
hace algo inesperado y el proxy falla, el panel no se ve afectado.

## Licencia

GPL-2.0 o posterior. Ver `COPYING`.
