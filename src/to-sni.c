/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Implementación del protocolo StatusNotifierItem:
 * https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/
 *
 * Todas las llamadas D-Bus son asíncronas: el host y el watcher pueden
 * vivir en el mismo proceso, y una llamada síncrona a uno mismo bloquearía
 * el bucle principal del panel.
 */
#include "to-common.h"
#include "to-sni.h"

#include <libdbusmenu-gtk/menu.h>
#include <string.h>
#include <unistd.h>

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE    "org.kde.StatusNotifierItem"
#define DBUS_NAME     "org.freedesktop.DBus"
#define DBUS_PATH     "/org/freedesktop/DBus"
#define PROPS_IFACE   "org.freedesktop.DBus.Properties"

/* Proxy XEmbed propio (proxy/xembed-proxy.c) */
#define PROXY_IFACE    "org.xfce.TrayOverflow.XEmbedItem"
#define PROXY_EXE_NAME "xembed-proxy"

static const gchar watcher_xml[] =
  "<node>"
  "  <interface name='" WATCHER_IFACE "'>"
  "    <method name='RegisterStatusNotifierItem'>"
  "      <arg type='s' direction='in' name='service'/>"
  "    </method>"
  "    <method name='RegisterStatusNotifierHost'>"
  "      <arg type='s' direction='in' name='service'/>"
  "    </method>"
  "    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
  "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
  "    <property name='ProtocolVersion' type='i' access='read'/>"
  "    <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
  "    <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
  "    <signal name='StatusNotifierHostRegistered'/>"
  "    <signal name='StatusNotifierHostUnregistered'/>"
  "  </interface>"
  "</node>";



/* ════════════════════════════════════════════════════════════════════════
 * Estado del backend (singleton)
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct
{
  TrayModel       *model;
  GDBusConnection *conn;
  GCancellable    *cancellable;

  /* Watcher de respaldo */
  GDBusNodeInfo   *watcher_info;
  guint            watcher_reg_id;
  guint            watcher_own_id;
  gboolean         watcher_owned;
  guint            watcher_retry_id;
  GHashTable      *watched_items;   /* clave → WatchEntry */
  GHashTable      *watched_hosts;   /* nombre → WatchEntry */

  /* Host */
  gchar           *host_name;
  guint            host_own_id;
  guint            host_watch_id;
  gchar           *watcher_owner;
  guint            sub_registered;
  guint            sub_unregistered;
  GHashTable      *items;           /* clave de registro → SniItem* */
} SniBackend;

typedef struct
{
  gchar *name;
  guint  sub_id;
} WatchEntry;

static SniBackend *backend = NULL;

static void host_add_item        (const gchar *reg_key);
static void host_remove_item     (const gchar *reg_key);
static void watcher_try_start    (void);



/* ════════════════════════════════════════════════════════════════════════
 * SniItem: subclase de TrayItem para ítems SNI
 * ════════════════════════════════════════════════════════════════════════ */

#define SNI_TYPE_ITEM (sni_item_get_type ())
G_DECLARE_FINAL_TYPE (SniItem, sni_item, SNI, ITEM, TrayItem)

struct _SniItem
{
  TrayItem      parent_instance;

  gchar        *reg_key;        /* "bus/ruta" tal como lo anuncia el watcher */
  gchar        *bus_name;
  gchar        *object_path;
  gchar        *owner;          /* nombre único (":1.42") */

  GCancellable *cancellable;
  guint         sub_signals;
  guint         sub_owner;
  guint         refresh_id;

  gboolean      have_props;
  gboolean      have_pid;
  gboolean      ready;
  gboolean      removed;

  gchar        *item_id;
  gchar        *exe;
  gchar        *menu_path;
  GtkWidget    *menu;

  /* Ítem publicado por nuestro proxy XEmbed: acepta SetIconSize */
  gboolean      from_proxy;
};

G_DEFINE_TYPE (SniItem, sni_item, TRAY_TYPE_ITEM)



static gboolean
menu_path_is_valid (const gchar *path)
{
  return path != NULL
         && g_variant_is_object_path (path)
         && g_strcmp0 (path, "/") != 0
         && g_strcmp0 (path, "/NO_DBUSMENU") != 0;
}



static void
sni_item_drop_menu (SniItem *item)
{
  if (item->menu != NULL)
    {
      gtk_widget_destroy (item->menu);
      g_object_unref (item->menu);
      item->menu = NULL;
    }
}



static GtkWidget *
sni_item_get_menu (TrayItem *tray_item)
{
  SniItem *item = SNI_ITEM (tray_item);

  if (item->menu == NULL && item->owner != NULL && menu_path_is_valid (item->menu_path))
    {
      DbusmenuGtkMenu *menu;

      menu = dbusmenu_gtkmenu_new (item->owner, item->menu_path);
      if (menu != NULL)
        {
          GtkAccelGroup *accel = gtk_accel_group_new ();

          dbusmenu_gtkclient_set_accel_group (dbusmenu_gtkmenu_get_client (menu), accel);
          g_object_unref (accel);
          item->menu = g_object_ref_sink (GTK_WIDGET (menu));
        }
    }

  return item->menu;
}



