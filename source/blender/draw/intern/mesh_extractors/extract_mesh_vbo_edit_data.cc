/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_meshdata_types.h"

#include "extract_mesh.hh"

#include "draw_cache_impl.hh"

#include "draw_subdivision.hh"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include <set>

namespace blender::draw {

static void mesh_render_data_edge_flag(const MeshRenderData &mr,
                                       const BMEdge *eed,
                                       EditLoopData &eattr)
{
  const ToolSettings *ts = mr.toolsettings;
  const bool is_vertex_select_mode = (ts != nullptr) && (ts->selectmode & SCE_SELECT_VERTEX) != 0;
  const bool is_face_only_select_mode = (ts != nullptr) && (ts->selectmode == SCE_SELECT_FACE);

  if (eed == mr.eed_act) {
    eattr.e_flag |= VFLAG_EDGE_ACTIVE;
  }
  if (!is_vertex_select_mode && BM_elem_flag_test(eed, BM_ELEM_SELECT)) {
    eattr.e_flag |= VFLAG_EDGE_SELECTED;
  }
  if (is_vertex_select_mode && BM_elem_flag_test(eed->v1, BM_ELEM_SELECT) &&
      BM_elem_flag_test(eed->v2, BM_ELEM_SELECT))
  {
    eattr.e_flag |= VFLAG_EDGE_SELECTED;
    eattr.e_flag |= VFLAG_VERT_SELECTED;
  }
  if (BM_elem_flag_test(eed, BM_ELEM_SEAM)) {
    eattr.e_flag |= VFLAG_EDGE_SEAM;
  }
  if (!BM_elem_flag_test(eed, BM_ELEM_SMOOTH)) {
    eattr.e_flag |= VFLAG_EDGE_SHARP;
  }

  /* Use active edge color for active face edges because
   * specular highlights make it hard to see #55456#510873.
   *
   * This isn't ideal since it can't be used when mixing edge/face modes
   * but it's still better than not being able to see the active face. */
  if (is_face_only_select_mode) {
    if (mr.efa_act != nullptr) {
      if (BM_edge_in_face(eed, mr.efa_act)) {
        eattr.e_flag |= VFLAG_EDGE_ACTIVE;
      }
    }
  }

  /* Use half a byte for value range */
  if (mr.edge_crease_ofs != -1) {
    float crease = BM_ELEM_CD_GET_FLOAT(eed, mr.edge_crease_ofs);
    if (crease > 0) {
      eattr.crease = uchar(ceil(crease * 15.0f));
    }
  }
  /* Use a byte for value range */
  if (mr.bweight_ofs != -1) {
    float bweight = BM_ELEM_CD_GET_FLOAT(eed, mr.bweight_ofs);
    if (bweight > 0) {
      eattr.bweight = uchar(bweight * 255.0f);
    }
  }
#ifdef WITH_FREESTYLE
  if (mr.freestyle_edge_ofs != -1) {
    if (BM_ELEM_CD_GET_BOOL(eed, mr.freestyle_edge_ofs)) {
      eattr.e_flag |= VFLAG_EDGE_FREESTYLE;
    }
  }
#endif
}

static void mesh_render_data_vert_flag(const MeshRenderData &mr,
                                       const BMVert *eve,
                                       EditLoopData &eattr)
{
  if (eve == mr.eve_act) {
    eattr.e_flag |= VFLAG_VERT_ACTIVE;
  }
  if (BM_elem_flag_test(eve, BM_ELEM_SELECT)) {
    eattr.e_flag |= VFLAG_VERT_SELECTED;
  }
  /* Use half a byte for value range */
  if (mr.vert_crease_ofs != -1) {
    float crease = BM_ELEM_CD_GET_FLOAT(eve, mr.vert_crease_ofs);
    if (crease > 0) {
      eattr.crease |= uchar(ceil(crease * 15.0f)) << 4;
    }
  }
}

static const GPUVertFormat &get_edit_data_format()
{
  static const GPUVertFormat format = []() {
    GPUVertFormat format{};
    /* WARNING: Adjust #EditLoopData struct accordingly. */
    GPU_vertformat_attr_add(&format, "data", gpu::VertAttrType::UINT_8_8_8_8);
    GPU_vertformat_alias_add(&format, "flag");
    GPU_vertformat_attr_add(&format, "fset_color", gpu::VertAttrType::UNORM_8_8_8_8);
    GPU_vertformat_attr_add(&format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();
  return format;
}

static void extract_edit_data_mesh(const MeshRenderData &mr, MutableSpan<EditLoopData> vbo_data)
{
  MutableSpan corners_data = vbo_data.take_front(mr.corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const OffsetIndices faces = mr.faces;
  const Span<int> corner_verts = mr.corner_verts;
  const Span<int> corner_edges = mr.corner_edges;
  
  /* Extract face set data for face set colors */
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const VArraySpan<int> face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
  const int face_set_seed = mr.mesh->face_sets_color_seed;
  const int face_set_default = mr.mesh->face_sets_color_default;
  
  /* DEBUG: Face Sets extraction - detailed diagnostics */
  static bool debug_printed = false;
  if (!debug_printed) {
    printf("=== DEBUG Face Sets: VBO Extraction Started ===\n");
    printf("DEBUG Face Sets: Mesh has %lld faces\n", (long long)faces.size());
    printf("DEBUG Face Sets: face_sets.is_empty()=%d\n", face_sets.is_empty());
    printf("DEBUG Face Sets: face_set_seed=%d, face_set_default=%d\n", 
           face_set_seed, face_set_default);
    
    if (!face_sets.is_empty()) {
      printf("DEBUG Face Sets: Face sets found! Sample values:\n");
      for (int i = 0; i < std::min((int)face_sets.size(), 10); i++) {
        printf("  Face %d: face_set_id=%d\n", i, face_sets[i]);
      }
      if (face_sets.size() > 10) {
        printf("  ... and %lld more faces\n", (long long)(face_sets.size() - 10));
      }
    } else {
      printf("DEBUG Face Sets: NO FACE SETS FOUND! Mesh has no .sculpt_face_set attribute\n");
    }
    printf("=== DEBUG Face Sets: VBO Extraction Info Complete ===\n");
    debug_printed = true;
  }
  
  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int face : range) {
      for (const int corner : faces[face]) {
        EditLoopData &value = corners_data[corner];
        value = {};
        if (const BMFace *bm_face = bm_original_face_get(mr, face)) {
          mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, value);
        }
        if (const BMVert *bm_vert = bm_original_vert_get(mr, corner_verts[corner])) {
          mesh_render_data_vert_flag(mr, bm_vert, value);
        }
        if (const BMEdge *bm_edge = bm_original_edge_get(mr, corner_edges[corner])) {
          mesh_render_data_edge_flag(mr, bm_edge, value);
        }
        
        /* Extract face set color */
        if (!face_sets.is_empty()) {
          const int face_set_id = face_sets[face];
          if (face_set_id != face_set_default) {
            BKE_paint_face_set_overlay_color_get(face_set_id, face_set_seed, value.face_set_color);
            /* Set alpha based on face sets opacity setting */
            /* For now, use full opacity - shader will apply face_sets_opacity */
            value.face_set_color[3] = 255;  /* Full opacity in VBO, shader handles opacity */
          } else {
            /* Default face set should be transparent (no face set assigned) */
            value.face_set_color = uchar4(0, 0, 0, 0);  /* transparent for default face set */
          }
        } else {
          /* No face sets at all - make transparent */
          value.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
        }
        
        /* Extract normal for shading */
        if (const BMFace *bm_face = bm_original_face_get(mr, face)) {
          const float *face_normal = bm_face_no_get(mr, bm_face);
          value.nor = float3(face_normal[0], face_normal[1], face_normal[2]);
        } else {
          value.nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
        }
      }
    }
  });

