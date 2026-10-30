/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Soporte de apps antiguas (bandeja clásica XEmbed): arranca y detiene el
 * proxy xembed-proxy, que convierte sus íconos en ítems StatusNotifier.
 * Un solo proxy por proceso, aunque haya varias instancias del plugin:
 * corre mientras al menos una lo pida.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
  TRAY_LEGACY_UNAVAILABLE,  /* el proxy no se compiló o no está instalado */
  TRAY_LEGACY_OFF,          /* nadie lo pidió */
  TRAY_LEGACY_ACTIVE,       /* el proxy es la bandeja clásica */
  TRAY_LEGACY_WAITING,      /* otra bandeja clásica está activa */
  TRAY_LEGACY_FAILED,       /* el proxy falla al arrancar */
  TRAY_LEGACY_NO_XWAYLAND,  /* Wayland sin XWayland: no hay dónde correr */
} TrayLegacyState;

/* @owner identifica a quien lo pide (una instancia del plugin) */
void            tray_legacy_set_wanted (gpointer owner, gboolean wanted);
gboolean        tray_legacy_available  (void);
TrayLegacyState tray_legacy_get_state  (GtkWidget *reference);

/* ¿Hay alguna bandeja clásica activa en la pantalla? (-1: no se sabe).
 * @is_proxy indica si es nuestro proxy. */
gint            tray_legacy_tray_owner (GtkWidget *reference, gboolean *is_proxy);

G_END_DECLS
