/*
 * Tray Overflow — proxy XEmbed → StatusNotifierItem
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hace de bandeja clásica de X11 (XEmbed) y vuelve a publicar cada ícono
 * como un ítem StatusNotifier por D-Bus, para que lo muestre cualquier
 * host SNI (Tray Overflow, KDE, etc.).
 *
 * Técnica (la misma idea que xembedsniproxy de KDE):
 *  - Cada ícono se reparenta en un contenedor propio que existe y está
 *    mapeado (si no, la app no dibuja), pero es invisible: forma vacía,
 *    opacidad 0, sin zona de entrada.
 *  - La ventana del ícono se redirige con Composite: se dibuja en un
 *    pixmap fuera de pantalla, de donde copiamos la imagen ante cada
 *    aviso de Damage.
 *  - Los clics se simulan: el contenedor se coloca bajo el puntero, se
 *    abre por un instante un píxel de entrada y se inyecta el clic, con
 *    XTest o con un evento directo según lo que acepte la app.
 *
 * Diferencias con xembedsniproxy:
 *  - Solo depende de Xlib y GLib.
 *  - Si otra bandeja ya está activa, espera y toma el relevo al quitarla.
 *  - Espera a que se suelte el botón físico antes de simular el clic.
 *  - Respeta XEMBED_MAPPED: una app que oculta su ícono deja de mostrarse.
 */
#include "config.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <stdlib.h>
#include <string.h>

#define PROXY_NAME                "trayoverflow-xembed-proxy"
/* Tamaño de cada ícono: el host puede pedir otro con SetIconSize (la app
 * se redibuja a ese tamaño, nítida); sin pedido se usa el por defecto */
#define DEFAULT_SIZE              32
#define MIN_SIZE                  8
#define MAX_SIZE                  256

/* Clics y pasos de rueda que pueden esperar a la vez (giros rápidos) */
#define MAX_PENDING_CLICKS        16
#define TO_IFACE                  "org.xfce.TrayOverflow.XEmbedItem"

/* Capturas: silencio a esperar tras el último dibujado de la app, tope
 * de espera (íconos animados) y espera tras anclar o redimensionar */
#define QUIET_MS                  50
#define SETTLE_QUIET_MS           120
#define MAX_WAIT_MS               250
#define SETTLE_FALLBACK_MS        700
/* --wayland: tras un clic, cuándo sacar el contenedor de la pantalla */
#define PARK_DELAY_MS             1500

#define SYSTEM_TRAY_REQUEST_DOCK  0
#define XEMBED_EMBEDDED_NOTIFY    0
#define XEMBED_VERSION            0
#define XEMBED_MAPPED             (1 << 0)

#define WATCHER_NAME              "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH              "/StatusNotifierWatcher"
#define ITEM_IFACE                "org.kde.StatusNotifierItem"
#define ITEM_PATH                 "/StatusNotifierItem"

static const gchar item_xml[] =
  "<node>"
  "  <interface name='" ITEM_IFACE "'>"
  "    <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='WindowId' type='i' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
  "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewToolTip'/>"
  "    <signal name='NewStatus'><arg type='s'/></signal>"
  "  </interface>"
  "  <interface name='" TO_IFACE "'>"
  "    <method name='SetIconSize'><arg type='i' name='pixels' direction='in'/></method>"
  "    <method name='SetPointer'><arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/></method>"
  "  </interface>"
  "</node>";



/* ════════════════════════════════════════════════════════════════════════
 * Estado
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct
{
  Window           win;          /* ventana del ícono (de la app) */
  Window           container;    /* nuestra ventana invisible */
  gint             size;         /* lado en píxeles al que se dibuja */
  Damage           damage;
  gint             ignore_unmaps;
  gboolean         xembed_mapped;
  gboolean         direct_events;

  /* Imagen actual, ARGB32 en orden de red, lista para IconPixmap */
  guchar          *pixels;
  gint             img_w;
  gint             img_h;
  guint            update_id;
  guint            late_update_id;
  gint64           first_damage;   /* primer aviso de la tanda en curso */
  gint64           settle_until;   /* hasta cuándo se espera un redibujado */

  gchar           *id;
  gchar           *title;

  /* Publicación SNI: una conexión D-Bus por ícono, así al cerrarla el
   * watcher sabe que el ícono desapareció */
  GDBusConnection *conn;
  guint            reg_id;
  guint            reg_size_id;

  /* Clic pendiente (se espera a que se suelte el botón físico) */
  guint            click_id;
  guint            park_id;          /* --wayland: sacar el contenedor de la vista */
  guint8           click_buttons[MAX_PENDING_CLICKS];   /* en orden de llegada */
  gint             n_clicks;
  gint             click_tries;
  gint             click_x;          /* --wayland: posición que manda el host */
  gint             click_y;
} Icon;

static struct
{
  Display        *dpy;
  gint            screen;
  Window          root;
  Window          manager;
  gboolean        owner;
  Time            claim_time;

  Visual         *visual;        /* visual ARGB para los contenedores */
  gint            depth;
  Colormap        colormap;

  gint            damage_event;
  gint            fixes_event;
  gboolean        have_xtest;

  Atom            a_selection;
  Atom            a_manager;
  Atom            a_opcode;
  Atom            a_tray_visual;
  Atom            a_tray_orientation;
  Atom            a_tray_icon_size;
  Atom            a_xembed;
  Atom            a_xembed_info;
  Atom            a_net_wm_name;
  Atom            a_net_wm_pid;
  Atom            a_utf8;
  Atom            a_opacity;

  GHashTable     *icons;         /* Window → Icon* */
  GDBusConnection *bus;          /* para vigilar al watcher */
  gchar          *bus_address;
  GDBusNodeInfo  *node;
  GMainLoop      *loop;
  gboolean        verbose;
  gboolean        wayland;       /* sesión Wayland: X es XWayland */
  gint            x_error;
} P;

static void icon_publish   (Icon *icon);
static void icon_unpublish (Icon *icon);
static void undock         (Window win);

#define LOG(...) G_STMT_START { if (P.verbose) g_printerr (PROXY_NAME ": " __VA_ARGS__); } G_STMT_END



/* ════════════════════════════════════════════════════════════════════════
 * Errores de X: las ventanas de las apps pueden desaparecer en cualquier
 * momento, así que los errores "ventana inexistente" son normales
 * ════════════════════════════════════════════════════════════════════════ */

static int
x_error_handler (Display *dpy, XErrorEvent *ev)
{
  P.x_error = ev->error_code;

  if (P.verbose)
    {
      gchar text[128];
      XGetErrorText (dpy, ev->error_code, text, sizeof (text));
      LOG ("error de X ignorado: %s (petición %d)\n", text, ev->request_code);
    }
  return 0;
}



static int
x_io_error_handler (Display *dpy)
{
  /* Se cerró la sesión gráfica: no hay nada que hacer */
  exit (0);
  return 0;
}



static void
trap_push (void)
{
  XSync (P.dpy, False);
  P.x_error = 0;
}



static gint
trap_pop (void)
{
  XSync (P.dpy, False);
  return P.x_error;
}



/* ════════════════════════════════════════════════════════════════════════
 * Propiedades de ventanas
 * ════════════════════════════════════════════════════════════════════════ */

