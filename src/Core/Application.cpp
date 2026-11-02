#include "Application.h"

// Win32 文件对话框（编辑器 Open/Save 用）。
// 放在引擎侧而不是编辑器层：编辑器不应依赖 windows.h。
// WIN32_LEAN_AND_MEAN 必须在 windows.h 之前定义，NOMINMAX 防止 min/max 宏
// 和 <algorithm> 的 std::min/std::max 打架。
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif
#include "AssetPaths.h"
#include "Core/FrameInfo.h"
#include "Render/GeometryPass.h"
#include "Render/LightingPass.h"
#include "Render/BloomPass.h"
#include "Render/PresentPass.h"
#include "Render/UiCompositePass.h"
#include "Render/EditorOverlayPass.h"
#include "Render/SSAOPass.h"
#include "Render/ComputeProbePass.h"
#include "Render/CrossQueueProbePass.h"
#include "Render/SubresourceProbePass.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_set>
#include <stb_image_write.h>
#include "Core/Profiler.h"
#include "RHI/GpuProfiler.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

FirstApp::FirstApp(const diag::Options& options)
{
	diagnosticsOptions = options;

	// W1：配置必须在最前面读 —— 窗口标题就是配置里的一项，而窗口在
	// initWindow() 里创建，之后再改标题会让用户看到一次闪动。
	// 这里先读进一个局部对象，等 Scene 就绪后再整体搬过去。
	// 读失败（文件不存在/语法错）不是错误：EngineSettings 的默认值就是
	// 设计好的出厂预设，EngineConfig 会在控制台说明原因。
	EngineSettings loadedSettings;
	EngineConfig::load(loadedSettings);

	CameraState loadedCamera;                 // 同样的道理：相机位姿也要在
	const bool hasCamera = EngineConfig::load(loadedCamera);   // Scene 之后才能应用

	// D-6：先启动 CPU worker 池。它不依赖 Vulkan，后续场景加载/地形生成可以复用。
	jobSystem = std::make_unique<JobSystem>();

	initWindow(loadedSettings.windowTitle);
	vulkanDevice = std::make_unique<Device>(window, diagnosticsOptions);

	// 请求尺寸只是期望值。surface 可能因为标题栏/任务栏把它裁小，
	// 交换链创建时会采用 surface 的 currentExtent；渲染资源必须跟着实际尺寸走。
	VkExtent2D requestedExtent{ WIDTH, HEIGHT };
	vulkanSwapchain = std::make_unique<Swapchain>(*vulkanDevice, requestedExtent);
	const VkExtent2D renderExtent = vulkanSwapchain->getSwapChainExtent();
	currentRenderExtent = renderExtent;
	renderer = std::make_unique<Renderer>(*vulkanDevice, *vulkanSwapchain);

	// 最小化必须放在交换链/渲染器创建之后：GLFW 在 iconify 后客户区变成 0x0，
	// 先最小化会让 G-Buffer 附件按 0x0 创建而失败。
	if (window != nullptr && diagnosticsOptions.windowMinimized)
	{
		glfwShowWindow(window);
		glfwIconifyWindow(window);
	}

	// P1-4：无头截图（编译期或运行期任一关闭时 init 直接返回，不创建任何资源）。
	screenshotCapture.init(vulkanDevice->getDevice(), vulkanDevice->getVmaAllocator(),
		vulkanSwapchain->getSwapChainImageFormat(), vulkanSwapchain->getSwapChainExtent(),
		diagnosticsOptions.screenshot);
	if (screenshotCapture.isEnabled() && diagnosticsOptions.screenshotFrame >= 0)
		screenshotCapture.setTargetFrame(static_cast<uint64_t>(diagnosticsOptions.screenshotFrame));
	else
		screenshotAutoTarget = screenshotCapture.isEnabled();

	// P1-2：RenderDoc 抓帧挂钩（编译期关闭或未注入时为空操作）。
	renderDoc.init(diagnosticsOptions.captureFrame > 0, diagnosticsOptions.captureFrame);

	// D-4：每个 frame slot 一套 G-Buffer。两帧同时在飞时共享一套会跨帧读写冲突。
	std::array<GBuffer*, kFramesInFlight> gBufferPtrs{};
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		gBuffers[frame] = std::make_unique<GBuffer>(*vulkanDevice, renderExtent);
		gBufferPtrs[frame] = gBuffers[frame].get();
	}

	// D-1(b)：渲染图必须在各 Pass 之前创建 —— Pass 的 init() 会往里注册资源
	renderGraph = std::make_unique<RenderGraph>(*vulkanDevice);

	// P0-4：GPU 计时。编译期或运行期任一关闭时 init 直接返回，不创建任何资源。
	gpuTimer.init(vulkanDevice->getDevice(), vulkanDevice->getPhysicalDevice(),
		vulkanDevice->findPhysicalQueueFamilies().graphicsFamily.value(),
		kFramesInFlight, diagnosticsOptions.gpuTiming);
	renderGraph->setGpuTimer(&gpuTimer);

	// P1-1：Tracy GPU 上下文（每 pass zone）。编译期关闭时不创建任何资源。
	gpuProfiler.init(vulkanDevice->getPhysicalDevice(), vulkanDevice->getDevice(),
		vulkanDevice->getGraphicsQueue(), renderer->getCommandPool(), true);
	renderGraph->setGpuProfiler(&gpuProfiler);
	if (diagnosticsOptions.gpuTiming)
	{
		if (gpuTimer.isEnabled())
			std::cout << "[Diag] GPU timing enabled -> " << diagnosticsOptions.gpuTimingCsvPath
			          << " (queries=" << gpuTimer.queryPoolCount() << ")\n";
		else
			std::cout << "[Diag] --gpu-timing not active (not compiled in or device lacks timestamps)\n";
	}
	renderResources = std::make_unique<RenderResources>();

	// 注意顺序：Scene::init 里要加载 glTF，而 glTF 材质需要立刻分配描述符槽位，
	// 所以 DescriptorManager 必须先于 Scene 创建并初始化。
	descriptorManager = std::make_unique<DescriptorManager>();
	descriptorManager->init(*vulkanDevice, 256);
	descriptorManager->setFrameTimelineCallbacks(
		[this] { return renderer->getCompletedGraphicsFrame(); },
		[this](uint64_t frame) { renderer->waitForGraphicsFrame(frame); },
		[this] { return renderer->getLastSubmittedGraphicsFrame(); });

	scene = std::make_unique<Scene>();
	// 编辑器模式默认不加载 Sponza（见 Scene::init）。--empty-scene 对运行时构建仍有效。
