/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_weight_paint.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"
#include "BKE_deform.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curves.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_paint.hh"
#include "ED_image.hh"
#include "ED_object_vgroup.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"

#include "../sculpt_paint/paint_intern.hh"
#include "curves_weight_paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Paint Mode Data Wrapper
 * \{ */

class PaintModeDataWrapper : public PaintModeData {
 private:
  std::unique_ptr<CurvesWeightPaintStrokeOperation> operation_;

 public:
  PaintModeDataWrapper(std::unique_ptr<CurvesWeightPaintStrokeOperation> operation)
      : operation_(std::move(operation))
  {
  }

  CurvesWeightPaintStrokeOperation *operation()
  {
    return operation_.get();
  }
};

/* -------------------------------------------------------------------- */
/** \name Weight Paint Mode Toggle
 * \{ */

static void curves_weight_paint_mode_enter(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  Object *ob = CTX_data_active_object(C);
  
  /* Ensure weight paint data exists */
  BKE_paint_ensure(scene->toolsettings, (Paint **)&scene->toolsettings->curves_weight_paint);
  CurvesWeightPaint *curves_weight_paint = scene->toolsettings->curves_weight_paint;
  
  /* Set object mode */
  ob->mode = OB_MODE_WEIGHT_CURVES;
  
  /* Set paint mode */
  Paint *paint = BKE_paint_get_active_from_paintmode(scene, PaintMode::WeightCurves);
  
  /* Ensure brushes exist */
  BKE_paint_brushes_ensure(CTX_data_main(C), paint);
  
  /* Start paint cursor */
  ED_paint_cursor_start(&curves_weight_paint->paint, 
                        curves_weight_paint_poll);
  paint_init_pivot(ob, scene, paint);
  
  /* Ensure deform verts exist for curves */
  bke::curves::ensure_deform_verts(ob);
  
  /* Ensure at least one vertex group exists */
  if (BLI_listbase_is_empty(&ob->defbase)) {
    printf("[DEBUG] Curves Weight Paint: No vertex groups found, creating default 'Group'\n");
    bDeformGroup *defgroup = BKE_object_defgroup_add_name(ob, "Group");
    if (defgroup) {
      const int defgroup_index = 0;  /* First vertex group has index 0 */
      BKE_object_defgroup_active_index_set(ob, 1);
      
      /* Initialize all points with weight 1.0 for the new vertex group */
      const int total_points = bke::curves::get_curves_vertex_count(ob);
      printf("[DEBUG] Curves Weight Paint: Initializing %d points with weight 1.0\n", total_points);
      for (int i = 0; i < total_points; i++) {
        /* Use WEIGHT_REPLACE mode (value = 1) */
        bke::curves::set_vertex_group_weight(ob, i, defgroup_index, 1.0f, 1);
      }
      
      DEG_relations_tag_update(CTX_data_main(C));
      printf("[DEBUG] Curves Weight Paint: Created vertex group 'Group' and initialized weights to 1.0\n");
    }
    else {
      printf("[ERROR] Curves Weight Paint: Failed to create default vertex group\n");
    }
  }
  else {
    /* Ensure there's an active vertex group */
    const int active_index = BKE_object_defgroup_active_index_get(ob);
    printf("[DEBUG] Curves Weight Paint: Found %d vertex groups, active index: %d\n", 
           BLI_listbase_count(&ob->defbase), active_index);
    if (active_index == 0) {
      BKE_object_defgroup_active_index_set(ob, 1);
      printf("[DEBUG] Curves Weight Paint: Set first vertex group as active\n");
    }
  }
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
}

static void curves_weight_paint_mode_exit(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  
  /* Set object mode back to object */
  ob->mode = OB_MODE_OBJECT;
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
}

static bool curves_weight_paint_toggle_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_CURVES && ob->data;
}

