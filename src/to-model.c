/*
 * Tray Overflow — plugin de bandeja para xfce4-panel
 * Copyright (C) 2026 Juan Pérez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "to-common.h"
#include "to-model.h"
#include "to-sni.h"

struct _TrayModel
{
  GObject    parent_instance;
  GPtrArray *items;
  gboolean   disposing;
};

G_DEFINE_TYPE (TrayModel, tray_model, G_TYPE_OBJECT)

enum
{
  SIGNAL_ITEM_ADDED,
  SIGNAL_ITEM_REMOVED,
  N_SIGNALS
};

static guint      signals[N_SIGNALS];
static TrayModel *default_model = NULL;



static void
tray_model_dispose (GObject *object)
{
  TrayModel *model = TRAY_MODEL (object);

  if (!model->disposing)
    {
      model->disposing = TRUE;
      to_sni_backend_stop ();
    }

  G_OBJECT_CLASS (tray_model_parent_class)->dispose (object);
}



static void
tray_model_finalize (GObject *object)
{
  TrayModel *model = TRAY_MODEL (object);

  g_ptr_array_unref (model->items);
  if (default_model == model)
    default_model = NULL;

  G_OBJECT_CLASS (tray_model_parent_class)->finalize (object);
}



static void
tray_model_class_init (TrayModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = tray_model_dispose;
  object_class->finalize = tray_model_finalize;

  signals[SIGNAL_ITEM_ADDED] =
    g_signal_new ("item-added", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, TRAY_TYPE_ITEM);

  signals[SIGNAL_ITEM_REMOVED] =
    g_signal_new ("item-removed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, TRAY_TYPE_ITEM);
}



static void
tray_model_init (TrayModel *model)
{
  model->items = g_ptr_array_new_with_free_func (g_object_unref);
}



TrayModel *
tray_model_ref_default (void)
{
  if (default_model != NULL)
    return g_object_ref (default_model);

  default_model = g_object_new (TRAY_TYPE_MODEL, NULL);
  to_sni_backend_start (default_model);

  return default_model;
}



GPtrArray *
tray_model_get_items (TrayModel *model)
{
  g_return_val_if_fail (TRAY_IS_MODEL (model), NULL);
  return model->items;
}



gboolean
tray_model_has_item (TrayModel *model, TrayItem *item)
{
  g_return_val_if_fail (TRAY_IS_MODEL (model), FALSE);
  return g_ptr_array_find (model->items, item, NULL);
}



void
tray_model_add_item (TrayModel *model, TrayItem *item)
{
  g_return_if_fail (TRAY_IS_MODEL (model));
  g_return_if_fail (TRAY_IS_ITEM (item));

  if (model->disposing || g_ptr_array_find (model->items, item, NULL))
    return;

  g_ptr_array_add (model->items, g_object_ref (item));
  g_signal_emit (model, signals[SIGNAL_ITEM_ADDED], 0, item);
}



void
tray_model_remove_item (TrayModel *model, TrayItem *item)
{
  guint index;

  g_return_if_fail (TRAY_IS_MODEL (model));

  if (!g_ptr_array_find (model->items, item, &index))
    return;

  /* Mantener vivo el ítem mientras se notifica */
  g_object_ref (item);
  g_ptr_array_remove_index (model->items, index);
  if (!model->disposing)
    g_signal_emit (model, signals[SIGNAL_ITEM_REMOVED], 0, item);
  g_object_unref (item);
}
