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
  gboolean         popup_rgba;        /* visual con transparencia */
  gboolean         popup_rounded;     /* esquinas redondeadas ahora */
  gint             popup_radius;
  GtkCssProvider  *corners_css;
  guint            corners_idle_id;
  gulong           composited_id;     /* se prendió o apagó el compositor */
  gulong           theme_name_id;     /* cambió el tema de GTK */
  gboolean         popup_layer;       /* Wayland: popup como capa (layer-shell) */
  guint            focus_check_id;    /* Wayland: cierre al perder el foco */
  gboolean         seat_grabbed;
  gboolean         gtk_grabbed;
  guint            grab_retry_id;
  gint             grab_tries;
  guint            grab_check_id;
  gboolean         autohide_blocked;

  /* Clic en un ícono del popup: el popup sigue abierto salvo que la app
   * active otra ventana. Mientras la app recibe el clic, sin grab. */
  guint            act_id;
  gboolean         hide_if_no_grab;   /* tras un clic: sin grab, cerrar */
  gboolean         wait_app_menu;     /* X11: o esperar al menú de una app antigua */
  gboolean         refocus_hold;      /* Wayland: ver focus_check */
  gint             refocus_count;
  gint64           refocus_last;
  gboolean         watching_active;
  gulong           active_window;     /* _NET_ACTIVE_WINDOW conocido */
  guint            active_check_id;

  /* Menú abierto desde el popup */
  gboolean         menu_open;
  gboolean         menu_chosen;
  GtkWidget       *menu_shown;
  gulong           menu_deactivate_id;
  gulong           menu_hook_id;     /* "activate" de los ítems del menú */

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
