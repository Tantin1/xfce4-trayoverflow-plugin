/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TrayItem: representación genérica de un ícono de bandeja.
 * No sabe nada de D-Bus ni de XEmbed: cada backend crea una subclase,
 * rellena los datos con los setters "para backends" e implementa las
 * acciones virtuales (activar, menú, scroll...).
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
  TRAY_ITEM_STATUS_PASSIVE,
  TRAY_ITEM_STATUS_ACTIVE,
  TRAY_ITEM_STATUS_ATTENTION,
} TrayItemStatus;

typedef enum
{
  TRAY_ITEM_ICON_MAIN,
  TRAY_ITEM_ICON_ATTENTION,
  TRAY_ITEM_ICON_OVERLAY,
  TRAY_ITEM_N_ICONS,
} TrayItemIconRole;

#define TRAY_TYPE_ITEM (tray_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (TrayItem, tray_item, TRAY, ITEM, GObject)

struct _TrayItemClass
{
  GObjectClass parent_class;

  /* Virtuales, implementadas por cada backend.
   * Las coordenadas son de pantalla, en píxeles de dispositivo. */
  GtkWidget *(*get_menu)           (TrayItem *item);
  void       (*activate)           (TrayItem *item, gint x, gint y);
  void       (*secondary_activate) (TrayItem *item, gint x, gint y);
  void       (*context_menu)       (TrayItem *item, gint x, gint y);
  void       (*scroll)             (TrayItem *item, gint delta, gboolean horizontal,
                                    gint x, gint y);
  /* Opcional: el backend puede pedirle a la app que dibuje a ese tamaño
   * (en píxeles de dispositivo), para no tener que reescalar */
  void       (*set_icon_size)      (TrayItem *item, gint pixels);
};

/* ── API pública (vista) ─────────────────────────────────────────────── */

/* Identidad estable entre reinicios; NULL hasta que el ítem está listo */
const gchar    *tray_item_get_key            (TrayItem *item);
const gchar    *tray_item_get_title          (TrayItem *item);
/* Título "para mostrar": título, tooltip o clave, lo primero que exista */
const gchar    *tray_item_get_display_name   (TrayItem *item);
const gchar    *tray_item_get_icon_name      (TrayItem *item);
TrayItemStatus  tray_item_get_status         (TrayItem *item);
gboolean        tray_item_get_is_menu        (TrayItem *item);
/* App antigua (XEmbed) mostrada por nuestro proxy */
gboolean        tray_item_get_is_legacy      (TrayItem *item);
gchar          *tray_item_get_tooltip_markup (TrayItem *item);

/* Devuelve un pixbuf de (size * scale) píxeles, o NULL.
 * @context se usa para colorear íconos simbólicos; puede ser NULL. */
GdkPixbuf      *tray_item_render_icon        (TrayItem        *item,
                                              gint             size,
                                              gint             scale,
                                              GtkStyleContext *context,
                                              gboolean         prefer_symbolic);

/* Carga un ícono por nombre (del tema del usuario) o por ruta de archivo,
 * a (size * scale) píxeles. NULL si no existe. */
GdkPixbuf      *tray_icon_load               (const gchar     *name,
                                              gint             size,
                                              gint             scale,
                                              GtkStyleContext *context);

GtkWidget      *tray_item_get_menu           (TrayItem *item);
void            tray_item_activate           (TrayItem *item, gint x, gint y);
void            tray_item_secondary_activate (TrayItem *item, gint x, gint y);
void            tray_item_context_menu       (TrayItem *item, gint x, gint y);
void            tray_item_scroll             (TrayItem *item, gint delta, gboolean horizontal,
                                              gint x, gint y);

/* Cada vista que muestra el ítem declara a qué tamaño lo dibuja
 * (@requester la identifica); se usa el mayor de los pedidos */
void            tray_item_request_size       (TrayItem *item, gpointer requester, gint pixels);
void            tray_item_release_size       (TrayItem *item, gpointer requester);

/* ── API para backends ───────────────────────────────────────────────── */

void tray_item_set_key             (TrayItem *item, const gchar *key);
void tray_item_set_title           (TrayItem *item, const gchar *title);
void tray_item_set_status          (TrayItem *item, TrayItemStatus status);
void tray_item_set_is_menu         (TrayItem *item, gboolean is_menu);
void tray_item_set_is_legacy       (TrayItem *item, gboolean is_legacy);
/* @pixmaps: GPtrArray de GdkPixbuf (transfer full), puede ser NULL */
void tray_item_set_icon            (TrayItem         *item,
                                    TrayItemIconRole  role,
                                    const gchar      *icon_name,
                                    GPtrArray        *pixmaps);
void tray_item_set_icon_theme_path (TrayItem *item, const gchar *path);
void tray_item_set_tooltip         (TrayItem    *item,
                                    const gchar *title,
                                    const gchar *text);
/* Emite "changed" una vez tras una tanda de setters */
void tray_item_emit_changed        (TrayItem *item);
/* La app rechazó Activate: la vista debe mostrar el menú en su lugar.
 * Emite "activate-fallback". */
void tray_item_emit_activate_fallback (TrayItem *item);

G_END_DECLS
