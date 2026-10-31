/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vista del plugin:
 *
 *   [▲][ícono][ícono][ícono]      ← panel (visibles)
 *    │
 *    └─► ┌──────────────┐
 *        │ ▣  ▣  ▣  ▣   │         ← popup (ocultos), cuadrícula
 *        │ ▣  ▣         │
 *        └──────────────┘
 *
 * Un mismo TrayButton se mueve entre el panel y el popup según la
 * configuración; nunca se recrea al ocultarlo o mostrarlo.
 *
 * Este archivo: utilidades, flecha, distribución de los íconos, ítems del
 * modelo, señales del panel y construcción. El popup está en to-popup.c y
 * el arrastre en to-dnd.c.
 */
#include "to-common.h"
#include "to-plugin.h"
#include "to-popup.h"
#include "to-dnd.h"
#include "to-button.h"
#include "to-dialog.h"
#include "to-legacy.h"

#include <libxfce4util/libxfce4util.h>

#include <string.h>

static const gchar plugin_css[] =
  "." TO_CSS_PREFIX "-button {"
  "  padding: 1px; margin: 0; min-width: 0; min-height: 0; border-radius: 4px;"
  "}"
  "." TO_CSS_PREFIX "-arrow {"
  "  padding: 0 1px; margin: 0; min-width: 0; min-height: 0;"
  "}"
  "." TO_CSS_PREFIX "-popup {"
  "  background-color: @theme_bg_color;"
  "}"
  "." TO_CSS_PREFIX "-popup.rounded {"
  "  background-color: transparent;"
  "}"
  "." TO_CSS_PREFIX "-popup-frame {"
  "  background-color: @theme_bg_color;"
  "  border: 1px solid alpha(@theme_fg_color, 0.18);"
  "  padding: 6px;"
  "}"
  "." TO_CSS_PREFIX "-popup ." TO_CSS_PREFIX "-button {"
  "  padding: 4px;"
  "}"
  "." TO_CSS_PREFIX "-empty {"
  "  opacity: 0.7; padding: 8px 12px;"
  "}";



/* ════════════════════════════════════════════════════════════════════════
 * Utilidades
 * ════════════════════════════════════════════════════════════════════════ */

gboolean
tray_plugin_is_horizontal (TrayPlugin *p)
{
  return xfce_panel_plugin_get_mode (p->panel_plugin) == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL;
}



gboolean
tray_plugin_in_popup (TrayPlugin *p, GtkWidget *widget)
{
  return p->popup != NULL && gtk_widget_get_toplevel (widget) == p->popup;
}



gint
tray_plugin_get_panel_icon_size (TrayPlugin *p)
{
  gint size = tray_settings_get_panel_icon_size (p->settings);

  if (size > 0)
    return size;

  /* Automático: el mismo criterio que el resto de plugins del panel
   * (respeta el "tamaño de ícono" del panel y el número de filas) */
  return MAX (TRAY_ICON_SIZE_MIN, xfce_panel_plugin_get_icon_size (p->panel_plugin));
}



static gboolean
item_is_hidden (TrayPlugin *p, TrayItem *item)
{
  return tray_settings_is_hidden (p->settings, tray_item_get_key (item),
                                  tray_item_get_status (item) == TRAY_ITEM_STATUS_PASSIVE);
}



/* ════════════════════════════════════════════════════════════════════════
 * Flecha
 * ════════════════════════════════════════════════════════════════════════ */

static void
arrow_update_icon (TrayPlugin *p)
{
  const gchar *icon;

  switch (xfce_panel_plugin_arrow_type (p->panel_plugin))
    {
    case GTK_ARROW_DOWN:  icon = "pan-down-symbolic";  break;
    case GTK_ARROW_LEFT:  icon = "pan-start-symbolic"; break;
    case GTK_ARROW_RIGHT: icon = "pan-end-symbolic";   break;
    case GTK_ARROW_UP:
    default:              icon = "pan-up-symbolic";    break;
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (p->arrow_image), icon, GTK_ICON_SIZE_MENU);
  gtk_image_set_pixel_size (GTK_IMAGE (p->arrow_image), 16);
}



