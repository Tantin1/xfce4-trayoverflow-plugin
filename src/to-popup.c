/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Popup (ventana emergente con los íconos ocultos): mostrar, ubicar y
 * cerrar; grab en X11 y foco en Wayland; menús abiertos desde el popup;
 * qué hacer tras un clic o un giro en sus íconos; esquinas; construcción.
 */
#include "to-common.h"
#include "to-plugin.h"
#include "to-popup.h"
#include "to-dnd.h"
#include "to-button.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#endif

#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

#define POPUP_GAP      4
/* Tras un clic en un ícono del popup: cuánto esperar sin grab, para que
 * la app reciba el clic (las apps antiguas lo reciben simulado) */
#define ACT_REGRAB_MS  600
/* Menú propio de una app antigua: más margen, para que lo muestre antes
 * de que intentemos recuperar el puntero (Java tarda) */
#define MENU_REGRAB_MS 1500
#define MENU_WAIT_MS   150   /* cada cuánto ver si ese menú se cerró */
/* Wayland: cuántas veces seguidas (con menos de REFOCUS_WINDOW_MS entre
 * una y otra) el popup recupera el foco antes de rendirse (ver focus_check).
 * Generoso: girar la rueda rápido puede traer una notificación por paso. */
#define REFOCUS_MAX       20
#define REFOCUS_WINDOW_MS 1000
/* Radio de las esquinas del popup: opción "redondeadas" y tope para el
 * que se copia del tema */
#define POPUP_RADIUS_ROUNDED 8
#define POPUP_RADIUS_MAX     24

static void kbd_select (TrayPlugin *p, GtkWidget *button);



/* ════════════════════════════════════════════════════════════════════════
 * Popup: grab y cierre
 * ════════════════════════════════════════════════════════════════════════ */

gboolean
tray_popup_try_grab (gpointer user_data)
{
  TrayPlugin *p = user_data;
  GdkWindow *window;
  GdkSeat *seat;
  GdkGrabStatus status;

  p->grab_retry_id = 0;

  if (p->popup == NULL || !gtk_widget_get_visible (p->popup)
      || p->dragging || p->menu_open || p->seat_grabbed)
    return G_SOURCE_REMOVE;

  window = gtk_widget_get_window (p->popup);
  seat = gdk_display_get_default_seat (gtk_widget_get_display (p->popup));

  /* owner_events = TRUE: los clics dentro de nuestras ventanas llegan
   * normalmente; los de afuera llegan al popup y lo cierran.
   * Wayland: sin grab del dispositivo. No alcanza a otras apps, y con él
   * GTK abre los tooltips como popups con grab: algunos compositores
   * (labwc 0.20) les dan el foco y el popup se cerraría. Basta el grab
   * de GTK de más abajo para los clics sobre el panel. */
  if (p->popup_layer)
    status = window != NULL ? GDK_GRAB_SUCCESS : GDK_GRAB_NOT_VIEWABLE;
  else
    status = window != NULL
             ? gdk_seat_grab (seat, window, GDK_SEAT_CAPABILITY_ALL, TRUE,
                              NULL, NULL, NULL, NULL)
             : GDK_GRAB_NOT_VIEWABLE;

  if (status != GDK_GRAB_SUCCESS)
    g_debug ("Grab del popup fallido (estado %d, intento %d)", status, p->grab_tries);

  if (status == GDK_GRAB_SUCCESS)
    {
      p->seat_grabbed = TRUE;
      p->hide_if_no_grab = FALSE;
      p->wait_app_menu = FALSE;
      /* El grab de GTK redirige al popup los clics sobre el resto del
       * panel (mismo proceso), así también lo cierran */
      if (!p->gtk_grabbed)
        {
          gtk_grab_add (p->popup);
          p->gtk_grabbed = TRUE;
        }
    }
  else if (++p->grab_tries < 20)
    {
      /* Otro grab (p. ej. un menú que se cierra) aún no se liberó */
      p->grab_retry_id = g_timeout_add (50, tray_popup_try_grab, p);
    }
  else if (p->hide_if_no_grab && p->wait_app_menu)
    {
      /* Una app antigua mostró su menú y tiene el puntero: el popup sigue
       * abierto y lo recupera cuando el menú se cierre */
      p->grab_retry_id = g_timeout_add (MENU_WAIT_MS, tray_popup_try_grab, p);
    }
  else if (p->hide_if_no_grab)
    {
      /* Tras un clic, la app tomó el control (p. ej. abrió un menú o un
       * control propio): sin grab el popup no se cerraría solo */
      tray_popup_hide (p);
    }

  return G_SOURCE_REMOVE;
}



/* ── Ventana activa ──────────────────────────────────────────────────────
 * Como en Windows: el popup se cierra cuando otra app pasa a primer
 * plano (p. ej. abrió su ventana tras un clic), no por el clic en sí. */

#ifdef GDK_WINDOWING_X11
static gulong
read_active_window (GdkDisplay *display)
{
  Display *xdisplay = GDK_DISPLAY_XDISPLAY (display);
  Atom type;
  gint format;
  gulong n_items, after, window = 0;
  guchar *data = NULL;

  gdk_x11_display_error_trap_push (display);
  if (XGetWindowProperty (xdisplay, DefaultRootWindow (xdisplay),
                          gdk_x11_get_xatom_by_name_for_display (display, "_NET_ACTIVE_WINDOW"),
                          0, 1, False, XA_WINDOW, &type, &format, &n_items, &after,
                          &data) == Success
      && data != NULL && format == 32 && n_items == 1)
    window = *(gulong *) data;
  if (data != NULL)
    XFree (data);
  gdk_x11_display_error_trap_pop_ignored (display);

  return window;
}



