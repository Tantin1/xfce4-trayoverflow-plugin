/*
 * Tray Overflow — pruebas de la parte de apps modernas (sin panel ni D-Bus)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../../src/to-sni.c"

#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>



/* ── Clave estable de cada app ───────────────────────────────────────── */

static void
test_generic_id (void)
{
  g_assert_true (item_id_is_generic (NULL));
  g_assert_true (item_id_is_generic (""));
  g_assert_true (item_id_is_generic ("chrome_status_icon_1"));
  g_assert_true (item_id_is_generic ("12345"));

  g_assert_false (item_id_is_generic ("TelegramDesktop"));
  g_assert_false (item_id_is_generic ("nm-applet"));
  g_assert_false (item_id_is_generic ("123abc"));
}



static void
test_interpreter (void)
{
  g_assert_true (is_interpreter ("python"));
  g_assert_true (is_interpreter ("python3.12"));
  g_assert_true (is_interpreter ("bash"));
  g_assert_true (is_interpreter ("java"));

  g_assert_false (is_interpreter ("telegram-desktop"));
  g_assert_false (is_interpreter ("shelly"));       /* empieza con "sh" */
  g_assert_false (is_interpreter ("nodepad"));
}



/* Lanza @argv y devuelve su PID una vez que el programa está corriendo */
static GPid
spawn (const gchar *const *argv)
{
  GPid pid = 0;
  GError *error = NULL;

  /* Sin heredar la salida: si no, al fallar la prueba meson espera a
   * que el programa termine (20 s) */
  g_spawn_async (NULL, (gchar **) argv, NULL,
                 G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL
                 | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, &pid, &error);
  g_assert_no_error (error);
  return pid;
}



/* Nombre del programa de @pid, una vez que dejó de ser una copia de esta
 * prueba: justo después de lanzarlo puede no haber cambiado todavía (el
 * hijo existe antes de ejecutar el programa), y mientras se reemplaza la
 * línea de comandos puede leerse vacía un instante (se usa entonces el
 * nombre corto, "sh"). Espera hasta 2 s */
static gchar *
exe_name_after_exec (GPid pid)
{
  gchar *self = exe_name_from_pid (getpid ());
  gchar *name = NULL;
  gint i;

  for (i = 0; i < 200; i++)
    {
      g_free (name);
      name = exe_name_from_pid (pid);
      if (name != NULL && g_strcmp0 (name, self) != 0 && g_strcmp0 (name, "sh") != 0)
        break;
      g_usleep (10000);
    }

  g_free (self);
  return name;
}



static void
stop (GPid pid)
{
  kill (pid, SIGKILL);
  g_spawn_close_pid (pid);
}



static void
test_exe_name (void)
{
  gchar *dir = g_dir_make_tmp ("trayoverflow-exe.XXXXXX", NULL);
  gchar *script = g_build_filename (dir, "mi-app", NULL);
  const gchar *direct[] = { "/bin/sleep", "20", NULL };
  const gchar *via_sh[] = { "/bin/sh", script, NULL };
  const gchar *via_sh_opt[] = { "/bin/sh", "-e", script, NULL };
  gchar *name;
  GPid pid;

  g_assert_nonnull (dir);
  g_assert_true (g_file_set_contents (script, "exec /bin/sleep 20\n", -1, NULL));

  pid = spawn (direct);
  name = exe_name_after_exec (pid);
  g_assert_cmpstr (name, ==, "sleep");
  g_free (name);
  stop (pid);

  /* Intérprete: cuenta el programa, no el intérprete (o sleep, si el
   * script ya se reemplazó por él) */
  pid = spawn (via_sh);
  name = exe_name_after_exec (pid);
  if (g_strcmp0 (name, "mi-app") != 0 && g_strcmp0 (name, "sleep") != 0)
    g_error ("nombre inesperado: %s", name != NULL ? name : "(ninguno)");
  g_free (name);
  stop (pid);

  pid = spawn (via_sh_opt);
  name = exe_name_after_exec (pid);
  if (g_strcmp0 (name, "mi-app") != 0 && g_strcmp0 (name, "sleep") != 0)
    g_error ("nombre inesperado: %s", name != NULL ? name : "(ninguno)");
  g_free (name);
  stop (pid);

  /* Proceso que no existe: sin nombre, sin romperse */
  name = exe_name_from_pid (0x7ffffff0);
  g_assert_null (name);

  g_unlink (script);
  g_rmdir (dir);
  g_free (script);
  g_free (dir);
}



