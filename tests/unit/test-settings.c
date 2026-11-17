/*
 * Tray Overflow — pruebas de la configuración (sin panel)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Necesita un xfconfd propio: se corre con tests/unit/con-dbus.sh
 * (lo hace solo "meson test").
 */
#include "../../src/to-settings.c"

#define BASE "/plugins/plugin-99"

static XfconfChannel *channel;



/* Lo mismo que tray_settings_new, sin un plugin del panel */
static TraySettings *
settings_new (void)
{
  TraySettings *s;

  g_assert_true (xfconf_init (NULL));
  s = g_object_new (TRAY_TYPE_SETTINGS, NULL);
  s->channel = channel;
  s->base = g_strdup (BASE);
  load_all (s);
  mark_saved (s);
  s->property_changed_id = g_signal_connect (s->channel, "property-changed",
                                             G_CALLBACK (property_changed), s);
  return s;
}



static gboolean
quit_loop (gpointer loop)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}



/* Deja correr el bucle principal (avisos de xfconf, temporizadores) */
static void
spin (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (ms, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}



/* Deja que xfconf termine las escrituras en curso. Las escrituras son
 * asíncronas: dos seguidas en la misma opción, sin pasar por el bucle
 * principal, hacen que xfconf 4.20 cancele la primera y avise "Operation
 * was cancelled" (en una prueba, un aviso es un error). En el plugin no
 * pasa: el bucle corre siempre y hay un solo guardado cada 2 s. */
#define SETTLE_MS 150

static void
settle (void)
{
  spin (SETTLE_MS);
}



static void
reset_channel (void)
{
  settle ();
  xfconf_channel_reset_property (channel, BASE, TRUE);
  settle ();
}



static gboolean
has (const gchar *name)
{
  gchar *p = g_strconcat (BASE, "/", name, NULL);
  gboolean result = xfconf_channel_has_property (channel, p);

  g_free (p);
  return result;
}



static gint
stored_int (const gchar *name)
{
  gchar *p = g_strconcat (BASE, "/", name, NULL);
  gint value = xfconf_channel_get_int (channel, p, -1);

  g_free (p);
  return value;
}



static void
set_int_outside (const gchar *name, gint value)
{
  gchar *p = g_strconcat (BASE, "/", name, NULL);

  xfconf_channel_set_int (channel, p, value);
  g_free (p);
  settle ();
}



/* ── Valores por defecto ─────────────────────────────────────────────── */

static void
test_defaults (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();

  g_assert_cmpint (tray_settings_get_panel_icon_size (s), ==, 22);
  g_assert_cmpint (tray_settings_get_popup_icon_size (s), ==, 22);
  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, 4);
  g_assert_cmpint (tray_settings_get_passive_mode (s), ==, TRAY_PASSIVE_HIDE);
  g_assert_true (tray_settings_get_symbolic_icons (s));
  g_assert_true (tray_settings_get_legacy_tray (s));
  g_assert_false (tray_settings_get_new_in_popup (s));
  g_assert_true (tray_settings_get_show_labels (s));
  g_assert_cmpint (tray_settings_get_button_min_size (s), ==, TRAY_BUTTON_MIN_DEFAULT);

  g_object_unref (s);
}



/* Valores fuera de rango guardados a mano: se acomodan al leer */
static void
test_clamp (void)
{
  TraySettings *s;

  reset_channel ();
  set_int_outside ("popup-columns", 500);
  set_int_outside ("popup-icon-size", 1);
  set_int_outside ("passive-mode", 9);
  s = settings_new ();

  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, TRAY_COLUMNS_MAX);
  g_assert_cmpint (tray_settings_get_popup_icon_size (s), ==, TRAY_ICON_SIZE_MIN);
  g_assert_cmpint (tray_settings_get_passive_mode (s), ==, TRAY_PASSIVE_SHOW);

  g_object_unref (s);
}



/* ── Guardar solo lo modificado (0060) ───────────────────────────────── */

