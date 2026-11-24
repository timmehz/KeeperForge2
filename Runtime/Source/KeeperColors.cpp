#include "KeeperColors.h"
#include "ColorPicker.h"
#include "Config.h"
#include "FrameRate.h"
#include "GameAddresses.h"
#include "HookManager.h"
#include "Logger.h"
#include "Memory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <compare>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace KeeperForge2 {
	namespace {
		template<typename T> T Load(const std::uint8_t* at) { T value{}; std::memcpy(&value, at, sizeof(T)); return value; }
		template<typename T> void Store(std::uint8_t* at, T value) { std::memcpy(at, &value, sizeof(T)); }

		// The keepers: players 3 to 7, named by the game's own color for them.
		constexpr std::uint32_t FirstKeeper = 3;
		constexpr std::size_t Keepers = 5;
		constexpr const char* Keys[Keepers] = { "keeper_color_red", "keeper_color_blue", "keeper_color_green", "keeper_color_yellow", "keeper_color_purple" };
		constexpr const wchar_t* Names[Keepers] = { L"Red", L"Blue", L"Green", L"Yellow", L"Purple" };
		// The word for the game's own color, as config.yml's other colors have it.
		constexpr char DefaultWord[] = "game";
		// The table has three shades a player, 4 bytes each (red, green, blue, 255): bright, dark, middle.
		constexpr std::size_t ShadeBytes = 4;
		constexpr std::size_t PlayerBytes = 3 * ShadeBytes;

		std::array<std::array<std::uint8_t, PlayerBytes>, Keepers> gameShades{};
		std::array<std::uint32_t, Keepers> gameColors{};
		std::array<std::optional<std::uint32_t>, Keepers> chosen{};
		bool captured = false;
		bool dirty = true;

		std::uint8_t* PlayerEntry(std::size_t keeper) {
			return reinterpret_cast<std::uint8_t*>(Game::Resolve(Game::Address::PlayerColors)) + (FirstKeeper + keeper) * PlayerBytes;
		}
		// The game's own colors, kept once its table has been filled (by its start-up, which may come after our install).
		void Capture() {
			for (std::size_t keeper = 0; keeper < Keepers; ++keeper) {
				std::memcpy(gameShades[keeper].data(), PlayerEntry(keeper), PlayerBytes);
				const auto& bright = gameShades[keeper];
				gameColors[keeper] = static_cast<std::uint32_t>(bright[0]) << 16 | static_cast<std::uint32_t>(bright[1]) << 8 | bright[2];
			}
			captured = true;
		}
		// The dark and middle shades from the bright one, the way the game's own keeper colors have them: each channel above
		// 50 scaled down from there (230 gives 150 and 190, 50 gives 0 and 50); below it, the dark one is 0.
		std::uint8_t Dark(int channel) { return static_cast<std::uint8_t>(channel > 50 ? (channel - 50) * 150 / 180 : 0); }
		std::uint8_t Middle(int channel) { return static_cast<std::uint8_t>(channel > 50 ? 50 + (channel - 50) * 140 / 180 : channel); }
		void Write(std::size_t keeper) {
			auto* entry = PlayerEntry(keeper);
			if (!chosen[keeper]) {
				std::memcpy(entry, gameShades[keeper].data(), PlayerBytes);
				return;
			}
			const std::uint32_t color = *chosen[keeper];
			const int channels[3] = { static_cast<int>((color >> 16) & 0xFF), static_cast<int>((color >> 8) & 0xFF), static_cast<int>(color & 0xFF) };
			for (int c = 0; c < 3; ++c) {
				entry[c] = static_cast<std::uint8_t>(channels[c]);
				entry[ShadeBytes + c] = Dark(channels[c]);
				entry[2 * ShadeBytes + c] = Middle(channels[c]);
			}
			entry[3] = entry[ShadeBytes + 3] = entry[2 * ShadeBytes + 3] = 0xFF;
		}
		void Apply() {
			if (!captured) Capture();
			for (std::size_t keeper = 0; keeper < Keepers; ++keeper) Write(keeper);
			dirty = false;
		}
		// Every frame: the first one keeps the game's colors and puts ours in; later ones only after a change.
		void AfterPresent() {
			if (!captured || dirty) Apply();
		}
		// The color a keeper is shown in now: the one chosen, else the game's.
		std::uint32_t CurrentColor(std::size_t keeper) { return chosen[keeper] ? *chosen[keeper] : gameColors[keeper]; }
		// Six hex digits, as config.yml's other colors have them, or game for the game's own.
		std::string Word(const std::optional<std::uint32_t>& color) {
			if (!color) return DefaultWord;
			std::string word;
			for (const wchar_t c : ColorPicker::Hex(*color)) if (c != L'#') word += static_cast<char>(c);
			return word;
		}
		void Save(std::size_t keeper) { Config::SetString(Keys[keeper], Word(chosen[keeper])); }

		bool Hook(std::uintptr_t target, void* detour, void** original) {
			if (HookManager::CreateHook(target, detour, original) && HookManager::EnableHook(target)) return true;
			HookManager::RemoveHook(target);
			return false;
		}

		// ---- Claimed floors and wall tops (docs/game/keeper-colors.md). They are meshes of each player's own, loaded at a
		// game's player set-up, whose colour is in their textures. While they load, the textures of the materials they use
		// are noted by player; a texture only one player's meshes use is that player's. When the game paints such a texture
		// into its texture cache, the painted pixels in the keeper's own hue are turned to the chosen color.
		constexpr std::size_t MaterialEntryBytes = 0x10;
		constexpr std::uintptr_t MaterialEntries = 0x4;
		constexpr std::uintptr_t MaterialFlags = 0x10;
		constexpr std::uintptr_t MaterialTextureCount = 0x15;
		constexpr std::uintptr_t MaterialFrameCount = 0x19;
		constexpr std::uint32_t MaterialAllEntries = 0x4000;
		constexpr std::int32_t MaxMaterialEntries = 64;
		constexpr std::size_t TextureNameBytes = 0xC;
		constexpr std::uintptr_t TextureNameHandle = 0x4;
		constexpr std::uintptr_t HandleNumber = 0x24;
		constexpr std::uintptr_t HandleSmaller = 0x18;
		constexpr std::uintptr_t HandleWidth = 0x40;
		constexpr std::uintptr_t HandleHeight = 0x41;
		constexpr int MaxSmaller = 12;
		constexpr std::uintptr_t SurfaceWidth = 0x4;
		constexpr std::uintptr_t SurfaceHeight = 0x8;
		constexpr std::uintptr_t SurfacePitch = 0xC;
		constexpr std::uintptr_t SurfaceFormat = 0x10;
		constexpr std::uintptr_t SurfaceLock = 0x18;
		constexpr std::uintptr_t SurfaceUnlock = 0x1C;
		constexpr std::uintptr_t FormatBytesPerPixel = 0x8;
		constexpr std::uintptr_t FormatRedMask = 0x1C;
		constexpr std::uintptr_t FormatGreenMask = 0x20;
		constexpr std::uintptr_t FormatBlueMask = 0x24;
		constexpr std::uintptr_t ScalerFirstSurface = 0x1;
		constexpr std::uintptr_t ScalerSecondSurface = 0x5;

		// Recursive: loading another keeper's texture to compare paints through our hook again.
		std::recursive_mutex textureLock;
		// Texture number -> the players (bit = player number) whose claimed meshes use it.
		std::map<std::int32_t, std::uint32_t> texturePlayers;
		struct KeeperTexture { std::int32_t number; std::size_t keeper; std::string kind; };
		// The textures only one keeper's claimed meshes use.
		std::vector<KeeperTexture> keeperTextures;
		// The textures only the Good player's claimed meshes use (white where the keepers have their color); keeper unused.
		std::vector<KeeperTexture> goodTextures;
		constexpr std::uint32_t GoodPlayer = 1;
		// The base name of the claimed meshes loading now ("Claimed Floor", "Claimed Top"), null at other times.
		const char* loadingBase = nullptr;
		// How many recolors have been logged this game (the first few, at log_level debug).
		int recolorsLogged = 0;
		constexpr int RecolorsToLog = 8;
		// Weight a pixel (0-255) is the keeper's color, for a keeper texture at one size, worked out once a game.
		struct MaskKey {
			std::int32_t number;
			std::int32_t width;
			std::int32_t height;
			auto operator<=>(const MaskKey&) const = default;
		};
		// For a keeper texture at one size, worked out once a game: how much each pixel is the keeper's color (0-255).
		struct ColorParts {
			std::vector<std::uint8_t> weight;
		};
		std::map<MaskKey, ColorParts> colorMasks;

		// The player whose mesh is loading: the name in the game's buffer is the base, the player object's number d, "_",
		// the piece; player d is number d + 1. -1 if it is not one.
		int LoadingPlayer() {
			if (loadingBase == nullptr) return -1;
			const auto* name = reinterpret_cast<const char*>(Game::Resolve(Game::Address::MeshNameBuffer));
			const std::size_t baseLength = std::strlen(loadingBase);
			if (std::strncmp(name, loadingBase, baseLength) != 0) return -1;
			const char* digits = name + baseLength;
			if (*digits < '0' || *digits > '9') return -1;
			const int object = std::atoi(digits);
			return object >= 0 && object < 31 ? object + 1 : -1;
		}
		void NoteMaterial(std::int32_t number) {
			const int player = LoadingPlayer();
			if (player < 0) return;
			const auto count = Memory::Read<std::int32_t>(Game::Resolve(Game::Address::MaterialCount));
			const auto* materials = Memory::Read<std::uint8_t* const*>(Game::Resolve(Game::Address::Materials));
			if (materials == nullptr || number < 0 || number >= count) return;
			const auto* material = materials[number];
			if (material == nullptr) return;
			const auto flags = Load<std::uint32_t>(material + MaterialFlags);
			const auto* entries = Load<const std::uint8_t*>(material + MaterialEntries);
			if (entries == nullptr) return;
			std::int32_t used = 1;
			if (flags & MaterialAllEntries) used = Load<std::int32_t>(material + MaterialTextureCount) * Load<std::int32_t>(material + MaterialFrameCount);
			used = std::clamp(used, 0, MaxMaterialEntries);
			std::lock_guard guard(textureLock);
			for (std::int32_t entry = 0; entry < used; ++entry) {
				const auto* handle = Load<const std::uint8_t*>(entries + static_cast<std::size_t>(entry) * MaterialEntryBytes);
				if (handle == nullptr) continue;
				texturePlayers[Load<std::int32_t>(handle + HandleNumber)] |= 1u << player;
			}
		}
		const char* TextureName(std::int32_t number) {
			const auto* names = Memory::Read<const std::uint8_t*>(Game::Resolve(Game::Address::TextureNames));
			if (names == nullptr) return "";
			const auto* name = Load<const char*>(names + static_cast<std::size_t>(number) * TextureNameBytes);
			return name != nullptr ? name : "";
		}
		// A texture's kind: its name with each run of digits as '#' (T_Claimed_Path2_64 is T_Claimed_Path#_#), so each
		// keeper's version of one texture has the same kind.
		std::string Kind(const char* name) {
			std::string kind;
			for (const char* at = name; *at != 0; ++at) {
				if (*at >= '0' && *at <= '9') {
					if (kind.empty() || kind.back() != '#') kind += '#';
				}
				else kind += *at;
			}
			return kind;
		}
		void UpdateKeeperTextures() {
			std::lock_guard guard(textureLock);
			keeperTextures.clear();
			goodTextures.clear();
			std::string goodNames;
			std::array<std::string, Keepers> names;
			for (const auto& [number, players] : texturePlayers) {
				// One player only, and a keeper.
				if (players == 0 || (players & (players - 1)) != 0) continue;
				std::uint32_t player = 0;
				while (((players >> player) & 1) == 0) ++player;
				if (player == GoodPlayer) {
					goodTextures.push_back({ number, 0, Kind(TextureName(number)) });
					goodNames += (goodNames.empty() ? "" : ", ") + std::string(TextureName(number));
					continue;
				}
				if (player < FirstKeeper || player >= FirstKeeper + Keepers) continue;
				const std::size_t keeper = player - FirstKeeper;
				keeperTextures.push_back({ number, keeper, Kind(TextureName(number)) });
				names[keeper] += (names[keeper].empty() ? "" : ", ") + std::string(TextureName(number));
			}
			Logger::Info("Keeper colors: " + std::to_string(keeperTextures.size()) + " keeper textures in the claimed floors and wall tops ("
				+ std::to_string(texturePlayers.size()) + " textures seen).");
			Logger::Debug("Keeper colors: Good player textures: " + (goodNames.empty() ? std::string("none") : goodNames) + ".");
			for (std::size_t keeper = 0; keeper < Keepers; ++keeper) {
				Logger::Debug("Keeper colors: " + std::string(Keys[keeper]) + " textures: " + (names[keeper].empty() ? std::string("none") : names[keeper]) + ".");
			}
		}

		using MeshesLoadFn = std::int32_t(__fastcall*)(void* players, void* unused, const char* base);
		MeshesLoadFn originalFloorLoad = nullptr;
		MeshesLoadFn originalTopLoad = nullptr;
		std::int32_t LoadMeshes(MeshesLoadFn original, void* players, void* unused, const char* base) {
			loadingBase = base;
			const auto result = original(players, unused, base);
			loadingBase = nullptr;
			return result;
		}
		// The floors load first at each game's set-up. Texture numbers are given again for each game, so what was noted
		// for the last one no longer holds.
		std::int32_t __fastcall FloorLoadDetour(void* players, void* unused, const char* base) {
			{
				std::lock_guard guard(textureLock);
				texturePlayers.clear();
				keeperTextures.clear();
				recolorsLogged = 0;
				colorMasks.clear();
			}
			return LoadMeshes(originalFloorLoad, players, unused, base);
		}
		// The wall tops load right after the floors: the list is made then.
		std::int32_t __fastcall TopLoadDetour(void* players, void* unused, const char* base) {
			const auto result = LoadMeshes(originalTopLoad, players, unused, base);
			UpdateKeeperTextures();
			return result;
		}
		using MaterialCreateFn = std::int32_t(__cdecl*)(void* description, std::int32_t addNow);
		MaterialCreateFn originalMaterialCreate = nullptr;
		std::int32_t __cdecl MaterialCreateDetour(void* description, std::int32_t addNow) {
			const auto number = originalMaterialCreate(description, addNow);
			if (loadingBase != nullptr) NoteMaterial(number);
			return number;
		}

		// ---- The recolouring.
		struct Hsv { float h; float s; float v; };
		Hsv ToHsv(float r, float g, float b) {
			const float high = (std::max)({ r, g, b });
			const float low = (std::min)({ r, g, b });
			const float range = high - low;
			Hsv out{ 0.0f, high > 0.0f ? range / high : 0.0f, high };
			if (range <= 0.0f) return out;
			if (high == r) out.h = 60.0f * std::fmod((g - b) / range, 6.0f);
			else if (high == g) out.h = 60.0f * ((b - r) / range + 2.0f);
			else out.h = 60.0f * ((r - g) / range + 4.0f);
			if (out.h < 0.0f) out.h += 360.0f;
			return out;
		}
		void FromHsv(Hsv in, float& r, float& g, float& b) {
			float h = std::fmod(in.h, 360.0f);
			if (h < 0.0f) h += 360.0f;
			const float c = in.v * in.s;
			const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
			const float m = in.v - c;
			float rr = 0, gg = 0, bb = 0;
			if (h < 60) { rr = c; gg = x; }
			else if (h < 120) { rr = x; gg = c; }
			else if (h < 180) { gg = c; bb = x; }
			else if (h < 240) { gg = x; bb = c; }
			else if (h < 300) { rr = x; bb = c; }
			else { rr = c; bb = x; }
			r = rr + m;
			g = gg + m;
			b = bb + m;
		}
		Hsv ColorHsv(std::uint32_t color) {
			return ToHsv(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f);
		}
		struct Channel {
			std::uint32_t mask = 0;
			std::uint32_t shift = 0;
			std::uint32_t maximum = 0;
			explicit Channel(std::uint32_t m) : mask(m) {
				if (mask == 0) return;
				while (((mask >> shift) & 1) == 0) ++shift;
				maximum = mask >> shift;
			}
			float Get(std::uint32_t pixel) const { return maximum == 0 ? 0.0f : static_cast<float>((pixel & mask) >> shift) / maximum; }
			std::uint32_t Put(float value) const {
				return (static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * maximum)) << shift) & mask;
			}
		};
		// Which pixels are the keeper's color: where the keeper's texture differs from another keeper's version of the same
		// texture (the stone is the same in all of them). A difference below the low mark is the same pixel, at the full
		// mark or more it is fully the keeper's color.
		constexpr float DifferenceLow = 0.10f;
		constexpr float DifferenceFull = 0.25f;
		// Without another keeper's version: hue close to the keeper's own and strongly colored only.
		constexpr float HueFull = 15.0f;
		constexpr float HueEdge = 30.0f;
		constexpr float SaturationLow = 0.45f;
		constexpr float SaturationFull = 0.65f;
		constexpr float ValueLow = 0.10f;
		float HueDistance(float a, float b) {
			const float d = std::fabs(a - b);
			return d > 180.0f ? 360.0f - d : d;
		}
		float SignedHue(float from, float to) {
			float d = to - from;
			if (d > 180.0f) d -= 360.0f;
			if (d < -180.0f) d += 360.0f;
			return d;
		}
		// A surface locked for reading and writing: the area x, y, width, height, clipped to it.
		struct Locked {
			std::uint8_t* surface = nullptr;
			std::uint8_t* pixels = nullptr;
			std::int32_t pitch = 0;
			std::int32_t bytes = 0;
			std::int32_t surfaceWidth = 0;
			std::int32_t surfaceHeight = 0;
			std::int32_t left = 0, top = 0, right = 0, bottom = 0;
			std::uint32_t masks[3]{};
			std::uintptr_t unlock = 0;
			std::uint32_t Get(std::int32_t column, std::int32_t row) const {
				std::uint32_t pixel = 0;
				std::memcpy(&pixel, pixels + static_cast<std::ptrdiff_t>(row) * pitch + static_cast<std::ptrdiff_t>(column) * bytes, bytes);
				return pixel;
			}
			void Set(std::int32_t column, std::int32_t row, std::uint32_t pixel) {
				std::memcpy(pixels + static_cast<std::ptrdiff_t>(row) * pitch + static_cast<std::ptrdiff_t>(column) * bytes, &pixel, bytes);
			}
			std::size_t Count() const { return static_cast<std::size_t>(right - left) * static_cast<std::size_t>(bottom - top); }
		};
		using LockFn = std::uint8_t*(__fastcall*)(void* surface, void* unused);
		using UnlockFn = void(__fastcall*)(void* surface, void* unused, void* pixels);
		bool Lock(std::uint8_t* surface, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, Locked& out) {
			const auto* vtable = Load<const std::uint8_t*>(surface);
			const auto* format = Load<const std::uint8_t*>(surface + SurfaceFormat);
			if (vtable == nullptr || format == nullptr) return false;
			out.bytes = Load<std::int32_t>(format + FormatBytesPerPixel);
			if (out.bytes != 2 && out.bytes != 4) return false;
			out.pixels = reinterpret_cast<LockFn>(Load<std::uintptr_t>(vtable + SurfaceLock))(surface, nullptr);
			if (out.pixels == nullptr) {
				if (recolorsLogged < RecolorsToLog) {
					++recolorsLogged;
					Logger::Debug("Keeper colors: could not lock a texture page to recolor it.");
				}
				return false;
			}
			out.surface = surface;
			out.unlock = Load<std::uintptr_t>(vtable + SurfaceUnlock);
			out.pitch = Load<std::int32_t>(surface + SurfacePitch);
			out.surfaceWidth = Load<std::int32_t>(surface + SurfaceWidth);
			out.surfaceHeight = Load<std::int32_t>(surface + SurfaceHeight);
			out.masks[0] = Load<std::uint32_t>(format + FormatRedMask);
			out.masks[1] = Load<std::uint32_t>(format + FormatGreenMask);
			out.masks[2] = Load<std::uint32_t>(format + FormatBlueMask);
			out.left = (std::max)(x, 0);
			out.top = (std::max)(y, 0);
			out.right = (std::max)(out.left, (std::min)(x + width, out.surfaceWidth));
			out.bottom = (std::max)(out.top, (std::min)(y + height, out.surfaceHeight));
			return true;
		}
		void Unlock(Locked& locked) {
			if (locked.pixels != nullptr) reinterpret_cast<UnlockFn>(locked.unlock)(locked.surface, nullptr, locked.pixels);
			locked.pixels = nullptr;
		}

		using PaintFn = void(__fastcall*)(std::uint8_t* destination, void* unused, std::uint8_t* source, std::int32_t x, std::int32_t y);
		PaintFn originalPaint = nullptr;
		using HandleLoadFn = void(__fastcall*)(std::uint8_t* handle);
		// The texture handles of the other keepers' versions of this texture at this size.
		std::vector<std::uint8_t*> OtherVersions(const KeeperTexture& texture, std::int32_t width, std::int32_t height) {
			std::vector<std::uint8_t*> handles;
			const auto* names = Memory::Read<const std::uint8_t*>(Game::Resolve(Game::Address::TextureNames));
			if (names == nullptr) return handles;
			for (const auto& other : keeperTextures) {
				if (other.keeper == texture.keeper || other.kind != texture.kind) continue;
				auto* handle = Load<std::uint8_t*>(names + static_cast<std::size_t>(other.number) * TextureNameBytes + TextureNameHandle);
				for (int step = 0; handle != nullptr && step < MaxSmaller; ++step) {
					if (Load<std::uint8_t>(handle + HandleWidth) == width && Load<std::uint8_t>(handle + HandleHeight) == height) {
						handles.push_back(handle);
						break;
					}
					handle = Load<std::uint8_t*>(handle + HandleSmaller);
				}
			}
			return handles;
		}
		// A handle's surface, loaded if it is not yet; null unless it is this size.
		std::uint8_t* LoadedSurface(std::uint8_t* handle, std::int32_t width, std::int32_t height) {
			if (Load<std::uint8_t*>(handle) == nullptr) reinterpret_cast<HandleLoadFn>(Game::Resolve(Game::Address::TextureHandleLoad))(handle);
			auto* surface = Load<std::uint8_t*>(handle);
			if (surface == nullptr || Load<std::int32_t>(surface + SurfaceWidth) != width || Load<std::int32_t>(surface + SurfaceHeight) != height) return nullptr;
			return surface;
		}
		float Brightness(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }
		// The keeper texture just painted at x, y is compared with each other keeper's version, painted in turn to the same
		// place, then put back. A pixel counts by its smallest difference: a colored part differs from every other keeper's
		// color, while a spot where one version's stone differs a little (compression) rarely differs from them all. Empty if
		// there is no other version.
		ColorParts FindColorParts(std::uint8_t* destination, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, const KeeperTexture& texture) {
			const MaskKey key{ texture.number, width, height };
			if (const auto found = colorMasks.find(key); found != colorMasks.end()) return found->second;
			colorMasks[key] = {};
			ColorParts parts;
			// The other keepers' versions: the same stone, only the colored parts differ. (The Good player's versions were
			// tried once: its floor texture has a name of its own, T_Claimed_Good, and its wall tops' white is flat and much
			// brighter than the keepers' trim, so the tint came out flat and too bright.)
			const auto others = OtherVersions(texture, width, height);
			if (others.empty()) return parts;
			Locked locked;
			if (!Lock(destination, x, y, width, height, locked)) return parts;
			std::vector<std::uint32_t> saved;
			saved.reserve(locked.Count());
			for (std::int32_t row = locked.top; row < locked.bottom; ++row)
				for (std::int32_t column = locked.left; column < locked.right; ++column) saved.push_back(locked.Get(column, row));
			const Channel channels[3] = { Channel(locked.masks[0]), Channel(locked.masks[1]), Channel(locked.masks[2]) };
			Unlock(locked);
			std::vector<float> smallest(saved.size(), 1.0f);
			std::size_t compared = 0;
			for (auto* handle : others) {
				auto* other = LoadedSurface(handle, width, height);
				if (other == nullptr) continue;
				originalPaint(destination, nullptr, other, x, y);
				if (!Lock(destination, x, y, width, height, locked)) continue;
				if (saved.size() == locked.Count()) {
					std::size_t at = 0;
					for (std::int32_t row = locked.top; row < locked.bottom; ++row) {
						for (std::int32_t column = locked.left; column < locked.right; ++column, ++at) {
							const std::uint32_t theirs = locked.Get(column, row);
							float difference = 0.0f;
							for (const auto& channel : channels) difference = (std::max)(difference, std::fabs(channel.Get(saved[at]) - channel.Get(theirs)));
							smallest[at] = (std::min)(smallest[at], difference);
						}
					}
					++compared;
				}
				Unlock(locked);
			}
			// The keeper's own pixels back.
			if (Lock(destination, x, y, width, height, locked)) {
				if (saved.size() == locked.Count()) {
					std::size_t at = 0;
					for (std::int32_t row = locked.top; row < locked.bottom; ++row)
						for (std::int32_t column = locked.left; column < locked.right; ++column, ++at) locked.Set(column, row, saved[at]);
				}
				Unlock(locked);
			}
			if (compared == 0) return parts;
			parts.weight.resize(saved.size());
			std::size_t colored = 0;
			for (std::size_t at = 0; at < saved.size(); ++at) {
				const float weight = std::clamp((smallest[at] - DifferenceLow) / (DifferenceFull - DifferenceLow), 0.0f, 1.0f);
				parts.weight[at] = static_cast<std::uint8_t>(std::lround(weight * 255.0f));
				if (weight > 0.5f) ++colored;
			}
			colorMasks[key] = parts;
			Logger::Debug("Keeper colors: " + std::string(TextureName(texture.number)) + " " + std::to_string(width) + "x" + std::to_string(height) + ": "
				+ std::to_string(colored) + " of " + std::to_string(parts.weight.size()) + " pixels are the keeper's color (against "
				+ std::to_string(compared) + " other keepers' versions).");
			return parts;
		}
		// The chosen color (tint, 0-1 a channel), shaded as a pixel of the keeper's own color is: shade is the pixel's
		// brightness over the keeper's game color's (gameLight). At 1 or below the tint is darkened by it; above, blended
		// toward white, reaching it at a white pixel.
		void ShadeTint(const float tint[3], float shade, float gameLight, float out[3]) {
			if (shade <= 1.0f) {
				for (int c = 0; c < 3; ++c) out[c] = tint[c] * shade;
				return;
			}
			const float toWhite = gameLight < 1.0f ? std::clamp((shade - 1.0f) * gameLight / (1.0f - gameLight), 0.0f, 1.0f) : 0.0f;
			for (int c = 0; c < 3; ++c) out[c] = tint[c] + (1.0f - tint[c]) * toWhite;
		}
		float ColorBrightness(std::uint32_t color) {
			return Brightness(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f);
		}
		void Recolor(std::uint8_t* surface, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height, std::size_t keeper, const ColorParts& parts) {
			Locked locked;
			if (!Lock(surface, x, y, width, height, locked)) return;
			const Channel red(locked.masks[0]), green(locked.masks[1]), blue(locked.masks[2]);
			const std::uint32_t colorBits = red.mask | green.mask | blue.mask;
			const bool masked = parts.weight.size() == locked.Count();
			const std::uint32_t color = *chosen[keeper];
			const float tint[3] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f };
			// The keeper's game color's brightness: a colored pixel that bright becomes the chosen color itself, darker ones
			// darker in step, brighter ones toward white (at full white).
			const float gameLight = (std::max)(ColorBrightness(gameColors[keeper]), 0.05f);
			const Hsv from = ColorHsv(gameColors[keeper]);
			const Hsv to = ColorHsv(color);
			const float saturationScale = from.s > 0.0f ? to.s / from.s : 1.0f;
			const float valueScale = from.v > 0.0f ? to.v / from.v : 1.0f;
			std::size_t at = 0;
			for (std::int32_t row = locked.top; row < locked.bottom; ++row) {
				for (std::int32_t column = locked.left; column < locked.right; ++column, ++at) {
					std::uint32_t pixel = locked.Get(column, row);
					const float r = red.Get(pixel), g = green.Get(pixel), b = blue.Get(pixel);
					float weight = 0.0f;
					float nr = r, ng = g, nb = b;
					if (masked) {
						// The chosen color, shaded as the keeper's own color is there.
						weight = parts.weight[at] / 255.0f;
						if (weight <= 0.0f) continue;
						float shaded[3];
						ShadeTint(tint, Brightness(r, g, b) / gameLight, gameLight, shaded);
						nr = shaded[0];
						ng = shaded[1];
						nb = shaded[2];
					}
					else {
						const Hsv hsv = ToHsv(r, g, b);
						if (hsv.s >= SaturationLow && hsv.v >= ValueLow) {
							const float distance = HueDistance(hsv.h, from.h);
							if (distance < HueEdge) {
								weight = distance <= HueFull ? 1.0f : (HueEdge - distance) / (HueEdge - HueFull);
								if (hsv.s < SaturationFull) weight *= (hsv.s - SaturationLow) / (SaturationFull - SaturationLow);
							}
						}
						if (weight <= 0.0f) continue;
						const Hsv turned{ to.h + SignedHue(from.h, hsv.h), std::clamp(hsv.s * saturationScale, 0.0f, 1.0f), std::clamp(hsv.v * valueScale, 0.0f, 1.0f) };
						FromHsv(turned, nr, ng, nb);
					}
					nr = r + (nr - r) * weight;
					ng = g + (ng - g) * weight;
					nb = b + (nb - b) * weight;
					pixel = (pixel & ~colorBits) | red.Put(nr) | green.Put(ng) | blue.Put(nb);
					locked.Set(column, row, pixel);
				}
			}
			Unlock(locked);
			if (recolorsLogged < RecolorsToLog) {
				++recolorsLogged;
				Logger::Debug("Keeper colors: recolored " + std::to_string(locked.right - locked.left) + "x" + std::to_string(locked.bottom - locked.top) + " at "
					+ std::to_string(x) + ", " + std::to_string(y) + " for " + Keys[keeper]
					+ (masked ? " (colored parts from another keeper's version, shaded as the game's)" : " (by hue)")
					+ " (page " + std::to_string(locked.surfaceWidth) + "x" + std::to_string(locked.surfaceHeight) + ", " + std::to_string(locked.bytes) + " bytes a pixel).");
			}
		}
		// The keeper texture this surface is (any of its sizes), or null.
		const KeeperTexture* KeeperTextureOf(const std::uint8_t* surface) {
			const auto* names = Memory::Read<const std::uint8_t*>(Game::Resolve(Game::Address::TextureNames));
			if (names == nullptr) return nullptr;
			for (const auto& texture : keeperTextures) {
				if (!chosen[texture.keeper]) continue;
				const auto* handle = Load<const std::uint8_t*>(names + static_cast<std::size_t>(texture.number) * TextureNameBytes + TextureNameHandle);
				for (int step = 0; handle != nullptr && step < MaxSmaller; ++step) {
					if (Load<const std::uint8_t*>(handle) == surface) return &texture;
					handle = Load<const std::uint8_t*>(handle + HandleSmaller);
				}
			}
			return nullptr;
		}
		bool AnyChosen() {
			return std::any_of(chosen.begin(), chosen.end(), [](const auto& color) { return color.has_value(); });
		}
		void __fastcall PaintDetour(std::uint8_t* destination, void* unused, std::uint8_t* source, std::int32_t x, std::int32_t y) {
			originalPaint(destination, unused, source, x, y);
			if (destination == nullptr || source == nullptr || !captured || !AnyChosen()) return;
			// Not the scaler's work surfaces: what is painted there is painted again into the cache, and turned then.
			const auto scaler = Game::Resolve(Game::Address::SurfaceScaler);
			if (destination == Memory::Read<std::uint8_t*>(scaler + ScalerFirstSurface) || destination == Memory::Read<std::uint8_t*>(scaler + ScalerSecondSurface)) return;
			std::lock_guard guard(textureLock);
			if (keeperTextures.empty()) return;
			const auto* found = KeeperTextureOf(source);
			if (found == nullptr) return;
			const KeeperTexture texture = *found;
			const auto width = Load<std::int32_t>(source + SurfaceWidth);
			const auto height = Load<std::int32_t>(source + SurfaceHeight);
			const auto parts = FindColorParts(destination, x, y, width, height, texture);
			Recolor(destination, x, y, width, height, texture.keeper, parts);
		}

		// ---- The minimap (docs/game/keeper-colors.md). Its colors come from GUI\map\MapColours.png, read when a level's
		// interface is set up (0x41E810, fastcall (minimap)) into the minimap's table (minimap + 0x138A, 0xFFBBGGRR each: red in the low byte),
		// three a player (players 1-7 from + 0x48, + 0x6C and + 0x90, 4 bytes a player); each is also handed to the
		// minimap's drawer (minimap + 0x53), thiscall vtable + 0x294 (player, kind 1 / 0 / 2, color). After it, the keepers
		// with a color of their own get it in all three, shaded as the game's own entry is.
		constexpr std::uintptr_t MinimapColorTable = 0x138A;
		constexpr std::uintptr_t MinimapDrawer = 0x53;
		constexpr std::uintptr_t MinimapDrawerSetColor = 0x294;
		struct MinimapKind { std::int32_t kind; std::uintptr_t first; };
		constexpr MinimapKind MinimapKinds[] = { { 1, 0x48 }, { 0, 0x6C }, { 2, 0x90 } };
		using MinimapLoadFn = void(__fastcall*)(std::uint8_t* minimap);
		using MinimapSetColorFn = void(__fastcall*)(void* drawer, void* unused, std::int32_t player, std::int32_t kind, std::uint32_t* color);
		MinimapLoadFn originalMinimapLoad = nullptr;
		void __fastcall MinimapLoadDetour(std::uint8_t* minimap) {
			originalMinimapLoad(minimap);
			if (minimap == nullptr || !captured) return;
			auto* table = Load<std::uint8_t*>(minimap + MinimapColorTable);
			auto* drawer = Load<std::uint8_t*>(minimap + MinimapDrawer);
			if (table == nullptr) return;
			std::string changed;
			for (std::size_t keeper = 0; keeper < Keepers; ++keeper) {
				if (!chosen[keeper]) continue;
				const std::int32_t player = static_cast<std::int32_t>(FirstKeeper + keeper);
				const std::uint32_t color = *chosen[keeper];
				const float tint[3] = { ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f };
				const float gameLight = (std::max)(ColorBrightness(gameColors[keeper]), 0.05f);
				for (const auto& kind : MinimapKinds) {
					auto* entry = table + kind.first + static_cast<std::size_t>(player - 1) * 4;
					const auto stored = Load<std::uint32_t>(entry);
					// Not filled (the picture could not be read).
					if (stored == 0) continue;
					// Stored blue, green, red from the top (0xFFBBGGRR); as 0xRRGGBB to work with.
					const std::uint32_t game = (stored & 0xFF) << 16 | (stored & 0xFF00) | ((stored >> 16) & 0xFF);
					float shaded[3];
					ShadeTint(tint, ColorBrightness(game) / gameLight, gameLight, shaded);
					// Back in the table's order: red in the low byte.
					std::uint32_t value = 0xFF000000u;
					for (int c = 0; c < 3; ++c) value |= static_cast<std::uint32_t>(std::lround(std::clamp(shaded[c], 0.0f, 1.0f) * 255.0f)) << (8 * c);
					Store<std::uint32_t>(entry, value);
					if (drawer != nullptr) {
						std::uint32_t handed = value;
						const auto setColor = reinterpret_cast<MinimapSetColorFn>(Load<std::uintptr_t>(Load<const std::uint8_t*>(drawer) + MinimapDrawerSetColor));
						setColor(drawer, nullptr, player, kind.kind, &handed);
					}
				}
				changed += (changed.empty() ? "" : ", ") + std::string(Keys[keeper]);
			}
			if (!changed.empty()) Logger::Debug("Keeper colors: minimap colors set for " + changed + ".");
		}

		bool InstallTerrainHooks() {
			struct Target { std::uintptr_t address; std::vector<std::uint8_t> bytes; void* detour; void** original; };
			const Target targets[] = {
				{ Game::Resolve(Game::Address::ClaimedFloorMeshesLoad), { 0x83, 0xEC, 0x34, 0x8B, 0x41, 0x06, 0x53, 0x8B, 0x5C, 0x24, 0x3C },
					reinterpret_cast<void*>(&FloorLoadDetour), reinterpret_cast<void**>(&originalFloorLoad) },
				{ Game::Resolve(Game::Address::ClaimedTopMeshesLoad), { 0x83, 0xEC, 0x34, 0x8B, 0x41, 0x06, 0x53, 0x8B, 0x5C, 0x24, 0x3C },
					reinterpret_cast<void*>(&TopLoadDetour), reinterpret_cast<void**>(&originalTopLoad) },
				{ Game::Resolve(Game::Address::MaterialCreate), { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x0B, 0xFA, 0x64, 0x00 },
					reinterpret_cast<void*>(&MaterialCreateDetour), reinterpret_cast<void**>(&originalMaterialCreate) },
				{ Game::Resolve(Game::Address::EngineSurfacePaint), { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x55 },
					reinterpret_cast<void*>(&PaintDetour), reinterpret_cast<void**>(&originalPaint) },
				{ Game::Resolve(Game::Address::MinimapColorsLoad), { 0x83, 0xEC, 0x38, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57 },
					reinterpret_cast<void*>(&MinimapLoadDetour), reinterpret_cast<void**>(&originalMinimapLoad) },
			};
			for (const auto& target : targets) {
				if (!Memory::Matches(target.address, target.bytes)) return false;
			}
			for (const auto& target : targets) {
				if (!Hook(target.address, target.detour, target.original)) return false;
			}
			return true;
		}

		// ---- The color boxes: one a row of the skirmish screen's player list, just right of its Is Ready column, in
		// the keeper's color. Clicking one opens the color picker for that row's keeper. One widget (a copy of Add
		// Computer Player) covers the column beside the list: its draw draws the boxes where the list draws its rows,
		// its click picks the row under the mouse.
		constexpr std::int32_t BoxesButtonId = 9002;
		constexpr std::int32_t AddComputerButtonId = 528;
		constexpr std::int32_t PlayerListButtonId = 86;
		constexpr std::int32_t ButtonListEnd = -1;
		constexpr std::size_t MaxButtons = 64;
		// The list's box is x 64, y 536, w 1328, h 300 (layout); the space up to the buttons at x 1480 is free.
		constexpr std::int16_t BoxesX = 1394;
		constexpr std::int16_t BoxesWidth = 84;
		constexpr std::int16_t BoxesY = 536;
		constexpr std::int16_t BoxesHeight = 300;
		// The boxes fill the row less a 1-pixel gap at the bottom, and the widget's whole width (the free column beside the list).
		constexpr std::int32_t BoxInset = 0;
		constexpr std::int32_t BoxGap = 1;
		constexpr std::int32_t MinBoxSize = 8;
		constexpr std::size_t ImageRecordBytes = 0x28;
		std::vector<std::uint8_t> buttonList;
		// Player slots: 8 of 63 bytes, the type in the low 3 bits of +0x3A (0 empty). The list shows the slots in use in order.
		constexpr std::size_t SlotBytes = 63;
		constexpr std::size_t Slots = 8;
		constexpr std::uintptr_t SlotFlags = 0x3A;
		constexpr std::uint8_t SlotTypeMask = 0x07;

		std::uint8_t SlotType(std::size_t slot) {
			return Load<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(Game::Resolve(Game::Address::PlayerSlots)) + slot * SlotBytes + SlotFlags) & SlotTypeMask;
		}

		using ButtonRectFn = std::int32_t*(__fastcall*)(void* button, void* unused, std::int32_t* box);
		using BoxToScreenFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* box, const std::int32_t* layoutBox);
		using MouseToLayoutFn = std::int32_t*(__fastcall*)(void* manager, void* unused, std::int32_t* out);
		using PictureDrawFn = void(__fastcall*)(void* drawer, void* unused, std::int32_t* out, void* frame, std::int32_t x, std::int32_t y, void* picture,
			const std::int32_t* area, std::int32_t mode);
		using ImageCreateFn = void*(__fastcall*)(void* record, void* unused, const std::int32_t* widthHeight, void* format, std::int32_t, std::int32_t);
		using ImageAllocateFn = void(__fastcall*)(void* record, void* unused, std::int32_t* result);

		// A widget's box in frame pixels.
		bool WidgetBox(std::uint8_t* widget, std::uint8_t* frontEnd, std::int32_t* box) {
			std::int32_t layoutBox[4]{};
			reinterpret_cast<ButtonRectFn>(Game::Resolve(Game::Address::GuiButtonRect))(widget, nullptr, layoutBox);
			reinterpret_cast<BoxToScreenFn>(Game::Resolve(Game::Address::GuiBoxToScreen))(frontEnd + Game::Offset::MenuGuiManager, nullptr, box, layoutBox);
			return box[2] > box[0] && box[3] > box[1];
		}
		// The skirmish screen's player list widget, if the screen is up.
		std::uint8_t* PlayerList(std::uint8_t* frontEnd) {
			auto* window = Memory::Read<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(frontEnd) + Game::Offset::FrontEndWindowList);
			for (int guard = 0; window != nullptr && guard < 256; ++guard) {
				if (Load<std::int32_t>(window + Game::Offset::MenuWindowId) == Game::SkirmishWindowId) {
					auto* widget = Load<std::uint8_t*>(window + Game::Offset::MenuWindowWidgets);
					for (int count = 0; widget != nullptr && count < 256; ++count) {
						if (Load<std::int32_t>(widget + Game::Offset::MenuWidgetId) == PlayerListButtonId) return widget;
						widget = Load<std::uint8_t*>(widget + Game::Offset::MenuWidgetNext);
					}
					return nullptr;
				}
				window = Load<std::uint8_t*>(window + Game::Offset::MenuWindowNext);
			}
			return nullptr;
		}
		// The rows as drawn last (frame pixels): the slot, and its box.
		struct RowBox { std::size_t slot; std::int32_t left, top, right, bottom; };
		std::vector<RowBox> rowBoxes;
		// Works out the rows' boxes from the list's own box and row height; false if the list is not there.
		bool LayOutRows(std::uint8_t* button, std::uint8_t* frontEnd) {
			rowBoxes.clear();
			auto* list = PlayerList(frontEnd);
			std::int32_t listBox[4]{};
			std::int32_t ownBox[4]{};
			if (list == nullptr || !WidgetBox(list, frontEnd, listBox) || !WidgetBox(button, frontEnd, ownBox)) return false;
			const auto rowHeight = Load<std::int32_t>(list + Game::Offset::MenuListRowHeight);
			if (rowHeight <= 0) return false;
			const std::int32_t height = (std::max)(rowHeight - BoxGap, MinBoxSize);
			const std::int32_t width = (std::max)(ownBox[2] - ownBox[0], height);
			const std::int32_t left = ownBox[0] + ((ownBox[2] - ownBox[0]) - width) / 2;
			const std::uint8_t mapSlots = Load<std::uint8_t>(frontEnd + Game::Offset::FrontEndSlotCount);
			std::int32_t top = listBox[1];
			for (std::size_t slot = 0; slot < Slots && slot < mapSlots; ++slot) {
				if (SlotType(slot) == 0) continue;
				if (slot < Keepers) rowBoxes.push_back({ slot, left, top + BoxInset, left + width, top + BoxInset + height });
				top += rowHeight;
			}
			return true;
		}
		// The mouse in frame pixels, as the boxes are.
		bool MouseInFrame(std::uint8_t* frontEnd, std::int32_t& x, std::int32_t& y) {
			auto* manager = frontEnd + Game::Offset::MenuGuiManager;
			std::int32_t layout[2]{};
			reinterpret_cast<MouseToLayoutFn>(Game::Resolve(Game::Address::GuiMouseToLayout))(manager, nullptr, layout);
			const std::int32_t layoutBox[4] = { layout[0], layout[1], layout[0] + 1, layout[1] + 1 };
			std::int32_t box[4]{};
			reinterpret_cast<BoxToScreenFn>(Game::Resolve(Game::Address::GuiBoxToScreen))(manager, nullptr, box, layoutBox);
			x = box[0];
			y = box[1];
			return true;
		}

		// The picture the boxes are drawn with (the game's pixel format), refilled for each box.
		alignas(4) std::uint8_t boxPicture[ImageRecordBytes]{};
		bool boxPictureMade = false;
		std::int32_t boxPictureWidth = 0;
		std::int32_t boxPictureHeight = 0;
		bool MakeBoxPicture(std::int32_t width, std::int32_t height) {
			if (boxPictureMade && boxPictureWidth == width && boxPictureHeight == height) return true;
			if (boxPictureMade) reinterpret_cast<void(__fastcall*)(void*)>(Game::Resolve(Game::Address::ImageFree))(boxPicture);
			boxPictureMade = false;
			const std::int32_t widthHeight[2] = { width, height };
			alignas(4) std::uint8_t made[ImageRecordBytes]{};
			auto* result = reinterpret_cast<ImageCreateFn>(Game::Resolve(Game::Address::ImageCreate))(
				made, nullptr, widthHeight, reinterpret_cast<void*>(Game::Resolve(Game::Address::ImagePixelFormat)), 0, 0);
			std::memcpy(boxPicture, result != nullptr ? result : made, ImageRecordBytes);
			std::int32_t allocated = -1;
			reinterpret_cast<ImageAllocateFn>(Game::Resolve(Game::Address::ImageAllocate))(boxPicture, nullptr, &allocated);
			if (allocated != 0 || Load<std::uint8_t*>(boxPicture + 0x18) == nullptr) return false;
			boxPictureMade = true;
			boxPictureWidth = width;
			boxPictureHeight = height;
			return true;
		}
		// Fills the picture: the color inside a 1-pixel border (dark, or white under the mouse).
		void FillBoxPicture(std::uint32_t color, bool hovered) {
			const auto* record = reinterpret_cast<const std::uint32_t*>(boxPicture);
			const Channel red(record[0]), green(record[1]), blue(record[2]);
			const std::uint32_t alpha = record[3];
			const std::uint32_t bytes = (record[4] + 7) / 8;
			if (bytes == 0 || bytes > 4) return;
			auto* pixels = Load<std::uint8_t*>(boxPicture + 0x18);
			const auto pitch = Load<std::uint32_t>(boxPicture + 0x24);
			const auto pack = [&](std::uint32_t rgb) {
				return red.Put(((rgb >> 16) & 0xFF) / 255.0f) | green.Put(((rgb >> 8) & 0xFF) / 255.0f) | blue.Put((rgb & 0xFF) / 255.0f) | alpha;
			};
			const std::uint32_t inside = pack(color);
			const std::uint32_t border = pack(hovered ? 0xFFFFFF : 0x101010);
			for (std::int32_t y = 0; y < boxPictureHeight; ++y) {
				for (std::int32_t x = 0; x < boxPictureWidth; ++x) {
					const bool edge = x == 0 || y == 0 || x == boxPictureWidth - 1 || y == boxPictureHeight - 1;
					const std::uint32_t value = edge ? border : inside;
					std::memcpy(pixels + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * bytes, &value, bytes);
				}
			}
		}
		char __cdecl RenderBoxes(std::uint8_t* button, std::uint8_t* frontEnd) {
			if (!captured || frontEnd == nullptr || !LayOutRows(button, frontEnd) || rowBoxes.empty()) return 0;
			if (!MakeBoxPicture(rowBoxes.front().right - rowBoxes.front().left, rowBoxes.front().bottom - rowBoxes.front().top)) return 0;
			std::int32_t mouseX = 0, mouseY = 0;
			MouseInFrame(frontEnd, mouseX, mouseY);
			for (const auto& row : rowBoxes) {
				const bool hovered = mouseX >= row.left && mouseX < row.right && mouseY >= row.top && mouseY < row.bottom;
				FillBoxPicture(CurrentColor(row.slot), hovered);
				std::int32_t out[4]{};
				reinterpret_cast<PictureDrawFn>(Game::Resolve(Game::Address::PictureDraw))(frontEnd + Game::Offset::FrontEndPictureDrawer, nullptr, out,
					frontEnd + Game::Offset::FrontEndFrame, row.left, row.top, boxPicture, nullptr, 0);
			}
			return 0;
		}
		void OpenPicker(std::size_t keeper) {
			const auto before = chosen[keeper];
			ColorPicker::Open(std::wstring(L"Keeper Color - ") + Names[keeper] + L" keeper", CurrentColor(keeper), gameColors[keeper],
				[keeper](std::uint32_t now) {
					chosen[keeper] = now;
					dirty = true;
				},
				[keeper, before](std::uint32_t now, bool kept) {
					if (kept) {
						// The game's own color again is no color of ours.
						if (now == gameColors[keeper]) chosen[keeper].reset();
						else chosen[keeper] = now;
						Save(keeper);
						Logger::Info("Keeper colors: " + std::string(Keys[keeper]) + " is now " + Word(chosen[keeper]) + ".");
					}
					else chosen[keeper] = before;
					dirty = true;
				});
		}
		void __cdecl OnBoxesClick(std::int32_t, std::int32_t, std::uint8_t* frontEnd) {
			if (frontEnd == nullptr) return;
			if (!captured) Apply();
			// The rows as the boxes were last drawn (every frame the screen is up).
			if (rowBoxes.empty()) return;
			std::int32_t mouseX = 0, mouseY = 0;
			MouseInFrame(frontEnd, mouseX, mouseY);
			// The row under the mouse; anywhere across the column counts, so a click a little off the box still opens it.
			for (const auto& row : rowBoxes) {
				if (mouseY >= row.top && mouseY < row.bottom + BoxGap) {
					OpenPicker(row.slot);
					return;
				}
			}
		}
		// The skirmish screen's list as it is now (with the other features' buttons), ours added before the end.
		bool AddButton() {
			const auto windowConfig = Game::Resolve(Game::Address::SkirmishWindowConfig);
			const auto* buttons = reinterpret_cast<const std::uint8_t*>(Memory::Read<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons));
			std::size_t count = 0;
			const std::uint8_t* addComputer = nullptr;
			bool listThere = false;
			for (; count < MaxButtons; ++count) {
				const auto* entry = buttons + count * Game::ButtonConfigSize;
				const auto id = Load<std::int32_t>(entry + Game::Offset::ButtonConfigId);
				if (id == ButtonListEnd) break;
				if (id == BoxesButtonId) return false;
				if (id == AddComputerButtonId) addComputer = entry;
				if (id == PlayerListButtonId) listThere = true;
			}
			if (count == MaxButtons || addComputer == nullptr || !listThere) return false;
			buttonList.assign((count + 2) * Game::ButtonConfigSize, 0);
			std::memcpy(buttonList.data(), buttons, count * Game::ButtonConfigSize);
			auto* ours = buttonList.data() + count * Game::ButtonConfigSize;
			std::memcpy(ours, addComputer, Game::ButtonConfigSize);
			Store<std::int32_t>(ours + Game::Offset::ButtonConfigId, BoxesButtonId);
			Store<std::uint32_t>(ours + Game::Offset::ButtonConfigClick, reinterpret_cast<std::uint32_t>(&OnBoxesClick));
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigX, BoxesX);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigY, BoxesY);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigWidth, BoxesWidth);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigHeight, BoxesHeight);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigTextWidth, BoxesWidth);
			Store<std::int16_t>(ours + Game::Offset::ButtonConfigTextHeight, BoxesHeight);
			Store<std::uint32_t>(ours + Game::Offset::ButtonConfigRender, reinterpret_cast<std::uint32_t>(&RenderBoxes));
			Store<std::int32_t>(ours + Game::Offset::ButtonConfigTextId, 0);
			Store<std::int32_t>(ours + Game::Offset::ButtonConfigTextIndex, 0);
			std::memcpy(ours + Game::ButtonConfigSize, buttons + count * Game::ButtonConfigSize, Game::ButtonConfigSize);
			Memory::Write<std::uint32_t>(windowConfig + Game::Offset::WindowConfigButtons, reinterpret_cast<std::uint32_t>(buttonList.data()));
			return true;
		}
	}

	void KeeperColors::Install() {
		std::string set;
		for (std::size_t keeper = 0; keeper < Keepers; ++keeper) {
			const auto word = Config::GetString(Keys[keeper], DefaultWord);
			std::uint32_t color = 0;
			std::wstring wide;
			for (const char c : word) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
			if (ColorPicker::ParseHex(wide, color)) {
				chosen[keeper] = color;
				set += (set.empty() ? "" : ", ") + std::string(Keys[keeper]) + " " + word;
			}
			else if (word != DefaultWord) Logger::Warning("Keeper colors: " + std::string(Keys[keeper]) + " \"" + word + "\" is not six hex digits or game; the game's color is used.");
		}
		if (!FrameRate::IsPresentHooked()) {
			Logger::Warning("Keeper colors: no frame hook, keeper colors are left as the game's.");
			return;
		}
		FrameRate::AddAfterPresent(&AfterPresent);
		if (!InstallTerrainHooks()) Logger::Warning("Keeper colors: unexpected code at the claimed terrain's loading, texture painting or minimap colors; claimed floors, wall tops and the minimap keep the game's colors.");
		if (!AddButton()) Logger::Warning("Keeper colors: unexpected skirmish screen, the color boxes are left out (colors from config.yml still apply).");
		Logger::Info("Keeper colors: " + (set.empty() ? std::string("the game's own") : set) + ".");
	}
}