  const Span<int2> edges = mr.edges;
  const Span<int> loose_edges = mr.loose_edges;
  threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      EditLoopData &value_1 = loose_edge_data[i * 2 + 0];
      EditLoopData &value_2 = loose_edge_data[i * 2 + 1];
      if (const BMEdge *bm_edge = bm_original_edge_get(mr, loose_edges[i])) {
        value_1 = {};
        mesh_render_data_edge_flag(mr, bm_edge, value_1);
        value_2 = value_1;
      }
      else {
        value_2 = value_1 = {};
      }
      const int2 edge = edges[loose_edges[i]];
      if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[0])) {
        mesh_render_data_vert_flag(mr, bm_vert, value_1);
      }
      if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[1])) {
        mesh_render_data_vert_flag(mr, bm_vert, value_2);
      }
      
      /* Initialize face set color for loose edges */
      value_1.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      value_2.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
    }
  });

  const Span<int> loose_verts = mr.loose_verts;
  threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      loose_vert_data[i] = {};
      if (const BMVert *eve = bm_original_vert_get(mr, loose_verts[i])) {
        mesh_render_data_vert_flag(mr, eve, loose_vert_data[i]);
      }
      /* Initialize face set color for loose verts */
      loose_vert_data[i].face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      
      /* Initialize normals for loose verts */
      loose_vert_data[i].nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
    }
  });
}

