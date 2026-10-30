/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vista del plugin:
 *
 *   [▲][ícono][ícono][ícono]      ← panel (visibles)
 *    │
 *    └─► ┌──────────────┐
 *        │ ▣  ▣  ▣  ▣   │         ← popup (ocultos), cuadrícula
 *        │ ▣  ▣         │
 *        └──────────────┘
 *
 * Un mismo TrayButton se mueve entre el panel y el popup según la
 * configuración; nunca se recrea al ocultarlo o mostrarlo.
 */
#include "to-common.h"
#include "to-plugin.h"
#include "to-button.h"
#include "to-dialog.h"
#include "to-legacy.h"

#include <libxfce4util/libxfce4util.h>

#include <stdio.h>
#include <string.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#endif

#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

#define DND_TARGET     "application/x-" TO_CSS_PREFIX "-item"
#define DRAG_OPEN_MS   450
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

static const GtkTargetEntry dnd_targets[] = {
  { (gchar *) DND_TARGET, GTK_TARGET_SAME_APP, 0 },
};

static const gchar plugin_css[] =
  "." TO_CSS_PREFIX "-button {"
  "  padding: 1px; margin: 0; min-width: 0; min-height: 0; border-radius: 4px;"
  "}"
  "." TO_CSS_PREFIX "-arrow {"
  "  padding: 0 1px; margin: 0; min-width: 0; min-height: 0;"
  "}"
  "." TO_CSS_PREFIX "-popup {"
  "  background-color: @theme_bg_color;"
  "}"
  "." TO_CSS_PREFIX "-popup.rounded {"
  "  background-color: transparent;"
  "}"
  "." TO_CSS_PREFIX "-popup-frame {"
  "  background-color: @theme_bg_color;"
  "  border: 1px solid alpha(@theme_fg_color, 0.18);"
  "  padding: 6px;"
  "}"
  "." TO_CSS_PREFIX "-popup ." TO_CSS_PREFIX "-button {"
  "  padding: 4px;"
  "}"
  "." TO_CSS_PREFIX "-empty {"
  "  opacity: 0.7; padding: 8px 12px;"
  "}";

static void tray_plugin_schedule_relayout (TrayPlugin *p);
static void tray_plugin_relayout          (TrayPlugin *p);
static void popup_show                    (TrayPlugin *p);
static void popup_hide                    (TrayPlugin *p);
static void popup_grab                    (TrayPlugin *p);



/* ════════════════════════════════════════════════════════════════════════
 * Utilidades
 * ════════════════════════════════════════════════════════════════════════ */

static gboolean
is_horizontal (TrayPlugin *p)
{
  return xfce_panel_plugin_get_mode (p->panel_plugin) == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL;
}



static gboolean
in_popup (TrayPlugin *p, GtkWidget *widget)
{
  return p->popup != NULL && gtk_widget_get_toplevel (widget) == p->popup;
}



gint
tray_plugin_get_panel_icon_size (TrayPlugin *p)
{
  gint size = tray_settings_get_panel_icon_size (p->settings);

  if (size > 0)
    return size;

  /* Automático: el mismo criterio que el resto de plugins del panel
   * (respeta el "tamaño de ícono" del panel y el número de filas) */
  return MAX (TRAY_ICON_SIZE_MIN, xfce_panel_plugin_get_icon_size (p->panel_plugin));
}



static gboolean
item_is_hidden (TrayPlugin *p, TrayItem *item)
{
  return tray_settings_is_hidden (p->settings, tray_item_get_key (item),
                                  tray_item_get_status (item) == TRAY_ITEM_STATUS_PASSIVE);
}



/* ════════════════════════════════════════════════════════════════════════
 * Flecha
 * ════════════════════════════════════════════════════════════════════════ */

static void
arrow_update_icon (TrayPlugin *p)
{
  const gchar *icon;

  switch (xfce_panel_plugin_arrow_type (p->panel_plugin))
    {
    case GTK_ARROW_DOWN:  icon = "pan-down-symbolic";  break;
    case GTK_ARROW_LEFT:  icon = "pan-start-symbolic"; break;
    case GTK_ARROW_RIGHT: icon = "pan-end-symbolic";   break;
    case GTK_ARROW_UP:
    default:              icon = "pan-up-symbolic";    break;
    }

  gtk_image_set_from_icon_name (GTK_IMAGE (p->arrow_image), icon, GTK_ICON_SIZE_MENU);
  gtk_image_set_pixel_size (GTK_IMAGE (p->arrow_image), 16);
}



/* Punto de "requiere atención" sobre la flecha */
static gboolean
arrow_dot_draw (GtkWidget *widget,
                cairo_t   *cr,
                gpointer   user_data)
{
  GtkStyleContext *context = gtk_widget_get_style_context (widget);
  GdkRGBA color;
  gint w = gtk_widget_get_allocated_width (widget);
  gint h = gtk_widget_get_allocated_height (widget);

  if (!gtk_style_context_lookup_color (context, "theme_selected_bg_color", &color))
    gdk_rgba_parse (&color, "#e95420");

  gdk_cairo_set_source_rgba (cr, &color);
  cairo_arc (cr, w / 2.0, h / 2.0, MIN (w, h) / 2.0, 0, 2 * G_PI);
  cairo_fill (cr);

  return TRUE;
}



