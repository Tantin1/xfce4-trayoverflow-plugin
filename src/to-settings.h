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

/* Esquinas del popup */
typedef enum
{
  TRAY_CORNERS_THEME = 0,   /* el radio que el tema usa en sus menús */
  TRAY_CORNERS_ROUNDED,
  TRAY_CORNERS_SQUARE,
} TrayCorners;

/* Lugar de la flecha en el panel, respecto de los íconos visibles */
typedef enum
{
  TRAY_ARROW_START = 0,     /* izquierda (arriba en paneles verticales) */
  TRAY_ARROW_END,           /* derecha (abajo) */
} TrayArrowPosition;

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
gboolean tray_settings_get_new_in_popup      (TraySettings *s);
gboolean tray_settings_get_symbolic_icons    (TraySettings *s);
gint     tray_settings_get_popup_corners     (TraySettings *s);
gint     tray_settings_get_arrow_position    (TraySettings *s);

void     tray_settings_set_panel_icon_size   (TraySettings *s, gint value);
void     tray_settings_set_popup_icon_size   (TraySettings *s, gint value);
void     tray_settings_set_panel_spacing     (TraySettings *s, gint value);
void     tray_settings_set_popup_spacing     (TraySettings *s, gint value);
void     tray_settings_set_popup_columns     (TraySettings *s, gint value);
void     tray_settings_set_passive_in_popup  (TraySettings *s, gboolean value);
void     tray_settings_set_new_in_popup      (TraySettings *s, gboolean value);
void     tray_settings_set_symbolic_icons    (TraySettings *s, gboolean value);
void     tray_settings_set_popup_corners     (TraySettings *s, gint value);
void     tray_settings_set_arrow_position    (TraySettings *s, gint value);

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

/* Valor guardado que significa "usar el ícono propio de la app", aunque
 * exista un ícono por defecto para ella (ver default_icon_for) */
#define TRAY_ICON_OWN "-"

/* Ícono a usar para una app en lugar del que manda (nombre del tema o ruta
 * de archivo): el elegido por el usuario o, si no eligió, uno por defecto
 * para apps conocidas que se ven mal. NULL = el de la app */
const gchar *tray_settings_get_icon_override (TraySettings *s, const gchar *key);
/* Solo lo elegido por el usuario: NULL (nada), TRAY_ICON_OWN o un ícono */
const gchar *tray_settings_get_icon_choice   (TraySettings *s, const gchar *key);
/* @icon: un ícono, TRAY_ICON_OWN, o NULL / "" para volver al por defecto */
void     tray_settings_set_icon_override     (TraySettings *s, const gchar *key,
                                              const gchar *icon);

/* Agrupar varios cambios en un solo guardado y una sola señal "changed" */
void     tray_settings_freeze                (TraySettings *s);
void     tray_settings_thaw                  (TraySettings *s);

/* El guardado en xfconf es diferido: escribir ya lo pendiente */
void     tray_settings_flush                 (TraySettings *s);
/* El plugin se quitó del panel: descartar lo pendiente y no escribir más */
void     tray_settings_discard               (TraySettings *s);

G_END_DECLS
