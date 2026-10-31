/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Diálogo de configuración:
 *  - Apariencia: tamaños, separación y columnas (se aplican en vivo).
 *  - Ítems: ocultar/mostrar, ordenar y olvidar apps, incluidas las que
 *    no están abiertas; aviso sobre apps antiguas (XEmbed).
 */
#include "to-common.h"
#include "to-dialog.h"

#include <gdk/gdkx.h>
#include <libxfce4ui/libxfce4ui.h>
#include <string.h>

#include "to-legacy.h"

#define LIST_ICON_SIZE    24
#define CHOOSER_ICON_SIZE 32
#define PREVIEW_ICON_SIZE 48
#define MAX_SUGGESTIONS   30
#define RESPONSE_APP_ICON 1

enum
{
  COL_KEY,
  COL_PIXBUF,
  COL_MARKUP,
  COL_HIDDEN,
  COL_RUNNING,
  N_COLUMNS
};

typedef struct
{
  TrayPlugin   *p;
  GtkWidget    *dialog;

  GtkWidget    *panel_auto;
  GtkWidget    *panel_size;
  GtkWidget    *panel_spacing;
  GtkWidget    *arrow_position;
  GtkWidget    *popup_size;
  GtkWidget    *popup_spacing;
  GtkWidget    *popup_columns;
  GtkWidget    *popup_corners;
  GtkWidget    *passive;
  GtkWidget    *new_in_popup;
  GtkWidget    *symbolic;

  GtkListStore *store;
  GtkWidget    *tree;
  GtkWidget    *up_button;
  GtkWidget    *down_button;
  GtkWidget    *forget_button;
  GtkWidget    *icon_button;

  GtkWidget    *legacy_check;
  GtkWidget    *legacy_image;
  GtkWidget    *legacy_label;
  guint         legacy_refresh_id;

  gboolean      rebuilding;
  gboolean      syncing;    /* actualizando los controles desde la configuración */
} DialogData;



/* ── Pestaña Apariencia ──────────────────────────────────────────────── */

static void
panel_auto_toggled (GtkToggleButton *toggle,
                    DialogData      *d)
{
  gboolean automatic = gtk_toggle_button_get_active (toggle);

  gtk_widget_set_sensitive (d->panel_size, !automatic);

  if (d->syncing)
    return;

  if (automatic)
    {
      tray_settings_set_panel_icon_size (d->p->settings, 0);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->panel_size),
                                 tray_plugin_get_panel_icon_size (d->p));
    }
  else
    {
      tray_settings_set_panel_icon_size (d->p->settings,
        gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->panel_size)));
    }
}



static void
spin_changed (GtkSpinButton *spin,
              DialogData    *d)
{
  gint value = gtk_spin_button_get_value_as_int (spin);
  TraySettings *s = d->p->settings;

  if (d->syncing)
    return;

  if (GTK_WIDGET (spin) == d->panel_size)
    {
      if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (d->panel_auto)))
        tray_settings_set_panel_icon_size (s, value);
    }
  else if (GTK_WIDGET (spin) == d->panel_spacing)
    tray_settings_set_panel_spacing (s, value);
  else if (GTK_WIDGET (spin) == d->popup_size)
    tray_settings_set_popup_icon_size (s, value);
  else if (GTK_WIDGET (spin) == d->popup_spacing)
    tray_settings_set_popup_spacing (s, value);
  else if (GTK_WIDGET (spin) == d->popup_columns)
    tray_settings_set_popup_columns (s, value);
}



static void
corners_changed (GtkComboBox *combo,
                 DialogData  *d)
{
  if (!d->syncing)
    tray_settings_set_popup_corners (d->p->settings, gtk_combo_box_get_active (combo));
}



static void
arrow_position_changed (GtkComboBox *combo,
                        DialogData  *d)
{
  if (!d->syncing)
    tray_settings_set_arrow_position (d->p->settings, gtk_combo_box_get_active (combo));
}



static void
passive_toggled (GtkToggleButton *toggle,
                 DialogData      *d)
{
  if (!d->syncing)
    tray_settings_set_passive_in_popup (d->p->settings, gtk_toggle_button_get_active (toggle));
}



static void
symbolic_toggled (GtkToggleButton *toggle,
                  DialogData      *d)
{
  if (!d->syncing)
    tray_settings_set_symbolic_icons (d->p->settings, gtk_toggle_button_get_active (toggle));
}



static void
new_in_popup_toggled (GtkToggleButton *toggle,
                      DialogData      *d)
{
  if (!d->syncing)
    tray_settings_set_new_in_popup (d->p->settings, gtk_toggle_button_get_active (toggle));
}



static GtkWidget *
make_spin (DialogData *d, gint min, gint max, gint value)
{
  GtkWidget *spin = gtk_spin_button_new_with_range (min, max, 1);

  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), value);
  gtk_widget_set_halign (spin, GTK_ALIGN_START);
  g_signal_connect (spin, "value-changed", G_CALLBACK (spin_changed), d);
  return spin;
}