static void
sni_item_call (SniItem             *item,
               const gchar         *method,
               GVariant            *params,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
  if (item->owner == NULL || item->removed || backend == NULL)
    {
      if (params != NULL)
        g_variant_unref (g_variant_ref_sink (params));
      return;
    }

  g_dbus_connection_call (backend->conn, item->owner, item->object_path,
                          ITEM_IFACE, method, params, NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable,
                          callback, user_data);
}



static void
sni_item_activate_done (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  SniItem *item = user_data;
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (reply != NULL)
    g_variant_unref (reply);

  if (error != NULL)
    {
      /* La app no implementa Activate (típico de AppIndicator/Ayatana):
       * a partir de ahora el clic izquierdo abre su menú. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
          && !item->removed
          && menu_path_is_valid (item->menu_path))
        {
          tray_item_set_is_menu (TRAY_ITEM (item), TRUE);
          tray_item_emit_activate_fallback (TRAY_ITEM (item));
        }
      g_error_free (error);
    }

  g_object_unref (item);
}



static void
sni_item_activate (TrayItem *tray_item, gint x, gint y)
{
  SniItem *item = SNI_ITEM (tray_item);

  sni_item_call (item, "Activate", g_variant_new ("(ii)", x, y),
                 sni_item_activate_done, g_object_ref (item));
}



static void
sni_item_secondary_activate (TrayItem *tray_item, gint x, gint y)
{
  sni_item_call (SNI_ITEM (tray_item), "SecondaryActivate",
                 g_variant_new ("(ii)", x, y), NULL, NULL);
}



static void
sni_item_context_menu (TrayItem *tray_item, gint x, gint y)
{
  sni_item_call (SNI_ITEM (tray_item), "ContextMenu",
                 g_variant_new ("(ii)", x, y), NULL, NULL);
}



static void
sni_item_scroll (TrayItem *tray_item, gint delta, gboolean horizontal, gint x, gint y)
{
  SniItem *item = SNI_ITEM (tray_item);

  /* Scroll no trae posición: a nuestro proxy se la mandamos antes (llega
   * primero, mismo origen y destino), para que en Wayland la rueda caiga
   * sobre el ícono aunque no haya habido un clic antes */
  if (item->from_proxy && item->owner != NULL && !item->removed && backend != NULL)
    g_dbus_connection_call (backend->conn, item->owner, item->object_path,
                            PROXY_IFACE, "SetPointer", g_variant_new ("(ii)", x, y),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable,
                            NULL, NULL);

  sni_item_call (item, "Scroll",
                 g_variant_new ("(is)", delta, horizontal ? "horizontal" : "vertical"),
                 NULL, NULL);
}



/* Solo para ítems de nuestro proxy XEmbed: que la app antigua se dibuje
 * al tamaño exacto en que se va a mostrar */
static void
sni_item_set_icon_size (TrayItem *tray_item, gint pixels)
{
  SniItem *item = SNI_ITEM (tray_item);

  if (!item->from_proxy || item->owner == NULL || item->removed || backend == NULL)
    return;

  g_dbus_connection_call (backend->conn, item->owner, item->object_path,
                          PROXY_IFACE, "SetIconSize", g_variant_new ("(i)", pixels),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, item->cancellable,
                          NULL, NULL);
}



/* Deja el ítem inerte: sin señales, sin llamadas pendientes */
static void
sni_item_shutdown (SniItem *item)
{
  item->removed = TRUE;
  g_cancellable_cancel (item->cancellable);

  if (item->refresh_id != 0)
    {
      g_source_remove (item->refresh_id);
      item->refresh_id = 0;
    }

  if (backend != NULL && backend->conn != NULL)
    {
      if (item->sub_signals != 0)
        g_dbus_connection_signal_unsubscribe (backend->conn, item->sub_signals);
      if (item->sub_owner != 0)
        g_dbus_connection_signal_unsubscribe (backend->conn, item->sub_owner);
    }
  item->sub_signals = 0;
  item->sub_owner = 0;
}



static void
sni_item_finalize (GObject *object)
{
  SniItem *item = SNI_ITEM (object);

  sni_item_drop_menu (item);
  g_clear_object (&item->cancellable);
  g_free (item->reg_key);
  g_free (item->bus_name);
  g_free (item->object_path);
  g_free (item->owner);
  g_free (item->item_id);
  g_free (item->exe);
  g_free (item->menu_path);

  G_OBJECT_CLASS (sni_item_parent_class)->finalize (object);
}



static void
sni_item_class_init (SniItemClass *klass)
{
  TrayItemClass *item_class = TRAY_ITEM_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = sni_item_finalize;

  item_class->get_menu = sni_item_get_menu;
  item_class->activate = sni_item_activate;
  item_class->secondary_activate = sni_item_secondary_activate;
  item_class->context_menu = sni_item_context_menu;
  item_class->scroll = sni_item_scroll;
  item_class->set_icon_size = sni_item_set_icon_size;
}



static void
sni_item_init (SniItem *item)
{
  item->cancellable = g_cancellable_new ();
}



/* ── Identidad estable ───────────────────────────────────────────────── */

/* IDs que no sirven para reconocer a una app entre reinicios */
static gboolean
item_id_is_generic (const gchar *id)
{
  const gchar *p;

  if (id == NULL || *id == '\0')
    return TRUE;

  /* Electron/Chromium: "chrome_status_icon_1" */
  if (g_str_has_prefix (id, "chrome_status_icon_"))
    return TRUE;

  for (p = id; *p != '\0'; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;

  return TRUE;
}



static gboolean
is_interpreter (const gchar *name)
{
  static const gchar *const interpreters[] = {
    "python", "python2", "python3", "perl", "ruby", "node", "nodejs",
    "java", "sh", "bash", "dash", "gjs", "mono", NULL
  };

  if (g_str_has_prefix (name, "python"))
    return TRUE;

  return g_strv_contains (interpreters, name);
}



/* Nombre del ejecutable a partir del PID, saltando intérpretes
 * ("python3 /usr/bin/blueman-applet" → "blueman-applet") */
static gchar *
exe_name_from_pid (guint32 pid)
{
  gchar *path, *contents = NULL;
  gsize length = 0;
  gchar *result = NULL;

  path = g_strdup_printf ("/proc/%u/cmdline", pid);
  if (g_file_get_contents (path, &contents, &length, NULL) && length > 0)
    {
      const gchar *arg0 = contents;
      const gchar *end = contents + length;
      gchar *first;

      /* Algunos procesos (Chromium/Electron) reescriben su cmdline en una
       * sola cadena con espacios: cortamos en el primer espacio */
      first = g_strndup (arg0, strcspn (arg0, " "));
      result = g_path_get_basename (first);
      g_free (first);

      if (is_interpreter (result))
        {
          const gchar *arg = arg0 + strlen (arg0) + 1;

          while (arg < end && *arg == '-')
            arg += strlen (arg) + 1;

          if (arg < end && *arg != '\0')
            {
              g_free (result);
              result = g_path_get_basename (arg);
            }
        }
    }
  g_free (contents);
  g_free (path);

  if (result == NULL || *result == '\0')
    {
      g_free (result);
      result = NULL;

      path = g_strdup_printf ("/proc/%u/comm", pid);
      if (g_file_get_contents (path, &contents, NULL, NULL))
        result = g_strstrip (contents);
      g_free (path);
    }

  /* Apps Flatpak: el PID es el del proxy de D-Bus, no sirve */
  if (g_strcmp0 (result, "xdg-dbus-proxy") == 0)
    {
      g_free (result);
      result = NULL;
    }

  return result;
}



static gchar *
sni_item_compute_key (SniItem *item)
{
  const gchar *title;

  if (!item_id_is_generic (item->item_id))
    return g_strdup (item->item_id);

  if (item->exe != NULL && *item->exe != '\0')
    return g_strdup (item->exe);

  title = tray_item_get_title (TRAY_ITEM (item));
  if (title != NULL && *title != '\0')
    return g_strdup (title);

  return g_strdup (item->reg_key);
}



static void
sni_item_check_ready (SniItem *item)
{
  if (item->removed)
    return;

  if (item->ready)
    {
      tray_item_emit_changed (TRAY_ITEM (item));
      return;
    }

  if (item->have_props && item->have_pid)
    {
      gchar *key = sni_item_compute_key (item);

      tray_item_set_key (TRAY_ITEM (item), key);
      g_free (key);
      item->ready = TRUE;

      /* Precargar el menú: así el primer clic derecho ya lo tiene */
      sni_item_get_menu (TRAY_ITEM (item));

      tray_model_add_item (backend->model, TRAY_ITEM (item));
    }
}



/* ── Lectura de propiedades ──────────────────────────────────────────── */

static void
free_pixel_data (guchar *pixels, gpointer user_data)
{
  g_free (pixels);
}



/* a(iiay): ARGB32 en orden de red → GdkPixbuf RGBA */
static GPtrArray *
parse_pixmaps (GVariant *value)
{
  GPtrArray *array;
  GVariantIter iter;
  GVariant *data;
  gint width, height;

  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE ("a(iiay)")))
    return NULL;

  array = g_ptr_array_new_with_free_func (g_object_unref);

  g_variant_iter_init (&iter, value);
  while (g_variant_iter_next (&iter, "(ii@ay)", &width, &height, &data))
    {
      gsize length = 0;
      const guchar *src = g_variant_get_fixed_array (data, &length, 1);

      if (width > 0 && height > 0 && width <= 1024 && height <= 1024
          && length >= (gsize) width * height * 4)
        {
          gsize i, n = (gsize) width * height;
          guchar *dst = g_malloc (n * 4);

          for (i = 0; i < n; i++)
            {
              dst[i * 4 + 0] = src[i * 4 + 1]; /* R */
              dst[i * 4 + 1] = src[i * 4 + 2]; /* G */
              dst[i * 4 + 2] = src[i * 4 + 3]; /* B */
              dst[i * 4 + 3] = src[i * 4 + 0]; /* A */
            }

          g_ptr_array_add (array,
                           gdk_pixbuf_new_from_data (dst, GDK_COLORSPACE_RGB, TRUE, 8,
                                                     width, height, width * 4,
                                                     free_pixel_data, NULL));
        }

      g_variant_unref (data);
    }

  if (array->len == 0)
    {
      g_ptr_array_unref (array);
      return NULL;
    }

  return array;
}