static gchar *
get_utf8_property (Window win, Atom property)
{
  Atom type;
  gint format;
  gulong n_items, after;
  guchar *data = NULL;
  gchar *result = NULL;

  if (XGetWindowProperty (P.dpy, win, property, 0, 1024, False, P.a_utf8,
                          &type, &format, &n_items, &after, &data) == Success
      && data != NULL && type == P.a_utf8 && format == 8 && n_items > 0)
    {
      result = g_strndup ((const gchar *) data, n_items);
      if (!g_utf8_validate (result, -1, NULL))
        g_clear_pointer (&result, g_free);
    }

  if (data != NULL)
    XFree (data);
  return result;
}



static gchar *
get_window_title (Window win)
{
  gchar *title = get_utf8_property (win, P.a_net_wm_name);
  XTextProperty prop;

  if (title != NULL && *title != '\0')
    return title;
  g_clear_pointer (&title, g_free);   /* título vacío: probar WM_NAME */

  if (XGetWMName (P.dpy, win, &prop) && prop.value != NULL && prop.nitems > 0)
    {
      title = g_locale_to_utf8 ((const gchar *) prop.value, prop.nitems, NULL, NULL, NULL);
      XFree (prop.value);
    }

  return title;
}



static gulong
get_window_pid (Window win)
{
  Atom type;
  gint format;
  gulong n_items, after, pid = 0;
  guchar *data = NULL;

  if (XGetWindowProperty (P.dpy, win, P.a_net_wm_pid, 0, 1, False, XA_CARDINAL,
                          &type, &format, &n_items, &after, &data) == Success
      && data != NULL && format == 32 && n_items == 1)
    pid = *(gulong *) data;

  if (data != NULL)
    XFree (data);
  return pid;
}



static gchar *
exe_from_pid (gulong pid)
{
  gchar *path, *contents = NULL, *name = NULL;

  if (pid == 0)
    return NULL;

  path = g_strdup_printf ("/proc/%lu/comm", pid);
  if (g_file_get_contents (path, &contents, NULL, NULL))
    name = g_strdup (g_strstrip (contents));
  g_free (contents);
  g_free (path);

  return name;
}



/* Títulos que ponen los toolkits y no dicen nada de la app */
static gboolean
title_is_generic (const gchar *title)
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



/* Nombre de una app Java a partir de su línea de comandos:
 * "java -jar JDownloader.jar" → "JDownloader". NULL si no es Java. */
static gchar *
java_app_name (gulong pid)
{
  gchar *path, *contents = NULL, *name = NULL, *argv0;
  const gchar *arg, *end;
  gsize length = 0;
  gboolean is_java;

  if (pid == 0)
    return NULL;

  path = g_strdup_printf ("/proc/%lu/cmdline", pid);
  if (!g_file_get_contents (path, &contents, &length, NULL) || length == 0)
    {
      g_free (path);
      g_free (contents);
      return NULL;
    }
  g_free (path);

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



/* Identidad estable de la app (para que el plugin recuerde si la
 * ocultaste) y un título legible */
static void
icon_read_identity (Icon *icon)
{
  XClassHint hint = { NULL, NULL };
  gulong pid;
  gchar *exe = NULL, *java;
  gchar *title;

  if (XGetClassHint (P.dpy, icon->win, &hint))
    {
      if (hint.res_name != NULL && *hint.res_name != '\0')
        icon->id = g_ascii_strdown (hint.res_name, -1);
      if (hint.res_class != NULL && *hint.res_class != '\0')
        icon->title = g_strdup (hint.res_class);
      XFree (hint.res_name);
      XFree (hint.res_class);
    }

  pid = get_window_pid (icon->win);
  exe = exe_from_pid (pid);
  if (icon->id == NULL && exe != NULL)
    icon->id = g_strdup (exe);

  /* Java: el título del ícono es genérico y el WM_CLASS es el nombre de
   * una clase; el nombre legible sale de la línea de comandos */
  java = java_app_name (pid);
  if (java != NULL)
    {
      g_free (icon->title);
      icon->title = java;
    }

  title = get_window_title (icon->win);
  if (title != NULL && *title != '\0' && !title_is_generic (title))
    {
      g_free (icon->title);
      icon->title = title;
    }
  else
    {
      g_free (title);
    }

  if (icon->title == NULL)
    icon->title = g_strdup (exe != NULL ? exe : icon->id);

  /* Nunca un ID vacío ni solo numérico (el plugin los considera
   * genéricos y buscaría el ejecutable... que sería este proxy) */
  if (icon->id == NULL)
    icon->id = icon->title != NULL ? g_strdup (icon->title)
                                   : g_strdup_printf ("xembed-%lx", icon->win);
  if (icon->title == NULL)
    icon->title = g_strdup (icon->id);

  g_free (exe);
  LOG ("ícono 0x%lx: id=%s título=%s\n", icon->win, icon->id, icon->title);
}



static gboolean
read_xembed_mapped (Window win)
{
  Atom type;
  gint format;
  gulong n_items, after;
  guchar *data = NULL;
  gboolean mapped = TRUE;

  /* _XEMBED_INFO: [versión, flags]. Sin la propiedad, se asume visible */
  if (XGetWindowProperty (P.dpy, win, P.a_xembed_info, 0, 2, False, P.a_xembed_info,
                          &type, &format, &n_items, &after, &data) == Success
      && data != NULL && format == 32 && n_items >= 2)
    mapped = (((gulong *) data)[1] & XEMBED_MAPPED) != 0;

  if (data != NULL)
    XFree (data);
  return mapped;
}



/* ¿La app (o alguna subventana) escucha clics de X "clásicos"? Si sí, se
 * le pueden enviar eventos directos (GTK2, Wine...); si no (GTK3, que usa
 * XInput2), hay que inyectarlos con XTest */
static gboolean
wants_core_button_events (Window win, gint level)
{
  XWindowAttributes wa;
  Window root, parent, *children = NULL;
  guint n_children = 0, i;
  gboolean result = FALSE;

  if (level > 4 || !XGetWindowAttributes (P.dpy, win, &wa))
    return FALSE;
  if (wa.all_event_masks & ButtonPressMask)
    return TRUE;
  if (wa.do_not_propagate_mask & ButtonPressMask)
    return FALSE;

  if (XQueryTree (P.dpy, win, &root, &parent, &children, &n_children))
    {
      for (i = 0; i < n_children && !result; i++)
        result = wants_core_button_events (children[i], level + 1);
      if (children != NULL)
        XFree (children);
    }

  return result;
}



/* ════════════════════════════════════════════════════════════════════════
 * Captura de la imagen del ícono
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



static guint8
channel (gulong pixel, gulong mask)
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



/* Convierte la XImage en ARGB no premultiplicado, en orden de red.
 * Devuelve NULL si la imagen es completamente transparente (pasa por un
 * instante con algunas apps, p. ej. Wine). */
static guchar *
convert_image (XImage *image, Visual *visual, gint depth, gint w, gint h)
{
  gulong rmask = visual->red_mask, gmask = visual->green_mask, bmask = visual->blue_mask;
  gulong amask = 0;
  gboolean use_alpha = FALSE, any_opaque = FALSE;
  guchar *out;
  gulong key;
  gint x, y;

  if (depth == 32)
    {
      amask = 0xffffffffUL & ~(rmask | gmask | bmask);
      use_alpha = amask != 0;

      /* Algunas apps dibujan sin alfa en una ventana de 32 bits */
      if (use_alpha)
        {
          gboolean any = FALSE;

          for (y = 0; y < h && !any; y++)
            for (x = 0; x < w && !any; x++)
              any = (XGetPixel (image, x, y) & amask) != 0;
          use_alpha = any;
        }
    }

  out = g_malloc ((gsize) w * h * 4);

  /* Sin alfa: el color de la esquina se toma como fondo transparente
   * (la app pinta sobre el fondo de nuestro contenedor) */
  key = XGetPixel (image, 0, 0) & (rmask | gmask | bmask);

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        gulong pixel = XGetPixel (image, x, y);
        guchar *o = out + ((gsize) y * w + x) * 4;
        guint8 r = channel (pixel, rmask);
        guint8 g = channel (pixel, gmask);
        guint8 b = channel (pixel, bmask);
        guint8 a;

        if (use_alpha)
          {
            a = channel (pixel, amask);
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
            a = (pixel & (rmask | gmask | bmask)) == key ? 0 : 255;
          }

        if (a != 0)
          any_opaque = TRUE;

        o[0] = a;
        o[1] = r;
        o[2] = g;
        o[3] = b;
      }

  if (!any_opaque)
    {
      g_free (out);
      return NULL;
    }

  return out;
}



