# Tray Overflow — overflow tray for xfce4-panel

Tray Overflow is an Xfce panel plugin that displays application status icons
(StatusNotifierItem / AppIndicator) and lets you **move less frequently used
icons into a popup**, similar to the notification area in Windows 10/11.

```text
  [▲][✉][📁][■]          ← panel: visible icons
   │
   └─► ┌───────────┐
       │ ⚙  💬  ■  │      ← popup: hidden icons
       └───────────┘
```

## Features

- **Hide and show** icons by dragging them between the panel and the popup,
  or through the list in the settings dialog.
- **Reorder** icons by dragging them within the panel or popup. An insertion
  indicator shows their new position before you release the mouse button.
- **Drag-to-open**: hovering over the arrow while dragging an icon opens the
  popup, as in Windows.
- **Remember closed applications**: hide an application before it starts;
  its icon will go straight into the popup when it launches.
- Application **context menus**, including inside the popup.
- An **attention dot** on the arrow when a hidden application requests
  attention, with a tooltip listing the relevant applications.
- Optionally send **inactive** icons (the *Passive* status) to the popup.
- Independent icon size and spacing for the panel and popup, configurable
  popup columns, and additional columns when icons would not fit vertically.
- Horizontal, vertical, and multi-row panels; HiDPI support. If a manually
  selected icon size will not fit in the available panel rows, the largest
  fitting size is used without changing the saved preference. A **single-row**
  option keeps icons on one row even when the panel has several.
- **Full-row buttons**, similar to Windows: each panel icon has a button as
  tall as its panel row, with a configurable minimum width (equivalent to a
  22 px icon by default), even when the icon itself is smaller. The popup has
  the same minimum button sizing. **Compact buttons** make each button as
  small as its icon. Button corners follow the current theme.
- The popup uses the **theme's menu colors** for its background, border,
  and text, consistently with other Xfce plugins.
- **X11 and Wayland** support, including the Xfce 4.20 Wayland session,
  subject to the limitations below.
- Multiple instances, such as one per panel, with separate settings.
- Optional support for **legacy XEmbed tray applications**, including Java
  and Wine applications, with the same popup and drag-and-drop controls.
- **Custom application icons**, selected from the icon theme or a file.
- Built-in icon replacements for applications with problematic tray icons,
  such as JDownloader and Clipman, using your icon theme when available.
- Interface translations: German, Simplified Chinese, Korean, Spanish,
  French, Dutch, Italian, Japanese, Polish, Brazilian Portuguese, Russian,
  and Turkish, with English as the default. All translations except Spanish
  are machine-generated and welcome human review in `po/`.

## Requirements

- `xfce4-panel` 4.16 or later (tested with 4.18; designed with 4.20 in mind).
- Development packages to build the plugin:

| Distribution | Command |
|---|---|
| Debian / Ubuntu / Mint | `sudo apt install build-essential meson ninja-build gettext libgtk-3-dev libxfce4panel-2.0-dev libxfce4ui-2-dev libxfconf-0-dev libdbusmenu-gtk3-dev libgtk-layer-shell-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxtst-dev libxext-dev` |
| Arch / Manjaro / CachyOS | `sudo pacman -S base-devel meson gettext gtk3 xfce4-panel libxfce4ui xfconf libdbusmenu-gtk3 gtk-layer-shell libxcomposite libxdamage libxfixes libxtst libxext` |
| Fedora | `sudo dnf install gcc meson gettext gtk3-devel xfce4-panel-devel libxfce4ui-devel xfconf-devel libdbusmenu-gtk3-devel gtk-layer-shell-devel libXcomposite-devel libXdamage-devel libXfixes-devel libXtst-devel libXext-devel` |

The `libX*` development libraries are needed only for legacy XEmbed
support, and `gtk-layer-shell` is needed only for the Wayland popup. If
optional libraries are missing, Meson can build without those features.

## Installation

For a source installation:

```bash
tar xzf xfce4-trayoverflow-plugin.tar.gz
cd xfce4-trayoverflow-plugin
bash install.sh
```

Then right-click the panel → **Panel → Add New Items** → **Tray Overflow**.

