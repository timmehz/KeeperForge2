#pragma once
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// What the DirectDraw layer (DirectDrawLayer.cpp) asks of a graphics API. The game's Direct3D 6 work arrives here
// already turned into plain data: pre-transformed triangles, 32-bit BGRA textures, the texture stage setup as shader
// constants, and blend and depth settings. Every backend draws the same thing the same way:
// - the frame: a render target the size of the game's screen mode, with a depth buffer;
// - the screen: a texture holding a CPU picture (the game drawing straight onto its visible surface);
// - present: either of them scaled into a rectangle of the window, black around it.
// All calls come from the layer with its lock held, so a backend never sees two at once (they may come from
// different threads: the game plays its videos on one of its own).
namespace KeeperForge2::Render {
	enum class Api { D3D11, D3D12, Vulkan };

	// One vertex as the shaders take it. The game's vertices (x, y, z, 1/w, diffuse, up to 3 texture coordinates) are
	// copied into this layout, so every backend has one vertex format. diffuse is the D3DCOLOR as the game stores it
	// (bytes B, G, R, A); the shaders swizzle it.
	struct Vertex {
		float x, y, z, rhw;
		std::uint32_t diffuse;
		float uv[3][2];
	};
	static_assert(sizeof(Vertex) == 44);

	// D3DBLEND values 1-11 and D3DCMPFUNC values 1-8; the layer turns BOTHSRCALPHA/BOTHINVSRCALPHA into these.
	enum class Blend : std::uint8_t { Zero = 1, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor, InvDestColor, SrcAlphaSat };
	enum class Compare : std::uint8_t { Never = 1, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
	// D3DTEXTUREADDRESS values.
	enum class Address : std::uint8_t { Wrap = 1, Mirror, Clamp, Border };

	// The pixel shader's constants (std140/cbuffer layout: 16-byte rows). The ops and arguments are D3DTOP and D3DTA
	// values per texture stage (x = stage 0, y = 1, z = 2, w unused); the shader runs the stages the way Direct3D 6
	// does. misc.x is the alpha test function (0 = off), misc.y has bit n set when stage n has a texture.
	// view and depth turn the game's screen coordinates into the API's clip space (see each backend).
	struct ShaderConstants {
		std::int32_t colorOp[4];
		std::int32_t colorArg1[4];
		std::int32_t colorArg2[4];
		std::int32_t alphaOp[4];
		std::int32_t alphaArg1[4];
		std::int32_t alphaArg2[4];
		std::int32_t texCoordIndex[4];
		std::int32_t misc[4];
		float textureFactor[4];
		// x = 2 / width, y = y scale, z = y offset, w = alpha test reference (0-1).
		float view[4];
		// x = z scale, y = z offset.
		float depth[4];
		// Bump mapping (a stage with D3DTOP_BUMPENVMAP or BUMPENVMAPLUMINANCE): its 2x2 matrix (D3DTSS_BUMPENVMAT00,
		// 01, 10, 11) and x = luminance scale, y = luminance offset (D3DTSS_BUMPENVLSCALE, LOFFSET). Last, so shaders
		// made before these rows existed still read the rest right.
		float bumpMatrix[4];
		float bumpLuminance[4];
	};
	static_assert(sizeof(ShaderConstants) == 208);

	struct SamplerState {
		Address addressU = Address::Wrap;
		Address addressV = Address::Wrap;
		bool linear = false;
		bool operator==(const SamplerState&) const = default;
		// 0-31, for backends that keep one sampler of each kind.
		std::uint32_t Key() const { return (static_cast<std::uint32_t>(addressU) - 1) | (static_cast<std::uint32_t>(addressV) - 1) << 2 | (linear ? 16u : 0u); }
	};
	constexpr std::uint32_t SamplerKinds = 32;