const bool loadDefaultScene = !diagnosticsOptions.emptyScene && !diagnosticsOptions.editorMode;
scene->init(*vulkanDevice, renderer->getCommandPool(), *descriptorManager,
		loadDefaultScene, jobSystem.get());
	scene->getCameraController().registerCallbacks(window);

	// W1：把配置应用到场景。顺序很重要 —— Scene::init 里会摆好默认相机位姿，
	// 配置只能在那之后再覆盖，否则会被 init 的默认值抹掉。
	scene->getSettings() = loadedSettings;
	// B-03: --terrain-test enables the GPU-vs-CPU selection parity mirror.
	scene->getSettings().terrainGpuSelectionSelfTest = diagnosticsOptions.terrainTest;
	if (hasCamera)
	{
		scene->getCameraTransform().setTranslation(loadedCamera.position);
		scene->getCameraTransform().setEuler(loadedCamera.rotation);
		std::cout << "[Config] 已应用相机位姿 ("
			<< loadedCamera.position.x << ", " << loadedCamera.position.y << ", " << loadedCamera.position.z << ")\n";
	}

	// --terrain-test: persistent end-to-end terrain for render/screenshot runs.
	// Placement mirrors the editor rule: centre in front of the camera, flattened
	// to Y=0 so TerrainConfig::baseHeight remains the visible ground elevation.
	if (diagnosticsOptions.terrainTest)
	{
		std::string gpuTerrainError;
		if (!scene->selfTestTerrainCompute(gpuTerrainError))
			throw std::runtime_error("--terrain-test GPU compute self-test failed: " + gpuTerrainError);

		terrain::TerrainConfig terrainConfig;
		// B-02: render/screenshot regression uses stitched LOD boundaries with
		// skirts disabled. User-created terrain keeps skirts as a fallback.
		terrainConfig.skirtsEnabled = false;
		const TransformComponent& cameraTransform = scene->getCameraTransform();
		glm::vec3 forward(std::sin(cameraTransform.getEuler().y), 0.0f,
			std::cos(cameraTransform.getEuler().y));
		if (glm::dot(forward, forward) < 1e-8f)
			forward = glm::vec3(0.0f, 0.0f, 1.0f);
		else
			forward = glm::normalize(forward);

		const float distance = terrainConfig.sizeZ * 0.5f + 10.0f;
		const glm::vec3 spawnPosition(
			cameraTransform.getTranslation().x + forward.x * distance,
			0.0f,
			cameraTransform.getTranslation().z + forward.z * distance);

		const entt::entity terrainEntity = scene->createTerrainEntity(
			terrainConfig, "Terrain", spawnPosition);
		if (terrainEntity == entt::null)
			throw std::runtime_error("--terrain-test failed to create terrain");

		const auto* terrain = scene->getRegistry().try_get<TerrainComponent>(terrainEntity);
		const auto* terrainMesh = scene->getRegistry().try_get<MeshComponent>(terrainEntity);
		const size_t initialChunks = static_cast<size_t>(terrainConfig.chunkGridX) *
			static_cast<size_t>(terrainConfig.chunkGridZ);
		const size_t initialSubMeshes = (terrainMesh && terrainMesh->model)
			? terrainMesh->model->getSubMeshes().size() : 0u;
		std::cout << "[Terrain] demo entity created"
			<< " | " << terrainConfig.resolutionX << "x" << terrainConfig.resolutionZ
			<< " vertices=" << (terrain ? terrain->vertexCount : 0u)
			<< " indices=" << (terrain ? terrain->indexCount : 0u)
			<< " chunks=" << initialChunks
			<< " lods=" << terrainConfig.lodLevelCount
			<< " submeshes=" << initialSubMeshes
			<< " pos=(" << spawnPosition.x << ", " << spawnPosition.y << ", " << spawnPosition.z << ")\n";

		// Stage 7 regression: submit the same terrain through JobSystem, wait for
		// the main-thread upload, and verify revision/stats before screenshots.
		bool asyncCallback = false;
		bool asyncSuccess = false;
		if (!scene->requestRegenerateTerrain(
				terrainEntity,
				[&asyncCallback, &asyncSuccess](bool success, const std::string&)
				{
					asyncCallback = true;
					asyncSuccess = success;
				}))
		{
			throw std::runtime_error("--terrain-test failed to submit async generation");
		}
		scene->waitForTerrainGeneration();
		const auto* asyncTerrain = scene->getRegistry().try_get<TerrainComponent>(terrainEntity);
		if (!asyncCallback || !asyncSuccess || !asyncTerrain ||
			asyncTerrain->generating || asyncTerrain->revision < 2u)
		{
			throw std::runtime_error("--terrain-test async generation did not complete");
		}
		const auto* asyncMesh = scene->getRegistry().try_get<MeshComponent>(terrainEntity);
		const size_t asyncChunks = static_cast<size_t>(asyncTerrain->config.chunkGridX) *
			static_cast<size_t>(asyncTerrain->config.chunkGridZ);
		const size_t asyncSubMeshes = (asyncMesh && asyncMesh->model)
			? asyncMesh->model->getSubMeshes().size() : 0u;
		std::cout << "[Terrain] async generation PASS | revision="
			<< asyncTerrain->revision << " | " << asyncTerrain->vertexCount
			<< " vertices, " << asyncTerrain->indexCount << " indices, "
			<< asyncChunks << " chunks, " << asyncSubMeshes << " LOD submeshes\n";

		// P0 回归：CPU 网格构建还在 worker 上跑时销毁实体，任务必须先退役再释放。
		// 关掉 GPU 高度场，让 requestRegenerateTerrain() 同步提交 CPU job —— 这样
		// 紧接着的 destroyEntity() 一定落在 worker 仍在写任务字段的窗口里（旧实现
		// 在这里直接 erase shared_ptr，属于 use-after-free）。
		{
			terrain::TerrainConfig retireConfig;
			retireConfig.seed = 6060u;
			retireConfig.resolutionX = 129u;
			retireConfig.resolutionZ = 129u;
			retireConfig.sizeX = 64.0f;
			retireConfig.sizeZ = 64.0f;
			retireConfig.chunkGridX = 2u;   // >1 -> CPU mesh builder
			retireConfig.chunkGridZ = 2u;
			retireConfig.lodLevelCount = 1u;
			retireConfig.skirtsEnabled = false;

			const entt::entity retireEntity = scene->createTerrainEntity(
				retireConfig, "TerrainRetireProbe",
				spawnPosition + glm::vec3(0.0f, 0.0f, -terrainConfig.sizeZ * 4.0f));
			if (retireEntity == entt::null)
				throw std::runtime_error("--terrain-test retirement probe: create failed");

			scene->setTerrainGpuHeightEnabled(false);
			const bool retireSubmitted = scene->requestRegenerateTerrain(
				retireEntity, [](bool, const std::string&) {});
			scene->setTerrainGpuHeightEnabled(true);
			if (!retireSubmitted)
				throw std::runtime_error("--terrain-test retirement probe: submit failed");

			scene->destroyEntity(retireEntity);
			const std::size_t retireTracked = scene->getPendingTerrainBuildCount() +
				scene->getRetiringTerrainBuildCount();
			scene->waitForTerrainGeneration();
			if (retireTracked == 0u || scene->hasPendingTerrainGeneration())
				throw std::runtime_error("--terrain-test retirement probe: task not retired cleanly");

			std::cout << "[Terrain] build task retirement PASS | cpuBuildInFlight=1 tracked="
				<< retireTracked << " drained=1\n";
		}

		// B-05: a 257x257 single-chunk rebuild should spend only submit/poll
		// time on the update thread; GPU mesh assembly stays asynchronous.
		// createTerrainEntity is intentionally synchronous and provides the
		// before/after frame-spike baseline.
		terrain::TerrainConfig b05Config;
		b05Config.seed = 5150u;
		b05Config.resolutionX = 257u;
		b05Config.resolutionZ = 257u;
		b05Config.sizeX = 100.0f;
		b05Config.sizeZ = 100.0f;
		b05Config.heightScale = 18.0f;
		b05Config.chunkGridX = 1u;
		b05Config.chunkGridZ = 1u;
		b05Config.lodLevelCount = 3u;
		b05Config.skirtsEnabled = false;
		b05Config.localColorRange = false;

		const glm::vec3 b05Position = spawnPosition +
			glm::vec3(0.0f, 0.0f, -terrainConfig.sizeZ * 1.5f);
		const auto b05SyncStart = std::chrono::steady_clock::now();
		const entt::entity b05Entity = scene->createTerrainEntity(
			b05Config, "TerrainB05AsyncUpload", b05Position);
		const double b05SyncBaselineMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - b05SyncStart).count();
		const auto* b05Initial = b05Entity != entt::null
			? scene->getRegistry().try_get<TerrainComponent>(b05Entity) : nullptr;
		if (!b05Initial || !b05Initial->meshGeneratedOnGpu ||
			b05Initial->config.chunkGridX != 1u ||
			b05Initial->config.chunkGridZ != 1u)
		{
			throw std::runtime_error(
				"--terrain-test B-05 baseline did not use the single-chunk GPU mesh path");
		}

		const TerrainGpuMeshBuilder::Stats b05StatsBefore = scene->getTerrainGpuMeshStats();
		scene->resetTerrainPollStats();
		bool b05Callback = false;
		bool b05Success = false;
		if (!scene->requestRegenerateTerrain(
				b05Entity,
				[&b05Callback, &b05Success](bool success, const std::string&)
				{
					b05Callback = true;
					b05Success = success;
				}))
		{
			throw std::runtime_error("--terrain-test failed to submit B-05 async rebuild");
		}
		scene->waitForTerrainGeneration();
		const auto* b05After = scene->getRegistry().try_get<TerrainComponent>(b05Entity);
		const TerrainGpuMeshBuilder::Stats b05StatsAfter = scene->getTerrainGpuMeshStats();
		const uint64_t b05MeshSubmits = b05StatsAfter.submitted - b05StatsBefore.submitted;
		const double b05MaxPollMs = scene->getMaxTerrainPollMs();
		if (!b05Callback || !b05Success || !b05After || b05After->generating ||
			b05After->revision < 2u || !b05After->meshGeneratedOnGpu ||
			b05After->generationBackend.find("GPU mesh") == std::string::npos ||
			b05MeshSubmits == 0u)
		{
			throw std::runtime_error(
				"--terrain-test B-05 async GPU rebuild did not complete on the GPU path");
		}
		if (b05MaxPollMs >= b05SyncBaselineMs)
		{
			throw std::runtime_error(
				"--terrain-test B-05 async poll exceeded the synchronous baseline");
		}
		const double b05SpikeRatio = b05MaxPollMs > 1e-6
			? b05SyncBaselineMs / b05MaxPollMs : 0.0;
		std::cout << "[Terrain] async GPU upload PASS"
			<< " | resolution=" << b05Config.resolutionX << "x" << b05Config.resolutionZ
			<< " chunks=1"
			<< " syncBaseline=" << b05SyncBaselineMs << "ms"
			<< " asyncMaxPoll=" << b05MaxPollMs << "ms"
			<< " maxSubmit=" << b05StatsAfter.maxSubmitMs << "ms"
			<< " generation=" << b05After->lastGenerationMs << "ms"
			<< " spikeRatio=" << b05SpikeRatio
			<< " revision=" << b05After->revision
			<< " gpuMesh=1\n";

		scene->destroyEntity(b05Entity);

		// B-01 smoke test: independent chunk entities stream around the camera,
		// own separate Models, unload cleanly, and sample one continuous field.
		terrain::TerrainConfig streamingConfig;
		streamingConfig.seed = 9001u;
		streamingConfig.resolutionX = 17u;
		streamingConfig.resolutionZ = 17u;
		streamingConfig.sizeX = 16.0f;
		streamingConfig.sizeZ = 16.0f;
		streamingConfig.heightScale = 3.0f;
		streamingConfig.lodLevelCount = 2u;
