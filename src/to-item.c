/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-item.h"

#include <string.h>

typedef struct
{
  gchar          *key;
  gchar          *title;
  TrayItemStatus  status;
  gboolean        is_menu;
  gboolean        is_legacy;    /* app antigua, vía nuestro proxy XEmbed */

  gchar          *icon_name[TRAY_ITEM_N_ICONS];
  GPtrArray      *pixmaps[TRAY_ITEM_N_ICONS];

  gchar          *theme_path;
  GtkIconTheme   *path_theme;   /* tema que solo busca en theme_path */

  gchar          *tooltip_title;
  gchar          *tooltip_text;

  GHashTable     *size_requests;   /* requester → píxeles */
  gint            size_sent;       /* último tamaño pasado al backend */
} TrayItemPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (TrayItem, tray_item, G_TYPE_OBJECT)

enum
{
  SIGNAL_CHANGED,
  SIGNAL_ACTIVATE_FALLBACK,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

#define PRIV(item) ((TrayItemPrivate *) tray_item_get_instance_private (TRAY_ITEM (item)))



static void
tray_item_finalize (GObject *object)
{
  TrayItemPrivate *priv = PRIV (object);
  gint i;

  g_free (priv->key);
  g_free (priv->title);
  for (i = 0; i < TRAY_ITEM_N_ICONS; i++)
    {
      g_free (priv->icon_name[i]);
      if (priv->pixmaps[i] != NULL)
        g_ptr_array_unref (priv->pixmaps[i]);
    }
  g_free (priv->theme_path);
  g_clear_object (&priv->path_theme);
  g_free (priv->tooltip_title);
  g_free (priv->tooltip_text);
  g_clear_pointer (&priv->size_requests, g_hash_table_unref);

  G_OBJECT_CLASS (tray_item_parent_class)->finalize (object);
}



static void
tray_item_class_init (TrayItemClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = tray_item_finalize;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  signals[SIGNAL_ACTIVATE_FALLBACK] =
    g_signal_new ("activate-fallback", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}



static void
tray_item_init (TrayItem *item)
{
  PRIV (item)->status = TRAY_ITEM_STATUS_ACTIVE;
}



/* ── Getters ─────────────────────────────────────────────────────────── */

const gchar *
tray_item_get_key (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  return PRIV (item)->key;
}



const gchar *
tray_item_get_title (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  return PRIV (item)->title;
}



const gchar *
tray_item_get_display_name (TrayItem *item)
{
  TrayItemPrivate *priv;

  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  priv = PRIV (item);

  if (priv->title != NULL && *priv->title != '\0')
    return priv->title;
  if (priv->tooltip_title != NULL && *priv->tooltip_title != '\0')
    return priv->tooltip_title;
  return priv->key != NULL ? priv->key : "";
}



const gchar *
tray_item_get_icon_name (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  return PRIV (item)->icon_name[TRAY_ITEM_ICON_MAIN];
}



TrayItemStatus
tray_item_get_status (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), TRAY_ITEM_STATUS_ACTIVE);
  return PRIV (item)->status;
}



gboolean
tray_item_get_is_menu (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), FALSE);
  return PRIV (item)->is_menu;
}



gboolean
tray_item_get_is_legacy (TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_ITEM (item), FALSE);
  return PRIV (item)->is_legacy;
}



/* KDE permite un subconjunto de HTML en los tooltips. Convertimos los
 * saltos de línea HTML y, si el resultado no es markup Pango válido,
 * lo escapamos para mostrarlo como texto plano. */
