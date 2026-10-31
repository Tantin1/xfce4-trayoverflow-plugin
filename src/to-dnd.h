/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "to-plugin.h"

G_BEGIN_DECLS

void tray_dnd_setup_button      (TrayPlugin *p, GtkWidget *button);
void tray_dnd_setup_arrow       (TrayPlugin *p);
void tray_dnd_setup_drop_target (TrayPlugin *p, GtkWidget *widget);

G_END_DECLS