static const gchar *
lookup_string (GVariant *dict, const gchar *key)
{
  const gchar *value = NULL;

  if (!g_variant_lookup (dict, key, "&s", &value)
      && !g_variant_lookup (dict, key, "&o", &value))
    return NULL;

  return value;
}



static void
sni_item_apply_properties (SniItem  *item,
                           GVariant *dict)
{
  TrayItem *tray_item = TRAY_ITEM (item);
  const gchar *status;
  const gchar *menu_path;
  GVariant *value;
  gboolean is_menu = FALSE;

  g_free (item->item_id);
  item->item_id = g_strdup (lookup_string (dict, "Id"));

  tray_item_set_title (tray_item, lookup_string (dict, "Title"));

  status = lookup_string (dict, "Status");
  if (g_strcmp0 (status, "Passive") == 0)
    tray_item_set_status (tray_item, TRAY_ITEM_STATUS_PASSIVE);
  else if (g_strcmp0 (status, "NeedsAttention") == 0)
    tray_item_set_status (tray_item, TRAY_ITEM_STATUS_ATTENTION);
  else
    tray_item_set_status (tray_item, TRAY_ITEM_STATUS_ACTIVE);

  tray_item_set_icon_theme_path (tray_item, lookup_string (dict, "IconThemePath"));

  value = g_variant_lookup_value (dict, "IconPixmap", NULL);
  tray_item_set_icon (tray_item, TRAY_ITEM_ICON_MAIN,
                      lookup_string (dict, "IconName"), parse_pixmaps (value));
  g_clear_pointer (&value, g_variant_unref);

  value = g_variant_lookup_value (dict, "AttentionIconPixmap", NULL);
  tray_item_set_icon (tray_item, TRAY_ITEM_ICON_ATTENTION,
                      lookup_string (dict, "AttentionIconName"), parse_pixmaps (value));
  g_clear_pointer (&value, g_variant_unref);

  value = g_variant_lookup_value (dict, "OverlayIconPixmap", NULL);
  tray_item_set_icon (tray_item, TRAY_ITEM_ICON_OVERLAY,
                      lookup_string (dict, "OverlayIconName"), parse_pixmaps (value));
  g_clear_pointer (&value, g_variant_unref);

  /* ToolTip: (sa(iiay)ss) → ícono, pixmaps, título, texto */
  value = g_variant_lookup_value (dict, "ToolTip", G_VARIANT_TYPE ("(sa(iiay)ss)"));
  if (value != NULL)
    {
      const gchar *tip_title = NULL, *tip_text = NULL;

      g_variant_get (value, "(&s@a(iiay)&s&s)", NULL, NULL, &tip_title, &tip_text);
      tray_item_set_tooltip (tray_item, tip_title, tip_text);
      g_variant_unref (value);
    }
  else
    {
      tray_item_set_tooltip (tray_item, NULL, NULL);
    }

  if (g_variant_lookup (dict, "ItemIsMenu", "b", &is_menu) || !tray_item_get_is_menu (tray_item))
    tray_item_set_is_menu (tray_item, is_menu);

  menu_path = lookup_string (dict, "Menu");
  if (g_strcmp0 (menu_path, item->menu_path) != 0)
    {
      g_free (item->menu_path);
      item->menu_path = g_strdup (menu_path);
      sni_item_drop_menu (item);
      if (item->ready)
        sni_item_get_menu (tray_item);
    }
}



