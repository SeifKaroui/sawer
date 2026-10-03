#include "renderer/GpuRenderer.hpp"
#include "canvas/Camera.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sawer {

void GpuRenderer::release_mesh(CachedMesh& mesh)
{
    resident_metadata_bytes_ -= mesh.resident.capacity() * sizeof(MeshRange);
    if (!mesh.resident.empty()) {
        // A raster scene can be rebuilt for a drawable/DPI change without a
        // document edit. Never retain descriptors to ranges that were freed.
        if (!scene_streamed_) scene_valid_ = false;
        if (mesh.last_submission == submitted_mesh_serial_ + 1U) resident_scene_invalidated_ = true;
        if (mesh.last_submission <= completed_mesh_serial_) mesh_arena_.release(mesh.resident);
        else mesh_retirements_.push_back({std::move(mesh.resident), mesh.last_submission});
    }
    std::vector<MeshRange>{}.swap(mesh.resident);
}

void GpuRenderer::release_mesh_storage() noexcept
{
    for (auto& page : mesh_pages_) {
        if (page != nullptr) SDL_ReleaseGPUBuffer(device_, page);
        page = nullptr;
    }
    for (const auto submission : mesh_submissions_) SDL_ReleaseGPUFence(device_, submission.fence);
    mesh_arena_.clear();
    decltype(mesh_uploads_){}.swap(mesh_uploads_);
    decltype(mesh_upload_vertices_){}.swap(mesh_upload_vertices_);
    decltype(mesh_retirements_){}.swap(mesh_retirements_);
    decltype(mesh_submissions_){}.swap(mesh_submissions_);
    submitted_mesh_serial_ = completed_mesh_serial_ = 0U;
    resident_metadata_bytes_ = 0U;
}

void GpuRenderer::reap_mesh_submissions()
{
    while (!mesh_submissions_.empty()) {
        const auto submission = mesh_submissions_.front();
        if (!SDL_QueryGPUFence(device_, submission.fence)) {
            if (mesh_submissions_.size() < 3U) break;
            if (!SDL_WaitForGPUFences(device_, true, &submission.fence, 1U))
                throw std::runtime_error{"GPU resident frame completion failed: " + std::string{SDL_GetError()}};
        }
        completed_mesh_serial_ = std::max(completed_mesh_serial_, submission.serial);
        SDL_ReleaseGPUFence(device_, submission.fence);
        mesh_submissions_.erase(mesh_submissions_.begin());
    }
    for (auto retirement = mesh_retirements_.begin(); retirement != mesh_retirements_.end();) {
        if (retirement->submission > completed_mesh_serial_) { ++retirement; continue; }
        mesh_arena_.release(retirement->ranges);
        retirement = mesh_retirements_.erase(retirement);
    }
}

void GpuRenderer::sync_document_changes(const Document& document)
{
    if (mesh_document_identity_ != document.cache_identity()
        || !document.changes_since(mesh_document_revision_, mesh_changes_)) {
        invalidate_document_cache();
    } else {
        for (const auto& change : mesh_changes_) {
            const auto cached = object_cache_.find(change.id);
            if (cached == object_cache_.end()) continue;
            release_mesh(cached->second);
            if (cached->second.alternate) release_mesh(*cached->second.alternate);
            geometry_cache_bytes_ -= cached->second.capacity_bytes();
            object_cache_.erase(cached);
            geometry_recency_.erase(change.id);
        }
    }
    mesh_document_identity_ = document.cache_identity();
    mesh_document_revision_ = document.revision();
}

