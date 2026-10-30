/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-button.h"

#include <math.h>

#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#include <libxfce4panel/libxfce4panel.h>
#endif

struct _TrayButton
{
  GtkButton  parent_instance;

  TrayItem  *item;
  GtkWidget *image;
  gint       icon_size;
  gboolean   sized;            /* ya recibió su tamaño real */

  guint      pressed_button;   /* botón presionado sobre nosotros, 0 si ninguno */
  gdouble    scroll_x;         /* acumuladores de scroll suave */
  gdouble    scroll_y;

  gulong     changed_id;
  gulong     fallback_id;
  GdkEvent  *activate_event;   /* último clic que activó la app (ver
                                * tray_button_activate_fallback) */

  /* Con qué se dibujó el ícono por última vez, para no redibujar en vano */
  GdkRGBA    drawn_color;
  gint       drawn_scale;
  gchar     *tooltip;

  gchar     *icon_override;    /* ícono elegido por el usuario, o NULL */
  GtkWidget *panel_anchor;     /* el plugin en el panel (referencia débil) */
};

G_DEFINE_TYPE (TrayButton, tray_button, GTK_TYPE_BUTTON)

enum
{
  SIGNAL_SHOW_MENU,
  SIGNAL_ACTED,
  SIGNAL_SCROLLED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];



/* Coordenadas de pantalla en píxeles de dispositivo, como espera SNI */
static void
event_root_coords (GtkWidget *widget,
                   gdouble    x_root,
                   gdouble    y_root,
                   gint      *x,
                   gint      *y)
{
  gint scale = gtk_widget_get_scale_factor (widget);

#ifdef HAVE_GTK_LAYER_SHELL
  {
    /* Wayland: x_root/y_root son relativas a la ventana (el panel o el
     * popup, que son capas ancladas arriba a la izquierda): se suma su
     * posición. Las apps antiguas (vía XWayland) ubican su menú en este
     * punto. En píxeles lógicos, que es lo que usa XWayland. */
    GtkWidget *toplevel = gtk_widget_get_toplevel (widget);

    if (GTK_IS_WINDOW (toplevel) && gtk_layer_is_layer_window (GTK_WINDOW (toplevel)))
      {
        GtkWindow *window = GTK_WINDOW (toplevel);
        GtkWidget *plugin = gtk_widget_get_ancestor (widget, XFCE_TYPE_PANEL_PLUGIN);
        GdkMonitor *monitor = gtk_layer_get_monitor (window);
        GdkRectangle geometry = { 0, 0, 0, 0 };
        GtkAllocation alloc;
        gint ox, oy, bx = 0, by = 0, cx, cy;
        gint pw = gtk_widget_get_allocated_width (toplevel);
        gint ph = gtk_widget_get_allocated_height (toplevel);

        if (monitor == NULL && gtk_widget_get_window (toplevel) != NULL)
          monitor = gdk_display_get_monitor_at_window (gtk_widget_get_display (toplevel),
                                                       gtk_widget_get_window (toplevel));
        if (monitor != NULL)
          gdk_monitor_get_geometry (monitor, &geometry);

        ox = geometry.x + gtk_layer_get_margin (window, GTK_LAYER_SHELL_EDGE_LEFT);
        oy = geometry.y + gtk_layer_get_margin (window, GTK_LAYER_SHELL_EDGE_TOP);

        /* Ícono del popup: el punto del clic (el popup se cierra antes
         * de que aparezca el menú de la app) */
        if (plugin == NULL)
          {
            *x = ox + (gint) x_root;
            *y = oy + (gint) y_root;
            return;
          }

        /* Ícono del panel: las ventanas X quedan debajo del panel, así
         * que el punto va justo afuera de su borde, hacia donde se abren
         * los menús (el proxy esconde su ventana del lado del panel) */
        gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &bx, &by);
        gtk_widget_get_allocation (widget, &alloc);
        cx = ox + bx + alloc.width / 2;
        cy = oy + by + alloc.height / 2;

        switch (xfce_panel_plugin_arrow_type (XFCE_PANEL_PLUGIN (plugin)))
          {
          case GTK_ARROW_UP:    cy = oy - 1;  break;
          case GTK_ARROW_DOWN:  cy = oy + ph; break;
          case GTK_ARROW_LEFT:  cx = ox - 1;  break;
          case GTK_ARROW_RIGHT: cx = ox + pw; break;
          default: break;
          }

        *x = cx;
        *y = cy;
        return;
      }
  }
#endif

  *x = (gint) (x_root * scale);
  *y = (gint) (y_root * scale);
}



static gboolean
menu_has_children (GtkWidget *menu)
{
  GList *children;
  gboolean result;

  if (menu == NULL)
    return FALSE;

  children = gtk_container_get_children (GTK_CONTAINER (menu));
  result = children != NULL;
  g_list_free (children);
  return result;
}