`install.sh` installs under `/usr`, which is where the panel typically looks
for plugins. On many distributions, installing to `/usr/local` will not make
the plugin visible. **Do not mix manual and package-managed installations.**
If you use a `.deb` or Arch package, prefer that installation method instead.

### Debian package (Debian, Ubuntu, Mint)

The repository's **Releases** page provides packages for **Debian 12**,
**Debian 13**, and **Ubuntu 24.04** (also suitable for Linux Mint 22 and
Xubuntu 24.04). Download the package for your distribution and run:

```bash
sudo apt install ./xfce4-trayoverflow-plugin_*_debian12.deb   # choose the correct file
xfce4-panel -r
```

To build a package locally (the filename includes your distribution):

```bash
bash packaging/build-deb.sh
sudo apt install ./xfce4-trayoverflow-plugin_*.deb
xfce4-panel -r
```

### Arch package (Arch, Manjaro, CachyOS, EndeavourOS)

The **Releases** page also provides a `.pkg.tar.zst` package. To build one
yourself, run the following commands as a regular user, **not root**:

```bash
bash packaging/build-arch.sh
sudo pacman -U ./xfce4-trayoverflow-plugin-*.pkg.tar.zst
xfce4-panel -r
```

Pacman will track the installation, which you can remove with
`sudo pacman -R xfce4-trayoverflow-plugin`.

Each push to GitHub triggers builds and automated tests on Debian 12,
Debian 13, and Ubuntu 24.04. Arch is built and packaged, but has no
automated runtime tests in the workflow; see `.github/workflows/build.yml`.
This describes the workflow configuration, not a guarantee that any
particular run passed.

### Uninstallation

```bash
bash uninstall.sh                        # only for an install.sh installation
sudo apt remove xfce4-trayoverflow-plugin # only for a Debian package
sudo pacman -R xfce4-trayoverflow-plugin # only for an Arch package
```

Do not mix installation methods. Before installing a distribution package,
remove a previous manual installation with `bash uninstall.sh`; do the
reverse when switching back to manual installation. Check which method is
currently in use before removing anything.

## Usage

| Action | Result |
|---|---|
| Click the arrow | Open or close the popup |
| Left-click an icon | Activate the application (usually its window) |
| Middle-click an icon | Invoke the application's secondary action |
| Right-click an icon | Show the application's menu |
| Scroll over an icon | Send scroll events to the application (e.g. volume) |
| Drag an icon onto the arrow or into the popup | Hide it |
| Drag an icon from the popup to the panel | Show it |
| Right-click the arrow | Open the plugin menu (Properties, About, etc.) |
| Use the keyboard shortcut below | Open or close the popup |
| Arrow keys, Home, End, Tab inside the popup | Select an icon |
| Enter or Space | Activate the selected icon, like a click |
| Menu key or Shift+F10 | Open the selected icon's context menu |

The popup closes on an outside click or Escape, with the Wayland exceptions
below. Clicking a popup icon closes it only if the application opens or
activates a window or opens its menu; otherwise the popup stays visible.
Applications that expose only a menu (including many AppIndicators) also
open that menu on a left-click.

### Keyboard shortcut

To get behavior similar to Win+B on Windows, open **Settings → Keyboard →
Application Shortcuts** and assign a shortcut to:

```text
xfce4-panel --plugin-event=trayoverflow:popup
```

This opens the popup with the first icon selected (navigate with the arrow
keys and Enter), or closes it if already open. Use
`trayoverflow:popup:bool:true` to open only, and `:bool:false` to close only.
If there are multiple plugin instances, the first with hidden icons responds.

## Settings

**Panel button:** icon size (automatic or fixed), icon spacing, minimum
button size (or **Compact**), arrow position, **symbolic icons**
(monochrome icons using the panel text color, only for applications whose
icons come from the theme), and single-row layout.

**Popup:** icon size and spacing, number of columns, and corner style.

**Items:** all applications previously seen, with a checkbox to hide each
one, reorder controls, an **Icon…** button to choose a replacement, and
**Forget** to remove an unused application from the list. **Put new
applications in the popup** starts newly discovered icons hidden, like
Windows 11; already known applications are not changed. **Move inactive
icons to the popup** is also configured here.

Hover over a setting to see its explanation. On shorter displays, the
settings tabs can be scrolled.

### Choose a different icon