static gchar *
tooltip_text_to_markup (const gchar *text)
{
  static gsize regex_once = 0;
  static GRegex *regex = NULL;
  gchar *normalized;

  /* Se compila una sola vez: los tooltips cambian a menudo */
  if (g_once_init_enter (&regex_once))
    {
      regex = g_regex_new ("<br\\s*/?>", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
      g_once_init_leave (&regex_once, 1);
    }

  normalized = g_regex_replace_literal (regex, text, -1, 0, "\n", 0, NULL);

  if (normalized == NULL)
    normalized = g_strdup (text);

  if (!pango_parse_markup (normalized, -1, 0, NULL, NULL, NULL, NULL))
    {
      gchar *escaped = g_markup_escape_text (normalized, -1);
      g_free (normalized);
      normalized = escaped;
    }

  return normalized;
}



gchar *
tray_item_get_tooltip_markup (TrayItem *item)
{
  TrayItemPrivate *priv;
  const gchar *title;
  gboolean has_title, has_text;
  gchar *title_esc, *text_markup, *result;

  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  priv = PRIV (item);

  title = (priv->tooltip_title != NULL && *priv->tooltip_title != '\0')
          ? priv->tooltip_title : priv->title;

  has_title = title != NULL && *title != '\0';
  has_text = priv->tooltip_text != NULL && *priv->tooltip_text != '\0';

  if (!has_title && !has_text)
    return NULL;

  title_esc = has_title ? g_markup_escape_text (title, -1) : NULL;
  text_markup = has_text ? tooltip_text_to_markup (priv->tooltip_text) : NULL;

  if (has_title && has_text && g_strcmp0 (title, priv->tooltip_text) != 0)
    result = g_strdup_printf ("<b>%s</b>\n%s", title_esc, text_markup);
  else if (has_title)
    result = g_strdup (title_esc);
  else
    result = g_strdup (text_markup);

  g_free (title_esc);
  g_free (text_markup);
  return result;
}



/* ── Renderizado de íconos ───────────────────────────────────────────── */

static GdkPixbuf *
load_from_theme (GtkIconTheme    *theme,
                 const gchar     *name,
                 gint             size,
                 gint             scale,
                 GtkStyleContext *context)
{
  GtkIconInfo *info;
  GdkPixbuf *pixbuf = NULL;

  info = gtk_icon_theme_lookup_icon_for_scale (theme, name, size, scale,
                                               GTK_ICON_LOOKUP_FORCE_SIZE);
  if (info == NULL)
    return NULL;

  if (context != NULL)
    pixbuf = gtk_icon_info_load_symbolic_for_context (info, context, NULL, NULL);
  if (pixbuf == NULL)
    pixbuf = gtk_icon_info_load_icon (info, NULL);

  g_object_unref (info);
  return pixbuf;
}



static GdkPixbuf *
load_named (TrayItem        *item,
            const gchar     *name,
            gint             size,
            gint             scale,
            GtkStyleContext *context)
{
  TrayItemPrivate *priv = PRIV (item);
  GdkPixbuf *pixbuf;
  gint px = size * scale;

  if (name == NULL || *name == '\0')
    return NULL;

  /* Algunas apps envían una ruta absoluta en vez de un nombre */
  if (g_path_is_absolute (name))
    return gdk_pixbuf_new_from_file_at_scale (name, px, px, TRUE, NULL);

  /* Primero el tema del usuario, así se respeta su estilo */
  pixbuf = load_from_theme (gtk_icon_theme_get_default (), name, size, scale, context);
  if (pixbuf != NULL)
    return pixbuf;

  /* Luego la carpeta propia de la app (IconThemePath) */
  if (priv->theme_path != NULL && *priv->theme_path != '\0')
    {
      if (priv->path_theme == NULL)
        {
          const gchar *paths[] = { priv->theme_path, NULL };

          priv->path_theme = gtk_icon_theme_new ();
          gtk_icon_theme_set_search_path (priv->path_theme, paths, 1);
          gtk_icon_theme_set_custom_theme (priv->path_theme, "hicolor");
        }
      pixbuf = load_from_theme (priv->path_theme, name, size, scale, context);
      if (pixbuf != NULL)
        return pixbuf;
    }

  /* Temas modernos (Adwaita 46+) solo traen la variante simbólica de
   * muchos íconos: probarla antes de rendirse */
  if (!g_str_has_suffix (name, "-symbolic"))
    {
      gchar *symbolic = g_strconcat (name, "-symbolic", NULL);

      pixbuf = load_from_theme (gtk_icon_theme_get_default (), symbolic, size, scale, context);
      g_free (symbolic);
    }

  return pixbuf;
}



GdkPixbuf *
tray_icon_load (const gchar     *name,
                gint             size,
                gint             scale,
                GtkStyleContext *context)
{
  GdkPixbuf *pixbuf;
  gint px;

  if (name == NULL || *name == '\0')
    return NULL;

  size = MAX (size, 1);
  scale = MAX (scale, 1);
  px = size * scale;

  if (g_path_is_absolute (name))
    {
      GdkPixbuf *loaded = gdk_pixbuf_new_from_file_at_scale (name, px, px, TRUE, NULL);
      gint w, h;

      if (loaded == NULL)
        return NULL;

      /* Imagen no cuadrada: centrarla en el cuadrado, sin deformarla */
      w = gdk_pixbuf_get_width (loaded);
      h = gdk_pixbuf_get_height (loaded);
      if (w == h)
        return loaded;

      pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, px, px);
      gdk_pixbuf_fill (pixbuf, 0);
      gdk_pixbuf_copy_area (loaded, 0, 0, MIN (w, px), MIN (h, px),
                            pixbuf, (px - MIN (w, px)) / 2, (px - MIN (h, px)) / 2);
      g_object_unref (loaded);
      return pixbuf;
    }

  return load_from_theme (gtk_icon_theme_get_default (), name, size, scale, context);
}



