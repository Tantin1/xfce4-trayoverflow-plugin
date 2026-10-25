/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Backend StatusNotifierItem (SNI / AppIndicator):
 *  - Host: se registra ante org.kde.StatusNotifierWatcher y crea ítems.
 *  - Watcher de respaldo: si nadie provee el watcher, lo provee el plugin.
 *    Si otro lo tiene (p. ej. la bandeja nativa), quedamos en cola y
 *    tomamos el relevo automáticamente cuando lo suelte.
 */
#pragma once

#include "to-model.h"

G_BEGIN_DECLS

void to_sni_backend_start (TrayModel *model);
void to_sni_backend_stop  (void);

/* TRUE si este proceso es quien provee el watcher en este momento */
gboolean to_sni_backend_owns_watcher (void);

G_END_DECLS
