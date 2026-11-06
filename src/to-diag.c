/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Informe de diagnóstico (ver to-diag.h).
 */
#include "to-common.h"
#include "to-diag.h"
#include "to-button.h"
#include "to-legacy.h"
#include "to-sni.h"

#include <gdk/gdkx.h>
#include <string.h>

/* No mostrar la carpeta personal (en rutas de íconos): "~/…" */
static gchar *
hide_home (const gchar *text)
{
  const gchar *home = g_get_home_dir ();
  gsize len;

  if (text == NULL)
    return g_strdup ("");

  len = home != NULL ? strlen (home) : 0;
  if (len > 1 && g_str_has_prefix (text, home) && (text[len] == '/' || text[len] == '\0'))
    return g_strconcat ("~", text + len, NULL);

  return g_strdup (text);
}



static const gchar *
yes_no (gboolean value)
{
  return value ? "yes" : "no";
}



static const gchar *
legacy_state_name (TrayLegacyState state)
{
  switch (state)
    {
    case TRAY_LEGACY_UNAVAILABLE: return "not installed";
    case TRAY_LEGACY_OFF:         return "off";
    case TRAY_LEGACY_ACTIVE:      return "active";
    case TRAY_LEGACY_WAITING:     return "waiting (another classic tray is active)";
    case TRAY_LEGACY_FAILED:      return "FAILED to start";
    case TRAY_LEGACY_NO_XWAYLAND: return "unavailable (no XWayland)";
    default:                      return "unknown";
    }
}



static const gchar *
status_name (TrayItemStatus status)
{
  switch (status)
    {
    case TRAY_ITEM_STATUS_PASSIVE:   return "passive";
    case TRAY_ITEM_STATUS_ATTENTION: return "NEEDS ATTENTION";
    case TRAY_ITEM_STATUS_ACTIVE:
    default:                         return "active";
    }
}



static const gchar *
mode_name (XfcePanelPluginMode mode)
{
  switch (mode)
    {
    case XFCE_PANEL_PLUGIN_MODE_VERTICAL: return "vertical";
    case XFCE_PANEL_PLUGIN_MODE_DESKBAR:  return "deskbar";
    case XFCE_PANEL_PLUGIN_MODE_HORIZONTAL:
    default:                              return "horizontal";
    }
}



/* De dónde sale el ícono que se ve */
static gchar *
icon_source (TrayPlugin *p,
             TrayItem   *item)
{
  const gchar *key = tray_item_get_key (item);
  const gchar *choice = tray_settings_get_icon_choice (p->settings, key);
  const gchar *override = tray_settings_get_icon_override (p->settings, key);
  const gchar *name = tray_item_get_icon_name (item);
  gchar *shown, *result;

  if (choice != NULL && g_strcmp0 (choice, TRAY_ICON_OWN) != 0)
    {
      shown = hide_home (choice);
      result = g_strdup_printf ("chosen by user: %s", shown);
    }
  else if (override != NULL)
    {
      shown = hide_home (override);
      result = g_strdup_printf ("default list: %s", shown);
    }
  else
    {
      shown = hide_home (name);
      result = (name != NULL && *name != '\0')
               ? g_strdup_printf ("app, name: %s%s", shown,
                                  choice != NULL ? " (own icon forced)" : "")
               : g_strdup_printf ("app, pixels%s",
                                  choice != NULL ? " (own icon forced)" : "");
    }

  g_free (shown);
  return result;
}



static gboolean
in_order (GPtrArray *order,
          GtkWidget *button)
{
  guint i;

  for (i = 0; order != NULL && i < order->len; i++)
    if (g_ptr_array_index (order, i) == button)
      return TRUE;

  return FALSE;
}