static void
test_save_only_changed (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();

  /* Sin cambios no se escribe nada */
  tray_settings_set_popup_columns (s, 4);
  tray_settings_flush (s);
  settle ();
  g_assert_false (has ("popup-columns"));

  tray_settings_set_popup_columns (s, 6);
  tray_settings_remember (s, "app-a", "App A", "");
  tray_settings_flush (s);
  settle ();

  g_assert_cmpint (stored_int ("popup-columns"), ==, 6);
  g_assert_true (has ("known-items"));
  /* Lo que nadie tocó no queda escrito: sigue el valor por defecto */
  g_assert_false (has ("panel-icon-size"));
  g_assert_false (has ("popup-icon-size"));
  g_assert_false (has ("symbolic-icons"));
  g_assert_false (has ("legacy-tray"));
  g_assert_false (has ("show-labels"));
  g_assert_false (has ("button-min-size"));
  g_assert_false (has ("hidden-items"));

  g_object_unref (s);

  /* Y se lee de vuelta */
  s = settings_new ();
  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, 6);
  g_assert_cmpint (tray_settings_get_index (s, "app-a"), ==, 0);
  g_object_unref (s);
}



/* Un cambio hecho desde afuera mientras hay un guardado nuestro pendiente
 * en OTRA opción: quedan los dos */
static void
test_external_change_kept (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();

  tray_settings_set_popup_columns (s, 7);          /* pendiente (2 s) */
  g_assert_cmpuint (s->save_id, !=, 0);
  set_int_outside ("popup-spacing", 9);            /* desde afuera */
  spin (RELOAD_DELAY_MS + 500);

  g_assert_cmpint (tray_settings_get_popup_spacing (s), ==, 9);
  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, 7);
  g_assert_cmpint (stored_int ("popup-spacing"), ==, 9);
  g_assert_cmpint (stored_int ("popup-columns"), ==, 7);

  /* Al cerrar tampoco se pisa lo de afuera */
  set_int_outside ("panel-spacing", 5);
  g_object_unref (s);
  g_assert_cmpint (stored_int ("panel-spacing"), ==, 5);
}



/* En la misma opción gana lo nuestro */
static void
test_same_option_ours_wins (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();

  tray_settings_set_popup_columns (s, 7);
  set_int_outside ("popup-columns", 2);
  spin (RELOAD_DELAY_MS + 500);

  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, 7);
  g_assert_cmpint (stored_int ("popup-columns"), ==, 7);

  g_object_unref (s);
}



static guint changed_count;

static void
count_changed (TraySettings *s, gpointer data)
{
  changed_count++;
}



/* Cambio desde afuera sin nada pendiente: se aplica y avisa una vez; el
 * eco de nuestras propias escrituras no avisa */
static void
test_external_reload (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();
  g_signal_connect (s, "changed", G_CALLBACK (count_changed), NULL);

  changed_count = 0;
  set_int_outside ("popup-columns", 3);
  spin (RELOAD_DELAY_MS + 400);
  g_assert_cmpint (tray_settings_get_popup_columns (s), ==, 3);
  g_assert_cmpuint (changed_count, ==, 1);

  changed_count = 0;
  tray_settings_set_popup_spacing (s, 11);   /* avisa una vez, al cambiar */
  tray_settings_flush (s);
  settle ();
  spin (RELOAD_DELAY_MS + 400);
  g_assert_cmpuint (changed_count, ==, 1);

  g_object_unref (s);
}



/* ── Ocultar, mostrar, inactivos ─────────────────────────────────────── */

static void
test_hidden (void)
{
  TraySettings *s;

  reset_channel ();
  s = settings_new ();

  g_assert_false (tray_settings_is_hidden (s, "a", FALSE));
  tray_settings_set_hidden (s, "a", TRUE);
  g_assert_true (tray_settings_is_hidden (s, "a", FALSE));
  tray_settings_set_hidden (s, "a", FALSE);
  g_assert_false (tray_settings_is_hidden (s, "a", FALSE));

  /* Inactivos al popup, salvo los puestos a mano en el panel */
  tray_settings_set_passive_mode (s, TRAY_PASSIVE_POPUP);
  g_assert_true (tray_settings_is_hidden (s, "b", TRUE));
  g_assert_false (tray_settings_is_hidden (s, "b", FALSE));
  g_assert_false (tray_settings_is_hidden (s, "a", TRUE));

  tray_settings_set_passive_mode (s, TRAY_PASSIVE_SHOW);
  g_assert_false (tray_settings_is_hidden (s, "b", TRUE));

  g_object_unref (s);
}



/* ── Registro: apps nuevas, orden, olvidar ───────────────────────────── */