static void
add_section (GtkWidget *grid, gint *row, const gchar *title)
{
  GtkWidget *label = gtk_label_new (NULL);
  gchar *markup = g_markup_printf_escaped ("<b>%s</b>", title);

  gtk_label_set_markup (GTK_LABEL (label), markup);
  g_free (markup);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  if (*row > 0)
    gtk_widget_set_margin_top (label, 12);
  gtk_grid_attach (GTK_GRID (grid), label, 0, (*row)++, 3, 1);
}



static void
add_row (GtkWidget *grid, gint *row, const gchar *text, GtkWidget *control, GtkWidget *extra)
{
  GtkWidget *label = gtk_label_new_with_mnemonic (text);

  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_margin_start (label, 12);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), control);
  gtk_grid_attach (GTK_GRID (grid), label, 0, *row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), control, 1, *row, 1, 1);
  if (extra != NULL)
    gtk_grid_attach (GTK_GRID (grid), extra, 2, *row, 1, 1);
  (*row)++;
}



static GtkWidget *
appearance_page (DialogData *d)
{
  TraySettings *s = d->p->settings;
  GtkWidget *grid, *hint;
  gboolean automatic = tray_settings_get_panel_icon_size (s) == 0;
  gint row = 0;

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 12);

  add_section (grid, &row, _("Panel"));

  d->panel_size = make_spin (d, TRAY_ICON_SIZE_MIN, TRAY_ICON_SIZE_MAX,
                             tray_plugin_get_panel_icon_size (d->p));
  gtk_widget_set_sensitive (d->panel_size, !automatic);
  d->panel_auto = gtk_check_button_new_with_mnemonic (_("_Automatic"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->panel_auto), automatic);
  g_signal_connect (d->panel_auto, "toggled", G_CALLBACK (panel_auto_toggled), d);
  add_row (grid, &row, _("Icon _size (px):"), d->panel_size, d->panel_auto);

  d->panel_spacing = make_spin (d, 0, TRAY_SPACING_MAX, tray_settings_get_panel_spacing (s));
  add_row (grid, &row, _("Icon s_pacing (px):"), d->panel_spacing, NULL);

  /* También vale para el popup, como en la bandeja de XFCE */
  d->symbolic = gtk_check_button_new_with_mnemonic (_("Use sym_bolic icons (if available)"));
  gtk_widget_set_tooltip_text (d->symbolic,
                               _("Monochrome icons in the color of the panel text, in the "
                                 "panel and in the popup. Only for apps whose icon comes "
                                 "from the icon theme; the rest keep their own icon."));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->symbolic),
                                tray_settings_get_symbolic_icons (s));
  g_signal_connect (d->symbolic, "toggled", G_CALLBACK (symbolic_toggled), d);
  gtk_widget_set_margin_start (d->symbolic, 12);
  gtk_grid_attach (GTK_GRID (grid), d->symbolic, 0, row++, 3, 1);

  /* Mismo orden que TrayArrowPosition */
  d->arrow_position = gtk_combo_box_text_new ();
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (d->arrow_position), _("Left"));
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (d->arrow_position), _("Right"));
  gtk_combo_box_set_active (GTK_COMBO_BOX (d->arrow_position),
                            tray_settings_get_arrow_position (s));
  g_signal_connect (d->arrow_position, "changed", G_CALLBACK (arrow_position_changed), d);
  add_row (grid, &row, _("Arro_w position:"), d->arrow_position, NULL);

  hint = gtk_label_new (_("Relative to the visible icons. In vertical panels, "
                          "left means top and right means bottom."));
  gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 50);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 12);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_grid_attach (GTK_GRID (grid), hint, 0, row++, 3, 1);

  add_section (grid, &row, _("Popup"));

  d->popup_size = make_spin (d, TRAY_ICON_SIZE_MIN, TRAY_ICON_SIZE_MAX,
                             tray_settings_get_popup_icon_size (s));
  add_row (grid, &row, _("Icon si_ze (px):"), d->popup_size, NULL);

  d->popup_spacing = make_spin (d, 0, TRAY_SPACING_MAX, tray_settings_get_popup_spacing (s));
  add_row (grid, &row, _("Icon spa_cing (px):"), d->popup_spacing, NULL);

  d->popup_columns = make_spin (d, 1, TRAY_COLUMNS_MAX, tray_settings_get_popup_columns (s));
  add_row (grid, &row, _("C_olumns:"), d->popup_columns, NULL);

  /* Mismo orden que TrayCorners */
  d->popup_corners = gtk_combo_box_text_new ();
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (d->popup_corners), _("From theme"));
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (d->popup_corners), _("Rounded"));
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (d->popup_corners), _("Square"));
  gtk_combo_box_set_active (GTK_COMBO_BOX (d->popup_corners),
                            tray_settings_get_popup_corners (s));
  g_signal_connect (d->popup_corners, "changed", G_CALLBACK (corners_changed), d);
  add_row (grid, &row, _("Co_rners:"), d->popup_corners, NULL);

  hint = gtk_label_new (_("Without a compositor, corners are always square."));
  gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 50);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 12);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_grid_attach (GTK_GRID (grid), hint, 0, row++, 3, 1);

  add_section (grid, &row, _("Behavior"));

  d->passive = gtk_check_button_new_with_mnemonic (_("Move _inactive icons to the popup"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->passive),
                                tray_settings_get_passive_in_popup (s));
  g_signal_connect (d->passive, "toggled", G_CALLBACK (passive_toggled), d);
  gtk_widget_set_margin_start (d->passive, 12);
  gtk_grid_attach (GTK_GRID (grid), d->passive, 0, row++, 3, 1);

  hint = gtk_label_new (_("Some apps mark their icon as inactive when there is nothing "
                          "to report. Icons you place in the panel yourself stay there."));
  gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 50);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_set_margin_start (hint, 36);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_grid_attach (GTK_GRID (grid), hint, 0, row++, 3, 1);

  return grid;
}



