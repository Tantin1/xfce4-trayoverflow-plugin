/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TrayModel: lista compartida de ítems "listos". Es un singleton por
 * proceso: todas las instancias del plugin (p. ej. en dos paneles) ven
 * los mismos ítems, y los backends se arrancan una sola vez.
 */
#pragma once

#include "to-item.h"

G_BEGIN_DECLS

#define TRAY_TYPE_MODEL (tray_model_get_type ())
G_DECLARE_FINAL_TYPE (TrayModel, tray_model, TRAY, MODEL, GObject)

/* Devuelve una referencia nueva al modelo compartido (lo crea si hace falta) */
TrayModel *tray_model_ref_default (void);

/* Arreglo de TrayItem*, propiedad del modelo */
GPtrArray *tray_model_get_items   (TrayModel *model);
gboolean   tray_model_has_item    (TrayModel *model, TrayItem *item);

/* Para backends */
void       tray_model_add_item    (TrayModel *model, TrayItem *item);
void       tray_model_remove_item (TrayModel *model, TrayItem *item);

G_END_DECLS
