/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-legacy.h"

#include <gio/gio.h>
#include <signal.h>
#include <string.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#define PROXY_WINDOW_NAME   "trayoverflow-xembed-proxy"
#define RESPAWN_DELAY_MS    2000
#define QUICK_EXIT_US       (5 * G_USEC_PER_SEC)
#define MAX_QUICK_FAILURES  5

static GHashTable  *wanters = NULL;     /* instancias que lo piden */
static GSubprocess *proxy = NULL;
static gint64       started_at = 0;
static guint        respawn_id = 0;
static gint         quick_failures = 0;
static gboolean     failed = FALSE;

static void proxy_start (void);



/* ¿La sesión es X11? En Wayland, las apps antiguas corren en XWayland */
static gboolean
session_is_x11 (void)
{
#ifdef GDK_WINDOWING_X11
  GdkDisplay *display = gdk_display_get_default ();

  return display != NULL && GDK_IS_X11_DISPLAY (display);
#else
  return FALSE;
#endif
}



/* Wayland: XWayland disponible (el compositor exporta DISPLAY; puede
 * arrancarlo recién cuando alguien se conecta) */
static gboolean
xwayland_available (void)
{
  const gchar *display = g_getenv ("DISPLAY");

  return display != NULL && *display != '\0';
}



gboolean
tray_legacy_available (void)
{
#if HAVE_XEMBED_PROXY
  return g_file_test (PROXY_PATH, G_FILE_TEST_IS_EXECUTABLE);
#else
  return FALSE;
#endif
}



/* Corre en el hijo, entre fork y exec: si el panel muere (incluso por un
 * cierre forzado), el proxy recibe SIGTERM y suelta las ventanas */
static void
child_setup (gpointer user_data)
{
#ifdef __linux__
  prctl (PR_SET_PDEATHSIG, SIGTERM);
#endif
}



static gboolean
respawn_cb (gpointer user_data)
{
  respawn_id = 0;
  proxy_start ();
  return G_SOURCE_REMOVE;
}



static void
proxy_exited (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  GSubprocess *process = G_SUBPROCESS (source);

  g_subprocess_wait_finish (process, result, NULL);

  /* Un proceso viejo que detuvimos a propósito: nada que hacer */
  if (process != proxy)
    return;

  g_clear_object (&proxy);

  if (wanters == NULL || g_hash_table_size (wanters) == 0)
    return;

  /* Se cerró solo: reiniciarlo, salvo que falle una y otra vez apenas
   * arranca (p. ej. un servidor X sin Composite) */
  if (g_get_monotonic_time () - started_at < QUICK_EXIT_US)
    quick_failures++;
  else
    quick_failures = 0;

  if (quick_failures >= MAX_QUICK_FAILURES)
    {
      g_warning ("Tray Overflow: el proxy de apps antiguas falla al arrancar; "
                 "se desactiva hasta reiniciar el panel");
      failed = TRUE;
      return;
    }

  if (respawn_id == 0)
    respawn_id = g_timeout_add (RESPAWN_DELAY_MS, respawn_cb, NULL);
}



static void
proxy_start (void)
{
  GSubprocessLauncher *launcher;
  GError *error = NULL;

  gboolean wayland = !session_is_x11 ();

  if (proxy != NULL || failed || !tray_legacy_available ())
    return;

  if (wayland && !xwayland_available ())
    return;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_child_setup (launcher, child_setup, NULL, NULL);
  /* En Wayland el proxy no ve el puntero real: usa la posición del
   * ícono que le mandamos en cada clic */
  proxy = g_subprocess_launcher_spawn (launcher, &error, PROXY_PATH,
                                       wayland ? "--wayland" : NULL, NULL);
  g_object_unref (launcher);

  if (proxy == NULL)
    {
      g_warning ("Tray Overflow: no se pudo iniciar %s: %s", PROXY_PATH, error->message);
      g_error_free (error);
      failed = TRUE;
      return;
    }

  started_at = g_get_monotonic_time ();
  g_subprocess_wait_async (proxy, NULL, proxy_exited, NULL);
}



