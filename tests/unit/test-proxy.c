/*
 * Tray Overflow — pruebas de la lógica del proxy (sin pantalla)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xp-logic.h"

#include <string.h>



/* ── Nombre de las apps Java ─────────────────────────────────────────── */

/* Arma una línea de comandos como la de /proc/PID/cmdline: los
 * argumentos de @spaced (separados por espacios) pasan a estar separados
 * y terminados por '\0' */
static gchar *
java_name (const gchar *spaced)
{
  gsize length = strlen (spaced) + 1;
  gchar *cmdline = g_strdup (spaced);
  gchar *name;
  gsize i;

  for (i = 0; i < length; i++)
    if (cmdline[i] == ' ')
      cmdline[i] = '\0';

  name = xp_java_app_name (cmdline, length);
  g_free (cmdline);
  return name;
}



static void
check_java (const gchar *cmdline, const gchar *expected)
{
  gchar *name = java_name (cmdline);

  g_assert_cmpstr (name, ==, expected);
  g_free (name);
}



static void
test_java_name (void)
{
  check_java ("java -jar JDownloader.jar", "JDownloader");
  check_java ("/usr/lib/jvm/default/bin/java -Xmx512m -jar /opt/jd/JDownloader.jar", "JDownloader");
  check_java ("java -jar app", "app");
  check_java ("java com.frostwire.gui.Main", "Frostwire");
  check_java ("java -cp /opt/a.jar:/opt/b.jar org.example.tool.Main", "Example");
  check_java ("java --class-path x.jar -Dfoo=bar Main", "Main");
  check_java ("java -m biglybt/com.biglybt.ui.Main", "Biglybt");
  check_java ("java --module mimodulo", "Mimodulo");
  check_java ("java --add-opens java.base/java.lang=ALL-UNNAMED -jar X.jar", "X");

  /* No es Java, o no alcanza para saber */
  check_java ("python3 -jar X.jar", NULL);
  check_java ("java", NULL);
  check_java ("java -jar", NULL);
  check_java ("java -Xmx1g", NULL);
  g_assert_null (xp_java_app_name (NULL, 0));
  g_assert_null (xp_java_app_name ("", 0));

  /* Sin el '\0' final (lectura cortada): no lee de más */
  {
    const gchar raw[] = { 'j', 'a', 'v', 'a', '\0', '-', 'j', 'a', 'r', '\0', 'A', '.', 'j', 'a', 'r' };
    gchar *name = xp_java_app_name (raw, sizeof raw);

    g_assert_cmpstr (name, ==, "A");
    g_free (name);
  }
}



static void
test_generic_title (void)
{
  g_assert_true (xp_title_is_generic ("JavaEmbeddedFrame"));
  g_assert_false (xp_title_is_generic ("JDownloader"));
  g_assert_false (xp_title_is_generic (""));
  g_assert_false (xp_title_is_generic (NULL));
}



/* ── Imagen ──────────────────────────────────────────────────────────── */

#define R8 0xff0000UL
#define G8 0x00ff00UL
#define B8 0x0000ffUL
#define A8 0xff000000UL

typedef struct
{
  const guint32 *pixels;
  gint           width;
} Image;



static gulong
get_pixel (gpointer data, gint x, gint y)
{
  Image *image = data;

  return image->pixels[y * image->width + x];
}



static void
test_channel (void)
{
  g_assert_cmpuint (xp_channel (0x123456, R8), ==, 0x12);
  g_assert_cmpuint (xp_channel (0x123456, G8), ==, 0x34);
  g_assert_cmpuint (xp_channel (0x123456, B8), ==, 0x56);
  g_assert_cmpuint (xp_channel (0x123456, 0), ==, 0);

  /* 16 bits (5-6-5): el máximo de cada canal da 255 */
  g_assert_cmpuint (xp_channel (0xffff, 0xf800), ==, 255);
  g_assert_cmpuint (xp_channel (0xffff, 0x07e0), ==, 255);
  g_assert_cmpuint (xp_channel (0x0000, 0x001f), ==, 0);
  /* 30 bits (10 por canal): se queda con los 8 más altos */
  g_assert_cmpuint (xp_channel (0x3ffUL << 20, 0x3ffUL << 20), ==, 255);
}