/* ── Pestaña Ítems ───────────────────────────────────────────────────── */

static GHashTable *
running_items (DialogData *d)
{
  GHashTable *running = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *items = tray_model_get_items (d->p->model);
  guint i;

  for (i = 0; i < items->len; i++)
    {
      TrayItem *item = g_ptr_array_index (items, i);
      if (tray_item_get_key (item) != NULL)
        g_hash_table_insert (running, (gpointer) tray_item_get_key (item), item);
    }

  return running;
}



static gchar *
selected_key (DialogData *d)
{
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  gchar *key = NULL;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (d->tree));
  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    gtk_tree_model_get (model, &iter, COL_KEY, &key, -1);

  return key;
}



static void
update_buttons (DialogData *d)
{
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  gboolean has = FALSE, running = FALSE, first = FALSE, last = FALSE;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (d->tree));
  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
      GtkTreeIter next = iter;

      has = TRUE;
      gtk_tree_model_get (model, &iter, COL_RUNNING, &running, -1);
      first = gtk_tree_path_get_indices (path)[0] == 0;
      last = !gtk_tree_model_iter_next (model, &next);
      gtk_tree_path_free (path);
    }

  gtk_widget_set_sensitive (d->icon_button, has);
  gtk_widget_set_sensitive (d->up_button, has && !first);
  gtk_widget_set_sensitive (d->down_button, has && !last);
  /* Una app abierta se volvería a registrar al instante */
  gtk_widget_set_sensitive (d->forget_button, has && !running);
}



static void
rebuild_list (DialogData *d)
{
  GPtrArray *known = tray_settings_get_known (d->p->settings);
  GHashTable *running = running_items (d);
  GtkStyleContext *context = gtk_widget_get_style_context (d->tree);
  gchar *keep = selected_key (d);
  guint i;

  d->rebuilding = TRUE;
  gtk_list_store_clear (d->store);

  for (i = 0; i < known->len; i++)
    {
      TrayKnown *k = g_ptr_array_index (known, i);
      TrayItem *item = g_hash_table_lookup (running, k->key);
      const gchar *custom = tray_settings_get_icon_override (d->p->settings, k->key);
      GdkPixbuf *pixbuf = tray_icon_load (custom, LIST_ICON_SIZE, 1, context);
      const gchar *title;
      gchar *markup;
      gboolean hidden;
      GtkTreeIter iter;

      if (item != NULL)
        {
          if (pixbuf == NULL)
            pixbuf = tray_item_render_icon (item, LIST_ICON_SIZE, 1, context, FALSE);
          title = tray_item_get_display_name (item);
          hidden = tray_settings_is_hidden (d->p->settings, k->key,
                                            tray_item_get_status (item) == TRAY_ITEM_STATUS_PASSIVE);
        }
      else
        {
          GtkIconTheme *theme = gtk_icon_theme_get_default ();
          const gchar *icon = (k->icon_name != NULL && *k->icon_name != '\0')
                              ? k->icon_name : "application-x-executable";

          /* Ícono guardado en la caché (apps que solo envían píxeles) */
          if (pixbuf != NULL)
            ;
          else if (g_path_is_absolute (icon))
            pixbuf = gdk_pixbuf_new_from_file_at_size (icon, LIST_ICON_SIZE,
                                                       LIST_ICON_SIZE, NULL);
          else
            pixbuf = gtk_icon_theme_load_icon (theme, icon, LIST_ICON_SIZE,
                                               GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
          if (pixbuf == NULL)
            pixbuf = gtk_icon_theme_load_icon (theme, "application-x-executable",
                                               LIST_ICON_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
          title = (k->title != NULL && *k->title != '\0') ? k->title : k->key;
          hidden = tray_settings_is_hidden (d->p->settings, k->key, FALSE);
        }

      if (item != NULL)
        markup = g_markup_printf_escaped ("%s", title);
      else
        markup = g_markup_printf_escaped ("%s <span alpha=\"60%%\"><small>%s</small></span>",
                                          title, _("(not running)"));

      gtk_list_store_insert_with_values (d->store, &iter, -1,
                                         COL_KEY, k->key,
                                         COL_PIXBUF, pixbuf,
                                         COL_MARKUP, markup,
                                         COL_HIDDEN, hidden,
                                         COL_RUNNING, item != NULL,
                                         -1);

      if (keep != NULL && g_strcmp0 (keep, k->key) == 0)
        gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (d->tree)),
                                        &iter);

      g_free (markup);
      if (pixbuf != NULL)
        g_object_unref (pixbuf);
    }

  g_free (keep);
  g_hash_table_unref (running);
  d->rebuilding = FALSE;
  update_buttons (d);
}



