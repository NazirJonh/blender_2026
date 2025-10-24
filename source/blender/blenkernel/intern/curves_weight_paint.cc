/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Functions for weight painting on curves.
 */

#include "BKE_curves_weight_paint.hh"

#include "MEM_guardedalloc.h"

#include "DNA_curves_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_object_deform.h"

namespace blender::bke::curves {

void ensure_deform_verts(Object *ob)
{
  if (ob->type != OB_CURVES) {
    return;
  }

  Curves *curves_id = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  /* Check if deform verts already exist. */
  if (!curves.deform_verts().is_empty()) {
    return;
  }

  const int points_num = curves.points_num();
  if (points_num == 0) {
    return;
  }

  /* Use the proper CurvesGeometry method to create deform verts. */
  curves.deform_verts_for_write();
}

float get_vertex_group_weight(const Object *ob, int vertex_index, int def_nr)
{
  if (ob->type != OB_CURVES) {
    return -1.0f;
  }

  const Curves *curves_id = static_cast<const Curves *>(ob->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const Span<MDeformVert> dverts = curves.deform_verts();
  if (dverts.is_empty() || vertex_index < 0 || vertex_index >= dverts.size()) {
    return -1.0f;
  }

  const MDeformVert *dv = &dverts[vertex_index];
  const MDeformWeight *dw = BKE_defvert_find_index(dv, def_nr);

  return dw ? dw->weight : 0.0f;
}

void set_vertex_group_weight(Object *ob, int vertex_index, int def_nr, float weight, int assign_mode)
{
  if (ob->type != OB_CURVES) {
    return;
  }

  /* Ensure deform verts exist. */
  ensure_deform_verts(ob);

  Curves *curves_id = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const MutableSpan<MDeformVert> dverts = curves.deform_verts_for_write();
  if (dverts.is_empty() || vertex_index < 0 || vertex_index >= dverts.size()) {
    return;
  }

  MDeformVert *dv = &dverts[vertex_index];
  MDeformWeight *dw = BKE_defvert_find_index(dv, def_nr);

  /* Clamp weight to valid range. */
  weight = std::clamp(weight, 0.0f, 1.0f);

  if (dw) {
    /* Vertex is already in the group, update weight. */
    switch (assign_mode) {
      case WEIGHT_REPLACE:
        dw->weight = weight;
        break;
      case WEIGHT_ADD:
        dw->weight = std::min(dw->weight + weight, 1.0f);
        break;
      case WEIGHT_SUBTRACT:
        dw->weight -= weight;
        if (dw->weight <= 0.0f) {
          BKE_defvert_remove_group(dv, dw);
        }
        break;
    }
  }
  else if (weight > 0.0f) {
    /* Add vertex to group if weight is positive. */
    switch (assign_mode) {
      case WEIGHT_REPLACE:
      case WEIGHT_ADD:
        BKE_defvert_add_index_notest(dv, def_nr, weight);
        break;
      case WEIGHT_SUBTRACT:
        /* Don't add for subtract mode. */
        break;
    }
  }
}

void normalize_point_weights(Object *ob, int vertex_index, bool lock_active, bool auto_normalize)
{
  if (ob->type != OB_CURVES || !auto_normalize) {
    return;
  }

  Curves *curves_id = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const MutableSpan<MDeformVert> dverts = curves.deform_verts_for_write();
  if (dverts.is_empty() || vertex_index < 0 || vertex_index >= dverts.size()) {
    return;
  }

  MDeformVert *dv = &dverts[vertex_index];

  /* Get active vertex group index. */
  const int active_def_nr = BKE_object_defgroup_active_index_get(ob) - 1;

  /* Create lock flags for normalization. */
  Vector<bool> lock_flags(dv->totweight, false);
  Vector<bool> validmap(dv->totweight, true);

  if (lock_active && active_def_nr >= 0) {
    for (int i = 0; i < dv->totweight; i++) {
      if (dv->dw[i].def_nr == active_def_nr) {
        lock_flags[i] = true;
        break;
      }
    }
  }

  /* Normalize weights. */
  BKE_defvert_normalize_ex(*dv, validmap.as_span(), lock_flags.as_span(), {});
}

void remove_vertex_from_group(Object *ob, int vertex_index, int def_nr)
{
  if (ob->type != OB_CURVES) {
    return;
  }

  Curves *curves_id = static_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const MutableSpan<MDeformVert> dverts = curves.deform_verts_for_write();
  if (dverts.is_empty() || vertex_index < 0 || vertex_index >= dverts.size()) {
    return;
  }

  MDeformVert *dv = &dverts[vertex_index];
  MDeformWeight *dw = BKE_defvert_find_index(dv, def_nr);

  if (dw) {
    BKE_defvert_remove_group(dv, dw);
  }
}

int get_curves_vertex_count(const Object *ob)
{
  if (ob->type != OB_CURVES) {
    return 0;
  }

  const Curves *curves_id = static_cast<const Curves *>(ob->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  return curves.points_num();
}

bool has_deform_verts(const Object *ob)
{
  if (ob->type != OB_CURVES) {
    return false;
  }

  const Curves *curves_id = static_cast<const Curves *>(ob->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  return !curves.deform_verts().is_empty();
}

}  // namespace blender::bke::curves