/* ── Menú ────────────────────────────────────────────────────────────── */

static void
test_menu_path (void)
{
  g_assert_true (menu_path_is_valid ("/MenuBar"));
  g_assert_true (menu_path_is_valid ("/org/ayatana/NotificationItem/app/Menu"));

  g_assert_false (menu_path_is_valid (NULL));
  g_assert_false (menu_path_is_valid (""));
  g_assert_false (menu_path_is_valid ("/"));
  g_assert_false (menu_path_is_valid ("/NO_DBUSMENU"));
  g_assert_false (menu_path_is_valid ("no es una ruta"));
}



/* ── Imágenes que manda la app (IconPixmap) ──────────────────────────── */

static void
add_pixmap (GVariantBuilder *builder, gint width, gint height, gsize bytes, guchar fill)
{
  guchar *data = g_malloc (MAX (bytes, 1));

  memset (data, fill, bytes);
  g_variant_builder_add (builder, "(ii@ay)", width, height,
                         g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, data, bytes, 1));
  g_free (data);
}



static void
test_pixmaps (void)
{
  GVariantBuilder builder;
  GVariant *value;
  GPtrArray *array;
  const guchar argb[] = { 0x80, 0x11, 0x22, 0x33 };   /* A R G B */
  const guchar *pixels;
  guint i;

  g_assert_null (parse_pixmaps (NULL));

  /* Otro tipo de dato */
  value = g_variant_ref_sink (g_variant_new_string ("no"));
  g_assert_null (parse_pixmaps (value));
  g_variant_unref (value);

  /* Lista vacía */
  value = g_variant_ref_sink (g_variant_new ("a(iiay)", NULL));
  g_assert_null (parse_pixmaps (value));
  g_variant_unref (value);

  /* Una imagen de 1x1: ARGB → RGBA */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiay)"));
  g_variant_builder_add (&builder, "(ii@ay)", 1, 1,
                         g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, argb, 4, 1));
  value = g_variant_ref_sink (g_variant_builder_end (&builder));
  array = parse_pixmaps (value);
  g_assert_nonnull (array);
  g_assert_cmpuint (array->len, ==, 1);
  pixels = gdk_pixbuf_read_pixels (g_ptr_array_index (array, 0));
  g_assert_cmpuint (pixels[0], ==, 0x11);
  g_assert_cmpuint (pixels[1], ==, 0x22);
  g_assert_cmpuint (pixels[2], ==, 0x33);
  g_assert_cmpuint (pixels[3], ==, 0x80);
  g_ptr_array_unref (array);
  g_variant_unref (value);

  /* Imágenes mentirosas: se descartan sin leer de más */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiay)"));
  add_pixmap (&builder, 16, 16, 16 * 16 * 4 - 1, 0);   /* faltan datos */
  add_pixmap (&builder, 0, 16, 64, 0);
  add_pixmap (&builder, -4, 4, 64, 0);
  add_pixmap (&builder, 2000, 1, 2000 * 4, 0);         /* demasiado grande */
  add_pixmap (&builder, 0x7fffffff, 0x7fffffff, 16, 0);
  add_pixmap (&builder, 4, 4, 0, 0);
  value = g_variant_ref_sink (g_variant_builder_end (&builder));
  g_assert_null (parse_pixmaps (value));
  g_variant_unref (value);

  /* Una buena entre malas */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiay)"));
  add_pixmap (&builder, 16, 16, 10, 0);
  add_pixmap (&builder, 22, 22, 22 * 22 * 4, 0xff);
  value = g_variant_ref_sink (g_variant_builder_end (&builder));
  array = parse_pixmaps (value);
  g_assert_nonnull (array);
  g_assert_cmpuint (array->len, ==, 1);
  g_assert_cmpint (gdk_pixbuf_get_width (g_ptr_array_index (array, 0)), ==, 22);
  g_ptr_array_unref (array);
  g_variant_unref (value);

  /* Tope de imágenes por ícono (0054) */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiay)"));
  for (i = 0; i < SNI_PIXMAPS_MAX + 20; i++)
    add_pixmap (&builder, 8, 8, 8 * 8 * 4, 0xff);
  value = g_variant_ref_sink (g_variant_builder_end (&builder));
  array = parse_pixmaps (value);
  g_assert_nonnull (array);
  g_assert_cmpuint (array->len, ==, SNI_PIXMAPS_MAX);
  g_ptr_array_unref (array);
  g_variant_unref (value);
}