static void
hidden_toggled (GtkCellRendererToggle *renderer,
                gchar                 *path_str,
                DialogData            *d)
{
  GtkTreeIter iter;
  gchar *key = NULL;
  gboolean hidden;

  if (!gtk_tree_model_get_iter_from_string (GTK_TREE_MODEL (d->store), &iter, path_str))
    return;

  gtk_tree_model_get (GTK_TREE_MODEL (d->store), &iter, COL_KEY, &key, COL_HIDDEN, &hidden, -1);
  tray_settings_set_hidden (d->p->settings, key, !hidden);
  g_free (key);
}



static void
move_selected (DialogData *d, gboolean up)
{
  GPtrArray *known = tray_settings_get_known (d->p->settings);
  gchar *key = selected_key (d);
  gint index;

  if (key == NULL)
    return;

  index = tray_settings_get_index (d->p->settings, key);
  if (up && index > 0)
    {
      TrayKnown *prev = g_ptr_array_index (known, index - 1);
      tray_settings_move (d->p->settings, key, prev->key, FALSE);
    }
  else if (!up && index >= 0 && (guint) index + 1 < known->len)
    {
      TrayKnown *next = g_ptr_array_index (known, index + 1);
      tray_settings_move (d->p->settings, key, next->key, TRUE);
    }

  g_free (key);
}



static void
up_clicked (GtkButton *button, DialogData *d)
{
  move_selected (d, TRUE);
}



static void
down_clicked (GtkButton *button, DialogData *d)
{
  move_selected (d, FALSE);
}



static void
forget_clicked (GtkButton *button, DialogData *d)
{
  gchar *key = selected_key (d);

  if (key != NULL)
    tray_settings_forget (d->p->settings, key);
  g_free (key);
}



/* ── Elegir el ícono de una app ──────────────────────────────────────── */

typedef struct
{
  GtkWidget *entry;
  GtkWidget *preview;
} IconChooser;



/* Palabras que identifican a la app: el título sin espacios y los
 * tramos de la clave ("org-jdownloader-…" → "jdownloader") */
static GPtrArray *
app_words (const gchar *key, const gchar *title)
{
  static const gchar *const ignore[] = {
    "org", "com", "net", "io", "app", "update", "launcher", "tray", "status",
    "icon", "indicator", "applet", NULL
  };
  GPtrArray *words = g_ptr_array_new_with_free_func (g_free);
  gchar **parts;
  gchar *lower;
  guint i;

  if (title != NULL)
    {
      gchar **split = g_strsplit (title, " ", -1);
      gchar *joined = g_strjoinv ("", split);

      lower = g_ascii_strdown (joined, -1);
      g_free (joined);
      g_strfreev (split);
      if (strlen (lower) >= 3)
        g_ptr_array_add (words, lower);
      else
        g_free (lower);
    }

  lower = g_ascii_strdown (key != NULL ? key : "", -1);
  parts = g_strsplit_set (lower, "-._ ", -1);
  for (i = 0; parts[i] != NULL; i++)
    if (strlen (parts[i]) >= 4 && !g_strv_contains (ignore, parts[i]))
      g_ptr_array_add (words, g_strdup (parts[i]));
  g_strfreev (parts);
  g_free (lower);

  return words;
}



/* Íconos del tema cuyo nombre contiene alguna palabra de la app */
static GPtrArray *
suggest_icons (const gchar *key, const gchar *title)
{
  GPtrArray *words = app_words (key, title);
  GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
  GList *names, *l;

  if (words->len == 0)
    {
      g_ptr_array_unref (words);
      return found;
    }

  names = gtk_icon_theme_list_icons (gtk_icon_theme_get_default (), NULL);
  names = g_list_sort (names, (GCompareFunc) g_strcmp0);

  for (l = names; l != NULL && found->len < MAX_SUGGESTIONS; l = l->next)
    {
      gchar *lower = g_ascii_strdown (l->data, -1);
      guint i;

      /* Los simbólicos son monocromos: no sirven para una app */
      if (!g_str_has_suffix (lower, "-symbolic"))
        for (i = 0; i < words->len; i++)
          if (strstr (lower, g_ptr_array_index (words, i)) != NULL)
            {
              g_ptr_array_add (found, g_strdup (l->data));
              break;
            }
      g_free (lower);
    }

  g_list_free_full (names, g_free);
  g_ptr_array_unref (words);
  return found;
}



static void
chooser_update_preview (GtkEditable *entry, IconChooser *c)
{
  gchar *text = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (entry))));
  GdkPixbuf *pixbuf = tray_icon_load (text, PREVIEW_ICON_SIZE, 1, NULL);

  g_free (text);

  if (pixbuf != NULL)
    {
      gtk_image_set_from_pixbuf (GTK_IMAGE (c->preview), pixbuf);
      g_object_unref (pixbuf);
    }
  else
    {
      gtk_image_set_from_icon_name (GTK_IMAGE (c->preview), "image-missing",
                                    GTK_ICON_SIZE_DIALOG);
    }
}