static gboolean
active_window_check (gpointer user_data)
{
  TrayPlugin *p = user_data;
  GdkDisplay *display;
  gulong window;

  p->active_check_id = 0;

  if (p->popup == NULL || !gtk_widget_get_visible (p->popup))
    return G_SOURCE_REMOVE;

  display = gtk_widget_get_display (p->popup);
  window = read_active_window (display);
  if (window == p->active_window)
    return G_SOURCE_REMOVE;
  p->active_window = window;

  /* Ninguna, o una ventana de este proceso (el panel, el diálogo) */
  if (window == 0 || gdk_x11_window_lookup_for_display (display, window) != NULL)
    return G_SOURCE_REMOVE;

  tray_popup_hide (p);
  return G_SOURCE_REMOVE;
}



static GdkFilterReturn
root_event_filter (GdkXEvent *gdk_xevent,
                   GdkEvent  *event,
                   gpointer   user_data)
{
  TrayPlugin *p = user_data;
  XEvent *xevent = gdk_xevent;

  if (xevent->type == PropertyNotify && p->active_check_id == 0
      && xevent->xproperty.atom
         == gdk_x11_get_xatom_by_name_for_display (gtk_widget_get_display (p->popup),
                                                   "_NET_ACTIVE_WINDOW"))
    p->active_check_id = g_idle_add (active_window_check, p);

  return GDK_FILTER_CONTINUE;
}
#endif



static void
active_watch_start (TrayPlugin *p)
{
#ifdef GDK_WINDOWING_X11
  GdkDisplay *display = gtk_widget_get_display (p->popup);
  GdkWindow *root;

  if (p->watching_active || !GDK_IS_X11_DISPLAY (display))
    return;

  root = gdk_screen_get_root_window (gtk_widget_get_screen (p->popup));
  gdk_window_set_events (root, gdk_window_get_events (root) | GDK_PROPERTY_CHANGE_MASK);
  gdk_window_add_filter (root, root_event_filter, p);
  p->active_window = read_active_window (display);
  p->watching_active = TRUE;
#endif
}



static void
active_watch_stop (TrayPlugin *p)
{
#ifdef GDK_WINDOWING_X11
  if (p->active_check_id != 0)
    {
      g_source_remove (p->active_check_id);
      p->active_check_id = 0;
    }

  if (p->watching_active)
    {
      gdk_window_remove_filter (gdk_screen_get_root_window (gtk_widget_get_screen (p->popup)),
                                root_event_filter, p);
      p->watching_active = FALSE;
    }
#endif
}



/* ── Wayland: foco ───────────────────────────────────────────────────────
 * Sin _NET_ACTIVE_WINDOW ni grabs fuera del proceso: como los popups del
 * propio panel, se cierra al perder el foco del teclado (otra app pasó a
 * primer plano, p. ej. tras un clic en su ícono, o se hizo clic en ella). */

#ifdef HAVE_GTK_LAYER_SHELL
/* ¿El puntero está sobre esta ventana nuestra? (En Wayland no hay
 * coordenadas de pantalla: se pregunta qué ventana tiene debajo.) */
static gboolean
pointer_over (TrayPlugin *p,
              GtkWidget  *toplevel)
{
  GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (p->popup));
  GdkDevice *pointer = seat != NULL ? gdk_seat_get_pointer (seat) : NULL;
  GdkWindow *window;

  if (pointer == NULL || !GTK_IS_WINDOW (toplevel))
    return FALSE;

  window = gdk_device_get_window_at_position (pointer, NULL, NULL);
  return window != NULL
         && gdk_window_get_effective_toplevel (window) == gtk_widget_get_window (toplevel);
}



