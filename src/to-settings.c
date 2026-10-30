/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-settings.h"

#include <glib/gstdio.h>
#include <string.h>
#include <xfconf/xfconf.h>

#define DEFAULT_PANEL_ICON_SIZE  0    /* automático */
#define DEFAULT_POPUP_ICON_SIZE  24
#define DEFAULT_PANEL_SPACING    2
#define DEFAULT_POPUP_SPACING    6
#define DEFAULT_POPUP_COLUMNS    4
#define DEFAULT_POPUP_CORNERS    TRAY_CORNERS_THEME
#define DEFAULT_ARROW_POSITION   TRAY_ARROW_START
#define DEFAULT_PASSIVE_IN_POPUP TRUE
#define DEFAULT_LEGACY_TRAY      FALSE
#define MAX_KNOWN                200
#define SAVE_DELAY_SECONDS       2    /* agrupar escrituras en xfconf */
#define RELOAD_DELAY_MS          250  /* agrupar cambios hechos desde afuera */

struct _TraySettings
{
  GObject        parent_instance;

  XfconfChannel *channel;
  gchar         *base;

  gint           panel_icon_size;
  gint           popup_icon_size;
  gint           panel_spacing;
  gint           popup_spacing;
  gint           popup_columns;
  gint           popup_corners;
  gint           arrow_position;
  gboolean       passive_in_popup;
  gboolean       legacy_tray;

  GHashTable    *hidden;   /* claves ocultas explícitamente */
  GHashTable    *visible;  /* claves mostradas explícitamente */
  GPtrArray     *known;    /* TrayKnown*, en orden de visualización */
  GHashTable    *icons;    /* clave → ícono elegido por el usuario */

  gint           freeze_count;
  gboolean       pending_changed;
  gboolean       pending_known;

  guint          save_id;     /* guardado diferido pendiente */
  gboolean       discarded;   /* el plugin fue quitado: no volver a escribir */

  gulong         property_changed_id;
  guint          reload_id;   /* releer xfconf tras un cambio desde afuera */
};

G_DEFINE_TYPE (TraySettings, tray_settings, G_TYPE_OBJECT)

enum
{
  SIGNAL_CHANGED,
  SIGNAL_KNOWN_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];



static void
known_free (gpointer data)
{
  TrayKnown *known = data;

  g_free (known->key);
  g_free (known->title);
  g_free (known->icon_name);
  g_free (known);
}



/* Borra la copia del ícono en la caché, si era nuestra. No va en
 * known_free: al releer la configuración se vacía la lista entera. */
static void
known_unlink_cached_icon (TrayKnown *known)
{
  if (known->icon_name != NULL && g_path_is_absolute (known->icon_name)
      && g_str_has_prefix (known->icon_name, g_get_user_cache_dir ()))
    g_unlink (known->icon_name);
}



static gchar *
prop (TraySettings *s, const gchar *name)
{
  return g_strconcat (s->base, "/", name, NULL);
}



/* ── Lectura y escritura en xfconf ───────────────────────────────────── */

static gint
read_int (TraySettings *s, const gchar *name, gint def, gint min, gint max)
{
  gchar *p = prop (s, name);
  gint value = xfconf_channel_get_int (s->channel, p, def);

  g_free (p);
  return CLAMP (value, min, max);
}



static void
write_int (TraySettings *s, const gchar *name, gint value)
{
  gchar *p = prop (s, name);

  xfconf_channel_set_int (s->channel, p, value);
  g_free (p);
}



static gchar **
read_list (TraySettings *s, const gchar *name)
{
  gchar *p = prop (s, name);
  gchar **list = xfconf_channel_get_string_list (s->channel, p);

  g_free (p);
  return list;
}



/* xfconf no acepta arreglos vacíos: en ese caso borramos la propiedad */
static void
write_list (TraySettings *s, const gchar *name, GPtrArray *values)
{
  gchar *p = prop (s, name);

  if (values->len == 0)
    {
      xfconf_channel_reset_property (s->channel, p, FALSE);
    }
  else
    {
      g_ptr_array_add (values, NULL);
      xfconf_channel_set_string_list (s->channel, p,
                                      (const gchar *const *) values->pdata);
      g_ptr_array_remove_index (values, values->len - 1);
    }

  g_free (p);
}



