/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Configuración de una instancia del plugin, persistida en xfconf
 * (canal xfce4-panel, bajo la propiedad base del plugin).
 *
 * El registro de "apps conocidas" es a la vez el orden de los íconos:
 * una lista ordenada de claves con título e ícono, que permite ocultar
 * de antemano apps que no están abiertas.
 */
#pragma once

#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

#define TRAY_ICON_SIZE_MIN 8
#define TRAY_ICON_SIZE_MAX 128
#define TRAY_SPACING_MAX   32
#define TRAY_COLUMNS_MAX   12

typedef struct
{
  gchar *key;
  gchar *title;
  gchar *icon_name;
} TrayKnown;

#define TRAY_TYPE_SETTINGS (tray_settings_get_type ())
G_DECLARE_FINAL_TYPE (TraySettings, tray_settings, TRAY, SETTINGS, GObject)

TraySettings *tray_settings_new (XfcePanelPlugin *plugin);

/* Apariencia. Tamaño 0 en el panel = automático */
gint     tray_settings_get_panel_icon_size   (TraySettings *s);
gint     tray_settings_get_popup_icon_size   (TraySettings *s);
gint     tray_settings_get_panel_spacing     (TraySettings *s);
gint     tray_settings_get_popup_spacing     (TraySettings *s);
gint     tray_settings_get_popup_columns     (TraySettings *s);
gboolean tray_settings_get_passive_in_popup  (TraySettings *s);

void     tray_settings_set_panel_icon_size   (TraySettings *s, gint value);
void     tray_settings_set_popup_icon_size   (TraySettings *s, gint value);
void     tray_settings_set_panel_spacing     (TraySettings *s, gint value);
void     tray_settings_set_popup_spacing     (TraySettings *s, gint value);
void     tray_settings_set_popup_columns     (TraySettings *s, gint value);
void     tray_settings_set_passive_in_popup  (TraySettings *s, gboolean value);

/* Mostrar apps antiguas (bandeja clásica XEmbed) mediante el proxy */
gboolean tray_settings_get_legacy_tray       (TraySettings *s);
void     tray_settings_set_legacy_tray       (TraySettings *s, gboolean value);

/* Visibilidad. @passive indica si la app está en estado Passive */
gboolean tray_settings_is_hidden             (TraySettings *s, const gchar *key, gboolean passive);
/* Ocultar/mostrar explícitamente (anula la regla de los pasivos) */
void     tray_settings_set_hidden            (TraySettings *s, const gchar *key, gboolean hidden);

/* Orden y registro de apps conocidas (arreglo de TrayKnown*, en orden) */
GPtrArray *tray_settings_get_known           (TraySettings *s);
gint     tray_settings_get_index             (TraySettings *s, const gchar *key);
/* Agrega o actualiza una app; devuelve TRUE si era nueva */
gboolean tray_settings_remember              (TraySettings *s, const gchar *key,
                                              const gchar *title, const gchar *icon_name);
void     tray_settings_forget                (TraySettings *s, const gchar *key);
/* Mueve @key justo antes (o después) de @anchor. @anchor == NULL → al final */
void     tray_settings_move                  (TraySettings *s, const gchar *key,
                                              const gchar *anchor, gboolean after);

/* Agrupar varios cambios en un solo guardado y una sola señal "changed" */
void     tray_settings_freeze                (TraySettings *s);
void     tray_settings_thaw                  (TraySettings *s);

/* El guardado en xfconf es diferido: escribir ya lo pendiente */
void     tray_settings_flush                 (TraySettings *s);
/* El plugin se quitó del panel: descartar lo pendiente y no escribir más */
void     tray_settings_discard               (TraySettings *s);

G_END_DECLS