static void
icon_emit (Icon *icon, const gchar *signal)
{
  if (icon->conn != NULL && icon->reg_id != 0)
    g_dbus_connection_emit_signal (icon->conn, NULL, ITEM_PATH, ITEM_IFACE,
                                   signal, NULL, NULL);
}



static gboolean
icon_update (gpointer user_data)
{
  Icon *icon = user_data;
  XWindowAttributes wa;
  XImage *image;
  guchar *pixels;
  gint w, h;

  icon->update_id = 0;

  trap_push ();
  if (!XGetWindowAttributes (P.dpy, icon->win, &wa) || trap_pop () != 0)
    return G_SOURCE_REMOVE;

  w = MIN (wa.width, icon->size);
  h = MIN (wa.height, icon->size);
  if (w <= 0 || h <= 0)
    return G_SOURCE_REMOVE;

  trap_push ();
  image = XGetImage (P.dpy, icon->win, 0, 0, w, h, AllPlanes, ZPixmap);
  if (trap_pop () != 0 || image == NULL)
    {
      if (image != NULL)
        XDestroyImage (image);
      return G_SOURCE_REMOVE;
    }

  pixels = convert_image (image, wa.visual, wa.depth, w, h);
  XDestroyImage (image);

  if (pixels == NULL)
    return G_SOURCE_REMOVE;   /* transparente: conservar la imagen anterior */

  if (icon->pixels != NULL && icon->img_w == w && icon->img_h == h
      && memcmp (icon->pixels, pixels, (gsize) w * h * 4) == 0)
    {
      g_free (pixels);
      return G_SOURCE_REMOVE;   /* sin cambios: no molestar al host */
    }

  g_free (icon->pixels);
  icon->pixels = pixels;
  icon->img_w = w;
  icon->img_h = h;

  /* Recién ahora hay algo que mostrar */
  if (icon->xembed_mapped && icon->conn == NULL)
    icon_publish (icon);
  else
    icon_emit (icon, "NewIcon");

  return G_SOURCE_REMOVE;
}



static void
icon_schedule_update (Icon *icon, guint delay_ms)
{
  if (icon->update_id == 0)
    icon->update_id = g_timeout_add (delay_ms, icon_update, icon);
}



/* La app avisó (Damage) que dibujó algo. No capturar enseguida: una app
 * suele dibujar en varios pasos (borrar, fondo, ícono) y capturar a mitad
 * publica fotogramas a medio hacer. Se espera a que haya un silencio,
 * más largo justo después de un cambio de tamaño, pero con un tope para
 * que un ícono animado se siga actualizando. */
static void
icon_damaged (Icon *icon)
{
  gint64 now = g_get_monotonic_time ();
  gboolean settling = now < icon->settle_until;
  guint quiet = settling ? SETTLE_QUIET_MS : QUIET_MS;

  if (icon->update_id == 0)
    {
      icon->first_damage = now;
    }
  else
    {
      /* Lleva demasiado esperando (animación): dejar que salga la que está */
      if (!settling && (now - icon->first_damage) / 1000 >= MAX_WAIT_MS)
        return;
      g_source_remove (icon->update_id);
    }

  icon->update_id = g_timeout_add (quiet, icon_update, icon);
}



/* Tras anclar o redimensionar, la ventana tiene un contenido viejo o
 * basura hasta que la app se redibuja: no publicar nada hasta entonces */
static void
icon_begin_settle (Icon *icon, guint fallback_ms);



static gboolean
icon_late_update (gpointer user_data)
{
  Icon *icon = user_data;

  /* Respaldo: la app nunca avisó que se redibujó; capturar lo que haya */
  icon->late_update_id = 0;
  icon->settle_until = 0;
  icon_schedule_update (icon, 0);
  return G_SOURCE_REMOVE;
}



static void
icon_begin_settle (Icon *icon, guint fallback_ms)
{
  icon->settle_until = g_get_monotonic_time () + (gint64) fallback_ms * 1000;

  if (icon->update_id != 0)
    {
      g_source_remove (icon->update_id);
      icon->update_id = 0;
    }
  if (icon->late_update_id != 0)
    g_source_remove (icon->late_update_id);
  icon->late_update_id = g_timeout_add (fallback_ms, icon_late_update, icon);
}



/* ════════════════════════════════════════════════════════════════════════
 * Clics simulados
 * ════════════════════════════════════════════════════════════════════════ */

static void
container_set_input (Icon *icon, gint x, gint y, gboolean active)
{
  XRectangle rect = { (short) x, (short) y, 1, 1 };

  /* Inactivo: forma vacía (invisible incluso sin compositor) y sin zona
   * de entrada (los clics la atraviesan). Activo: un solo píxel. */
  XShapeCombineRectangles (P.dpy, icon->container, ShapeBounding, 0, 0,
                           &rect, active ? 1 : 0, ShapeSet, Unsorted);
  XShapeCombineRectangles (P.dpy, icon->container, ShapeInput, 0, 0,
                           &rect, active ? 1 : 0, ShapeSet, Unsorted);
  if (active)
    XRaiseWindow (P.dpy, icon->container);
  else
    XLowerWindow (P.dpy, icon->container);
}



/* Wine usa formas (Shape) en sus íconos: buscar un punto realmente
 * clicable cerca de la esquina superior izquierda */
static void
click_point (Icon *icon, gint *cx, gint *cy)
{
  XWindowAttributes wa;
  XRectangle *rects;
  gint count = 0, ordering, i;
  gint best = G_MAXINT;

  memset (&wa, 0, sizeof (wa));
  *cx = icon->size / 2;
  *cy = icon->size / 2;

  if (XGetWindowAttributes (P.dpy, icon->win, &wa))
    {
      *cx = wa.width / 2;
      *cy = wa.height / 2;
    }

  rects = XShapeGetRectangles (P.dpy, icon->win, ShapeBounding, &count, &ordering);
  if (rects != NULL)
    {
      gboolean shaped = !(count == 1 && rects[0].x == 0 && rects[0].y == 0
                          && rects[0].width == wa.width && rects[0].height == wa.height);

      for (i = 0; shaped && i < count; i++)
        {
          gint d = rects[i].x * rects[i].x + rects[i].y * rects[i].y;

          if (d < best && rects[i].width > 0 && rects[i].height > 0)
            {
              best = d;
              *cx = rects[i].x + rects[i].width / 2;
              *cy = rects[i].y + rects[i].height / 2;
            }
        }
      XFree (rects);
    }
}