static void
write_set (TraySettings *s, const gchar *name, GHashTable *set)
{
  GPtrArray *values = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, set);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (values, key);

  write_list (s, name, values);
  g_ptr_array_unref (values);
}



static void
write_known (TraySettings *s)
{
  GPtrArray *keys = g_ptr_array_new ();
  GPtrArray *titles = g_ptr_array_new ();
  GPtrArray *icons = g_ptr_array_new ();
  guint i;

  for (i = 0; i < s->known->len; i++)
    {
      TrayKnown *k = g_ptr_array_index (s->known, i);

      g_ptr_array_add (keys, k->key);
      g_ptr_array_add (titles, k->title != NULL ? k->title : "");
      g_ptr_array_add (icons, k->icon_name != NULL ? k->icon_name : "");
    }

  write_list (s, "known-items", keys);
  write_list (s, "known-titles", titles);
  write_list (s, "known-icons", icons);

  g_ptr_array_unref (keys);
  g_ptr_array_unref (titles);
  g_ptr_array_unref (icons);
}



/* Pares clave, ícono en una sola lista (xfconf no tiene diccionarios) */
static void
write_icons (TraySettings *s)
{
  GPtrArray *values = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, s->icons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_ptr_array_add (values, key);
      g_ptr_array_add (values, value);
    }

  write_list (s, "icon-overrides", values);
  g_ptr_array_unref (values);
}



static void
save_all (TraySettings *s)
{
  write_int (s, "panel-icon-size", s->panel_icon_size);
  write_int (s, "popup-icon-size", s->popup_icon_size);
  write_int (s, "panel-spacing", s->panel_spacing);
  write_int (s, "popup-spacing", s->popup_spacing);
  write_int (s, "popup-columns", s->popup_columns);
  write_int (s, "popup-corners", s->popup_corners);
  write_int (s, "arrow-position", s->arrow_position);

  {
    gchar *p = prop (s, "passive-in-popup");
    xfconf_channel_set_bool (s->channel, p, s->passive_in_popup);
    g_free (p);
    p = prop (s, "legacy-tray");
    xfconf_channel_set_bool (s->channel, p, s->legacy_tray);
    g_free (p);
  }

  write_set (s, "hidden-items", s->hidden);
  write_set (s, "visible-items", s->visible);
  write_known (s);
  write_icons (s);
}



static void
load_set (TraySettings *s, const gchar *name, GHashTable *set)
{
  gchar **list = read_list (s, name);
  gint i;

  for (i = 0; list != NULL && list[i] != NULL; i++)
    if (*list[i] != '\0')
      g_hash_table_add (set, g_strdup (list[i]));

  g_strfreev (list);
}



