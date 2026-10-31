/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "to-plugin.h"

G_BEGIN_DECLS

void     tray_popup_create            (TrayPlugin *p);
void     tray_popup_setup_button      (TrayPlugin *p, GtkWidget *button);

void     tray_popup_show              (TrayPlugin *p);
void     tray_popup_hide              (TrayPlugin *p);
void     tray_popup_reposition        (TrayPlugin *p);

void     tray_popup_grab              (TrayPlugin *p);
void     tray_popup_ungrab            (TrayPlugin *p);
gboolean tray_popup_try_grab          (gpointer    user_data);   /* GSourceFunc */

void     tray_popup_apply_corners     (TrayPlugin *p);
void     tray_popup_menu_hook_remove  (TrayPlugin *p);

gboolean tray_popup_toggle_from_keyboard (TrayPlugin *p, gboolean open);
void     tray_popup_clear_selection   (TrayPlugin *p);

G_END_DECLS
