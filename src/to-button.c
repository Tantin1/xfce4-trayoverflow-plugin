/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-button.h"

#include <math.h>

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

  /* Con qué se dibujó el ícono por última vez, para no redibujar en vano */
  GdkRGBA    drawn_color;
  gint       drawn_scale;
  gchar     *tooltip;
};

G_DEFINE_TYPE (TrayButton, tray_button, GTK_TYPE_BUTTON)

enum
{
  SIGNAL_SHOW_MENU,
  SIGNAL_ACTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];



/* Coordenadas de pantalla en píxeles de dispositivo, como espera SNI */
static void
event_root_coords (GtkWidget      *widget,
                   GdkEventButton *event,
                   gint           *x,
                   gint           *y)
{
  gint scale = gtk_widget_get_scale_factor (widget);

  *x = (gint) (event->x_root * scale);
  *y = (gint) (event->y_root * scale);
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
      event_root_coords (widget, event, &x, &y);
      tray_item_context_menu (button->item, x, y);
      g_signal_emit (button, signals[SIGNAL_ACTED], 0);
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
  gboolean inside;
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

  event_root_coords (widget, event, &x, &y);

  if (event->button == 1 && tray_item_get_is_menu (button->item))
    {
      GtkWidget *menu = tray_item_get_menu (button->item);

      if (menu_has_children (menu))
        {
          g_signal_emit (button, signals[SIGNAL_SHOW_MENU], 0, menu, event);
          return TRUE;
        }
      tray_item_context_menu (button->item, x, y);
    }
  else if (event->button == 1)
    tray_item_activate (button->item, x, y);
  else if (event->button == 2)
    tray_item_secondary_activate (button->item, x, y);
  else
    return TRUE;

  g_signal_emit (button, signals[SIGNAL_ACTED], 0);
  return TRUE;
}



static gboolean
tray_button_scroll (GtkWidget      *widget,
                    GdkEventScroll *event)
{
  TrayButton *button = TRAY_BUTTON (widget);
  gint dx = 0, dy = 0;

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

  if (dy != 0)
    tray_item_scroll (button->item, dy, FALSE);
  if (dx != 0)
    tray_item_scroll (button->item, dx, TRUE);

  return TRUE;
}



static void
tray_button_drag_begin (GtkWidget      *widget,
                        GdkDragContext *context)
{
  TrayButton *button = TRAY_BUTTON (widget);
  GdkPixbuf *pixbuf;

  /* Un arrastre no es un clic */
  button->pressed_button = 0;

  pixbuf = tray_item_render_icon (button->item, button->icon_size, 1,
                                  gtk_widget_get_style_context (widget));
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

  if (menu_has_children (menu))
    g_signal_emit (button, signals[SIGNAL_SHOW_MENU], 0, menu, NULL);
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

  /* El usuario activó el ítem (la app puede abrir una ventana) */
  signals[SIGNAL_ACTED] =
    g_signal_new ("acted", G_TYPE_FROM_CLASS (klass),
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

  pixbuf = tray_item_render_icon (button->item, button->icon_size, scale,
                                  gtk_widget_get_style_context (widget));
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