static void
refocus_stop (TrayPlugin *p)
{
  if (p->refocus_hold)
    {
      p->refocus_hold = FALSE;
      gtk_layer_set_keyboard_mode (GTK_WINDOW (p->popup),
                                   GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    }
}



/* El puntero salió del popup: devolver el teclado (ver focus_check). Si
 * el compositor se lo da a otra superficie, el popup se cierra como
 * siempre, salvo que el puntero esté sobre el panel. */
static gboolean
popup_pointer_left (GtkWidget        *widget,
                    GdkEventCrossing *event,
                    TrayPlugin       *p)
{
  if (event->mode == GDK_CROSSING_NORMAL && event->detail != GDK_NOTIFY_INFERIOR)
    refocus_stop (p);
  return FALSE;
}



static gboolean
focus_check (gpointer user_data)
{
  TrayPlugin *p = user_data;
  gboolean reclaim = FALSE;

  p->focus_check_id = 0;

  if (p->popup == NULL || !gtk_widget_get_visible (p->popup)
      || p->menu_open || p->dragging
      || gtk_window_has_toplevel_focus (GTK_WINDOW (p->popup)))
    return G_SOURCE_REMOVE;

  /* El puntero sigue sobre el popup: el usuario no se fue (para eso hay
   * que hacer clic en otro lado). Algunos compositores (labwc 0.20) le dan
   * el foco a cada notificación que aparece, y se lo vuelven a dar
   * apenas el popup vuelve al modo normal. Recuperarlo pidiendo el
   * teclado en exclusiva mientras el puntero siga encima (ver
   * popup_pointer_left). Con un límite, por si algo insiste. */
  if (pointer_over (p, p->popup))
    {
      gint64 now = g_get_monotonic_time ();

      if (now - p->refocus_last > REFOCUS_WINDOW_MS * 1000)
        p->refocus_count = 0;
      p->refocus_last = now;
      reclaim = p->refocus_count++ < REFOCUS_MAX;
    }

  if (reclaim)
    {
      p->refocus_hold = TRUE;
      gtk_layer_set_keyboard_mode (GTK_WINDOW (p->popup),
                                   GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
      return G_SOURCE_REMOVE;
    }

  /* Clic sobre el panel: lo decide ese clic (p. ej. sobre la flecha
   * solo cierra; si cerráramos acá, el clic volvería a abrir) */
  if (pointer_over (p, gtk_widget_get_toplevel (GTK_WIDGET (p->panel_plugin))))
    return G_SOURCE_REMOVE;

  tray_popup_hide (p);
  return G_SOURCE_REMOVE;
}



static void
popup_focus_changed (GObject    *object,
                     GParamSpec *pspec,
                     TrayPlugin *p)
{
  if (!gtk_window_has_toplevel_focus (GTK_WINDOW (p->popup)) && p->focus_check_id == 0)
    p->focus_check_id = g_idle_add (focus_check, p);
}
#endif



void
tray_popup_grab (TrayPlugin *p)
{
  if (p->grab_retry_id != 0)
    g_source_remove (p->grab_retry_id);
  p->grab_retry_id = 0;
  p->grab_tries = 0;
  tray_popup_try_grab (p);
}



void
tray_popup_ungrab (TrayPlugin *p)
{
  if (p->grab_retry_id != 0)
    {
      g_source_remove (p->grab_retry_id);
      p->grab_retry_id = 0;
    }

  if (p->gtk_grabbed)
    {
      gtk_grab_remove (p->popup);
      p->gtk_grabbed = FALSE;
    }

  if (p->seat_grabbed)
    {
      if (!p->popup_layer)
        gdk_seat_ungrab (gdk_display_get_default_seat (gtk_widget_get_display (p->popup)));
      p->seat_grabbed = FALSE;
    }
}



/* Posición de la flecha en la pantalla y geometría de su monitor. Mismo
 * cálculo que xfce_panel_plugin_position_widget (en Wayland, márgenes de
 * la capa del panel), pero sin recortar al borde de la pantalla */
static gboolean
arrow_screen_rect (TrayPlugin   *p,
                   GdkRectangle *arrow,
                   GdkRectangle *monitor_geometry)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel (p->arrow);
  GdkWindow *window = gtk_widget_get_window (p->arrow);
  GdkMonitor *monitor;
  GtkAllocation alloc;
  gint x = 0, y = 0;

  if (window == NULL || !GTK_IS_WINDOW (toplevel))
    return FALSE;

  monitor = gdk_display_get_monitor_at_window (gtk_widget_get_display (p->arrow), window);
  if (monitor == NULL)
    return FALSE;
  gdk_monitor_get_geometry (monitor, monitor_geometry);

#ifdef HAVE_GTK_LAYER_SHELL
  if (p->popup_layer)
    {
      x = monitor_geometry->x + gtk_layer_get_margin (GTK_WINDOW (toplevel),
                                                      GTK_LAYER_SHELL_EDGE_LEFT);
      y = monitor_geometry->y + gtk_layer_get_margin (GTK_WINDOW (toplevel),
                                                      GTK_LAYER_SHELL_EDGE_TOP);
    }
  else
#endif
    gdk_window_get_origin (window, &x, &y);

  /* La flecha no tiene ventana propia: su asignación es relativa a la
   * ventana que la contiene */
  gtk_widget_get_allocation (p->arrow, &alloc);
  arrow->x = x + alloc.x;
  arrow->y = y + alloc.y;
  arrow->width = alloc.width;
  arrow->height = alloc.height;
  return TRUE;
}



void
tray_popup_reposition (TrayPlugin *p)
{
  GdkRectangle arrow, geometry;
  GtkRequisition size;
  gint x, y;

  if (p->popup == NULL)
    return;

  /* Encoger al tamaño natural antes de calcular la posición */
  gtk_window_resize (GTK_WINDOW (p->popup), 1, 1);
  xfce_panel_plugin_position_widget (p->panel_plugin, p->popup, p->arrow, &x, &y);

  /* Centrar el popup respecto de la flecha (el panel lo alinea por el
   * borde), sin salirse del monitor: cerca de una esquina queda pegado
   * al borde de la pantalla */
  gtk_widget_get_preferred_size (p->popup, &size, NULL);
  if (arrow_screen_rect (p, &arrow, &geometry))
    {
      GtkArrowType type = xfce_panel_plugin_arrow_type (p->panel_plugin);

      /* Popup arriba o abajo del panel: centrar en horizontal (también en
       * modo "deskbar", donde el panel es vertical pero el plugin no) */
      if (type == GTK_ARROW_UP || type == GTK_ARROW_DOWN)
        {
          x = arrow.x + (arrow.width - size.width) / 2;
          x = MIN (x, geometry.x + geometry.width - size.width);
          x = MAX (x, geometry.x);
        }
      else
        {
          y = arrow.y + (arrow.height - size.height) / 2;
          y = MIN (y, geometry.y + geometry.height - size.height);
          y = MAX (y, geometry.y);
        }
    }

  /* Pequeña separación respecto del panel */
  switch (xfce_panel_plugin_arrow_type (p->panel_plugin))
    {
    case GTK_ARROW_UP:    y -= POPUP_GAP; break;
    case GTK_ARROW_DOWN:  y += POPUP_GAP; break;
    case GTK_ARROW_LEFT:  x -= POPUP_GAP; break;
    case GTK_ARROW_RIGHT: x += POPUP_GAP; break;
    default: break;
    }

#ifdef HAVE_GTK_LAYER_SHELL
  if (p->popup_layer)
    {
      /* Wayland: sin coordenadas absolutas; la capa se ubica con márgenes
       * respecto del monitor del panel (como hace el propio panel) */
      GdkWindow *panel_window = gtk_widget_get_window (GTK_WIDGET (p->panel_plugin));
      GdkMonitor *monitor = panel_window != NULL
                            ? gdk_display_get_monitor_at_window (gtk_widget_get_display (p->popup),
                                                                 panel_window)
                            : NULL;
      GdkRectangle geometry = { 0, 0, 0, 0 };

      if (monitor != NULL)
        gdk_monitor_get_geometry (monitor, &geometry);

      gtk_layer_set_margin (GTK_WINDOW (p->popup), GTK_LAYER_SHELL_EDGE_LEFT, x - geometry.x);
      gtk_layer_set_margin (GTK_WINDOW (p->popup), GTK_LAYER_SHELL_EDGE_TOP, y - geometry.y);
      return;
    }
#endif

  gtk_window_move (GTK_WINDOW (p->popup), x, y);
}



void
tray_popup_show (TrayPlugin *p)
{
  if (p->popup == NULL)
    return;

  if (!gtk_widget_get_visible (p->popup))
    {
      GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (p->panel_plugin));

      /* Los grabs de GTK valen por grupo de ventanas: el popup debe
       * compartir grupo con la ventana del panel para que los clics
       * sobre el panel lleguen a él (y lo cierren) */
      if (GTK_IS_WINDOW (toplevel))
        {
          GtkWindowGroup *group = gtk_window_get_group (GTK_WINDOW (toplevel));

          if (gtk_window_get_group (GTK_WINDOW (p->popup)) != group)
            gtk_window_group_add_window (group, GTK_WINDOW (p->popup));
        }

      /* La distribución se mantiene al día sola; solo aplicar un cambio
       * que esté esperando su turno */
      if (p->relayout_id != 0 || !p->layout_valid)
        tray_plugin_relayout (p);
#ifdef HAVE_GTK_LAYER_SHELL
      if (p->popup_layer)
        {
          GdkWindow *panel_window = gtk_widget_get_window (GTK_WIDGET (p->panel_plugin));

          if (panel_window != NULL)
            gtk_layer_set_monitor (GTK_WINDOW (p->popup),
                                   gdk_display_get_monitor_at_window (
                                     gtk_widget_get_display (p->popup), panel_window));
        }
#endif
      tray_popup_reposition (p);
      gtk_widget_show (p->popup);
      active_watch_start (p);

      if (!p->autohide_blocked)
        {
          xfce_panel_plugin_block_autohide (p->panel_plugin, TRUE);
          p->autohide_blocked = TRUE;
        }
    }

  tray_plugin_arrow_set_active (p, TRUE);

  if (!p->dragging)
    tray_popup_grab (p);
}