/* Punto de "requiere atención" sobre la flecha */
static gboolean
arrow_dot_draw (GtkWidget *widget,
                cairo_t   *cr,
                gpointer   user_data)
{
  GtkStyleContext *context = gtk_widget_get_style_context (widget);
  GdkRGBA color;
  gint w = gtk_widget_get_allocated_width (widget);
  gint h = gtk_widget_get_allocated_height (widget);

  if (!gtk_style_context_lookup_color (context, "theme_selected_bg_color", &color))
    gdk_rgba_parse (&color, "#e95420");

  gdk_cairo_set_source_rgba (cr, &color);
  cairo_arc (cr, w / 2.0, h / 2.0, MIN (w, h) / 2.0, 0, 2 * G_PI);
  cairo_fill (cr);

  return TRUE;
}



void
tray_plugin_arrow_set_active (TrayPlugin *p, gboolean active)
{
  p->updating_arrow = TRUE;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (p->arrow), active);
  p->updating_arrow = FALSE;

  /* Con el popup abierto, el tooltip de la flecha solo estorba */
  gtk_widget_set_has_tooltip (p->arrow, !active);
  if (active)
    gtk_tooltip_trigger_tooltip_query (gtk_widget_get_display (p->arrow));
}



static void
arrow_toggled (GtkToggleButton *button,
               TrayPlugin      *p)
{
  if (p->updating_arrow)
    return;

  if (p->suppress_toggle)
    {
      p->suppress_toggle = FALSE;
      tray_plugin_arrow_set_active (p, FALSE);
      return;
    }

  if (gtk_toggle_button_get_active (button))
    tray_popup_show (p);
  else
    tray_popup_hide (p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Distribución de íconos
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct
{
  TrayItem *item;
  gint      index;
  guint     arrival;
} SortEntry;



static gint
sort_entries (gconstpointer a, gconstpointer b)
{
  const SortEntry *ea = a, *eb = b;

  /* Desconocidos (índice -1) al final, en orden de llegada */
  if (ea->index != eb->index)
    {
      if (ea->index < 0)
        return 1;
      if (eb->index < 0)
        return -1;
      return ea->index - eb->index;
    }
  return (gint) ea->arrival - (gint) eb->arrival;
}



static void
clear_grid (GtkWidget *grid)
{
  GList *children, *l;

  children = gtk_container_get_children (GTK_CONTAINER (grid));
  for (l = children; l != NULL; l = l->next)
    if (TRAY_IS_BUTTON (l->data))
      {
        /* Un botón que cambia de lugar no recibe el "leave": limpiar el
         * resalte para que no quede marcado como si tuviera el ratón */
        gtk_widget_unset_state_flags (l->data, GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE);
        gtk_container_remove (GTK_CONTAINER (grid), l->data);
      }
  g_list_free (children);
}



static gboolean
same_order (GPtrArray *a, GPtrArray *b)
{
  guint i;

  if (a->len != b->len)
    return FALSE;

  for (i = 0; i < a->len; i++)
    if (g_ptr_array_index (a, i) != g_ptr_array_index (b, i))
      return FALSE;

  return TRUE;
}



void
tray_plugin_relayout (TrayPlugin *p)
{
  GPtrArray *items = tray_model_get_items (p->model);
  GPtrArray *new_panel, *new_popup;
  GArray *entries;
  TrayLayoutParams params;
  gboolean attention = FALSE;
  gboolean rebuild;
  guint i;

  if (p->relayout_id != 0)
    {
      g_source_remove (p->relayout_id);
      p->relayout_id = 0;
    }

  memset (&params, 0, sizeof (params));
  params.horizontal = tray_plugin_is_horizontal (p);
  params.nrows = MAX (1, (gint) xfce_panel_plugin_get_nrows (p->panel_plugin));
  params.panel_size = tray_plugin_get_panel_icon_size (p);
  params.popup_size = tray_settings_get_popup_icon_size (p->settings);
  params.columns = tray_settings_get_popup_columns (p->settings);
  params.panel_spacing = tray_settings_get_panel_spacing (p->settings);
  params.popup_spacing = tray_settings_get_popup_spacing (p->settings);

  /* Ordenar los ítems listos según la configuración */
  entries = g_array_new (FALSE, FALSE, sizeof (SortEntry));
  for (i = 0; i < items->len; i++)
    {
      TrayItem *item = g_ptr_array_index (items, i);
      SortEntry entry;

      if (tray_item_get_key (item) == NULL || !g_hash_table_contains (p->buttons, item))
        continue;

      entry.item = item;
      entry.index = tray_settings_get_index (p->settings, tray_item_get_key (item));
      entry.arrival = i;
      g_array_append_val (entries, entry);
    }
  g_array_sort (entries, sort_entries);

  /* Calcular la distribución nueva sin tocar todavía los widgets */
  new_panel = g_ptr_array_new ();
  new_popup = g_ptr_array_new ();

  for (i = 0; i < entries->len; i++)
    {
      SortEntry *entry = &g_array_index (entries, SortEntry, i);
      GtkWidget *button = g_hash_table_lookup (p->buttons, entry->item);

      if (item_is_hidden (p, entry->item))
        {
          g_ptr_array_add (new_popup, button);
          if (tray_item_get_status (entry->item) == TRAY_ITEM_STATUS_ATTENTION)
            attention = TRUE;
        }
      else
        {
          g_ptr_array_add (new_panel, button);
        }
    }
  g_array_unref (entries);

  params.drag_empty = p->dragging && new_panel->len == 0;

  /* Si nada cambió (caso típico: una app actualizó su ícono o su
   * título), no se reconstruyen las cuadrículas: el botón ya se
   * redibujó solo. Se actualiza solo la flecha. */
  rebuild = !p->layout_valid
            || memcmp (&params, &p->layout_params, sizeof (params)) != 0
            || !same_order (new_panel, p->panel_order)
            || !same_order (new_popup, p->popup_order);

  if (rebuild)
    {
      clear_grid (p->panel_grid);
      clear_grid (p->popup_grid);

      g_ptr_array_unref (p->panel_order);
      g_ptr_array_unref (p->popup_order);
      p->panel_order = g_steal_pointer (&new_panel);
      p->popup_order = g_steal_pointer (&new_popup);
      tray_popup_clear_selection (p);   /* los íconos cambiaron de lugar */

      /* Panel: horizontal se llena por columnas de @nrows filas;
       * vertical/deskbar, por filas de @nrows columnas */
      for (i = 0; i < p->panel_order->len; i++)
        {
          GtkWidget *button = g_ptr_array_index (p->panel_order, i);
          gint major = i / params.nrows, minor = i % params.nrows;

          tray_button_set_icon_size (TRAY_BUTTON (button), params.panel_size);
          if (params.horizontal)
            gtk_grid_attach (GTK_GRID (p->panel_grid), button, major, minor, 1, 1);
          else
            gtk_grid_attach (GTK_GRID (p->panel_grid), button, minor, major, 1, 1);
          gtk_widget_show (button);
        }

      gtk_grid_set_row_spacing (GTK_GRID (p->panel_grid), params.panel_spacing);
      gtk_grid_set_column_spacing (GTK_GRID (p->panel_grid), params.panel_spacing);
      gtk_box_set_spacing (GTK_BOX (p->box), params.panel_spacing);

      /* Con el panel vacío durante un arrastre, dejar una zona para soltar */
      if (params.drag_empty)
        gtk_widget_set_size_request (p->panel_grid, params.panel_size + 2,
                                     params.panel_size + 2);
      else
        gtk_widget_set_size_request (p->panel_grid, -1, -1);

      /* Popup: cuadrícula por filas */
      for (i = 0; i < p->popup_order->len; i++)
        {
          GtkWidget *button = g_ptr_array_index (p->popup_order, i);

          tray_button_set_icon_size (TRAY_BUTTON (button), params.popup_size);
          gtk_grid_attach (GTK_GRID (p->popup_grid), button,
                           i % params.columns, i / params.columns, 1, 1);
          gtk_widget_show (button);
        }

      gtk_grid_set_row_spacing (GTK_GRID (p->popup_grid), params.popup_spacing);
      gtk_grid_set_column_spacing (GTK_GRID (p->popup_grid), params.popup_spacing);
      gtk_widget_set_visible (p->popup_empty, p->popup_order->len == 0);
      gtk_widget_set_visible (p->popup_grid, p->popup_order->len > 0);

      gtk_orientable_set_orientation (GTK_ORIENTABLE (p->box),
                                      params.horizontal ? GTK_ORIENTATION_HORIZONTAL
                                                        : GTK_ORIENTATION_VERTICAL);
      arrow_update_icon (p);

      p->layout_params = params;
      p->layout_valid = TRUE;
    }
  else
    {
      g_ptr_array_unref (new_panel);
      g_ptr_array_unref (new_popup);
    }

  /* Flecha: solo si hay ocultos, o durante un arrastre (como destino) */
  gtk_widget_set_visible (p->arrow, p->popup_order->len > 0 || p->dragging);
  gtk_widget_set_visible (p->arrow_dot, attention);

  if (p->popup != NULL && gtk_widget_get_visible (p->popup))
    {
      if (p->popup_order->len == 0 && !p->dragging)
        tray_popup_hide (p);
      else if (rebuild)
        tray_popup_reposition (p);
    }
}



static gboolean
relayout_idle (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->relayout_id = 0;
  tray_plugin_relayout (p);
  return G_SOURCE_REMOVE;
}



void
tray_plugin_schedule_relayout (TrayPlugin *p)
{
  if (p->relayout_id == 0)
    p->relayout_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE + 10, relayout_idle, p, NULL);
}



/* ════════════════════════════════════════════════════════════════════════
 * Ítems del modelo
 * ════════════════════════════════════════════════════════════════════════ */

/* Nombre de ícono para el registro de apps conocidas. Si la app solo
 * envía píxeles (Electron, muchas apps Qt), guardamos una copia en la
 * caché para poder mostrarla en el diálogo aunque la app esté cerrada. */
static gchar *
item_icon_for_registry (TrayPlugin *p,
                        TrayItem   *item,
                        gboolean    refresh_cache)
{
  const gchar *name = tray_item_get_icon_name (item);
  gchar *checksum, *dir, *path;
  GdkPixbuf *pixbuf;

  if (name != NULL && *name != '\0')
    return g_strdup (name);

  checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, tray_item_get_key (item), -1);
  dir = g_build_filename (g_get_user_cache_dir (), "xfce4", TO_CSS_PREFIX, NULL);
  path = g_strdup_printf ("%s/%s.png", dir, checksum);
  g_free (checksum);

  if (refresh_cache || !g_file_test (path, G_FILE_TEST_EXISTS))
    {
      pixbuf = tray_item_render_icon (item, 32, 1, NULL, FALSE);
      if (pixbuf != NULL)
        {
          if (g_mkdir_with_parents (dir, 0700) != 0
              || !gdk_pixbuf_save (pixbuf, path, "png", NULL, NULL))
            g_clear_pointer (&path, g_free);
          g_object_unref (pixbuf);
        }
      else
        {
          g_clear_pointer (&path, g_free);
        }
    }

  g_free (dir);
  return path;
}