/* Elige el pixmap más adecuado para @px y lo escala manteniendo aspecto */
static GdkPixbuf *
pick_pixmap (GPtrArray *pixmaps,
             gint       px)
{
  GdkPixbuf *best = NULL;
  gint best_dim = 0;
  gint w, h, nw, nh;
  guint i;

  if (pixmaps == NULL || pixmaps->len == 0)
    return NULL;

  for (i = 0; i < pixmaps->len; i++)
    {
      GdkPixbuf *candidate = g_ptr_array_index (pixmaps, i);
      gint dim = MAX (gdk_pixbuf_get_width (candidate),
                      gdk_pixbuf_get_height (candidate));

      /* Preferimos el más chico que sea >= px; si no hay, el más grande */
      if (best == NULL
          || (dim >= px && (best_dim < px || dim < best_dim))
          || (best_dim < px && dim > best_dim))
        {
          best = candidate;
          best_dim = dim;
        }
    }

  w = gdk_pixbuf_get_width (best);
  h = gdk_pixbuf_get_height (best);

  if (MAX (w, h) == px)
    return g_object_ref (best);

  if (w >= h)
    {
      nw = px;
      nh = MAX (1, h * px / w);
    }
  else
    {
      nh = px;
      nw = MAX (1, w * px / h);
    }

  return gdk_pixbuf_scale_simple (best, nw, nh,
                                  MAX (w, h) > px ? GDK_INTERP_HYPER
                                                  : GDK_INTERP_BILINEAR);
}



static GdkPixbuf *
render_role (TrayItem         *item,
             TrayItemIconRole  role,
             gint              size,
             gint              scale,
             GtkStyleContext  *context)
{
  TrayItemPrivate *priv = PRIV (item);
  GdkPixbuf *pixbuf;

  pixbuf = load_named (item, priv->icon_name[role], size, scale, context);
  if (pixbuf == NULL)
    pixbuf = pick_pixmap (priv->pixmaps[role], size * scale);

  return pixbuf;
}



GdkPixbuf *
tray_item_render_icon (TrayItem        *item,
                       gint             size,
                       gint             scale,
                       GtkStyleContext *context)
{
  TrayItemPrivate *priv;
  GdkPixbuf *base = NULL;
  GdkPixbuf *overlay;

  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  priv = PRIV (item);

  size = MAX (size, 1);
  scale = MAX (scale, 1);

  if (priv->status == TRAY_ITEM_STATUS_ATTENTION)
    base = render_role (item, TRAY_ITEM_ICON_ATTENTION, size, scale, context);
  if (base == NULL)
    base = render_role (item, TRAY_ITEM_ICON_MAIN, size, scale, context);
  if (base == NULL)
    base = load_from_theme (gtk_icon_theme_get_default (), "image-missing",
                            size, scale, context);
  if (base == NULL)
    return NULL;

  /* Ícono superpuesto (overlay): esquina inferior derecha, a mitad de tamaño */
  overlay = render_role (item, TRAY_ITEM_ICON_OVERLAY, MAX (size / 2, 1), scale, context);
  if (overlay != NULL)
    {
      GdkPixbuf *composed;
      gint bw, bh, ow, oh;

      composed = gdk_pixbuf_add_alpha (base, FALSE, 0, 0, 0);
      g_object_unref (base);
      base = composed;

      bw = gdk_pixbuf_get_width (base);
      bh = gdk_pixbuf_get_height (base);
      ow = MIN (gdk_pixbuf_get_width (overlay), bw);
      oh = MIN (gdk_pixbuf_get_height (overlay), bh);

      gdk_pixbuf_composite (overlay, base, bw - ow, bh - oh, ow, oh,
                            bw - ow, bh - oh, 1.0, 1.0,
                            GDK_INTERP_BILINEAR, 255);
      g_object_unref (overlay);
    }

  return base;
}



/* ── Acciones (virtuales) ────────────────────────────────────────────── */

GtkWidget *
tray_item_get_menu (TrayItem *item)
{
  TrayItemClass *klass;

  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);
  klass = TRAY_ITEM_GET_CLASS (item);
  return klass->get_menu != NULL ? klass->get_menu (item) : NULL;
}



void
tray_item_activate (TrayItem *item, gint x, gint y)
{
  TrayItemClass *klass;

  g_return_if_fail (TRAY_IS_ITEM (item));
  klass = TRAY_ITEM_GET_CLASS (item);
  if (klass->activate != NULL)
    klass->activate (item, x, y);
}



void
tray_item_secondary_activate (TrayItem *item, gint x, gint y)
{
  TrayItemClass *klass;

  g_return_if_fail (TRAY_IS_ITEM (item));
  klass = TRAY_ITEM_GET_CLASS (item);
  if (klass->secondary_activate != NULL)
    klass->secondary_activate (item, x, y);
}



