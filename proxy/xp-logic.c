/*
 * Tray Overflow — proxy XEmbed: lógica que no depende de X11
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xp-logic.h"

#include <string.h>



/* ════════════════════════════════════════════════════════════════════════
 * Nombre legible de la app
 * ════════════════════════════════════════════════════════════════════════ */

gboolean
xp_title_is_generic (const gchar *title)
{
  /* Java (AWT) usa este nombre en el ícono de todas sus apps */
  return g_strcmp0 (title, "JavaEmbeddedFrame") == 0;
}



/* Opciones de java que llevan su valor en el argumento siguiente */
static gboolean
java_option_takes_value (const gchar *option)
{
  static const gchar *const options[] = {
    "-cp", "-classpath", "--class-path", "-p", "--module-path",
    "--upgrade-module-path", "--add-modules", "--limit-modules",
    "--add-reads", "--add-exports", "--add-opens", "--patch-module", NULL
  };

  return g_strv_contains (options, option);
}



/* "com.frostwire.gui.Main" → "Frostwire": el primer tramo del paquete
 * que no sea un dominio genérico; si no hay paquete, la clase */
static gchar *
java_name_from_class (const gchar *klass)
{
  static const gchar *const skip[] = {
    "com", "org", "net", "io", "de", "me", "edu", "gov", "fr", "uk", "it", "nl", NULL
  };
  gchar **parts = g_strsplit (klass, ".", -1);
  guint n = g_strv_length (parts), i;
  gchar *name = NULL;

  for (i = 0; i + 1 < n && name == NULL; i++)
    if (*parts[i] != '\0' && !g_strv_contains (skip, parts[i]))
      name = g_strdup (parts[i]);

  if (name == NULL && n > 0 && *parts[n - 1] != '\0')
    name = g_strdup (parts[n - 1]);
  g_strfreev (parts);

  if (name != NULL)
    name[0] = g_ascii_toupper (name[0]);
  return name;
}



gchar *
xp_java_app_name (const gchar *cmdline, gsize length)
{
  gchar *contents, *name = NULL, *argv0;
  const gchar *arg, *end;
  gboolean is_java;

  if (cmdline == NULL || length == 0)
    return NULL;

  /* Copia terminada en '\0': el último argumento podría no estarlo */
  contents = g_malloc (length + 1);
  memcpy (contents, cmdline, length);
  contents[length] = '\0';

  end = contents + length;
  argv0 = g_path_get_basename (contents);
  is_java = g_str_has_prefix (argv0, "java");
  g_free (argv0);

  for (arg = contents + strlen (contents) + 1; is_java && arg < end && name == NULL;
       arg += strlen (arg) + 1)
    {
      const gchar *next = arg + strlen (arg) + 1;
      gboolean has_next = next < end && *next != '\0';

      if (strcmp (arg, "-jar") == 0)
        {
          if (has_next)
            {
              name = g_path_get_basename (next);
              if (g_str_has_suffix (name, ".jar"))
                name[strlen (name) - 4] = '\0';
            }
          break;
        }

      if (strcmp (arg, "-m") == 0 || strcmp (arg, "--module") == 0)
        {
          /* "módulo/clase" o solo "módulo" */
          if (has_next)
            {
              const gchar *slash = strchr (next, '/');
              name = java_name_from_class (slash != NULL ? slash + 1 : next);
            }
          break;
        }

      if (java_option_takes_value (arg))
        {
          if (has_next)
            arg = next;
          continue;
        }

      if (*arg == '-' || *arg == '\0')
        continue;

      /* Primer argumento que no es opción: la clase principal */
      name = java_name_from_class (arg);
    }

  g_free (contents);

  if (name != NULL && *name == '\0')
    g_clear_pointer (&name, g_free);
  return name;
}



/* ════════════════════════════════════════════════════════════════════════
 * Imagen del ícono
 * ════════════════════════════════════════════════════════════════════════ */

static guint
mask_shift (gulong mask)
{
  guint shift = 0;

  if (mask == 0)
    return 0;
  while (!(mask & 1))
    {
      mask >>= 1;
      shift++;
    }
  return shift;
}



static guint
mask_bits (gulong mask)
{
  guint bits = 0;

  mask >>= mask_shift (mask);
  while (mask & 1)
    {
      mask >>= 1;
      bits++;
    }
  return bits;
}



guint8
xp_channel (gulong pixel, gulong mask)
{
  guint bits = mask_bits (mask);
  gulong value;

  if (bits == 0)
    return 0;

  value = (pixel & mask) >> mask_shift (mask);
  if (bits >= 8)
    return (guint8) (value >> (bits - 8));
  return (guint8) (value * 255 / ((1u << bits) - 1));
}



guint8 *
xp_shape_mask (const XpRect *rects, gint count, gint w, gint h)
{
  guint8 *mask;
  gint i, y;

  if (rects == NULL || count <= 0 || w <= 0 || h <= 0)
    return NULL;

  /* Un solo rectángulo que cubre todo: sin forma */
  if (count == 1 && rects[0].x <= 0 && rects[0].y <= 0
      && rects[0].x + rects[0].width >= w && rects[0].y + rects[0].height >= h)
    return NULL;

  mask = g_malloc0 ((gsize) w * h);
  for (i = 0; i < count; i++)
    {
      gint x0 = CLAMP (rects[i].x, 0, w), x1 = CLAMP (rects[i].x + rects[i].width, 0, w);
      gint y0 = CLAMP (rects[i].y, 0, h), y1 = CLAMP (rects[i].y + rects[i].height, 0, h);

      for (y = y0; y < y1; y++)
        memset (mask + (gsize) y * w + x0, 1, x1 - x0);
    }

  return mask;
}