static void
remember_item (TrayPlugin *p,
               TrayItem   *item,
               gboolean    refresh_cache)
{
  gchar *icon = item_icon_for_registry (p, item, refresh_cache);

  tray_settings_remember (p->settings, tray_item_get_key (item),
                          tray_item_get_display_name (item), icon);
  g_free (icon);
}



static void
item_changed (TrayItem   *item,
              TrayPlugin *p)
{
  /* Mantener al día título e ícono del registro (para el diálogo).
   * La caché de íconos solo se renueva al aparecer la app, no en cada
   * cambio (algunas apps cambian el ícono a menudo). */
  remember_item (p, item, FALSE);

  /* Un cambio de estado puede moverlo entre panel y popup */
  tray_plugin_schedule_relayout (p);
}



/* Íconos elegidos por el usuario (configuración de esta instancia) */
static void
apply_icon_overrides (TrayPlugin *p)
{
  GHashTableIter iter;
  gpointer key, value;
  gboolean symbolic = tray_settings_get_symbolic_icons (p->settings);

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      tray_button_set_icon_override (TRAY_BUTTON (value),
                                     tray_settings_get_icon_override (p->settings,
                                                                      tray_item_get_key (key)));
      tray_button_set_symbolic (TRAY_BUTTON (value), symbolic);
    }
}



