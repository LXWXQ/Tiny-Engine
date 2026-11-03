#include "GeometrySystem.h"
#include "Render/SubMeshCulling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
struct TerrainGpuPushConstants
{
    glm::mat4 viewProjection{ 1.0f };
    glm::vec4 cameraPosition{ 0.0f };
    glm::uvec4 counts{ 0u };
    glm::vec4 lodTiming{ 0.24f, 0.15f, 0.0f, 0.0f };
};
static_assert(sizeof(TerrainGpuPushConstants) == 112,
              "terrain_lod.comp push constant layout mismatch");

uint32_t terrainGpuFloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float terrainGpuBitsFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool terrainGpuDebugMatches(const std::array<uint32_t, 8>& a,
                            const std::array<uint32_t, 8>& b)
{
    // Culled chunks emit no commands; their internal hysteresis bookkeeping does
    // not affect the frame. Visible chunks must match the full rendered contract.
    if (a[0] == 0u && b[0] == 0u)
        return a[4] == 0u && b[4] == 0u;

    for (size_t i = 0; i < a.size(); ++i)
    {
        // transitionFrom is internal bookkeeping for culled chunks; GPU may
        // complete a hidden transition while the CPU mirror never advanced it.
        // The rendered contract is visible/lodA/lodB/blend/commandCount/stable/to.
        if (i == 6u)
            continue;
        if (i == 3u)
        {
            if (std::fabs(terrainGpuBitsFloat(a[i]) - terrainGpuBitsFloat(b[i])) > 1e-4f)
                return false;
        }
        else if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

void GeometrySystem::prepareTerrainGpuSelection(VkCommandBuffer commandBuffer,
                                                FrameInfo& frameInfo)
{
    TerrainGpuRunContext run;
    if (!terrainGpuSelectionAvailable || !terrainGpuPipeline)
    {
        currentTerrainGpuRun = {};
        return;
    }

    run.enabled = true;
    TerrainGpuFrameResources& frameRes =
        terrainGpuFrames[frameInfo.frameInFlightIndex];
    run.frame = &frameRes;

    auto* records = static_cast<TerrainGpuChunkRecord*>(frameRes.records.mapped);
    auto* counts = static_cast<uint32_t*>(frameRes.counts.mapped);
    auto* stats = static_cast<uint32_t*>(frameRes.stats.mapped);
    auto* debug = static_cast<uint32_t*>(frameRes.debug.mapped);

    // The previous use of this frame slot is fence-complete. Consume its GPU
    // stats for telemetry and, in --terrain-test, compare GPU selection with the
    // CPU mirror stored two frames ago.
    vmaInvalidateAllocation(vulkanDevice.getVmaAllocator(), frameRes.stats.allocation,
                            0, VK_WHOLE_SIZE);
    std::array<uint32_t, kTerrainGpuStatsCount> previousStats{};
    std::memcpy(previousStats.data(), stats, sizeof(previousStats));

    if (frameInfo.settings.terrainGpuSelectionSelfTest &&
        frameRes.debugValid && !frameRes.parityExpected.empty())
    {
        vmaInvalidateAllocation(vulkanDevice.getVmaAllocator(),
                                frameRes.debug.allocation, 0, VK_WHOLE_SIZE);
        bool match = frameRes.recordCount == frameRes.parityExpected.size();
        if (match)
        {
            for (uint32_t i = 0; i < frameRes.recordCount; ++i)
            {
                std::array<uint32_t, kTerrainGpuDebugWords> actual{};
                std::memcpy(actual.data(), debug + i * kTerrainGpuDebugWords,
                            sizeof(actual));
                if (!terrainGpuDebugMatches(actual, frameRes.parityExpected[i]))
                {
                    match = false;
                    if (!terrainGpuParityFailed)
                    {
                        std::cout << "[Terrain] GPU selection selfTest FAIL | record="
                                  << i << "\n  gpu=";
                        for (uint32_t v : actual) std::cout << v << " ";
                        std::cout << "\n  cpu=";
                        for (uint32_t v : frameRes.parityExpected[i]) std::cout << v << " ";
                        std::cout << "\n";
                    }
                    break;
                }
            }
        }

        if (match)
        {
            ++terrainGpuParityPasses;
            bool sawTransition = false;
            for (const auto& expected : frameRes.parityExpected)
                sawTransition = sawTransition || expected[4] > 1u;

            if (terrainGpuParityPasses >= 3u && !terrainGpuParityLogged)
            {
                terrainGpuParityLogged = true;
                std::cout << "[Terrain] GPU selection selfTest PASS | matches="
                          << terrainGpuParityPasses << " records="
                          << frameRes.recordCount << "\n";
            }
            if (sawTransition && !terrainGpuTransitionLogged)
            {
                terrainGpuTransitionLogged = true;
                std::cout << "[Terrain] GPU selection transition PASS | records="
                          << frameRes.recordCount << "\n";
            }
        }
        else
        {
            terrainGpuParityFailed = true;
            throw std::runtime_error("GPU terrain selection parity failed");
        }
    }

    // F2-b: when the arena batch was active last frame, all those chunks were
    // drawn by a single indirect call that was already counted then.
    if (!terrainBatchWasActive)
        frameInfo.telemetry.drawCalls += previousStats[0];
    frameInfo.telemetry.subMeshesCulled += previousStats[1];
    frameInfo.telemetry.terrainLod0Chunks += previousStats[2];
    frameInfo.telemetry.terrainLod1Chunks += previousStats[3];
    frameInfo.telemetry.terrainLod2Chunks += previousStats[4];
    frameInfo.telemetry.terrainLodBlends += previousStats[5];
    frameInfo.telemetry.terrainLodSwitches += previousStats[6];
    frameInfo.telemetry.triangles += previousStats[7];
    frameInfo.telemetry.vertices += previousStats[8];

    // Reclaim slots that have not been visible for 8 frames. Streamed chunks
    // can otherwise churn through 64 slots during continuous camera movement;
    // an offscreen entity's hysteresis state is allowed to reset when it
    // becomes visible again.
    for (auto it = terrainGpuEntitySlots.begin();
         it != terrainGpuEntitySlots.end();)
    {
        const entt::entity owner = static_cast<entt::entity>(it->first);
        const bool destroyed = !frameInfo.registry.valid(owner);
        if (destroyed || it->second.lastFrame + 8u < frameInfo.frameNumber)
        {
            terrainGpuSlotUsed[it->second.slot] = false;
            // F2-f：arena 槽位的生命周期归 Scene，实体槽回收不再动它。
            it = terrainGpuEntitySlots.erase(it);
        }
        else
        {
            ++it;
        }
    }

    std::memset(counts, 0, static_cast<size_t>(frameRes.counts.size));
    std::memset(stats, 0, static_cast<size_t>(frameRes.stats.size));

    const float gpuDeltaTime = std::max(frameInfo.frameTime, 0.0f) *
                               static_cast<float>(kFramesInFlight);
    const render::SubMeshCuller culler(
        frameInfo.projectionMatrix * frameInfo.viewMatrix);
    const glm::vec3 cameraPosition = frameInfo.cameraTransform
        ? frameInfo.cameraTransform->getTranslation()
        : glm::vec3(0.0f);

    std::vector<std::array<uint32_t, kTerrainGpuDebugWords>> parityExpected;
    parityExpected.reserve(kTerrainGpuMaxRecords);
    uint32_t recordCursor = 0u;

    const bool terrainBatchRequested =
        frameInfo.settings.terrainBatchEnabled && sceneInstanceSetLayout != VK_NULL_HANDLE;
    if (!frameInfo.settings.terrainBatchEnabled)
    {
        static bool loggedDisabled = false;
        if (!loggedDisabled)
        {
            loggedDisabled = true;
            std::cout << "[Terrain] terrain batching disabled | falling back to per-chunk draws"
                      << std::endl;
        }
    }
    bool terrainBatchActive = terrainBatchRequested && ensureTerrainChunkInstances();
    auto* chunkInstanceBytes = terrainBatchActive
        ? static_cast<uint8_t*>(terrainChunkInstances[frameInfo.frameInFlightIndex].mapped)
        : nullptr;
    if (terrainBatchActive && chunkInstanceBytes != nullptr)
    {
        std::memset(chunkInstanceBytes, 0,
                    static_cast<size_t>(kTerrainGpuMaxEntities) * kTerrainChunkInstanceStride);
    }
    else
    {
        terrainBatchActive = false;
    }

    for (entt::entity entity : frameInfo.visibleEntities)
    {
        const MeshComponent* mesh = frameInfo.registry.try_get<MeshComponent>(entity);
        const WorldTransformComponent* worldTransform =
            frameInfo.registry.try_get<WorldTransformComponent>(entity);
        const RenderTagComponent* tag =
            frameInfo.registry.try_get<RenderTagComponent>(entity);
        const TerrainComponent* terrain =
            frameInfo.registry.try_get<TerrainComponent>(entity);
        if (!mesh || !mesh->model || !worldTransform || !tag || !tag->isTerrain ||
            !terrain)
        {
            continue;
        }

        const uint32_t entityId = static_cast<uint32_t>(entt::to_integral(entity));
        auto slotIt = terrainGpuEntitySlots.find(entityId);
        if (slotIt == terrainGpuEntitySlots.end())
        {
            uint32_t slotIndex = kTerrainGpuMaxEntities;
            for (uint32_t i = 0; i < kTerrainGpuMaxEntities; ++i)
            {
                if (!terrainGpuSlotUsed[i])
                {
                    slotIndex = i;
                    break;
                }
            }
            if (slotIndex >= kTerrainGpuMaxEntities)
                continue;

            terrainGpuSlotUsed[slotIndex] = true;
            TerrainGpuEntitySlot newSlot;
            newSlot.slot = slotIndex;
            newSlot.model = mesh->model.get();
            newSlot.revision = terrain->revision;
            newSlot.lastFrame = frameInfo.frameNumber;
            newSlot.resetPending = true;
            slotIt = terrainGpuEntitySlots.emplace(entityId, newSlot).first;
        }
        else
        {
            if (slotIt->second.model != mesh->model.get() ||
                slotIt->second.revision != terrain->revision)
            {
                slotIt->second.model = mesh->model.get();
                slotIt->second.revision = terrain->revision;
                slotIt->second.resetPending = true;
            }
            slotIt->second.lastFrame = frameInfo.frameNumber;
        }

        TerrainGpuEntitySlot& slot = slotIt->second;
        const uint32_t slotIndex = slot.slot;
        run.entitySlots[entityId] = slotIndex;

        // F2-f：arena 基址与是否合批无关——不合批时逐 chunk 的 indirect 命令同样要指向
        // 自己那块 arena 切片，否则会拿模型内的偏移去索引整份缓冲，画出来是拉伸的乱几何。
        uint32_t arenaIndexBase = 0u;
        uint32_t arenaVertexBase = 0u;
        bool chunkArenaBacked = false;
        if (terrainMeshArena != nullptr &&
            terrain->arenaIndexBase != TerrainMeshArena::kInvalidOffset)
        {
            arenaIndexBase = terrain->arenaIndexBase;
            arenaVertexBase = terrain->arenaVertexBase;
            chunkArenaBacked = true;
        }
        else if (terrainBatchActive)
        {
            if (!terrainArenaWarned)
            {
                terrainArenaWarned = true;
                std::cout << "[Terrain] chunk has no arena slice | falling back to per-chunk draws"
                          << std::endl;
            }
            terrainBatchActive = false;
        }
        if (terrainBatchActive && chunkInstanceBytes != nullptr)
        {
            auto* row = reinterpret_cast<float*>(chunkInstanceBytes +
                static_cast<size_t>(slotIndex) * kTerrainChunkInstanceStride);
            std::memcpy(row, &worldTransform->matrix, sizeof(glm::mat4));
            std::memcpy(row + 16, &worldTransform->normalMatrix, sizeof(glm::mat4));
            const BoundsComponent* terrainBounds =
                frameInfo.registry.try_get<BoundsComponent>(entity);
            float terrainMinY = 0.0f;
            float terrainHeightRange = 1.0f;
            if (terrainBounds && terrainBounds->getLocal().valid)
            {
                const AABB& localBounds = terrainBounds->getLocal();
                terrainMinY = localBounds.min.y;
                terrainHeightRange = std::max(localBounds.max.y - localBounds.min.y, 1e-4f);
            }
            row[38] = 1.0f; // misc.z: terrainMode (float field, not a bit pattern)
            row[39] = static_cast<float>(terrain->config.surfaceStyle);
            row[40] = terrainMinY;
            row[41] = terrainHeightRange;
            row[42] = terrain->config.materialDetailStrength;
            row[43] = terrain->config.uvTiling;
        }

        const uint32_t lodCount = std::clamp(terrain->config.lodLevelCount, 1u, 3u);
        const float distanceScale = terrain->config.lodDistanceScale;
        std::array<int32_t, kTerrainGpuChunksPerEntity> chunkRecords{};
        chunkRecords.fill(-1);
        bool hasRecords = false;

        for (const SubMesh& subMesh : mesh->model->getSubMeshes())
        {
            if (!subMesh.isTerrainChunk)
                continue;
            const uint32_t chunkKey =
                subMesh.terrainChunkZ * 4u + subMesh.terrainChunkX;
            if (chunkKey >= kTerrainGpuChunksPerEntity)
                continue;

            int32_t& recordIndex = chunkRecords[chunkKey];
            if (recordIndex < 0)
            {
                if (recordCursor >= kTerrainGpuMaxRecords)
                    break;
                recordIndex = static_cast<int32_t>(recordCursor++);
                TerrainGpuChunkRecord& record = records[recordIndex];
                record = TerrainGpuChunkRecord{};
                record.worldMatrix = worldTransform->matrix;
                record.boundsMin = glm::vec4(subMesh.boundsMin, 1.0f);
                record.boundsMax = glm::vec4(subMesh.boundsMax, 1.0f);
                record.lodParams = glm::vec4(distanceScale, static_cast<float>(lodCount),
                                             0.0f, 0.0f);
                record.commandParams = glm::uvec4(
                    slotIndex * kTerrainGpuCommandsPerEntity,
                    slotIndex * kTerrainGpuChunksPerEntity + chunkKey,
                    slot.resetPending ? 1u : 0u,
                    slotIndex);
                record.arena = glm::uvec4(arenaIndexBase, arenaVertexBase,
                                          chunkArenaBacked ? 1u : 0u, 0u);
                parityExpected.push_back({});
                hasRecords = true;
            }

            glm::uvec4* lodWord = nullptr;
            if (subMesh.terrainLod == 0u)
                lodWord = &records[recordIndex].lod0;
            else if (subMesh.terrainLod == 1u)
                lodWord = &records[recordIndex].lod1;
            else if (subMesh.terrainLod == 2u)
                lodWord = &records[recordIndex].lod2;
            if (lodWord)
            {
                *lodWord = glm::uvec4(subMesh.firstIndex, subMesh.indexCount, 1u, 0u);
            }
        }

        if (!hasRecords)
        {
            slot.resetPending = false;
            continue;
        }

        if (frameInfo.settings.terrainGpuSelectionSelfTest)
        {
            auto& parityStates = terrainGpuParityStates[frameInfo.frameInFlightIndex];
            for (uint32_t chunkKey = 0; chunkKey < kTerrainGpuChunksPerEntity; ++chunkKey)
            {
                const int32_t recordIndex = chunkRecords[chunkKey];
                if (recordIndex < 0)
                    continue;

                const uint32_t stateIndex =
                    slotIndex * kTerrainGpuChunksPerEntity + chunkKey;
                render::TerrainLodState& state = parityStates[stateIndex];
                if (slot.resetPending)
                {
                    state = {};
                    // Reset this chunk's persistent state in every frame slot,
                    // not only the slot currently being recorded.
                    for (auto& frame : terrainGpuFrames)
                    {
                        if (!frame.state.mapped)
                            continue;
                        auto* words = static_cast<int32_t*>(frame.state.mapped) +
                                      stateIndex * 4u;
                        std::memset(words, 0, 4u * sizeof(int32_t));
                        vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                                           frame.state.allocation,
                                           static_cast<VkDeviceSize>(stateIndex) *
                                               4u * sizeof(int32_t),
                                           4u * sizeof(int32_t));
                    }
                }
                std::array<uint32_t, kTerrainGpuDebugWords> expected{};
                expected[3] = terrainGpuFloatBits(1.0f);

                const TerrainGpuChunkRecord& record = records[recordIndex];
                bool visible = culler.intersects(glm::vec3(record.boundsMin),
                                                 glm::vec3(record.boundsMax),
                                                 record.worldMatrix);
                expected[0] = visible ? 1u : 0u;
                if (visible)
                {
                    glm::vec3 worldMin;
                    glm::vec3 worldMax;
                    render::worldBounds(glm::vec3(record.boundsMin), glm::vec3(record.boundsMax),
                                        record.worldMatrix, worldMin, worldMax);
                    const glm::vec3 closest = glm::clamp(cameraPosition, worldMin, worldMax);
                    const float distance = glm::length(cameraPosition - closest);
                    const float extent = std::max(worldMax.x - worldMin.x,
                                                  worldMax.z - worldMin.z);
                    const render::TerrainLodSelection selection =
                        render::updateTerrainLodSelection(
                            distance, extent, distanceScale, lodCount,
                            gpuDeltaTime, state);
                    expected[1] = selection.lodA;
                    expected[2] = selection.lodB;
                    expected[3] = terrainGpuFloatBits(selection.blend);
                    expected[4] = selection.blending ? 2u : 1u;
                }
                expected[5] = static_cast<uint32_t>(std::max(state.stableLod, 0));
                expected[6] = static_cast<uint32_t>(std::max(state.transitionFrom, 0));
                expected[7] = static_cast<uint32_t>(std::max(state.transitionTo, 0));
                parityExpected[recordIndex] = expected;
            }
        }

        slot.resetPending = false;
        ++run.entityCount;
    }

    terrainBatchActive = terrainBatchActive && recordCursor > 0u;

    frameRes.recordCount = recordCursor;
    if (frameInfo.settings.terrainGpuSelectionSelfTest)
    {
        frameRes.parityExpected = std::move(parityExpected);
    }
    else
    {
        frameRes.parityExpected.clear();
    }

    if (recordCursor > 0u)
    {
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(), frameRes.records.allocation,
                           0, VK_WHOLE_SIZE);
    }
    if (terrainBatchActive)
    {
        vmaFlushAllocation(vulkanDevice.getVmaAllocator(),
                           terrainChunkInstances[frameInfo.frameInFlightIndex].allocation,
                           0, VK_WHOLE_SIZE);
        auto& indirect = terrainIndirectBuffers[frameInfo.frameInFlightIndex];
        vkCmdFillBuffer(commandBuffer, indirect.buffer, 0,
                        static_cast<VkDeviceSize>(kTerrainGpuMaxEntities) *
                            kTerrainGpuCommandsPerEntity * sizeof(VkDrawIndexedIndirectCommand),
                        0u);
        VkMemoryBarrier clearToCompute{};
        clearToCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearToCompute.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &clearToCompute, 0, nullptr, 0, nullptr);
    }
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), frameRes.counts.allocation,
                       0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vulkanDevice.getVmaAllocator(), frameRes.stats.allocation,
                       0, VK_WHOLE_SIZE);

    if (recordCursor > 0u)
    {
        // Record/state buffers are host-written and then reused by the next
        // dispatch in this frame slot. The COMPUTE->COMPUTE leg is required:
        // without it the previous use of this slot's persistent LOD state can
        // still be in flight when the next dispatch reads it.
        VkMemoryBarrier hostAndStateToCompute{};
        hostAndStateToCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostAndStateToCompute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
                                              VK_ACCESS_SHADER_WRITE_BIT;
        hostAndStateToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                              VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &hostAndStateToCompute, 0, nullptr, 0, nullptr);

        terrainGpuPipeline->bind(commandBuffer);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                terrainGpuPipelineLayout, 0, 1,
                                &frameRes.descriptorSet, 0, nullptr);

        TerrainGpuPushConstants push{};
        push.viewProjection = frameInfo.projectionMatrix * frameInfo.viewMatrix;
        push.cameraPosition = glm::vec4(cameraPosition, gpuDeltaTime);
        push.counts = glm::uvec4(recordCursor, frameInfo.frameInFlightIndex, 0u, 0u);
        push.lodTiming = glm::vec4(0.24f, 0.15f, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, terrainGpuPipelineLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(TerrainGpuPushConstants), &push);

        const uint32_t groupCount = (recordCursor + 63u) / 64u;
        vkCmdDispatch(commandBuffer, groupCount, 1, 1);

        VkMemoryBarrier computeToDraw{};
        computeToDraw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        computeToDraw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        computeToDraw.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &computeToDraw, 0, nullptr, 0, nullptr);
        frameRes.debugValid = true;
    }
    else
    {
        frameRes.debugValid = false;
    }

    if (terrainBatchActive && frameInfo.frameNumber % 120u == 0u)
    {
        std::cout << "[Terrain] batched draw | chunks=" << recordCursor
                  << " arenaAllocs=" << (terrainMeshArena ? terrainMeshArena->getAllocationCount() : 0u)
                  << " commandSpan=" << (kTerrainGpuMaxEntities * kTerrainGpuCommandsPerEntity)
                  << std::endl;
    }
    terrainBatchWasActive = terrainBatchActive;
    run.batchActive = terrainBatchActive;
    currentTerrainGpuRun = std::move(run);
}