static void
sni_item_get_all_done (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  SniItem *item = user_data;
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (reply != NULL)
    {
      GVariant *dict = g_variant_get_child_value (reply, 0);

      if (!item->removed)
        {
          sni_item_apply_properties (item, dict);
          item->have_props = TRUE;
          sni_item_check_ready (item);
        }

      g_variant_unref (dict);
      g_variant_unref (reply);
    }
  else
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
          && !item->removed && !item->ready)
        {
          g_debug ("Ítem SNI %s inválido: %s", item->reg_key, error->message);
          host_remove_item (item->reg_key);
        }
      g_error_free (error);
    }

  g_object_unref (item);
}



static gboolean
sni_item_refresh (gpointer user_data)
{
  SniItem *item = user_data;

  item->refresh_id = 0;

  if (item->removed || item->owner == NULL || backend == NULL)
    return G_SOURCE_REMOVE;

  g_dbus_connection_call (backend->conn, item->owner, item->object_path,
                          PROPS_IFACE, "GetAll", g_variant_new ("(s)", ITEM_IFACE),
                          G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
                          item->cancellable, sni_item_get_all_done, g_object_ref (item));

  return G_SOURCE_REMOVE;
}



/* Las señales New* llegan en ráfagas: agrupamos en una sola lectura */
static void
sni_item_schedule_refresh (SniItem *item)
{
  if (item->refresh_id == 0 && !item->removed)
    item->refresh_id = g_timeout_add (40, sni_item_refresh, item);
}



static void
sni_item_signal_cb (GDBusConnection *conn,
                    const gchar     *sender,
                    const gchar     *path,
                    const gchar     *iface,
                    const gchar     *signal,
                    GVariant        *params,
                    gpointer         user_data)
{
  sni_item_schedule_refresh (SNI_ITEM (user_data));
}