	struct DrawState {
		bool blend = false;
		Blend source = Blend::One;
		Blend destination = Blend::Zero;
		bool depthTest = true;
		bool depthWrite = true;
		Compare depthFunc = Compare::LessEqual;
		ShaderConstants constants{};
		SamplerState samplers[3];
		// Backend texture handles, 0 for none.
		std::uint32_t textures[3]{};
		// Everything a pipeline depends on, as one number.
		std::uint32_t PipelineKey() const {
			return (blend ? 1u : 0u) | static_cast<std::uint32_t>(source) << 1 | static_cast<std::uint32_t>(destination) << 5
				| (depthTest ? 1u << 9 : 0u) | (depthWrite ? 1u << 10 : 0u) | static_cast<std::uint32_t>(depthFunc) << 11;
		}
	};

	// A rectangle of the window's client area, in pixels.
	struct PresentRect {
		std::int32_t left, top, right, bottom;
	};
	enum class PresentSource { Frame, Screen };

	// Anti-aliasing (config.yml anti_aliasing). FXAA and SMAA smooth the finished frame when it is shown (only
	// PresentSource::Frame; what ReadFrame gives back is never smoothed); level picks their quality preset
	// (2 = low, 4 = high, 8 = ultra). MSAA draws the frame with level samples per pixel; the frame is resolved to one
	// sample before it is shown or read back.
	enum class AntiAliasingMode { Off, Fxaa, Smaa, Msaa };
	struct AntiAliasing {
		AntiAliasingMode mode = AntiAliasingMode::Off;
		std::uint32_t level = 4;
		bool operator==(const AntiAliasing&) const = default;
	};
	constexpr AntiAliasing DefaultAntiAliasing{ AntiAliasingMode::Smaa, 4 };
	// "off", "fxaa x2", "smaa x4", "msaa x8" and so on (case and spaces do not matter, "smaa" alone is x4).
	std::optional<AntiAliasing> AntiAliasingFromConfigName(const std::string& name);
	std::string AntiAliasingConfigName(const AntiAliasing& value);
	// For the log, such as "SMAA x4 (high)".
	std::string AntiAliasingName(const AntiAliasing& value);

	// The constants every post pass (FXAA, SMAA, the MSAA copy) takes, the same in HLSL and GLSL: metrics is
	// 1/width, 1/height, width, height of the frame; params is the preset's quality: for FXAA minimum contrast,
	// relative contrast, sub-pixel amount, search steps; for SMAA edge threshold, search pixels, contrast factor.
	struct PostConstants {
		float metrics[4];
		float params[4];
	};
	static_assert(sizeof(PostConstants) == 32);
	PostConstants MakePostConstants(const AntiAliasing& value, std::uint32_t width, std::uint32_t height);
	// The HLSL of the FXAA or SMAA passes, for DirectX 11 and 12 (the preset is in the constants).
	std::string PostHlsl(const AntiAliasing& value);

	// A pixel as memory holds it (A in the top byte, then R, G, B) as red, green, blue, alpha from 0 to 1, the order
	// every API's clear color takes whatever the format's byte order.
	inline void PixelToFloats(std::uint32_t pixel, float (&rgba)[4]) {
		rgba[0] = static_cast<float>((pixel >> 16) & 0xFF) / 255.0f;
		rgba[1] = static_cast<float>((pixel >> 8) & 0xFF) / 255.0f;
		rgba[2] = static_cast<float>(pixel & 0xFF) / 255.0f;
		rgba[3] = static_cast<float>((pixel >> 24) & 0xFF) / 255.0f;
	}