static void
button_value_free (gpointer data)
{
  GtkWidget *button = data;

  gtk_widget_destroy (button);
  g_object_unref (button);
}



static void
model_item_added (TrayModel  *model,
                  TrayItem   *item,
                  TrayPlugin *p)
{
  GtkWidget *button;

  if (g_hash_table_contains (p->buttons, item))
    return;

  button = tray_button_new (item);
  g_object_ref_sink (button);
  g_hash_table_insert (p->buttons, item, button);
  tray_button_set_icon_override (TRAY_BUTTON (button),
                                 tray_settings_get_icon_override (p->settings,
                                                                  tray_item_get_key (item)));
  tray_button_set_symbolic (TRAY_BUTTON (button),
                            tray_settings_get_symbolic_icons (p->settings));

  tray_popup_setup_button (p, button);
  tray_dnd_setup_button (p, button);

  g_signal_connect (item, "changed", G_CALLBACK (item_changed), p);

  remember_item (p, item, TRUE);

  tray_plugin_schedule_relayout (p);
}



static void
model_item_removed (TrayModel  *model,
                    TrayItem   *item,
                    TrayPlugin *p)
{
  GtkWidget *button = g_hash_table_lookup (p->buttons, item);

  g_signal_handlers_disconnect_by_data (item, p);

  if (button == NULL)
    return;

  if (p->drag_item == item)
    p->drag_item = NULL;

  g_ptr_array_remove (p->panel_order, button);
  g_ptr_array_remove (p->popup_order, button);
  g_hash_table_remove (p->buttons, item);

  /* El botón destruido deja un hueco en la cuadrícula: rearmarla */
  p->layout_valid = FALSE;

  tray_plugin_schedule_relayout (p);
}



