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
      find_drop_anchor (p, widget, p->panel_order, x, y, !tray_plugin_is_horizontal (p),
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