	// Ray tracing (config.yml ray_tracing, RayScene.h, docs/game/ray-tracing.md). The scene in camera space (x right, y
	// down, z into the screen, in the game's own units up to one scale), built on the CPU each frame: triangles as a
	// corner and two edges, and a bounding volume tree over them. A node with count 0 is inner, its children at
	// leftOrFirst and leftOrFirst + 1; otherwise a leaf of count triangles from leftOrFirst. The same layout in the
	// compute shaders (std430: a node is two vec4, a triangle three). corner[3] is the triangle's material:
	// RayMaterialSolid, or RayMaterialMirror for water and lava (they reflect, and other rays pass through them).
	constexpr float RayMaterialSolid = 0.0f;
	constexpr float RayMaterialMirror = 1.0f;
	struct RayTriangle {
		float corner[4];
		float edge1[4];
		float edge2[4];
	};
	static_assert(sizeof(RayTriangle) == 48);
	struct RayNode {
		float boundsMin[3];
		std::int32_t leftOrFirst;
		float boundsMax[3];
		std::int32_t count;
	};
	static_assert(sizeof(RayNode) == 32);
	// A light for shadows, in the same space: position and reach, color (0-1) and strength (two vec4).
	struct RayLight {
		float position[3];
		float radius;
		float color[3];
		float strength;
	};
	static_assert(sizeof(RayLight) == 32);
	struct RaySceneData {
		const RayNode* nodes = nullptr;
		std::uint32_t nodeCount = 0;
		const RayTriangle* triangles = nullptr;
		std::uint32_t triangleCount = 0;
		const RayLight* lights = nullptr;
		std::uint32_t lightCount = 0;
	};
	// What a trace works out. width and height are the result's (the texture's) size; frame sizes, the projection's
	// middle and focal length (pixels) are the frame's. radius: how far ambient occlusion looks (scene units); rays per
	// pixel. occlusion and shadows: how strongly each darkens (0 off, 1 full); ambient: the light every point gets
	// besides the scene's lights, against which a light's shadow is weighed; reflections: how strongly water and lava
	// show what their reflection rays hit (0 off), reflectReach how far those look; bounce: how strongly light bounced off
	// nearby surfaces is added (0 off), bounceReach how far its rays look. The same 80 bytes are the compute shaders'
	// push constants.
	struct RayTraceSettings {
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		float frameWidth = 0.0f;
		float frameHeight = 0.0f;
		float centerX = 0.0f;
		float centerY = 0.0f;
		float focal = 0.0f;
		float radius = 0.0f;
		std::uint32_t rays = 0;
		std::uint32_t frame = 0;
		std::uint32_t nodeCount = 0;
		std::uint32_t triangleCount = 0;
		std::uint32_t lightCount = 0;
		float occlusion = 1.0f;
		float shadows = 1.0f;
		float ambient = 0.35f;
		float reflections = 0.0f;
		float reflectReach = 0.0f;
		float bounce = 0.0f;
		float bounceReach = 0.0f;
	};
	static_assert(sizeof(RayTraceSettings) == 80);

	class Backend {
		public:
			virtual ~Backend() = default;
			// Ray tracing in compute shaders (our own tracer: no API ray tracing, which the drivers do not give a 32-bit
			// program). TraceAmbientOcclusion writes, into texture (made by CreateTexture at settings.width x height), how
			// much light each point the camera sees keeps: white all of it, darker closed in (ambient occlusion) or cut off
			// from the scene's lights (shadows), already smoothed. With reflections, reflectionTexture (the same size) gets
			// what water and lava reflect, its alpha how much of it shows (0 elsewhere); with bounce, bounceTexture (the same
			// size) gets the light bounced onto each point from nearby surfaces, smoothed, already scaled by its strength.
			// The frame as it is now is where reflected and bounced colors come from. A texture of 0 is not written. False
			// when this backend cannot (then nothing was written).
			virtual bool CanRayTrace() const { return false; }
			virtual bool TraceAmbientOcclusion(const RaySceneData& scene, const RayTraceSettings& settings, std::uint32_t texture, std::uint32_t reflectionTexture,
				std::uint32_t bounceTexture) { return false; }
			virtual const char* Name() const = 0;
			// Set before Init. A backend that cannot do what is asked turns it down or off (and logs it) in Init.
			void SetAntiAliasing(const AntiAliasing& value) { antiAliasing = value; }
			const AntiAliasing& GetAntiAliasing() const { return antiAliasing; }
			// texture_filtering: the most samples a linear sampler takes along a slanted view (1 = none, up to 16). Set before
			// Init; mip levels are used whenever a texture has them.
			void SetAnisotropy(std::uint32_t value) { anisotropy = value < 1 ? 1 : value > 16 ? 16 : value; }
			// Creates the device and the window's swap chain. Returns false (and logs why) if this API cannot run.
			virtual bool Init(HWND window) = 0;
			// The game's screen mode: makes the frame (color and depth) this size. Its contents are undefined after.
			virtual bool SetFrameSize(std::uint32_t width, std::uint32_t height) = 0;