static void
load_all (TraySettings *s)
{
  gchar **keys, **titles, **icons;
  guint n_titles, n_icons;
  gchar *p;
  gint i;

  s->panel_icon_size = read_int (s, "panel-icon-size", DEFAULT_PANEL_ICON_SIZE,
                                 0, TRAY_ICON_SIZE_MAX);
  if (s->panel_icon_size != 0 && s->panel_icon_size < TRAY_ICON_SIZE_MIN)
    s->panel_icon_size = TRAY_ICON_SIZE_MIN;

  s->popup_icon_size = read_int (s, "popup-icon-size", DEFAULT_POPUP_ICON_SIZE,
                                 TRAY_ICON_SIZE_MIN, TRAY_ICON_SIZE_MAX);
  s->panel_spacing = read_int (s, "panel-spacing", DEFAULT_PANEL_SPACING,
                               0, TRAY_SPACING_MAX);
  s->popup_spacing = read_int (s, "popup-spacing", DEFAULT_POPUP_SPACING,
                               0, TRAY_SPACING_MAX);
  s->popup_columns = read_int (s, "popup-columns", DEFAULT_POPUP_COLUMNS,
                               1, TRAY_COLUMNS_MAX);
  s->popup_corners = read_int (s, "popup-corners", DEFAULT_POPUP_CORNERS,
                               TRAY_CORNERS_THEME, TRAY_CORNERS_SQUARE);
  s->arrow_position = read_int (s, "arrow-position", DEFAULT_ARROW_POSITION,
                                TRAY_ARROW_START, TRAY_ARROW_END);

  p = prop (s, "passive-in-popup");
  s->passive_in_popup = xfconf_channel_get_bool (s->channel, p, DEFAULT_PASSIVE_IN_POPUP);
  g_free (p);

  p = prop (s, "legacy-tray");
  s->legacy_tray = xfconf_channel_get_bool (s->channel, p, DEFAULT_LEGACY_TRAY);
  g_free (p);

  load_set (s, "hidden-items", s->hidden);
  load_set (s, "visible-items", s->visible);

  keys = read_list (s, "icon-overrides");
  for (i = 0; keys != NULL && keys[i] != NULL && keys[i + 1] != NULL; i += 2)
    if (*keys[i] != '\0' && *keys[i + 1] != '\0')
      g_hash_table_replace (s->icons, g_strdup (keys[i]), g_strdup (keys[i + 1]));
  g_strfreev (keys);

  keys = read_list (s, "known-items");
  titles = read_list (s, "known-titles");
  icons = read_list (s, "known-icons");
  n_titles = titles != NULL ? g_strv_length (titles) : 0;
  n_icons = icons != NULL ? g_strv_length (icons) : 0;

  for (i = 0; keys != NULL && keys[i] != NULL; i++)
    {
      TrayKnown *k;

      if (*keys[i] == '\0' || tray_settings_get_index (s, keys[i]) >= 0)
        continue;

      k = g_new0 (TrayKnown, 1);
      k->key = g_strdup (keys[i]);
      k->title = g_strdup ((guint) i < n_titles ? titles[i] : "");
      k->icon_name = g_strdup ((guint) i < n_icons ? icons[i] : "");
      g_ptr_array_add (s->known, k);
    }

  g_strfreev (keys);
  g_strfreev (titles);
  g_strfreev (icons);
}



/* ── Notificación y guardado diferido ────────────────────────────────── */

static gboolean
save_timeout (gpointer user_data)
{
  TraySettings *s = user_data;

  s->save_id = 0;
  save_all (s);
  return G_SOURCE_REMOVE;
}



/* Los cambios se aplican al instante (señales), pero se escriben en
 * xfconf como mucho una vez cada SAVE_DELAY_SECONDS: apps que cambian
 * su título o ícono a menudo no deben provocar una escritura por cambio */
static void
schedule_save (TraySettings *s)
{
  if (s->save_id == 0 && !s->discarded)
    s->save_id = g_timeout_add_seconds (SAVE_DELAY_SECONDS, save_timeout, s);
}



void
tray_settings_flush (TraySettings *s)
{
  g_return_if_fail (TRAY_IS_SETTINGS (s));

  if (s->save_id != 0)
    {
      g_source_remove (s->save_id);
      s->save_id = 0;
      save_all (s);
    }
}



void
tray_settings_discard (TraySettings *s)
{
  g_return_if_fail (TRAY_IS_SETTINGS (s));

  if (s->save_id != 0)
    {
      g_source_remove (s->save_id);
      s->save_id = 0;
    }
  if (s->reload_id != 0)
    {
      g_source_remove (s->reload_id);
      s->reload_id = 0;
    }
  s->discarded = TRUE;
}



static void
notify (TraySettings *s, gboolean layout)
{
  if (s->freeze_count > 0)
    {
      if (layout)
        s->pending_changed = TRUE;
      s->pending_known = TRUE;
      return;
    }

  schedule_save (s);
  if (layout)
    g_signal_emit (s, signals[SIGNAL_CHANGED], 0);
  g_signal_emit (s, signals[SIGNAL_KNOWN_CHANGED], 0);
}



