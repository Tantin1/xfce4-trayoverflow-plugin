/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Arrastrar y soltar: ocultar, mostrar y reordenar íconos entre el panel
 * y el popup; quedarse sobre la flecha abre el popup.
 */
#include "to-common.h"
#include "to-plugin.h"
#include "to-popup.h"
#include "to-dnd.h"
#include "to-button.h"

#include <stdio.h>

#define DND_TARGET     "application/x-" TO_CSS_PREFIX "-item"
#define DRAG_OPEN_MS   450
#define DROP_MARK_WIDTH 2   /* grosor de la marca de inserción */

static const GtkTargetEntry dnd_targets[] = {
  { (gchar *) DND_TARGET, GTK_TARGET_SAME_APP, 0 },
};



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
  p->drag_from_popup = tray_plugin_in_popup (p, widget);
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



static void drop_mark_clear (TrayPlugin *p);

static void
button_drag_end (GtkWidget      *widget,
                 GdkDragContext *context,
                 TrayPlugin     *p)
{
  p->dragging = FALSE;
  p->drag_item = NULL;
  drop_mark_clear (p);

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
          tray_popup_hide (p);
        }
      else
        {
          /* GTK suelta su propio grab del arrastre después de emitir
           * "drag-end": recuperar el nuestro un instante más tarde */
          if (p->grab_retry_id != 0)
            g_source_remove (p->grab_retry_id);
          p->seat_grabbed = FALSE;   /* el arrastre ya nos lo quitó */
          p->grab_tries = 0;
          p->grab_retry_id = g_timeout_add (60, tray_popup_try_grab, p);
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
find_drop_anchor (TrayPlugin   *p,
                  GtkWidget    *dest,
                  GPtrArray    *order,
                  gint          x,
                  gint          y,
                  gboolean      row_major,
                  TrayItem     *dragged,
                  const gchar **anchor,
                  gboolean     *after,
                  GdkRectangle *anchor_rect)
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
          if (anchor_rect != NULL)
            {
              anchor_rect->x = bx;
              anchor_rect->y = by;
              anchor_rect->width = alloc.width;
              anchor_rect->height = alloc.height;
            }

          if (row_major)
            *after = y > by + alloc.height || (y >= by && x > cx);
          else
            *after = x > bx + alloc.width || (x >= bx && y > cy);
        }
    }
}



/* ¿Los íconos de este destino siguen el orden de izquierda a derecha
 * (TRUE) o de arriba abajo (FALSE)? Una sola fila o columna sigue su
 * propio eje: así, en un panel de una fila, "antes" y "después" son la
 * mitad izquierda y la derecha del ícono, no la de arriba y la de abajo. */
static gboolean
drop_row_major (TrayPlugin *p,
                GtkWidget  *dest)
{
  if (dest == p->popup_frame)
    return p->layout_params.columns > 1;

  if (tray_plugin_is_horizontal (p))
    /* Horizontal de varias filas: se llena por columnas */
    return tray_plugin_get_panel_rows (p) <= 1;

  /* Vertical: por filas de @nrows columnas; con una sola, de arriba abajo */
  return tray_plugin_get_panel_rows (p) > 1;
}



/* Dónde caería el ícono soltado en (x, y) de @dest */
static void
drop_target_anchor (TrayPlugin   *p,
                    GtkWidget    *dest,
                    gint          x,
                    gint          y,
                    TrayItem     *dragged,
                    const gchar **anchor,
                    gboolean     *after,
                    GdkRectangle *anchor_rect,
                    gboolean     *row_major)
{
  GPtrArray *order = dest == p->popup_frame ? p->popup_order : p->panel_order;

  *row_major = drop_row_major (p, dest);
  find_drop_anchor (p, dest, order, x, y, *row_major, dragged, anchor, after, anchor_rect);
}



/* ── Marca de inserción: una línea en el hueco donde caerá el ícono ──── */

static void
drop_mark_clear (TrayPlugin *p)
{
  if (p->drop_mark_widget != NULL)
    {
      gtk_widget_queue_draw (p->drop_mark_widget);
      p->drop_mark_widget = NULL;
    }
}



static gboolean
drop_mark_draw (GtkWidget  *widget,
                cairo_t    *cr,
                TrayPlugin *p)
{
  GtkStyleContext *context;
  GdkRGBA color;

  if (widget != p->drop_mark_widget)
    return FALSE;

  context = gtk_widget_get_style_context (widget);
  if (!gtk_style_context_lookup_color (context, "theme_selected_bg_color", &color))
    gtk_style_context_get_color (context, gtk_style_context_get_state (context), &color);

  gdk_cairo_set_source_rgba (cr, &color);
  cairo_rectangle (cr, p->drop_mark.x, p->drop_mark.y,
                   p->drop_mark.width, p->drop_mark.height);
  cairo_fill (cr);
  return FALSE;
}