static gboolean
sni_item_remove_idle (gpointer user_data)
{
  gchar *reg_key = user_data;

  host_remove_item (reg_key);
  return G_SOURCE_REMOVE;
}



static void
sni_item_owner_changed_cb (GDBusConnection *conn,
                           const gchar     *sender,
                           const gchar     *path,
                           const gchar     *iface,
                           const gchar     *signal,
                           GVariant        *params,
                           gpointer         user_data)
{
  SniItem *item = SNI_ITEM (user_data);
  const gchar *new_owner = NULL;

  g_variant_get (params, "(&s&s&s)", NULL, NULL, &new_owner);
  if ((new_owner == NULL || *new_owner == '\0') && !item->removed)
    g_idle_add_full (G_PRIORITY_DEFAULT, sni_item_remove_idle,
                     g_strdup (item->reg_key), g_free);
}



static void
sni_item_pid_done (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  SniItem *item = user_data;
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (error != NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      g_object_unref (item);
      return;
    }

  if (reply != NULL)
    {
      guint32 pid = 0;

      g_variant_get (reply, "(u)", &pid);
      if (pid > 0)
        item->exe = exe_name_from_pid (pid);
      item->from_proxy = g_strcmp0 (item->exe, PROXY_EXE_NAME) == 0;
      tray_item_set_is_legacy (TRAY_ITEM (item), item->from_proxy);
      g_variant_unref (reply);
    }
  g_clear_error (&error);

  if (!item->removed)
    {
      item->have_pid = TRUE;
      sni_item_check_ready (item);
    }

  g_object_unref (item);
}



static void
sni_item_got_owner (SniItem *item)
{
  item->sub_signals =
    g_dbus_connection_signal_subscribe (backend->conn, item->owner, ITEM_IFACE,
                                        NULL, item->object_path, NULL,
                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                        sni_item_signal_cb, item, NULL);

  item->sub_owner =
    g_dbus_connection_signal_subscribe (backend->conn, DBUS_NAME, DBUS_NAME,
                                        "NameOwnerChanged", DBUS_PATH, item->owner,
                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                        sni_item_owner_changed_cb, item, NULL);

  g_dbus_connection_call (backend->conn, DBUS_NAME, DBUS_PATH, DBUS_NAME,
                          "GetConnectionUnixProcessID",
                          g_variant_new ("(s)", item->owner),
                          G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
                          item->cancellable, sni_item_pid_done, g_object_ref (item));

  sni_item_refresh (item);
}



static void
sni_item_name_owner_done (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  SniItem *item = user_data;
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (reply != NULL)
    {
      if (!item->removed)
        {
          g_variant_get (reply, "(s)", &item->owner);
          sni_item_got_owner (item);
        }
      g_variant_unref (reply);
    }
  else
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !item->removed)
        host_remove_item (item->reg_key);
      g_error_free (error);
    }

  g_object_unref (item);
}



static SniItem *
sni_item_new (const gchar *reg_key,
              const gchar *bus_name,
              const gchar *object_path)
{
  SniItem *item = g_object_new (SNI_TYPE_ITEM, NULL);

  item->reg_key = g_strdup (reg_key);
  item->bus_name = g_strdup (bus_name);
  item->object_path = g_strdup (object_path);

  return item;
}



static void
sni_item_start (SniItem *item)
{
  if (item->bus_name[0] == ':')
    {
      item->owner = g_strdup (item->bus_name);
      sni_item_got_owner (item);
    }
  else
    {
      g_dbus_connection_call (backend->conn, DBUS_NAME, DBUS_PATH, DBUS_NAME,
                              "GetNameOwner", g_variant_new ("(s)", item->bus_name),
                              G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, -1,
                              item->cancellable, sni_item_name_owner_done,
                              g_object_ref (item));
    }
}



/* ════════════════════════════════════════════════════════════════════════
 * Watcher de respaldo
 * ════════════════════════════════════════════════════════════════════════ */

static void
watch_entry_free (gpointer data)
{
  WatchEntry *entry = data;

  if (backend != NULL && backend->conn != NULL && entry->sub_id != 0)
    g_dbus_connection_signal_unsubscribe (backend->conn, entry->sub_id);
  g_free (entry->name);
  g_free (entry);
}



static void
watcher_emit (const gchar *signal,
              GVariant    *params)
{
  g_dbus_connection_emit_signal (backend->conn, NULL, WATCHER_PATH, WATCHER_IFACE,
                                 signal, params, NULL);
}



static GVariant *
watcher_items_variant (void)
{
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
  g_hash_table_iter_init (&iter, backend->watched_items);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_variant_builder_add (&builder, "s", (const gchar *) key);

  return g_variant_builder_end (&builder);
}



static void
watcher_emit_items_changed (void)
{
  GVariantBuilder changed;

  g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&changed, "{sv}", "RegisteredStatusNotifierItems",
                         watcher_items_variant ());

  g_dbus_connection_emit_signal (backend->conn, NULL, WATCHER_PATH, PROPS_IFACE,
                                 "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", WATCHER_IFACE, &changed, NULL),
                                 NULL);
}