static void
send_button_event (Icon *icon, gint type, gint button, gint rx, gint ry, gint cx, gint cy)
{
  XEvent ev;
  Window target = icon->win, child = None;
  gint tx = cx, ty = cy, depth;

  /* El clic va a la subventana más profunda bajo el punto, como haría
   * el servidor con un clic real: Java, por ejemplo, lo escucha en una
   * subventana y no en la ventana del ícono */
  for (depth = 0; depth < 8; depth++)
    {
      gint nx, ny;

      if (!XTranslateCoordinates (P.dpy, icon->win, target, cx, cy, &nx, &ny, &child))
        break;
      tx = nx;
      ty = ny;
      if (child == None)
        break;
      target = child;
    }

  memset (&ev, 0, sizeof (ev));
  ev.xbutton.type = type;
  ev.xbutton.display = P.dpy;
  ev.xbutton.window = target;
  ev.xbutton.root = P.root;
  ev.xbutton.subwindow = None;
  ev.xbutton.time = CurrentTime;
  ev.xbutton.x = tx;
  ev.xbutton.y = ty;
  ev.xbutton.x_root = rx;
  ev.xbutton.y_root = ry;
  ev.xbutton.button = button;
  ev.xbutton.same_screen = True;
  ev.xbutton.state = type == ButtonRelease ? (Button1Mask << (button - 1)) : 0;

  XSendEvent (P.dpy, target, False,
              type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &ev);
}



/* --wayland: un rato después del clic (la app ya ubicó su menú), sacar
 * el contenedor de la pantalla. XWayland no respeta su forma vacía y
 * algunos compositores lo muestran como un cuadrado negro. */
static gboolean
park_container (gpointer user_data)
{
  Icon *icon = user_data;

  icon->park_id = 0;
  trap_push ();
  XMoveWindow (P.dpy, icon->container, -2 * MAX_SIZE, -2 * MAX_SIZE);
  trap_pop ();
  return G_SOURCE_REMOVE;
}



/* --wayland: (rx, ry) es un punto justo afuera del borde del panel, y las
 * ventanas X quedan debajo del panel. El contenedor va del lado del borde
 * de pantalla más cercano, con un lado sobre ese punto: queda tapado por
 * el panel y el menú de la app sale pegado a él. */
static void
wayland_container_origin (Icon *icon, gint rx, gint ry, gint *x, gint *y)
{
  gint sw = DisplayWidth (P.dpy, P.screen);
  gint sh = DisplayHeight (P.dpy, P.screen);
  gint left = rx, right = sw - rx, top = ry, bottom = sh - ry;
  gint nearest = MIN (MIN (left, right), MIN (top, bottom));

  *x = rx - icon->size / 2;
  *y = ry - icon->size / 2;

  if (nearest == bottom)
    *y = ry + 1;
  else if (nearest == top)
    *y = ry - icon->size;
  else if (nearest == right)
    *x = rx + 1;
  else
    *x = rx - icon->size;
}



static void
do_click (Icon *icon, gint button, gint rx, gint ry)
{
  gint cx, cy, ox, oy;

  trap_push ();
  click_point (icon, &cx, &cy);

  /* Poner el ícono justo debajo del puntero. Las apps calculan dónde
   * mostrar su menú a partir de la posición de su ventana, así que el
   * contenedor se queda ahí después del clic (invisible). */
  ox = rx - cx;
  oy = ry - cy;
  if (P.wayland)
    {
      if (icon->park_id != 0)
        g_source_remove (icon->park_id);
      icon->park_id = g_timeout_add (PARK_DELAY_MS, park_container, icon);
      wayland_container_origin (icon, rx, ry, &ox, &oy);
    }
  XMoveWindow (P.dpy, icon->container, ox, oy);
  container_set_input (icon, cx, cy, TRUE);
  XSync (P.dpy, False);

  LOG ("clic %d en 0x%lx (%s) en %d,%d\n", button, icon->win,
       icon->direct_events || !P.have_xtest ? "directo" : "XTest", rx, ry);

  if (icon->direct_events || !P.have_xtest)
    {
      send_button_event (icon, ButtonPress, button, rx, ry, cx, cy);
      send_button_event (icon, ButtonRelease, button, rx, ry, cx, cy);
    }
  else
    {
      /* XWayland: su puntero no sigue al real fuera de las ventanas X;
       * llevarlo al ícono para que el clic simulado caiga en él */
      if (P.wayland)
        XTestFakeMotionEvent (P.dpy, -1, ox + cx, oy + cy, CurrentTime);
      XTestFakeButtonEvent (P.dpy, button, True, CurrentTime);
      XTestFakeButtonEvent (P.dpy, button, False, CurrentTime);
    }

  XSync (P.dpy, False);
  container_set_input (icon, cx, cy, FALSE);
  trap_pop ();
}



static gboolean
click_try (gpointer user_data)
{
  Icon *icon = user_data;
  Window root_ret, child_ret;
  gint rx, ry, wx, wy, i;
  guint mask = 0;

  XQueryPointer (P.dpy, P.root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);

  /* XWayland no sabe dónde está el puntero (sobre el panel, que no es una
   * ventana X): usar la posición del ícono que manda el host, para que la
   * app muestre su menú junto a él. El botón físico tampoco se ve desde X. */
  if (P.wayland)
    {
      rx = icon->click_x;
      ry = icon->click_y;
    }
  /* Si el botón físico sigue presionado (p. ej. el clic derecho llega al
   * presionar), un clic simulado se fusionaría con él y se perdería */
  else if ((mask & (Button1Mask | Button2Mask | Button3Mask)) && ++icon->click_tries < 100)
    return G_SOURCE_CONTINUE;

  icon->click_id = 0;
  for (i = 0; i < icon->n_clicks; i++)
    do_click (icon, icon->click_buttons[i], rx, ry);
  icon->n_clicks = 0;
  return G_SOURCE_REMOVE;
}



/* El host pide otro tamaño (p. ej. el del panel o el del popup, por la
 * escala HiDPI): redimensionar la ventana para que la app se redibuje
 * nítida a ese tamaño, como haría con una bandeja normal */
static void
icon_set_size (Icon *icon, gint size)
{
  size = CLAMP (size, MIN_SIZE, MAX_SIZE);
  if (size == icon->size)
    return;

  LOG ("tamaño de 0x%lx: %d → %d px\n", icon->win, icon->size, size);
  icon->size = size;

  /* Con ForgetGravity y fondo transparente (ver dock), X descarta el
   * contenido viejo y limpia la ventana: no queda basura ni el ícono
   * anterior en una esquina. Esperar a que la app se redibuje. */
  icon_begin_settle (icon, SETTLE_FALLBACK_MS);

  trap_push ();
  XResizeWindow (P.dpy, icon->container, size, size);
  XResizeWindow (P.dpy, icon->win, size, size);
  trap_pop ();
}



/* Encola un clic o un paso de rueda. Antes, uno nuevo reemplazaba al
 * pendiente: varios giros rápidos contaban como uno. */