void
tray_settings_freeze (TraySettings *s)
{
  g_return_if_fail (TRAY_IS_SETTINGS (s));
  s->freeze_count++;
}



void
tray_settings_thaw (TraySettings *s)
{
  gboolean layout, known;

  g_return_if_fail (TRAY_IS_SETTINGS (s));
  g_return_if_fail (s->freeze_count > 0);

  if (--s->freeze_count > 0)
    return;

  layout = s->pending_changed;
  known = s->pending_known;
  s->pending_changed = FALSE;
  s->pending_known = FALSE;

  if (layout || known)
    notify (s, layout);
}



/* ── Cambios hechos desde afuera (xfconf-query, otro programa) ──────────
 * xfconf también avisa de nuestras propias escrituras: se relee todo y
 * solo se avisa si algo quedó distinto de lo que ya teníamos. */

static gint
compare_strings (gconstpointer a,
                 gconstpointer b)
{
  return g_strcmp0 (*(const gchar *const *) a, *(const gchar *const *) b);
}



static void
snapshot_set (GString *out, const gchar *name, GHashTable *set, gboolean values)
{
  GPtrArray *keys = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;
  guint i;

  g_hash_table_iter_init (&iter, set);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_strings);

  g_string_append_printf (out, "%s:", name);
  for (i = 0; i < keys->len; i++)
    {
      const gchar *k = g_ptr_array_index (keys, i);

      g_string_append_printf (out, "%s\x1f", k);
      if (values)
        g_string_append_printf (out, "%s\x1f", (const gchar *) g_hash_table_lookup (set, k));
    }
  g_string_append_c (out, '\n');
  g_ptr_array_unref (keys);
}



/* Todo el estado, en texto, para comparar antes y después de releer */
static gchar *
snapshot (TraySettings *s)
{
  GString *out = g_string_new (NULL);
  guint i;

  g_string_append_printf (out, "%d %d %d %d %d %d %d %d %d\n",
                          s->panel_icon_size, s->popup_icon_size, s->panel_spacing,
                          s->popup_spacing, s->popup_columns, s->popup_corners,
                          s->arrow_position,
                          s->passive_in_popup, s->legacy_tray);
  snapshot_set (out, "hidden", s->hidden, FALSE);
  snapshot_set (out, "visible", s->visible, FALSE);
  snapshot_set (out, "icons", s->icons, TRUE);

  for (i = 0; i < s->known->len; i++)
    {
      TrayKnown *k = g_ptr_array_index (s->known, i);

      g_string_append_printf (out, "%s\x1f%s\x1f%s\n", k->key,
                              k->title != NULL ? k->title : "",
                              k->icon_name != NULL ? k->icon_name : "");
    }

  return g_string_free (out, FALSE);
}



static gboolean
reload_timeout (gpointer user_data)
{
  TraySettings *s = user_data;
  gchar *before, *after;

  s->reload_id = 0;

  /* Hay cambios nuestros sin escribir: ganan ellos (se escriben enseguida
   * y su aviso de vuelta ya coincidirá) */
  if (s->save_id != 0 || s->freeze_count > 0 || s->discarded)
    return G_SOURCE_REMOVE;

  before = snapshot (s);

  g_hash_table_remove_all (s->hidden);
  g_hash_table_remove_all (s->visible);
  g_hash_table_remove_all (s->icons);
  g_ptr_array_set_size (s->known, 0);
  load_all (s);

  after = snapshot (s);

  /* Sin schedule_save: lo leído ya está en xfconf */
  if (strcmp (before, after) != 0)
    {
      g_debug ("Configuración cambiada desde afuera: se aplica");
      g_signal_emit (s, signals[SIGNAL_CHANGED], 0);
      g_signal_emit (s, signals[SIGNAL_KNOWN_CHANGED], 0);
    }

  g_free (before);
  g_free (after);
  return G_SOURCE_REMOVE;
}