static void
test_convert_alpha (void)
{
  /* opaco rojo, transparente, verde al 50 % premultiplicado, opaco azul */
  const guint32 pixels[] = { 0xffff0000, 0x00000000, 0x80008000, 0xff0000ff };
  Image image = { pixels, 2 };
  guchar *out = xp_convert_image (get_pixel, &image, R8, G8, B8, A8, NULL, 2, 2);
  const guchar expected[] = { 0xff, 0xff, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00,
                              0x80, 0x00, 0xff, 0x00,
                              0xff, 0x00, 0x00, 0xff };

  g_assert_nonnull (out);
  g_assert_cmpmem (out, 16, expected, 16);
  g_free (out);
}



/* 32 bits pero la app no usa el alfa: la esquina es el fondo */
static void
test_convert_no_alpha (void)
{
  const guint32 pixels[] = { 0x00c0c0c0, 0x00ff0000, 0x00c0c0c0, 0x0000ff00 };
  Image image = { pixels, 2 };
  guchar *out = xp_convert_image (get_pixel, &image, R8, G8, B8, A8, NULL, 2, 2);
  guchar *out24 = xp_convert_image (get_pixel, &image, R8, G8, B8, 0, NULL, 2, 2);

  g_assert_nonnull (out);
  g_assert_cmpuint (out[0], ==, 0);       /* esquina: transparente */
  g_assert_cmpuint (out[4], ==, 255);     /* rojo: opaco */
  g_assert_cmpuint (out[5], ==, 0xff);
  g_assert_cmpuint (out[8], ==, 0);       /* mismo color que la esquina */
  g_assert_cmpuint (out[12], ==, 255);
  g_assert_cmpuint (out[14], ==, 0xff);

  /* Igual que una ventana de 24 bits */
  g_assert_nonnull (out24);
  g_assert_cmpmem (out, 16, out24, 16);

  g_free (out);
  g_free (out24);
}



/* Todo transparente (o todo del color de la esquina): no hay imagen */
static void
test_convert_empty (void)
{
  const guint32 clear[] = { 0, 0, 0, 0 };
  const guint32 flat[] = { 0x00336699, 0x00336699, 0x00336699, 0x00336699 };
  Image image = { clear, 2 };

  g_assert_null (xp_convert_image (get_pixel, &image, R8, G8, B8, A8, NULL, 2, 2));
  image.pixels = flat;
  g_assert_null (xp_convert_image (get_pixel, &image, R8, G8, B8, 0, NULL, 2, 2));
}



/* Íconos sin alfa (0067) ───────────────────────────────────────────── */

#define W 0x00ffffffU   /* blanco: fondo */
#define K 0x00000000U   /* negro */
#define R 0x00ff0000U

/* Alfa de la imagen convertida en una cadena: '.' transparente, '#' opaco */
static gchar *
alpha_map (const guint32 *pixels, gint w, gint h, gulong amask, const guint8 *mask)
{
  Image image = { pixels, w };
  guchar *out = xp_convert_image (get_pixel, &image, R8, G8, B8, amask, mask, w, h);
  GString *map = g_string_new (NULL);
  gint i;

  for (i = 0; out != NULL && i < w * h; i++)
    g_string_append_c (map, out[i * 4] ? '#' : '.');
  g_free (out);
  return g_string_free (map, FALSE);
}



/* Un ojo blanco dentro de un ícono negro sobre fondo blanco: antes se
 * agujereaba (todo el blanco era transparente) */
static void
test_no_alpha_hole_kept (void)
{
  const guint32 pixels[] = { W, W, W, W, W,
                             W, K, K, K, W,
                             W, K, W, K, W,
                             W, K, K, K, W,
                             W, W, W, W, W };
  gchar *map = alpha_map (pixels, 5, 5, 0, NULL);

  g_assert_cmpstr (map, ==, "....."
                            ".###."
                            ".###."
                            ".###."
                            ".....");
  g_free (map);
}