static void
suggestion_clicked (GtkButton *button, IconChooser *c)
{
  gtk_entry_set_text (GTK_ENTRY (c->entry),
                      g_object_get_data (G_OBJECT (button), "icon-name"));
}



static void
choose_file_clicked (GtkButton *button, IconChooser *c)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (button));
  GtkFileChooserNative *native;
  GtkFileFilter *filter;

  native = gtk_file_chooser_native_new (_("Choose an icon file"), GTK_WINDOW (toplevel),
                                        GTK_FILE_CHOOSER_ACTION_OPEN, _("_Open"), _("_Cancel"));
  filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, _("Images"));
  gtk_file_filter_add_pixbuf_formats (filter);
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (native), filter);

  if (gtk_native_dialog_run (GTK_NATIVE_DIALOG (native)) == GTK_RESPONSE_ACCEPT)
    {
      gchar *path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (native));

      if (path != NULL)
        gtk_entry_set_text (GTK_ENTRY (c->entry), path);
      g_free (path);
    }

  g_object_unref (native);
}



static void
icon_clicked (GtkButton *button, DialogData *d)
{
  GtkWidget *dialog, *main_dialog, *content, *grid, *label, *flow, *scrolled;
  GtkWidget *file_button, *hint;
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GPtrArray *suggestions;
  IconChooser c;
  const gchar *current, *title = NULL;
  gchar *key = selected_key (d), *heading;
  TrayKnown *k;
  gint index, response;
  guint i;

  if (key == NULL)
    return;

  index = tray_settings_get_index (d->p->settings, key);
  k = index >= 0 ? g_ptr_array_index (tray_settings_get_known (d->p->settings), index) : NULL;
  if (k != NULL && k->title != NULL && *k->title != '\0')
    title = k->title;

  /* Mejor el título actual si la app está abierta */
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (d->tree));
  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      GHashTable *running = running_items (d);
      TrayItem *item = g_hash_table_lookup (running, key);

      if (item != NULL)
        title = tray_item_get_display_name (item);
      g_hash_table_unref (running);
    }
  if (title == NULL)
    title = key;

  heading = g_strdup_printf (_("Icon for %s"), title);
  dialog = gtk_dialog_new_with_buttons (heading, GTK_WINDOW (d->dialog),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("Use the app's _own icon"), RESPONSE_APP_ICON,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        _("_OK"), GTK_RESPONSE_OK,
                                        NULL);
  g_free (heading);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 420, -1);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 12);
  gtk_box_pack_start (GTK_BOX (content), grid, TRUE, TRUE, 0);

  /* Sugerencias del tema */
  suggestions = suggest_icons (key, title);
  label = gtk_label_new (suggestions->len > 0 ? _("Suggested icons from your theme:")
                                              : _("No matching icons in your theme."));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 3, 1);

  flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  /* Con el mínimo por fila, el alto pedido es el real (si no, GTK lo
   * calcula como si hubiera un solo ícono por fila) */
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 6);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 8);
  gtk_widget_set_valign (flow, GTK_ALIGN_START);
  for (i = 0; i < suggestions->len; i++)
    {
      const gchar *name = g_ptr_array_index (suggestions, i);
      GtkWidget *b = gtk_button_new ();
      GtkWidget *image = gtk_image_new_from_icon_name (name, GTK_ICON_SIZE_DND);

      gtk_image_set_pixel_size (GTK_IMAGE (image), CHOOSER_ICON_SIZE);
      gtk_button_set_image (GTK_BUTTON (b), image);
      gtk_button_set_relief (GTK_BUTTON (b), GTK_RELIEF_NONE);
      gtk_widget_set_tooltip_text (b, name);
      g_object_set_data_full (G_OBJECT (b), "icon-name", g_strdup (name), g_free);
      g_signal_connect (b, "clicked", G_CALLBACK (suggestion_clicked), &c);
      gtk_flow_box_insert (GTK_FLOW_BOX (flow), b, -1);
    }
  g_ptr_array_unref (suggestions);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 200);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_container_add (GTK_CONTAINER (scrolled), flow);
  gtk_widget_set_hexpand (scrolled, TRUE);
  gtk_grid_attach (GTK_GRID (grid), scrolled, 0, 1, 3, 1);

  /* Nombre o archivo, con vista previa */
  label = gtk_label_new_with_mnemonic (_("Icon _name or file:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 3, 1);

  c.preview = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (c.preview), PREVIEW_ICON_SIZE);
  gtk_widget_set_size_request (c.preview, PREVIEW_ICON_SIZE, PREVIEW_ICON_SIZE);
  gtk_grid_attach (GTK_GRID (grid), c.preview, 0, 3, 1, 1);

  c.entry = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (c.entry), TRUE);
  gtk_widget_set_hexpand (c.entry, TRUE);
  gtk_widget_set_valign (c.entry, GTK_ALIGN_CENTER);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), c.entry);
  gtk_grid_attach (GTK_GRID (grid), c.entry, 1, 3, 1, 1);

  file_button = gtk_button_new_with_mnemonic (_("Choose _File…"));
  gtk_widget_set_valign (file_button, GTK_ALIGN_CENTER);
  g_signal_connect (file_button, "clicked", G_CALLBACK (choose_file_clicked), &c);
  gtk_grid_attach (GTK_GRID (grid), file_button, 2, 3, 1, 1);

  hint = gtk_label_new (_("While a custom icon is set, changes in the app's own icon "
                          "(for example a notice) are not shown."));
  gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 50);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_grid_attach (GTK_GRID (grid), hint, 0, 4, 3, 1);

  g_signal_connect (c.entry, "changed", G_CALLBACK (chooser_update_preview), &c);
  /* El ícono en uso (elegido o por defecto); vacío si es el de la app */
  current = tray_settings_get_icon_override (d->p->settings, key);
  gtk_entry_set_text (GTK_ENTRY (c.entry), current != NULL ? current : "");
  chooser_update_preview (GTK_EDITABLE (c.entry), &c);

  /* gtk_dialog_run tiene su propio bucle: si mientras tanto se cierra la
   * configuración (p. ej. se reinicia el panel), @d ya no existe */
  main_dialog = d->dialog;
  g_object_add_weak_pointer (G_OBJECT (main_dialog), (gpointer *) &main_dialog);
  g_object_add_weak_pointer (G_OBJECT (dialog), (gpointer *) &dialog);

  gtk_widget_show_all (dialog);
  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (main_dialog == NULL)
    {
      if (dialog != NULL)
        {
          g_object_remove_weak_pointer (G_OBJECT (dialog), (gpointer *) &dialog);
          gtk_widget_destroy (dialog);
        }
      g_free (key);
      return;
    }
  g_object_remove_weak_pointer (G_OBJECT (main_dialog), (gpointer *) &main_dialog);

  if (response == GTK_RESPONSE_OK)
    {
      gchar *text = g_strstrip (g_strdup (gtk_entry_get_text (GTK_ENTRY (c.entry))));

      tray_settings_set_icon_override (d->p->settings, key, text);
      g_free (text);
    }
  else if (response == RESPONSE_APP_ICON)
    {
      /* Explícito: si la app tiene un ícono por defecto, que no vuelva */
      tray_settings_set_icon_override (d->p->settings, key, TRAY_ICON_OWN);
    }

  if (dialog != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (dialog), (gpointer *) &dialog);
      gtk_widget_destroy (dialog);
    }
  g_free (key);
}



