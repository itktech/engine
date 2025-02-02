// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/geometry/vertices_geometry.h"

#include <cstdint>
#include <utility>

#include "impeller/core/buffer_view.h"
#include "impeller/entity/contents/content_context.h"

namespace impeller {

// Fan mode isn't natively supported. Unroll into triangle mode by
// manipulating the index array.
//
// In Triangle fan, the first vertex is shared across all triangles, and then
// each sliding window of two vertices plus that first vertex defines a
// triangle.
static std::vector<uint16_t> fromFanIndices(
    const std::vector<Point>& vertices,
    const std::vector<uint16_t>& indices) {
  std::vector<uint16_t> unrolled_indices;

  // Un-fan index buffer if provided.
  if (indices.size() > 0u) {
    if (indices.size() < 3u) {
      return {};
    }

    auto center_point = indices[0];
    for (auto i = 1u; i < indices.size() - 1; i++) {
      unrolled_indices.push_back(center_point);
      unrolled_indices.push_back(indices[i]);
      unrolled_indices.push_back(indices[i + 1]);
    }
  } else {
    if (vertices.size() < 3u) {
      return {};
    }

    // If indices were not provided, create an index buffer that unfans
    // triangles instead of re-writing points, colors, et cetera.
    for (auto i = 1u; i < vertices.size() - 1; i++) {
      unrolled_indices.push_back(0);
      unrolled_indices.push_back(i);
      unrolled_indices.push_back(i + 1);
    }
  }
  return unrolled_indices;
}

/////// Vertices Geometry ///////

VerticesGeometry::VerticesGeometry(std::vector<Point> vertices,
                                   std::vector<uint16_t> indices,
                                   std::vector<Point> texture_coordinates,
                                   std::vector<Color> colors,
                                   Rect bounds,
                                   VertexMode vertex_mode)
    : vertices_(std::move(vertices)),
      colors_(std::move(colors)),
      texture_coordinates_(std::move(texture_coordinates)),
      indices_(std::move(indices)),
      bounds_(bounds),
      vertex_mode_(vertex_mode) {
  NormalizeIndices();
}

PrimitiveType VerticesGeometry::GetPrimitiveType() const {
  switch (vertex_mode_) {
    case VerticesGeometry::VertexMode::kTriangleFan:
      // Unrolled into triangle mode.
      return PrimitiveType::kTriangle;
    case VerticesGeometry::VertexMode::kTriangleStrip:
      return PrimitiveType::kTriangleStrip;
    case VerticesGeometry::VertexMode::kTriangles:
      return PrimitiveType::kTriangle;
  }
}

void VerticesGeometry::NormalizeIndices() {
  // Convert triangle fan if present.
  if (vertex_mode_ == VerticesGeometry::VertexMode::kTriangleFan) {
    indices_ = fromFanIndices(vertices_, indices_);
    return;
  }
}

bool VerticesGeometry::HasVertexColors() const {
  return colors_.size() > 0;
}

bool VerticesGeometry::HasTextureCoordinates() const {
  return texture_coordinates_.size() > 0;
}

std::optional<Rect> VerticesGeometry::GetTextureCoordinateCoverge() const {
  if (!HasTextureCoordinates()) {
    return std::nullopt;
  }
  auto vertex_count = vertices_.size();
  if (vertex_count == 0) {
    return std::nullopt;
  }

  return Rect::MakePointBounds(texture_coordinates_.begin(),
                               texture_coordinates_.end());
}

GeometryResult VerticesGeometry::GetPositionBuffer(
    const ContentContext& renderer,
    const Entity& entity,
    RenderPass& pass) const {
  auto index_count = indices_.size();
  auto vertex_count = vertices_.size();

  size_t total_vtx_bytes = vertex_count * sizeof(float) * 2;
  size_t total_idx_bytes = index_count * sizeof(uint16_t);

  auto vertex_buffer = renderer.GetTransientsBuffer().Emplace(
      reinterpret_cast<const uint8_t*>(vertices_.data()), total_vtx_bytes,
      alignof(float));

  BufferView index_buffer = {};
  if (index_count) {
    index_buffer = renderer.GetTransientsBuffer().Emplace(
        indices_.data(), total_idx_bytes, alignof(uint16_t));
  }

  return GeometryResult{
      .type = GetPrimitiveType(),
      .vertex_buffer =
          {
              .vertex_buffer = vertex_buffer,
              .index_buffer = index_buffer,
              .vertex_count = index_count > 0 ? index_count : vertex_count,
              .index_type =
                  index_count > 0 ? IndexType::k16bit : IndexType::kNone,
          },
      .transform = entity.GetShaderTransform(pass),
  };
}

GeometryResult VerticesGeometry::GetPositionColorBuffer(
    const ContentContext& renderer,
    const Entity& entity,
    RenderPass& pass) const {
  using VS = GeometryColorPipeline::VertexShader;

  auto index_count = indices_.size();
  auto vertex_count = vertices_.size();
  size_t total_vtx_bytes = vertex_count * sizeof(VS::PerVertexData);
  size_t total_idx_bytes = index_count * sizeof(uint16_t);

  auto vertex_buffer = renderer.GetTransientsBuffer().Emplace(
      total_vtx_bytes, alignof(VS::PerVertexData), [&](uint8_t* data) {
        VS::PerVertexData* vtx_contents =
            reinterpret_cast<VS::PerVertexData*>(data);
        for (auto i = 0u; i < vertices_.size(); i++) {
          VS::PerVertexData vertex_data = {
              .position = vertices_[i],
              .color = colors_[i],
          };
          std::memcpy(vtx_contents++, &vertex_data, sizeof(VS::PerVertexData));
        }
      });

  BufferView index_buffer = {};
  if (index_count > 0) {
    index_buffer = renderer.GetTransientsBuffer().Emplace(
        indices_.data(), total_idx_bytes, alignof(uint16_t));
  }

  return GeometryResult{
      .type = GetPrimitiveType(),
      .vertex_buffer =
          {
              .vertex_buffer = vertex_buffer,
              .index_buffer = index_buffer,
              .vertex_count = index_count > 0 ? index_count : vertex_count,
              .index_type =
                  index_count > 0 ? IndexType::k16bit : IndexType::kNone,
          },
      .transform = entity.GetShaderTransform(pass),
  };
}

GeometryResult VerticesGeometry::GetPositionUVColorBuffer(
    Rect texture_coverage,
    Matrix effect_transform,
    const ContentContext& renderer,
    const Entity& entity,
    RenderPass& pass) const {
  using VS = PorterDuffBlendPipeline::VertexShader;

  auto vertex_count = vertices_.size();
  auto uv_transform =
      texture_coverage.GetNormalizingTransform() * effect_transform;
  auto has_texture_coordinates = HasTextureCoordinates();

  size_t total_vtx_bytes = vertices_.size() * sizeof(VS::PerVertexData);
  auto vertex_buffer = renderer.GetTransientsBuffer().Emplace(
      total_vtx_bytes, alignof(VS::PerVertexData), [&](uint8_t* data) {
        VS::PerVertexData* vtx_contents =
            reinterpret_cast<VS::PerVertexData*>(data);
        for (auto i = 0u; i < vertices_.size(); i++) {
          auto vertex = vertices_[i];
          auto texture_coord =
              has_texture_coordinates ? texture_coordinates_[i] : vertices_[i];
          auto uv = uv_transform * texture_coord;
          // From experimentation we need to clamp these values to < 1.0 or else
          // there can be flickering.
          VS::PerVertexData vertex_data = {
              .vertices = vertex,
              .texture_coords =
                  Point(std::clamp(uv.x, 0.0f, 1.0f - kEhCloseEnough),
                        std::clamp(uv.y, 0.0f, 1.0f - kEhCloseEnough)),
              .color = colors_[i],
          };
          std::memcpy(vtx_contents++, &vertex_data, sizeof(VS::PerVertexData));
        }
      });

  BufferView index_buffer = {};
  auto index_count = indices_.size();
  size_t total_idx_bytes = index_count * sizeof(uint16_t);
  if (index_count > 0) {
    index_buffer = renderer.GetTransientsBuffer().Emplace(
        indices_.data(), total_idx_bytes, alignof(uint16_t));
  }

  return GeometryResult{
      .type = GetPrimitiveType(),
      .vertex_buffer =
          {
              .vertex_buffer = vertex_buffer,
              .index_buffer = index_buffer,
              .vertex_count = index_count > 0 ? index_count : vertex_count,
              .index_type =
                  index_count > 0 ? IndexType::k16bit : IndexType::kNone,
          },
      .transform = entity.GetShaderTransform(pass),
  };
}

GeometryVertexType VerticesGeometry::GetVertexType() const {
  if (HasVertexColors()) {
    return GeometryVertexType::kColor;
  }
  if (HasTextureCoordinates()) {
    return GeometryVertexType::kUV;
  }

  return GeometryVertexType::kPosition;
}

std::optional<Rect> VerticesGeometry::GetCoverage(
    const Matrix& transform) const {
  return bounds_.TransformBounds(transform);
}

}  // namespace impeller