static void
arrow_set_active (TrayPlugin *p, gboolean active)
{
  p->updating_arrow = TRUE;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (p->arrow), active);
  p->updating_arrow = FALSE;

  /* Con el popup abierto, el tooltip de la flecha solo estorba */
  gtk_widget_set_has_tooltip (p->arrow, !active);
  if (active)
    gtk_tooltip_trigger_tooltip_query (gtk_widget_get_display (p->arrow));
}



static void
arrow_toggled (GtkToggleButton *button,
               TrayPlugin      *p)
{
  if (p->updating_arrow)
    return;

  if (p->suppress_toggle)
    {
      p->suppress_toggle = FALSE;
      arrow_set_active (p, FALSE);
      return;
    }

  if (gtk_toggle_button_get_active (button))
    popup_show (p);
  else
    popup_hide (p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Popup: grab y cierre
 * ════════════════════════════════════════════════════════════════════════ */

static gboolean
popup_try_grab (gpointer user_data)
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
      p->grab_retry_id = g_timeout_add (50, popup_try_grab, p);
    }
  else if (p->hide_if_no_grab && p->wait_app_menu)
    {
      /* Una app antigua mostró su menú y tiene el puntero: el popup sigue
       * abierto y lo recupera cuando el menú se cierre */
      p->grab_retry_id = g_timeout_add (MENU_WAIT_MS, popup_try_grab, p);
    }
  else if (p->hide_if_no_grab)
    {
      /* Tras un clic, la app tomó el control (p. ej. abrió un menú o un
       * control propio): sin grab el popup no se cerraría solo */
      popup_hide (p);
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

  popup_hide (p);
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

  popup_hide (p);
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



static void
popup_grab (TrayPlugin *p)
{
  if (p->grab_retry_id != 0)
    g_source_remove (p->grab_retry_id);
  p->grab_retry_id = 0;
  p->grab_tries = 0;
  popup_try_grab (p);
}



static void
popup_ungrab (TrayPlugin *p)
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



static void
popup_reposition (TrayPlugin *p)
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



static void
popup_show (TrayPlugin *p)
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
      popup_reposition (p);
      gtk_widget_show (p->popup);
      active_watch_start (p);

      if (!p->autohide_blocked)
        {
          xfce_panel_plugin_block_autohide (p->panel_plugin, TRUE);
          p->autohide_blocked = TRUE;
        }
    }

  arrow_set_active (p, TRUE);

  if (!p->dragging)
    popup_grab (p);
}



static void
popup_hide (TrayPlugin *p)
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
      popup_ungrab (p);
      gtk_widget_hide (p->popup);
    }

  arrow_set_active (p, FALSE);

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

  popup_hide (p);

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