static void extract_edit_data_bm(const MeshRenderData &mr, MutableSpan<EditLoopData> vbo_data)
{
  MutableSpan corners_data = vbo_data.take_front(mr.corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  const BMesh &bm = *mr.bm;
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  
  /* Get face set data from BMesh */
  const int face_set_offset = CustomData_get_offset_named(
      &bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
  const int face_set_seed = mr.mesh->face_sets_color_seed;
  const int face_set_default = mr.mesh->face_sets_color_default;

  /* DEBUG: BMesh Face Sets extraction - detailed diagnostics */
  static bool bm_debug_printed = false;
  if (!bm_debug_printed) {
    printf("=== DEBUG Face Sets: BMesh VBO Extraction Started ===\n");
    printf("DEBUG Face Sets: BMesh has %d faces\n", bm.totface);
    printf("DEBUG Face Sets: face_set_offset=%d (offset in CustomData)\n", face_set_offset);
    printf("DEBUG Face Sets: face_set_seed=%d, face_set_default=%d\n", 
           face_set_seed, face_set_default);
    
              if (face_set_offset != -1) {
                printf("DEBUG Face Sets: Face sets found in BMesh! Sample values:\n");
                printf("DEBUG Face Sets: CustomData layer info:\n");
                printf("  - Layer name: %s\n", CustomData_get_layer_name(&bm.pdata, CD_PROP_INT32, face_set_offset));
                
                /* DEBUG: List all CustomData layers to see what's available */
                printf("DEBUG Face Sets: All CustomData layers in BMesh:\n");
                for (int i = 0; i < bm.pdata.totlayer; i++) {
                  CustomDataLayer *layer = &bm.pdata.layers[i];
                  printf("  Layer %d: name='%s', type=%d, offset=%d\n", 
                         i, layer->name, layer->type, layer->offset);
                }
                
                /* DEBUG: Check ALL faces for different face set IDs */
                std::set<int> unique_face_set_ids;
                for (int i = 0; i < bm.totface; i++) {
                  const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), i);
                  int face_set_id = BM_ELEM_CD_GET_INT(&face, face_set_offset);
                  unique_face_set_ids.insert(face_set_id);
                }
                
                printf("DEBUG Face Sets: Found %d unique face set IDs: ", (int)unique_face_set_ids.size());
                for (int id : unique_face_set_ids) {
                  printf("%d ", id);
                }
                printf("\n");
                
                /* DEBUG: Show sample faces for each unique ID */
                for (int id : unique_face_set_ids) {
                  int count = 0;
                  for (int i = 0; i < bm.totface && count < 3; i++) {
                    const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), i);
                    int face_set_id = BM_ELEM_CD_GET_INT(&face, face_set_offset);
                    if (face_set_id == id) {
                      printf("  Face %d: face_set_id=%d\n", i, face_set_id);
                      count++;
                    }
                  }
                }
              } else {
                printf("DEBUG Face Sets: NO FACE SETS FOUND in BMesh! No .sculpt_face_set CustomData layer\n");
                /* DEBUG: List all CustomData layers to see what's available */
                printf("DEBUG Face Sets: All CustomData layers in BMesh:\n");
                for (int i = 0; i < bm.pdata.totlayer; i++) {
                  CustomDataLayer *layer = &bm.pdata.layers[i];
                  printf("  Layer %d: name='%s', type=%d, offset=%d\n", 
                         i, layer->name, layer->type, layer->offset);
                }
              }
    printf("=== DEBUG Face Sets: BMesh VBO Extraction Info Complete ===\n");
    bm_debug_printed = true;
  }

  threading::parallel_for(IndexRange(bm.totface), 2048, [&](const IndexRange range) {
    for (const int face_index : range) {
      const BMFace &face = *BM_face_at_index(&const_cast<BMesh &>(bm), face_index);
      const BMLoop *loop = BM_FACE_FIRST_LOOP(&face);
      
      /* Get face set ID for this face */
      int face_set_id = face_set_default;
      if (face_set_offset != -1) {
        face_set_id = BM_ELEM_CD_GET_INT(&face, face_set_offset);
      }
      
      for ([[maybe_unused]] const int i : IndexRange(face.len)) {
        const int index = BM_elem_index_get(loop);
        EditLoopData &value = corners_data[index];
        value = {};
        mesh_render_data_face_flag(mr, &face, uv_offsets_none, corners_data[index]);
        mesh_render_data_edge_flag(mr, loop->e, corners_data[index]);
        mesh_render_data_vert_flag(mr, loop->v, corners_data[index]);
        
                  /* Extract face set color */
                  if (face_set_id != face_set_default) {
                    BKE_paint_face_set_overlay_color_get(face_set_id, face_set_seed, value.face_set_color);
                    /* Set alpha based on face sets opacity setting */
                    /* For now, use full opacity - shader will apply face_sets_opacity */
                    value.face_set_color[3] = 255;  /* Full opacity in VBO, shader handles opacity */
                    /* DEBUG: Print color generation for non-default face sets - only for first few unique IDs */
                    static std::set<int> printed_face_set_ids;
                    if (printed_face_set_ids.find(face_set_id) == printed_face_set_ids.end() && printed_face_set_ids.size() < 5) {
                      printf("DEBUG Face Sets: Generated color for face_set_id=%d: RGB(%d,%d,%d) (seed=%d)\n", 
                             face_set_id, value.face_set_color[0], value.face_set_color[1], value.face_set_color[2], face_set_seed);
                      printed_face_set_ids.insert(face_set_id);
                    }
                  } else {
                    value.face_set_color = uchar4(0, 0, 0, 0);  /* transparent for default face set */
                  }
                  
                  /* DEBUG: Print VBO data for faces with different face set IDs */
                  static std::set<int> printed_vbo_face_set_ids;
                  if (printed_vbo_face_set_ids.find(face_set_id) == printed_vbo_face_set_ids.end() && printed_vbo_face_set_ids.size() < 6) {
                    printf("DEBUG Face Sets: VBO data for face %d (face_set_id=%d): face_set_color=[%d,%d,%d,%d]\n", 
                           face_index, face_set_id, value.face_set_color[0], value.face_set_color[1], value.face_set_color[2], value.face_set_color[3]);
                    printed_vbo_face_set_ids.insert(face_set_id);
                  }
        
        /* Extract normal for shading */
        const float *face_normal = bm_face_no_get(mr, &face);
        value.nor = float3(face_normal[0], face_normal[1], face_normal[2]);
        
        loop = loop->next;
      }
    }
  });

  const Span<int> loose_edges = mr.loose_edges;
  threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      EditLoopData &value_1 = loose_edge_data[i * 2 + 0];
      EditLoopData &value_2 = loose_edge_data[i * 2 + 1];
      const BMEdge &edge = *BM_edge_at_index(&const_cast<BMesh &>(bm), loose_edges[i]);
      value_1 = {};
      mesh_render_data_edge_flag(mr, &edge, value_1);
      value_2 = value_1;
      mesh_render_data_vert_flag(mr, edge.v1, value_1);
      mesh_render_data_vert_flag(mr, edge.v2, value_2);
      
      /* Initialize face set color for loose edges */
      value_1.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      value_2.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      
      /* Initialize normals for loose edges */
      value_1.nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
      value_2.nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
    }
  });

  const Span<int> loose_verts = mr.loose_verts;
  threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      loose_vert_data[i] = {};
      const BMVert &vert = *BM_vert_at_index(&const_cast<BMesh &>(bm), loose_verts[i]);
      mesh_render_data_vert_flag(mr, &vert, loose_vert_data[i]);
      
      /* Initialize face set color for loose verts */
      loose_vert_data[i].face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      
      /* Initialize normals for loose verts */
      loose_vert_data[i].nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
    }
  });
}

