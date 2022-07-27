// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018-2021 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/geometry/TriangleMesh.h"

#include <vtkBooleanOperationPolyDataFilter.h>
#include <vtkCleanPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkFillHolesFilter.h>
#include <vtkPlane.h>
#include <vtkQuadricDecimation.h>

#include <Eigen/Core>
#include <string>
#include <unordered_map>

#include "open3d/core/EigenConverter.h"
#include "open3d/core/ShapeUtil.h"
#include "open3d/core/Tensor.h"
#include "open3d/core/TensorCheck.h"
#include "open3d/t/geometry/PointCloud.h"
#include "open3d/t/geometry/RaycastingScene.h"
#include "open3d/t/geometry/VtkUtils.h"
#include "open3d/t/geometry/kernel/PointCloud.h"
#include "open3d/t/geometry/kernel/Transform.h"

namespace open3d {
namespace t {
namespace geometry {

class PointCloud;  // forward declaration

TriangleMesh::TriangleMesh(const core::Device &device)
    : Geometry(Geometry::GeometryType::TriangleMesh, 3),
      device_(device),
      vertex_attr_(TensorMap("positions")),
      triangle_attr_(TensorMap("indices")) {}

TriangleMesh::TriangleMesh(const core::Tensor &vertex_positions,
                           const core::Tensor &triangle_indices)
    : TriangleMesh([&]() {
          if (vertex_positions.GetDevice() != triangle_indices.GetDevice()) {
              utility::LogError(
                      "vertex_positions' device {} does not match "
                      "triangle_indices' device {}.",
                      vertex_positions.GetDevice().ToString(),
                      triangle_indices.GetDevice().ToString());
          }
          return vertex_positions.GetDevice();
      }()) {
    SetVertexPositions(vertex_positions);
    SetTriangleIndices(triangle_indices);
}

std::string TriangleMesh::ToString() const {
    if (vertex_attr_.size() == 0 || triangle_attr_.size() == 0)
        return fmt::format("TriangleMesh on {} [{} vertices and {} triangles].",
                           GetDevice().ToString(), vertex_attr_.size(),
                           triangle_attr_.size());

    auto str = fmt::format(
            "TriangleMesh on {} [{} vertices ({}) and {} triangles ({})].",
            GetDevice().ToString(), GetVertexPositions().GetLength(),
            GetVertexPositions().GetDtype().ToString(),
            GetTriangleIndices().GetLength(),
            GetTriangleIndices().GetDtype().ToString());

    std::string vertices_attr_str = "\nVertex Attributes:";
    if (vertex_attr_.size() == 1) {
        vertices_attr_str += " None.";
    } else {
        for (const auto &kv : vertex_attr_) {
            if (kv.first != "positions") {
                vertices_attr_str +=
                        fmt::format(" {} (dtype = {}, shape = {}),", kv.first,
                                    kv.second.GetDtype().ToString(),
                                    kv.second.GetShape().ToString());
            }
        }
        vertices_attr_str[vertices_attr_str.size() - 1] = '.';
    }

    std::string triangles_attr_str = "\nTriangle Attributes:";
    if (triangle_attr_.size() == 1) {
        triangles_attr_str += " None.";
    } else {
        for (const auto &kv : triangle_attr_) {
            if (kv.first != "indices") {
                triangles_attr_str +=
                        fmt::format(" {} (dtype = {}, shape = {}),", kv.first,
                                    kv.second.GetDtype().ToString(),
                                    kv.second.GetShape().ToString());
            }
        }
        triangles_attr_str[triangles_attr_str.size() - 1] = '.';
    }

    return str + vertices_attr_str + triangles_attr_str;
}

TriangleMesh &TriangleMesh::Transform(const core::Tensor &transformation) {
    core::AssertTensorShape(transformation, {4, 4});

    kernel::transform::TransformPoints(transformation, GetVertexPositions());
    if (HasVertexNormals()) {
        kernel::transform::TransformNormals(transformation, GetVertexNormals());
    }
    if (HasTriangleNormals()) {
        kernel::transform::TransformNormals(transformation,
                                            GetTriangleNormals());
    }

    return *this;
}

TriangleMesh &TriangleMesh::Translate(const core::Tensor &translation,
                                      bool relative) {
    core::AssertTensorShape(translation, {3});

    core::Tensor transform =
            translation.To(GetDevice(), GetVertexPositions().GetDtype());

    if (!relative) {
        transform -= GetCenter();
    }
    GetVertexPositions() += transform;
    return *this;
}

TriangleMesh &TriangleMesh::Scale(double scale, const core::Tensor &center) {
    core::AssertTensorShape(center, {3});
    core::AssertTensorDevice(center, device_);

    const core::Tensor center_d =
            center.To(GetDevice(), GetVertexPositions().GetDtype());

    GetVertexPositions().Sub_(center_d).Mul_(scale).Add_(center_d);
    return *this;
}

TriangleMesh &TriangleMesh::Rotate(const core::Tensor &R,
                                   const core::Tensor &center) {
    core::AssertTensorShape(R, {3, 3});
    core::AssertTensorShape(center, {3});

    kernel::transform::RotatePoints(R, GetVertexPositions(), center);
    if (HasVertexNormals()) {
        kernel::transform::RotateNormals(R, GetVertexNormals());
    }
    if (HasTriangleNormals()) {
        kernel::transform::RotateNormals(R, GetTriangleNormals());
    }
    return *this;
}

geometry::TriangleMesh TriangleMesh::FromLegacy(
        const open3d::geometry::TriangleMesh &mesh_legacy,
        core::Dtype float_dtype,
        core::Dtype int_dtype,
        const core::Device &device) {
    if (float_dtype != core::Float32 && float_dtype != core::Float64) {
        utility::LogError("float_dtype must be Float32 or Float64, but got {}.",
                          float_dtype.ToString());
    }
    if (int_dtype != core::Int32 && int_dtype != core::Int64) {
        utility::LogError("int_dtype must be Int32 or Int64, but got {}.",
                          int_dtype.ToString());
    }

    TriangleMesh mesh(device);
    if (mesh_legacy.HasVertices()) {
        mesh.SetVertexPositions(
                core::eigen_converter::EigenVector3dVectorToTensor(
                        mesh_legacy.vertices_, float_dtype, device));
    } else {
        utility::LogWarning("Creating from empty legacy TriangleMesh.");
    }
    if (mesh_legacy.HasVertexColors()) {
        mesh.SetVertexColors(core::eigen_converter::EigenVector3dVectorToTensor(
                mesh_legacy.vertex_colors_, float_dtype, device));
    }
    if (mesh_legacy.HasVertexNormals()) {
        mesh.SetVertexNormals(
                core::eigen_converter::EigenVector3dVectorToTensor(
                        mesh_legacy.vertex_normals_, float_dtype, device));
    }
    if (mesh_legacy.HasTriangles()) {
        mesh.SetTriangleIndices(
                core::eigen_converter::EigenVector3iVectorToTensor(
                        mesh_legacy.triangles_, int_dtype, device));
    }
    if (mesh_legacy.HasTriangleNormals()) {
        mesh.SetTriangleNormals(
                core::eigen_converter::EigenVector3dVectorToTensor(
                        mesh_legacy.triangle_normals_, float_dtype, device));
    }
    if (mesh_legacy.HasTriangleUvs()) {
        mesh.SetTriangleAttr(
                "texture_uvs",
                core::eigen_converter::EigenVector2dVectorToTensor(
                        mesh_legacy.triangle_uvs_, float_dtype, device)
                        .Reshape({-1, 3, 2}));
    }
    return mesh;
}

open3d::geometry::TriangleMesh TriangleMesh::ToLegacy() const {
    open3d::geometry::TriangleMesh mesh_legacy;
    if (HasVertexPositions()) {
        mesh_legacy.vertices_ =
                core::eigen_converter::TensorToEigenVector3dVector(
                        GetVertexPositions());
    }
    if (HasVertexColors()) {
        mesh_legacy.vertex_colors_ =
                core::eigen_converter::TensorToEigenVector3dVector(
                        GetVertexColors());
    }
    if (HasVertexNormals()) {
        mesh_legacy.vertex_normals_ =
                core::eigen_converter::TensorToEigenVector3dVector(
                        GetVertexNormals());
    }
    if (HasTriangleIndices()) {
        mesh_legacy.triangles_ =
                core::eigen_converter::TensorToEigenVector3iVector(
                        GetTriangleIndices());
    }
    if (HasTriangleNormals()) {
        mesh_legacy.triangle_normals_ =
                core::eigen_converter::TensorToEigenVector3dVector(
                        GetTriangleNormals());
    }
    if (HasTriangleAttr("texture_uvs")) {
        mesh_legacy.triangle_uvs_ =
                core::eigen_converter::TensorToEigenVector2dVector(
                        GetTriangleAttr("texture_uvs").Reshape({-1, 2}));
    }
    if (HasVertexAttr("texture_uvs")) {
        utility::LogWarning("{}",
                            "texture_uvs as a vertex attribute is not "
                            "supported by legacy TriangleMesh. Ignored.");
    }

    return mesh_legacy;
}

TriangleMesh TriangleMesh::To(const core::Device &device, bool copy) const {
    if (!copy && GetDevice() == device) {
        return *this;
    }
    TriangleMesh mesh(device);
    for (const auto &kv : triangle_attr_) {
        mesh.SetTriangleAttr(kv.first, kv.second.To(device, /*copy=*/true));
    }
    for (const auto &kv : vertex_attr_) {
        mesh.SetVertexAttr(kv.first, kv.second.To(device, /*copy=*/true));
    }
    return mesh;
}

TriangleMesh TriangleMesh::ComputeConvexHull(bool joggle_inputs) const {
    PointCloud pcd(GetVertexPositions());
    return pcd.ComputeConvexHull();
}

TriangleMesh TriangleMesh::ClipPlane(const core::Tensor &point,
                                     const core::Tensor &normal) const {
    using namespace vtkutils;
    core::AssertTensorShape(point, {3});
    core::AssertTensorShape(normal, {3});
    // allow int types for convenience
    core::AssertTensorDtypes(
            point, {core::Float32, core::Float64, core::Int32, core::Int64});
    core::AssertTensorDtypes(
            normal, {core::Float32, core::Float64, core::Int32, core::Int64});

    auto point_ = point.To(core::Device(), core::Float64).Contiguous();
    auto normal_ = normal.To(core::Device(), core::Float64).Contiguous();

    auto polydata = CreateVtkPolyDataFromGeometry(*this);

    vtkNew<vtkPlane> clipPlane;
    clipPlane->SetNormal(normal_.GetDataPtr<double>());
    clipPlane->SetOrigin(point_.GetDataPtr<double>());
    vtkNew<vtkClipPolyData> clipper;
    clipper->SetInputData(polydata);
    clipper->SetClipFunction(clipPlane);
    vtkNew<vtkCleanPolyData> cleaner;
    cleaner->SetInputConnection(clipper->GetOutputPort());
    cleaner->Update();
    auto clipped_polydata = cleaner->GetOutput();
    return CreateTriangleMeshFromVtkPolyData(clipped_polydata);
}

TriangleMesh TriangleMesh::SimplifyQuadricDecimation(
        double target_reduction, bool preserve_volume) const {
    using namespace vtkutils;
    if (target_reduction >= 1.0 || target_reduction < 0) {
        utility::LogError(
                "target_reduction must be in the range [0,1) but is {}",
                target_reduction);
    }

    auto polydata = CreateVtkPolyDataFromGeometry(*this);

    vtkNew<vtkQuadricDecimation> decimate;
    decimate->SetInputData(polydata);
    decimate->SetTargetReduction(target_reduction);
    decimate->SetVolumePreservation(preserve_volume);
    decimate->Update();
    auto decimated_polydata = decimate->GetOutput();

    return CreateTriangleMeshFromVtkPolyData(decimated_polydata);
}

namespace {
TriangleMesh BooleanOperation(const TriangleMesh &mesh_A,
                              const TriangleMesh &mesh_B,
                              double tolerance,
                              int op) {
    using namespace vtkutils;
    // clean meshes before passing them to the boolean operation
    auto polydata_A = CreateVtkPolyDataFromGeometry(mesh_A);
    vtkNew<vtkCleanPolyData> cleaner_A;
    cleaner_A->SetInputData(polydata_A);

    auto polydata_B = CreateVtkPolyDataFromGeometry(mesh_B);
    vtkNew<vtkCleanPolyData> cleaner_B;
    cleaner_B->SetInputData(polydata_B);

    vtkNew<vtkBooleanOperationPolyDataFilter> boolean_filter;
    boolean_filter->SetOperation(op);
    boolean_filter->SetTolerance(tolerance);
    boolean_filter->SetInputConnection(0, cleaner_A->GetOutputPort());
    boolean_filter->SetInputConnection(1, cleaner_B->GetOutputPort());
    boolean_filter->Update();
    auto out_polydata = boolean_filter->GetOutput();

    return CreateTriangleMeshFromVtkPolyData(out_polydata);
}
}  // namespace

TriangleMesh TriangleMesh::BooleanUnion(const TriangleMesh &mesh,
                                        double tolerance) const {
    return BooleanOperation(*this, mesh, tolerance,
                            vtkBooleanOperationPolyDataFilter::VTK_UNION);
}

TriangleMesh TriangleMesh::BooleanIntersection(const TriangleMesh &mesh,
                                               double tolerance) const {
    return BooleanOperation(
            *this, mesh, tolerance,
            vtkBooleanOperationPolyDataFilter::VTK_INTERSECTION);
}

TriangleMesh TriangleMesh::BooleanDifference(const TriangleMesh &mesh,
                                             double tolerance) const {
    return BooleanOperation(*this, mesh, tolerance,
                            vtkBooleanOperationPolyDataFilter::VTK_DIFFERENCE);
}

TriangleMesh TriangleMesh::FillHoles(double hole_size) const {
    using namespace vtkutils;
    auto polydata = CreateVtkPolyDataFromGeometry(*this);
    vtkNew<vtkFillHolesFilter> fill_holes;
    fill_holes->SetInputData(polydata);
    fill_holes->SetHoleSize(hole_size);
    fill_holes->Update();
    auto result = fill_holes->GetOutput();
    return CreateTriangleMeshFromVtkPolyData(result);
}

namespace {
/// Bakes vertex or triangle attributes to a texure.
///
/// \tparam TAttr The data type of the attribute.
/// \tparam TInt The data type for triangle indices.
/// \tparam VERTEX_ATTR If true bake vertex attributes with interpolation.
/// If false bake triangle attributes.
///
/// \param size The texture size.
/// \param triangle_indices The triangle_indices of the TriangleMesh.
/// \param primitive_ids The primitive ids from ComputePrimitiveInfoTexture().
/// \param primitive_uvs The primitive uvs from ComputePrimitiveInfoTexture().
/// \param sqrdistance The squared distances from ComputePrimitiveInfoTexture().
/// \param fill_value Fill value for the generated textures.
template <class TAttr, class TInt, bool VERTEX_ATTR>
core::Tensor BakeAttribute(int size,
                           float margin,
                           const core::Tensor &attr,
                           const core::Tensor &triangle_indices,
                           const core::Tensor &primitive_ids,
                           const core::Tensor &primitive_uvs,
                           const core::Tensor &sqrdistance,
                           TAttr fill_value) {
    core::SizeVector tex_shape({size, size});
    tex_shape.insert(tex_shape.end(), attr.GetShapeRef().begin() + 1,
                     attr.GetShapeRef().end());
    core::SizeVector components_shape(attr.GetShapeRef().begin() + 1,
                                      attr.GetShapeRef().end());
    const int num_components =
            components_shape.NumElements();  // is 1 for empty shape
    core::Tensor tex = core::Tensor::Empty(tex_shape, attr.GetDtype());

    const float threshold = (margin / size) * (margin / size);
    Eigen::Map<const Eigen::MatrixXf> sqrdistance_map(
            sqrdistance.GetDataPtr<float>(), size, size);
    Eigen::Map<Eigen::Matrix<TAttr, Eigen::Dynamic, Eigen::Dynamic>> tex_map(
            tex.GetDataPtr<TAttr>(), num_components, size * size);
    Eigen::Map<const Eigen::Matrix<uint32_t, Eigen::Dynamic, Eigen::Dynamic>>
            tid_map(primitive_ids.GetDataPtr<uint32_t>(), size, size);
    Eigen::Map<const Eigen::MatrixXf> uv_map(primitive_uvs.GetDataPtr<float>(),
                                             2, size * size);
    Eigen::Map<const Eigen::Matrix<TAttr, Eigen::Dynamic, Eigen::Dynamic>>
            attr_map(attr.GetDataPtr<TAttr>(), num_components,
                     attr.GetLength());
    Eigen::Map<const Eigen::Matrix<TInt, 3, Eigen::Dynamic>>
            triangle_indices_map(triangle_indices.GetDataPtr<TInt>(), 3,
                                 triangle_indices.GetLength());

    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            const int64_t linear_idx = i * size + j;
            if (sqrdistance_map(j, i) <= threshold) {
                const uint32_t tid = tid_map(j, i);
                if (VERTEX_ATTR) {
                    const auto &a = attr_map.col(triangle_indices_map(0, tid));
                    const auto &b = attr_map.col(triangle_indices_map(1, tid));
                    const auto &c = attr_map.col(triangle_indices_map(2, tid));
                    TAttr u = uv_map(0, linear_idx);
                    TAttr v = uv_map(1, linear_idx);
                    tex_map.col(linear_idx) =
                            std::max<TAttr>(0, 1 - u - v) * a + u * b + v * c;
                } else {
                    tex_map.col(linear_idx) = attr_map.col(tid);
                }
            } else {
                tex_map.col(linear_idx).setConstant(fill_value);
            }
        }
    }