static void
refresh_all_icons (TrayPlugin *p)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    tray_button_update (TRAY_BUTTON (value));
}



/* La flecha antes o después de los íconos visibles (en un GtkBox
 * vertical, "antes" es arriba) */
static void
arrow_apply_position (TrayPlugin *p)
{
  gint position = tray_settings_get_arrow_position (p->settings) == TRAY_ARROW_END ? 1 : 0;
  gint current;

  gtk_container_child_get (GTK_CONTAINER (p->box), p->arrow, "position", &current, NULL);
  if (current != position)
    gtk_box_reorder_child (GTK_BOX (p->box), p->arrow, position);
}



/* ════════════════════════════════════════════════════════════════════════
 * Señales del panel
 * ════════════════════════════════════════════════════════════════════════ */

static gboolean
plugin_size_changed (XfcePanelPlugin *panel_plugin,
                     gint             size,
                     TrayPlugin      *p)
{
  tray_plugin_relayout (p);
  return TRUE;
}



static void
plugin_mode_changed (XfcePanelPlugin     *panel_plugin,
                     XfcePanelPluginMode  mode,
                     TrayPlugin          *p)
{
  tray_plugin_relayout (p);
}



static void
plugin_nrows_changed (XfcePanelPlugin *panel_plugin,
                      guint            rows,
                      TrayPlugin      *p)
{
  tray_plugin_relayout (p);
}



static void
plugin_screen_position_changed (XfcePanelPlugin    *panel_plugin,
                                XfceScreenPosition  position,
                                TrayPlugin         *p)
{
  tray_popup_hide (p);
  arrow_update_icon (p);
}



static void
plugin_configure (XfcePanelPlugin *panel_plugin,
                  TrayPlugin      *p)
{
  tray_popup_hide (p);
  tray_dialog_show (p);
}



static void
plugin_about (XfcePanelPlugin *panel_plugin,
              TrayPlugin      *p)
{
  static const gchar *authors[] = { "Juan Pérez <juanchiperez97@gmail.com>", NULL };

  gtk_show_about_dialog (NULL,
                         "program-name", _("Tray Overflow"),
                         "logo-icon-name", "org.xfce.panel.systray",
                         "version", PACKAGE_VERSION,
                         "comments", _("Status notifier area with a popup for hidden icons"),
                         "authors", authors,
                         "license-type", GTK_LICENSE_GPL_2_0,
                         NULL);
}



/* Atajo de teclado: xfce4-panel --plugin-event=trayoverflow:popup
 * (abre o cierra), o con :bool:true / :bool:false para solo abrir o
 * cerrar. Con varias instancias, responde la primera que tenga íconos
 * ocultos (devolver TRUE corta el aviso a las demás). */