static void
selection_changed (GtkTreeSelection *selection, DialogData *d)
{
  if (!d->rebuilding)
    update_buttons (d);
}



/* Explica qué pasa con las apps antiguas (XEmbed) según el estado real */
static void
legacy_update_notice (DialogData *d)
{
  const gchar *icon = "dialog-information-symbolic";
  const gchar *text;
  gchar *owned = NULL;
  gboolean is_proxy = FALSE;

  switch (tray_legacy_get_state (d->dialog))
    {
    case TRAY_LEGACY_ACTIVE:
      text = _("Older apps that use the classic tray (XEmbed) are shown here, "
               "together with the others.");
      break;

    case TRAY_LEGACY_WAITING:
      text = _("Another classic tray is active (for example the native Status "
               "Tray plugin), so older apps are shown there. They will move "
               "here as soon as you remove it.");
      break;

    case TRAY_LEGACY_FAILED:
      icon = "dialog-warning-symbolic";
      text = owned = g_strdup_printf (_("Support for older apps could not start. "
                                        "To see why, run %s --verbose in a terminal."),
                                      PROXY_PATH);
      break;

    case TRAY_LEGACY_NO_XWAYLAND:
      icon = "dialog-warning-symbolic";
      text = _("Older apps need XWayland, which is not available in this "
               "session.");
      break;

    case TRAY_LEGACY_UNAVAILABLE:
    case TRAY_LEGACY_OFF:
    default:
      if (tray_legacy_tray_owner (d->dialog, &is_proxy) == 0)
        {
          icon = "dialog-warning-symbolic";
          text = tray_legacy_available ()
                 ? _("No classic tray is running: older apps that use XEmbed "
                     "will not show an icon. Enable the option above to show "
                     "them here.")
                 : _("No classic tray is running: older apps that use XEmbed "
                     "will not show an icon. Add the native Status Tray plugin "
                     "to the panel to see them.");
        }
      else
        {
          text = _("Older apps that use the classic tray (XEmbed) are shown by "
                   "another tray, such as the native Status Tray plugin. It also "
                   "shows modern apps: to avoid seeing them twice, hide them in "
                   "its properties.");
        }
      break;
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (d->legacy_image), icon, GTK_ICON_SIZE_BUTTON);
  gtk_label_set_text (GTK_LABEL (d->legacy_label), text);
  g_free (owned);
}



static gboolean
legacy_refresh_cb (gpointer user_data)
{
  DialogData *d = user_data;

  d->legacy_refresh_id = 0;
  legacy_update_notice (d);
  return G_SOURCE_REMOVE;
}