static void
watcher_item_owner_changed (GDBusConnection *conn,
                            const gchar     *sender,
                            const gchar     *path,
                            const gchar     *iface,
                            const gchar     *signal,
                            GVariant        *params,
                            gpointer         user_data)
{
  const gchar *new_owner = NULL;
  gchar *key;

  g_variant_get (params, "(&s&s&s)", NULL, NULL, &new_owner);
  if (new_owner != NULL && *new_owner != '\0')
    return;

  /* Copia: la entrada (y user_data) se liberan al quitarla */
  key = g_strdup (user_data);
  if (g_hash_table_remove (backend->watched_items, key))
    {
      watcher_emit ("StatusNotifierItemUnregistered", g_variant_new ("(s)", key));
      watcher_emit_items_changed ();
    }
  g_free (key);
}



static void
watcher_host_owner_changed (GDBusConnection *conn,
                            const gchar     *sender,
                            const gchar     *path,
                            const gchar     *iface,
                            const gchar     *signal,
                            GVariant        *params,
                            gpointer         user_data)
{
  const gchar *new_owner = NULL;
  gchar *name;

  g_variant_get (params, "(&s&s&s)", NULL, NULL, &new_owner);
  if (new_owner != NULL && *new_owner != '\0')
    return;

  name = g_strdup (user_data);
  if (g_hash_table_remove (backend->watched_hosts, name))
    watcher_emit ("StatusNotifierHostUnregistered", NULL);
  g_free (name);
}



static guint
subscribe_name_vanish (const gchar         *name,
                       GDBusSignalCallback  callback,
                       const gchar         *user_key)
{
  return g_dbus_connection_signal_subscribe (backend->conn, DBUS_NAME, DBUS_NAME,
                                             "NameOwnerChanged", DBUS_PATH, name,
                                             G_DBUS_SIGNAL_FLAGS_NONE, callback,
                                             g_strdup (user_key), g_free);
}



static void
watcher_method_call (GDBusConnection       *conn,
                     const gchar           *sender,
                     const gchar           *object_path,
                     const gchar           *interface_name,
                     const gchar           *method_name,
                     GVariant              *params,
                     GDBusMethodInvocation *invocation,
                     gpointer               user_data)
{
  const gchar *service = NULL;
  const gchar *bus_name, *path;
  WatchEntry *entry;
  gchar *key;

  g_variant_get (params, "(&s)", &service);

  if (g_strcmp0 (method_name, "RegisterStatusNotifierItem") == 0)
    {
      if (service[0] == '/')
        {
          /* Ayatana: registra solo la ruta del objeto */
          bus_name = sender;
          path = service;
        }
      else
        {
          bus_name = service;
          path = "/StatusNotifierItem";
        }

      if (!g_dbus_is_name (bus_name) || !g_variant_is_object_path (path))
        {
          g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR,
                                                         G_IO_ERROR_INVALID_ARGUMENT,
                                                         "Invalid service");
          return;
        }

      key = g_strconcat (bus_name, path, NULL);

      entry = g_new0 (WatchEntry, 1);
      entry->name = g_strdup (bus_name);
      entry->sub_id = subscribe_name_vanish (bus_name, watcher_item_owner_changed, key);
      g_hash_table_replace (backend->watched_items, g_strdup (key), entry);

      g_dbus_method_invocation_return_value (invocation, NULL);

      watcher_emit ("StatusNotifierItemRegistered", g_variant_new ("(s)", key));
      watcher_emit_items_changed ();
      g_free (key);
    }
  else if (g_strcmp0 (method_name, "RegisterStatusNotifierHost") == 0)
    {
      if (!g_hash_table_contains (backend->watched_hosts, sender))
        {
          entry = g_new0 (WatchEntry, 1);
          entry->name = g_strdup (sender);
          entry->sub_id = subscribe_name_vanish (sender, watcher_host_owner_changed, sender);
          g_hash_table_insert (backend->watched_hosts, g_strdup (sender), entry);
        }

      g_dbus_method_invocation_return_value (invocation, NULL);
      watcher_emit ("StatusNotifierHostRegistered", NULL);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method %s", method_name);
    }
}



static GVariant *
watcher_get_property (GDBusConnection *conn,
                      const gchar     *sender,
                      const gchar     *object_path,
                      const gchar     *interface_name,
                      const gchar     *property_name,
                      GError         **error,
                      gpointer         user_data)
{
  if (g_strcmp0 (property_name, "RegisteredStatusNotifierItems") == 0)
    return watcher_items_variant ();

  if (g_strcmp0 (property_name, "IsStatusNotifierHostRegistered") == 0)
    return g_variant_new_boolean (TRUE);

  if (g_strcmp0 (property_name, "ProtocolVersion") == 0)
    return g_variant_new_int32 (0);

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property %s", property_name);
  return NULL;
}



static const GDBusInterfaceVTable watcher_vtable = {
  watcher_method_call,
  watcher_get_property,
  NULL,
  { NULL }
};



static void
watcher_name_acquired (GDBusConnection *conn,
                       const gchar     *name,
                       gpointer         user_data)
{
  backend->watcher_owned = TRUE;
  g_debug ("Proveyendo %s", WATCHER_NAME);
}