static gboolean
plugin_remote_event (XfcePanelPlugin *panel_plugin,
                     const gchar     *name,
                     const GValue    *value,
                     TrayPlugin      *p)
{
  gboolean open;

  if (g_strcmp0 (name, "popup") != 0)
    return FALSE;

  if (value != NULL && G_VALUE_HOLDS_BOOLEAN (value))
    open = g_value_get_boolean (value);
  else
    open = p->popup == NULL || !gtk_widget_get_visible (p->popup);

  return tray_popup_toggle_from_keyboard (p, open);
}



static void
apply_legacy (TrayPlugin *p)
{
  tray_legacy_set_wanted (p, tray_settings_get_legacy_tray (p->settings));
}



static void
plugin_free (XfcePanelPlugin *panel_plugin,
             TrayPlugin      *p)
{
  GHashTableIter iter;
  gpointer key;

  if (p->dialog != NULL)
    gtk_widget_destroy (p->dialog);

  tray_legacy_set_wanted (p, FALSE);

  tray_popup_hide (p);

  if (p->relayout_id != 0)
    g_source_remove (p->relayout_id);
  if (p->grab_check_id != 0)
    g_source_remove (p->grab_check_id);
  if (p->suppress_id != 0)
    g_source_remove (p->suppress_id);
  g_clear_handle_id (&p->corners_idle_id, g_source_remove);
  g_idle_remove_by_data (p);

  tray_popup_menu_hook_remove (p);
  if (p->menu_shown != NULL)
    {
      g_clear_signal_handler (&p->menu_deactivate_id, p->menu_shown);
      g_clear_object (&p->menu_shown);
    }

  g_signal_handlers_disconnect_by_data (p->model, p);
  g_signal_handlers_disconnect_by_data (p->settings, p);
  g_clear_signal_handler (&p->theme_changed_id, gtk_icon_theme_get_default ());

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_signal_handlers_disconnect_by_data (key, p);
  g_hash_table_unref (p->buttons);
  g_ptr_array_unref (p->panel_order);
  g_ptr_array_unref (p->popup_order);

  if (p->popup != NULL)
    {
      GdkScreen *screen = gtk_widget_get_screen (p->popup);

      g_clear_signal_handler (&p->composited_id, screen);
      g_clear_signal_handler (&p->theme_name_id, gtk_settings_get_for_screen (screen));
      gtk_widget_destroy (p->popup);
    }
  g_clear_object (&p->corners_css);

  g_object_unref (p->model);
  g_object_unref (p->settings);

  gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                GTK_STYLE_PROVIDER (p->css));
  g_object_unref (p->css);

  g_free (p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Construcción
 * ════════════════════════════════════════════════════════════════════════ */

static void
tray_plugin_construct (XfcePanelPlugin *panel_plugin)
{
  TrayPlugin *p;
  GtkWidget *overlay;
  GPtrArray *items;
  guint i;

  /* Solo enlazar nuestro dominio: textdomain() (que llama
   * xfce_textdomain) cambiaría el dominio por defecto de todo el panel */
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  p = g_new0 (TrayPlugin, 1);
  p->panel_plugin = panel_plugin;
  p->settings = tray_settings_new (panel_plugin);
  p->buttons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, button_value_free);
  p->panel_order = g_ptr_array_new ();
  p->popup_order = g_ptr_array_new ();

  p->css = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (p->css, plugin_css, -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (p->css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  xfce_panel_plugin_set_small (panel_plugin, FALSE);

  /* Contenedor: [flecha][cuadrícula de íconos] */
  p->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (panel_plugin), p->box);
  xfce_panel_plugin_add_action_widget (panel_plugin, p->box);
  tray_dnd_setup_drop_target (p, p->box);

  p->arrow = gtk_toggle_button_new ();
  gtk_button_set_relief (GTK_BUTTON (p->arrow), GTK_RELIEF_NONE);
  gtk_widget_set_can_focus (p->arrow, FALSE);
  gtk_widget_set_tooltip_text (p->arrow, _("Show hidden icons"));
  gtk_style_context_add_class (gtk_widget_get_style_context (p->arrow),
                               TO_CSS_PREFIX "-arrow");
  gtk_box_pack_start (GTK_BOX (p->box), p->arrow, FALSE, FALSE, 0);
  xfce_panel_plugin_add_action_widget (panel_plugin, p->arrow);
  g_signal_connect (p->arrow, "toggled", G_CALLBACK (arrow_toggled), p);

  overlay = gtk_overlay_new ();
  gtk_container_add (GTK_CONTAINER (p->arrow), overlay);
  p->arrow_image = gtk_image_new ();
  gtk_container_add (GTK_CONTAINER (overlay), p->arrow_image);
  p->arrow_dot = gtk_drawing_area_new ();
  gtk_widget_set_size_request (p->arrow_dot, 6, 6);
  gtk_widget_set_halign (p->arrow_dot, GTK_ALIGN_END);
  gtk_widget_set_valign (p->arrow_dot, GTK_ALIGN_START);
  gtk_widget_set_no_show_all (p->arrow_dot, TRUE);
  g_signal_connect (p->arrow_dot, "draw", G_CALLBACK (arrow_dot_draw), NULL);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->arrow_dot);
  gtk_widget_show_all (overlay);
  gtk_widget_set_no_show_all (p->arrow, TRUE);

  /* La flecha es destino: soltar ahí oculta; esperar encima abre el popup */
  tray_dnd_setup_arrow (p);

  p->panel_grid = gtk_grid_new ();
  gtk_widget_set_halign (p->panel_grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->panel_grid, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (p->box), p->panel_grid, TRUE, TRUE, 0);
  arrow_apply_position (p);

  gtk_widget_show (p->box);
  gtk_widget_show (p->panel_grid);

  tray_popup_create (p);

  xfce_panel_plugin_menu_show_configure (panel_plugin);
  xfce_panel_plugin_menu_show_about (panel_plugin);

  g_signal_connect (panel_plugin, "free-data", G_CALLBACK (plugin_free), p);
  /* El panel pide guardar (p. ej. al cerrar sesión): escribir lo pendiente */
  g_signal_connect_swapped (panel_plugin, "save", G_CALLBACK (tray_settings_flush), p->settings);
  /* Quitado del panel: su configuración se borra, no volver a escribirla */
  g_signal_connect_swapped (panel_plugin, "removed", G_CALLBACK (tray_settings_discard), p->settings);
  g_signal_connect (panel_plugin, "size-changed", G_CALLBACK (plugin_size_changed), p);
  g_signal_connect (panel_plugin, "mode-changed", G_CALLBACK (plugin_mode_changed), p);
  g_signal_connect (panel_plugin, "nrows-changed", G_CALLBACK (plugin_nrows_changed), p);
  g_signal_connect (panel_plugin, "screen-position-changed",
                    G_CALLBACK (plugin_screen_position_changed), p);
  g_signal_connect (panel_plugin, "configure-plugin", G_CALLBACK (plugin_configure), p);
  g_signal_connect (panel_plugin, "about", G_CALLBACK (plugin_about), p);
  g_signal_connect (panel_plugin, "remote-event", G_CALLBACK (plugin_remote_event), p);
  g_signal_connect_swapped (panel_plugin, "notify::scale-factor",
                            G_CALLBACK (refresh_all_icons), p);
  /* Cambió el "tamaño de ícono" global del panel (modo automático) */
  g_signal_connect_swapped (panel_plugin, "notify::icon-size",
                            G_CALLBACK (tray_plugin_schedule_relayout), p);

  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (tray_plugin_schedule_relayout), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (apply_icon_overrides), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (tray_popup_apply_corners), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (arrow_apply_position), p);

  /* Apps antiguas (XEmbed): arrancar o detener el proxy según la opción */
  g_signal_connect_swapped (p->settings, "changed", G_CALLBACK (apply_legacy), p);
  apply_legacy (p);

  p->theme_changed_id = g_signal_connect_swapped (gtk_icon_theme_get_default (), "changed",
                                                  G_CALLBACK (refresh_all_icons), p);

  /* Modelo compartido: tomar los ítems que ya existan */
  p->model = tray_model_ref_default ();
  g_signal_connect (p->model, "item-added", G_CALLBACK (model_item_added), p);
  g_signal_connect (p->model, "item-removed", G_CALLBACK (model_item_removed), p);

  items = tray_model_get_items (p->model);
  for (i = 0; i < items->len; i++)
    model_item_added (p->model, g_ptr_array_index (items, i), p);

  tray_plugin_relayout (p);
}

XFCE_PANEL_PLUGIN_REGISTER (tray_plugin_construct);