void
tray_item_context_menu (TrayItem *item, gint x, gint y)
{
  TrayItemClass *klass;

  g_return_if_fail (TRAY_IS_ITEM (item));
  klass = TRAY_ITEM_GET_CLASS (item);
  if (klass->context_menu != NULL)
    klass->context_menu (item, x, y);
}



void
tray_item_scroll (TrayItem *item, gint delta, gboolean horizontal, gint x, gint y)
{
  TrayItemClass *klass;

  g_return_if_fail (TRAY_IS_ITEM (item));
  klass = TRAY_ITEM_GET_CLASS (item);
  if (klass->scroll != NULL)
    klass->scroll (item, delta, horizontal, x, y);
}



static void
apply_size_requests (TrayItem *item)
{
  TrayItemPrivate *priv = PRIV (item);
  TrayItemClass *klass = TRAY_ITEM_GET_CLASS (item);
  GHashTableIter iter;
  gpointer value;
  gint size = 0;

  if (klass->set_icon_size == NULL || priv->size_requests == NULL)
    return;

  g_hash_table_iter_init (&iter, priv->size_requests);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    size = MAX (size, GPOINTER_TO_INT (value));

  if (size > 0 && size != priv->size_sent)
    {
      priv->size_sent = size;
      klass->set_icon_size (item, size);
    }
}



void
tray_item_request_size (TrayItem *item, gpointer requester, gint pixels)
{
  TrayItemPrivate *priv;

  g_return_if_fail (TRAY_IS_ITEM (item));
  priv = PRIV (item);

  if (priv->size_requests == NULL)
    priv->size_requests = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_hash_table_insert (priv->size_requests, requester, GINT_TO_POINTER (MAX (pixels, 1)));
  apply_size_requests (item);
}



void
tray_item_release_size (TrayItem *item, gpointer requester)
{
  TrayItemPrivate *priv;

  g_return_if_fail (TRAY_IS_ITEM (item));
  priv = PRIV (item);

  if (priv->size_requests != NULL && g_hash_table_remove (priv->size_requests, requester))
    apply_size_requests (item);
}



/* ── Setters para backends ───────────────────────────────────────────── */

static void
replace_string (gchar **dest, const gchar *value)
{
  if (g_strcmp0 (*dest, value) != 0)
    {
      g_free (*dest);
      *dest = g_strdup (value);
    }
}



void
tray_item_set_key (TrayItem *item, const gchar *key)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  replace_string (&PRIV (item)->key, key);
}



void
tray_item_set_title (TrayItem *item, const gchar *title)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  replace_string (&PRIV (item)->title, title);
}



void
tray_item_set_status (TrayItem *item, TrayItemStatus status)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  PRIV (item)->status = status;
}



void
tray_item_set_is_menu (TrayItem *item, gboolean is_menu)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  PRIV (item)->is_menu = is_menu;
}



void
tray_item_set_is_legacy (TrayItem *item, gboolean is_legacy)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  PRIV (item)->is_legacy = is_legacy;
}



void
tray_item_set_icon (TrayItem         *item,
                    TrayItemIconRole  role,
                    const gchar      *icon_name,
                    GPtrArray        *pixmaps)
{
  TrayItemPrivate *priv;

  g_return_if_fail (TRAY_IS_ITEM (item));
  g_return_if_fail (role < TRAY_ITEM_N_ICONS);
  priv = PRIV (item);

  replace_string (&priv->icon_name[role], icon_name);

  if (priv->pixmaps[role] != NULL)
    g_ptr_array_unref (priv->pixmaps[role]);
  priv->pixmaps[role] = pixmaps;
}



void
tray_item_set_icon_theme_path (TrayItem *item, const gchar *path)
{
  TrayItemPrivate *priv;

  g_return_if_fail (TRAY_IS_ITEM (item));
  priv = PRIV (item);

  if (g_strcmp0 (priv->theme_path, path) != 0)
    {
      replace_string (&priv->theme_path, path);
      g_clear_object (&priv->path_theme);
    }
}



void
tray_item_set_tooltip (TrayItem    *item,
                       const gchar *title,
                       const gchar *text)
{
  TrayItemPrivate *priv;

  g_return_if_fail (TRAY_IS_ITEM (item));
  priv = PRIV (item);

  replace_string (&priv->tooltip_title, title);
  replace_string (&priv->tooltip_text, text);
}



void
tray_item_emit_changed (TrayItem *item)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  g_signal_emit (item, signals[SIGNAL_CHANGED], 0);
}



void
tray_item_emit_activate_fallback (TrayItem *item)
{
  g_return_if_fail (TRAY_IS_ITEM (item));
  g_signal_emit (item, signals[SIGNAL_ACTIVATE_FALLBACK], 0);
}
