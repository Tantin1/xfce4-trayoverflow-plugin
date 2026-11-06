/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Informe de diagnóstico: sesión, versiones, estado de los backends y de
 * cada app, en texto plano para copiar y pegar en un reporte de errores.
 */
#pragma once

#include "to-plugin.h"

G_BEGIN_DECLS

/* Texto nuevo (liberar con g_free). En inglés a propósito: es para
 * reportes, que los lea quien los lea */
gchar *tray_diag_report (TrayPlugin *p);

G_END_DECLS