bool GpuRenderer::ensure_resident_mesh(CachedMesh& mesh, const Object& object)
{
    if (!mesh.resident.empty()) { mesh.last_submission = submitted_mesh_serial_ + 1U; return true; }
    // Large world-spanning triangles retain the camera-rebased streaming path.
    // Resident positions stay within a few thousand units of their origin.
    if (mesh.paged || mesh.vertices.empty()
        || object.bounds.max_x - object.bounds.min_x > 4'096.0
        || object.bounds.max_y - object.bounds.min_y > 4'096.0
        || mesh_upload_vertices_.size() + mesh.vertices.size() > 1'048'575U) return false;
    auto ranges = mesh_arena_.allocate(mesh.vertices.size());
    if (!ranges) {
        auto candidate = geometry_recency_.oldest();
        for (std::size_t examined = 0U; candidate && examined < 64U; ++examined) {
            const auto id = *candidate;
            candidate = geometry_recency_.next(id);
            if (id == object.id) continue;
            auto& cached = object_cache_.at(id);
            if (cached.last_submission <= completed_mesh_serial_) release_mesh(cached);
            if (cached.alternate && cached.alternate->last_submission <= completed_mesh_serial_)
                release_mesh(*cached.alternate);
            ranges = mesh_arena_.allocate(mesh.vertices.size());
            if (ranges) break;
        }
    }
    if (!ranges) return false;
    mesh.resident = std::move(*ranges);
    resident_metadata_bytes_ += mesh.resident.capacity() * sizeof(MeshRange);
    mesh.origin = {
        std::floor((object.bounds.min_x + object.bounds.max_x) * 0.5 / 2'048.0) * 2'048.0,
        std::floor((object.bounds.min_y + object.bounds.max_y) * 0.5 / 2'048.0) * 2'048.0};
    mesh.last_submission = submitted_mesh_serial_ + 1U;
    std::size_t source = 0U;
    for (const auto range : mesh.resident) {
        if (mesh_pages_[range.page] == nullptr) {
            const SDL_GPUBufferCreateInfo info{SDL_GPU_BUFFERUSAGE_VERTEX,
                MeshArena::page_vertices * static_cast<Uint32>(sizeof(GeometryVertex)), 0U};
            mesh_pages_[range.page] = SDL_CreateGPUBuffer(device_, &info);
            if (mesh_pages_[range.page] == nullptr)
                throw std::runtime_error{"GPU resident page creation failed: " + std::string{SDL_GetError()}};
        }
        mesh_uploads_.push_back({range, static_cast<std::uint32_t>(mesh_upload_vertices_.size())});
        for (std::size_t offset = 0U; offset < range.count; ++offset) {
            const auto& vertex = mesh.vertices[source++];
            mesh_upload_vertices_.push_back({{static_cast<float>(vertex.position.x - mesh.origin.x),
                static_cast<float>(vertex.position.y - mesh.origin.y)}, vertex.color});
        }
    }
    return true;
}

std::size_t GpuRenderer::geometry_metadata_bytes() const noexcept
{
    if (object_cache_.empty()) return 0U;
    return geometry_recency_.memory_bytes() + object_cache_.bucket_count() * sizeof(void*)
        + object_cache_.size() * (sizeof(decltype(object_cache_)::value_type) + 2U * sizeof(void*))
        + resident_metadata_bytes_;
}

void GpuRenderer::draw_resident(SDL_GPUCommandBuffer* command, SDL_GPURenderPass* pass,
    const SceneDraw& draw, const Camera& camera, const bool multisampled)
{
    const auto viewport = camera.viewport();
    const auto position = camera.position();
    const float x = static_cast<float>(2.0 * camera.zoom() / viewport.x);
    const float y = static_cast<float>(-2.0 * camera.zoom() / viewport.y);
    const std::array<float, 32> matrices{{
        x, 0, 0, 0, 0, y, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
        static_cast<float>(draw.origin.x - position.x),
        static_cast<float>(draw.origin.y - position.y), 0, 1}};
    SDL_PushGPUVertexUniformData(command, 0U, matrices.data(), sizeof(matrices));
    SDL_BindGPUGraphicsPipeline(pass, !multisampled && retained_pipeline_direct_ != nullptr
        ? retained_pipeline_direct_ : retained_pipeline_);
    const SDL_GPUBufferBinding binding{draw.buffer, 0U};
    SDL_BindGPUVertexBuffers(pass, 0U, &binding, 1U);
    SDL_DrawGPUPrimitives(pass, draw.vertex_count, 1U, draw.first_vertex, 0U);
    ++stats_.draw_calls;
    ++stats_.resident_draws;
}

} // namespace sawer