static gboolean
drop_mark_motion (GtkWidget      *widget,
                  GdkDragContext *context,
                  gint            x,
                  gint            y,
                  guint           time_,
                  TrayPlugin     *p)
{
  const gchar *anchor = NULL;
  gboolean after = TRUE, row_major;
  GdkRectangle rect, mark;
  GtkAllocation alloc;
  gint spacing;

  if (!p->dragging || p->drag_item == NULL)
    return FALSE;

  drop_target_anchor (p, widget, x, y, p->drag_item, &anchor, &after, &rect, &row_major);

  /* Sin otros íconos no hay hueco que marcar */
  if (anchor == NULL)
    {
      drop_mark_clear (p);
      return FALSE;
    }

  /* La línea va en medio de la separación entre íconos */
  spacing = widget == p->popup_frame ? p->layout_params.popup_spacing
                                     : p->layout_params.panel_spacing;
  if (row_major)
    {
      mark.x = (after ? rect.x + rect.width + spacing / 2 : rect.x - (spacing + 1) / 2)
               - DROP_MARK_WIDTH / 2;
      mark.y = rect.y;
      mark.width = DROP_MARK_WIDTH;
      mark.height = rect.height;
    }
  else
    {
      mark.x = rect.x;
      mark.y = (after ? rect.y + rect.height + spacing / 2 : rect.y - (spacing + 1) / 2)
               - DROP_MARK_WIDTH / 2;
      mark.width = rect.width;
      mark.height = DROP_MARK_WIDTH;
    }

  /* Que no quede afuera del destino (primer o último ícono, sin aire) */
  gtk_widget_get_allocation (widget, &alloc);
  mark.x = CLAMP (mark.x, 0, MAX (0, alloc.width - mark.width));
  mark.y = CLAMP (mark.y, 0, MAX (0, alloc.height - mark.height));

  if (p->drop_mark_widget != widget
      || !gdk_rectangle_equal (&mark, &p->drop_mark))
    {
      drop_mark_clear (p);
      p->drop_mark_widget = widget;
      p->drop_mark = mark;
      gtk_widget_queue_draw (widget);
    }

  return FALSE;
}



static void
drop_mark_leave (GtkWidget      *widget,
                 GdkDragContext *context,
                 guint           time_,
                 TrayPlugin     *p)
{
  if (p->drop_mark_widget == widget)
    drop_mark_clear (p);
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
  else
    {
      gboolean row_major;

      hidden = widget == p->popup_frame;
      if (!hidden)
        p->dropped_on_panel = TRUE;
      /* El mismo cálculo que la marca de inserción */
      drop_target_anchor (p, widget, x, y, item, &anchor, &after, NULL, &row_major);
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
  tray_popup_show (p);
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



void
tray_dnd_setup_drop_target (TrayPlugin *p, GtkWidget *widget)
{
  gtk_drag_dest_set (widget, GTK_DEST_DEFAULT_ALL,
                     dnd_targets, G_N_ELEMENTS (dnd_targets), GDK_ACTION_MOVE);
  g_signal_connect (widget, "drag-data-received", G_CALLBACK (drop_on_widget), p);

  /* Panel y popup (la flecha no: ahí siempre va al final del popup) */
  if (widget != p->arrow)
    {
      g_signal_connect (widget, "drag-motion", G_CALLBACK (drop_mark_motion), p);
      g_signal_connect (widget, "drag-leave", G_CALLBACK (drop_mark_leave), p);
      /* Después de dibujar los íconos: la marca queda encima */
      g_signal_connect_after (widget, "draw", G_CALLBACK (drop_mark_draw), p);
    }
}






/* Un botón nuevo se puede arrastrar */
void
tray_dnd_setup_button (TrayPlugin *p,
                       GtkWidget  *button)
{
  gtk_drag_source_set (button, GDK_BUTTON1_MASK, dnd_targets,
                       G_N_ELEMENTS (dnd_targets), GDK_ACTION_MOVE);
  g_signal_connect_after (button, "drag-begin", G_CALLBACK (button_drag_begin), p);
  g_signal_connect (button, "drag-data-get", G_CALLBACK (button_drag_data_get), p);
  g_signal_connect (button, "drag-end", G_CALLBACK (button_drag_end), p);
}



/* Soltar sobre la flecha oculta el ícono; quedarse encima abre el popup */
void
tray_dnd_setup_arrow (TrayPlugin *p)
{
  tray_dnd_setup_drop_target (p, p->arrow);
  g_signal_connect (p->arrow, "drag-motion", G_CALLBACK (arrow_drag_motion), p);
  g_signal_connect (p->arrow, "drag-leave", G_CALLBACK (arrow_drag_leave), p);
}