Select an application under **Items** and click **Icon…**. Icon names from
your theme are suggested based on similarity; you can also enter another
name or select a PNG or SVG file. **Use the application's own icon**
restores the original.

When a custom icon is selected, changes to the application's original icon
(such as an alert overlay) will no longer be shown. Its tooltip continues
to come from the application.

### Applications with problematic icons

Some applications provide blurry, broken, or incorrectly sized tray icons.
For known applications, the plugin first tries an icon from your theme or
from the application's installed files, and falls back to the original icon
if none is available. A custom icon chosen in the settings has priority.

The plugin installs its defaults at:

```text
/usr/share/xfce4-trayoverflow-plugin/icon-defaults.conf
```

Each application has one line:

```ini
xfce4-clipman=clipman;xfce4-clipman-plugin;org.xfce.clipman
org-jdownloader-*=jdownloader
```

The left side is the application's key. You can inspect known keys with:

```bash
xfconf-query -c xfce4-panel -lv | grep -A20 known-items
```

An ending `*` matches all keys with that prefix. The right side lists one
or more icon names from the theme or file paths, separated by `;`, with the
first available icon taking precedence.

To add a replacement without changing the installed defaults, create:

```text
~/.config/xfce4/trayoverflow/icon-defaults.conf
```

Use the same format; this user file takes priority. Restart the panel for
changes to take effect. Reports of other problematic application icons are
welcome.

Settings are stored in xfconf, under the `xfce4-panel` channel and
`/plugins/plugin-N/`. Cached icons for closed applications are under
`~/.cache/xfce4/trayoverflow/`. Removing the plugin from the panel removes
its settings and cached icons, except where another instance still needs
those icons.

## Known limitations

### Legacy applications (XEmbed)

Most modern applications (Qt, Electron, Telegram, Discord, Steam,
AppIndicator, and others) use StatusNotifier and need no special setup.
Some older applications (GTK `GtkStatusIcon`, Java, Wine) use only the
classic X11 tray protocol, XEmbed. Enable them under:

**Properties → Items → Show legacy applications too**.

This starts a separate helper program, `xembed-proxy`, that exposes classic
tray icons as modern StatusNotifier items. The plugin starts and stops it
as needed and closes it if the panel crashes. The helper is described as
using approximately 7 MB of RAM at idle, with negligible idle CPU use;
actual resource usage depends on the system.

- Only **one classic tray** can be active at a time. If the native Xfce
  **Status Tray Plugin** owns it, legacy applications remain there and the
  proxy waits. Removing that tray allows their icons to appear here.
- If the native tray is kept for legacy icons, it may also display modern
  icons **twice**. Hide duplicates in that tray's properties.
- A legacy application's icon comes from its tray window, and clicks are
  simulated. This works for most applications, but unusual ones may behave
  differently. For diagnostics, with the option disabled and no competing
  classic tray, run:

  ```bash
  /usr/libexec/xfce4-trayoverflow-plugin/xembed-proxy --verbose
  ```

- Legacy applications do not expose tooltip text through this mechanism;
  the plugin displays the application name. For Java applications the name
  is derived from the command line (for example,
  `java -jar JDownloader.jar` → *JDownloader*). Custom application-drawn
  tooltip windows are not reproduced.
- Legacy icons are drawn at the displayed size for the panel or popup.
  GTK applications typically draw at standard sizes: **16, 24, 32, or
  48 px** fill the available space, while other sizes (for example, 22 px)
  may use the next smaller standard size.

The proxy can also be started independently in other desktop environments
that support StatusNotifier. The **Items** tab reports the current
classic-tray status.

### Wayland

The plugin supports the Xfce 4.20 Wayland session with compositors offering
*layer-shell* (such as labwc, Wayfire, and xfwl4). Build with
**gtk-layer-shell**; the Meson summary reports whether support is enabled.
The same plugin binary selects X11 or Wayland behavior at runtime.

Differences from X11:

- The popup closes when another application gains focus (for example, by
  clicking or opening a window). Clicking an **empty desktop** may not
  close it, depending on the compositor; use Escape or the arrow instead.
  On the still-developing xfwl4 compositor, clicking another window also
  may not close it, similarly to the panel clock's calendar popup.