streamingConfig.skirtsEnabled = false;
		streamingConfig.localColorRange = false;
		const entt::entity streamingManager = scene->createStreamingTerrain(
			streamingConfig, 1, 3, "TerrainStreamingSmoke");
		if (streamingManager == entt::null)
			throw std::runtime_error("--terrain-test streaming manager creation failed");

		const auto loadedStreamingChunks = [&]()
		{
			size_t loaded = 0;
			for (auto [entity, chunk, terrain, mesh] :
				scene->getRegistry().view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
			{
				(void)entity;
				(void)chunk;
				if (terrain.generated && !terrain.generating && mesh.model &&
					terrain.cpuHeightField && terrain.cpuHeightField->generatedOnGpu)
					++loaded;
			}
			return loaded;
		};

		for (uint32_t frame = 0; frame < 20 && loadedStreamingChunks() < 9u; ++frame)
		{
			scene->refreshStreamingTerrain();
			scene->waitForTerrainGeneration();
		}
		if (scene->getStreamingTerrainChunkCount() != 9u ||
			loadedStreamingChunks() != 9u)
			throw std::runtime_error("--terrain-test streaming chunk set did not settle");

		std::unordered_set<const Model*> streamingModels;
		for (auto [entity, chunk, terrain, mesh] :
			scene->getRegistry().view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
		{
			(void)entity;
			(void)chunk;
			(void)terrain;
			streamingModels.insert(mesh.model.get());
		}
		if (streamingModels.size() != 9u)
			throw std::runtime_error("--terrain-test streaming chunks are not independent Models");

		scene->refreshTransformsAndVisibility();
		const glm::vec3 streamingFocus = scene->getCameraTransform().getTranslation();
		const int32_t centerChunkX =
			static_cast<int32_t>(std::floor(streamingFocus.x / streamingConfig.sizeX));
		const int32_t centerChunkZ =
			static_cast<int32_t>(std::floor(streamingFocus.z / streamingConfig.sizeZ));
		std::vector<entt::entity> regionChunks;
		scene->queryTerrainChunksInRegionRange(
			centerChunkX - 2, centerChunkZ - 2,
			centerChunkX + 2, centerChunkZ + 2, regionChunks);
		if (regionChunks.size() != 9u)
			throw std::runtime_error("--terrain-test region query did not return 9 streamed chunks");
		for (entt::entity chunkEntity : regionChunks)
		{
			const auto* chunk = scene->getRegistry().try_get<TerrainChunkComponent>(chunkEntity);
			if (!chunk || chunk->world != streamingManager)
				throw std::runtime_error("--terrain-test region query returned a non-chunk entity");
		}
		std::cout << "[Terrain] spatial region query PASS | chunks=9 regions=5x5\n";

		// B-07: a small brush at the centre of one streamed chunk must rebuild
		// only that chunk; untouched chunks keep their revision and Model.
		struct ChunkSnapshot
		{
			entt::entity entity = entt::null;
			uint64_t revision = 0;
			const Model* model = nullptr;
		};
		std::vector<ChunkSnapshot> chunkSnapshots;
		for (auto [entity, chunk, terrain, mesh] :
			scene->getRegistry().view<TerrainChunkComponent, TerrainComponent, MeshComponent>().each())
		{
			(void)chunk;
			chunkSnapshots.push_back({ entity, terrain.revision, mesh.model.get() });
		}

		// B-07: pick a stream point where the brush is provably alone. The demo
		// 257x257 terrain sits in front of the camera and the camera pose saved in
		// config/engine.json shifts the 3x3 stream centre, so isolation is measured
		// instead of assumed from the chunk row: the ground query must resolve to
		// the chunk itself and no other terrain bounds may come within brush reach.
		const float kBrushRadius = 2.5f;
		const float kSampleFractions[] = { 0.0f, 0.45f, -0.45f, 0.25f, -0.25f };
		const auto brushAvoidsOtherTerrain = [&](entt::entity target, float worldX,
			float worldZ)
		{
			for (auto [entity, terrain, bounds] :
				scene->getRegistry().view<TerrainComponent, WorldBoundsComponent>().each())
			{
				(void)terrain;
				if (entity == target || !bounds.world.valid)
					continue;
				const auto& box = bounds.world;
				if (worldX + kBrushRadius >= box.min.x &&
					worldX - kBrushRadius <= box.max.x &&
					worldZ + kBrushRadius >= box.min.z &&
					worldZ - kBrushRadius <= box.max.z)
					return false;
			}
			return true;
		};
		entt::entity sculptChunk = entt::null;
		float sculptX = 0.0f;
		float sculptZ = 0.0f;
		float sculptGround = 0.0f;
		for (int32_t pass = 0; pass < 2 && sculptChunk == entt::null; ++pass)
		{
			for (const ChunkSnapshot& snapshot : chunkSnapshots)
			{
				const auto* chunk =
					scene->getRegistry().try_get<TerrainChunkComponent>(snapshot.entity);
				const auto* terrain =
					scene->getRegistry().try_get<TerrainComponent>(snapshot.entity);
				const auto* transform =
					scene->getRegistry().try_get<TransformComponent>(snapshot.entity);
				if (!chunk || !terrain || !transform || !terrain->cpuHeightField)
					continue;
				if (pass == 0 && chunk->chunkZ != centerChunkZ - 1)
					continue;
				const terrain::HeightField& field = *terrain->cpuHeightField;
				const glm::vec3 center = transform->getTranslation();
				for (float fractionZ : kSampleFractions)
				{
					for (float fractionX : kSampleFractions)
					{
						const float sampleX = center.x + field.sizeX * 0.5f * fractionX;
						const float sampleZ = center.z + field.sizeZ * 0.5f * fractionZ;
						float ground = 0.0f;
						entt::entity groundOwner = entt::null;
						if (!scene->sampleTerrainGroundHeightWorld(
							sampleX, sampleZ, ground, &groundOwner) ||
							groundOwner != snapshot.entity)
							continue;
						if (!brushAvoidsOtherTerrain(snapshot.entity, sampleX, sampleZ))
							continue;
						sculptChunk = snapshot.entity;
						sculptX = sampleX;
						sculptZ = sampleZ;
						sculptGround = ground;
						break;
					}
					if (sculptChunk != entt::null)
						break;
				}
				if (sculptChunk != entt::null)
					break;
			}
		}
		if (sculptChunk == entt::null)
			throw std::runtime_error("--terrain-test B-07 could not find an isolated streamed chunk");
		entt::entity groundEntity = entt::null;
		float queriedGround = 0.0f;
		if (!scene->sampleTerrainGroundHeightWorld(sculptX, sculptZ, queriedGround,
			&groundEntity) || groundEntity != sculptChunk)
			throw std::runtime_error("--terrain-test B-07 sculpt ground query failed");
		sculptGround = queriedGround;

		terrain::TerrainBrushSettings brush;
		brush.center = glm::vec3(sculptX, sculptGround, sculptZ);
		brush.radius = kBrushRadius;
		brush.strength = 0.45f;
		brush.mode = terrain::TerrainBrushMode::Raise;
		Scene::TerrainSculptResult sculptResult;
		std::string sculptError;
		if (!scene->sculptTerrain(brush, sculptResult, sculptError))
			throw std::runtime_error("--terrain-test B-07 sculpt failed: " + sculptError);
		if (sculptResult.rebuiltEntities != 1u || sculptResult.rebuilt.size() != 1u ||
			sculptResult.rebuilt[0] != sculptChunk || sculptResult.modifiedVertices == 0u)
			throw std::runtime_error("--terrain-test B-07 sculpt rebuilt an unexpected chunk set");

		for (const ChunkSnapshot& snapshot : chunkSnapshots)
		{
			const auto* terrain = scene->getRegistry().try_get<TerrainComponent>(snapshot.entity);
			const auto* mesh = scene->getRegistry().try_get<MeshComponent>(snapshot.entity);
			if (!terrain || !mesh || !mesh->model)
				throw std::runtime_error("--terrain-test B-07 sculpt lost a streamed chunk");
			if (snapshot.entity == sculptChunk)
			{
				if (terrain->revision != snapshot.revision + 1u ||
					mesh->model.get() == snapshot.model)
					throw std::runtime_error("--terrain-test B-07 sculpt did not replace the target chunk");
			}
			else if (terrain->revision != snapshot.revision ||
					 mesh->model.get() != snapshot.model)
			{
				throw std::runtime_error("--terrain-test B-07 sculpt rebuilt an untouched chunk");
			}
		}
		std::cout << "[Terrain] sculpt PASS | chunks=9 rebuilt=1 vertices="
			<< sculptResult.modifiedVertices << " radius=2.5 mode=Raise\n";

		// B-07: kinematic controller falls to the sculpted ground, walks across
		// the hill in both directions, and lands after a jump without oscillating.
		float characterGround = 0.0f;
		if (!scene->sampleTerrainGroundHeightWorld(sculptX, sculptZ, characterGround, nullptr))
			throw std::runtime_error("--terrain-test B-07 character ground query failed");
		const entt::entity character = scene->createCharacterEntity(
			"TerrainB07Character", glm::vec3(sculptX, characterGround + 3.0f, sculptZ));
		if (character == entt::null)
			throw std::runtime_error("--terrain-test B-07 character creation failed");

		CharacterMoveResult characterMove;
		float maxGroundedError = 0.0f;
		uint32_t groundedFrames = 0u;
		for (uint32_t frame = 0; frame < 180u; ++frame)
		{
			if (!scene->updateCharacterController(character, glm::vec3(0.0f), false,
												  1.0f / 60.0f, characterMove))
				throw std::runtime_error("--terrain-test B-07 character update failed");
			float ground = 0.0f;
			if (scene->sampleTerrainGroundHeightWorld(characterMove.position.x,
													  characterMove.position.z,
													  ground, nullptr))
			{
				if (characterMove.grounded)
				{
					++groundedFrames;
					maxGroundedError = std::max(maxGroundedError,
						std::abs(characterMove.position.y - ground));
				}
			}
		}
		if (!characterMove.grounded || groundedFrames < 120u || maxGroundedError > 0.08f)
			throw std::runtime_error("--terrain-test B-07 character did not settle on the ground");

		const float walkStartX = characterMove.position.x;
		float walkMaxX = walkStartX;
		for (uint32_t frame = 0; frame < 60u; ++frame)
		{
			if (!scene->updateCharacterController(character, glm::vec3(1.0f, 0.0f, 0.0f),
												  false, 1.0f / 60.0f, characterMove))
				throw std::runtime_error("--terrain-test B-07 character walk failed");
			walkMaxX = std::max(walkMaxX, characterMove.position.x);
			float ground = 0.0f;
			if (characterMove.grounded &&
				scene->sampleTerrainGroundHeightWorld(characterMove.position.x,
													  characterMove.position.z,
													  ground, nullptr))
				maxGroundedError = std::max(maxGroundedError,
					std::abs(characterMove.position.y - ground));
		}
		for (uint32_t frame = 0; frame < 60u; ++frame)
		{
			if (!scene->updateCharacterController(character, glm::vec3(-1.0f, 0.0f, 0.0f),
												  false, 1.0f / 60.0f, characterMove))
				throw std::runtime_error("--terrain-test B-07 character walk failed");
			float ground = 0.0f;
			if (characterMove.grounded &&
				scene->sampleTerrainGroundHeightWorld(characterMove.position.x,
													  characterMove.position.z,
													  ground, nullptr))
				maxGroundedError = std::max(maxGroundedError,
					std::abs(characterMove.position.y - ground));
		}
		if (walkMaxX - walkStartX < 1.0f ||
			maxGroundedError > 0.12f)
			throw std::runtime_error("--terrain-test B-07 character traversal was unstable");
		float jumpGround = 0.0f;
		scene->sampleTerrainGroundHeightWorld(characterMove.position.x,
											  characterMove.position.z,
											  jumpGround, nullptr);
		float peakY = characterMove.position.y;
		if (!scene->updateCharacterController(character, glm::vec3(0.0f), true,
											  1.0f / 60.0f, characterMove))
			throw std::runtime_error("--terrain-test B-07 character jump failed");
		for (uint32_t frame = 0; frame < 150u; ++frame)
		{
			if (!scene->updateCharacterController(character, glm::vec3(0.0f), false,
												  1.0f / 60.0f, characterMove))
				throw std::runtime_error("--terrain-test B-07 character jump failed");
			peakY = std::max(peakY, characterMove.position.y);
		}
		if (!characterMove.grounded || peakY < jumpGround + 0.4f)
			throw std::runtime_error("--terrain-test B-07 character jump did not land");
		std::cout << "[Terrain] character controller PASS | grounded=1"
			<< " maxGroundError=" << maxGroundedError
			<< " jumpHeight=" << (peakY - jumpGround)
			<< " walkDistance=" << (walkMaxX - walkStartX) << std::endl;

		bool characterSerialized = false;
		for (const scene::EntityDescription& described : scene->captureDescription().entities)
		{
			if (!described.isCharacter)
				continue;
			characterSerialized =
				std::abs(described.characterRadius - 0.35f) < 1e-5f &&
				std::abs(described.characterHeight - 1.8f) < 1e-5f &&
				std::abs(described.characterMoveSpeed - 6.0f) < 1e-5f;
			break;
		}
		if (!characterSerialized)
			throw std::runtime_error("--terrain-test B-07 character capture failed");
		std::cout << "[Terrain] character serialization PASS | captured=1" << std::endl;

		scene->destroyEntity(character);

		// B-08: exercise the bounded material library with one real external
		// 16-PNG set and three procedural fallbacks. The set index is part of
		// TerrainConfig, so it must survive scene description capture.
		const std::filesystem::path materialDir =
			std::filesystem::path(".tmp") / "b08_material_set1";
		std::filesystem::remove_all(materialDir);
		std::filesystem::create_directories(materialDir);
		const char* mapNames[] = { "albedo", "normal", "height", "roughness" };
		const char* layerNames[] = { "sand", "grass", "rock", "snow" };
		std::array<std::array<unsigned char, 64>, 4> mapPixels{};
		for (uint32_t layer = 0; layer < 4; ++layer)
		{
			for (uint32_t pixel = 0; pixel < 16; ++pixel)
			{
				const uint32_t i = pixel * 4u;
				mapPixels[0][i + 0u] = static_cast<unsigned char>(64u + layer * 40u);
				mapPixels[0][i + 1u] = static_cast<unsigned char>(160u + layer * 12u);
				mapPixels[0][i + 2u] = static_cast<unsigned char>(96u + layer * 8u);
				mapPixels[0][i + 3u] = 255u;

				mapPixels[1][i + 0u] = 128u;
				mapPixels[1][i + 1u] = 128u;
				mapPixels[1][i + 2u] = 255u;
				mapPixels[1][i + 3u] = 255u;

				mapPixels[2][i + 0u] = static_cast<unsigned char>(80u + layer * 20u);
				mapPixels[2][i + 1u] = 128u;
				mapPixels[2][i + 2u] = 0u;
				mapPixels[2][i + 3u] = 255u;

				mapPixels[3][i + 0u] = 220u;
				mapPixels[3][i + 1u] = 220u;
				mapPixels[3][i + 2u] = 220u;
				mapPixels[3][i + 3u] = 255u;
			}
		}
		for (uint32_t mapIndex = 0; mapIndex < 4; ++mapIndex)
		{
			for (uint32_t layer = 0; layer < 4; ++layer)
			{
				const std::string filename = std::string(mapNames[mapIndex]) + "_" +
					layerNames[layer] + ".png";
				const std::string path = (materialDir / filename).string();
				if (stbi_write_png(path.c_str(), 4, 4, 4,
					mapPixels[mapIndex].data(), 4 * 4) == 0)
					throw std::runtime_error("--terrain-test B-08 could not write external PNG");
			}
		}

		std::string materialError;
		if (!scene->ensureTerrainMaterialSetFromDirectory(1, materialDir.string(), materialError))
			throw std::runtime_error("--terrain-test B-08 external material set failed: " + materialError);
		for (uint32_t materialSet = 2; materialSet < 4; ++materialSet)
		{
			if (!scene->ensureTerrainMaterialSet(materialSet, materialError))
				throw std::runtime_error("--terrain-test B-08 material set failed: " + materialError);
		}
		const TerrainMaterial::Stats materialStats = scene->getTerrainMaterialStats();
		if (materialStats.residentSets != 4u || materialStats.maxSets != 4u ||
			materialStats.externalSets != 1u || materialStats.proceduralSets != 3u)
			throw std::runtime_error("--terrain-test B-08 material set residency was unexpected");
		if (scene->ensureTerrainMaterialSet(4, materialError))
			throw std::runtime_error("--terrain-test B-08 accepted an out-of-range material set");

		terrain::TerrainConfig materialConfig;
		materialConfig.resolutionX = 33u;
		materialConfig.resolutionZ = 33u;
		materialConfig.sizeX = 24.0f;
		materialConfig.sizeZ = 24.0f;
		materialConfig.chunkGridX = 1u;
		materialConfig.chunkGridZ = 1u;
		materialConfig.lodLevelCount = 1u;
		materialConfig.skirtsEnabled = false;
		materialConfig.localColorRange = false;
		materialConfig.materialSetIndex = 1u;
		const entt::entity materialTerrain = scene->createTerrainEntity(
			materialConfig, "B08 Material Terrain", glm::vec3(0.0f, 0.0f, -80.0f));
		if (materialTerrain == entt::null)
			throw std::runtime_error("--terrain-test B-08 material terrain creation failed");
		const auto* materialTerrainComponent =
			scene->getRegistry().try_get<TerrainComponent>(materialTerrain);
		if (!materialTerrainComponent || materialTerrainComponent->config.materialSetIndex != 1u ||
			!materialTerrainComponent->generated)
			throw std::runtime_error("--terrain-test B-08 material terrain lost its set index");
		bool materialSerialized = false;
		for (const scene::EntityDescription& described : scene->captureDescription().entities)
		{
			if (described.isTerrain && described.terrainConfig.materialSetIndex == 1u)
			{
				materialSerialized = true;
				break;
			}
		}
		if (!materialSerialized)
			throw std::runtime_error("--terrain-test B-08 material set index was not captured");
		scene->destroyEntity(materialTerrain);
		std::filesystem::remove_all(materialDir);
		std::cout << "[Terrain] material sets PASS | resident=" << materialStats.residentSets
			<< " max=" << materialStats.maxSets
			<< " external=" << materialStats.externalSets
			<< " fallback=" << materialStats.proceduralSets
			<< " serialized=1" << std::endl;

		scene->destroyEntity(streamingManager);
		if (scene->hasStreamingTerrain() ||
			scene->getRegistry().view<TerrainChunkComponent>().size() != 0u)
			throw std::runtime_error("--terrain-test streaming cleanup leaked chunk entities");
		std::cout << "[Terrain] streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9\n";
	}
	loadAllPBRTextures();

	// 烘焙 IBL：必须在 buildGlobalSet 之前完成，因为需要把结果写进描述符集。
	// 这一步是纯 GPU 计算（irradiance 卷积 / GGX prefilter / BRDF 积分表），
	// 放在这里是因为此时环境贴图已经上传完毕。
	iblBaker = std::make_unique<IBLBaker>(*vulkanDevice, renderer->getCommandPool());
	iblBaker->bake(*environmentTex);
	createUniformBuffers();
	initMaterials();

	auto geoPass = std::make_unique<GeometryPass>(*vulkanDevice, gBufferPtrs, *renderGraph, *renderResources,
		descriptorManager->getGlobalSetLayout(), descriptorManager->getMaterialSetLayout(),
		descriptorManager->getTerrainSetLayout());
	auto ssaoPass = std::make_unique<SSAOPass>(*vulkanDevice, *renderGraph, *renderResources,
		descriptorManager->getGlobalSetLayout(), descriptorManager.get());
	auto lightPass = std::make_unique<LightingPass>(*vulkanDevice, *gBuffers[0], *vulkanSwapchain,
		*renderGraph, *renderResources, descriptorManager->getGlobalSetLayout());
	// D-2 的两个新成员：Bloom 只负责在 HDR 图上生成辉光，
	// PresentPass 负责把 HDR + 辉光变成最终显示值并叠加 ImGui。
	auto bloomPass = std::make_unique<BloomPass>(*vulkanDevice, *renderGraph, *renderResources,
		descriptorManager.get());
	auto presentPass = std::make_unique<PresentPass>(*vulkanDevice, *vulkanSwapchain, *renderGraph,
		*renderResources, descriptorManager.get(), descriptorManager->getGlobalSetLayout());

	// TD-09：编辑器模式下最后一步是「只画 ImGui」的合成 Pass。
	// 它在非编辑器模式下不会往渲染图里注册任何东西（见 addToGraph 的早退）。
	auto editorOverlay = std::make_unique<EditorOverlayPass>(*vulkanDevice, *renderGraph, *renderResources);
	auto uiComposite = std::make_unique<UiCompositePass>(*vulkanDevice, *renderGraph, *renderResources);

	auto computeProbePass = std::make_unique<ComputeProbePass>(*vulkanDevice, *renderGraph,
		descriptorManager.get());
	auto crossQueueProbePass = std::make_unique<CrossQueueProbePass>(*vulkanDevice, *renderGraph,
		descriptorManager.get());
	auto subresourceProbePass = std::make_unique<SubresourceProbePass>(*vulkanDevice, *renderGraph);

	presentPassPtr = presentPass.get();
	uiCompositePassPtr = uiComposite.get();
	editorOverlayPassPtr = editorOverlay.get();
	ssaoPassPtr = ssaoPass.get();
	computeProbePassPtr = computeProbePass.get();
	crossQueueProbePassPtr = crossQueueProbePass.get();
	subresourceProbePassPtr = subresourceProbePass.get();

	renderPipeline.addPass(std::move(geoPass));
	renderPipeline.addPass(std::move(ssaoPass));
	renderPipeline.addPass(std::move(lightPass));
	renderPipeline.addPass(std::move(bloomPass));
	renderPipeline.addPass(std::move(presentPass));
	// TD-15：带深度测试的包围盒线框，必须在 Present 之后、UiComposite 之前。
	renderPipeline.addPass(std::move(editorOverlay));
	renderPipeline.addPass(std::move(uiComposite));
	renderPipeline.addPass(std::move(crossQueueProbePass));
	renderPipeline.addPass(std::move(subresourceProbePass));
	renderPipeline.addPass(std::move(computeProbePass));

	renderPipeline.initAll();

	// ---- 把跨帧常驻的只读资源导入渲染图 ----
	// initialLayout 给 SHADER_READ_ONLY，渲染图会把它们标成「跨帧常驻」，
	// 每帧不复位布局记录（否则会被当成「内容未定义」而丢弃）。
	auto importPersistent = [&](const char* name, VkImage img, VkImageView view,
		VkFormat fmt, uint32_t layers, VkImageCreateFlags flags)
	{
		RenderGraph::TextureDesc d;
		d.name = name;
		d.format = fmt;
		d.extent = vulkanSwapchain->getSwapChainExtent();
		d.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		d.arrayLayers = layers;
		d.flags = flags;
		return renderGraph->importTexture(d, img, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	};

	const VkImageCreateFlags cubeFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	renderResources->environment = importPersistent("Environment",
		environmentTex->getImage(), environmentTex->getView(), VK_FORMAT_R32G32B32A32_SFLOAT, 1, 0);
	renderResources->irradiance = importPersistent("IBL.Irradiance",
		iblBaker->getIrradianceMap().getImage(), iblBaker->getIrradianceMap().getView(),
		VK_FORMAT_R16G16B16A16_SFLOAT, 6, cubeFlags);
	renderResources->prefiltered = importPersistent("IBL.Prefiltered",
		iblBaker->getPrefilteredMap().getImage(), iblBaker->getPrefilteredMap().getView(),
		VK_FORMAT_R16G16B16A16_SFLOAT, 6, cubeFlags);
	renderResources->brdfLut = importPersistent("IBL.BrdfLut",
		iblBaker->getBrdfLutImage(), iblBaker->getBrdfLutView(), VK_FORMAT_R16G16_SFLOAT, 1, 0);

	rebuildGlobalDescriptors();

	// D-1(b)：ImGui 改用动态渲染，只需要交换链颜色格式。
	// D-2   ：UI 的绘制不再属于光照通道 —— 它由 PresentPass 在最终图像上叠加，
	//         这样以后往 Present 之前插任何后效都不会把 UI 一起糊掉。
	imguiSystem = std::make_unique<ImGuiSystem>(window, *vulkanDevice,
		vulkanSwapchain->getSwapChainImageFormat(), vulkanSwapchain->imageCount(),
		renderer->getCommandPool());
	presentPassPtr->setImGuiSystem(imguiSystem.get());

	currentTime = std::chrono::high_resolution_clock::now();
	lastPerfLog = currentTime;

}

FirstApp::~FirstApp()
{
	// =====================================================================
	// 【为什么这里要把销毁顺序全部显式写出来】
	//
	// 原来的写法是：函数体里先 glfwDestroyWindow + glfwTerminate，其余成员
	// 等析构体结束后按逆序自动销毁。两个问题：
	//
	//   1) 窗口和 VkSurface 先没了，Device / Swapchain / ImGuiSystem 才开始
	//      销毁。能跑通只是因为当前驱动容忍度高。
	//
	//   2) 更隐蔽的一个：**renderPipeline 是值成员，而它在 Application.h 里
	//      声明得比 vulkanDevice 晚，所以成员逆序析构时它排在 vulkanDevice
	//      之后销毁** —— 也就是说 Pass 持有的 VMA 分配（SSAOPass 的 UBO 等）
	//      会在 vmaDestroyAllocator 之后才被释放，直接触发 VMA 的
	//      "Some allocations were not freed" 断言崩溃。
	//
	//      这个 bug 一直存在，只是以前没暴露：验证脚本是用
	//      TerminateProcess 杀进程的，而 TerminateProcess 根本不跑析构函数。
	//      直到 W1 需要"退出自动保存配置"、改成 WM_CLOSE 正常关窗，它才浮出来。
	//
	// 结论：不要依赖成员声明顺序来表达销毁依赖 —— 有人插一个新成员就会
	// 悄悄破坏它，而且编译器一声不吭。下面按"依赖方向"显式排好。
	// =====================================================================
	vkDeviceWaitIdle(vulkanDevice->getDevice());

	// 编辑器只持有引用（Scene / DescriptorManager / ImGui），
	// 但它必须在这些对象之前销毁 —— 否则它持有的 Scene* 会先失效。
	shutdownEditor();

	finalizeDiagnostics(); 
	// W1：退出时保存。放在析构里而不是 run() 的循环之后，是因为自检和异常
	// 退出也走析构，这样"改完参数直接关窗口"这条最常用的路径一定不会丢。
	// 但自检模式要跳过：它会把 bloom/ssao 关掉，照常保存就污染了用户配置。
	if (!suppressAutoSave)
		saveConfig();
	else
		std::cout << "[Config] 自检模式：跳过退出自动保存，避免污染用户配置\n";

	// 1) Pass 持有 GPU 资源（各自的 UBO / 采样器 / 管线），必须最先销毁。
	renderPipeline.clearAll();

	// 2) 依赖窗口的东西：ImGui 的 GLFW 后端持有窗口指针，
	//    Swapchain 的 image view 依赖 VkSurface（surface 又依赖窗口）。
	imguiSystem.reset();
	vulkanSwapchain.reset();

	// 3) 其余 GPU 资源
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		if (globalUboBuffers[frame] != VK_NULL_HANDLE)
			vmaDestroyBuffer(vulkanDevice->getVmaAllocator(),
				globalUboBuffers[frame], globalUboAllocations[frame]);
	}

	descriptorManager->cleanup();
	scene->cleanup();

	environmentTex.reset();
	iblBaker.reset();
	renderGraph.reset();          // 渲染图持有大量 image/view，必须在 Device 之前
	renderResources.reset();
	for (auto& gBuffer : gBuffers)
		gBuffer.reset();
	renderer.reset();
	descriptorManager.reset();
	scene.reset();

	// 4) Device 最后（它会销毁 VMA 分配器和 VkDevice）
	vulkanDevice.reset();

	// 5) 窗口系统收尾
	glfwDestroyWindow(window);
	glfwTerminate();
}

