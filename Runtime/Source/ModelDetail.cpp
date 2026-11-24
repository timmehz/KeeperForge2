#include "ModelDetail.h"
#include "Config.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include "StringUtils.h"
#include <cstdint>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		// The engine's mesh reduction from 16 up makes every mesh's detail sum 0 (the whole mesh), as the game does itself
		// in play mode 5 (see GameAddresses.h, MeshReductionEngineRead).
		constexpr std::uint8_t FullDetailReduction = 16;
		std::vector<std::uint8_t> ReadOf(std::uintptr_t address) {
			return { 0xA1, static_cast<std::uint8_t>(address), static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address >> 16),
				static_cast<std::uint8_t>(address >> 24) };
		}

		// ---- Which batches are models (see GameAddresses.h, EngineBatchBegin). Set when a creature's or object's mesh adds
		// its triangles, cleared when a batch starts and after it is drawn.
		bool drawingModels = false;
		// All four hooks are in, so drawingModels can be trusted.
		bool modelBatchesReady = false;
		using BatchBeginFn = void(__cdecl*)(void* batch);
		using BatchDrawFn = void(__cdecl*)();
		using AddTrianglesFn = std::uint32_t(__fastcall*)(void* primitive, void* unused, std::uint32_t index, void* entry);
		BatchBeginFn originalBatchBegin = nullptr;
		BatchDrawFn originalBatchDraw = nullptr;
		AddTrianglesFn originalAnimMeshAdd = nullptr;
		AddTrianglesFn originalDynamicMeshAdd = nullptr;
		void __cdecl BatchBeginDetour(void* batch) {
			drawingModels = false;
			originalBatchBegin(batch);
		}
		void __cdecl BatchDrawDetour() {
			originalBatchDraw();
			drawingModels = false;
		}
		std::uint32_t __fastcall AnimMeshAddDetour(void* primitive, void* unused, std::uint32_t index, void* entry) {
			drawingModels = true;
			return originalAnimMeshAdd(primitive, unused, index, entry);
		}
		std::uint32_t __fastcall DynamicMeshAddDetour(void* primitive, void* unused, std::uint32_t index, void* entry) {
			drawingModels = true;
			return originalDynamicMeshAdd(primitive, unused, index, entry);
		}
		bool Hook(std::uintptr_t target, const std::vector<std::uint8_t>& expected, void* detour, void** original) {
			if (!Memory::Matches(target, expected)) return false;
			if (!HookManager::CreateHook(target, detour, original) || !HookManager::EnableHook(target)) {
				HookManager::RemoveHook(target);
				return false;
			}
			return true;
		}
		void InstallModelBatches() {
			if (StringUtils::ToLower(Config::GetString("model_smoothing", "medium")) == "off") return;
			// mov eax, [esp+4]; mov edx, [0x76F338] / push ebp; mov ebp, esp; sub esp, 0xC; mov eax, [0x76C2F0] /
			// mov eax, [esp+4]; cmp eax, 0x7D0 (both adds) [verified in the assembly].
			const std::vector<std::uint8_t> add = { 0x8B, 0x44, 0x24, 0x04, 0x3D, 0xD0, 0x07, 0x00, 0x00 };
			const bool hooked = Hook(Game::Resolve(Game::Address::EngineBatchBegin), { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x15, 0x38, 0xF3, 0x76, 0x00 },
					reinterpret_cast<void*>(&BatchBeginDetour), reinterpret_cast<void**>(&originalBatchBegin))
				&& Hook(Game::Resolve(Game::Address::EngineBatchDraw), { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0xA1, 0xF0, 0xC2, 0x76, 0x00 },
					reinterpret_cast<void*>(&BatchDrawDetour), reinterpret_cast<void**>(&originalBatchDraw))
				&& Hook(Game::Resolve(Game::Address::AnimMeshAddTriangles), add,
					reinterpret_cast<void*>(&AnimMeshAddDetour), reinterpret_cast<void**>(&originalAnimMeshAdd))
				&& Hook(Game::Resolve(Game::Address::DynamicMeshAddTriangles), add,
					reinterpret_cast<void*>(&DynamicMeshAddDetour), reinterpret_cast<void**>(&originalDynamicMeshAdd));
			modelBatchesReady = hooked;
			if (!hooked) {
				// Whatever was hooked stays harmless: without all four, the flag is never trusted.
				Logger::Warning("Model detail: unexpected game code; models cannot be told from terrain, so model_smoothing does nothing.");
			}
		}
	}

	bool ModelDetail::DrawingModels() { return drawingModels && modelBatchesReady; }

	void ModelDetail::Install() {
		InstallModelBatches();
		auto word = StringUtils::ToLower(Config::GetString("model_detail", "full"));
		if (word != "full" && word != "game") {
			Logger::Warning("Model detail: \"" + word + "\" in config.yml is not full or game; using full and writing that to config.yml.");
			Config::SetString("model_detail", "full");
			word = "full";
		}
		if (word == "game") return;
		const auto read = Game::Resolve(Game::Address::MeshReductionEngineRead);
		if (!Memory::Matches(read, ReadOf(Game::Resolve(Game::Address::MeshReductionSetting)))) {
			Logger::Warning("Model detail: unexpected game code, models keep the game's own detail.");
			return;
		}
		// mov eax, 16 in place of mov eax, [setting]: the engine is handed full detail; the saved setting stays as it was.
		Memory::Patch(read, { 0xB8, FullDetailReduction, 0x00, 0x00, 0x00 });
		Logger::Info("Model detail: every model is drawn whole at any distance (model_detail: full).");
	}
}