/* ── Texto junto al ícono (XAyatanaLabel) ────────────────────────────── */

static void
apply_label (SniItem *item, const gchar *label, const gchar *guide)
{
  GVariantBuilder builder;
  GVariant *dict;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "Id", g_variant_new_string ("app"));
  if (label != NULL)
    g_variant_builder_add (&builder, "{sv}", "XAyatanaLabel", g_variant_new_string (label));
  if (guide != NULL)
    g_variant_builder_add (&builder, "{sv}", "XAyatanaLabelGuide", g_variant_new_string (guide));
  dict = g_variant_ref_sink (g_variant_builder_end (&builder));

  sni_item_apply_properties (item, dict);
  g_variant_unref (dict);
}



static void
test_label (void)
{
  SniItem *item = g_object_new (SNI_TYPE_ITEM, NULL);
  TrayItem *tray_item = TRAY_ITEM (item);
  gchar *longer = g_strnfill (200, 'x');

  apply_label (item, NULL, NULL);
  g_assert_null (tray_item_get_label (tray_item));
  tray_item_emit_changed (tray_item);

  apply_label (item, "42 °C", "100 °C");
  g_assert_cmpstr (tray_item_get_label (tray_item), ==, "42 °C");
  g_assert_cmpstr (tray_item_get_label_guide (tray_item), ==, "100 °C");
  g_assert_true (tray_item_emit_changed (tray_item));

  /* El mismo texto otra vez: nada cambió */
  apply_label (item, "42 °C", "100 °C");
  g_assert_false (tray_item_emit_changed (tray_item));

  /* Una sola línea, sin espacios sobrantes, de largo acotado */
  apply_label (item, "  es\n", NULL);
  g_assert_cmpstr (tray_item_get_label (tray_item), ==, "es");
  g_assert_null (tray_item_get_label_guide (tray_item));

  apply_label (item, "a\nb\tc", NULL);
  g_assert_cmpstr (tray_item_get_label (tray_item), ==, "a b c");

  apply_label (item, longer, longer);
  g_assert_cmpint (g_utf8_strlen (tray_item_get_label (tray_item), -1), ==, 32);
  g_assert_cmpint (g_utf8_strlen (tray_item_get_label_guide (tray_item), -1), ==, 32);

  /* Texto que no es UTF-8 válido (D-Bus no lo deja pasar, pero otro
   * origen podría): no se rompe */
  tray_item_set_label (tray_item, "caf\xe9", NULL);
  g_assert_true (g_utf8_validate (tray_item_get_label (tray_item), -1, NULL));

  /* Vacío o solo espacios: sin texto (y sin guía) */
  apply_label (item, "   ", "guía");
  g_assert_null (tray_item_get_label (tray_item));
  g_assert_null (tray_item_get_label_guide (tray_item));
  apply_label (item, "", NULL);
  g_assert_null (tray_item_get_label (tray_item));

  g_free (longer);
  g_object_unref (item);
}



int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sni/generic-id", test_generic_id);
  g_test_add_func ("/sni/interpreter", test_interpreter);
  g_test_add_func ("/sni/exe-name", test_exe_name);
  g_test_add_func ("/sni/menu-path", test_menu_path);
  g_test_add_func ("/sni/pixmaps", test_pixmaps);
  g_test_add_func ("/sni/label", test_label);

  return g_test_run ();
}
