/*
 * Tray Overflow — proxy XEmbed: lógica que no depende de X11
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Todo lo de este archivo se puede probar sin pantalla ni apps reales
 * (tests/unit/test-proxy.c).
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Títulos que ponen los toolkits y no dicen nada de la app */
gboolean xp_title_is_generic (const gchar *title);

/* Nombre de una app Java a partir de su línea de comandos, tal como está
 * en /proc/PID/cmdline (argumentos separados por '\0'):
 * "java -jar JDownloader.jar" → "JDownloader". NULL si no es Java. */
gchar   *xp_java_app_name     (const gchar *cmdline, gsize length);

/* Un canal de color (0–255) de un píxel, según la máscara del visual */
guint8   xp_channel           (gulong pixel, gulong mask);

/* Lee un píxel de la imagen capturada */
typedef gulong (*XpGetPixel) (gpointer image, gint x, gint y);

/* Un rectángulo de la forma (Shape) de una ventana */
typedef struct
{
  gint x, y, width, height;
} XpRect;

/* Máscara de @w × @h (1 byte por píxel: 1 = parte del ícono) a partir de
 * la forma de la ventana. NULL si la forma es la ventana entera (no hay
 * forma) o no hay rectángulos. Liberar con g_free. */
guint8  *xp_shape_mask        (const XpRect *rects, gint count, gint w, gint h);

/* Convierte una imagen de @w × @h en ARGB no premultiplicado, en orden de
 * red (4 bytes por píxel; liberar con g_free). @amask == 0: la imagen no
 * tiene alfa. @mask (o NULL): forma de la ventana, de xp_shape_mask; lo
 * que queda afuera es transparente.
 *
 * Sin alfa ni forma, el fondo se adivina: el color que más se repite en
 * las cuatro esquinas, y solo donde toca el borde (relleno desde los
 * bordes). Así un ojo blanco sobre un fondo blanco no se agujerea.
 *
 * Devuelve NULL si es completamente transparente (pasa por un instante
 * con algunas apps, p. ej. Wine). */
guchar  *xp_convert_image     (XpGetPixel get_pixel, gpointer image,
                               gulong rmask, gulong gmask, gulong bmask, gulong amask,
                               const guint8 *mask, gint w, gint h);

G_END_DECLS