			// Textures are 32-bit BGRA, rows top first. Handles are never 0. levels: how many mip levels it has (1, or up to
			// MipLevelCount); each is filled on its own with UpdateTexture's level, which is MipLevelSize of the texture's size.
			virtual std::uint32_t CreateTexture(std::uint32_t width, std::uint32_t height, std::uint32_t levels = 1) = 0;
			virtual void UpdateTexture(std::uint32_t texture, const void* pixels, std::uint32_t pitch, std::uint32_t level = 0) = 0;
			virtual void DestroyTexture(std::uint32_t texture) = 0;
			// A rectangle of the frame (inside it) into a texture at (toX, toY) (inside it too), on the card: the game copying
			// part of the frame into a surface of its own. ReadTexture brings such a texture's picture into memory (rows top
			// first), only when the game uses that surface some other way; the texture must not be bigger than the frame.
			virtual void CopyFrameToTexture(std::uint32_t texture, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t toX, std::uint32_t toY) = 0;
			virtual void ReadTexture(std::uint32_t texture, void* pixels, std::uint32_t pitch) = 0;

			// The frame's color from or to a CPU picture of the frame's size (BGRA, rows top first).
			virtual void UploadFrame(const void* pixels, std::uint32_t pitch) = 0;
			virtual void ReadFrame(void* pixels, std::uint32_t pitch) = 0;
			// Only a rectangle of it (inside the frame): pixels is still the whole picture's memory, and only that
			// rectangle there is written. Much less to copy when the game copies a small part of the frame elsewhere.
			virtual void ReadFrameArea(void* pixels, std::uint32_t pitch, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) = 0;
			// A flip while the frame is only on the card: keeps a copy of the frame as it is now there (the picture the
			// game's screen surface holds after a real flip), and gives it back in memory only if the game reads that
			// surface later. Both are the frame's size; the copy is lost when the frame changes size.
			virtual void SaveShownFrame() = 0;
			virtual void ReadShownFrame(void* pixels, std::uint32_t pitch) = 0;
			virtual void ClearDepth() = 0;
			// Fills the whole frame's color with one pixel (BGRA as a 32-bit number, as in memory), on the card.
			virtual void ClearFrame(std::uint32_t pixel) = 0;
			// A triangle list into the frame. indices index vertices.
			virtual void Draw(const DrawState& state, const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount) = 0;

			// The screen picture (any size, BGRA, rows top first) for PresentSource::Screen.
			virtual void UploadScreen(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) = 0;
			// Shows the frame or the screen scaled into target (in the window's client area, black elsewhere).
			virtual void Present(PresentSource source, const PresentRect& target, bool vsync) = 0;

			// How this API's clip space is reached from the game's screen coordinates (see ShaderConstants::view/depth).
			// y_clip = (y + 0.5) * yScale + yOffset with yScale = ±2 / height; z_clip = z * zScale + zOffset.
			virtual bool FlipsY() const = 0;

		protected:
			AntiAliasing antiAliasing;
			std::uint32_t anisotropy = 1;
	};

	// Mip levels down to 1x1 for a texture of this size, and the size of one of them (never below 1).
	constexpr std::uint32_t MipLevelCount(std::uint32_t width, std::uint32_t height) {
		std::uint32_t levels = 1;
		for (std::uint32_t side = width > height ? width : height; side > 1; side >>= 1) ++levels;
		return levels;
	}
	constexpr std::uint32_t MipLevelSize(std::uint32_t size, std::uint32_t level) {
		const std::uint32_t shifted = level < 32 ? size >> level : 0;
		return shifted == 0 ? 1 : shifted;
	}

	std::unique_ptr<Backend> CreateD3D11Backend();
	std::unique_ptr<Backend> CreateD3D12Backend();
	std::unique_ptr<Backend> CreateVulkanBackend();
	std::unique_ptr<Backend> CreateBackend(Api api);

	const char* ApiName(Api api);
	// The config.yml word for an API (dx11, dx12, vulkan), and back. Unknown words give none.
	constexpr Api DefaultApi = Api::Vulkan;
	const char* ApiConfigName(Api api);
	std::optional<Api> ApiFromConfigName(const std::string& name);
}