void
tray_popup_hide (TrayPlugin *p)
{
  if (p->drag_open_id != 0)
    {
      g_source_remove (p->drag_open_id);
      p->drag_open_id = 0;
    }
  if (p->act_id != 0)
    {
      g_source_remove (p->act_id);
      p->act_id = 0;
    }
  p->hide_if_no_grab = FALSE;
  p->wait_app_menu = FALSE;
  kbd_select (p, NULL);
  if (p->focus_check_id != 0)
    {
      g_source_remove (p->focus_check_id);
      p->focus_check_id = 0;
    }
#ifdef HAVE_GTK_LAYER_SHELL
  p->refocus_count = 0;
  if (p->popup_layer)
    refocus_stop (p);
#endif

  if (p->popup != NULL)
    {
      active_watch_stop (p);
      tray_popup_ungrab (p);
      gtk_widget_hide (p->popup);
    }

  tray_plugin_arrow_set_active (p, FALSE);

  if (p->autohide_blocked)
    {
      xfce_panel_plugin_block_autohide (p->panel_plugin, FALSE);
      p->autohide_blocked = FALSE;
    }
}



static gboolean
suppress_toggle_expire (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->suppress_id = 0;
  p->suppress_toggle = FALSE;
  return G_SOURCE_REMOVE;
}



/* ¿El punto de pantalla (rx, ry) cae sobre @widget?
 * Con el grab activo, GTK entrega los clics con coordenadas relativas a
 * otras ventanas, así que solo las coordenadas de pantalla son fiables. */
static gboolean
widget_contains_root (GtkWidget *widget,
                      gdouble    rx,
                      gdouble    ry)
{
  GtkWidget *toplevel;
  GdkWindow *window;
  GtkAllocation alloc;
  gint ox, oy, wx, wy;

  if (widget == NULL || !gtk_widget_get_mapped (widget))
    return FALSE;

  toplevel = gtk_widget_get_toplevel (widget);
  window = gtk_widget_get_window (toplevel);
  if (window == NULL || !gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &wx, &wy))
    return FALSE;

  gdk_window_get_origin (window, &ox, &oy);
  gtk_widget_get_allocation (widget, &alloc);

  return rx >= ox + wx && ry >= oy + wy
         && rx < ox + wx + alloc.width && ry < oy + wy + alloc.height;
}



/* Wayland: ¿el puntero está sobre @widget? No hay coordenadas de
 * pantalla; se mira qué ventana propia está bajo el puntero. */
static gboolean
pointer_on_widget (GtkWidget *widget,
                   GdkDevice *device)
{
  GdkWindow *window;
  gpointer data = NULL;

  if (widget == NULL || device == NULL || !gtk_widget_get_mapped (widget))
    return FALSE;

  window = gdk_device_get_window_at_position (device, NULL, NULL);
  if (window == NULL)
    return FALSE;

  if (GTK_IS_WINDOW (widget))
    return gdk_window_get_effective_toplevel (window) == gtk_widget_get_window (widget);

  gdk_window_get_user_data (window, &data);
  return data == widget || (GTK_IS_WIDGET (data) && gtk_widget_is_ancestor (data, widget));
}



/* Reenvía un clic a la ventana de este proceso que está bajo el puntero
 * (otro ícono del panel, otro plugin...). Debe llamarse ya sin grab. */
static void
redispatch_press (TrayPlugin     *p,
                  GdkEventButton *event)
{
  GdkDevice *device = gdk_event_get_device ((GdkEvent *) event);
  GdkWindow *target;
  gpointer user_data = NULL;
  GdkEvent *copy;
  gint wx = 0, wy = 0;

  if (device == NULL)
    return;

  target = gdk_device_get_window_at_position (device, &wx, &wy);
  if (target == NULL || target == gtk_widget_get_window (p->popup))
    return;

  gdk_window_get_user_data (target, &user_data);
  if (!GTK_IS_WIDGET (user_data))
    return;

  copy = gdk_event_copy ((GdkEvent *) event);
  g_object_unref (copy->button.window);
  copy->button.window = g_object_ref (target);
  copy->button.x = wx;
  copy->button.y = wy;
  gtk_main_do_event (copy);
  gdk_event_free (copy);
}



