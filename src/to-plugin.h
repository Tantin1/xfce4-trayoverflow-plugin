/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <libxfce4panel/libxfce4panel.h>

#include "to-model.h"
#include "to-settings.h"

G_BEGIN_DECLS

typedef struct _TrayPlugin TrayPlugin;

/* Todo lo que, además del orden de los íconos, decide la distribución */
typedef struct
{
  gboolean horizontal;
  gboolean drag_empty;
  gint     nrows;
  gint     panel_size;
  gint     popup_size;
  gint     columns;
  gint     panel_spacing;
  gint     popup_spacing;
} TrayLayoutParams;

struct _TrayPlugin
{
  XfcePanelPlugin *panel_plugin;
  TraySettings    *settings;
  TrayModel       *model;
  GtkCssProvider  *css;

  /* TrayItem* → TrayButton* (referencia propia; el botón sobrevive
   * mientras se mueve entre el panel y el popup) */
  GHashTable      *buttons;
  GPtrArray       *panel_order;     /* TrayButton*, orden visual (sin ref) */
  GPtrArray       *popup_order;

  /* Panel */
  GtkWidget       *box;
  GtkWidget       *arrow;
  GtkWidget       *arrow_image;
  GtkWidget       *arrow_dot;
  GtkWidget       *panel_grid;
  gboolean         updating_arrow;
  gboolean         suppress_toggle;
  guint            suppress_id;

  /* Popup */
  GtkWidget       *popup;
  GtkWidget       *popup_frame;
  GtkWidget       *popup_grid;
  GtkWidget       *popup_empty;
  gboolean         popup_rounded;
  gboolean         seat_grabbed;
  gboolean         gtk_grabbed;
  guint            grab_retry_id;
  gint             grab_tries;
  guint            grab_check_id;
  gboolean         autohide_blocked;

  /* Menú abierto desde el popup */
  gboolean         menu_open;
  gboolean         menu_chosen;
  GtkWidget       *menu_shown;
  gulong           menu_deactivate_id;
  GPtrArray       *menu_hooks;

  /* Arrastre */
  gboolean         dragging;
  gboolean         drag_from_popup;
  gboolean         dropped_on_panel;
  TrayItem        *drag_item;
  guint            drag_open_id;

  guint            relayout_id;
  TrayLayoutParams layout_params;   /* con qué se armaron las cuadrículas */
  gboolean         layout_valid;
  gulong           theme_changed_id;

  GtkWidget       *dialog;
};

/* Para el diálogo de configuración */
gint tray_plugin_get_panel_icon_size (TrayPlugin *plugin);

G_END_DECLS