- Legacy XEmbed applications need the compositor to provide **XWayland**
  (labwc does). Clicking a legacy icon inside the popup always closes the
  popup so it will not cover the application's menu. Such menus close on
  Escape or by choosing an item; an outside click does not close them,
  which is an XWayland limitation.

### Flatpak applications with generic IDs

To recognize an application after a restart, the plugin uses its ID, or
its executable name if the ID is generic (as with some Electron apps).
A Flatpak application's executable may be inaccessible, in which case
its Flatpak App ID is used (for example, `com.discordapp.Discord`). If
that also cannot be determined, the title is used; a changing title may
make the application look new to the plugin.

Since version 0.2.34, some Flatpak applications previously identified by
their title are identified by App ID. They appear as new on their first
run after the change and may need to be hidden again.

## Diagnostics

If something is not working, open **Properties → Diagnostics → Copy** to
copy a summary of versions, session type, tray state, each application's
state, and the origin of each icon. It does not intentionally include
personal information beyond the names of tray applications.

If the plugin does not appear in the panel's plugin list:

```bash
# Does the panel see the plugin?
ls /usr/share/xfce4/panel/plugins/trayoverflow.desktop
xfce4-panel -q; PANEL_DEBUG=1 xfce4-panel 2>&1 | grep -i trayoverflow

# Plugin logging
xfce4-panel -q; G_MESSAGES_DEBUG=trayoverflow xfce4-panel

# Which applications are registered with StatusNotifier?
busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher \
  org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems

# Inspect saved configuration
xfconf-query -c xfce4-panel -lv | grep -A1 trayoverflow
```

These commands can stop or start a panel; run them deliberately, not as
part of a documentation-only update.

Changes made through `xfconf-query` (or another application) are applied
immediately, including in an open settings window. If a change is not
reflected, `xfce4-panel -r` restarts the panel without ending the session.

## Applying patches

Some changes may be distributed as `.patch` files. In the project root:

```bash
patch -p1 --dry-run < change.patch   # check first
patch -p1 < change.patch
```

Rebuild using your existing installation or packaging method. Do not run
`bash install.sh` over a Pacman- or Dpkg-managed installation.

## Automated tests

After a **manual** source installation, the X11 test suite can be run with:

```bash
bash tests/run.sh          # all tests (about a minute)
bash tests/run.sh --lista  # list test names
bash tests/run.sh popup_rueda
```

Each test starts an isolated Xvfb display with its own panel, D-Bus, and
temporary configuration; it does not access your regular panel or its
settings. Test applications include a classic tray app and a modern one.
The suite exercises popup behavior, scrolling, legacy application menus,
and the proxy. Only X11 is covered; Wayland needs separate manual testing.

Tests require `Xvfb`, `xfwm4`, `xdotool`, ImageMagick, and PyGObject;
see the header of `tests/run.sh` for the packages. A failed test leaves
its log directory and the last screenshot for troubleshooting.

## Source layout

| File | Purpose |
|---|---|
| `src/to-item.[ch]` | Protocol-independent icon representation |
| `src/to-sni.[ch]` | StatusNotifier host and fallback watcher backend |
| `src/to-model.[ch]` | Shared application icon list (one per process) |
| `src/to-settings.[ch]` | Configuration and remembered apps (xfconf) |
| `src/to-button.[ch]` | Icon buttons, clicks, scrolling, and tooltips |
| `src/to-plugin.[ch]` | Panel integration, arrow, layout, construction |
| `src/to-popup.[ch]` | Popup positioning, display, closing, menus, and corners (X11/Wayland) |
| `src/to-dnd.[ch]` | Drag-and-drop between the panel and popup |
| `src/to-dialog.[ch]` | Settings dialog |
| `src/to-legacy.[ch]` | Legacy proxy lifecycle |
| `proxy/xembed-proxy.c` | Standalone XEmbed → StatusNotifier proxy |
| `tests/` | Automated test runner and test applications |

The proxy is a separate process by design: an unexpected legacy
application or a proxy crash should not crash the panel itself.

## Maintainer and license

Maintainer: **Tantin1** — `juanperez33840@gmail.com`.

Copyright notices remain with their respective source files. Licensed
under **GPL-2.0-or-later**; see `COPYING`.