gchar *
tray_diag_report (TrayPlugin *p)
{
  GString *out = g_string_new (NULL);
  GtkWidget *widget = GTK_WIDGET (p->panel_plugin);
  GdkDisplay *display = gtk_widget_get_display (widget);
  gboolean x11 = GDK_IS_X11_DISPLAY (display);
  gchar *theme = NULL, *gtk_theme = NULL;
  GPtrArray *items, *known;
  gboolean is_proxy = FALSE;
  guint i, running = 0;

  /* ── Versiones y sesión ── */
  g_string_append_printf (out, "Tray Overflow %s\n", PACKAGE_VERSION);
  g_string_append_printf (out, "libxfce4panel %u.%u.%u, GTK %u.%u.%u\n",
                          libxfce4panel_major_version, libxfce4panel_minor_version,
                          libxfce4panel_micro_version,
                          gtk_get_major_version (), gtk_get_minor_version (),
                          gtk_get_micro_version ());
  g_string_append_printf (out, "Session: %s, desktop: %s\n",
                          x11 ? "X11" : "Wayland",
                          g_getenv ("XDG_CURRENT_DESKTOP") != NULL
                          ? g_getenv ("XDG_CURRENT_DESKTOP") : "?");
  if (x11)
    g_string_append_printf (out, "Window manager: %s\n",
                            gdk_x11_screen_get_window_manager_name (
                              gtk_widget_get_screen (widget)));
  else
    g_string_append_printf (out, "Popup as layer-shell surface: %s\n",
                            yes_no (p->popup_layer));
  g_string_append_printf (out, "Compositing: %s, scale: %d\n",
                          yes_no (gdk_screen_is_composited (gtk_widget_get_screen (widget))),
                          gtk_widget_get_scale_factor (widget));

  g_object_get (gtk_widget_get_settings (widget),
                "gtk-icon-theme-name", &theme, "gtk-theme-name", &gtk_theme, NULL);
  g_string_append_printf (out, "Theme: %s, icons: %s\n",
                          gtk_theme != NULL ? gtk_theme : "?", theme != NULL ? theme : "?");
  g_free (theme);
  g_free (gtk_theme);

  /* ── Panel y backends ── */
  g_string_append_printf (out, "\nPanel: %s, size %d, rows %u (icons in %d)\n",
                          mode_name (xfce_panel_plugin_get_mode (p->panel_plugin)),
                          xfce_panel_plugin_get_size (p->panel_plugin),
                          xfce_panel_plugin_get_nrows (p->panel_plugin),
                          tray_plugin_get_panel_rows (p));
  g_string_append_printf (out, "Modern apps (SNI) watcher: %s\n",
                          to_sni_backend_owns_watcher () ? "this plugin" : "another program");
  g_string_append_printf (out, "Older apps (XEmbed) proxy: %s\n",
                          legacy_state_name (tray_legacy_get_state (widget)));
  /* En Wayland solo se mira si el proxy está pedido (si no, mirar
   * despertaría XWayland en vano) */
  if (x11 || tray_settings_get_legacy_tray (p->settings))
    {
      gint owner = tray_legacy_tray_owner (widget, &is_proxy);

      g_string_append_printf (out, "Classic tray owner: %s\n",
                              owner == 1 ? (is_proxy ? "our proxy" : "another tray")
                              : owner == 0 ? "none" : "unknown");
    }

  /* ── Opciones ── */
  g_string_append_printf (out,
                          "\nSettings: panel icon %d (0 = auto, now %d), popup icon %d, "
                          "columns %d (shown %d), spacing %d/%d, corners %d, arrow %d\n",
                          tray_settings_get_panel_icon_size (p->settings),
                          tray_plugin_get_panel_icon_size (p),
                          tray_settings_get_popup_icon_size (p->settings),
                          tray_settings_get_popup_columns (p->settings),
                          p->layout_params.columns,
                          tray_settings_get_panel_spacing (p->settings),
                          tray_settings_get_popup_spacing (p->settings),
                          tray_settings_get_popup_corners (p->settings),
                          tray_settings_get_arrow_position (p->settings));
  g_string_append_printf (out,
                          "Options: passive in popup %s, new in popup %s, "
                          "symbolic %s, single row %s, older apps %s\n",
                          yes_no (tray_settings_get_passive_in_popup (p->settings)),
                          yes_no (tray_settings_get_new_in_popup (p->settings)),
                          yes_no (tray_settings_get_symbolic_icons (p->settings)),
                          yes_no (tray_settings_get_single_row (p->settings)),
                          yes_no (tray_settings_get_legacy_tray (p->settings)));

  /* ── Apps abiertas ── */
  items = tray_model_get_items (p->model);
  for (i = 0; i < items->len; i++)
    if (tray_item_get_key (g_ptr_array_index (items, i)) != NULL)
      running++;

  g_string_append_printf (out, "\nRunning apps (%u):\n", running);
  for (i = 0; i < items->len; i++)
    {
      TrayItem *item = g_ptr_array_index (items, i);
      GtkWidget *button = g_hash_table_lookup (p->buttons, item);
      const gchar *where;
      gchar *source;

      if (tray_item_get_key (item) == NULL)
        continue;

      if (button != NULL && in_order (p->popup_order, button))
        where = "popup";
      else if (button != NULL && in_order (p->panel_order, button))
        where = "panel";
      else
        where = "not placed";

      source = icon_source (p, item);
      g_string_append_printf (out, "- %s \"%s\": %s, %s, %s, icon: %s\n",
                              tray_item_get_key (item),
                              tray_item_get_display_name (item),
                              tray_item_get_is_legacy (item) ? "older app (XEmbed)" : "modern (SNI)",
                              status_name (tray_item_get_status (item)),
                              where, source);
      g_free (source);
    }

  /* ── Apps recordadas ── */
  known = tray_settings_get_known (p->settings);
  g_string_append_printf (out, "\nRemembered apps: %u\n", known->len);

  return g_string_free (out, FALSE);
}
