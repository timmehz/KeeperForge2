#include "Backend.h"
#include "ShaderSources.h"
#include "StringUtils.h"

namespace KeeperForge2::Render {
	std::optional<AntiAliasing> AntiAliasingFromConfigName(const std::string& name) {
		std::string word;
		for (const char c : StringUtils::ToLower(name)) if (c != ' ' && c != '_' && c != '-' && c != '\t') word += c;
		if (word == "off" || word == "none" || word == "0") return AntiAliasing{};
		AntiAliasing value;
		if (word.starts_with("fxaa")) value.mode = AntiAliasingMode::Fxaa;
		else if (word.starts_with("smaa")) value.mode = AntiAliasingMode::Smaa;
		else if (word.starts_with("msaa")) value.mode = AntiAliasingMode::Msaa;
		else return std::nullopt;
		const std::string level = word.substr(4);
		if (level.empty() || level == "x4" || level == "4" || level == "4x") value.level = 4;
		else if (level == "x2" || level == "2" || level == "2x") value.level = 2;
		else if (level == "x8" || level == "8" || level == "8x") value.level = 8;
		else return std::nullopt;
		return value;
	}
	std::string AntiAliasingConfigName(const AntiAliasing& value) {
		switch (value.mode) {
			case AntiAliasingMode::Fxaa: return "fxaa x" + std::to_string(value.level);
			case AntiAliasingMode::Smaa: return "smaa x" + std::to_string(value.level);
			case AntiAliasingMode::Msaa: return "msaa x" + std::to_string(value.level);
			default: return "off";
		}
	}
	std::string AntiAliasingName(const AntiAliasing& value) {
		const char* quality = value.level <= 2 ? "low" : value.level <= 4 ? "high" : "ultra";
		switch (value.mode) {
			case AntiAliasingMode::Fxaa: return "FXAA x" + std::to_string(value.level) + " (" + quality + ")";
			case AntiAliasingMode::Smaa: return "SMAA x" + std::to_string(value.level) + " (" + quality + ")";
			case AntiAliasingMode::Msaa: return "MSAA x" + std::to_string(value.level) + " (" + std::to_string(value.level) + " samples per pixel)";
			default: return "no anti-aliasing";
		}
	}

	PostConstants MakePostConstants(const AntiAliasing& value, std::uint32_t width, std::uint32_t height) {
		PostConstants constants{};
		const float w = static_cast<float>(width > 0 ? width : 1);
		const float h = static_cast<float>(height > 0 ? height : 1);
		constants.metrics[0] = 1.0f / w;
		constants.metrics[1] = 1.0f / h;
		constants.metrics[2] = w;
		constants.metrics[3] = h;
		if (value.mode == AntiAliasingMode::Smaa) {
			// SMAA's presets: the smallest brightness difference that is an edge, how far an edge is followed each way
			// (pixels), and how much weaker than the strongest difference around it an edge may be (as a factor).
			constants.params[0] = value.level <= 2 ? 0.15f : value.level <= 4 ? 0.1f : 0.05f;
			constants.params[1] = value.level <= 2 ? 8.0f : value.level <= 4 ? 16.0f : 32.0f;
			constants.params[2] = 2.0f;
			constants.params[3] = 0.0f;
			return constants;
		}
		// FXAA's presets: x2 skips more faint edges and follows them less far; x8 catches faint ones and long ones.
		if (value.level <= 2) {
			constants.params[0] = 0.0833f;
			constants.params[1] = 0.25f;
			constants.params[2] = 0.5f;
			constants.params[3] = 4.0f;
		}
		else if (value.level <= 4) {
			constants.params[0] = 0.0625f;
			constants.params[1] = 0.166f;
			constants.params[2] = 0.75f;
			constants.params[3] = 8.0f;
		}
		else {
			constants.params[0] = 0.0312f;
			constants.params[1] = 0.125f;
			constants.params[2] = 1.0f;
			constants.params[3] = 12.0f;
		}
		return constants;
	}

	std::string PostHlsl(const AntiAliasing& value) {
		if (value.mode == AntiAliasingMode::Fxaa) return std::string(Shaders::PostCommonHlsl) + Shaders::FxaaHlsl;
		if (value.mode == AntiAliasingMode::Smaa) return std::string(Shaders::PostCommonHlsl) + Shaders::SmaaHlsl;
		return {};
	}

	std::unique_ptr<Backend> CreateBackend(Api api) {
		switch (api) {
			case Api::D3D11: return CreateD3D11Backend();
			case Api::D3D12: return CreateD3D12Backend();
			case Api::Vulkan: return CreateVulkanBackend();
			default: return nullptr;
		}
	}

	const char* ApiName(Api api) {
		switch (api) {
			case Api::D3D11: return "DirectX 11";
			case Api::D3D12: return "DirectX 12";
			case Api::Vulkan: return "Vulkan";
			default: return "unknown";
		}
	}

	const char* ApiConfigName(Api api) {
		switch (api) {
			case Api::D3D11: return "dx11";
			case Api::D3D12: return "dx12";
			case Api::Vulkan: return "vulkan";
			default: return "unknown";
		}
	}

	std::optional<Api> ApiFromConfigName(const std::string& name) {
		const std::string word = StringUtils::ToLower(name);
		if (word == "dx11" || word == "d3d11" || word == "directx11") return Api::D3D11;
		if (word == "dx12" || word == "d3d12" || word == "directx12") return Api::D3D12;
		if (word == "vulkan" || word == "vk") return Api::Vulkan;
		return std::nullopt;
	}
}