static void
property_changed (XfconfChannel *channel,
                  const gchar   *property,
                  const GValue  *value,
                  TraySettings  *s)
{
  gsize len = strlen (s->base);

  /* Solo las propiedades de esta instancia (/plugins/plugin-N/...) */
  if (strncmp (property, s->base, len) != 0 || property[len] != '/')
    return;

  if (s->reload_id == 0 && !s->discarded)
    s->reload_id = g_timeout_add (RELOAD_DELAY_MS, reload_timeout, s);
}



/* ── GObject ─────────────────────────────────────────────────────────── */

static void
tray_settings_finalize (GObject *object)
{
  TraySettings *s = TRAY_SETTINGS (object);

  /* Escribir lo pendiente antes de cerrar (salvo que el plugin se haya
   * quitado del panel: ahí su configuración ya no debe existir) */
  tray_settings_flush (s);

  if (s->reload_id != 0)
    g_source_remove (s->reload_id);
  if (s->property_changed_id != 0)
    g_signal_handler_disconnect (s->channel, s->property_changed_id);

  g_hash_table_unref (s->hidden);
  g_hash_table_unref (s->visible);
  g_hash_table_unref (s->icons);
  g_ptr_array_unref (s->known);
  g_free (s->base);
  xfconf_shutdown ();

  G_OBJECT_CLASS (tray_settings_parent_class)->finalize (object);
}