static gboolean
popup_button_press (GtkWidget      *widget,
                    GdkEventButton *event,
                    TrayPlugin     *p)
{
  gboolean on_arrow;

  if (p->popup_layer)
    {
      GdkDevice *device = gdk_event_get_device ((GdkEvent *) event);

      if (pointer_on_widget (p->popup, device))
        return FALSE;
      on_arrow = pointer_on_widget (p->arrow, device);
    }
  else
    {
      if (widget_contains_root (p->popup, event->x_root, event->y_root))
        return FALSE;
      on_arrow = widget_contains_root (p->arrow, event->x_root, event->y_root);
    }

  tray_popup_hide (p);

  if (on_arrow)
    {
      /* El "soltar" de este clic llegará a la flecha y la volvería a
       * activar: lo ignoramos, este clic solo cierra */
      p->suppress_toggle = TRUE;
      if (p->suppress_id != 0)
        g_source_remove (p->suppress_id);
      p->suppress_id = g_timeout_add (400, suppress_toggle_expire, p);
    }
  else
    {
      /* Clic sobre otra parte del panel (otro ícono, otro plugin):
       * además de cerrar, que llegue a su destino, como en Windows */
      redispatch_press (p, event);
    }

  return TRUE;
}



/* ── Teclado ─────────────────────────────────────────────────────────────
 * El ícono elegido con las flechas se marca con el mismo resaltado que al
 * pasar el ratón (PRELIGHT). Los botones no toman el foco de GTK: así un
 * clic no deja un recuadro de foco ni le quita el foco a nadie. */

static void
kbd_select (TrayPlugin *p,
            GtkWidget  *button)
{
  if (p->kbd_button == button)
    return;

  if (p->kbd_button != NULL)
    {
      gtk_widget_unset_state_flags (p->kbd_button, GTK_STATE_FLAG_PRELIGHT);
      g_clear_weak_pointer (&p->kbd_button);
    }
  if (button != NULL)
    {
      g_set_weak_pointer (&p->kbd_button, button);
      gtk_widget_set_state_flags (button, GTK_STATE_FLAG_PRELIGHT, FALSE);
    }
}



void
tray_popup_clear_selection (TrayPlugin *p)
{
  kbd_select (p, NULL);
}



/* El ratón pasa por un ícono: deja de valer la elección del teclado */
static gboolean
button_entered (GtkWidget        *widget,
                GdkEventCrossing *event,
                TrayPlugin       *p)
{
  if (p->kbd_button != NULL && p->kbd_button != widget)
    kbd_select (p, NULL);
  return FALSE;
}



/* Posición del ícono elegido después de la tecla @keyval, o -1 si la
 * tecla no es de movimiento. Cuadrícula por filas de "columns" íconos. */
static gint
kbd_move (TrayPlugin *p,
          guint       keyval,
          gint        current)
{
  gint n = (gint) p->popup_order->len;
  gint cols = MAX (p->layout_params.columns, 1);

  if (current < 0)   /* nada elegido: cualquier movimiento elige el primero */
    {
      switch (keyval)
        {
        case GDK_KEY_End: case GDK_KEY_KP_End:
          return n - 1;
        case GDK_KEY_Left: case GDK_KEY_Right: case GDK_KEY_Up: case GDK_KEY_Down:
        case GDK_KEY_KP_Left: case GDK_KEY_KP_Right: case GDK_KEY_KP_Up: case GDK_KEY_KP_Down:
        case GDK_KEY_Home: case GDK_KEY_KP_Home: case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab:
          return 0;
        default:
          return -1;
        }
    }

  switch (keyval)
    {
    case GDK_KEY_Left: case GDK_KEY_KP_Left:
      return MAX (current - 1, 0);
    case GDK_KEY_Right: case GDK_KEY_KP_Right:
      return MIN (current + 1, n - 1);
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
      return current - cols >= 0 ? current - cols : current;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
      if (current + cols < n)
        return current + cols;
      /* Última fila incompleta: ir al último ícono */
      return current / cols < (n - 1) / cols ? n - 1 : current;
    case GDK_KEY_Home: case GDK_KEY_KP_Home:
      return 0;
    case GDK_KEY_End: case GDK_KEY_KP_End:
      return n - 1;
    case GDK_KEY_Tab:
      return (current + 1) % n;
    case GDK_KEY_ISO_Left_Tab:
      return (current + n - 1) % n;
    default:
      return -1;
    }
}



static gboolean
popup_key_press (GtkWidget   *widget,
                 GdkEventKey *event,
                 TrayPlugin  *p)
{
  guint current_index = 0;
  gint current = -1, next;
  gboolean menu;

  if (event->keyval == GDK_KEY_Escape)
    {
      tray_popup_hide (p);
      return TRUE;
    }

  if (p->popup_order->len == 0 || p->menu_open)
    return FALSE;

  if (p->kbd_button != NULL
      && g_ptr_array_find (p->popup_order, p->kbd_button, &current_index))
    current = (gint) current_index;

  /* Enter / espacio: como un clic; tecla de menú o Mayús+F10: como el
   * clic derecho */
  menu = event->keyval == GDK_KEY_Menu
         || (event->keyval == GDK_KEY_F10 && (event->state & GDK_SHIFT_MASK));
  if (menu || event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter
      || event->keyval == GDK_KEY_ISO_Enter || event->keyval == GDK_KEY_space
      || event->keyval == GDK_KEY_KP_Space)
    {
      if (current < 0)
        return TRUE;
      tray_button_activate_from_keyboard (TRAY_BUTTON (p->kbd_button), menu);
      return TRUE;
    }

  next = kbd_move (p, event->keyval, current);
  if (next < 0)
    return FALSE;

  kbd_select (p, g_ptr_array_index (p->popup_order, next));
  return TRUE;
}



/* Abrir o cerrar desde un atajo de teclado (ver plugin_remote_event).
 * Al abrir, se elige el primer ícono para seguir con las flechas.
 * Devuelve FALSE si no hay nada que mostrar (sin íconos ocultos). */
gboolean
tray_popup_toggle_from_keyboard (TrayPlugin *p,
                                 gboolean    open)
{
  if (p->popup == NULL)
    return FALSE;

  if (!open)
    {
      if (gtk_widget_get_visible (p->popup))
        tray_popup_hide (p);
      return TRUE;
    }

  if (p->popup_order->len == 0)
    return FALSE;

  if (!gtk_widget_get_visible (p->popup))
    tray_popup_show (p);
  kbd_select (p, g_ptr_array_index (p->popup_order, 0));
  return TRUE;
}