gpu::VertBufPtr extract_edit_data(const MeshRenderData &mr)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(get_edit_data_format()));
  const int size = mr.corners_num + mr.loose_indices_num;
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan vbo_data = vbo->data<EditLoopData>();
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_edit_data_mesh(mr, vbo_data);
  }
  else {
    extract_edit_data_bm(mr, vbo_data);
  }
  return vbo;
}

static void extract_edit_subdiv_data_mesh(const MeshRenderData &mr,
                                          const DRWSubdivCache &subdiv_cache,
                                          MutableSpan<EditLoopData> vbo_data)
{
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const int corners_num = subdiv_cache.num_subdiv_loops;
  const int loose_edges_num = mr.loose_edges.size();
  const int verts_per_edge = subdiv_verts_per_coarse_edge(subdiv_cache);
  const Span<int> subdiv_loop_face_index(subdiv_cache.subdiv_loop_face_index, corners_num);
  const Span<int> subdiv_loop_vert_index = subdiv_cache.verts_orig_index->data<int>();
  /* NOTE: #subdiv_loop_edge_index already has the origindex layer baked in. */
  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();

  MutableSpan corners_data = vbo_data.take_front(corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(corners_num, loose_edges_num * verts_per_edge);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());
  
  /* Extract face set data for face set colors */
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const VArraySpan<int> face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
  const int face_set_seed = mr.mesh->face_sets_color_seed;
  const int face_set_default = mr.mesh->face_sets_color_default;

  threading::parallel_for(IndexRange(subdiv_cache.num_subdiv_quads), 2048, [&](IndexRange range) {
    for (const int subdiv_quad : range) {
      const int coarse_face = subdiv_loop_face_index[subdiv_quad * 4];
      for (const int subdiv_corner : IndexRange(subdiv_quad * 4, 4)) {
        EditLoopData &value = corners_data[subdiv_corner];
        value = {};

        if (const BMFace *bm_face = bm_original_face_get(mr, coarse_face)) {
          mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, value);
        }

        const int vert_origindex = subdiv_loop_vert_index[subdiv_corner];
        if (vert_origindex != -1) {
          if (const BMVert *bm_vert = bm_original_vert_get(mr, vert_origindex)) {
            mesh_render_data_vert_flag(mr, bm_vert, value);
          }
        }

        const int edge_origindex = subdiv_loop_edge_index[subdiv_corner];
        if (edge_origindex != -1) {
          if (const BMEdge *bm_edge = BM_edge_at_index(mr.bm, edge_origindex)) {
            mesh_render_data_edge_flag(mr, bm_edge, value);
          }
        }
        
        /* Extract face set color */
        if (!face_sets.is_empty() && coarse_face < face_sets.size()) {
          const int face_set_id = face_sets[coarse_face];
          if (face_set_id != face_set_default) {
            BKE_paint_face_set_overlay_color_get(face_set_id, face_set_seed, value.face_set_color);
            /* Fix alpha channel - BKE_paint_face_set_overlay_color_get doesn't set alpha */
            value.face_set_color[3] = 255;  /* Set alpha to full opacity */
          } else {
            value.face_set_color = uchar4(0, 0, 0, 0);  /* transparent for default face set */
          }
        } else {
          value.face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
        }
      }
    }
  });

  const Span<int2> edges = mr.edges;
  const Span<int> loose_edges = mr.loose_edges;
  threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      MutableSpan<EditLoopData> data = loose_edge_data.slice(i * verts_per_edge, verts_per_edge);
      if (const BMEdge *edge = bm_original_edge_get(mr, loose_edges[i])) {
        EditLoopData value{};
        mesh_render_data_edge_flag(mr, edge, value);
        data.fill(value);
      }
      else {
        data.fill({});
      }
      const int2 edge = edges[loose_edges[i]];
      if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[0])) {
        mesh_render_data_vert_flag(mr, bm_vert, data.first());
      }
      if (const BMVert *bm_vert = bm_original_vert_get(mr, edge[1])) {
        mesh_render_data_vert_flag(mr, bm_vert, data.last());
      }
      
      /* Initialize face set color for loose edges */
      for (EditLoopData &value : data) {
        value.face_set_color = uchar4(255, 255, 255, 255);  /* white by default */
      }
    }
  });

  const Span<int> loose_verts = mr.loose_verts;
  threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      loose_vert_data[i] = {};
      if (const BMVert *eve = bm_original_vert_get(mr, loose_verts[i])) {
        mesh_render_data_vert_flag(mr, eve, loose_vert_data[i]);
      }
      
      /* Initialize face set color for loose verts */
      loose_vert_data[i].face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      
      /* Initialize normals for loose verts */
      loose_vert_data[i].nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
    }
  });
}