static void
legacy_toggled (GtkToggleButton *toggle,
                DialogData      *d)
{
  if (!d->syncing)
    tray_settings_set_legacy_tray (d->p->settings, gtk_toggle_button_get_active (toggle));

  /* El proxy tarda un instante en tomar (o soltar) la bandeja clásica */
  legacy_update_notice (d);
  if (d->legacy_refresh_id != 0)
    g_source_remove (d->legacy_refresh_id);
  d->legacy_refresh_id = g_timeout_add (1200, legacy_refresh_cb, d);
}



static GtkWidget *
legacy_section (DialogData *d)
{
  GtkWidget *vbox, *box;

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  d->legacy_check = gtk_check_button_new_with_mnemonic (
                      _("Also show _older apps (classic XEmbed tray)"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->legacy_check),
                                tray_settings_get_legacy_tray (d->p->settings));
  gtk_widget_set_sensitive (d->legacy_check, tray_legacy_available ());
  if (!tray_legacy_available ())
    gtk_widget_set_tooltip_text (d->legacy_check,
                                 _("This build does not include support for older apps"));
  g_signal_connect (d->legacy_check, "toggled", G_CALLBACK (legacy_toggled), d);
  gtk_box_pack_start (GTK_BOX (vbox), d->legacy_check, FALSE, FALSE, 0);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  d->legacy_image = gtk_image_new ();
  gtk_widget_set_valign (d->legacy_image, GTK_ALIGN_START);
  d->legacy_label = gtk_label_new (NULL);
  gtk_label_set_line_wrap (GTK_LABEL (d->legacy_label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (d->legacy_label), 55);
  gtk_label_set_xalign (GTK_LABEL (d->legacy_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (d->legacy_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (box), d->legacy_image, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), d->legacy_label, TRUE, TRUE, 0);
  gtk_widget_set_margin_start (box, 24);
  gtk_box_pack_start (GTK_BOX (vbox), box, FALSE, FALSE, 0);

  return vbox;
}



static GtkWidget *
items_page (DialogData *d)
{
  GtkWidget *vbox, *label, *scrolled, *buttons;
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);

  label = gtk_label_new (_("Drag icons between the panel and the popup, or choose here "
                           "which ones are hidden. Apps that are not running are "
                           "remembered, so you can hide them in advance."));
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 55);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

  d->store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, GDK_TYPE_PIXBUF,
                                 G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  d->tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL (d->store));
  g_object_unref (d->store);
  gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (d->tree), TRUE);

  renderer = gtk_cell_renderer_toggle_new ();
  g_signal_connect (renderer, "toggled", G_CALLBACK (hidden_toggled), d);
  column = gtk_tree_view_column_new_with_attributes (_("Hidden"), renderer,
                                                     "active", COL_HIDDEN, NULL);
  gtk_tree_view_append_column (GTK_TREE_VIEW (d->tree), column);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("App"));
  gtk_tree_view_column_set_expand (column, TRUE);
  renderer = gtk_cell_renderer_pixbuf_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_add_attribute (column, renderer, "pixbuf", COL_PIXBUF);
  renderer = gtk_cell_renderer_text_new ();
  g_object_set (renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_add_attribute (column, renderer, "markup", COL_MARKUP);
  gtk_tree_view_append_column (GTK_TREE_VIEW (d->tree), column);

  g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (d->tree)), "changed",
                    G_CALLBACK (selection_changed), d);

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);
  gtk_container_add (GTK_CONTAINER (scrolled), d->tree);
  gtk_box_pack_start (GTK_BOX (vbox), scrolled, TRUE, TRUE, 0);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  d->up_button = gtk_button_new_from_icon_name ("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (d->up_button, _("Move up"));
  g_signal_connect (d->up_button, "clicked", G_CALLBACK (up_clicked), d);
  d->down_button = gtk_button_new_from_icon_name ("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (d->down_button, _("Move down"));
  g_signal_connect (d->down_button, "clicked", G_CALLBACK (down_clicked), d);
  d->forget_button = gtk_button_new_with_mnemonic (_("_Forget"));
  gtk_widget_set_tooltip_text (d->forget_button,
                               _("Remove an app that is not running from the list"));
  g_signal_connect (d->forget_button, "clicked", G_CALLBACK (forget_clicked), d);
  gtk_box_pack_start (GTK_BOX (buttons), d->up_button, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (buttons), d->down_button, FALSE, FALSE, 0);
  gtk_box_pack_end (GTK_BOX (buttons), d->forget_button, FALSE, FALSE, 0);
  d->icon_button = gtk_button_new_with_mnemonic (_("Ic_on…"));
  gtk_widget_set_tooltip_text (d->icon_button,
                               _("Choose a different icon for the selected app"));
  g_signal_connect (d->icon_button, "clicked", G_CALLBACK (icon_clicked), d);
  gtk_box_pack_end (GTK_BOX (buttons), d->icon_button, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), buttons, FALSE, FALSE, 0);

  /* Junto a la lista: decide con qué casilla empieza una app nueva */
  d->new_in_popup = gtk_check_button_new_with_mnemonic (_("Put _new apps in the popup"));
  gtk_widget_set_tooltip_text (d->new_in_popup,
                               _("Apps that appear for the first time start hidden. "
                                 "Apps already in the list are not changed."));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->new_in_popup),
                                tray_settings_get_new_in_popup (d->p->settings));
  g_signal_connect (d->new_in_popup, "toggled", G_CALLBACK (new_in_popup_toggled), d);
  gtk_box_pack_start (GTK_BOX (vbox), d->new_in_popup, FALSE, FALSE, 0);

  gtk_box_pack_start (GTK_BOX (vbox), legacy_section (d), FALSE, FALSE, 4);

  rebuild_list (d);
  return vbox;
}