static gboolean
popup_grab_check (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->grab_check_id = 0;

  /* Si no fue por un menú propio ni por un arrastre, otra app tomó el
   * control: cerramos como lo haría un menú */
  if (!p->dragging && !p->menu_open && p->popup != NULL
      && gtk_widget_get_visible (p->popup) && !p->seat_grabbed
      && p->grab_retry_id == 0)
    tray_popup_hide (p);

  return G_SOURCE_REMOVE;
}



static gboolean
popup_grab_broken (GtkWidget          *widget,
                   GdkEventGrabBroken *event,
                   TrayPlugin         *p)
{
  if (event->implicit)
    return FALSE;

  p->seat_grabbed = FALSE;
  if (p->gtk_grabbed)
    {
      gtk_grab_remove (p->popup);
      p->gtk_grabbed = FALSE;
    }

  /* Esperar: el inicio de un arrastre rompe el grab antes de que nos
   * llegue "drag-begin" */
  if (p->grab_check_id == 0)
    p->grab_check_id = g_timeout_add (100, popup_grab_check, p);

  return FALSE;
}



static gboolean
popup_draw (GtkWidget  *widget,
            cairo_t    *cr,
            TrayPlugin *p)
{
  if (p->popup_rgba)
    {
      /* Limpiar a transparente para que se vean las esquinas redondeadas.
       * Sin compositor el radio es 0 y el marco tapa toda la ventana */
      cairo_save (cr);
      cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba (cr, 0, 0, 0, 0);
      cairo_paint (cr);
      cairo_restore (cr);
    }
  return FALSE;
}



/* ════════════════════════════════════════════════════════════════════════
 * Menús abiertos desde el popup
 * ════════════════════════════════════════════════════════════════════════ */

/* Mientras un menú abierto desde el popup está visible, cualquier
 * "activate" de un GtkMenuItem pasa por acá. Así también cuentan los
 * ítems que la app agrega después de abrirse el menú (DBusMenu tras
 * AboutToShow). "selection-done" no sirve: llega también al descartar. */
static gboolean
menu_activate_hook (GSignalInvocationHint *hint,
                    guint                  n_values,
                    const GValue          *values,
                    gpointer               user_data)
{
  TrayPlugin *p = user_data;
  GtkWidget *item = g_value_get_object (&values[0]);

  /* Los ítems con submenú solo lo abren: no son una elección */
  if (p->menu_shown == NULL || !GTK_IS_MENU_ITEM (item)
      || gtk_menu_item_get_submenu (GTK_MENU_ITEM (item)) != NULL)
    return TRUE;

  /* ¿Pertenece a nuestro menú o a uno de sus submenús? */
  while (item != NULL)
    {
      GtkWidget *menu = gtk_widget_get_parent (item);

      if (!GTK_IS_MENU (menu))
        break;
      if (menu == p->menu_shown)
        {
          p->menu_chosen = TRUE;
          break;
        }
      item = gtk_menu_get_attach_widget (GTK_MENU (menu));
    }

  return TRUE;   /* seguir enganchado hasta que se cierre el menú */
}



void
tray_popup_menu_hook_remove (TrayPlugin *p)
{
  if (p->menu_hook_id != 0)
    {
      g_signal_remove_emission_hook (g_signal_lookup ("activate", GTK_TYPE_MENU_ITEM),
                                     p->menu_hook_id);
      p->menu_hook_id = 0;
    }
}



static gboolean
menu_closed_idle (gpointer user_data)
{
  TrayPlugin *p = user_data;

  tray_popup_menu_hook_remove (p);

  if (p->menu_shown != NULL)
    {
      g_clear_signal_handler (&p->menu_deactivate_id, p->menu_shown);
      g_clear_object (&p->menu_shown);
    }

  p->menu_open = FALSE;

  if (p->popup != NULL && gtk_widget_get_visible (p->popup))
    {
      if (p->menu_chosen)
        tray_popup_hide (p);
      else
        tray_popup_grab (p);   /* menú descartado: el popup sigue abierto */
    }

  return G_SOURCE_REMOVE;
}



static void
menu_deactivated (GtkMenuShell *menu,
                  TrayPlugin   *p)
{
  /* GTK emite "deactivate" antes que el "activate" del ítem elegido:
   * decidimos en un idle, cuando ya se sabe si se eligió algo */
  g_idle_add (menu_closed_idle, p);
}



static void
restore_tooltip (GtkMenuShell *menu,
                 GtkWidget    *button)
{
  g_signal_handlers_disconnect_by_func (menu, restore_tooltip, button);
  gtk_widget_set_has_tooltip (button, TRUE);
}