void FirstApp::initWindow(const std::string& title)
{
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	// 回归/截图这类无人值守运行默认不显示窗口：否则每次跑测试都会弹到最前，
	// 盖住用户正在做的事。隐藏窗口仍保留真实客户区尺寸，交换链与离屏渲染不变。
	// 最小化模式同样先隐藏创建，等渲染器就绪后再 show+iconify，避免启动瞬间
	// 有一次可见窗口抢焦点。
	if (diagnosticsOptions.windowHidden || diagnosticsOptions.windowMinimized)
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	window = glfwCreateWindow(WIDTH, HEIGHT, title.c_str(), nullptr, nullptr);
	const char* presentation = diagnosticsOptions.windowMinimized ? "minimized"
		: (diagnosticsOptions.windowHidden ? "hidden" : "visible");
	std::cout << "[Window] presentation=" << presentation << " | " << WIDTH << "x" << HEIGHT
		<< " (--hidden/--minimized/--visible)" << std::endl;
}

void FirstApp::setResizeTestEnabled(bool enabled)
{
	resizeTestEnabled = enabled;
}

void FirstApp::setMaterialStressEnabled(bool enabled)
{
	materialStressEnabled = enabled;
}

void FirstApp::materialStressStep()
{
	if (materialStressPendingSlot != kInvalidMaterialSlot)
	descriptorManager->releaseMaterialSet(materialStressPendingSlot);

	auto material = scene->getMaterialSystem().getOrCreateMaterial("__TD04Stress");
	materialStressPendingSlot = descriptorManager->allocateMaterialSet(*material);
	++materialStressCycles;
	materialStressMaxPending = std::max(materialStressMaxPending,
		descriptorManager->getPendingMaterialReleaseCount());
}