static void
watcher_name_lost (GDBusConnection *conn,
                   const gchar     *name,
                   gpointer         user_data)
{
  /* Otro proceso tiene el watcher: quedamos en cola por si lo suelta */
  backend->watcher_owned = FALSE;
  g_hash_table_remove_all (backend->watched_items);
  g_hash_table_remove_all (backend->watched_hosts);
}



static void
watcher_try_start (void)
{
  GError *error = NULL;

  if (backend == NULL || backend->watcher_reg_id != 0)
    return;

  /* Si la bandeja nativa corre en este mismo proceso, ya exportó su
   * watcher en esta conexión y el registro falla: en ese caso no pedimos
   * el nombre y lo volvemos a intentar cuando el watcher desaparezca. */
  backend->watcher_reg_id =
    g_dbus_connection_register_object (backend->conn, WATCHER_PATH,
                                       backend->watcher_info->interfaces[0],
                                       &watcher_vtable, NULL, NULL, &error);
  if (backend->watcher_reg_id == 0)
    {
      g_debug ("No se pudo exportar el watcher: %s", error->message);
      g_error_free (error);
      return;
    }

  backend->watcher_own_id =
    g_bus_own_name_on_connection (backend->conn, WATCHER_NAME,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  watcher_name_acquired, watcher_name_lost,
                                  NULL, NULL);
}



static gboolean
watcher_retry_cb (gpointer user_data)
{
  backend->watcher_retry_id = 0;
  watcher_try_start ();
  return G_SOURCE_REMOVE;
}



gboolean
to_sni_backend_owns_watcher (void)
{
  return backend != NULL && backend->watcher_owned;
}



/* ════════════════════════════════════════════════════════════════════════
 * Host
 * ════════════════════════════════════════════════════════════════════════ */

static void
item_hash_value_free (gpointer data)
{
  SniItem *item = data;

  sni_item_shutdown (item);
  g_object_unref (item);
}



static void
host_add_item (const gchar *reg_key)
{
  const gchar *slash;
  gchar *bus_name;
  SniItem *item;

  if (backend == NULL || reg_key == NULL || g_hash_table_contains (backend->items, reg_key))
    return;

  slash = strchr (reg_key, '/');
  if (slash == NULL)
    {
      bus_name = g_strdup (reg_key);
      slash = "/StatusNotifierItem";
    }
  else
    {
      bus_name = g_strndup (reg_key, slash - reg_key);
    }

  if (g_dbus_is_name (bus_name) && g_variant_is_object_path (slash))
    {
      item = sni_item_new (reg_key, bus_name, slash);
      g_hash_table_insert (backend->items, g_strdup (reg_key), item);
      sni_item_start (item);
    }

  g_free (bus_name);
}



static void
host_remove_item (const gchar *reg_key)
{
  SniItem *item;
  gchar *key;

  if (backend == NULL)
    return;

  item = g_hash_table_lookup (backend->items, reg_key);
  if (item == NULL)
    return;

  /* reg_key puede pertenecer al propio ítem: copiarlo antes de liberar */
  key = g_strdup (reg_key);
  g_object_ref (item);
  g_hash_table_remove (backend->items, key);
  tray_model_remove_item (backend->model, TRAY_ITEM (item));
  g_object_unref (item);
  g_free (key);
}



static void
host_remove_all_items (void)
{
  GList *keys, *l;

  keys = g_hash_table_get_keys (backend->items);
  for (l = keys; l != NULL; l = l->next)
    {
      gchar *key = g_strdup (l->data);
      host_remove_item (key);
      g_free (key);
    }
  g_list_free (keys);
}



static void
host_item_registered_cb (GDBusConnection *conn,
                         const gchar     *sender,
                         const gchar     *path,
                         const gchar     *iface,
                         const gchar     *signal,
                         GVariant        *params,
                         gpointer         user_data)
{
  const gchar *key = NULL;

  if (g_variant_is_of_type (params, G_VARIANT_TYPE ("(s)")))
    {
      g_variant_get (params, "(&s)", &key);
      host_add_item (key);
    }
}



static void
host_item_unregistered_cb (GDBusConnection *conn,
                           const gchar     *sender,
                           const gchar     *path,
                           const gchar     *iface,
                           const gchar     *signal,
                           GVariant        *params,
                           gpointer         user_data)
{
  const gchar *key = NULL;

  if (g_variant_is_of_type (params, G_VARIANT_TYPE ("(s)")))
    {
      g_variant_get (params, "(&s)", &key);
      host_remove_item (key);
    }
}



static void
host_items_done (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GError *error = NULL;
  GVariant *reply, *value;
  GVariantIter iter;
  const gchar *key;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (reply == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("No se pudo leer la lista de ítems del watcher: %s", error->message);
      g_error_free (error);
      return;
    }

  g_variant_get (reply, "(v)", &value);
  if (g_variant_is_of_type (value, G_VARIANT_TYPE ("as")))
    {
      g_variant_iter_init (&iter, value);
      while (g_variant_iter_next (&iter, "&s", &key))
        host_add_item (key);
    }

  g_variant_unref (value);
  g_variant_unref (reply);
}



