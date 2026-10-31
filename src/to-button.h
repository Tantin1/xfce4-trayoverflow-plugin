/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TrayButton: el botón que representa a un TrayItem, en el panel o en
 * el popup. Traduce clics y scroll en acciones del ítem y avisa a la
 * vista (señales) cuando hay que mostrar un menú o cuando el usuario
 * activó algo, para que el popup pueda cerrarse.
 */
#pragma once

#include "to-item.h"

G_BEGIN_DECLS

#define TRAY_TYPE_BUTTON (tray_button_get_type ())
G_DECLARE_FINAL_TYPE (TrayButton, tray_button, TRAY, BUTTON, GtkButton)

GtkWidget *tray_button_new           (TrayItem *item);
TrayItem  *tray_button_get_item      (TrayButton *button);
void       tray_button_set_icon_size (TrayButton *button, gint size);
void       tray_button_update        (TrayButton *button);
/* Ícono elegido por el usuario (nombre del tema o ruta), NULL = el de la app */
void       tray_button_set_icon_override (TrayButton *button, const gchar *icon);
void       tray_button_set_symbolic      (TrayButton *button, gboolean symbolic);
void       tray_button_set_panel_anchor  (TrayButton *button, GtkWidget *anchor);
void       tray_button_activate_from_keyboard (TrayButton *button, gboolean menu);

G_END_DECLS