static void
icon_click (Icon *icon, gint button)
{
  if (icon->n_clicks >= MAX_PENDING_CLICKS)
    return;

  icon->click_buttons[icon->n_clicks++] = (guint8) button;
  icon->click_tries = 0;
  if (icon->click_id == 0)
    icon->click_id = g_timeout_add (10, click_try, icon);
}



/* ════════════════════════════════════════════════════════════════════════
 * Publicación StatusNotifierItem
 * ════════════════════════════════════════════════════════════════════════ */

static void
item_method_call (GDBusConnection       *conn,
                  const gchar           *sender,
                  const gchar           *path,
                  const gchar           *iface,
                  const gchar           *method,
                  GVariant              *params,
                  GDBusMethodInvocation *invocation,
                  gpointer               user_data)
{
  Icon *icon = user_data;

  if (g_strcmp0 (iface, TO_IFACE) == 0 && g_strcmp0 (method, "SetIconSize") == 0)
    {
      gint size = 0;

      g_variant_get (params, "(i)", &size);
      icon_set_size (icon, size);
    }
  else if (g_strcmp0 (iface, TO_IFACE) == 0 && g_strcmp0 (method, "SetPointer") == 0)
    {
      /* El host avisa dónde está el puntero antes de Scroll, que no trae
       * posición (en Wayland hace falta para llevar ahí el de XWayland) */
      g_variant_get (params, "(ii)", &icon->click_x, &icon->click_y);
    }
  else if (g_strcmp0 (method, "Activate") == 0
           || g_strcmp0 (method, "SecondaryActivate") == 0
           || g_strcmp0 (method, "ContextMenu") == 0)
    {
      gint x = 0, y = 0;

      g_variant_get (params, "(ii)", &x, &y);
      icon->click_x = x;
      icon->click_y = y;
      icon_click (icon, method[0] == 'A' ? 1 : method[0] == 'S' ? 2 : 3);
    }
  else if (g_strcmp0 (method, "Scroll") == 0)
    {
      gint delta = 0, button, steps;
      const gchar *orientation = NULL;

      g_variant_get (params, "(i&s)", &delta, &orientation);
      if (g_ascii_strcasecmp (orientation, "horizontal") == 0)
        button = delta < 0 ? 6 : 7;
      else
        button = delta < 0 ? 4 : 5;

      /* Un paso de rueda por unidad (en X, cada paso es un clic) */
      for (steps = MIN (ABS (delta), MAX_PENDING_CLICKS); steps > 0; steps--)
        icon_click (icon, button);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method %s", method);
      return;
    }

  g_dbus_method_invocation_return_value (invocation, NULL);
}



static GVariant *
pixmap_variant (Icon *icon)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiay)"));
  if (icon->pixels != NULL)
    g_variant_builder_add (&builder, "(ii@ay)", icon->img_w, icon->img_h,
                           g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, icon->pixels,
                                                      (gsize) icon->img_w * icon->img_h * 4, 1));
  return g_variant_builder_end (&builder);
}



static GVariant *
item_get_property (GDBusConnection *conn,
                   const gchar     *sender,
                   const gchar     *path,
                   const gchar     *iface,
                   const gchar     *name,
                   GError         **error,
                   gpointer         user_data)
{
  Icon *icon = user_data;

  if (g_strcmp0 (name, "Category") == 0)
    return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (name, "Id") == 0)
    return g_variant_new_string (icon->id);
  if (g_strcmp0 (name, "Title") == 0)
    return g_variant_new_string (icon->title);
  if (g_strcmp0 (name, "Status") == 0)
    return g_variant_new_string ("Active");
  if (g_strcmp0 (name, "WindowId") == 0)
    return g_variant_new_int32 ((gint32) icon->win);
  if (g_strcmp0 (name, "IconName") == 0)
    return g_variant_new_string ("");
  if (g_strcmp0 (name, "IconPixmap") == 0)
    return pixmap_variant (icon);
  if (g_strcmp0 (name, "ToolTip") == 0)
    return g_variant_new ("(s@a(iiay)ss)", "",
                          g_variant_new_array (G_VARIANT_TYPE ("(iiay)"), NULL, 0),
                          icon->title, "");
  if (g_strcmp0 (name, "ItemIsMenu") == 0)
    return g_variant_new_boolean (FALSE);
  if (g_strcmp0 (name, "Menu") == 0)
    return g_variant_new_object_path ("/NO_DBUSMENU");

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s", name);
  return NULL;
}



static const GDBusInterfaceVTable item_vtable = {
  item_method_call,
  item_get_property,
  NULL,
  { NULL }
};