static gboolean
tray_button_button_press (GtkWidget      *widget,
                          GdkEventButton *event)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GtkWidget *menu = NULL;
  gboolean wants_menu;
  gint x, y;

  /* Ignorar los eventos de doble/triple clic sintetizados por GTK */
  if (event->type != GDK_BUTTON_PRESS)
    return TRUE;

  button->pressed_button = 0;

  /* El clic derecho abre el menú al presionar, como en cualquier menú.
   * El izquierdo (aunque la app sea "solo menú") se resuelve al soltar,
   * para que el ícono se pueda arrastrar. */
  wants_menu = event->button == 3;

  if (wants_menu)
    {
      menu = tray_item_get_menu (button->item);

      if (menu_has_children (menu))
        {
          g_signal_emit (button, signals[SIGNAL_SHOW_MENU], 0, menu, event);
          return TRUE;
        }

      /* Sin menú D-Bus: que la app muestre el suyo */
      event_root_coords (widget, event->x_root, event->y_root, &x, &y);
      tray_item_context_menu (button->item, x, y);
      g_signal_emit (button, signals[SIGNAL_ACTED], 0, TRUE);
      return TRUE;
    }

  button->pressed_button = event->button;

  /* Encadenar para la animación de "presionado" */
  GTK_WIDGET_CLASS (tray_button_parent_class)->button_press_event (widget, event);
  return TRUE;
}



static gboolean
tray_button_button_release (GtkWidget      *widget,
                            GdkEventButton *event)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GtkAllocation alloc;
  gboolean inside, app_menu = FALSE;
  guint pressed = button->pressed_button;
  gint x, y;

  button->pressed_button = 0;
  GTK_WIDGET_CLASS (tray_button_parent_class)->button_release_event (widget, event);

  if (pressed == 0 || pressed != event->button)
    return TRUE;

  /* Solo actuar si se soltó dentro del botón */
  gtk_widget_get_allocation (widget, &alloc);
  inside = event->x >= 0 && event->y >= 0
           && event->x < alloc.width && event->y < alloc.height;
  if (!inside)
    return TRUE;

  event_root_coords (widget, event->x_root, event->y_root, &x, &y);

  if (event->button == 1 && tray_item_get_is_menu (button->item))
    {
      GtkWidget *menu = tray_item_get_menu (button->item);

      if (menu_has_children (menu))
        {
          g_signal_emit (button, signals[SIGNAL_SHOW_MENU], 0, menu, event);
          return TRUE;
        }
      tray_item_context_menu (button->item, x, y);
      app_menu = TRUE;
    }
  else if (event->button == 1)
    {
      /* Si la app rechaza Activate, su menú se abre más tarde, al llegar
       * la respuesta: ahí hará falta este clic como evento de origen */
      g_clear_pointer (&button->activate_event, gdk_event_free);
      button->activate_event = gdk_event_copy ((GdkEvent *) event);
      tray_item_activate (button->item, x, y);
    }
  else if (event->button == 2)
    tray_item_secondary_activate (button->item, x, y);
  else
    return TRUE;

  g_signal_emit (button, signals[SIGNAL_ACTED], 0, app_menu);
  return TRUE;
}



static gboolean
tray_button_scroll (GtkWidget      *widget,
                    GdkEventScroll *event)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GtkWidget *ref = widget;
  gint dx = 0, dy = 0, x, y;

  switch (event->direction)
    {
    case GDK_SCROLL_UP:    dy = -1; break;
    case GDK_SCROLL_DOWN:  dy = 1;  break;
    case GDK_SCROLL_LEFT:  dx = -1; break;
    case GDK_SCROLL_RIGHT: dx = 1;  break;
    case GDK_SCROLL_SMOOTH:
      {
        gdouble ddx = 0, ddy = 0;

        gdk_event_get_scroll_deltas ((GdkEvent *) event, &ddx, &ddy);
        button->scroll_x += ddx;
        button->scroll_y += ddy;
        dx = (gint) trunc (button->scroll_x);
        dy = (gint) trunc (button->scroll_y);
        button->scroll_x -= dx;
        button->scroll_y -= dy;
      }
      break;
    default:
      break;
    }

  if (dx == 0 && dy == 0)
    return TRUE;

  /* Antes de mandar el giro, para que el popup suelte el puntero (X11) */
  g_signal_emit (button, signals[SIGNAL_SCROLLED], 0);

  /* Wayland, ícono del popup: un giro no abre ningún menú, así que el
   * punto puede ser el borde del panel, donde el proxy esconde su ventana
   * debajo del panel. Bajo el puntero, labwc le daría el foco a esa
   * ventana (el popup se cerraría) y se vería un cuadrado negro. */