static gboolean
popup_key_press (GtkWidget   *widget,
                 GdkEventKey *event,
                 TrayPlugin  *p)
{
  if (event->keyval == GDK_KEY_Escape)
    {
      popup_hide (p);
      return TRUE;
    }
  return FALSE;
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
    popup_hide (p);

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



static void
menu_hook_remove (TrayPlugin *p)
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

  menu_hook_remove (p);

  if (p->menu_shown != NULL)
    {
      g_clear_signal_handler (&p->menu_deactivate_id, p->menu_shown);
      g_clear_object (&p->menu_shown);
    }

  p->menu_open = FALSE;

  if (p->popup != NULL && gtk_widget_get_visible (p->popup))
    {
      if (p->menu_chosen)
        popup_hide (p);
      else
        popup_grab (p);   /* menú descartado: el popup sigue abierto */
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

  if (!in_popup (p, GTK_WIDGET (button)))
    {
      xfce_panel_plugin_popup_menu (p->panel_plugin, GTK_MENU (menu),
                                    GTK_WIDGET (button), event);
      return;
    }

  if (p->menu_open)
    return;

  /* Soltamos nuestro grab para que el menú tome el suyo */
  popup_ungrab (p);

  p->menu_open = TRUE;
  p->menu_chosen = FALSE;
  p->menu_shown = g_object_ref (menu);
  p->menu_deactivate_id = g_signal_connect (menu, "deactivate",
                                            G_CALLBACK (menu_deactivated), p);
  menu_hook_remove (p);
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
  popup_grab (p);
  return G_SOURCE_REMOVE;
}



static void
button_acted (TrayButton *button,
              gboolean    app_menu,
              TrayPlugin *p)
{
  gboolean legacy;

  if (!in_popup (p, GTK_WIDGET (button)))
    return;

  legacy = tray_item_get_is_legacy (tray_button_get_item (button));

  /* La app va a mostrar su propio menú: el popup estorbaría. En X11, con
   * una app antigua se puede saber cuándo se cierra ese menú (devuelve el
   * puntero): el popup queda abierto, como en Windows (ver popup_try_grab) */
  if (app_menu && !(legacy && !p->popup_layer))
    {
      popup_hide (p);
      return;
    }

  /* Wayland: nuestro grab no afecta a otras apps; si la app pasa a primer
   * plano, el popup pierde el foco y se cierra (ver focus_check).
   * Las apps antiguas (XWayland) no le quitan el foco al mostrar su menú
   * y el popup lo taparía: con ellas, el clic siempre cierra. */
  if (p->popup_layer)
    {
      if (legacy)
        popup_hide (p);
      return;
    }

  /* Activar: el popup queda abierto. Se cierra solo si la app pasa a
   * primer plano (ver active_window_check). Mientras tanto, sin grab:
   * con él, el clic simulado de una app antigua llegaría al popup. */
  p->wait_app_menu = legacy;
  popup_ungrab (p);
  if (p->act_id != 0)
    g_source_remove (p->act_id);
  p->act_id = g_timeout_add (app_menu ? MENU_REGRAB_MS : ACT_REGRAB_MS, act_regrab, p);
}



static void
button_scrolled (TrayButton *button,
                 TrayPlugin *p)
{
  if (!in_popup (p, GTK_WIDGET (button))
      || !tray_item_get_is_legacy (tray_button_get_item (button)))
    return;

  /* Wayland: no hay grab del puntero (ver tray_button_scroll) */
  if (p->popup_layer)
    return;

  /* X11: con el grab del popup, el giro simulado del proxy le llegaría al
   * popup y no a la app antigua. Se suelta un momento, como con los clics. */

  p->wait_app_menu = TRUE;
  popup_ungrab (p);
  if (p->act_id != 0)
    g_source_remove (p->act_id);
  p->act_id = g_timeout_add (ACT_REGRAB_MS, act_regrab, p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Arrastrar y soltar
 * ════════════════════════════════════════════════════════════════════════ */

static void
button_drag_begin (GtkWidget      *widget,
                   GdkDragContext *context,
                   TrayPlugin     *p)
{
  p->dragging = TRUE;
  p->drag_item = tray_button_get_item (TRAY_BUTTON (widget));
  p->drag_from_popup = in_popup (p, widget);
  p->dropped_on_panel = FALSE;

  /* Mostrar la flecha (y el aviso del popup vacío) como destino */
  tray_plugin_relayout (p);
}



static void
button_drag_data_get (GtkWidget        *widget,
                      GdkDragContext   *context,
                      GtkSelectionData *data,
                      guint             info,
                      guint             time_,
                      TrayPlugin       *p)
{
  gchar *text = g_strdup_printf ("%p", (gpointer) tray_button_get_item (TRAY_BUTTON (widget)));

  gtk_selection_data_set (data, gtk_selection_data_get_target (data), 8,
                          (const guchar *) text, strlen (text));
  g_free (text);
}



static void
button_drag_end (GtkWidget      *widget,
                 GdkDragContext *context,
                 TrayPlugin     *p)
{
  p->dragging = FALSE;
  p->drag_item = NULL;

  if (p->drag_open_id != 0)
    {
      g_source_remove (p->drag_open_id);
      p->drag_open_id = 0;
    }

  tray_plugin_relayout (p);

  if (p->popup != NULL && gtk_widget_get_visible (p->popup))
    {
      /* Sacar un ícono del popup al panel lo cierra (como en Windows);
       * reordenar dentro del popup lo deja abierto */
      if (p->popup_order->len == 0 || (p->drag_from_popup && p->dropped_on_panel))
        {
          popup_hide (p);
        }
      else
        {
          /* GTK suelta su propio grab del arrastre después de emitir
           * "drag-end": recuperar el nuestro un instante más tarde */
          if (p->grab_retry_id != 0)
            g_source_remove (p->grab_retry_id);
          p->seat_grabbed = FALSE;   /* el arrastre ya nos lo quitó */
          p->grab_tries = 0;
          p->grab_retry_id = g_timeout_add (60, popup_try_grab, p);
        }
    }
}



/* Recupera el ítem arrastrado y verifica que siga existiendo */
static TrayItem *
drag_data_item (TrayPlugin       *p,
                GtkSelectionData *data)
{
  const guchar *raw;
  gint length;
  gchar *text;
  gpointer ptr = NULL;
  GHashTableIter iter;
  gpointer key;

  raw = gtk_selection_data_get_data_with_length (data, &length);
  if (raw == NULL || length <= 0)
    return NULL;

  text = g_strndup ((const gchar *) raw, length);
  if (sscanf (text, "%p", &ptr) != 1)
    ptr = NULL;
  g_free (text);

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (key == ptr)
      return TRAY_ITEM (key);

  return NULL;
}



/* Busca el botón más cercano al punto (x, y) de @dest y decide si el
 * ítem va antes o después. @row_major: cuadrícula que se llena por filas. */
static void
find_drop_anchor (TrayPlugin  *p,
                  GtkWidget   *dest,
                  GPtrArray   *order,
                  gint         x,
                  gint         y,
                  gboolean     row_major,
                  TrayItem    *dragged,
                  const gchar **anchor,
                  gboolean    *after)
{
  gdouble best = G_MAXDOUBLE;
  guint i;

  *anchor = NULL;
  *after = TRUE;

  for (i = 0; i < order->len; i++)
    {
      GtkWidget *btn = g_ptr_array_index (order, i);
      TrayItem *item = tray_button_get_item (TRAY_BUTTON (btn));
      GtkAllocation alloc;
      gint bx, by;
      gdouble cx, cy, dist;

      if (item == dragged || !gtk_widget_get_mapped (btn))
        continue;

      if (!gtk_widget_translate_coordinates (btn, dest, 0, 0, &bx, &by))
        continue;

      gtk_widget_get_allocation (btn, &alloc);
      cx = bx + alloc.width / 2.0;
      cy = by + alloc.height / 2.0;
      dist = (x - cx) * (x - cx) + (y - cy) * (y - cy);

      if (dist < best)
        {
          best = dist;
          *anchor = tray_item_get_key (item);

          if (row_major)
            *after = y > by + alloc.height || (y >= by && x > cx);
          else
            *after = x > bx + alloc.width || (x >= bx && y > cy);
        }
    }
}



static void
drop_on_widget (GtkWidget        *widget,
                GdkDragContext   *context,
                gint              x,
                gint              y,
                GtkSelectionData *data,
                guint             info,
                guint             time_,
                TrayPlugin       *p)
{
  TrayItem *item = drag_data_item (p, data);
  const gchar *key, *anchor = NULL;
  gboolean after = TRUE;
  gboolean hidden;

  if (item == NULL || (key = tray_item_get_key (item)) == NULL)
    return;

  if (widget == p->arrow)
    {
      /* Soltar sobre la flecha: al final del popup */
      hidden = TRUE;
      if (p->popup_order->len > 0)
        anchor = tray_item_get_key (tray_button_get_item (
                   g_ptr_array_index (p->popup_order, p->popup_order->len - 1)));
    }
  else if (widget == p->popup_frame)
    {
      hidden = TRUE;
      find_drop_anchor (p, widget, p->popup_order, x, y, TRUE, item, &anchor, &after);
    }
  else
    {
      hidden = FALSE;
      p->dropped_on_panel = TRUE;
      /* Panel horizontal multi-fila se llena por columnas; vertical, por filas */
      find_drop_anchor (p, widget, p->panel_order, x, y, !is_horizontal (p),
                        item, &anchor, &after);
    }

  tray_settings_freeze (p->settings);
  tray_settings_set_hidden (p->settings, key, hidden);
  if (anchor != NULL)
    tray_settings_move (p->settings, key, anchor, after);
  tray_settings_thaw (p->settings);
}



static gboolean
drag_open_timeout (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->drag_open_id = 0;
  popup_show (p);
  return G_SOURCE_REMOVE;
}



static gboolean
arrow_drag_motion (GtkWidget      *widget,
                   GdkDragContext *context,
                   gint            x,
                   gint            y,
                   guint           time_,
                   TrayPlugin     *p)
{
  /* Como en Windows: quedarse sobre la flecha abre el popup */
  if (p->drag_open_id == 0
      && (p->popup == NULL || !gtk_widget_get_visible (p->popup)))
    p->drag_open_id = g_timeout_add (DRAG_OPEN_MS, drag_open_timeout, p);

  return FALSE;
}



static void
arrow_drag_leave (GtkWidget      *widget,
                  GdkDragContext *context,
                  guint           time_,
                  TrayPlugin     *p)
{
  if (p->drag_open_id != 0)
    {
      g_source_remove (p->drag_open_id);
      p->drag_open_id = 0;
    }
}



static void
setup_drop_target (TrayPlugin *p, GtkWidget *widget)
{
  gtk_drag_dest_set (widget, GTK_DEST_DEFAULT_ALL,
                     dnd_targets, G_N_ELEMENTS (dnd_targets), GDK_ACTION_MOVE);
  g_signal_connect (widget, "drag-data-received", G_CALLBACK (drop_on_widget), p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Distribución de íconos
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct
{
  TrayItem *item;
  gint      index;
  guint     arrival;
} SortEntry;



static gint
sort_entries (gconstpointer a, gconstpointer b)
{
  const SortEntry *ea = a, *eb = b;

  /* Desconocidos (índice -1) al final, en orden de llegada */
  if (ea->index != eb->index)
    {
      if (ea->index < 0)
        return 1;
      if (eb->index < 0)
        return -1;
      return ea->index - eb->index;
    }
  return (gint) ea->arrival - (gint) eb->arrival;
}



static void
clear_grid (GtkWidget *grid)
{
  GList *children, *l;

  children = gtk_container_get_children (GTK_CONTAINER (grid));
  for (l = children; l != NULL; l = l->next)
    if (TRAY_IS_BUTTON (l->data))
      {
        /* Un botón que cambia de lugar no recibe el "leave": limpiar el
         * resalte para que no quede marcado como si tuviera el ratón */
        gtk_widget_unset_state_flags (l->data, GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE);
        gtk_container_remove (GTK_CONTAINER (grid), l->data);
      }
  g_list_free (children);
}



static gboolean
same_order (GPtrArray *a, GPtrArray *b)
{
  guint i;

  if (a->len != b->len)
    return FALSE;

  for (i = 0; i < a->len; i++)
    if (g_ptr_array_index (a, i) != g_ptr_array_index (b, i))
      return FALSE;

  return TRUE;
}



static void
tray_plugin_relayout (TrayPlugin *p)
{
  GPtrArray *items = tray_model_get_items (p->model);
  GPtrArray *new_panel, *new_popup;
  GArray *entries;
  TrayLayoutParams params;
  gboolean attention = FALSE;
  gboolean rebuild;
  guint i;

  if (p->relayout_id != 0)
    {
      g_source_remove (p->relayout_id);
      p->relayout_id = 0;
    }

  memset (&params, 0, sizeof (params));
  params.horizontal = is_horizontal (p);
  params.nrows = MAX (1, (gint) xfce_panel_plugin_get_nrows (p->panel_plugin));
  params.panel_size = tray_plugin_get_panel_icon_size (p);
  params.popup_size = tray_settings_get_popup_icon_size (p->settings);
  params.columns = tray_settings_get_popup_columns (p->settings);
  params.panel_spacing = tray_settings_get_panel_spacing (p->settings);
  params.popup_spacing = tray_settings_get_popup_spacing (p->settings);

  /* Ordenar los ítems listos según la configuración */
  entries = g_array_new (FALSE, FALSE, sizeof (SortEntry));
  for (i = 0; i < items->len; i++)
    {
      TrayItem *item = g_ptr_array_index (items, i);
      SortEntry entry;

      if (tray_item_get_key (item) == NULL || !g_hash_table_contains (p->buttons, item))
        continue;

      entry.item = item;
      entry.index = tray_settings_get_index (p->settings, tray_item_get_key (item));
      entry.arrival = i;
      g_array_append_val (entries, entry);
    }
  g_array_sort (entries, sort_entries);

  /* Calcular la distribución nueva sin tocar todavía los widgets */
  new_panel = g_ptr_array_new ();
  new_popup = g_ptr_array_new ();

  for (i = 0; i < entries->len; i++)
    {
      SortEntry *entry = &g_array_index (entries, SortEntry, i);
      GtkWidget *button = g_hash_table_lookup (p->buttons, entry->item);

      if (item_is_hidden (p, entry->item))
        {
          g_ptr_array_add (new_popup, button);
          if (tray_item_get_status (entry->item) == TRAY_ITEM_STATUS_ATTENTION)
            attention = TRUE;
        }
      else
        {
          g_ptr_array_add (new_panel, button);
        }
    }
  g_array_unref (entries);

  params.drag_empty = p->dragging && new_panel->len == 0;

  /* Si nada cambió (caso típico: una app actualizó su ícono o su
   * título), no se reconstruyen las cuadrículas: el botón ya se
   * redibujó solo. Se actualiza solo la flecha. */
  rebuild = !p->layout_valid
            || memcmp (&params, &p->layout_params, sizeof (params)) != 0
            || !same_order (new_panel, p->panel_order)
            || !same_order (new_popup, p->popup_order);

  if (rebuild)
    {
      clear_grid (p->panel_grid);
      clear_grid (p->popup_grid);

      g_ptr_array_unref (p->panel_order);
      g_ptr_array_unref (p->popup_order);
      p->panel_order = g_steal_pointer (&new_panel);
      p->popup_order = g_steal_pointer (&new_popup);

      /* Panel: horizontal se llena por columnas de @nrows filas;
       * vertical/deskbar, por filas de @nrows columnas */
      for (i = 0; i < p->panel_order->len; i++)
        {
          GtkWidget *button = g_ptr_array_index (p->panel_order, i);
          gint major = i / params.nrows, minor = i % params.nrows;

          tray_button_set_icon_size (TRAY_BUTTON (button), params.panel_size);
          if (params.horizontal)
            gtk_grid_attach (GTK_GRID (p->panel_grid), button, major, minor, 1, 1);
          else
            gtk_grid_attach (GTK_GRID (p->panel_grid), button, minor, major, 1, 1);
          gtk_widget_show (button);
        }

      gtk_grid_set_row_spacing (GTK_GRID (p->panel_grid), params.panel_spacing);
      gtk_grid_set_column_spacing (GTK_GRID (p->panel_grid), params.panel_spacing);
      gtk_box_set_spacing (GTK_BOX (p->box), params.panel_spacing);

      /* Con el panel vacío durante un arrastre, dejar una zona para soltar */
      if (params.drag_empty)
        gtk_widget_set_size_request (p->panel_grid, params.panel_size + 2,
                                     params.panel_size + 2);
      else
        gtk_widget_set_size_request (p->panel_grid, -1, -1);

      /* Popup: cuadrícula por filas */
      for (i = 0; i < p->popup_order->len; i++)
        {
          GtkWidget *button = g_ptr_array_index (p->popup_order, i);

          tray_button_set_icon_size (TRAY_BUTTON (button), params.popup_size);
          gtk_grid_attach (GTK_GRID (p->popup_grid), button,
                           i % params.columns, i / params.columns, 1, 1);
          gtk_widget_show (button);
        }

      gtk_grid_set_row_spacing (GTK_GRID (p->popup_grid), params.popup_spacing);
      gtk_grid_set_column_spacing (GTK_GRID (p->popup_grid), params.popup_spacing);
      gtk_widget_set_visible (p->popup_empty, p->popup_order->len == 0);
      gtk_widget_set_visible (p->popup_grid, p->popup_order->len > 0);

      gtk_orientable_set_orientation (GTK_ORIENTABLE (p->box),
                                      params.horizontal ? GTK_ORIENTATION_HORIZONTAL
                                                        : GTK_ORIENTATION_VERTICAL);
      arrow_update_icon (p);

      p->layout_params = params;
      p->layout_valid = TRUE;
    }
  else
    {
      g_ptr_array_unref (new_panel);
      g_ptr_array_unref (new_popup);
    }

  /* Flecha: solo si hay ocultos, o durante un arrastre (como destino) */
  gtk_widget_set_visible (p->arrow, p->popup_order->len > 0 || p->dragging);
  gtk_widget_set_visible (p->arrow_dot, attention);

  if (p->popup != NULL && gtk_widget_get_visible (p->popup))
    {
      if (p->popup_order->len == 0 && !p->dragging)
        popup_hide (p);
      else if (rebuild)
        popup_reposition (p);
    }
}



static gboolean
relayout_idle (gpointer user_data)
{
  TrayPlugin *p = user_data;

  p->relayout_id = 0;
  tray_plugin_relayout (p);
  return G_SOURCE_REMOVE;
}



static void
tray_plugin_schedule_relayout (TrayPlugin *p)
{
  if (p->relayout_id == 0)
    p->relayout_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE + 10, relayout_idle, p, NULL);
}



/* ════════════════════════════════════════════════════════════════════════
 * Ítems del modelo
 * ════════════════════════════════════════════════════════════════════════ */

/* Nombre de ícono para el registro de apps conocidas. Si la app solo
 * envía píxeles (Electron, muchas apps Qt), guardamos una copia en la
 * caché para poder mostrarla en el diálogo aunque la app esté cerrada. */
static gchar *
item_icon_for_registry (TrayPlugin *p,
                        TrayItem   *item,
                        gboolean    refresh_cache)
{
  const gchar *name = tray_item_get_icon_name (item);
  gchar *checksum, *dir, *path;
  GdkPixbuf *pixbuf;

  if (name != NULL && *name != '\0')
    return g_strdup (name);

  checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, tray_item_get_key (item), -1);
  dir = g_build_filename (g_get_user_cache_dir (), "xfce4", TO_CSS_PREFIX, NULL);
  path = g_strdup_printf ("%s/%s.png", dir, checksum);
  g_free (checksum);

  if (refresh_cache || !g_file_test (path, G_FILE_TEST_EXISTS))
    {
      pixbuf = tray_item_render_icon (item, 32, 1, NULL);
      if (pixbuf != NULL)
        {
          if (g_mkdir_with_parents (dir, 0700) != 0
              || !gdk_pixbuf_save (pixbuf, path, "png", NULL, NULL))
            g_clear_pointer (&path, g_free);
          g_object_unref (pixbuf);
        }
      else
        {
          g_clear_pointer (&path, g_free);
        }
    }

  g_free (dir);
  return path;
}



static void
remember_item (TrayPlugin *p,
               TrayItem   *item,
               gboolean    refresh_cache)
{
  gchar *icon = item_icon_for_registry (p, item, refresh_cache);

  tray_settings_remember (p->settings, tray_item_get_key (item),
                          tray_item_get_display_name (item), icon);
  g_free (icon);
}



static void
item_changed (TrayItem   *item,
              TrayPlugin *p)
{
  /* Mantener al día título e ícono del registro (para el diálogo).
   * La caché de íconos solo se renueva al aparecer la app, no en cada
   * cambio (algunas apps cambian el ícono a menudo). */
  remember_item (p, item, FALSE);

  /* Un cambio de estado puede moverlo entre panel y popup */
  tray_plugin_schedule_relayout (p);
}



/* Íconos elegidos por el usuario (configuración de esta instancia) */
static void
apply_icon_overrides (TrayPlugin *p)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, &value))
    tray_button_set_icon_override (TRAY_BUTTON (value),
                                   tray_settings_get_icon_override (p->settings,
                                                                    tray_item_get_key (key)));
}