void FirstApp::setComputeProbeEnabled(bool enabled)
{
	computeProbePassPtr->setEnabled(enabled);
}

void FirstApp::setSubresourceTestEnabled(bool enabled)
{
if (subresourceProbePassPtr != nullptr)
subresourceProbePassPtr->setMode(enabled
? SubresourceProbePass::Mode::Positive
: SubresourceProbePass::Mode::Disabled);
}

int FirstApp::runDeviceFaultSelfTest()
{
	suppressAutoSave = true;

	// P2-4：注意 VUID-vkGetDeviceFaultInfoEXT-device-07336 —— 该扩展只能在设备
	// "已 lost" 时调用，所以这里只校验"能力是否就绪"（扩展已启用 + 入口点已解析），
	// 真正的查询放在 Renderer 检测到 VK_ERROR_DEVICE_LOST 的那条路径上。
	const bool supported = vulkanDevice->isDeviceFaultSupported();
	const bool entryPoint = vulkanDevice->hasDeviceFaultEntryPoint();
	const bool ok = (supported == entryPoint);   // 要么都能用，要么都不支持

	std::cout << "[DeviceFault] selfTest " << (ok ? "PASS" : "FAIL")
	          << " | supported=" << (supported ? 1 : 0)
	          << " entryPoint=" << (entryPoint ? 1 : 0)
	          << " (query path reserved for lost-state)" << "\n";

	vkDeviceWaitIdle(vulkanDevice->getDevice());
	return ok ? 0 : 1;
}

int FirstApp::runSubresourceErrorSelfTest()
{
suppressAutoSave = true;
subresourceProbePassPtr->setMode(SubresourceProbePass::Mode::SemanticError);

try
{
glfwPollEvents();
if (!handleWindowMaintenance())
throw std::runtime_error("window is not renderable during TD-05 subresource test");
update(1.0f / 60.0f);
render(1.0f / 60.0f);
}
catch (const std::exception& e)
{
const std::string message = e.what();
if (message.find("mip 1 / layer 0") != std::string::npos &&
message.find("TD05.ReadMip1Layer0") != std::string::npos)
{
std::cout << "[TD05] subresource semantic error PASS: expected rejection observed\n";
vkDeviceWaitIdle(vulkanDevice->getDevice());
return 0;
}

std::cerr << "[TD05] unexpected error during negative test: " << message << "\n";
vkDeviceWaitIdle(vulkanDevice->getDevice());
return 1;
}

std::cerr << "[TD05] negative test failed: expected semantic error was not thrown\n";
vkDeviceWaitIdle(vulkanDevice->getDevice());
return 1;
}

void FirstApp::setRenderScale(float scale)
{
	scene->getSettings().renderScale = std::clamp(scale, 0.25f, 1.0f);
}

VkExtent2D FirstApp::getDesiredRenderExtent() const
{
	const VkExtent2D swapExtent = vulkanSwapchain->getSwapChainExtent();
	const float scale = std::clamp(scene->getSettings().renderScale, 0.25f, 1.0f);
	return VkExtent2D{
		std::max(1u, static_cast<uint32_t>(swapExtent.width * scale + 0.5f)),
		std::max(1u, static_cast<uint32_t>(swapExtent.height * scale + 0.5f))
	};
}