#ifdef HAVE_GTK_LAYER_SHELL
  if (button->panel_anchor != NULL
      && gtk_widget_get_ancestor (widget, XFCE_TYPE_PANEL_PLUGIN) == NULL)
    ref = button->panel_anchor;
#endif

  event_root_coords (ref, event->x_root, event->y_root, &x, &y);
  if (dy != 0)
    tray_item_scroll (button->item, dy, FALSE, x, y);
  if (dx != 0)
    tray_item_scroll (button->item, dx, TRUE, x, y);

  return TRUE;
}



/* El ícono elegido por el usuario, si lo hay y existe; si no, el de la app */
static GdkPixbuf *
render_icon (TrayButton *button,
             gint        scale)
{
  GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET (button));
  GdkPixbuf *pixbuf;

  pixbuf = tray_icon_load (button->icon_override, button->icon_size, scale, context);
  if (pixbuf == NULL)
    pixbuf = tray_item_render_icon (button->item, button->icon_size, scale, context);

  return pixbuf;
}



static void
tray_button_drag_begin (GtkWidget      *widget,
                        GdkDragContext *context)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GdkPixbuf *pixbuf;

  /* Un arrastre no es un clic */
  button->pressed_button = 0;

  pixbuf = render_icon (button, 1);
  if (pixbuf != NULL)
    {
      gtk_drag_set_icon_pixbuf (context, pixbuf,
                                gdk_pixbuf_get_width (pixbuf) / 2,
                                gdk_pixbuf_get_height (pixbuf) / 2);
      g_object_unref (pixbuf);
    }
}



static void
current_color (GtkWidget *widget,
               GdkRGBA   *color)
{
  GtkStyleContext *context = gtk_widget_get_style_context (widget);

  gtk_style_context_get_color (context, gtk_style_context_get_state (context), color);
}



static void
tray_button_style_updated (GtkWidget *widget)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GdkRGBA color;

  GTK_WIDGET_CLASS (tray_button_parent_class)->style_updated (widget);

  /* Los íconos simbólicos toman el color del tema, así que hay que
   * redibujar si el color cambió. Solo entonces: un cambio de estado
   * (p. ej. pasar el ratón) también emite style-updated, y redibujar
   * siempre provocaría un bucle de redibujado. */
  current_color (widget, &color);
  if (!gdk_rgba_equal (&color, &button->drawn_color)
      || gtk_widget_get_scale_factor (widget) != button->drawn_scale)
    tray_button_update (button);
}



static void
tray_button_activate_fallback (TrayButton *button)
{
  GtkWidget *menu = tray_item_get_menu (button->item);
  GdkEvent *event = NULL;

  if (!menu_has_children (menu))
    return;

  /* GTK quiere el evento que originó el menú (si no, avisa "no trigger
   * event for menu popup"). Se usa el clic que activó la app, con la
   * hora actual: el original ya es viejo y podría impedir el grab. */
  if (button->activate_event != NULL)
    {
      event = gdk_event_copy (button->activate_event);
      event->button.time = GDK_CURRENT_TIME;
    }

  g_signal_emit (button, signals[SIGNAL_SHOW_MENU], 0, menu, event);

  if (event != NULL)
    gdk_event_free (event);
}



static void
tray_button_dispose (GObject *object)
{
  TrayButton *button = TRAY_BUTTON (object);

  if (button->item != NULL)
    {
      tray_item_release_size (button->item, button);
      g_clear_signal_handler (&button->changed_id, button->item);
      g_clear_signal_handler (&button->fallback_id, button->item);
      g_clear_object (&button->item);
    }
  g_clear_pointer (&button->tooltip, g_free);
  g_clear_pointer (&button->icon_override, g_free);
  g_clear_pointer (&button->activate_event, gdk_event_free);
  g_clear_weak_pointer (&button->panel_anchor);

  G_OBJECT_CLASS (tray_button_parent_class)->dispose (object);
}