static void
tray_settings_class_init (TraySettingsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = tray_settings_finalize;

  /* Algo que afecta la distribución de íconos cambió */
  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* Cambió el registro de apps conocidas (títulos, íconos, lista) */
  signals[SIGNAL_KNOWN_CHANGED] =
    g_signal_new ("known-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}



static void
tray_settings_init (TraySettings *s)
{
  s->hidden = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  s->visible = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  s->known = g_ptr_array_new_with_free_func (known_free);
  s->icons = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}



TraySettings *
tray_settings_new (XfcePanelPlugin *plugin)
{
  TraySettings *s;
  GError *error = NULL;

  g_return_val_if_fail (XFCE_IS_PANEL_PLUGIN (plugin), NULL);

  /* xfconf_init lleva un contador interno: es seguro llamarlo aunque el
   * panel ya lo haya inicializado */
  if (!xfconf_init (&error))
    {
      g_warning ("Tray Overflow: xfconf no disponible: %s", error->message);
      g_error_free (error);
    }

  s = g_object_new (TRAY_TYPE_SETTINGS, NULL);
  s->channel = xfconf_channel_get (xfce_panel_get_channel_name ());
  s->base = g_strdup (xfce_panel_plugin_get_property_base (plugin));

  load_all (s);

  s->property_changed_id = g_signal_connect (s->channel, "property-changed",
                                             G_CALLBACK (property_changed), s);

  return s;
}



/* ── Apariencia ──────────────────────────────────────────────────────── */

#define DEFINE_INT_PROPERTY(field, min, max)                              \
  gint                                                                    \
  tray_settings_get_##field (TraySettings *s)                             \
  {                                                                       \
    g_return_val_if_fail (TRAY_IS_SETTINGS (s), 0);                       \
    return s->field;                                                      \
  }                                                                       \
                                                                          \
  void                                                                    \
  tray_settings_set_##field (TraySettings *s, gint value)                 \
  {                                                                       \
    g_return_if_fail (TRAY_IS_SETTINGS (s));                              \
    value = CLAMP (value, min, max);                                      \
    if (s->field != value)                                                \
      {                                                                   \
        s->field = value;                                                 \
        notify (s, TRUE);                                                 \
      }                                                                   \
  }

DEFINE_INT_PROPERTY (panel_icon_size, 0, TRAY_ICON_SIZE_MAX)
DEFINE_INT_PROPERTY (popup_icon_size, TRAY_ICON_SIZE_MIN, TRAY_ICON_SIZE_MAX)
DEFINE_INT_PROPERTY (panel_spacing, 0, TRAY_SPACING_MAX)
DEFINE_INT_PROPERTY (popup_spacing, 0, TRAY_SPACING_MAX)
DEFINE_INT_PROPERTY (popup_columns, 1, TRAY_COLUMNS_MAX)
DEFINE_INT_PROPERTY (popup_corners, TRAY_CORNERS_THEME, TRAY_CORNERS_SQUARE)
DEFINE_INT_PROPERTY (arrow_position, TRAY_ARROW_START, TRAY_ARROW_END)



gboolean
tray_settings_get_legacy_tray (TraySettings *s)
{
  g_return_val_if_fail (TRAY_IS_SETTINGS (s), FALSE);
  return s->legacy_tray;
}



void
tray_settings_set_legacy_tray (TraySettings *s, gboolean value)
{
  g_return_if_fail (TRAY_IS_SETTINGS (s));

  if (s->legacy_tray != !!value)
    {
      s->legacy_tray = !!value;
      notify (s, TRUE);
    }
}



gboolean
tray_settings_get_passive_in_popup (TraySettings *s)
{
  g_return_val_if_fail (TRAY_IS_SETTINGS (s), FALSE);
  return s->passive_in_popup;
}



void
tray_settings_set_passive_in_popup (TraySettings *s, gboolean value)
{
  g_return_if_fail (TRAY_IS_SETTINGS (s));

  if (s->passive_in_popup != !!value)
    {
      s->passive_in_popup = !!value;
      notify (s, TRUE);
    }
}



/* ── Visibilidad ─────────────────────────────────────────────────────── */

gboolean
tray_settings_is_hidden (TraySettings *s,
                         const gchar  *key,
                         gboolean      passive)
{
  g_return_val_if_fail (TRAY_IS_SETTINGS (s), FALSE);

  if (key == NULL)
    return FALSE;

  if (g_hash_table_contains (s->hidden, key))
    return TRUE;

  if (passive && s->passive_in_popup && !g_hash_table_contains (s->visible, key))
    return TRUE;

  return FALSE;
}



void
tray_settings_set_hidden (TraySettings *s,
                          const gchar  *key,
                          gboolean      hidden)
{
  gboolean changed;

  g_return_if_fail (TRAY_IS_SETTINGS (s));
  g_return_if_fail (key != NULL);

  if (hidden)
    {
      changed = g_hash_table_add (s->hidden, g_strdup (key));
      changed |= g_hash_table_remove (s->visible, key);
    }
  else
    {
      changed = g_hash_table_remove (s->hidden, key);
      changed |= g_hash_table_add (s->visible, g_strdup (key));
    }

  if (changed)
    notify (s, TRUE);
}



/* ── Registro y orden ────────────────────────────────────────────────── */

GPtrArray *
tray_settings_get_known (TraySettings *s)
{
  g_return_val_if_fail (TRAY_IS_SETTINGS (s), NULL);
  return s->known;
}



gint
tray_settings_get_index (TraySettings *s,
                         const gchar  *key)
{
  guint i;

  g_return_val_if_fail (TRAY_IS_SETTINGS (s), -1);

  if (key == NULL)
    return -1;

  for (i = 0; i < s->known->len; i++)
    {
      TrayKnown *k = g_ptr_array_index (s->known, i);
      if (g_strcmp0 (k->key, key) == 0)
        return (gint) i;
    }

  return -1;
}



gboolean
tray_settings_remember (TraySettings *s,
                        const gchar  *key,
                        const gchar  *title,
                        const gchar  *icon_name)
{
  TrayKnown *k;
  gint index;

  g_return_val_if_fail (TRAY_IS_SETTINGS (s), FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  if (title == NULL)
    title = "";
  if (icon_name == NULL)
    icon_name = "";

  index = tray_settings_get_index (s, key);
  if (index >= 0)
    {
      k = g_ptr_array_index (s->known, index);

      /* No pisar un buen título/ícono guardado con uno vacío */
      if ((*title != '\0' && g_strcmp0 (k->title, title) != 0)
          || (*icon_name != '\0' && g_strcmp0 (k->icon_name, icon_name) != 0))
        {
          if (*title != '\0')
            {
              g_free (k->title);
              k->title = g_strdup (title);
            }
          if (*icon_name != '\0')
            {
              g_free (k->icon_name);
              k->icon_name = g_strdup (icon_name);
            }
          notify (s, FALSE);
        }
      return FALSE;
    }

  k = g_new0 (TrayKnown, 1);
  k->key = g_strdup (key);
  k->title = g_strdup (title);
  k->icon_name = g_strdup (icon_name);
  g_ptr_array_add (s->known, k);

  /* Límite para que el registro no crezca sin fin: se descarta la más
   * antigua que no tenga una preferencia guardada */
  if (s->known->len > MAX_KNOWN)
    {
      guint i;

      for (i = 0; i < s->known->len - 1; i++)
        {
          TrayKnown *old = g_ptr_array_index (s->known, i);
          if (!g_hash_table_contains (s->hidden, old->key)
              && !g_hash_table_contains (s->visible, old->key))
            {
              known_unlink_cached_icon (old);
              g_ptr_array_remove_index (s->known, i);
              break;
            }
        }
    }

  notify (s, TRUE);
  return TRUE;
}



void
tray_settings_forget (TraySettings *s,
                      const gchar  *key)
{
  gint index;
  gboolean changed;

  g_return_if_fail (TRAY_IS_SETTINGS (s));

  changed = g_hash_table_remove (s->hidden, key);
  changed |= g_hash_table_remove (s->visible, key);
  changed |= g_hash_table_remove (s->icons, key);

  index = tray_settings_get_index (s, key);
  if (index >= 0)
    {
      known_unlink_cached_icon (g_ptr_array_index (s->known, index));
      g_ptr_array_remove_index (s->known, index);
      changed = TRUE;
    }

  if (changed)
    notify (s, TRUE);
}



void
tray_settings_move (TraySettings *s,
                    const gchar  *key,
                    const gchar  *anchor,
                    gboolean      after)
{
  gint from, to;
  TrayKnown *k;

  g_return_if_fail (TRAY_IS_SETTINGS (s));

  if (g_strcmp0 (key, anchor) == 0)
    return;

  from = tray_settings_get_index (s, key);
  if (from < 0)
    return;

  k = g_ptr_array_steal_index (s->known, from);

  if (anchor == NULL)
    {
      g_ptr_array_add (s->known, k);
    }
  else
    {
      to = tray_settings_get_index (s, anchor);
      if (to < 0)
        to = s->known->len;
      else if (after)
        to++;
      g_ptr_array_insert (s->known, to, k);
    }

  notify (s, TRUE);
}



/* ── Ícono elegido ───────────────────────────────────────────────────── */

/* Apps cuyo ícono se ve roto: lista de reemplazos por defecto.
 *
 * Se lee de dos archivos con el mismo formato; el del usuario se consulta
 * primero, así puede agregar apps o cambiar una de la lista:
 *   - DEFAULT_ICONS_PATH (se instala con el plugin)
 *   - ~/.config/xfce4/trayoverflow/icon-defaults.conf
 *
 * Una línea por app, "clave=ícono;ícono;…". Una clave que termina en '*'
 * vale para todas las que empiezan así. Los íconos (nombre del tema o
 * ruta de archivo) se prueban en orden: el primero que exista. */

typedef struct
{
  gchar   *key;
  gboolean prefix;
  gchar  **icons;
} IconDefault;

static GPtrArray *icon_defaults = NULL;



static void
icon_default_free (gpointer data)
{
  IconDefault *def = data;

  g_free (def->key);
  g_strfreev (def->icons);
  g_free (def);
}



static void
icon_defaults_load_file (const gchar *path)
{
  gchar *contents = NULL;
  gchar **lines;
  guint i;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return;

  lines = g_strsplit (contents, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      gchar *line = g_strstrip (lines[i]);
      gchar *eq = strchr (line, '=');
      IconDefault *def;
      gchar **icons, **out;
      guint j;

      if (*line == '\0' || *line == '#' || eq == NULL)
        continue;

      *eq = '\0';
      g_strstrip (line);
      if (*line == '\0' || strcmp (line, "*") == 0)
        continue;

      /* Íconos sin vacíos ni espacios sobrantes */
      icons = g_strsplit (eq + 1, ";", -1);
      out = icons;
      for (j = 0; icons[j] != NULL; j++)
        {
          g_strstrip (icons[j]);
          if (*icons[j] != '\0')
            *out++ = icons[j];
          else
            g_free (icons[j]);
        }
      *out = NULL;
      if (icons[0] == NULL)
        {
          g_free (icons);
          continue;
        }

      def = g_new0 (IconDefault, 1);
      def->prefix = g_str_has_suffix (line, "*");
      def->key = def->prefix ? g_strndup (line, strlen (line) - 1) : g_strdup (line);
      def->icons = icons;
      g_ptr_array_add (icon_defaults, def);
    }

  g_strfreev (lines);
  g_free (contents);
}



static void
icon_defaults_ensure (void)
{
  gchar *user_path;

  if (icon_defaults != NULL)
    return;

  icon_defaults = g_ptr_array_new_with_free_func (icon_default_free);

  user_path = g_build_filename (g_get_user_config_dir (), "xfce4", TO_CSS_PREFIX,
                                "icon-defaults.conf", NULL);
  icon_defaults_load_file (user_path);
  g_free (user_path);

  icon_defaults_load_file (DEFAULT_ICONS_PATH);
}



static gboolean
icon_exists (const gchar *icon)
{
  if (g_path_is_absolute (icon))
    return g_file_test (icon, G_FILE_TEST_IS_REGULAR);

  return gtk_icon_theme_has_icon (gtk_icon_theme_get_default (), icon);
}



/* Si la app está en la lista, el primero de sus íconos que exista (en el
 * tema del usuario o el que instala su paquete: hicolor, pixmaps). Si no
 * existe ninguno, NULL: se usa el de la app. */
static const gchar *
default_icon_for (const gchar *key)
{
  guint i, j;

  icon_defaults_ensure ();

  for (i = 0; i < icon_defaults->len; i++)
    {
      IconDefault *def = g_ptr_array_index (icon_defaults, i);

      if (def->prefix ? !g_str_has_prefix (key, def->key) : strcmp (key, def->key) != 0)
        continue;

      /* La primera regla que coincide decide (la del usuario va antes) */
      for (j = 0; def->icons[j] != NULL; j++)
        if (icon_exists (def->icons[j]))
          return def->icons[j];
      return NULL;
    }

  return NULL;
}



const gchar *
tray_settings_get_icon_choice (TraySettings *s,
                               const gchar  *key)
{
  g_return_val_if_fail (TRAY_IS_SETTINGS (s), NULL);

  return key != NULL ? g_hash_table_lookup (s->icons, key) : NULL;
}



const gchar *
tray_settings_get_icon_override (TraySettings *s,
                                 const gchar  *key)
{
  const gchar *choice;

  g_return_val_if_fail (TRAY_IS_SETTINGS (s), NULL);

  if (key == NULL)
    return NULL;

  choice = g_hash_table_lookup (s->icons, key);
  if (g_strcmp0 (choice, TRAY_ICON_OWN) == 0)
    return NULL;
  if (choice != NULL)
    return choice;

  return default_icon_for (key);
}



void
tray_settings_set_icon_override (TraySettings *s,
                                 const gchar  *key,
                                 const gchar  *icon)
{
  gboolean changed;

  g_return_if_fail (TRAY_IS_SETTINGS (s));
  g_return_if_fail (key != NULL);

  if (icon == NULL || *icon == '\0')
    changed = g_hash_table_remove (s->icons, key);
  else if (g_strcmp0 (g_hash_table_lookup (s->icons, key), icon) != 0)
    {
      g_hash_table_replace (s->icons, g_strdup (key), g_strdup (icon));
      changed = TRUE;
    }
  else
    changed = FALSE;

  if (changed)
    notify (s, TRUE);
}