static void
button_value_free (gpointer data)
{
  GtkWidget *button = data;

  gtk_widget_destroy (button);
  g_object_unref (button);
}



static void
model_item_added (TrayModel  *model,
                  TrayItem   *item,
                  TrayPlugin *p)
{
  GtkWidget *button;

  if (g_hash_table_contains (p->buttons, item))
    return;

  button = tray_button_new (item);
  g_object_ref_sink (button);
  g_hash_table_insert (p->buttons, item, button);
  tray_button_set_icon_override (TRAY_BUTTON (button),
                                 tray_settings_get_icon_override (p->settings,
                                                                  tray_item_get_key (item)));

  g_signal_connect (button, "show-menu", G_CALLBACK (button_show_menu), p);
  g_signal_connect (button, "acted", G_CALLBACK (button_acted), p);
  g_signal_connect (button, "scrolled", G_CALLBACK (button_scrolled), p);
  tray_button_set_panel_anchor (TRAY_BUTTON (button), GTK_WIDGET (p->panel_plugin));

  gtk_drag_source_set (button, GDK_BUTTON1_MASK, dnd_targets,
                       G_N_ELEMENTS (dnd_targets), GDK_ACTION_MOVE);
  g_signal_connect_after (button, "drag-begin", G_CALLBACK (button_drag_begin), p);
  g_signal_connect (button, "drag-data-get", G_CALLBACK (button_drag_data_get), p);
  g_signal_connect (button, "drag-end", G_CALLBACK (button_drag_end), p);

  g_signal_connect (item, "changed", G_CALLBACK (item_changed), p);

  remember_item (p, item, TRUE);

  tray_plugin_schedule_relayout (p);
}