static void
tray_button_class_init (TrayButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = tray_button_dispose;

  widget_class->button_press_event = tray_button_button_press;
  widget_class->button_release_event = tray_button_button_release;
  widget_class->scroll_event = tray_button_scroll;
  widget_class->drag_begin = tray_button_drag_begin;
  widget_class->style_updated = tray_button_style_updated;

  /* (menu, evento disparador o NULL) */
  signals[SIGNAL_SHOW_MENU] =
    g_signal_new ("show-menu", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, GTK_TYPE_WIDGET, G_TYPE_POINTER);

  /* El usuario activó el ítem (la app puede abrir una ventana).
   * (app_menu): TRUE si se le pidió a la app que muestre su propio menú */
  signals[SIGNAL_ACTED] =
    g_signal_new ("acted", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  /* Se va a mandar un giro de rueda a la app */
  signals[SIGNAL_SCROLLED] =
    g_signal_new ("scrolled", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}



static void
tray_button_init (TrayButton *button)
{
  GtkWidget *widget = GTK_WIDGET (button);

  button->icon_size = 16;

  gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
  gtk_widget_set_can_focus (widget, FALSE);
  gtk_widget_set_can_default (widget, FALSE);
  gtk_widget_add_events (widget, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
  gtk_style_context_add_class (gtk_widget_get_style_context (widget),
                               TO_CSS_PREFIX "-button");

  button->image = gtk_image_new ();
  gtk_container_add (GTK_CONTAINER (button), button->image);
  gtk_widget_show (button->image);
}



GtkWidget *
tray_button_new (TrayItem *item)
{
  TrayButton *button;

  g_return_val_if_fail (TRAY_IS_ITEM (item), NULL);

  button = g_object_new (TRAY_TYPE_BUTTON, NULL);
  button->item = g_object_ref (item);

  button->changed_id = g_signal_connect_swapped (item, "changed",
                                                 G_CALLBACK (tray_button_update), button);
  button->fallback_id = g_signal_connect_swapped (item, "activate-fallback",
                                                  G_CALLBACK (tray_button_activate_fallback),
                                                  button);

  tray_button_update (button);
  return GTK_WIDGET (button);
}



TrayItem *
tray_button_get_item (TrayButton *button)
{
  g_return_val_if_fail (TRAY_IS_BUTTON (button), NULL);
  return button->item;
}



void
tray_button_set_icon_size (TrayButton *button,
                           gint        size)
{
  g_return_if_fail (TRAY_IS_BUTTON (button));

  size = MAX (size, 1);
  if (button->icon_size != size || !button->sized)
    {
      button->icon_size = size;
      button->sized = TRUE;
      tray_button_update (button);
    }
}



void
tray_button_set_icon_override (TrayButton  *button,
                               const gchar *icon)
{
  g_return_if_fail (TRAY_IS_BUTTON (button));

  if (icon != NULL && *icon == '\0')
    icon = NULL;

  if (g_strcmp0 (button->icon_override, icon) != 0)
    {
      g_free (button->icon_override);
      button->icon_override = g_strdup (icon);
      tray_button_update (button);
    }
}



void
tray_button_update (TrayButton *button)
{
  GtkWidget *widget;
  GdkPixbuf *pixbuf;
  gchar *tooltip;
  gint scale;

  g_return_if_fail (TRAY_IS_BUTTON (button));

  /* Sin tamaño real todavía (recién creado): no dibujar a un tamaño
   * provisional que se vería un instante y causaría un redibujado inútil */
  if (button->item == NULL || !button->sized)
    return;

  widget = GTK_WIDGET (button);
  scale = gtk_widget_get_scale_factor (widget);

  button->drawn_scale = scale;
  current_color (widget, &button->drawn_color);

  /* Avisar a qué tamaño se muestra (apps XEmbed: se redibujan a ese
   * tamaño y llegan nítidas, sin reescalar). Solo una vez ubicado: el
   * tamaño provisional de un botón recién creado no es el real. */
  if (button->sized)
    tray_item_request_size (button->item, button, button->icon_size * scale);

  pixbuf = render_icon (button, scale);
  if (pixbuf != NULL)
    {
      cairo_surface_t *surface;

      surface = gdk_cairo_surface_create_from_pixbuf (pixbuf, scale, NULL);
      gtk_image_set_from_surface (GTK_IMAGE (button->image), surface);
      cairo_surface_destroy (surface);
      g_object_unref (pixbuf);
    }
  else
    {
      gtk_image_clear (GTK_IMAGE (button->image));
    }

  /* Reservar siempre el cuadrado completo para que la grilla no salte */
  gtk_widget_set_size_request (button->image, button->icon_size, button->icon_size);

  /* Reasignar el tooltip solo si cambió: hacerlo reactiva el tooltip
   * aunque esté suprimido (p. ej. con un menú abierto) */
  tooltip = tray_item_get_tooltip_markup (button->item);
  if (g_strcmp0 (tooltip, button->tooltip) != 0)
    {
      gtk_widget_set_tooltip_markup (widget, tooltip);
      g_free (button->tooltip);
      button->tooltip = tooltip;
    }
  else
    {
      g_free (tooltip);
    }
}



/* El plugin en el panel: referencia para ubicar los giros de los íconos
 * del popup (ver tray_button_scroll) */
void
tray_button_set_panel_anchor (TrayButton *button,
                              GtkWidget  *anchor)
{
  g_return_if_fail (TRAY_IS_BUTTON (button));
  g_set_weak_pointer (&button->panel_anchor, anchor);
}