static void
test_known (void)
{
  TraySettings *s;
  GPtrArray *known;

  reset_channel ();
  s = settings_new ();

  g_assert_true (tray_settings_remember (s, "a", "A", "icon-a"));
  g_assert_false (tray_settings_remember (s, "a", "", ""));   /* no pisa con vacío */
  g_assert_true (tray_settings_remember (s, "b", "B", ""));
  g_assert_true (tray_settings_remember (s, "c", "C", ""));

  known = tray_settings_get_known (s);
  g_assert_cmpstr (((TrayKnown *) g_ptr_array_index (known, 0))->title, ==, "A");
  g_assert_cmpstr (((TrayKnown *) g_ptr_array_index (known, 0))->icon_name, ==, "icon-a");

  tray_settings_move (s, "c", "a", FALSE);      /* c antes de a */
  g_assert_cmpint (tray_settings_get_index (s, "c"), ==, 0);
  tray_settings_move (s, "c", "b", TRUE);       /* c después de b */
  g_assert_cmpint (tray_settings_get_index (s, "c"), ==, 2);
  tray_settings_move (s, "a", NULL, FALSE);     /* a al final */
  g_assert_cmpint (tray_settings_get_index (s, "a"), ==, 2);

  /* Apps nuevas al popup: solo las que aparecen por primera vez */
  tray_settings_set_new_in_popup (s, TRUE);
  tray_settings_remember (s, "d", "D", "");
  g_assert_true (tray_settings_is_hidden (s, "d", FALSE));
  g_assert_false (tray_settings_is_hidden (s, "a", FALSE));

  /* Una app olvidada que vuelve cuenta como nueva */
  tray_settings_set_hidden (s, "d", FALSE);
  tray_settings_forget (s, "d");
  g_assert_cmpint (tray_settings_get_index (s, "d"), ==, -1);
  tray_settings_remember (s, "d", "D", "");
  g_assert_true (tray_settings_is_hidden (s, "d", FALSE));

  g_object_unref (s);
}



static gboolean
first_is_running (const gchar *key, gpointer data)
{
  return strcmp (key, "app-0") == 0;
}



static void
fill (TraySettings *s, guint n)
{
  guint i;

  for (i = 0; i < n; i++)
    {
      gchar *key = g_strdup_printf ("app-%u", i);

      tray_settings_remember (s, key, key, "");
      g_free (key);
    }
}



/* Límite del registro (0036) */
static void
test_known_limit (void)
{
  TraySettings *s;

  /* Sin preferencias: se va la más antigua */
  reset_channel ();
  s = settings_new ();
  fill (s, MAX_KNOWN + 1);
  g_assert_cmpuint (tray_settings_get_known (s)->len, ==, MAX_KNOWN);
  g_assert_cmpint (tray_settings_get_index (s, "app-0"), ==, -1);
  g_assert_cmpint (tray_settings_get_index (s, "app-1"), ==, 0);
  g_object_unref (s);

  /* Una app abierta, una visible a mano y una con ícono elegido no se van */
  reset_channel ();
  s = settings_new ();
  tray_settings_set_running_func (s, first_is_running, NULL);
  fill (s, 4);
  tray_settings_set_hidden (s, "app-1", FALSE);
  tray_settings_set_icon_override (s, "app-2", "some-icon");
  fill (s, MAX_KNOWN + 1);
  g_assert_cmpuint (tray_settings_get_known (s)->len, ==, MAX_KNOWN);
  g_assert_cmpint (tray_settings_get_index (s, "app-0"), >=, 0);
  g_assert_cmpint (tray_settings_get_index (s, "app-1"), >=, 0);
  g_assert_cmpint (tray_settings_get_index (s, "app-2"), >=, 0);
  g_assert_cmpint (tray_settings_get_index (s, "app-3"), ==, -1);
  g_object_unref (s);

  /* Con "apps nuevas al popup" todas quedan ocultas: el límite igual vale */
  reset_channel ();
  s = settings_new ();
  tray_settings_set_new_in_popup (s, TRUE);
  fill (s, MAX_KNOWN + 5);
  g_assert_cmpuint (tray_settings_get_known (s)->len, ==, MAX_KNOWN);
  g_assert_cmpint (tray_settings_get_index (s, "app-0"), ==, -1);
  g_object_unref (s);
}



/* ── Ícono elegido y lista de íconos por defecto ─────────────────────── */