static void
model_item_removed (TrayModel  *model,
                    TrayItem   *item,
                    TrayPlugin *p)
{
  GtkWidget *button = g_hash_table_lookup (p->buttons, item);

  g_signal_handlers_disconnect_by_data (item, p);

  if (button == NULL)
    return;

  if (p->drag_item == item)
    p->drag_item = NULL;

  g_ptr_array_remove (p->panel_order, button);
  g_ptr_array_remove (p->popup_order, button);
  g_hash_table_remove (p->buttons, item);

  /* El botón destruido deja un hueco en la cuadrícula: rearmarla */
  p->layout_valid = FALSE;

  tray_plugin_schedule_relayout (p);
}



static void
refresh_all_icons (TrayPlugin *p)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    tray_button_update (TRAY_BUTTON (value));
}



/* La flecha antes o después de los íconos visibles (en un GtkBox
 * vertical, "antes" es arriba) */
static void
arrow_apply_position (TrayPlugin *p)
{
  gint position = tray_settings_get_arrow_position (p->settings) == TRAY_ARROW_END ? 1 : 0;
  gint current;

  gtk_container_child_get (GTK_CONTAINER (p->box), p->arrow, "position", &current, NULL);
  if (current != position)
    gtk_box_reorder_child (GTK_BOX (p->box), p->arrow, position);
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
static void
popup_apply_corners (TrayPlugin *p)
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
  popup_apply_corners (p);
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

static void
popup_create (TrayPlugin *p)
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

  setup_drop_target (p, p->popup_frame);

  g_signal_connect (p->popup, "button-press-event", G_CALLBACK (popup_button_press), p);
  g_signal_connect (p->popup, "key-press-event", G_CALLBACK (popup_key_press), p);
  g_signal_connect (p->popup, "grab-broken-event", G_CALLBACK (popup_grab_broken), p);
  g_signal_connect (p->popup, "draw", G_CALLBACK (popup_draw), p);

  popup_apply_corners (p);
  p->composited_id = g_signal_connect_swapped (screen, "composited-changed",
                                               G_CALLBACK (popup_schedule_corners), p);
  p->theme_name_id = g_signal_connect_swapped (gtk_settings_get_for_screen (screen),
                                               "notify::gtk-theme-name",
                                               G_CALLBACK (popup_schedule_corners), p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Señales del panel
 * ════════════════════════════════════════════════════════════════════════ */

static gboolean
plugin_size_changed (XfcePanelPlugin *panel_plugin,
                     gint             size,
                     TrayPlugin      *p)
{
  tray_plugin_relayout (p);
  return TRUE;
}



static void
plugin_mode_changed (XfcePanelPlugin     *panel_plugin,
                     XfcePanelPluginMode  mode,
                     TrayPlugin          *p)
{
  tray_plugin_relayout (p);
}



static void
plugin_nrows_changed (XfcePanelPlugin *panel_plugin,
                      guint            rows,
                      TrayPlugin      *p)
{
  tray_plugin_relayout (p);
}



static void
plugin_screen_position_changed (XfcePanelPlugin    *panel_plugin,
                                XfceScreenPosition  position,
                                TrayPlugin         *p)
{
  popup_hide (p);
  arrow_update_icon (p);
}



static void
plugin_configure (XfcePanelPlugin *panel_plugin,
                  TrayPlugin      *p)
{
  popup_hide (p);
  tray_dialog_show (p);
}



static void
plugin_about (XfcePanelPlugin *panel_plugin,
              TrayPlugin      *p)
{
  static const gchar *authors[] = { "Juan Pérez <juanchiperez97@gmail.com>", NULL };

  gtk_show_about_dialog (NULL,
                         "program-name", _("Tray Overflow"),
                         "logo-icon-name", "org.xfce.panel.systray",
                         "version", PACKAGE_VERSION,
                         "comments", _("Status notifier area with a popup for hidden icons"),
                         "authors", authors,
                         "license-type", GTK_LICENSE_GPL_2_0,
                         NULL);
}



static void
apply_legacy (TrayPlugin *p)
{
  tray_legacy_set_wanted (p, tray_settings_get_legacy_tray (p->settings));
}



static void
plugin_free (XfcePanelPlugin *panel_plugin,
             TrayPlugin      *p)
{
  GHashTableIter iter;
  gpointer key;

  if (p->dialog != NULL)
    gtk_widget_destroy (p->dialog);

  tray_legacy_set_wanted (p, FALSE);

  popup_hide (p);

  if (p->relayout_id != 0)
    g_source_remove (p->relayout_id);
  if (p->grab_check_id != 0)
    g_source_remove (p->grab_check_id);
  if (p->suppress_id != 0)
    g_source_remove (p->suppress_id);
  g_clear_handle_id (&p->corners_idle_id, g_source_remove);
  g_idle_remove_by_data (p);

  menu_hook_remove (p);
  if (p->menu_shown != NULL)
    {
      g_clear_signal_handler (&p->menu_deactivate_id, p->menu_shown);
      g_clear_object (&p->menu_shown);
    }

  g_signal_handlers_disconnect_by_data (p->model, p);
  g_signal_handlers_disconnect_by_data (p->settings, p);
  g_clear_signal_handler (&p->theme_changed_id, gtk_icon_theme_get_default ());

  g_hash_table_iter_init (&iter, p->buttons);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_signal_handlers_disconnect_by_data (key, p);
  g_hash_table_unref (p->buttons);
  g_ptr_array_unref (p->panel_order);
  g_ptr_array_unref (p->popup_order);

  if (p->popup != NULL)
    {
      GdkScreen *screen = gtk_widget_get_screen (p->popup);

      g_clear_signal_handler (&p->composited_id, screen);
      g_clear_signal_handler (&p->theme_name_id, gtk_settings_get_for_screen (screen));
      gtk_widget_destroy (p->popup);
    }
  g_clear_object (&p->corners_css);

  g_object_unref (p->model);
  g_object_unref (p->settings);

  gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                GTK_STYLE_PROVIDER (p->css));
  g_object_unref (p->css);

  g_free (p);
}



/* ════════════════════════════════════════════════════════════════════════
 * Construcción
 * ════════════════════════════════════════════════════════════════════════ */

static void
tray_plugin_construct (XfcePanelPlugin *panel_plugin)
{
  TrayPlugin *p;
  GtkWidget *overlay;
  GPtrArray *items;
  guint i;

  /* Solo enlazar nuestro dominio: textdomain() (que llama
   * xfce_textdomain) cambiaría el dominio por defecto de todo el panel */
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  p = g_new0 (TrayPlugin, 1);
  p->panel_plugin = panel_plugin;
  p->settings = tray_settings_new (panel_plugin);
  p->buttons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, button_value_free);
  p->panel_order = g_ptr_array_new ();
  p->popup_order = g_ptr_array_new ();

  p->css = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (p->css, plugin_css, -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (p->css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  xfce_panel_plugin_set_small (panel_plugin, FALSE);

  /* Contenedor: [flecha][cuadrícula de íconos] */
  p->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (panel_plugin), p->box);
  xfce_panel_plugin_add_action_widget (panel_plugin, p->box);
  setup_drop_target (p, p->box);

  p->arrow = gtk_toggle_button_new ();
  gtk_button_set_relief (GTK_BUTTON (p->arrow), GTK_RELIEF_NONE);
  gtk_widget_set_can_focus (p->arrow, FALSE);
  gtk_widget_set_tooltip_text (p->arrow, _("Show hidden icons"));
  gtk_style_context_add_class (gtk_widget_get_style_context (p->arrow),
                               TO_CSS_PREFIX "-arrow");
  gtk_box_pack_start (GTK_BOX (p->box), p->arrow, FALSE, FALSE, 0);
  xfce_panel_plugin_add_action_widget (panel_plugin, p->arrow);
  g_signal_connect (p->arrow, "toggled", G_CALLBACK (arrow_toggled), p);

  overlay = gtk_overlay_new ();
  gtk_container_add (GTK_CONTAINER (p->arrow), overlay);
  p->arrow_image = gtk_image_new ();
  gtk_container_add (GTK_CONTAINER (overlay), p->arrow_image);
  p->arrow_dot = gtk_drawing_area_new ();
  gtk_widget_set_size_request (p->arrow_dot, 6, 6);
  gtk_widget_set_halign (p->arrow_dot, GTK_ALIGN_END);
  gtk_widget_set_valign (p->arrow_dot, GTK_ALIGN_START);
  gtk_widget_set_no_show_all (p->arrow_dot, TRUE);
  g_signal_connect (p->arrow_dot, "draw", G_CALLBACK (arrow_dot_draw), NULL);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), p->arrow_dot);
  gtk_widget_show_all (overlay);
  gtk_widget_set_no_show_all (p->arrow, TRUE);

  /* La flecha es destino: soltar ahí oculta; esperar encima abre el popup */
  setup_drop_target (p, p->arrow);
  g_signal_connect (p->arrow, "drag-motion", G_CALLBACK (arrow_drag_motion), p);
  g_signal_connect (p->arrow, "drag-leave", G_CALLBACK (arrow_drag_leave), p);

  p->panel_grid = gtk_grid_new ();
  gtk_widget_set_halign (p->panel_grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (p->panel_grid, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (p->box), p->panel_grid, TRUE, TRUE, 0);
  arrow_apply_position (p);

  gtk_widget_show (p->box);
  gtk_widget_show (p->panel_grid);

  popup_create (p);

  xfce_panel_plugin_menu_show_configure (panel_plugin);
  xfce_panel_plugin_menu_show_about (panel_plugin);

  g_signal_connect (panel_plugin, "free-data", G_CALLBACK (plugin_free), p);
  /* El panel pide guardar (p. ej. al cerrar sesión): escribir lo pendiente */
  g_signal_connect_swapped (panel_plugin, "save", G_CALLBACK (tray_settings_flush), p->settings);
  /* Quitado del panel: su configuración se borra, no volver a escribirla */
  g_signal_connect_swapped (panel_plugin, "removed", G_CALLBACK (tray_settings_discard), p->settings);
  g_signal_connect (panel_plugin, "size-changed", G_CALLBACK (plugin_size_changed), p);
  g_signal_connect (panel_plugin, "mode-changed", G_CALLBACK (plugin_mode_changed), p);
  g_signal_connect (panel_plugin, "nrows-changed", G_CALLBACK (plugin_nrows_changed), p);
  g_signal_connect (panel_plugin, "screen-position-changed",
                    G_CALLBACK (plugin_screen_position_changed), p);
  g_signal_connect (panel_plugin, "configure-plugin", G_CALLBACK (plugin_configure), p);
  g_signal_connect (panel_plugin, "about", G_CALLBACK (plugin_about), p);
  g_signal_connect_swapped (panel_plugin, "notify::scale-factor",
                            G_CALLBACK (refresh_all_icons), p);
  /* Cambió el "tamaño de ícono" global del panel (modo automático) */
  g_signal_connect_swapped (panel_plugin, "notify::icon-size",
                            G_CALLBACK (tray_plugin_schedule_relayout), p);

  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (tray_plugin_schedule_relayout), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (apply_icon_overrides), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (popup_apply_corners), p);
  g_signal_connect_swapped (p->settings, "changed",
                            G_CALLBACK (arrow_apply_position), p);

  /* Apps antiguas (XEmbed): arrancar o detener el proxy según la opción */
  g_signal_connect_swapped (p->settings, "changed", G_CALLBACK (apply_legacy), p);
  apply_legacy (p);

  p->theme_changed_id = g_signal_connect_swapped (gtk_icon_theme_get_default (), "changed",
                                                  G_CALLBACK (refresh_all_icons), p);

  /* Modelo compartido: tomar los ítems que ya existan */
  p->model = tray_model_ref_default ();
  g_signal_connect (p->model, "item-added", G_CALLBACK (model_item_added), p);
  g_signal_connect (p->model, "item-removed", G_CALLBACK (model_item_removed), p);

  items = tray_model_get_items (p->model);
  for (i = 0; i < items->len; i++)
    model_item_added (p->model, g_ptr_array_index (items, i), p);

  tray_plugin_relayout (p);
}

XFCE_PANEL_PLUGIN_REGISTER (tray_plugin_construct);