static void
host_register_done (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GError *error = NULL;
  GVariant *reply;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (reply != NULL)
    g_variant_unref (reply);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("RegisterStatusNotifierHost falló: %s", error->message);
      g_error_free (error);
    }
}



static void
host_unsubscribe (void)
{
  if (backend->sub_registered != 0)
    g_dbus_connection_signal_unsubscribe (backend->conn, backend->sub_registered);
  if (backend->sub_unregistered != 0)
    g_dbus_connection_signal_unsubscribe (backend->conn, backend->sub_unregistered);
  backend->sub_registered = 0;
  backend->sub_unregistered = 0;
}



static void
host_watcher_appeared (GDBusConnection *conn,
                       const gchar     *name,
                       const gchar     *name_owner,
                       gpointer         user_data)
{
  host_unsubscribe ();
  g_free (backend->watcher_owner);
  backend->watcher_owner = g_strdup (name_owner);

  backend->sub_registered =
    g_dbus_connection_signal_subscribe (conn, name_owner, WATCHER_IFACE,
                                        "StatusNotifierItemRegistered", WATCHER_PATH,
                                        NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                        host_item_registered_cb, NULL, NULL);
  backend->sub_unregistered =
    g_dbus_connection_signal_subscribe (conn, name_owner, WATCHER_IFACE,
                                        "StatusNotifierItemUnregistered", WATCHER_PATH,
                                        NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                        host_item_unregistered_cb, NULL, NULL);

  g_dbus_connection_call (conn, name_owner, WATCHER_PATH, WATCHER_IFACE,
                          "RegisterStatusNotifierHost",
                          g_variant_new ("(s)", backend->host_name),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          backend->cancellable, host_register_done, NULL);

  g_dbus_connection_call (conn, name_owner, WATCHER_PATH, PROPS_IFACE, "Get",
                          g_variant_new ("(ss)", WATCHER_IFACE,
                                         "RegisteredStatusNotifierItems"),
                          G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
                          backend->cancellable, host_items_done, NULL);
}



static void
host_watcher_vanished (GDBusConnection *conn,
                       const gchar     *name,
                       gpointer         user_data)
{
  if (backend == NULL)
    return;

  host_unsubscribe ();
  g_clear_pointer (&backend->watcher_owner, g_free);

  /* Las apps vuelven a registrarse solas cuando aparece un watcher nuevo */
  host_remove_all_items ();

  /* Si el watcher era de otro (p. ej. quitaron la bandeja nativa),
   * intentamos tomar el relevo */
  if (backend->watcher_retry_id == 0)
    backend->watcher_retry_id = g_timeout_add (200, watcher_retry_cb, NULL);
}



/* ════════════════════════════════════════════════════════════════════════
 * Arranque y parada
 * ════════════════════════════════════════════════════════════════════════ */

void
to_sni_backend_start (TrayModel *model)
{
  static guint counter = 0;
  GError *error = NULL;
  GDBusConnection *conn;

  if (backend != NULL)
    return;

  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (conn == NULL)
    {
      g_warning ("Tray Overflow: sin bus de sesión D-Bus: %s", error->message);
      g_error_free (error);
      return;
    }

  backend = g_new0 (SniBackend, 1);
  backend->model = model;
  backend->conn = conn;
  backend->cancellable = g_cancellable_new ();
  backend->watcher_info = g_dbus_node_info_new_for_xml (watcher_xml, NULL);
  backend->watched_items = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, watch_entry_free);
  backend->watched_hosts = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, watch_entry_free);
  backend->items = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, item_hash_value_free);

  watcher_try_start ();

  backend->host_name = g_strdup_printf ("org.kde.StatusNotifierHost-%d-%u",
                                        (gint) getpid (), ++counter);
  backend->host_own_id = g_bus_own_name_on_connection (conn, backend->host_name,
                                                       G_BUS_NAME_OWNER_FLAGS_NONE,
                                                       NULL, NULL, NULL, NULL);

  backend->host_watch_id = g_bus_watch_name_on_connection (conn, WATCHER_NAME,
                                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                           host_watcher_appeared,
                                                           host_watcher_vanished,
                                                           NULL, NULL);
}



void
to_sni_backend_stop (void)
{
  SniBackend *b = backend;

  if (b == NULL)
    return;

  g_cancellable_cancel (b->cancellable);

  if (b->watcher_retry_id != 0)
    g_source_remove (b->watcher_retry_id);
  if (b->host_watch_id != 0)
    g_bus_unwatch_name (b->host_watch_id);
  host_unsubscribe ();

  host_remove_all_items ();
  g_hash_table_unref (b->items);

  if (b->host_own_id != 0)
    g_bus_unown_name (b->host_own_id);

  g_hash_table_unref (b->watched_items);
  g_hash_table_unref (b->watched_hosts);
  if (b->watcher_own_id != 0)
    g_bus_unown_name (b->watcher_own_id);
  if (b->watcher_reg_id != 0)
    g_dbus_connection_unregister_object (b->conn, b->watcher_reg_id);
  g_dbus_node_info_unref (b->watcher_info);

  backend = NULL;

  g_clear_object (&b->cancellable);
  g_free (b->host_name);
  g_free (b->watcher_owner);
  g_object_unref (b->conn);
  g_free (b);
}