    return tex;
}

/// Computes textures with the primitive ids, primitive uvs, and the squared
/// distance to the closest primitive.
///
/// This is a helper function for the texture baking functions.
///
/// \param size The texture size.
/// \param primitive_ids The output tensor for the primitive ids.
/// \param primitive_uvs The output tensor for the primitive uvs.
/// \param sqrdistance The output tensor for the squared distances.
/// \param texture_uvs Input tensor with the texture uvs.
void ComputePrimitiveInfoTexture(int size,
                                 core::Tensor &primitive_ids,
                                 core::Tensor &primitive_uvs,
                                 core::Tensor &sqrdistance,
                                 const core::Tensor &texture_uvs) {
    const int64_t num_triangles = texture_uvs.GetLength();

    // Generate vertices for each triangle using (u,v,0) as position.
    core::Tensor vertices({num_triangles * 3, 3}, core::Float32);
    {
        const float *uv_ptr = texture_uvs.GetDataPtr<float>();
        float *v_ptr = vertices.GetDataPtr<float>();
        for (int64_t i = 0; i < texture_uvs.GetLength(); ++i) {
            for (int64_t j = 0; j < 3; ++j) {
                v_ptr[i * 9 + j * 3 + 0] = uv_ptr[i * 6 + j * 2 + 0];
                v_ptr[i * 9 + j * 3 + 1] = uv_ptr[i * 6 + j * 2 + 1];
                v_ptr[i * 9 + j * 3 + 2] = 0;
            }
        }
    }
    core::Tensor triangle_indices =
            core::Tensor::Empty({num_triangles, 3}, core::UInt32);
    std::iota(triangle_indices.GetDataPtr<uint32_t>(),
              triangle_indices.GetDataPtr<uint32_t>() +
                      triangle_indices.NumElements(),
              0);

    RaycastingScene scene;
    scene.AddTriangles(vertices, triangle_indices);

    core::Tensor query_points =
            core::Tensor::Empty({size, size, 3}, core::Float32);
    float *ptr = query_points.GetDataPtr<float>();
    for (int i = 0; i < size; ++i) {
        float v = 1 - (i + 0.5f) / size;
        for (int j = 0; j < size; ++j) {
            float u = (j + 0.5f) / size;
            ptr[i * size * 3 + j * 3 + 0] = u;
            ptr[i * size * 3 + j * 3 + 1] = v;
            ptr[i * size * 3 + j * 3 + 2] = 0;
        }
    }

    auto ans = scene.ComputeClosestPoints(query_points);

    Eigen::Map<Eigen::MatrixXf> query_points_map(
            query_points.GetDataPtr<float>(), 3, size * size);
    Eigen::Map<Eigen::MatrixXf> closest_points_map(
            ans["points"].GetDataPtr<float>(), 3, size * size);
    sqrdistance = core::Tensor::Empty({size, size}, core::Float32);
    Eigen::Map<Eigen::VectorXf> sqrdistance_map(sqrdistance.GetDataPtr<float>(),
                                                size * size);
    sqrdistance_map =
            (closest_points_map - query_points_map).colwise().squaredNorm();
    primitive_ids = ans["primitive_ids"];
    primitive_uvs = ans["primitive_uvs"];
}
}  // namespace
std::unordered_map<std::string, core::Tensor>
TriangleMesh::BakeVertexAttrTextures(
        int size,
        const std::unordered_set<std::string> &vertex_attr,
        double margin,
        double fill) const {
    if (!vertex_attr.size()) {
        return std::unordered_map<std::string, core::Tensor>();
    }
    if (!triangle_attr_.Contains("texture_uvs")) {
        utility::LogError("Cannot find triangle attribute 'texture_uvs'");
    }

    core::Tensor texture_uvs =
            triangle_attr_.at("texture_uvs").To(core::Device()).Contiguous();
    core::AssertTensorShape(texture_uvs, {core::None, 3, 2});
    core::AssertTensorDtype(texture_uvs, {core::Float32});

    core::Tensor vertices({GetTriangleIndices().GetLength() * 3, 3},
                          core::Float32);
    {
        float *uv_ptr = texture_uvs.GetDataPtr<float>();
        float *v_ptr = vertices.GetDataPtr<float>();
        for (int64_t i = 0; i < texture_uvs.GetLength(); ++i) {
            for (int64_t j = 0; j < 3; ++j) {
                v_ptr[i * 9 + j * 3 + 0] = uv_ptr[i * 6 + j * 2 + 0];
                v_ptr[i * 9 + j * 3 + 1] = uv_ptr[i * 6 + j * 2 + 1];
                v_ptr[i * 9 + j * 3 + 2] = 0;
            }
        }
    }
    core::Tensor triangle_indices =
            core::Tensor::Empty(GetTriangleIndices().GetShape(), core::UInt32);
    std::iota(triangle_indices.GetDataPtr<uint32_t>(),
              triangle_indices.GetDataPtr<uint32_t>() +
                      triangle_indices.NumElements(),
              0);

    core::Tensor primitive_ids, primitive_uvs, sqrdistance;
    {
        RaycastingScene scene;
        scene.AddTriangles(vertices, triangle_indices);
        core::Tensor query_points =
                core::Tensor::Empty({size, size, 3}, core::Float32);
        float *ptr = query_points.GetDataPtr<float>();
        for (int i = 0; i < size; ++i) {
            float v = 1 - (i + 0.5f) / size;
            for (int j = 0; j < size; ++j) {
                float u = (j + 0.5f) / size;
                ptr[i * size * 3 + j * 3 + 0] = u;
                ptr[i * size * 3 + j * 3 + 1] = v;
                ptr[i * size * 3 + j * 3 + 2] = 0;
            }
        }

        auto ans = scene.ComputeClosestPoints(query_points);

        Eigen::Map<Eigen::MatrixXf> query_points_map(
                query_points.GetDataPtr<float>(), 3, size * size);
        Eigen::Map<Eigen::MatrixXf> closest_points_map(
                ans["points"].GetDataPtr<float>(), 3, size * size);
        sqrdistance = core::Tensor::Empty({size, size}, core::Float32);
        Eigen::Map<Eigen::VectorXf> sqrdistance_map(
                sqrdistance.GetDataPtr<float>(), size * size);
        sqrdistance_map =
                (closest_points_map - query_points_map).colwise().squaredNorm();
        primitive_ids = ans["primitive_ids"];
        primitive_uvs = ans["primitive_uvs"];
    }

    std::unordered_map<std::string, core::Tensor> result;

    for (auto attr : vertex_attr) {
        if (!vertex_attr_.Contains(attr)) {
            utility::LogError("Cannot find vertex attribute '{}'", attr);
        }
        core::Tensor tensor =
                vertex_attr_.at(attr).To(core::Device()).Contiguous();
        DISPATCH_FLOAT_INT_DTYPE_TO_TEMPLATE(
                tensor.GetDtype(), GetTriangleIndices().GetDtype(), [&]() {
                    core::Tensor tex = BakeAttribute<scalar_t, int_t, true>(
                            size, margin, tensor, GetTriangleIndices(),
                            primitive_ids, primitive_uvs, sqrdistance,
                            scalar_t(fill));
                    result[attr] = tex;
                });
    }

    return result;
}

std::unordered_map<std::string, core::Tensor>
TriangleMesh::BakeTriangleAttrTextures(
        int size,
        const std::unordered_set<std::string> &triangle_attr,
        double margin,
        double fill) const {
    if (!triangle_attr.size()) {
        return std::unordered_map<std::string, core::Tensor>();
    }
    if (!triangle_attr_.Contains("texture_uvs")) {
        utility::LogError("Cannot find triangle attribute 'texture_uvs'");
    }

    core::Tensor texture_uvs =
            triangle_attr_.at("texture_uvs").To(core::Device()).Contiguous();
    core::AssertTensorShape(texture_uvs, {core::None, 3, 2});
    core::AssertTensorDtype(texture_uvs, {core::Float32});

    core::Tensor primitive_ids, primitive_uvs, sqrdistance;
    ComputePrimitiveInfoTexture(size, primitive_ids, primitive_uvs, sqrdistance,
                                texture_uvs);

    std::unordered_map<std::string, core::Tensor> result;
    for (auto attr : triangle_attr) {
        if (!triangle_attr_.Contains(attr)) {
            utility::LogError("Cannot find triangle attribute '{}'", attr);
        }
        core::Tensor tensor =
                triangle_attr_.at(attr).To(core::Device()).Contiguous();
        DISPATCH_DTYPE_TO_TEMPLATE(tensor.GetDtype(), [&]() {
            core::Tensor tex;
            if (GetTriangleIndices().GetDtype() == core::Int32) {
                tex = BakeAttribute<scalar_t, int32_t, false>(
                        size, margin, tensor, GetTriangleIndices(),
                        primitive_ids, primitive_uvs, sqrdistance,
                        scalar_t(fill));
            } else if (GetTriangleIndices().GetDtype() == core::Int64) {
                tex = BakeAttribute<scalar_t, int64_t, false>(
                        size, margin, tensor, GetTriangleIndices(),
                        primitive_ids, primitive_uvs, sqrdistance,
                        scalar_t(fill));
            } else {
                utility::LogError("Unsupported triangle indices data type.");
            }
            result[attr] = tex;
        });
    }

    return result;
}

}  // namespace geometry
}  // namespace t
}  // namespace open3d
