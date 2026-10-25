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
- Varias instancias (p. ej. una por panel), cada una con su configuración.
- **Apps antiguas (bandeja clásica XEmbed)**, opcional: se muestran junto a
  las demás, con ventana emergente y arrastre incluidos.
- Traducción al español incluida.

## Requisitos

- xfce4-panel 4.16 o superior (probado con 4.18; pensado para 4.20).
- Paquetes de desarrollo para compilar:

| Distribución | Comando |
|---|---|
| Debian / Ubuntu / Mint | `sudo apt install build-essential meson ninja-build gettext libgtk-3-dev libxfce4panel-2.0-dev libxfce4ui-2-dev libxfconf-0-dev libdbusmenu-gtk3-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxtst-dev libxext-dev` |
| Arch / Manjaro / CachyOS | `sudo pacman -S base-devel meson gettext gtk3 xfce4-panel libxfce4ui xfconf libdbusmenu-gtk3 libxcomposite libxdamage libxfixes libxtst libxext` |
| Fedora | `sudo dnf install gcc meson gettext gtk3-devel xfce4-panel-devel libxfce4ui-devel xfconf-devel libdbusmenu-gtk3-devel libXcomposite-devel libXdamage-devel libXfixes-devel libXtst-devel libXext-devel` |

Las bibliotecas `libX*` son solo para el soporte de apps antiguas. Si
faltan, el plugin se compila igual, sin esa opción.

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

```bash
bash packaging/build-deb.sh
sudo apt install ./xfce4-trayoverflow-plugin_*.deb
xfce4-panel -r
```

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

La ventana emergente se cierra con un clic fuera, con Escape o al activar
un ícono. Las apps que solo tienen menú (muchas que usan AppIndicator)
abren su menú también con el clic izquierdo.

## Configuración

**Apariencia**: tamaño de ícono del panel (automático o fijo), separación
entre íconos, tamaño y separación en la ventana emergente, número de
columnas, y si los íconos inactivos van a la ventana emergente.

**Elementos**: lista de todas las apps vistas alguna vez, con una casilla
para ocultarlas, botones para reordenar y **Olvidar** para quitar apps que
ya no usás.

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
- Las apps antiguas no informan su tooltip; se muestra su nombre.
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

Pensado para X11. Bajo la sesión Wayland experimental de XFCE 4.20 la
posición de la ventana emergente puede no ser correcta.

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

Si un cambio de configuración no se refleja, `xfce4-panel -r` reinicia el
panel sin cerrar la sesión.

## Aplicar parches

Las modificaciones se distribuyen como archivos `.patch`. Desde la carpeta
del proyecto:

```bash
patch -p1 --dry-run < cambio.patch   # comprobar que aplica
patch -p1 < cambio.patch
bash install.sh
```

## Estructura del código

| Archivo | Contenido |
|---|---|
| `src/to-item.[ch]` | Ícono genérico, independiente del protocolo |
| `src/to-sni.[ch]` | Backend StatusNotifier: host y watcher de respaldo |
| `src/to-model.[ch]` | Lista compartida de íconos (una por proceso) |
| `src/to-settings.[ch]` | Configuración y registro de apps (xfconf) |
| `src/to-button.[ch]` | Botón de cada ícono: clics, scroll, tooltip |
| `src/to-plugin.[ch]` | Panel, flecha, ventana emergente, arrastre |
| `src/to-dialog.[ch]` | Diálogo de configuración |
| `src/to-legacy.[ch]` | Arranque y parada del proxy de apps antiguas |
| `proxy/xembed-proxy.c` | Proxy XEmbed → StatusNotifier (programa aparte) |

El proxy es un programa independiente a propósito: si una app antigua
hace algo inesperado y el proxy falla, el panel no se ve afectado.

## Licencia

GPL-2.0 o posterior. Ver `COPYING`.