/* ── Ciclo de vida ───────────────────────────────────────────────────── */

static void
model_changed (TrayModel *model, TrayItem *item, DialogData *d)
{
  rebuild_list (d);
}



/* La configuración cambió (p. ej. desde xfconf-query): reflejarlo en los
 * controles sin volver a escribirla */
static void
sync_from_settings (DialogData *d)
{
  TraySettings *s = d->p->settings;
  gint panel_size = tray_settings_get_panel_icon_size (s);

  d->syncing = TRUE;

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->panel_auto), panel_size == 0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->panel_size),
                             tray_plugin_get_panel_icon_size (d->p));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->panel_spacing),
                             tray_settings_get_panel_spacing (s));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->popup_size),
                             tray_settings_get_popup_icon_size (s));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->popup_spacing),
                             tray_settings_get_popup_spacing (s));
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->popup_columns),
                             tray_settings_get_popup_columns (s));
  gtk_combo_box_set_active (GTK_COMBO_BOX (d->popup_corners),
                            tray_settings_get_popup_corners (s));
  gtk_combo_box_set_active (GTK_COMBO_BOX (d->arrow_position),
                            tray_settings_get_arrow_position (s));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->passive),
                                tray_settings_get_passive_in_popup (s));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->new_in_popup),
                                tray_settings_get_new_in_popup (s));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->symbolic),
                                tray_settings_get_symbolic_icons (s));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (d->legacy_check),
                                tray_settings_get_legacy_tray (s));

  d->syncing = FALSE;
}



static void
dialog_destroyed (GtkWidget *dialog, DialogData *d)
{
  g_signal_handlers_disconnect_by_data (d->p->settings, d);
  g_signal_handlers_disconnect_by_data (d->p->model, d);
  if (d->legacy_refresh_id != 0)
    g_source_remove (d->legacy_refresh_id);

  xfce_panel_plugin_unblock_menu (d->p->panel_plugin);
  d->p->dialog = NULL;
  g_free (d);
}



static void
dialog_response (GtkDialog *dialog, gint response, DialogData *d)
{
  gtk_widget_destroy (GTK_WIDGET (dialog));
}



void
tray_dialog_show (TrayPlugin *p)
{
  DialogData *d;
  GtkWidget *notebook, *content, *parent;

  if (p->dialog != NULL)
    {
      gtk_window_present (GTK_WINDOW (p->dialog));
      return;
    }

  d = g_new0 (DialogData, 1);
  d->p = p;

  /* En Wayland el panel es una capa (layer-shell), no una ventana común:
   * con él como padre, GTK ubica mal los desplegables del diálogo (salían
   * en una esquina de la pantalla). El plugin cierra el diálogo al quitarse */
  parent = gtk_widget_get_toplevel (GTK_WIDGET (p->panel_plugin));
  if (!GDK_IS_X11_DISPLAY (gtk_widget_get_display (parent)))
    parent = NULL;

  d->dialog = xfce_titled_dialog_new_with_mixed_buttons (
                _("Tray Overflow"),
                parent != NULL ? GTK_WINDOW (parent) : NULL,
                GTK_DIALOG_DESTROY_WITH_PARENT,
                "window-close-symbolic", _("_Close"), GTK_RESPONSE_CLOSE,
                NULL);
  p->dialog = d->dialog;

  gtk_window_set_icon_name (GTK_WINDOW (d->dialog), "org.xfce.panel.systray");
  gtk_window_set_default_size (GTK_WINDOW (d->dialog), 480, 520);
  xfce_panel_plugin_block_menu (p->panel_plugin);

  notebook = gtk_notebook_new ();
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), appearance_page (d),
                            gtk_label_new_with_mnemonic (_("A_ppearance")));
  gtk_notebook_append_page (GTK_NOTEBOOK (notebook), items_page (d),
                            gtk_label_new_with_mnemonic (_("_Items")));

  content = gtk_dialog_get_content_area (GTK_DIALOG (d->dialog));
  gtk_box_pack_start (GTK_BOX (content), notebook, TRUE, TRUE, 0);

  g_signal_connect_swapped (p->settings, "known-changed", G_CALLBACK (rebuild_list), d);
  g_signal_connect_swapped (p->settings, "changed", G_CALLBACK (sync_from_settings), d);
  g_signal_connect (p->model, "item-added", G_CALLBACK (model_changed), d);
  g_signal_connect (p->model, "item-removed", G_CALLBACK (model_changed), d);
  g_signal_connect (d->dialog, "response", G_CALLBACK (dialog_response), d);
  g_signal_connect (d->dialog, "destroy", G_CALLBACK (dialog_destroyed), d);

  gtk_widget_show_all (d->dialog);
  legacy_update_notice (d);
}