/* El color de fondo de una imagen sin alfa: el que más se repite en las
 * cuatro esquinas (si empatan, el de arriba a la izquierda) */
static gulong
guess_background (XpGetPixel get_pixel, gpointer image, gulong rgb, gint w, gint h)
{
  gulong corners[4];
  gint best = 0, best_count = 0, i, j;

  corners[0] = get_pixel (image, 0, 0) & rgb;
  corners[1] = get_pixel (image, w - 1, 0) & rgb;
  corners[2] = get_pixel (image, 0, h - 1) & rgb;
  corners[3] = get_pixel (image, w - 1, h - 1) & rgb;

  for (i = 0; i < 4; i++)
    {
      gint count = 0;

      for (j = 0; j < 4; j++)
        count += corners[j] == corners[i];
      if (count > best_count)
        {
          best = i;
          best_count = count;
        }
    }

  return corners[best];
}



/* Marca en @bg los píxeles del color @key unidos al borde de la imagen
 * (de a 4 vecinos). Lo de ese color encerrado por el ícono no se marca. */
static void
fill_from_edges (XpGetPixel get_pixel, gpointer image, gulong rgb, gulong key,
                 guint8 *bg, gint w, gint h)
{
  GArray *stack = g_array_new (FALSE, FALSE, sizeof (gint));
  gint x, y;

#define PUSH(px, py)                                                     \
  G_STMT_START {                                                         \
    gint i_ = (py) * w + (px);                                           \
    if (!bg[i_] && (get_pixel (image, (px), (py)) & rgb) == key)         \
      {                                                                  \
        bg[i_] = 1;                                                      \
        g_array_append_val (stack, i_);                                  \
      }                                                                  \
  } G_STMT_END

  for (x = 0; x < w; x++)
    {
      PUSH (x, 0);
      PUSH (x, h - 1);
    }
  for (y = 0; y < h; y++)
    {
      PUSH (0, y);
      PUSH (w - 1, y);
    }

  while (stack->len > 0)
    {
      gint i = g_array_index (stack, gint, stack->len - 1);

      g_array_set_size (stack, stack->len - 1);
      x = i % w;
      y = i / w;
      if (x > 0)
        PUSH (x - 1, y);
      if (x < w - 1)
        PUSH (x + 1, y);
      if (y > 0)
        PUSH (x, y - 1);
      if (y < h - 1)
        PUSH (x, y + 1);
    }

#undef PUSH

  g_array_free (stack, TRUE);
}



guchar *
xp_convert_image (XpGetPixel get_pixel, gpointer image,
                  gulong rmask, gulong gmask, gulong bmask, gulong amask,
                  const guint8 *mask, gint w, gint h)
{
  gboolean use_alpha = amask != 0, any_opaque = FALSE;
  gulong rgb = rmask | gmask | bmask;
  guint8 *bg = NULL;
  guchar *out;
  gint x, y;

  g_return_val_if_fail (get_pixel != NULL && w > 0 && h > 0, NULL);

  /* Algunas apps dibujan sin alfa en una ventana de 32 bits */
  if (use_alpha)
    {
      gboolean any = FALSE;

      for (y = 0; y < h && !any; y++)
        for (x = 0; x < w && !any; x++)
          any = (get_pixel (image, x, y) & amask) != 0;
      use_alpha = any;
    }

  /* Sin alfa ni forma: adivinar el fondo (la app pinta sobre el fondo de
   * nuestro contenedor, o sobre uno propio) */
  if (!use_alpha && mask == NULL)
    {
      bg = g_malloc0 ((gsize) w * h);
      fill_from_edges (get_pixel, image, rgb,
                       guess_background (get_pixel, image, rgb, w, h), bg, w, h);
    }

  out = g_malloc ((gsize) w * h * 4);

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gsize i = (gsize) y * w + x;
        gulong pixel = get_pixel (image, x, y);
        guchar *o = out + i * 4;
        guint8 r = xp_channel (pixel, rmask);
        guint8 g = xp_channel (pixel, gmask);
        guint8 b = xp_channel (pixel, bmask);
        guint8 a;

        if (mask != NULL && !mask[i])
          {
            a = 0;
          }
        else if (use_alpha)
          {
            a = xp_channel (pixel, amask);
            if (a > 0 && a < 255)
              {
                /* X entrega alfa premultiplicado: deshacerlo */
                r = (guint8) MIN (255, r * 255 / a);
                g = (guint8) MIN (255, g * 255 / a);
                b = (guint8) MIN (255, b * 255 / a);
              }
          }
        else
          {
            a = bg != NULL && bg[i] ? 0 : 255;
          }

        if (a != 0)
          any_opaque = TRUE;

        o[0] = a;
        o[1] = r;
        o[2] = g;
        o[3] = b;
      }

  g_free (bg);

  if (!any_opaque)
    {
      g_free (out);
      return NULL;
    }

  return out;
}