static void
button_show_menu (TrayButton *button,
                  GtkWidget  *menu,
                  GdkEvent   *event,
                  TrayPlugin *p)
{
  /* Que el tooltip del ícono no tape el menú */
  gtk_widget_set_has_tooltip (GTK_WIDGET (button), FALSE);
  gtk_tooltip_trigger_tooltip_query (gtk_widget_get_display (GTK_WIDGET (button)));
  g_signal_connect_object (menu, "deactivate", G_CALLBACK (restore_tooltip), button, 0);

  if (!tray_plugin_in_popup (p, GTK_WIDGET (button)))
    {
      xfce_panel_plugin_popup_menu (p->panel_plugin, GTK_MENU (menu),
                                    GTK_WIDGET (button), event);
      return;
    }

  if (p->menu_open)
    return;

  /* Soltamos nuestro grab para que el menú tome el suyo */
  tray_popup_ungrab (p);

  p->menu_open = TRUE;
  p->menu_chosen = FALSE;
  p->menu_shown = g_object_ref (menu);
  p->menu_deactivate_id = g_signal_connect (menu, "deactivate",
                                            G_CALLBACK (menu_deactivated), p);
  tray_popup_menu_hook_remove (p);
  p->menu_hook_id = g_signal_add_emission_hook (g_signal_lookup ("activate", GTK_TYPE_MENU_ITEM),
                                                0, menu_activate_hook, p, NULL);

  if (event != NULL)
    gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
  else
    gtk_menu_popup_at_widget (GTK_MENU (menu), GTK_WIDGET (button),
                              GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}



static gboolean
act_regrab (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->act_id = 0;

  /* La app no abrió ninguna ventana (si no, ya se habría cerrado):
   * el popup sigue abierto y recupera el grab. Si no puede (la app
   * tomó el control con un menú propio), se cierra. */
  p->hide_if_no_grab = TRUE;
  tray_popup_grab (p);
  return G_SOURCE_REMOVE;
}



static void
button_acted (TrayButton *button,
              gboolean    app_menu,
              TrayPlugin *p)
{
  gboolean legacy;

  if (!tray_plugin_in_popup (p, GTK_WIDGET (button)))
    return;

  legacy = tray_item_get_is_legacy (tray_button_get_item (button));

  /* La app va a mostrar su propio menú: el popup estorbaría. En X11, con
   * una app antigua se puede saber cuándo se cierra ese menú (devuelve el
   * puntero): el popup queda abierto, como en Windows (ver tray_popup_try_grab) */
  if (app_menu && !(legacy && !p->popup_layer))
    {
      tray_popup_hide (p);
      return;
    }

  /* Wayland: nuestro grab no afecta a otras apps; si la app pasa a primer
   * plano, el popup pierde el foco y se cierra (ver focus_check).
   * Las apps antiguas (XWayland) no le quitan el foco al mostrar su menú
   * y el popup lo taparía: con ellas, el clic siempre cierra. */
  if (p->popup_layer)
    {
      if (legacy)
        tray_popup_hide (p);
      return;
    }

  /* Activar: el popup queda abierto. Se cierra solo si la app pasa a
   * primer plano (ver active_window_check). Mientras tanto, sin grab:
   * con él, el clic simulado de una app antigua llegaría al popup. */
  p->wait_app_menu = legacy;
  tray_popup_ungrab (p);
  if (p->act_id != 0)
    g_source_remove (p->act_id);
  p->act_id = g_timeout_add (app_menu ? MENU_REGRAB_MS : ACT_REGRAB_MS, act_regrab, p);
}



static void
button_scrolled (TrayButton *button,
                 TrayPlugin *p)
{
  if (!tray_plugin_in_popup (p, GTK_WIDGET (button))
      || !tray_item_get_is_legacy (tray_button_get_item (button)))
    return;

  /* Wayland: no hay grab del puntero (ver tray_button_scroll) */
  if (p->popup_layer)
    return;

  /* X11: con el grab del popup, el giro simulado del proxy le llegaría al
   * popup y no a la app antigua. Se suelta un momento, como con los clics. */

  p->wait_app_menu = TRUE;
  tray_popup_ungrab (p);
  if (p->act_id != 0)
    g_source_remove (p->act_id);
  p->act_id = g_timeout_add (ACT_REGRAB_MS, act_regrab, p);
}






/* ════════════════════════════════════════════════════════════════════════
 * Popup: esquinas
 * ════════════════════════════════════════════════════════════════════════ */

static gint
style_radius (GtkStyleContext *context)
{
  gint radius = 0;

  gtk_style_context_get (context, gtk_style_context_get_state (context),
                         GTK_STYLE_PROPERTY_BORDER_RADIUS, &radius, NULL);
  return radius;
}



/* El radio que el tema usa en sus menús: el del menú en sí o el de la
 * decoración de las ventanas emergentes (Adwaita redondea esta última) */
static gint
theme_menu_radius (GdkScreen *screen)
{
  GtkWidget *menu = gtk_menu_new ();
  GtkWidgetPath *path = gtk_widget_path_new ();
  GtkStyleContext *context = gtk_style_context_new ();
  gint radius;

  g_object_ref_sink (menu);
  radius = style_radius (gtk_widget_get_style_context (menu));
  gtk_widget_destroy (menu);
  g_object_unref (menu);

  gtk_widget_path_append_type (path, GTK_TYPE_WINDOW);
  gtk_widget_path_iter_set_object_name (path, -1, "window");
  gtk_widget_path_iter_add_class (path, -1, "csd");
  gtk_widget_path_iter_add_class (path, -1, "popup");
  gtk_widget_path_append_type (path, G_TYPE_NONE);
  gtk_widget_path_iter_set_object_name (path, -1, "decoration");
  gtk_style_context_set_screen (context, screen);
  gtk_style_context_set_path (context, path);
  radius = MAX (radius, style_radius (context));
  g_object_unref (context);
  gtk_widget_path_unref (path);

  return CLAMP (radius, 0, POPUP_RADIUS_MAX);
}



/* Aplica la opción de esquinas. Sin compositor (X11) no hay
 * transparencia: siempre rectas */
void
tray_popup_apply_corners (TrayPlugin *p)
{
  GdkScreen *screen;
  gint radius = 0;
  gchar *css;

  if (p->popup == NULL)
    return;

  screen = gtk_widget_get_screen (p->popup);
  if (p->popup_rgba && gdk_screen_is_composited (screen))
    {
      switch (tray_settings_get_popup_corners (p->settings))
        {
        case TRAY_CORNERS_THEME:
          radius = theme_menu_radius (screen);
          break;
        case TRAY_CORNERS_ROUNDED:
          radius = POPUP_RADIUS_ROUNDED;
          break;
        default:
          break;
        }
    }

  p->popup_rounded = radius > 0;
  if (p->popup_rounded)
    gtk_style_context_add_class (gtk_widget_get_style_context (p->popup_frame), "rounded");
  else
    gtk_style_context_remove_class (gtk_widget_get_style_context (p->popup_frame), "rounded");

  if (radius == p->popup_radius)
    return;

  p->popup_radius = radius;
  css = g_strdup_printf ("." TO_CSS_PREFIX "-popup-frame { border-radius: %dpx; }", radius);
  gtk_css_provider_load_from_data (p->corners_css, css, -1, NULL);
  g_free (css);
  gtk_widget_queue_draw (p->popup);
}



static gboolean
popup_apply_corners_idle (gpointer data)
{
  TrayPlugin *p = data;

  p->corners_idle_id = 0;
  tray_popup_apply_corners (p);
  return G_SOURCE_REMOVE;
}



/* Cambió el tema o el compositor: esperar a que GTK cargue el estilo nuevo */
static void
popup_schedule_corners (TrayPlugin *p)
{
  if (p->corners_idle_id == 0)
    p->corners_idle_id = g_idle_add (popup_apply_corners_idle, p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Popup: construcción
 * ════════════════════════════════════════════════════════════════════════ */

void
tray_popup_create (TrayPlugin *p)
{
  GdkScreen *screen = gtk_widget_get_screen (GTK_WIDGET (p->panel_plugin));
  GdkVisual *visual = gdk_screen_get_rgba_visual (screen);
  GtkStyleContext *context;

#ifdef HAVE_GTK_LAYER_SHELL
  /* Wayland: capa sobre todo lo demás, como los popups del propio panel */
  p->popup_layer = gtk_layer_is_supported ();
#endif

  /* X11: ventana tipo POPUP (override-redirect): posición exacta y sin
   * decoraciones; el cierre por clic externo lo manejamos con un grab */
  p->popup = gtk_window_new (p->popup_layer ? GTK_WINDOW_TOPLEVEL : GTK_WINDOW_POPUP);
  gtk_window_set_type_hint (GTK_WINDOW (p->popup), GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU);
  gtk_window_set_screen (GTK_WINDOW (p->popup), screen);
  gtk_window_set_resizable (GTK_WINDOW (p->popup), FALSE);
  gtk_widget_add_events (p->popup, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK
                         | GDK_LEAVE_NOTIFY_MASK);

#ifdef HAVE_GTK_LAYER_SHELL
  if (p->popup_layer)
    {
      GtkWindow *window = GTK_WINDOW (p->popup);

      gtk_window_set_decorated (window, FALSE);
      gtk_layer_init_for_window (window);
      gtk_layer_set_namespace (window, "xfce4-panel");
      gtk_layer_set_layer (window, GTK_LAYER_SHELL_LAYER_OVERLAY);
      gtk_layer_set_exclusive_zone (window, -1);
      gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
      gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
      /* Con foco: Esc y el cierre al pasar a otra app */
      gtk_layer_set_keyboard_mode (window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
      g_signal_connect (p->popup, "notify::has-toplevel-focus",
                        G_CALLBACK (popup_focus_changed), p);
      g_signal_connect (p->popup, "leave-notify-event",
                        G_CALLBACK (popup_pointer_left), p);
    }
#endif

  context = gtk_widget_get_style_context (p->popup);
  gtk_style_context_add_class (context, TO_CSS_PREFIX "-popup");

  /* Visual con transparencia siempre que exista, aunque ahora no haya
   * compositor: así las esquinas pueden cambiar si se prende después */
  p->popup_rgba = visual != NULL;
  if (p->popup_rgba)
    {
      gtk_widget_set_visual (p->popup, visual);
      gtk_widget_set_app_paintable (p->popup, TRUE);
      gtk_style_context_add_class (context, "rounded");
    }

  p->popup_frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  context = gtk_widget_get_style_context (p->popup_frame);
  gtk_style_context_add_class (context, TO_CSS_PREFIX "-popup-frame");
  /* Radio de las esquinas: proveedor propio del marco, por encima del CSS
   * general del plugin y por debajo del gtk.css del usuario */
  p->corners_css = gtk_css_provider_new ();
  gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (p->corners_css),
                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  p->popup_radius = -1;
  gtk_container_add (GTK_CONTAINER (p->popup), p->popup_frame);
  gtk_widget_show (p->popup_frame);

  p->popup_grid = gtk_grid_new ();
  gtk_grid_set_row_homogeneous (GTK_GRID (p->popup_grid), TRUE);
  gtk_grid_set_column_homogeneous (GTK_GRID (p->popup_grid), TRUE);
  gtk_box_pack_start (GTK_BOX (p->popup_frame), p->popup_grid, TRUE, TRUE, 0);

  p->popup_empty = gtk_label_new (_("Drop icons here to hide them"));
  gtk_style_context_add_class (gtk_widget_get_style_context (p->popup_empty),
                               TO_CSS_PREFIX "-empty");
  gtk_box_pack_start (GTK_BOX (p->popup_frame), p->popup_empty, TRUE, TRUE, 0);

  tray_dnd_setup_drop_target (p, p->popup_frame);

  g_signal_connect (p->popup, "button-press-event", G_CALLBACK (popup_button_press), p);
  g_signal_connect (p->popup, "key-press-event", G_CALLBACK (popup_key_press), p);
  g_signal_connect (p->popup, "grab-broken-event", G_CALLBACK (popup_grab_broken), p);
  g_signal_connect (p->popup, "draw", G_CALLBACK (popup_draw), p);

  tray_popup_apply_corners (p);
  p->composited_id = g_signal_connect_swapped (screen, "composited-changed",
                                               G_CALLBACK (popup_schedule_corners), p);
  p->theme_name_id = g_signal_connect_swapped (gtk_settings_get_for_screen (screen),
                                               "notify::gtk-theme-name",
                                               G_CALLBACK (popup_schedule_corners), p);
}






/* Señales de un botón nuevo que tienen que ver con el popup */
void
tray_popup_setup_button (TrayPlugin *p,
                         GtkWidget  *button)
{
  g_signal_connect (button, "show-menu", G_CALLBACK (button_show_menu), p);
  g_signal_connect (button, "acted", G_CALLBACK (button_acted), p);
  g_signal_connect (button, "scrolled", G_CALLBACK (button_scrolled), p);
  g_signal_connect (button, "enter-notify-event", G_CALLBACK (button_entered), p);
  tray_button_set_panel_anchor (TRAY_BUTTON (button), GTK_WIDGET (p->panel_plugin));
}