static void extract_edit_subdiv_data_bm(const MeshRenderData &mr,
                                        const DRWSubdivCache &subdiv_cache,
                                        MutableSpan<EditLoopData> vbo_data)
{
  const BMUVOffsets uv_offsets_none = BMUVOFFSETS_NONE;
  const int corners_num = subdiv_cache.num_subdiv_loops;
  const int loose_edges_num = mr.loose_edges.size();
  const int verts_per_edge = subdiv_verts_per_coarse_edge(subdiv_cache);
  const Span<int> subdiv_loop_face_index(subdiv_cache.subdiv_loop_face_index, corners_num);
  const Span<int> subdiv_loop_vert_index = subdiv_cache.verts_orig_index->data<int>();
  const Span<int> subdiv_loop_edge_index = subdiv_cache.edges_orig_index->data<int>();

  MutableSpan corners_data = vbo_data.take_front(corners_num);
  MutableSpan loose_edge_data = vbo_data.slice(corners_num, loose_edges_num * verts_per_edge);
  MutableSpan loose_vert_data = vbo_data.take_back(mr.loose_verts.size());

  BMesh &bm = *mr.bm;
  
  /* Get face set data from BMesh */
  const int face_set_offset = CustomData_get_offset_named(
      &bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
  const int face_set_seed = mr.mesh->face_sets_color_seed;
  const int face_set_default = mr.mesh->face_sets_color_default;
  
  threading::parallel_for(IndexRange(subdiv_cache.num_subdiv_quads), 2048, [&](IndexRange range) {
    for (const int subdiv_quad : range) {
      const int coarse_face = subdiv_loop_face_index[subdiv_quad * 4];
      const BMFace *bm_face = BM_face_at_index(&bm, coarse_face);
      
      /* Get face set ID for this face */
      int face_set_id = face_set_default;
      if (face_set_offset != -1) {
        face_set_id = BM_ELEM_CD_GET_INT(bm_face, face_set_offset);
      }
      
      for (const int subdiv_corner : IndexRange(subdiv_quad * 4, 4)) {
        EditLoopData &value = corners_data[subdiv_corner];
        value = {};

        mesh_render_data_face_flag(mr, bm_face, uv_offsets_none, value);

        const int vert_origindex = subdiv_loop_vert_index[subdiv_corner];
        if (vert_origindex != -1) {
          const BMVert *bm_vert = BM_vert_at_index(mr.bm, vert_origindex);
          mesh_render_data_vert_flag(mr, bm_vert, value);
        }

        const int edge_origindex = subdiv_loop_edge_index[subdiv_corner];
        if (edge_origindex != -1) {
          const BMEdge *bm_edge = BM_edge_at_index(mr.bm, edge_origindex);
          mesh_render_data_edge_flag(mr, bm_edge, value);
        }
        
        /* Extract face set color */
        if (face_set_id != face_set_default) {
          BKE_paint_face_set_overlay_color_get(face_set_id, face_set_seed, value.face_set_color);
        } else {
          value.face_set_color = uchar4(255, 255, 255, 255);  /* white for default face set */
        }
      }
    }
  });

  const Span<int> loose_edges = mr.loose_edges;
  threading::parallel_for(loose_edges.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      MutableSpan<EditLoopData> data = loose_edge_data.slice(i * verts_per_edge, verts_per_edge);
      const BMEdge *edge = BM_edge_at_index(&bm, loose_edges[i]);
      EditLoopData value{};
      mesh_render_data_edge_flag(mr, edge, value);
      data.fill(value);
      mesh_render_data_vert_flag(mr, edge->v1, data.first());
      mesh_render_data_vert_flag(mr, edge->v2, data.last());
      
      /* Initialize face set color for loose edges */
      for (EditLoopData &value : data) {
        value.face_set_color = uchar4(255, 255, 255, 255);  /* white by default */
      }
    }
  });

  const Span<int> loose_verts = mr.loose_verts;
  threading::parallel_for(loose_verts.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      loose_vert_data[i] = {};
      const BMVert *vert = BM_vert_at_index(&bm, loose_verts[i]);
      mesh_render_data_vert_flag(mr, vert, loose_vert_data[i]);
      
      /* Initialize face set color for loose verts */
      loose_vert_data[i].face_set_color = uchar4(0, 0, 0, 0);  /* transparent by default */
      
      /* Initialize normals for loose verts */
      loose_vert_data[i].nor = float3(0.0f, 0.0f, 1.0f);  /* default up normal */
    }
  });
}

gpu::VertBufPtr extract_edit_data_subdiv(const MeshRenderData &mr,
                                         const DRWSubdivCache &subdiv_cache)
{
  gpu::VertBufPtr vbo = gpu::VertBufPtr(GPU_vertbuf_create_with_format(get_edit_data_format()));
  const int size = subdiv_full_vbo_size(mr, subdiv_cache);
  GPU_vertbuf_data_alloc(*vbo, size);
  MutableSpan vbo_data = vbo->data<EditLoopData>();
  if (mr.extract_type == MeshExtractType::Mesh) {
    extract_edit_subdiv_data_mesh(mr, subdiv_cache, vbo_data);
  }
  else {
    extract_edit_subdiv_data_bm(mr, subdiv_cache, vbo_data);
  }
  return vbo;
}

}  // namespace blender::draw