void FirstApp::rebuildGlobalDescriptors()
{
	VkDescriptorImageInfo envInfo{ environmentTex->getSampler(), environmentTex->getView(),
	VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

	const CubemapTexture& irradiance = iblBaker->getIrradianceMap();
	const CubemapTexture& prefiltered = iblBaker->getPrefilteredMap();
	VkDescriptorImageInfo irradianceInfo{ irradiance.getSampler(), irradiance.getView(),
	VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo prefilteredInfo{ prefiltered.getSampler(), prefiltered.getView(),
	VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo brdfLutInfo{ iblBaker->getBrdfLutSampler(), iblBaker->getBrdfLutView(),
	VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

	std::array<VkImageView, kFramesInFlight> ssaoViews{};
	std::array<const GBuffer*, kFramesInFlight> constGBufferPtrs{};
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		ssaoViews[frame] = ssaoPassPtr->getOutputImageView(frame);
		constGBufferPtrs[frame] = gBuffers[frame].get();
	}

	descriptorManager->buildGlobalSet(globalUboBuffers, envInfo, constGBufferPtrs,
	ssaoPassPtr->getOutputSampler(), ssaoViews,
	irradianceInfo, prefilteredInfo, brdfLutInfo);
}

bool FirstApp::handleWindowMaintenance()
{
	if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
	return false;

	int framebufferWidth = 0;
	int framebufferHeight = 0;
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
	if (framebufferWidth <= 0 || framebufferHeight <= 0)
	return false;

	const VkExtent2D framebufferExtent{
		static_cast<uint32_t>(framebufferWidth),
		static_cast<uint32_t>(framebufferHeight)
	};
	const VkExtent2D swapExtent = vulkanSwapchain->getSwapChainExtent();
	if (renderer->needsSwapchainRecreate() ||
	framebufferExtent.width != swapExtent.width ||
	framebufferExtent.height != swapExtent.height)
	{
		recreateSwapchain(framebufferExtent);
	}
	else
	{
		const VkExtent2D desiredExtent = getDesiredRenderExtent();
		if (desiredExtent.width != currentRenderExtent.width ||
		desiredExtent.height != currentRenderExtent.height)
		recreateRenderTargets(desiredExtent);
	}
	return true;
}

void FirstApp::recreateRenderTargets(VkExtent2D renderExtent)
{
	if (renderExtent.width == 0 || renderExtent.height == 0)
	return;

	vkDeviceWaitIdle(vulkanDevice->getDevice());

	for (auto& gBuffer : gBuffers)
	gBuffer->resize(renderExtent);

	renderPipeline.resizeAll(renderExtent);
	rebuildGlobalDescriptors();
	currentRenderExtent = renderExtent;

	std::cout << "[TD03] Render targets resized: "
	<< renderExtent.width << "x" << renderExtent.height
	<< " (swapchain " << vulkanSwapchain->getSwapChainExtent().width << "x"
	<< vulkanSwapchain->getSwapChainExtent().height << ")\n";
}

void FirstApp::recreateSwapchain(VkExtent2D framebufferExtent)
{
	if (framebufferExtent.width == 0 || framebufferExtent.height == 0)
	return;

	vkDeviceWaitIdle(vulkanDevice->getDevice());

	vulkanSwapchain->recreate(framebufferExtent);
	renderer->onSwapchainRecreated();
	screenshotCapture.onResize(vulkanSwapchain->getSwapChainExtent());

	// ImGui's Vulkan backend caches image count and output format. Rebuild it
	// only with the swapchain; dynamic resolution keeps the same backend.
	imguiSystem.reset();
	imguiSystem = std::make_unique<ImGuiSystem>(window, *vulkanDevice,
	vulkanSwapchain->getSwapChainImageFormat(), vulkanSwapchain->imageCount(),
	renderer->getCommandPool());
	presentPassPtr->setImGuiSystem(imguiSystem.get());

	const VkExtent2D renderExtent = getDesiredRenderExtent();
	for (auto& gBuffer : gBuffers)
	gBuffer->resize(renderExtent);

	renderPipeline.resizeAll(renderExtent);
	rebuildGlobalDescriptors();
	currentRenderExtent = renderExtent;

	std::cout << "[TD03] Swapchain recreated to "
	<< vulkanSwapchain->getSwapChainExtent().width << "x"
	<< vulkanSwapchain->getSwapChainExtent().height
	<< ", render targets " << renderExtent.width << "x" << renderExtent.height << "\n";
}



void FirstApp::createUniformBuffers()
{
	// D-4：每帧槽位一份 UBO。单份 UBO 在 CPU/GPU 重叠时会被下一帧的 memcpy
	// 覆盖，GPU 可能读到新旧混合的数据。
	static const char* const names[kFramesInFlight] = { "GlobalUbo.Frame0", "GlobalUbo.Frame1" };

	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		VkDeviceSize bufferSize = sizeof(GlobalUbo);
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = bufferSize;
		bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		allocInfo.flags |= tiny_vmaNameFlag();
		allocInfo.pUserData = const_cast<char*>(names[frame]);

		if (vmaCreateBuffer(vulkanDevice->getVmaAllocator(), &bufferInfo, &allocInfo,
			&globalUboBuffers[frame], &globalUboAllocations[frame], nullptr) != VK_SUCCESS)
			throw std::runtime_error("GlobalUbo: per-frame buffer creation failed");

		VmaAllocationInfo info;
		vmaGetAllocationInfo(vulkanDevice->getVmaAllocator(), globalUboAllocations[frame], &info);
		uboMapped[frame] = info.pMappedData;
	}
}

void FirstApp::loadAllPBRTextures()
{
std::string basePath = TINY_RESOURCE_DIR;

// HDR 环境贴图：天空盒 + IBL 都要用，无条件加载
environmentTex = std::make_unique<Texture>(*vulkanDevice,
basePath + "Texture/environment.hdr",
VK_FORMAT_R32G32B32A32_SFLOAT, renderer->getCommandPool(), true);

std::cout << "[Tiny] HDR environment loaded.\n";
}

void FirstApp::initMaterials()
{
auto& matSys = scene->getMaterialSystem();
std::string basePath = TINY_RESOURCE_DIR;

// ---- 兜底材质 ----
// 天空盒 / 网格虽然着色器里不读 set 1，但管线布局声明了 set 1，
// vkCmdBindDescriptorSets 不允许绑 VK_NULL_HANDLE。
// 原来靠"它们默认的 materialSetIndex 恰好是 0，而 0 号槽已经分配过"
// 才没崩 —— 这里显式给一个纯白兜底材质，把巧合变成保证。
auto defaultMat = matSys.getOrCreateMaterial("__EngineDefault");
matSys.buildMaterialDescriptor(defaultMat, *descriptorManager);

// ---- 判断是否真的需要 DamagedHelmet 材质 ----
// glTF 物体的材质已经由加载器逐子网格分配好了；
// 只有子网格没有材质的物体（OBJ 路径）才需要在这里统一指定。
auto& registry = scene->getRegistry();
auto needsHeroMaterial = [&]() -> bool
{
    for (entt::entity entity : registry.view<MeshComponent, RenderTagComponent>())
    {
        const auto& mesh = registry.get<MeshComponent>(entity);
        const auto& tag = registry.get<RenderTagComponent>(entity);
        if (!mesh.model || tag.isSkybox) continue;
        for (const auto& sub : mesh.model->getSubMeshes())
            if (sub.materialSlot == kInvalidMaterialSlot) return true;
    }
    return false;
}();

std::shared_ptr<Material> heroMat = defaultMat;
if (needsHeroMaterial)
{
// albedo 是颜色贴图 -> sRGB；normal / metalRoughness 是数据贴图 -> UNORM。
// 只在真的用得到时才加载，避免 Sponza 场景白白吃掉 48MB 显存。
matSys.loadTexture(basePath + "Models/DamagedHelmet/Default_albedo.jpg",
*vulkanDevice, renderer->getCommandPool(), /*srgb=*/true);
matSys.loadTexture(basePath + "Models/DamagedHelmet/Default_normal.jpg",
*vulkanDevice, renderer->getCommandPool(), /*srgb=*/false);
matSys.loadTexture(basePath + "Models/DamagedHelmet/Default_metalRoughness.jpg",
*vulkanDevice, renderer->getCommandPool(), /*srgb=*/false);

heroMat = matSys.getOrCreateMaterial("DamagedHelmet");
heroMat->albedoMap = matSys.getTexture(basePath + "Models/DamagedHelmet/Default_albedo.jpg");
heroMat->normalMap = matSys.getTexture(basePath + "Models/DamagedHelmet/Default_normal.jpg");
heroMat->metallicRoughnessMap = matSys.getTexture(basePath + "Models/DamagedHelmet/Default_metalRoughness.jpg");
matSys.buildMaterialDescriptor(heroMat, *descriptorManager);
}

// 只给"子网格没有材质"的物体指定整体材质；glTF 物体原样跳过
for (entt::entity entity : registry.view<MeshComponent, RenderTagComponent>())
{
    const auto& mesh = registry.get<MeshComponent>(entity);
    const auto& tag = registry.get<RenderTagComponent>(entity);
    if (!mesh.model) continue;

    bool hasPerSubMeshMaterial = true;
    for (const auto& sub : mesh.model->getSubMeshes())
        if (sub.materialSlot == kInvalidMaterialSlot) { hasPerSubMeshMaterial = false; break; }
    if (hasPerSubMeshMaterial) continue;

    MaterialComponent& materialComp = registry.get_or_emplace<MaterialComponent>(entity);
    materialComp.material = tag.isSkybox ? defaultMat : heroMat;
    materialComp.materialSetIndex = materialComp.material->descriptorSetIndex;
}

std::cout << "[Tiny] 材质就绪，描述符槽位占用 "
<< descriptorManager->getMaterialSlotUsed() << "/"
<< descriptorManager->getMaterialSlotCapacity() << "\n";
}

void FirstApp::run()
{
	// 编辑器在这里惰性初始化，而不是构造函数里。
	// 原因：main() 是"先构造 FirstApp，再按命令行参数调 setEditorEnabled"，
	// 而 initEditor 依赖 ImGui 后端与场景全部就绪 —— 放到 run() 开头两个条件都满足。
	if (editorRequested && !editorLayer)
		initEditor();

	if (screenshotCapture.isEnabled() && screenshotAutoTarget)
		screenshotCapture.setTargetFrame(60);   // 交互模式：预热后第 60 帧
while (!glfwWindowShouldClose(window))
{
glfwPollEvents();
if (!handleWindowMaintenance())
{
ImGui_ImplGlfw_Sleep(10);
continue;
}

auto newTime = std::chrono::high_resolution_clock::now();
float frameTime = std::chrono::duration<float, std::chrono::seconds::period>(newTime - currentTime).count();
currentTime = newTime;

auto updateStart = std::chrono::high_resolution_clock::now();
update(frameTime);
auto updateEnd = std::chrono::high_resolution_clock::now();
render(frameTime);
auto renderEnd = std::chrono::high_resolution_clock::now();
updateMsAccum += std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
renderMsAccum += std::chrono::duration<double, std::milli>(renderEnd - updateEnd).count();
++profiledFrames;

		TINY_PROFILE_FRAME_MARK(); 
// 每 5 秒打一行性能摘要。
// 除了日常观察，这在"无头验证"（重定向 stdout 跑一遍看验证层）时特别有用 ——
// 没有它就无法区分"渲染正常"和"卡死在第一帧"。
++framesSincePerfLog;
auto perfNow = std::chrono::high_resolution_clock::now();
const float perfElapsed = std::chrono::duration<float>(perfNow - lastPerfLog).count();
if (perfElapsed >= 5.0f)
{
const RenderTelemetry& t = scene->getTelemetry();
std::cout << "[Perf] " << static_cast<int>(framesSincePerfLog / perfElapsed) << " FPS"
<< " | draw calls " << t.drawCalls
<< " | desc binds " << t.descriptorBinds
<< " | triangles " << t.triangles
<< " | vertices " << t.vertices
<< " | update " << (updateMsAccum / profiledFrames * 1000.0) << "us"
<< " | render " << (renderMsAccum / profiledFrames * 1000.0) << "us\n";
framesSincePerfLog = 0;
updateMsAccum = 0.0;
renderMsAccum = 0.0;
profiledFrames = 0;
lastPerfLog = perfNow;
}
}
vkDeviceWaitIdle(vulkanDevice->getDevice());
}

void FirstApp::setBloomEnabled(bool enabled)
{
	// 和 ImGui 里那两个复选框走的是完全相同的路径
	scene->getSettings().bloomEnabled = enabled;
}

void FirstApp::setSSAOEnabled(bool enabled)
{
	scene->getSettings().ssaoEnabled = enabled;
}

void FirstApp::setCrossQueueTestEnabled(bool enabled)
{
	crossQueueProbePassPtr->setEnabled(enabled);
}

void FirstApp::saveConfig()
{
	// 相机位姿和渲染参数分两次写，但两次都会"读回已有内容再替换自己那一段"，
	// 所以顺序无关、也不会互相覆盖（见 EngineConfig::save 的说明）。
	EngineConfig::save(scene->getSettings());

	CameraState cam;
	cam.position = scene->getCameraTransform().getTranslation();
	cam.rotation = scene->getCameraTransform().getEuler();
	EngineConfig::save(cam);
}

void FirstApp::loadConfig()
{
	EngineConfig::load(scene->getSettings());

	CameraState cam;
	cam.position = scene->getCameraTransform().getTranslation();
	cam.rotation = scene->getCameraTransform().getEuler();
	if (EngineConfig::load(cam))
	{
		scene->getCameraTransform().setTranslation(cam.position);
		scene->getCameraTransform().setEuler(cam.rotation);
		// Treat a camera teleport as a cut, not as a giant motion-blur path.
		hasPreviousViewProjection = false;
	}

	// 标题是按配置改的，重载之后也要跟着变 —— 否则用户会以为没生效
	glfwSetWindowTitle(window, scene->getSettings().windowTitle.c_str());
}

int FirstApp::runHeadlessSelfTest(int frames)
{

	// 自检路径也要能带编辑器跑（--editor --selftest 是无头验证编辑器 UI 的唯一手段）。
	if (editorRequested && !editorLayer)
		initEditor();

	// 编辑器在真实场景上跑一次"创建/撤销/重做"。
	// 必须在场景加载完之后、开始跑帧之前。
	if (editorLayer && !editorLayer->selfTestWithScene())
		throw std::runtime_error("editor scene selfTest failed");
	// 固定帧率推进，不依赖窗口事件循环 —— 这样"关掉某个 Pass 之后验证层是否干净"
	// 也能进自动化回归，而不需要有人去点一下 UI。
	// 每帧仍然走完整的 update + render，包括 ImGui 构建与提交、present。
	suppressAutoSave = true;   // 自检会故意关掉 Pass，别把结果写进用户配置

	if (!jobSystem->selfTest())
		throw std::runtime_error("JobSystem selfTest failed");
	if (!Scene::selfTest())
		throw std::runtime_error("Scene ECS/spatial selfTest failed");

	// D-8：编辑器自检（命令栈 / 删除撤销 / 序列化往返）。
	// 放在场景自检之后：它只依赖 entt 与 JSON，不需要 Vulkan 设备。
	if (!editor::EditorLayer::selfTest())
		throw std::runtime_error("editor selfTest failed");
	std::cout << "[Editor] selfTest PASS | commands + delete-undo + serialization round trip\n";

	const EngineSettings& s = scene->getSettings();
	std::cout << "[SelfTest] 开始，共 " << frames << " 帧，bloom=" << (s.bloomEnabled ? 1 : 0)
		<< " ssao=" << (s.ssaoEnabled ? 1 : 0) << "\n";

	materialStressBaselineUsed = descriptorManager->getMaterialSlotUsed();

	if (screenshotCapture.isEnabled() && screenshotAutoTarget)
		screenshotCapture.setTargetFrame(static_cast<uint64_t>(std::min(frames, 60)));

	const auto benchmarkStart = std::chrono::high_resolution_clock::now();
	const float dt = 1.0f / 60.0f;
	auto setTerrainTestLodScale = [this](float scale)
	{
		for (auto [entity, terrain] :
			 scene->getRegistry().view<TerrainComponent>().each())
		{
			(void)entity;
			terrain.config.lodDistanceScale = scale;
		}
	};
	for (int i = 0; i < frames; ++i)
	{
		// B-03: force a real runtime LOD transition without moving the camera.
		if (diagnosticsOptions.terrainTest && frames >= 20)
		{
			if (i == 3)
				setTerrainTestLodScale(0.01f);
			else if (i == 12)
				setTerrainTestLodScale(1.0f);
		}
		if (resizeTestEnabled)
		{
			switch (i)
			{
			case 5:  glfwSetWindowSize(window, 1280, 720); break;
			case 10: glfwSetWindowSize(window, 800, 600); break;
			case 15: glfwSetWindowSize(window, 1600, 900); break;
			case 20: glfwSetWindowSize(window, WIDTH, HEIGHT); break;
			case 25: setRenderScale(0.5f); break;
			case 35: setRenderScale(1.0f); break;
			case 45: glfwIconifyWindow(window); break;
			case 46: glfwRestoreWindow(window); break;
			default: break;
			}
		}

		if (materialStressEnabled)
			materialStressStep();

		glfwPollEvents();
		if (!handleWindowMaintenance())
		{
			ImGui_ImplGlfw_Sleep(10);
			continue;
		}
		update(dt);
		render(dt);
		TINY_PROFILE_FRAME_MARK();
	}

	const auto benchmarkEnd = std::chrono::high_resolution_clock::now();
	const double benchmarkSeconds = std::chrono::duration<double>(benchmarkEnd - benchmarkStart).count();
	if (benchmarkSeconds > 0.0)
	{
		std::cout << "[SelfTest] performance: " << frames << " frames in "
			<< benchmarkSeconds << " s (" << (frames / benchmarkSeconds) << " FPS)\n";
	}

	if (materialStressEnabled && materialStressPendingSlot != kInvalidMaterialSlot)
	{
		descriptorManager->releaseMaterialSet(materialStressPendingSlot);
		materialStressPendingSlot = kInvalidMaterialSlot;
	}

	vkDeviceWaitIdle(vulkanDevice->getDevice());
	if (materialStressEnabled)
	{
		descriptorManager->collectMaterialGarbage();
		const uint32_t usedAfter = descriptorManager->getMaterialSlotUsed();
		if (usedAfter != materialStressBaselineUsed || descriptorManager->getPendingMaterialReleaseCount() != 0)
			throw std::runtime_error("TD04 material slot stress leaked or failed to retire slots");
		std::cout << "[TD04] material slot stress PASS: cycles=" << materialStressCycles
			<< " used_before=" << materialStressBaselineUsed
			<< " used_after=" << usedAfter
			<< " max_pending=" << materialStressMaxPending
			<< " capacity=" << descriptorManager->getMaterialSlotCapacity() << "\n";
	}

	std::cout << "[SelfTest] 完成 " << frames << " 帧（验证层不再写 stderr；见 [Validation] 摘要 / --validation-jsonl）\n";
	return 0;
}

// ---------------------------------------------------------------------------
// 场景编辑器接入（--editor）
//
// 三条边界，改动前请先读：
//   1. 编辑器只拿到 EngineServices 的回调与只读指针，拿不到 vkDevice 之类的对象，
//      所以它不可能绕过渲染图直接提交 GPU 命令。
//   2. 关掉 --editor 时这条路径完全不执行：不分配、不注册回调、不多跑一次 ImGui 窗口。
//   3. 场景仍由 Scene 拥有并照常被 RenderPipeline 渲染。编辑器只改数据，不碰渲染。
// ---------------------------------------------------------------------------
namespace
{
// 视口尺寸要连续这么多帧不变才真正重建纹理（防抖窗口）。
// 2 帧 ≈ 33ms：用户松手后几乎立刻生效，但足以吸收拖拽期间的抖动。
constexpr uint64_t kViewportSettleFrames = 2;

// Win32 文件对话框的 filter 串用 '|' 分段（ImGui 风格），
// 这里转成 Win32 要求的 '\0' 分段 + 双空字符结尾。
std::wstring buildFilterBlock(const char* filter)
{
    const int len = MultiByteToWideChar(CP_UTF8, 0, filter, -1, nullptr, 0);
    std::wstring wide(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1)
        MultiByteToWideChar(CP_UTF8, 0, filter, -1, wide.data(), len);

    std::wstring block;
    block.reserve(wide.size() + 2);
    for (wchar_t ch : wide)
        block.push_back(ch == L'|' ? L'\0' : ch);
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::wstring toWide(const char* text)
{
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1)
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), len);
    return wide;
}

std::string fromWide(const wchar_t* text)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 1)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// TD-09：编辑器离屏视口的资源生命周期
//
// 为什么格式必须和交换链一致：present.frag 里手写了 gamma 编码（pow 1/2.2），
// 它假设目标格式是 UNORM。视口纹理如果换成 SRGB 格式，硬件会在写入时再编码一次，
// 编辑器里看到的画面就会比运行时模式亮一档 —— 这种差异极难归因，所以直接复用同一格式。
// ---------------------------------------------------------------------------
void FirstApp::createEditorViewportTextures(VkExtent2D extent)
{
if (extent.width == 0 || extent.height == 0)
return;

vkDeviceWaitIdle(vulkanDevice->getDevice());

// 先摘掉 ImGui 对视口纹理的引用。
// 尺寸一变，底层 imageView 会被重建，旧描述符立刻变成悬空句柄。
removeEditorViewportImGuiTextures();

for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
{
RenderGraph::TextureDesc desc;
desc.name = "EditorViewport.Frame" + std::to_string(frame);
desc.format = vulkanSwapchain->getSwapChainImageFormat();
desc.extent = extent;
// 既当渲染目标（PresentPass 写）又被采样（ImGui 读）。
// TRANSFER_SRC 是给 --screenshot-viewport 用的：没有它
// vkCmdCopyImageToBuffer 会直接报 VUID-vkCmdCopyImageToBuffer-srcImage-00186。
// 加一个 usage 位不产生任何额外内存，所以无条件带上，不做条件编译。
desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
| VK_IMAGE_USAGE_SAMPLED_BIT
| VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

// 【必须原地 resize，不能每次 createTexture】
// createTexture 会在渲染图的资源表里**新增**一个条目，旧条目要等渲染图析构
// 才释放。拖拽缩放窗口时每秒会重建几十次，等于每秒泄漏几十张全屏纹理
// （1300x861x4B ≈ 4.5MB/张），几秒钟就能把显存吃光。
// resizeOwnedTexture 是 TD-03 提供的原地重建接口，handle 保持不变。
if (!renderResources->editorViewport[frame].valid())
renderResources->editorViewport[frame] = renderGraph->createTexture(desc);
else
renderGraph->resizeOwnedTexture(renderResources->editorViewport[frame], extent);
}

// ImGui 采样用的 sampler：线性过滤 + clamp。
// 视口是被整体缩放显示的一张图，不需要 mip。
if (editorViewportSampler == VK_NULL_HANDLE)
{
VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
si.magFilter = VK_FILTER_LINEAR;
si.minFilter = VK_FILTER_LINEAR;
si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
si.maxAnisotropy = 1.0f;
if (vkCreateSampler(vulkanDevice->getDevice(), &si, nullptr, &editorViewportSampler) != VK_SUCCESS)
throw std::runtime_error("editor viewport sampler creation failed");
}

// 重新注册成 ImGui 纹理。
// 布局写死 SHADER_READ_ONLY_OPTIMAL —— UiCompositePass 里声明了 read，
// 渲染图会保证 ImGui 真正绘制时它确实处于这个布局。
for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
{
editorViewportImGuiSets[frame] = ImGui_ImplVulkan_AddTexture(
editorViewportSampler,
renderGraph->getView(renderResources->editorViewport[frame]),
VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

renderResources->editorViewportExtent = extent;
renderResources->editorViewportEnabled = true;
editorViewportTexturesCreated = true;

// 叠加层建管线要用的附件格式：颜色 = 视口纹理格式，深度 = G-Buffer 深度格式。
// 第一次进入编辑器时补调一次 init()（构造期 initAll 时编辑器还没启用）。
renderResources->editorViewportFormatHint = vulkanSwapchain->getSwapChainImageFormat();
renderResources->depthFormatHint = gBuffers[0]->getDepth().format;
if (editorOverlayPassPtr != nullptr && !editorOverlayInitialized)
{
editorOverlayPassPtr->init();
editorOverlayInitialized = true;
}

std::cout << "[TD09] Editor viewport " << extent.width << "x" << extent.height
<< " (offscreen, " << kFramesInFlight << " slots)\n";
}

void FirstApp::removeEditorViewportImGuiTextures()
{
for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
{
if (editorViewportImGuiSets[frame] != VK_NULL_HANDLE)
{
ImGui_ImplVulkan_RemoveTexture(editorViewportImGuiSets[frame]);
editorViewportImGuiSets[frame] = VK_NULL_HANDLE;
}
}
}

void FirstApp::destroyEditorViewportTextures()
{
if (!editorViewportTexturesCreated)
return;

// 先摘 ImGui 描述符，再让纹理失效 —— 反过来的话 ImGui 会持有悬空 imageView。
removeEditorViewportImGuiTextures();

// 渲染图没有"销毁单个纹理"的接口（它的资源都是整帧生命周期），
// 这里只把 handle 置空让 addToGraph 走回退路径；内存由渲染图析构时统一释放。
for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
renderResources->editorViewport[frame] = RenderGraph::TextureHandle{};

renderResources->editorViewportEnabled = false;
renderResources->editorViewportExtent = VkExtent2D{ 0, 0 };
editorViewportTexturesCreated = false;
}

void FirstApp::initEditor()
{
    editor::EngineServices services;
    services.scene = scene.get();
    services.registry = &scene->getRegistry();
    services.settings = &scene->getSettings();

    services.saveConfig = [this] { saveConfig(); };
    services.reloadConfig = [this] { loadConfig(); };

    // 编辑器不直接碰 swapchain / G-Buffer，只表达"需要重建渲染目标"的意图。
    services.requestRenderTargetResize = [this]
    {
        recreateRenderTargets(getDesiredRenderExtent());
    };

    services.pushLog = [](const std::string&) { /* 预留：接引擎日志流 */ };

    services.openFileDialog = [](const char* filter, const char* title) -> std::string
    {
        const std::wstring filterBlock = buildFilterBlock(filter);
        const std::wstring wideTitle = toWide(title);

        wchar_t buffer[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = filterBlock.c_str();
        ofn.lpstrFile = buffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = wideTitle.c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&ofn))
            return {};
        return fromWide(buffer);
    };

    services.saveFileDialog = [](const char* filter, const char* title) -> std::string
    {
        const std::wstring filterBlock = buildFilterBlock(filter);
        const std::wstring wideTitle = toWide(title);

        wchar_t buffer[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = filterBlock.c_str();
        ofn.lpstrFile = buffer;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = wideTitle.c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = L"json";

        if (!GetSaveFileNameW(&ofn))
            return {};
        return fromWide(buffer);
    };

    // 编辑器只"报尺寸"，引擎在帧首统一重建 —— 不能在 UI 构建期做，
    // 那时命令缓冲已在录制，vkDeviceWaitIdle 会死锁。
    services.requestViewportSize = [this](uint32_t width, uint32_t height)
    {
        const VkExtent2D requested{ width, height };
        if (requested.width != pendingEditorViewportExtent.width ||
                requested.height != pendingEditorViewportExtent.height)
        {
                pendingEditorViewportExtent = requested;
        }
        // 每次上报都刷新时间戳 —— 防抖靠它判断"尺寸还在变"。
        viewportRequestFrame = viewportDebounceClock;
    };

    // 视口纹理的 ImGui 句柄。按 frame slot 各一张，因为两帧在飞时
    // 第 N+1 帧写视口会覆盖 GPU 仍在采样的第 N 帧内容。
    services.getViewportTextureId = [this](uint32_t frameSlot) -> ImTextureID
    {
        if (!editorViewportTexturesCreated || frameSlot >= kFramesInFlight)
            return (ImTextureID)0;
        return (ImTextureID)(uintptr_t)editorViewportImGuiSets[frameSlot];
    };

    editorLayer = std::make_unique<editor::EditorLayer>();
    editorLayer->initialize(services, std::string(TINY_RESOURCE_DIR) + "Scenes/main.scene.json");

    // 先按窗口尺寸建一张，之后由编辑器的布局回调纠正成真实视口大小。
    {
        const VkExtent2D swapExtent = vulkanSwapchain->getSwapChainExtent();
        createEditorViewportTextures(swapExtent);
    }

    // 切换到离屏输出：PresentPass 写视口纹理、不再叠 ImGui；
    // UiCompositePass 负责把 ImGui（含视口图片）画到交换链。
    presentPassPtr->setRenderImGui(false);
    if (uiCompositePassPtr != nullptr)
        uiCompositePassPtr->setImGuiSystem(imguiSystem.get());

    std::cout << "[Editor] scene editor enabled (--editor)\n";
}

void FirstApp::shutdownEditor()
{
    // 顺序很重要：先摘掉 ImGui 对视口纹理的引用，再销毁编辑器，
    // 最后才轮到 ImGuiSystem 自己析构。
    destroyEditorViewportTextures();
    if (editorViewportSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(vulkanDevice->getDevice(), editorViewportSampler, nullptr);
        editorViewportSampler = VK_NULL_HANDLE;
    }

    // 编辑器只持有引用，销毁它不会带走 GPU 资源。
    // 在 ~FirstApp 最前面调用，保证它不会晚于 Scene / ImGuiSystem 析构。
    editorLayer.reset();
}

void FirstApp::update(float dt)
{
	TINY_ZONE_SCOPED("update");
	imguiSystem->newFrame();

	// 编辑器每帧的第一步：处理快捷键与播放状态。
	// 必须在 NewFrame 之后（ImGui 的按键状态此时才有效），
	// 且在 scene->update 之前（编辑器可能要改场景，改动应当当帧生效）。
	// 帧首统一应用视口尺寸变更。放在这里而不是 UI 构建期，是因为重建纹理
	// 需要 vkDeviceWaitIdle，而 UI 构建时命令缓冲已在录制。
	// 【为什么要防抖】拖拽窗口边缘时面板尺寸每帧都在变。每帧重建 = 每帧一次
	// vkDeviceWaitIdle + 两张全屏纹理重建，缩放会卡成幻灯片。
	// 判据是"尺寸连续 kViewportSettleFrames 帧没再变"，也就是用户松手了。
	++viewportDebounceClock;
	if (editorLayer && pendingEditorViewportExtent.width > 0)
	{
		const VkExtent2D current = renderResources->editorViewportExtent;
		const bool sizeDiffers = pendingEditorViewportExtent.width != current.width ||
			pendingEditorViewportExtent.height != current.height;
		const bool settled = (viewportDebounceClock - viewportRequestFrame) >= kViewportSettleFrames;
		if (sizeDiffers && settled)
			createEditorViewportTextures(pendingEditorViewportExtent);
	}

	if (editorLayer)
		editorLayer->beginFrame(dt, false);

	// 纵横比必须用**视口**尺寸而不是窗口尺寸。
	// 用窗口尺寸会让画面在编辑器里横向拉伸（视口比窗口窄），
	// 而且取景范围与运行时模式不一致。
	float aspect = vulkanSwapchain->getSwapChainExtent().width /
		(float)vulkanSwapchain->getSwapChainExtent().height;
	if (editorLayer && renderResources->editorViewportExtent.width > 0)
	{
		aspect = renderResources->editorViewportExtent.width /
			(float)renderResources->editorViewportExtent.height;
	}
	// 相机输入门控：
//   * 编辑器模式下，只有鼠标位于 3D 视口内才允许相机响应键鼠；
//   * 正在输入文字（例如资产面板的过滤框）时一律屏蔽 ——
//     CameraController 直接读 GLFW，不知道 ImGui 正在吃键盘，
//     不屏蔽的话在过滤框里敲 "wasd" 相机会跟着跑。
bool allowCameraInput = true;
if (editorLayer)
{
const editor::EditorState& editorState = editorLayer->getState();
// 鼠标在视口内、或者已经按住右键/中键在看了 —— 后者很重要：
// 拖拽视角时鼠标可能移出视口矩形，只按 hover 判断会让视角"拖一半就停"。
const bool looking = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                     glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
allowCameraInput = (editorState.viewportHovered || looking) && !ImGui::GetIO().WantTextInput;
// 拖 gizmo 时也不能让相机跟着转
if (editorState.gizmo.isDragging())
allowCameraInput = false;
// B-07: Play 模式下键盘归角色控制器，编辑器相机保持不动。
if (editorState.playState != editor::PlayState::Editing)
allowCameraInput = false;
}

scene->update(dt, window, aspect, allowCameraInput);
}

void FirstApp::render(float dt)
{
	TINY_ZONE_SCOPED("render");
	if (auto commandBuffer = renderer->beginFrame())
	{
		scene->beginFrameTelemetry();

		// P1-2：到达目标帧时请求 RenderDoc 抓帧。
		if (renderDoc.shouldCapture(renderer->getFrameNumber()))
			renderDoc.triggerCapture();

		// beginFrame 已确认本 frame slot 的上一轮 GPU 工作完成，可以安全覆写 UBO。
		const uint32_t frameInFlight = renderer->getCurrentFrameInFlightIndex();

		GlobalUbo ubo{};
		ubo.projectionView = scene->getCamera().getProjection() * scene->getCamera().getView();
		ubo.previousProjectionView = hasPreviousViewProjection
			? previousViewProjection
			: ubo.projectionView;
		ubo.cameraPos = glm::vec4(scene->getCameraTransform().getTranslation(), 1.0f);
		ubo.lightDirection = scene->getSettings().directionalLightDir;
		ubo.lightColor = scene->getSettings().directionalLightColor;
		ubo.numLights = 0;
		ubo.exposure = scene->getSettings().exposure;

		memcpy(uboMapped[frameInFlight], &ubo, sizeof(ubo));
		// 持久映射的 UBO 不保证落在 HOST_COHERENT 内存，必须显式 flush，
		// 否则 GPU 可能读到上一帧的旧数据（对 non-coherent 堆是未定义行为）。
		// vmaFlushAllocation 对 coherent 内存是空操作，可以无条件调用。
		vmaFlushAllocation(vulkanDevice->getVmaAllocator(),
			globalUboAllocations[frameInFlight], 0, sizeof(ubo));

		FrameInfo frameInfo{
			frameInFlight,
			renderer->getCurrentImageIndex(),
			renderer->getFrameNumber(),
			dt,
			commandBuffer,
			scene->getSettings(),
			scene->getTelemetry(),
			scene->getDisplayTelemetry(),
			scene->getRegistry(),
			scene->getVisibleEntities(),
			scene->getCamera().getProjection(),
			scene->getCamera().getView(),
			globalUboBuffers[frameInFlight],
			descriptorManager.get(),
			&scene->getCameraTransform(),
			&scene->getCameraController()
		};

		// 编辑器选中项交给渲染侧（画包围盒用）
if (editorLayer)
frameInfo.editorSelection = editorLayer->getState().selection.primary();

// W1：把配置存取交给 UI 层。用回调而不是让 ImGuiSystem 持有 FirstApp，
		// 保持"UI 不依赖引擎总控"这条边界。
		frameInfo.onSaveConfig = [this] { saveConfig(); };
		frameInfo.onLoadConfig = [this] { loadConfig(); };

		// 编辑器接管 UI 时，ImGuiSystem 不再画内置面板，改调这个回调。
		if (editorLayer)
		{
			frameInfo.isEditorMode = [this] { return editorLayer != nullptr; };
			frameInfo.onBuildEditorUI = [this, &frameInfo]
			{
				editorLayer->setFrameSlot(frameInfo.frameInFlightIndex);
				editorLayer->drawUI();
			};
		}

		// D-2：先把本帧的 ImGui 面板建好并生成绘制列表（纯 CPU 工作，不含 GPU 命令）。
		// 放在这里而不是 Present 回调里，是为了让面板上的改动（曝光、Bloom 参数）
		// 当帧就生效 —— 否则会有一帧延迟，拖滑块的手感会发飘。
		imguiSystem->prepareDrawData(frameInfo);

		// D-1(b)：所有 Pass 把自己的工作声明进渲染图，
		// 屏障、布局转换、vkCmdBeginRendering 全部由渲染图负责。
	renderGraph->beginFrame(commandBuffer, renderer->getComputeCommandBuffer(), 		renderer->getCurrentFrameInFlightIndex(), renderer->getFrameNumber());
		renderPipeline.registerAll(*renderGraph, frameInfo);
		// 渲染完成 -> 把这张交换链图像转入 PRESENT_SRC_KHR 供 present
		renderGraph->presentTransition(renderResources->swapchainImages[frameInfo.swapchainImageIndex]);
		renderGraph->endFrame();

		// P1-4：目标帧读回一张图到 staging buffer（present 之前）。
		// 默认拍交换链（含 ImGui）；--screenshot-viewport 拍编辑器离屏视口（纯 3D）。
		if (screenshotCapture.isTargetFrame(renderer->getFrameNumber()))
		{
			if (diagnosticsOptions.screenshotViewport && editorLayer &&
				renderResources->editorViewportEnabled)
			{
				// 视口纹理在 UiCompositePass 里被声明为 read，此刻处于 SHADER_READ_ONLY_OPTIMAL。
				// 拷完必须转回同一布局：渲染图记录的布局必须与真实布局一致，
				// 否则下一帧它会用错误的 oldLayout 插屏障，验证层立刻报错。
				screenshotCapture.recordCopy(commandBuffer,
					renderGraph->getImage(renderResources->editorViewport[frameInFlight]),
					renderResources->editorViewportExtent,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
			else
			{
				screenshotCapture.recordCopy(commandBuffer,
					vulkanSwapchain->getImage(frameInfo.swapchainImageIndex),
					vulkanSwapchain->getSwapChainExtent());
			}
		}

		const auto& crossQueue = renderGraph->getCrossQueueDependencies();
		// P1-1：收集本帧的 Tracy GPU zone（必须在命令缓冲结束前）。
		gpuProfiler.collect(commandBuffer);
		renderer->endFrame(crossQueue.computeToGraphics, crossQueue.graphicsToCompute);

		if (editorLayer)
			editorLayer->endFrame();

		// This frame is fully submitted. It becomes the reprojection source for
		// the next frame's camera motion blur.
		previousViewProjection = ubo.projectionView;
		hasPreviousViewProjection = true;
		if (screenshotCapture.hasPendingCopy())
		{
			vkDeviceWaitIdle(vulkanDevice->getDevice());
			screenshotCapture.writePng(diagnosticsOptions.screenshotPath);
		}
	}
	else
	{
		// Acquire failed after update() already started an ImGui frame.
		imguiSystem->discardFrame();
	}
}

void FirstApp::finalizeDiagnostics()
{
    if (diagnosticsFinalized) return;
    diagnosticsFinalized = true;

    // 必须在 vkDeviceWaitIdle 之后调用：先读回最后几帧的 GPU 计时，再销毁
    // query pool / staging buffer（此时 device 仍有效）。守卫用的状态必须在
    // destroy() 之前取，否则会被清零而失去意义。
    const bool gpuTimingWasEnabled = gpuTimer.isEnabled();
    const bool screenshotWasEnabled = screenshotCapture.isEnabled();
    const uint32_t gpuTimingQueries = gpuTimer.queryPoolCount();
    const uint32_t screenshotBytes = screenshotCapture.stagedBytes();

#ifdef TINY_ENABLE_GPU_TIMING
    if (gpuTimer.isEnabled())
    {
        gpuTimer.finalize();
        if (gpuTimer.writeCsv(diagnosticsOptions.gpuTimingCsvPath))
            std::cout << "[Diag] gpu_timing.csv written: " << diagnosticsOptions.gpuTimingCsvPath
                      << " (" << gpuTimer.sampleCount() << " samples)\n";
    }
#endif
    gpuTimer.destroy();
    screenshotCapture.destroy();
    renderDoc.destroy();
    gpuProfiler.destroy();

    diag::printValidationSummary();

    // P0-6 零开销守卫：关闭时不应有任何调试用 GPU 资源 / 额外文件。
    const bool anyDebugEnabled = gpuTimingWasEnabled || screenshotWasEnabled;
    const bool guardOk = (gpuTimingQueries == 0 && screenshotBytes == 0);
    std::cout << "[Diag] zero-overhead guard "
              << (anyDebugEnabled ? "SKIP (debug capability on)" : (guardOk ? "PASS" : "FAIL"))
              << " | queryPool=" << gpuTimingQueries
              << " | screenshotBuffer=" << screenshotBytes
              << " | validationJsonl=" << (diagnosticsOptions.validationJsonlPath.empty() ? "off" : "on")
              << "\n";
}