static void
test_icon_defaults (void)
{
  const gchar *dir = g_getenv ("TRAYOVERFLOW_TEST_DIR");
  gchar *conf, *png, *contents;
  TraySettings *s;

  if (dir == NULL)
    dir = g_get_tmp_dir ();

  png = g_build_filename (dir, "existe.png", NULL);
  conf = g_build_filename (dir, "icon-defaults.conf", NULL);
  g_assert_true (g_file_set_contents (png, "x", -1, NULL));

  contents = g_strdup_printf ("# comentario\n"
                              "\n"
                              "linea sin igual\n"
                              "*=%s\n"
                              "vacia=\n"
                              "  exacta = /no/existe.png ; %s \n"
                              "org-prefijo-*=%s\n"
                              "org-prefijo-otra=/no/existe.png\n"
                              "rota=/no/existe.png;/tampoco.png\n",
                              png, png, png);
  g_assert_true (g_file_set_contents (conf, contents, -1, NULL));
  g_free (contents);

  /* Lista propia en vez de las instaladas */
  g_assert_null (icon_defaults);
  icon_defaults = g_ptr_array_new_with_free_func (icon_default_free);
  icon_defaults_load_file (conf);
  g_assert_cmpuint (icon_defaults->len, ==, 4);

  g_assert_cmpstr (default_icon_for ("exacta"), ==, png);        /* el primero que existe */
  g_assert_cmpstr (default_icon_for ("org-prefijo-app"), ==, png);
  g_assert_cmpstr (default_icon_for ("org-prefijo-otra"), ==, png); /* la primera regla decide */
  g_assert_null (default_icon_for ("rota"));
  g_assert_null (default_icon_for ("exacta-no"));
  g_assert_null (default_icon_for ("cualquiera"));               /* "*" sola se ignora */

  /* Prioridad: elegido a mano > "ícono propio" > lista > el de la app */
  reset_channel ();
  s = settings_new ();
  g_assert_cmpstr (tray_settings_get_icon_override (s, "exacta"), ==, png);
  g_assert_null (tray_settings_get_icon_choice (s, "exacta"));

  tray_settings_set_icon_override (s, "exacta", "a-mano");
  g_assert_cmpstr (tray_settings_get_icon_override (s, "exacta"), ==, "a-mano");

  tray_settings_set_icon_override (s, "exacta", TRAY_ICON_OWN);
  g_assert_null (tray_settings_get_icon_override (s, "exacta"));
  g_assert_cmpstr (tray_settings_get_icon_choice (s, "exacta"), ==, TRAY_ICON_OWN);

  tray_settings_set_icon_override (s, "exacta", NULL);
  g_assert_cmpstr (tray_settings_get_icon_override (s, "exacta"), ==, png);
  g_object_unref (s);

  g_free (conf);
  g_free (png);
}



/* La copia en caché se calcula desde la clave (0035) */
static void
test_cache_path (void)
{
  gchar *a = tray_settings_cached_icon_path ("app");
  gchar *b = tray_settings_cached_icon_path ("../../etc/passwd");
  gchar *dir = g_build_filename (g_get_user_cache_dir (), "xfce4", "trayoverflow", NULL);
  gchar *name;

  g_assert_true (g_str_has_prefix (a, dir));
  g_assert_true (g_str_has_prefix (b, dir));
  name = g_path_get_basename (b);
  g_assert_cmpuint (strlen (name), ==, 40 + 4);   /* sha1 + ".png" */
  g_assert_null (strchr (name, '/'));

  g_free (name);
  g_free (dir);
  g_free (a);
  g_free (b);
}



int
main (int argc, char **argv)
{
  gint result;

  g_test_init (&argc, &argv, NULL);

  if (!xfconf_init (NULL))
    {
      g_printerr ("xfconf no disponible: prueba salteada\n");
      return 77;
    }
  channel = xfconf_channel_get ("xfce4-panel");
  /* ¿Hay un xfconfd que responda? */
  xfconf_channel_set_int (channel, BASE "/probe", 1);
  spin (300);
  if (!xfconf_channel_has_property (channel, BASE "/probe"))
    {
      g_printerr ("xfconfd no responde: prueba salteada\n");
      return 77;
    }

  g_test_add_func ("/settings/defaults", test_defaults);
  g_test_add_func ("/settings/clamp", test_clamp);
  g_test_add_func ("/settings/save-only-changed", test_save_only_changed);
  g_test_add_func ("/settings/external-change-kept", test_external_change_kept);
  g_test_add_func ("/settings/same-option-ours-wins", test_same_option_ours_wins);
  g_test_add_func ("/settings/external-reload", test_external_reload);
  g_test_add_func ("/settings/hidden", test_hidden);
  g_test_add_func ("/settings/known", test_known);
  g_test_add_func ("/settings/known-limit", test_known_limit);
  g_test_add_func ("/settings/icon-defaults", test_icon_defaults);
  g_test_add_func ("/settings/cache-path", test_cache_path);

  result = g_test_run ();
  reset_channel ();
  return result;
}