/* El ícono toca una esquina: el fondo es el de las otras tres */
static void
test_no_alpha_corner_taken (void)
{
  const guint32 pixels[] = { R, R, W, W,
                             R, W, W, W,
                             W, W, K, W,
                             W, W, W, W };
  gchar *map = alpha_map (pixels, 4, 4, 0, NULL);

  g_assert_cmpstr (map, ==, "##.."
                            "#..."
                            "..#."
                            "....");
  g_free (map);

  /* Igual en una ventana de 32 bits sin alfa */
  map = alpha_map (pixels, 4, 4, A8, NULL);
  g_assert_cmpstr (map, ==, "##.."
                            "#..."
                            "..#."
                            "....");
  g_free (map);
}



static void
test_shape_mask (void)
{
  const XpRect full[] = { { 0, 0, 4, 4 } };
  const XpRect bigger[] = { { -2, -2, 10, 10 } };
  const XpRect parts[] = { { 1, 0, 2, 1 }, { 0, 1, 4, 2 }, { 3, 3, 9, 9 }, { -5, 3, 2, 1 } };
  const guint8 expected[] = { 0, 1, 1, 0,
                              1, 1, 1, 1,
                              1, 1, 1, 1,
                              0, 0, 0, 1 };
  guint8 *mask;

  g_assert_null (xp_shape_mask (full, 1, 4, 4));
  g_assert_null (xp_shape_mask (bigger, 1, 4, 4));
  g_assert_null (xp_shape_mask (NULL, 0, 4, 4));
  g_assert_null (xp_shape_mask (parts, 0, 4, 4));

  /* Rectángulos que se salen de la ventana: recortados, sin escribir afuera */
  mask = xp_shape_mask (parts, G_N_ELEMENTS (parts), 4, 4);
  g_assert_nonnull (mask);
  g_assert_cmpmem (mask, 16, expected, 16);
  g_free (mask);
}



/* Con forma (Wine): manda la forma, aunque el ícono tenga el color del
 * fondo adentro, y también sobre el alfa */
static void
test_shape_used (void)
{
  const guint32 pixels[] = { W, W, W, W,
                             W, W, W, W,
                             W, K, K, W,
                             W, W, W, W };
  const XpRect rects[] = { { 1, 1, 2, 2 } };
  guint8 *mask = xp_shape_mask (rects, 1, 4, 4);
  const guint32 argb[] = { 0xff000000, 0xff000000, 0xff000000, 0xff000000 };
  const guint8 corner[] = { 1, 0, 0, 0 };
  gchar *map = alpha_map (pixels, 4, 4, 0, mask);

  g_assert_cmpstr (map, ==, "...."
                            ".##."
                            ".##."
                            "....");
  g_free (map);

  map = alpha_map (argb, 2, 2, A8, corner);
  g_assert_cmpstr (map, ==, "#...");
  g_free (map);

  g_free (mask);
}



/* Una imagen grande no rompe el relleno (sin recursión) */
static void
test_no_alpha_large (void)
{
  gint n = 256, i;
  guint32 *pixels = g_new (guint32, n * n);
  gchar *map;

  for (i = 0; i < n * n; i++)
    pixels[i] = W;
  pixels[n * (n / 2) + n / 2] = K;

  map = alpha_map (pixels, n, n, 0, NULL);
  g_assert_cmpuint (strlen (map), ==, (gsize) n * n);
  g_assert_cmpint (map[n * (n / 2) + n / 2], ==, '#');
  g_assert_cmpint (map[0], ==, '.');

  g_free (map);
  g_free (pixels);
}



int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/proxy/java-name", test_java_name);
  g_test_add_func ("/proxy/generic-title", test_generic_title);
  g_test_add_func ("/proxy/channel", test_channel);
  g_test_add_func ("/proxy/convert-alpha", test_convert_alpha);
  g_test_add_func ("/proxy/convert-no-alpha", test_convert_no_alpha);
  g_test_add_func ("/proxy/convert-empty", test_convert_empty);
  g_test_add_func ("/proxy/no-alpha-hole-kept", test_no_alpha_hole_kept);
  g_test_add_func ("/proxy/no-alpha-corner-taken", test_no_alpha_corner_taken);
  g_test_add_func ("/proxy/no-alpha-large", test_no_alpha_large);
  g_test_add_func ("/proxy/shape-mask", test_shape_mask);
  g_test_add_func ("/proxy/shape-used", test_shape_used);

  return g_test_run ();
}