static void
icon_register_with_watcher (Icon *icon)
{
  if (icon->conn == NULL)
    return;

  g_dbus_connection_call (icon->conn, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", g_dbus_connection_get_unique_name (icon->conn)),
                          NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
}



static void
icon_publish (Icon *icon)
{
  GError *error = NULL;

  if (icon->conn != NULL || icon->pixels == NULL)
    return;

  icon->conn = g_dbus_connection_new_for_address_sync (
                 P.bus_address,
                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                 | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                 NULL, NULL, &error);
  if (icon->conn == NULL)
    {
      g_printerr (PROXY_NAME ": no se pudo conectar a D-Bus: %s\n", error->message);
      g_error_free (error);
      return;
    }

  g_dbus_connection_set_exit_on_close (icon->conn, FALSE);
  icon->reg_id = g_dbus_connection_register_object (icon->conn, ITEM_PATH,
                                                    P.node->interfaces[0], &item_vtable,
                                                    icon, NULL, &error);
  if (icon->reg_id == 0)
    {
      g_printerr (PROXY_NAME ": no se pudo exportar el ícono: %s\n", error->message);
      g_error_free (error);
      icon_unpublish (icon);
      return;
    }

  /* Interfaz propia para que Tray Overflow pida el tamaño de dibujo;
   * otros hosts simplemente no la usan */
  icon->reg_size_id = g_dbus_connection_register_object (icon->conn, ITEM_PATH,
                                                         P.node->interfaces[1], &item_vtable,
                                                         icon, NULL, NULL);

  LOG ("publicado 0x%lx como %s\n", icon->win, g_dbus_connection_get_unique_name (icon->conn));
  icon_register_with_watcher (icon);
}



static void
icon_unpublish (Icon *icon)
{
  if (icon->conn == NULL)
    return;

  if (icon->reg_id != 0)
    g_dbus_connection_unregister_object (icon->conn, icon->reg_id);
  if (icon->reg_size_id != 0)
    g_dbus_connection_unregister_object (icon->conn, icon->reg_size_id);
  icon->reg_id = 0;
  icon->reg_size_id = 0;

  /* Cerrar la conexión hace desaparecer su nombre único del bus: así el
   * watcher (el nuestro, el de XFCE o el de KDE) da de baja el ícono */
  g_dbus_connection_close_sync (icon->conn, NULL, NULL);
  g_clear_object (&icon->conn);
}



static void
watcher_appeared (GDBusConnection *conn,
                  const gchar     *name,
                  const gchar     *owner,
                  gpointer         user_data)
{
  GHashTableIter iter;
  gpointer value;

  /* Un watcher nuevo no conoce nuestros íconos: volver a registrarlos */
  LOG ("apareció el watcher %s\n", owner);
  g_hash_table_iter_init (&iter, P.icons);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    icon_register_with_watcher (value);
}



/* ════════════════════════════════════════════════════════════════════════
 * Anclar y desanclar íconos
 * ════════════════════════════════════════════════════════════════════════ */

static void
send_xembed_message (Window win, glong message, glong detail, glong data1, glong data2)
{
  XEvent ev;

  memset (&ev, 0, sizeof (ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = P.a_xembed;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = CurrentTime;
  ev.xclient.data.l[1] = message;
  ev.xclient.data.l[2] = detail;
  ev.xclient.data.l[3] = data1;
  ev.xclient.data.l[4] = data2;
  XSendEvent (P.dpy, win, False, NoEventMask, &ev);
}



static void
icon_free (gpointer data)
{
  Icon *icon = data;

  if (icon->update_id != 0)
    g_source_remove (icon->update_id);
  if (icon->late_update_id != 0)
    g_source_remove (icon->late_update_id);
  if (icon->click_id != 0)
    g_source_remove (icon->click_id);
  if (icon->park_id != 0)
    g_source_remove (icon->park_id);
  icon_unpublish (icon);

  trap_push ();
  if (icon->damage != None)
    XDamageDestroy (P.dpy, icon->damage);

  /* Devolver la ventana de la app a la raíz antes de destruir el
   * contenedor: destruir un padre destruye sus hijos, aunque sean ajenos */
  XUnmapWindow (P.dpy, icon->win);
  XCompositeUnredirectWindow (P.dpy, icon->win, CompositeRedirectManual);
  XReparentWindow (P.dpy, icon->win, P.root, 0, 0);
  XRemoveFromSaveSet (P.dpy, icon->win);
  XDestroyWindow (P.dpy, icon->container);
  trap_pop ();

  g_free (icon->pixels);
  g_free (icon->id);
  g_free (icon->title);
  g_free (icon);
}



static void
dock (Window win)
{
  XSetWindowAttributes attrs;
  XWindowAttributes wa;
  gulong opacity = 0;
  Icon *icon;

  if (win == None || g_hash_table_contains (P.icons, GSIZE_TO_POINTER (win)))
    return;

  /* Vigilar la ventana de la app (cierre, cambios de título,
   * XEMBED_MAPPED) ANTES de leer su estado: si la app lo cambia justo
   * después de leerlo, el aviso llega igual. Al revés, se perdía y el
   * ícono quedaba anclado sin publicarse nunca. */
  trap_push ();
  XSelectInput (P.dpy, win, StructureNotifyMask | PropertyChangeMask);
  if (!XGetWindowAttributes (P.dpy, win, &wa) || trap_pop () != 0)
    return;

  icon = g_new0 (Icon, 1);
  icon->win = win;
  icon->size = DEFAULT_SIZE;
  icon->xembed_mapped = read_xembed_mapped (win);
  icon->ignore_unmaps = wa.map_state != IsUnmapped ? 1 : 0;

  trap_push ();

  /* Contenedor: ventana propia, fuera del control del gestor de ventanas,
   * invisible y sin entrada (ver container_set_input) */
  attrs.background_pixel = 0;
  attrs.border_pixel = 0;
  attrs.override_redirect = True;
  attrs.colormap = P.colormap;
  attrs.event_mask = SubstructureNotifyMask | SubstructureRedirectMask;
  /* --wayland: fuera de la pantalla hasta el primer clic (ver park_container) */
  icon->container = XCreateWindow (P.dpy, P.root,
                                   P.wayland ? -2 * MAX_SIZE : 0, P.wayland ? -2 * MAX_SIZE : 0,
                                   icon->size, icon->size, 0,
                                   P.depth, InputOutput, P.visual,
                                   CWBackPixel | CWBorderPixel | CWOverrideRedirect
                                   | CWColormap | CWEventMask, &attrs);
  XStoreName (P.dpy, icon->container, PROXY_NAME "-container");
  XChangeProperty (P.dpy, icon->container, P.a_opacity, XA_CARDINAL, 32,
                   PropModeReplace, (guchar *) &opacity, 1);
  container_set_input (icon, 0, 0, FALSE);
  XMapWindow (P.dpy, icon->container);

  XAddToSaveSet (P.dpy, win);
  XReparentWindow (P.dpy, win, icon->container, 0, 0);

  /* Fondo transparente y ForgetGravity en la ventana de la app: cuando se
   * muestra o cambia de tamaño, X la limpia a transparente en lugar de
   * dejar memoria basura (que suele ser un trozo de lo que había en
   * pantalla) o el contenido anterior pegado en una esquina */
  {
    XSetWindowAttributes app_attrs;

    app_attrs.background_pixel = 0;
    app_attrs.bit_gravity = ForgetGravity;
    XChangeWindowAttributes (P.dpy, win, CWBackPixel | CWBitGravity, &app_attrs);
  }

  /* Que se dibuje fuera de pantalla, en un pixmap que podemos leer */
  XCompositeRedirectWindow (P.dpy, win, CompositeRedirectManual);
  icon->damage = XDamageCreate (P.dpy, win, XDamageReportNonEmpty);

  /* Darle siempre el tamaño de destino: así la app (como con una bandeja
   * normal) elige y dibuja el ícono a ese tamaño, sin reescalados */
  XResizeWindow (P.dpy, win, icon->size, icon->size);

  send_xembed_message (win, XEMBED_EMBEDDED_NOTIFY, 0, icon->container, XEMBED_VERSION);
  XMapWindow (P.dpy, win);

  if (trap_pop () != 0)
    {
      /* La ventana desapareció mientras la anclábamos */
      LOG ("no se pudo anclar 0x%lx\n", win);
      trap_push ();
      XDestroyWindow (P.dpy, icon->container);
      trap_pop ();
      g_free (icon);
      return;
    }

  icon->direct_events = wants_core_button_events (win, 0);
  icon_read_identity (icon);
  g_hash_table_insert (P.icons, GSIZE_TO_POINTER (win), icon);

  /* Esperar a que la app se dibuje por primera vez (llega un Damage); si
   * nunca avisa, se captura igual pasado el respaldo */
  icon_begin_settle (icon, SETTLE_FALLBACK_MS);
}



static void
undock (Window win)
{
  if (g_hash_table_remove (P.icons, GSIZE_TO_POINTER (win)))
    LOG ("desanclado 0x%lx\n", win);
}



/* ════════════════════════════════════════════════════════════════════════
 * Selección de la bandeja (_NET_SYSTEM_TRAY_Sn)
 * ════════════════════════════════════════════════════════════════════════ */

static Time
server_time (void)
{
  XEvent ev;
  guchar zero = 0;

  /* Un cambio de propiedad vacío devuelve la hora del servidor */
  XChangeProperty (P.dpy, P.manager, P.a_manager, XA_STRING, 8, PropModeAppend, &zero, 0);
  XWindowEvent (P.dpy, P.manager, PropertyChangeMask, &ev);
  return ev.xproperty.time;
}



static void
try_claim (void)
{
  XEvent ev;
  VisualID visual_id;
  gulong orientation = 0;

  if (P.owner || XGetSelectionOwner (P.dpy, P.a_selection) != None)
    return;

  P.claim_time = server_time ();
  XSetSelectionOwner (P.dpy, P.a_selection, P.manager, P.claim_time);
  if (XGetSelectionOwner (P.dpy, P.a_selection) != P.manager)
    return;

  P.owner = TRUE;

  /* Pedir íconos con transparencia y del tamaño que capturamos */
  visual_id = XVisualIDFromVisual (P.visual);
  XChangeProperty (P.dpy, P.manager, P.a_tray_visual, XA_VISUALID, 32,
                   PropModeReplace, (guchar *) &visual_id, 1);
  XChangeProperty (P.dpy, P.manager, P.a_tray_orientation, XA_CARDINAL, 32,
                   PropModeReplace, (guchar *) &orientation, 1);

  /* _NET_SYSTEM_TRAY_ICON_SIZE NO se publica a propósito: si existe, GTK
   * dibuja siempre a ese tamaño e ignora el de la ventana, y cada ícono
   * necesita el suyo (ver icon_set_size) */
  XDeleteProperty (P.dpy, P.manager, P.a_tray_icon_size);

  /* Anunciar la nueva bandeja: las apps se anclan solas */
  memset (&ev, 0, sizeof (ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = P.root;
  ev.xclient.message_type = P.a_manager;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = P.claim_time;
  ev.xclient.data.l[1] = P.a_selection;
  ev.xclient.data.l[2] = P.manager;
  XSendEvent (P.dpy, P.root, False, StructureNotifyMask, &ev);
  XFlush (P.dpy);

  g_printerr (PROXY_NAME ": bandeja clásica activa\n");
}



static void
lost_selection (void)
{
  P.owner = FALSE;
  g_hash_table_remove_all (P.icons);
  g_printerr (PROXY_NAME ": otra bandeja tomó el control; esperando a que se libere\n");
}



/* ════════════════════════════════════════════════════════════════════════
 * Eventos de X
 * ════════════════════════════════════════════════════════════════════════ */

static Icon *
icon_for (Window win)
{
  return g_hash_table_lookup (P.icons, GSIZE_TO_POINTER (win));
}



static void
handle_event (XEvent *ev)
{
  Icon *icon;

  if (ev->type == P.damage_event + XDamageNotify)
    {
      XDamageNotifyEvent *dn = (XDamageNotifyEvent *) ev;

      icon = icon_for (dn->drawable);
      if (icon != NULL)
        {
          XDamageSubtract (P.dpy, dn->damage, None, None);
          icon_damaged (icon);
        }
      return;
    }

  if (ev->type == P.fixes_event + XFixesSelectionNotify)
    {
      XFixesSelectionNotifyEvent *sn = (XFixesSelectionNotifyEvent *) ev;

      /* La bandeja que había se fue: intentar tomar su lugar */
      if (!P.owner && sn->selection == P.a_selection && sn->owner == None)
        try_claim ();
      return;
    }

  switch (ev->type)
    {
    case ClientMessage:
      if (ev->xclient.window == P.manager && ev->xclient.message_type == P.a_opcode
          && ev->xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK && P.owner)
        dock ((Window) ev->xclient.data.l[2]);
      break;

    case SelectionClear:
      if (ev->xselectionclear.selection == P.a_selection
          && ev->xselectionclear.window == P.manager)
        lost_selection ();
      break;

    case DestroyNotify:
      undock (ev->xdestroywindow.window);
      break;

    case UnmapNotify:
      icon = icon_for (ev->xunmap.window);
      if (icon != NULL)
        {
          if (icon->ignore_unmaps > 0)
            icon->ignore_unmaps--;
          else
            undock (ev->xunmap.window);
        }
      break;

    case ReparentNotify:
      icon = icon_for (ev->xreparent.window);
      if (icon != NULL && ev->xreparent.parent != icon->container)
        undock (ev->xreparent.window);   /* la app se llevó su ventana */
      break;

    case MapRequest:
      /* Con SubstructureRedirect, los intentos de la app de mostrarse
       * llegan a nosotros */
      if (icon_for (ev->xmaprequest.window) != NULL)
        XMapWindow (P.dpy, ev->xmaprequest.window);
      break;

    case ConfigureRequest:
      icon = icon_for (ev->xconfigurerequest.window);
      if (icon != NULL && (ev->xconfigurerequest.value_mask & (CWWidth | CWHeight)))
        {
          /* Se aceptan cambios de tamaño (sin pasar del de destino), no de posición */
          gint w = CLAMP (ev->xconfigurerequest.width, 1, icon->size);
          gint h = CLAMP (ev->xconfigurerequest.height, 1, icon->size);

          trap_push ();
          icon_begin_settle (icon, SETTLE_FALLBACK_MS);
          XResizeWindow (P.dpy, icon->win, w, h);
          trap_pop ();
        }
      break;

    case PropertyNotify:
      icon = icon_for (ev->xproperty.window);
      if (icon == NULL)
        break;

      if (ev->xproperty.atom == P.a_xembed_info)
        {
          gboolean mapped = read_xembed_mapped (icon->win);

          if (mapped != icon->xembed_mapped)
            {
              /* La app ocultó o volvió a mostrar su ícono */
              icon->xembed_mapped = mapped;
              if (mapped)
                icon_publish (icon);
              else
                icon_unpublish (icon);
            }
        }
      else if (ev->xproperty.atom == P.a_net_wm_name || ev->xproperty.atom == XA_WM_NAME)
        {
          gchar *title = get_window_title (icon->win);

          if (title != NULL && *title != '\0' && !title_is_generic (title)
              && g_strcmp0 (title, icon->title) != 0)
            {
              g_free (icon->title);
              icon->title = title;
              icon_emit (icon, "NewTitle");
              icon_emit (icon, "NewToolTip");
            }
          else
            {
              g_free (title);
            }
        }
      break;

    default:
      break;
    }
}



/* Fuente de eventos de X para el bucle de GLib. Mirar solo el socket no
 * alcanza: XSync, XGetImage o XQueryPointer (en trap_pop, icon_update,
 * click_try…) pueden leer eventos a la cola de Xlib, y ahí quedarían sin
 * atender hasta que llegue otro. prepare y check miran también esa cola
 * (como hace GDK). GLib no despacha una fuente dentro de sí misma, así
 * que handle_event no se llama de forma reentrante. */
typedef struct
{
  GSource  source;
  gpointer fd_tag;
} XSource;



static gboolean
x_source_prepare (GSource *source, gint *timeout)
{
  /* Mandar lo pendiente antes de dormir: las respuestas y los eventos
   * que provoque llegan al socket */
  XFlush (P.dpy);
  *timeout = -1;
  return XEventsQueued (P.dpy, QueuedAlready) > 0;
}



static gboolean
x_source_check (GSource *source)
{
  XSource *xs = (XSource *) source;

  if (g_source_query_unix_fd (source, xs->fd_tag) & G_IO_IN)
    return XEventsQueued (P.dpy, QueuedAfterReading) > 0;
  return XEventsQueued (P.dpy, QueuedAlready) > 0;
}



static void
x_events_drain (void)
{
  while (XPending (P.dpy))
    {
      XEvent ev;

      XNextEvent (P.dpy, &ev);
      handle_event (&ev);
    }
  XFlush (P.dpy);
}



static gboolean
x_source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
  x_events_drain ();
  return G_SOURCE_CONTINUE;
}



static GSourceFuncs x_source_funcs = {
  x_source_prepare,
  x_source_check,
  x_source_dispatch,
  NULL, NULL, NULL
};



static void
x_source_attach (void)
{
  GSource *source = g_source_new (&x_source_funcs, sizeof (XSource));
  XSource *xs = (XSource *) source;

  xs->fd_tag = g_source_add_unix_fd (source, ConnectionNumber (P.dpy), G_IO_IN);
  g_source_set_name (source, "X events");
  g_source_attach (source, NULL);
  g_source_unref (source);
}



static gboolean
quit_signal (gpointer user_data)
{
  g_main_loop_quit (P.loop);
  return G_SOURCE_REMOVE;
}



/* ════════════════════════════════════════════════════════════════════════
 * Arranque
 * ════════════════════════════════════════════════════════════════════════ */

static gboolean
init_x11 (void)
{
  XVisualInfo vinfo;
  gint major = 0, minor = 2, base, error_base;
  gchar *name;

  P.dpy = XOpenDisplay (NULL);
  if (P.dpy == NULL)
    {
      g_printerr (PROXY_NAME ": no hay pantalla X11 (DISPLAY)\n");
      return FALSE;
    }

  XSetErrorHandler (x_error_handler);
  XSetIOErrorHandler (x_io_error_handler);

  P.screen = DefaultScreen (P.dpy);
  P.root = RootWindow (P.dpy, P.screen);

  if (!XCompositeQueryExtension (P.dpy, &base, &error_base)
      || !XCompositeQueryVersion (P.dpy, &major, &minor) || (major == 0 && minor < 2))
    {
      g_printerr (PROXY_NAME ": el servidor X no tiene la extensión Composite\n");
      return FALSE;
    }
  if (!XDamageQueryExtension (P.dpy, &P.damage_event, &error_base))
    {
      g_printerr (PROXY_NAME ": el servidor X no tiene la extensión Damage\n");
      return FALSE;
    }
  if (!XFixesQueryExtension (P.dpy, &P.fixes_event, &error_base))
    {
      g_printerr (PROXY_NAME ": el servidor X no tiene la extensión XFixes\n");
      return FALSE;
    }
  if (!XShapeQueryExtension (P.dpy, &base, &error_base))
    {
      g_printerr (PROXY_NAME ": el servidor X no tiene la extensión Shape\n");
      return FALSE;
    }
  P.have_xtest = XTestQueryExtension (P.dpy, &base, &base, &major, &minor);

  /* Visual ARGB para que los íconos tengan transparencia real */
  if (XMatchVisualInfo (P.dpy, P.screen, 32, TrueColor, &vinfo))
    {
      P.visual = vinfo.visual;
      P.depth = 32;
      P.colormap = XCreateColormap (P.dpy, P.root, P.visual, AllocNone);
    }
  else
    {
      P.visual = DefaultVisual (P.dpy, P.screen);
      P.depth = DefaultDepth (P.dpy, P.screen);
      P.colormap = DefaultColormap (P.dpy, P.screen);
    }

  name = g_strdup_printf ("_NET_SYSTEM_TRAY_S%d", P.screen);
  P.a_selection = XInternAtom (P.dpy, name, False);
  g_free (name);
  P.a_manager = XInternAtom (P.dpy, "MANAGER", False);
  P.a_opcode = XInternAtom (P.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
  P.a_tray_visual = XInternAtom (P.dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
  P.a_tray_orientation = XInternAtom (P.dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
  P.a_tray_icon_size = XInternAtom (P.dpy, "_NET_SYSTEM_TRAY_ICON_SIZE", False);
  P.a_xembed = XInternAtom (P.dpy, "_XEMBED", False);
  P.a_xembed_info = XInternAtom (P.dpy, "_XEMBED_INFO", False);
  P.a_net_wm_name = XInternAtom (P.dpy, "_NET_WM_NAME", False);
  P.a_net_wm_pid = XInternAtom (P.dpy, "_NET_WM_PID", False);
  P.a_utf8 = XInternAtom (P.dpy, "UTF8_STRING", False);
  P.a_opacity = XInternAtom (P.dpy, "_NET_WM_WINDOW_OPACITY", False);

  /* Ventana dueña de la selección; su nombre permite al plugin saber
   * que la bandeja clásica activa es este proxy */
  P.manager = XCreateSimpleWindow (P.dpy, P.root, -1, -1, 1, 1, 0, 0, 0);
  XStoreName (P.dpy, P.manager, PROXY_NAME);
  XChangeProperty (P.dpy, P.manager, P.a_net_wm_name, P.a_utf8, 8, PropModeReplace,
                   (const guchar *) PROXY_NAME, strlen (PROXY_NAME));
  XSelectInput (P.dpy, P.manager, PropertyChangeMask | StructureNotifyMask);

  /* Enterarse cuando la bandeja actual (si hay) desaparezca */
  XFixesSelectSelectionInput (P.dpy, P.root, P.a_selection,
                              XFixesSetSelectionOwnerNotifyMask
                              | XFixesSelectionWindowDestroyNotifyMask
                              | XFixesSelectionClientCloseNotifyMask);
  return TRUE;
}



int
main (int argc, char **argv)
{
  gboolean verbose = FALSE, version = FALSE;
  GOptionEntry entries[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Mostrar mensajes de diagnóstico", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Mostrar la versión", NULL },
    { "wayland", 0, 0, G_OPTION_ARG_NONE, &P.wayland,
      "Sesión Wayland: usar la posición que manda el host en los clics", NULL },
    { NULL }
  };
  GOptionContext *context;
  GError *error = NULL;

  context = g_option_context_new (NULL);
  g_option_context_set_summary (context,
    "Muestra los íconos de bandeja clásicos (XEmbed) como íconos StatusNotifier.");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return 2;
    }
  g_option_context_free (context);

  if (version)
    {
      g_print (PROXY_NAME " " PACKAGE_VERSION "\n");
      return 0;
    }

  P.verbose = verbose || g_getenv ("TRAYOVERFLOW_PROXY_DEBUG") != NULL;

  if (!init_x11 ())
    return 1;

  P.bus_address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, &error);
  P.bus = P.bus_address != NULL ? g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error) : NULL;
  if (P.bus == NULL)
    {
      g_printerr (PROXY_NAME ": sin bus de sesión D-Bus: %s\n", error->message);
      return 1;
    }

  P.node = g_dbus_node_info_new_for_xml (item_xml, NULL);
  P.icons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, icon_free);
  P.loop = g_main_loop_new (NULL, FALSE);

  g_bus_watch_name_on_connection (P.bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  watcher_appeared, NULL, NULL, NULL);

  x_source_attach ();
  g_unix_signal_add (SIGTERM, quit_signal, NULL);
  g_unix_signal_add (SIGINT, quit_signal, NULL);
  g_unix_signal_add (SIGHUP, quit_signal, NULL);

  try_claim ();
  if (!P.owner)
    g_printerr (PROXY_NAME ": hay otra bandeja clásica activa; esperando a que se libere\n");

  x_events_drain ();
  g_main_loop_run (P.loop);

  /* Salida limpia: devolver las ventanas y liberar la bandeja para que
   * otra (p. ej. la nativa) pueda tomarla y las apps se vuelvan a anclar */
  g_hash_table_destroy (P.icons);
  if (P.owner)
    XSetSelectionOwner (P.dpy, P.a_selection, None, P.claim_time);
  XDestroyWindow (P.dpy, P.manager);
  XCloseDisplay (P.dpy);

  return 0;
}