static wmOperatorStatus curves_weight_paint_toggle_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  
  const bool is_mode_set = ob->mode == OB_MODE_WEIGHT_CURVES;
  
  if (!is_mode_set) {
    if (!blender::ed::object::mode_compat_set(C, ob, OB_MODE_WEIGHT_CURVES, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }
  
  if (is_mode_set) {
    curves_weight_paint_mode_exit(C);
  }
  else {
    curves_weight_paint_mode_enter(C);
  }
  
  WM_toolsystem_update_from_context_view3d(C);
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  
  return OPERATOR_FINISHED;
}

void CURVES_OT_weight_paint_toggle(wmOperatorType *ot)
{
  ot->name = "Curves Weight Paint Mode";
  ot->idname = "CURVES_OT_weight_paint_toggle";
  ot->description = "Toggle curves weight paint mode in 3D view";

  ot->exec = curves_weight_paint_toggle_exec;
  ot->poll = curves_weight_paint_toggle_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Add Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  Curves *curves = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  
  /* Ensure we have deform verts */
  bke::curves::ensure_deform_verts(ob);
  
  /* Add vertex group to object */
  bDeformGroup *defgroup = BKE_object_defgroup_add(ob);
  if (!defgroup) {
    BKE_report(op->reports, RPT_ERROR, "Could not add vertex group");
    return OPERATOR_CANCELLED;
  }

  /* Set as active group */
  const int defgroup_index = BLI_listbase_count(&ob->defbase) - 1;
  BKE_object_defgroup_active_index_set(ob, defgroup_index + 1);
  
  /* Initialize all points with weight 1.0 for the new vertex group */
  const int total_points = bke::curves::get_curves_vertex_count(ob);
  for (int i = 0; i < total_points; i++) {
    /* Use WEIGHT_REPLACE mode (value = 1) */
    bke::curves::set_vertex_group_weight(ob, i, defgroup_index, 1.0f, 1);
  }

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(CTX_data_main(C));

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_add(wmOperatorType *ot)
{
  ot->name = "Add Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_add";
  ot->description = "Add a new vertex group to the active curves object";

  ot->exec = curves_vertex_group_add_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Remove Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  bDeformGroup *defgroup = static_cast<bDeformGroup *>(BLI_findlink(&ob->defbase, 
                                                                    BKE_object_defgroup_active_index_get(ob) - 1));
  if (!defgroup) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group to remove");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  
  /* Remove weights from all points for this group */
  if (bke::curves::has_deform_verts(ob)) {
    const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
    const int total_points = bke::curves::get_curves_vertex_count(ob);
    
    for (int i = 0; i < total_points; i++) {
      bke::curves::remove_vertex_from_group(ob, i, defgroup_index);
    }
  }

  /* Remove vertex group from object */
  BKE_object_defgroup_remove(ob, defgroup);

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(CTX_data_main(C));

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_remove(wmOperatorType *ot)
{
  ot->name = "Remove Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_remove";
  ot->description = "Remove the active vertex group from the curves object";

  ot->exec = curves_vertex_group_remove_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Assign Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_assign_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
  if (defgroup_index < 0) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  
  /* Ensure we have deform verts */
  bke::curves::ensure_deform_verts(ob);
  
  /* Get weight value */
  const float weight = RNA_float_get(op->ptr, "weight");
  
  /* Assign weight to selected points */
  const int total_points = bke::curves::get_curves_vertex_count(ob);
  
  /* For now, assign to all points - later we'll add selection support */
  for (int i = 0; i < total_points; i++) {
    /* Use WEIGHT_REPLACE mode (value = 1) */
    bke::curves::set_vertex_group_weight(ob, i, defgroup_index, weight, 1);
  }

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_assign(wmOperatorType *ot)
{
  ot->name = "Assign Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_assign";
  ot->description = "Assign selected points to the active vertex group";

  ot->exec = curves_vertex_group_assign_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  
  RNA_def_float(ot->srna, "weight", 1.0f, 0.0f, 1.0f, "Weight", "Weight to assign", 0.0f, 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Remove From Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_remove_from_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
  if (defgroup_index < 0) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  
  if (!bke::curves::has_deform_verts(ob)) {
    return OPERATOR_CANCELLED;
  }
  
  /* Remove from selected points */
  const int total_points = bke::curves::get_curves_vertex_count(ob);
  
  /* For now, remove from all points - later we'll add selection support */
  for (int i = 0; i < total_points; i++) {
    bke::curves::remove_vertex_from_group(ob, i, defgroup_index);
  }

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_remove_from(wmOperatorType *ot)
{
  ot->name = "Remove from Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_remove_from";
  ot->description = "Remove selected points from the active vertex group";

  ot->exec = curves_vertex_group_remove_from_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Stroke Operators
 * \{ */

static std::unique_ptr<CurvesWeightPaintStrokeOperation> start_stroke_operation(
    const BrushStrokeMode brush_mode, const bContext &C)
{
  const Scene *scene = CTX_data_scene(&C);
  const Object *object = CTX_data_active_object(&C);
  if (!object || object->type != OB_CURVES) {
    return nullptr;
  }

  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (!brush) {
    return nullptr;
  }

  if (brush_mode == BRUSH_STROKE_SMOOTH) {
    return new_weight_paint_blur_operation();
  }

  switch (eBrushWeightPaintType(brush->weight_brush_type)) {
    case WPAINT_BRUSH_TYPE_DRAW:
      return new_weight_paint_draw_operation(brush_mode);
    case WPAINT_BRUSH_TYPE_BLUR:
      return new_weight_paint_blur_operation();
    case WPAINT_BRUSH_TYPE_AVERAGE:
      return new_weight_paint_average_operation();
    case WPAINT_BRUSH_TYPE_SMEAR:
      return new_weight_paint_smear_operation();
  }
  
  return nullptr;
}

static bool stroke_get_location(bContext *C,
                                float out[3],
                                const float mouse[2],
                                bool /*force_original*/)
{
  out[0] = mouse[0];
  out[1] = mouse[1];
  out[2] = 0.0f;
  return true;
}

static bool stroke_test_start(bContext *C, wmOperator *op, const float mouse[2])
{
  UNUSED_VARS(C, op, mouse);
  return true;
}

static void stroke_update_step(bContext *C,
                               wmOperator * /*op*/,
                               PaintStroke *stroke,
                               PointerRNA *stroke_element)
{
  StrokeExtension stroke_extension;
  RNA_float_get_array(stroke_element, "mouse", &stroke_extension.mouse_position[0]);
  stroke_extension.pressure = RNA_float_get(stroke_element, "pressure");
  stroke_extension.is_first = !paint_stroke_started(stroke);

  CurvesWeightPaintStrokeOperation *operation = static_cast<PaintModeDataWrapper *>(
      paint_stroke_mode_data(stroke))->operation();

  /* Call on_stroke_begin for the first sample of the stroke. */
  if (stroke_extension.is_first) {
    operation->on_stroke_begin(*C, stroke_extension);
  }

  operation->on_stroke_extended(*C, stroke_extension);
}

static void stroke_done(const bContext *C, PaintStroke *stroke)
{
  PaintModeDataWrapper *wrapper = static_cast<PaintModeDataWrapper *>(
      paint_stroke_mode_data(stroke));

  /* Call on_stroke_done to finalize the operation. */
  if (wrapper && wrapper->operation()) {
    wrapper->operation()->on_stroke_done(*C);
  }

  delete wrapper;
}

static bool curves_weight_paint_brush_stroke_poll(bContext *C)
{
  if (!curves_weight_paint_poll(C)) {
    return false;
  }
  if (!WM_toolsystem_active_tool_is_brush(C)) {
    return false;
  }
  return true;
}

static wmOperatorStatus curves_weight_paint_brush_stroke_invoke(bContext *C,
                                                                wmOperator *op,
                                                                const wmEvent *event)
{
  const Scene *scene = CTX_data_scene(C);
  const Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  Curves &curves = *static_cast<Curves *>(object->data);
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int active_defgroup_nr = BKE_object_defgroup_active_index_get(object) - 1;
  if (active_defgroup_nr >= 0 && BKE_object_defgroup_active_is_locked(object)) {
    BKE_report(op->reports, RPT_WARNING, "Active group is locked, aborting");
    return OPERATOR_CANCELLED;
  }

  const BrushStrokeMode brush_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));
  std::unique_ptr<CurvesWeightPaintStrokeOperation> operation = start_stroke_operation(
      brush_mode, *C);
  if (!operation) {
    return OPERATOR_CANCELLED;
  }

  op->customdata = paint_stroke_new(C,
                                    op,
                                    stroke_get_location,
                                    stroke_test_start,
                                    stroke_update_step,
                                    nullptr,
                                    stroke_done,
                                    event->type);

  paint_stroke_set_mode_data(static_cast<PaintStroke *>(op->customdata), 
                             std::make_unique<PaintModeDataWrapper>(std::move(operation)));

  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  OPERATOR_RETVAL_CHECK(paint_stroke_exec(C, op, static_cast<PaintStroke *>(op->customdata)));

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus curves_weight_paint_brush_stroke_modal(bContext *C,
                                                               wmOperator *op,
                                                               const wmEvent *event)
{
  return paint_stroke_modal(C, op, event, reinterpret_cast<PaintStroke **>(&op->customdata));
}

static void curves_weight_paint_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  paint_stroke_cancel(C, op, static_cast<PaintStroke *>(op->customdata));
}

static void CURVES_OT_weight_paint_brush_stroke(wmOperatorType *ot)
{
  ot->name = "Curves Weight Paint Brush Stroke";
  ot->idname = "CURVES_OT_weight_paint_brush_stroke";
  ot->description = "Paint weight on curves points";

  ot->poll = curves_weight_paint_brush_stroke_poll;
  ot->invoke = curves_weight_paint_brush_stroke_invoke;
  ot->modal = curves_weight_paint_brush_stroke_modal;
  ot->cancel = curves_weight_paint_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}

/** \} */

}  // namespace blender::ed::sculpt_paint

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_operatortypes_curves_weight_paint()
{
  using namespace blender::ed::sculpt_paint;
  WM_operatortype_append(CURVES_OT_weight_paint_toggle);
  WM_operatortype_append(CURVES_OT_vertex_group_add);
  WM_operatortype_append(CURVES_OT_vertex_group_remove);
  WM_operatortype_append(CURVES_OT_vertex_group_assign);
  WM_operatortype_append(CURVES_OT_vertex_group_remove_from);
  WM_operatortype_append(CURVES_OT_weight_paint_brush_stroke);
}