static void
proxy_stop (void)
{
  if (respawn_id != 0)
    {
      g_source_remove (respawn_id);
      respawn_id = 0;
    }

  if (proxy != NULL)
    {
      GSubprocess *process = g_steal_pointer (&proxy);

      /* SIGTERM: el proxy devuelve las ventanas y libera la bandeja */
      g_subprocess_send_signal (process, SIGTERM);
      g_object_unref (process);
    }

  quick_failures = 0;
}



void
tray_legacy_set_wanted (gpointer owner,
                        gboolean wanted)
{
  if (wanters == NULL)
    wanters = g_hash_table_new (g_direct_hash, g_direct_equal);

  if (wanted)
    g_hash_table_add (wanters, owner);
  else
    g_hash_table_remove (wanters, owner);

  if (g_hash_table_size (wanters) > 0)
    proxy_start ();
  else
    proxy_stop ();
}



#ifdef GDK_WINDOWING_X11
/* Dueño de la bandeja clásica en @xdisplay. Los errores de X (la ventana
 * puede desaparecer en el medio) los atrapa quien llama. */
static gint
selection_owner (Display  *xdisplay,
                 gboolean *is_proxy)
{
  gchar *name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d", DefaultScreen (xdisplay));
  Atom atom = XInternAtom (xdisplay, name, False);
  Window owner = XGetSelectionOwner (xdisplay, atom);
  gchar *window_name = NULL;

  g_free (name);
  if (owner == None)
    return 0;

  if (is_proxy != NULL && XFetchName (xdisplay, owner, &window_name) && window_name != NULL)
    {
      *is_proxy = strcmp (window_name, PROXY_WINDOW_NAME) == 0;
      XFree (window_name);
    }
  return 1;
}



static int
ignore_x_error (Display     *xdisplay,
                XErrorEvent *event)
{
  return 0;
}
#endif



gint
tray_legacy_tray_owner (GtkWidget *reference,
                        gboolean  *is_proxy)
{
  if (is_proxy != NULL)
    *is_proxy = FALSE;

#ifdef GDK_WINDOWING_X11
  {
    GdkDisplay *display = gtk_widget_get_display (reference);
    gint result;

    if (GDK_IS_X11_DISPLAY (display))
      {
        gdk_x11_display_error_trap_push (display);
        result = selection_owner (GDK_DISPLAY_XDISPLAY (display), is_proxy);
        gdk_x11_display_error_trap_pop_ignored (display);
        return result;
      }

    /* Wayland: las apps antiguas viven en XWayland. Sin el proxy pedido,
     * no conectarse (arrancaría XWayland en vano): en Wayland no suele
     * haber otra bandeja clásica. */
    if (wanters == NULL || g_hash_table_size (wanters) == 0 || !xwayland_available ())
      return 0;

    {
      Display *xdisplay = XOpenDisplay (NULL);
      int (*old_handler) (Display *, XErrorEvent *);

      if (xdisplay == NULL)
        return -1;

      old_handler = XSetErrorHandler (ignore_x_error);
      result = selection_owner (xdisplay, is_proxy);
      XSync (xdisplay, False);
      XSetErrorHandler (old_handler);
      XCloseDisplay (xdisplay);
      return result;
    }
  }
#endif

  return -1;
}



TrayLegacyState
tray_legacy_get_state (GtkWidget *reference)
{
  gboolean is_proxy = FALSE;
  gint owner;

  if (!tray_legacy_available ())
    return TRAY_LEGACY_UNAVAILABLE;
  if (wanters == NULL || g_hash_table_size (wanters) == 0)
    return TRAY_LEGACY_OFF;
  if (failed)
    return TRAY_LEGACY_FAILED;
  if (!session_is_x11 () && !xwayland_available ())
    return TRAY_LEGACY_NO_XWAYLAND;

  owner = tray_legacy_tray_owner (reference, &is_proxy);
  if (owner == 1 && !is_proxy)
    return TRAY_LEGACY_WAITING;

  /* Es el proxy, o todavía está arrancando */
  return TRAY_LEGACY_ACTIVE;
}
