#include "DirectDrawLayer.h"
#include "BackgroundRunning.h"
#include "GameFixes.h"
#include "CrashHandler.h"
#include "Logger.h"
#include <Windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// How the game uses DirectDraw and Direct3D 6 (the same calls patch.dll's wrapper answers, see docs/game/renderer.md):
// - DirectDraw 1 makes the screen (a primary surface with one back buffer), plain surfaces and the depth buffer;
//   DirectDraw 2 is asked for FourCC (video) surfaces; DirectDraw 4 makes the textures.
// - The back buffer is the 3D render target. The game draws 3D into it, then locks it and blits its panels onto it,
//   then flips. It sometimes draws straight onto the primary surface too.
// - Direct3D 3 makes one device on the back buffer and draws indexed triangle lists of pre-transformed vertices.
// So every surface lives in memory here. The back buffer also lives in the backend (the frame): whichever copy was
// changed last is the right one, and the other is brought up to date only when it is needed.
namespace KeeperForge2::Render::Layer {
	namespace {
		std::recursive_mutex layerMutex;
		// Which thread holds the lock and since when, for the hang watchdog (StartWatchdog).
		std::atomic<DWORD> lockOwner = 0;
		std::atomic<ULONGLONG> lockSince = 0;
		thread_local int lockDepth = 0;
		class Guard {
			public:
				explicit Guard(std::recursive_mutex& mutex) : mutex(mutex) {
					mutex.lock();
					if (lockDepth++ == 0) {
						lockSince = GetTickCount64();
						lockOwner = GetCurrentThreadId();
					}
				}
				~Guard() {
					if (--lockDepth == 0) lockOwner = 0;
					mutex.unlock();
				}
				Guard(const Guard&) = delete;
				Guard& operator=(const Guard&) = delete;
			private:
				std::recursive_mutex& mutex;
		};

		Api selectedApi = Api::D3D11;
		std::unique_ptr<Backend> backend;
		// When the game closes, the backend is left for Windows to clean up: by the time static objects are destroyed
		// the game's window is gone and the graphics drivers may already be unloading (destroying it there crashed).
		// Declared after backend, so it runs first.
		struct LeaveBackendAtExit {
			~LeaveBackendAtExit() { static_cast<void>(backend.release()); }
		} leaveBackendAtExit;
		bool backendFailed = false;
		bool inUse = false;
		HWND window = nullptr;
		std::uint32_t modeWidth = 640;
		std::uint32_t modeHeight = 480;
		bool stretch = false;
		Renderer::VideoAreaFn videoArea = nullptr;
		bool videoAreaLogged = false;
		bool vsync = false;
		bool depthClearPending = true;
		// config.yml gpu_blits: blits onto and out of the frame done on the card, and the shown frame kept there at a flip
		// (gpuBlits); whole-frame clears done on the card (gpuClears). "clears" keeps only the clears, "off" neither.
		bool gpuBlits = true;
		bool gpuClears = true;

		// ---- Frame timing, for the log: where a frame's time goes. Reading the frame back from the graphics card
		// waits for all the 3D work so far, so it is the cost to watch; every 10 s one line sums it up.
		// Copy: a surface whose picture was only on the card (CopyFrameOnCard) read back for the game.
		enum class ReadCause { Lock, Blit, Clear, Other, Screen, Copy, Count };
		ReadCause readCause = ReadCause::Other;
		struct FrameStats {
			std::int64_t start = 0;
			std::int64_t lastFrame = 0;
			std::uint32_t frames = 0;
			double slowestMs = 0;
			std::uint32_t slowFrames = 0;
			std::uint32_t reads = 0;
			std::uint32_t areaReads = 0;
			double readMs = 0;
			std::uint32_t readsBy[static_cast<int>(ReadCause::Count)]{};
			std::uint32_t uploads = 0;
			double uploadMs = 0;
			std::uint32_t gpuClears = 0;
			std::uint32_t gpuBlits = 0;
			std::uint32_t blitUploads = 0;
			std::uint32_t gpuCopies = 0;
			double presentMs = 0;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
		};
		FrameStats frameStats;
		std::int64_t Ticks() {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
		double TicksToMs(std::int64_t ticks) {
			static const std::int64_t rate = [] {
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				return frequency.QuadPart;
			}();
			return static_cast<double>(ticks) * 1000.0 / static_cast<double>(rate);
		}
		void ReadFrameTimed(void* pixels, std::uint32_t pitch) {
			const std::int64_t started = Ticks();
			backend->ReadFrame(pixels, pitch);
			frameStats.readMs += TicksToMs(Ticks() - started);
			++frameStats.reads;
			++frameStats.readsBy[static_cast<int>(readCause)];
		}
		void UploadFrameTimed(const void* pixels, std::uint32_t pitch) {
			const std::int64_t started = Ticks();
			backend->UploadFrame(pixels, pitch);
			frameStats.uploadMs += TicksToMs(Ticks() - started);
			++frameStats.uploads;
		}
		// After each frame is shown (not the menus' screens).
		void CountShownFrame(std::uint32_t width, std::uint32_t height, double presentMs) {
			const std::int64_t now = Ticks();
			FrameStats& stats = frameStats;
			stats.presentMs += presentMs;
			if (stats.start == 0 || width != stats.width || height != stats.height) {
				stats = {};
				stats.start = stats.lastFrame = now;
				stats.width = width;
				stats.height = height;
				return;
			}
			const double frameMs = TicksToMs(now - stats.lastFrame);
			stats.lastFrame = now;
			++stats.frames;
			stats.slowestMs = (std::max)(stats.slowestMs, frameMs);
			// Longer than one and a half frames at 60 fps: a visible stutter.
			if (frameMs > 25.0) ++stats.slowFrames;
			const double elapsedMs = TicksToMs(now - stats.start);
			if (elapsedMs < 10000.0 || stats.frames == 0) return;
			const double frames = stats.frames;
			const auto& by = stats.readsBy;
			char text[640];
			std::snprintf(text, sizeof(text),
				"Renderer: %ux%u, last %.0f s: %u frames (%.1f fps), slowest %.1f ms, %u over 25 ms. Per frame: %.2f read backs (%.2f ms; lock %u, blit %u, "
				"clear %u, other %u, screen %u, copy %u in all; %u of them only an area), %.2f uploads (%.2f ms), %.2f clears, %.2f blits and %.2f copies "
				"out of the frame on the graphics card (%.2f blit pictures uploaded), present %.2f ms.",
				stats.width, stats.height, elapsedMs / 1000.0, stats.frames, frames * 1000.0 / elapsedMs, stats.slowestMs, stats.slowFrames, stats.reads / frames,
				stats.readMs / frames, by[0], by[1], by[2], by[3], by[4], by[5], stats.areaReads, stats.uploads / frames, stats.uploadMs / frames,
				stats.gpuClears / frames, stats.gpuBlits / frames, stats.gpuCopies / frames, stats.blitUploads / frames, stats.presentMs / frames);
			Logger::Debug(text);
			stats = {};
			stats.start = stats.lastFrame = now;
			stats.width = width;
			stats.height = height;
		}

		void LogUnused(const char* name) {
			static std::mutex loggedMutex;
			static std::set<std::string> logged;
			std::lock_guard lock(loggedMutex);
			if (logged.insert(name).second) Logger::Debug(std::string("Renderer: the game called ") + name + ", which the renderer does not support.");
		}
#define KF_UNUSED(name) { LogUnused(name); return DDERR_UNSUPPORTED; }

		// ---- Bump mapping (bump_mapping in config.yml): 0 off, 1 du/dv, 2 du/dv with luminance. Set before the game's
		// display starts. When on, two 16-bit bump texture formats are offered as well, and their surfaces are kept as the
		// game writes them and turned into 32-bit texels when sent to the card (du, dv + 128 in red and green,
		// luminance in blue).
		int bumpMode = 0;
		enum class Bump : std::uint8_t { None, DuDv, DuDvLuminance };
		// L6V5U5: du in bits 0-4, dv in 5-9, luminance in 10-15. V8U8: du in the low byte, dv in the high one.
		void FillBumpFormat(DDPIXELFORMAT& format, Bump bump) {
			std::memset(&format, 0, sizeof(format));
			format.dwSize = sizeof(DDPIXELFORMAT);
			format.dwFlags = DDPF_BUMPDUDV | (bump == Bump::DuDvLuminance ? DDPF_BUMPLUMINANCE : 0);
			format.dwBumpBitCount = 16;
			format.dwBumpDuBitMask = bump == Bump::DuDvLuminance ? 0x001F : 0x00FF;
			format.dwBumpDvBitMask = bump == Bump::DuDvLuminance ? 0x03E0 : 0xFF00;
			format.dwBumpLuminanceBitMask = bump == Bump::DuDvLuminance ? 0xFC00 : 0;
		}
		// One 16-bit bump texel as the card reads it (0xAARRGGBB).
		std::uint32_t BumpTexel(std::uint16_t value, Bump bump) {
			int du = 0;
			int dv = 0;
			int luminance = 255;
			if (bump == Bump::DuDvLuminance) {
				// 5-bit signed du and dv (-16..15) to the 8-bit range, 6-bit luminance to 0-255.
				du = static_cast<int>(value & 0x1F);
				dv = static_cast<int>((value >> 5) & 0x1F);
				if (du >= 16) du -= 32;
				if (dv >= 16) dv -= 32;
				du *= 8;
				dv *= 8;
				luminance = static_cast<int>((value >> 10) & 0x3F) * 255 / 63;
			}
			else {
				du = static_cast<std::int8_t>(value & 0xFF);
				dv = static_cast<std::int8_t>(value >> 8);
			}
			return 0xFF000000u | static_cast<std::uint32_t>(du + 128) << 16 | static_cast<std::uint32_t>(dv + 128) << 8 | static_cast<std::uint32_t>(luminance);
		}

		// ---- Pixel formats: everything is 32-bit, as patch.dll offers it.
		void FillPixelFormat(DDPIXELFORMAT& format, bool alpha) {
			std::memset(&format, 0, sizeof(format));
			format.dwSize = sizeof(DDPIXELFORMAT);
			format.dwFlags = DDPF_RGB | (alpha ? DDPF_ALPHAPIXELS : 0);
			format.dwRGBBitCount = 32;
			format.dwRBitMask = 0x00FF0000;
			format.dwGBitMask = 0x0000FF00;
			format.dwBBitMask = 0x000000FF;
			format.dwRGBAlphaBitMask = alpha ? 0xFF000000 : 0;
		}
		// Copies a structure the game passed with its own dwSize: only as much as both sides have, dwSize kept.
		template<typename T> void CopyOut(void* destination, const T& source) {
			auto* out = static_cast<std::uint8_t*>(destination);
			DWORD size = 0;
			std::memcpy(&size, out, sizeof(DWORD));
			const DWORD count = std::min<DWORD>(size, sizeof(T));
			std::memcpy(out, &source, count);
			if (count >= sizeof(DWORD)) std::memcpy(out, &size, sizeof(DWORD));
		}

		class SurfaceObject;
		class DeviceObject;
		SurfaceObject* primary = nullptr;
		SurfaceObject* renderTarget = nullptr;
		std::unordered_set<SurfaceObject*> textures;
		// What the window shows now, to show it again after switching APIs.
		PresentSource lastPresent = PresentSource::Frame;

		bool EnsureBackend();
		void MoveToWindow();
		void PresentFrame();
		void PresentSurface(SurfaceObject* surface);
		// A fill of the whole frame (surface is the 3D render target) done by the graphics card. False when it cannot
		// be (another surface, no backend), and the fill is then done in memory.
		bool ClearOnCard(SurfaceObject* surface, std::uint32_t color);
		// A blit onto the frame (target is the 3D render target) drawn by the graphics card, so the frame is not read
		// back. False when it cannot be, and the blit is then done in memory as before.
		bool BlitOnCard(SurfaceObject* target, SurfaceObject* source, const RECT& from, const RECT& to, bool keyed, std::uint32_t keyLow, std::uint32_t keyHigh);
		// A blit out of the frame (source is the 3D render target, only up to date on the card) into a whole surface,
		// done on the card: the surface's picture then lives in its blitTexture (cardOnly) until the game uses it some
		// other way. False when it cannot be (keyed, stretched, not the whole surface...): the frame is read back then.
		bool CopyFrameOnCard(SurfaceObject* target, SurfaceObject* source, const RECT& from, const RECT& to, bool keyed);
		// Surfaces with a blitTexture, to forget them when the backend changes.
		std::unordered_set<SurfaceObject*> blitSources;

		// ---- Surfaces. One object answers as IDirectDrawSurface (1 and 2), IDirectDrawSurface4 and
		// IDirect3DTexture2; DirectDraw 1 hands out version 2's table, which starts with version 1's.
		enum class Kind { Primary, BackBuffer, Plain, Texture, ZBuffer };
		constexpr std::uint32_t SurfaceMagic = 0x4B465331;

		class SurfaceObject final : public IDirectDrawSurface2, public IDirectDrawSurface4, public IDirect3DTexture2 {
			public:
				std::uint32_t magic = SurfaceMagic;
				Kind kind;
				std::uint32_t width;
				std::uint32_t height;
				DWORD caps;
				bool alpha;
				// A bump texture: its texels are 16-bit (the first half of pixels, rows of width * 2 bytes).
				Bump bump = Bump::None;
				std::vector<std::uint32_t> pixels;
				ULONG references = 1;
				SurfaceObject* attachedBack = nullptr;
				SurfaceObject* attachedDepth = nullptr;
				bool colorKey = false;
				std::uint32_t colorKeyLow = 0;
				std::uint32_t colorKeyHigh = 0;
				// Textures: the backend's copy, and whether the memory copy changed since it was made.
				std::uint32_t gpuTexture = 0;
				bool gpuDirty = true;
				// The render target: which copy is up to date.
				bool cpuCurrent = true;
				bool gpuCurrent = false;
				// The primary: it shows this surface, whose memory copy is only taken when the primary is used.
				SurfaceObject* mirror = nullptr;
				// The primary: the picture last flipped to is kept on the card (Backend::SaveShownFrame) and is only read
				// into memory if the primary is used.
				bool shownOnCard = false;
				// Any surface blitted onto the frame on the card (BlitOnCard): the backend's copy of its memory picture,
				// with the color key it was made for (keyed pixels transparent), and whether the memory copy changed.
				std::uint32_t blitTexture = 0;
				bool blitDirty = true;
				bool blitKeyed = false;
				std::uint32_t blitKeyLow = 0;
				std::uint32_t blitKeyHigh = 0;
				// The picture is only in blitTexture (CopyFrameOnCard: copied there from the frame, unkeyed), not in memory.
				bool cardOnly = false;
				RECT lockedRect{};
				bool lockedWithRect = false;

				SurfaceObject(Kind kind, std::uint32_t width, std::uint32_t height, DWORD caps, bool alpha)
					: kind(kind), width(std::max<std::uint32_t>(width, 1)), height(std::max<std::uint32_t>(height, 1)), caps(caps), alpha(alpha) {
					if (kind != Kind::ZBuffer) pixels.assign(static_cast<std::size_t>(this->width) * this->height, 0);
					if (kind == Kind::Texture) textures.insert(this);
				}
				~SurfaceObject() {
					magic = 0;
					if (gpuTexture != 0 && backend) backend->DestroyTexture(gpuTexture);
					if (blitTexture != 0 && backend) backend->DestroyTexture(blitTexture);
					textures.erase(this);
					blitSources.erase(this);
					if (primary == this) primary = nullptr;
					if (renderTarget == this) renderTarget = nullptr;
					if (primary && primary->mirror == this) primary->mirror = nullptr;
					if (attachedBack) attachedBack->ReleaseObject();
					if (attachedDepth) attachedDepth->ReleaseObject();
				}
				static SurfaceObject* From(IDirectDrawSurface2* surface) { return Check(surface ? static_cast<SurfaceObject*>(surface) : nullptr); }
				static SurfaceObject* From(IDirectDrawSurface4* surface) { return Check(surface ? static_cast<SurfaceObject*>(surface) : nullptr); }
				static SurfaceObject* From(IDirect3DTexture2* texture) { return Check(texture ? static_cast<SurfaceObject*>(texture) : nullptr); }
				static SurfaceObject* From(IDirectDrawSurface* surface) { return From(reinterpret_cast<IDirectDrawSurface2*>(surface)); }
				static SurfaceObject* Check(SurfaceObject* surface) { return surface && surface->magic == SurfaceMagic ? surface : nullptr; }

				std::uint32_t BytesPerPixel() const { return bump != Bump::None ? 2u : 4u; }
				std::uint32_t Pitch() const { return width * BytesPerPixel(); }
				std::uint8_t* Bytes() { return reinterpret_cast<std::uint8_t*>(pixels.data()); }
				ULONG AddObject() { Guard lock(layerMutex); return ++references; }
				ULONG ReleaseObject() {
					Guard lock(layerMutex);
					if (references == 0) return 0;
					const ULONG left = --references;
					if (left == 0) delete this;
					return left;
				}
				DWORD Caps() const {
					DWORD result = caps;
					if (kind == Kind::Primary) result |= DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
					if (kind == Kind::BackBuffer) result |= DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
					if ((result & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY)) == 0) result |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
					return result;
				}
				void FillDescription(DDSURFACEDESC2& description) {
					std::memset(&description, 0, sizeof(description));
					description.dwSize = sizeof(DDSURFACEDESC2);
					description.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
					description.dwWidth = width;
					description.dwHeight = height;
					description.ddsCaps.dwCaps = Caps();
					if (kind == Kind::ZBuffer) {
						description.dwFlags |= DDSD_ZBUFFERBITDEPTH;
						description.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
						description.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
						description.ddpfPixelFormat.dwZBufferBitDepth = 32;
						description.ddpfPixelFormat.dwZBitMask = 0xFFFFFF;
						return;
					}
					description.dwFlags |= DDSD_PITCH;
					description.lPitch = static_cast<LONG>(Pitch());
					if (bump != Bump::None) FillBumpFormat(description.ddpfPixelFormat, bump);
					else FillPixelFormat(description.ddpfPixelFormat, alpha);
					if (kind == Kind::Primary && attachedBack) {
						description.dwFlags |= DDSD_BACKBUFFERCOUNT;
						description.dwBackBufferCount = 1;
					}
				}

				// Makes the memory copy right before it is read or partly written.
				void PrepareRead() {
					if (cardOnly) {
						cardOnly = false;
						if (backend && blitTexture != 0) {
							const std::int64_t started = Ticks();
							backend->ReadTexture(blitTexture, pixels.data(), Pitch());
							frameStats.readMs += TicksToMs(Ticks() - started);
							++frameStats.reads;
							++frameStats.readsBy[static_cast<int>(ReadCause::Copy)];
						}
						// Uploaded again as the game's keys and alpha want it, when next blitted.
						blitDirty = true;
					}
					if (this == renderTarget && !cpuCurrent && backend) {
						ReadFrameTimed(pixels.data(), Pitch());
						cpuCurrent = true;
						// Its memory picture changed under any blit copy made of it.
						blitDirty = true;
					}
					if (kind == Kind::Primary && shownOnCard) {
						shownOnCard = false;
						if (backend && renderTarget && renderTarget->width == width && renderTarget->height == height) {
							const std::int64_t started = Ticks();
							backend->ReadShownFrame(pixels.data(), Pitch());
							frameStats.readMs += TicksToMs(Ticks() - started);
							++frameStats.reads;
							++frameStats.readsBy[static_cast<int>(ReadCause::Screen)];
						}
					}
					if (kind == Kind::Primary && mirror) {
						SurfaceObject* shown = mirror;
						mirror = nullptr;
						shown->PrepareRead();
						if (shown->width == width && shown->height == height) std::memcpy(pixels.data(), shown->pixels.data(), pixels.size() * 4);
					}
				}
				// After the memory copy changed.
				// Before a blit reads area of this surface: for the frame, only that area is read back when it is a small part
				// of it (the memory copy then stays marked out of date, as the rest of it is), otherwise as PrepareRead.
				void PrepareReadArea(const RECT& area) {
					if (this != renderTarget || cpuCurrent || !backend) return PrepareRead();
					RECT part = area;
					if (!ClipRect(part, width, height)) return;
					const std::uint64_t partPixels = static_cast<std::uint64_t>(part.right - part.left) * static_cast<std::uint64_t>(part.bottom - part.top);
					// Half the frame or more: the whole of it costs about the same and leaves memory up to date.
					if (partPixels * 2 >= static_cast<std::uint64_t>(width) * height) return PrepareRead();
					const std::int64_t started = Ticks();
					backend->ReadFrameArea(pixels.data(), Pitch(), static_cast<std::uint32_t>(part.left), static_cast<std::uint32_t>(part.top),
						static_cast<std::uint32_t>(part.right - part.left), static_cast<std::uint32_t>(part.bottom - part.top));
					frameStats.readMs += TicksToMs(Ticks() - started);
					++frameStats.reads;
					++frameStats.areaReads;
					++frameStats.readsBy[static_cast<int>(readCause)];
					// For the log: what the game copies out of the frame, the first few times.
					static int logged = 0;
					if (logged < 5) {
						++logged;
						char text[160];
						std::snprintf(text, sizeof(text), "Renderer: a blit read %ldx%ld at %ld,%ld out of the %ux%u frame; only that was read back.",
							part.right - part.left, part.bottom - part.top, part.left, part.top, width, height);
						Logger::Debug(text);
					}
				}
				void Changed() {
					blitDirty = true;
					// Memory has the picture now (written over whole, or read first).
					cardOnly = false;
					if (this == renderTarget) {
						cpuCurrent = true;
						gpuCurrent = false;
					}
					if (kind == Kind::Texture) gpuDirty = true;
					if (kind == Kind::Primary) PresentSurface(this);
				}

				// ---- Blits, in memory.
				static bool ClipRect(RECT& rect, std::uint32_t width, std::uint32_t height) {
					rect.left = std::max<LONG>(rect.left, 0);
					rect.top = std::max<LONG>(rect.top, 0);
					rect.right = std::min<LONG>(rect.right, static_cast<LONG>(width));
					rect.bottom = std::min<LONG>(rect.bottom, static_cast<LONG>(height));
					return rect.right > rect.left && rect.bottom > rect.top;
				}
				bool Keyed(std::uint32_t pixel) const {
					const std::uint32_t value = pixel & 0x00FFFFFF;
					return value >= (colorKeyLow & 0x00FFFFFF) && value <= (colorKeyHigh & 0x00FFFFFF);
				}
				void Fill(const RECT* area, std::uint32_t color) {
					RECT rect = area ? *area : RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
					if (!ClipRect(rect, width, height)) return;
					if (bump != Bump::None) {
						auto* texels = reinterpret_cast<std::uint16_t*>(Bytes());
						for (LONG y = rect.top; y < rect.bottom; ++y) std::fill_n(texels + static_cast<std::size_t>(y) * width + rect.left, rect.right - rect.left, static_cast<std::uint16_t>(color));
						return;
					}
					for (LONG y = rect.top; y < rect.bottom; ++y) {
						std::fill_n(pixels.data() + static_cast<std::size_t>(y) * width + rect.left, rect.right - rect.left, color);
					}
				}
				// Copies source's sourceRect to destinationRect here (scaling with the nearest pixel when the sizes
				// differ), skipping source pixels in keyRange when keyed.
				void Copy(SurfaceObject* source, RECT sourceRect, RECT destinationRect, bool keyed, std::uint32_t keyLow, std::uint32_t keyHigh) {
					const LONG sourceWidth = sourceRect.right - sourceRect.left;
					const LONG sourceHeight = sourceRect.bottom - sourceRect.top;
					const LONG destinationWidth = destinationRect.right - destinationRect.left;
					const LONG destinationHeight = destinationRect.bottom - destinationRect.top;
					if (sourceWidth <= 0 || sourceHeight <= 0 || destinationWidth <= 0 || destinationHeight <= 0) return;
					if (bump != Bump::None || source->bump != Bump::None) {
						CopyBump(source, sourceRect, destinationRect);
						return;
					}
					// Same pixels on both sides (a surface copied onto itself) need a copy to read from.
					std::vector<std::uint32_t> selfCopy;
					const std::uint32_t* from = source->pixels.data();
					if (source == this) {
						selfCopy = pixels;
						from = selfCopy.data();
					}
					const bool scaled = sourceWidth != destinationWidth || sourceHeight != destinationHeight;
					keyLow &= 0x00FFFFFF;
					keyHigh &= 0x00FFFFFF;
					for (LONG y = 0; y < destinationHeight; ++y) {
						const LONG targetY = destinationRect.top + y;
						if (targetY < 0 || targetY >= static_cast<LONG>(height)) continue;
						const LONG fromY = sourceRect.top + (scaled ? y * sourceHeight / destinationHeight : y);
						if (fromY < 0 || fromY >= static_cast<LONG>(source->height)) continue;
						const std::uint32_t* sourceRow = from + static_cast<std::size_t>(fromY) * source->width;
						std::uint32_t* targetRow = pixels.data() + static_cast<std::size_t>(targetY) * width;
						if (!scaled && !keyed) {
							LONG first = 0;
							LONG last = destinationWidth;
							first = std::max<LONG>(first, -destinationRect.left);
							first = std::max<LONG>(first, -sourceRect.left);
							last = std::min<LONG>(last, static_cast<LONG>(width) - destinationRect.left);
							last = std::min<LONG>(last, static_cast<LONG>(source->width) - sourceRect.left);
							if (last > first) std::memcpy(targetRow + destinationRect.left + first, sourceRow + sourceRect.left + first, static_cast<std::size_t>(last - first) * 4);
							continue;
						}
						for (LONG x = 0; x < destinationWidth; ++x) {
							const LONG targetX = destinationRect.left + x;
							if (targetX < 0 || targetX >= static_cast<LONG>(width)) continue;
							const LONG fromX = sourceRect.left + (scaled ? x * sourceWidth / destinationWidth : x);
							if (fromX < 0 || fromX >= static_cast<LONG>(source->width)) continue;
							const std::uint32_t pixel = sourceRow[fromX];
							if (keyed && (pixel & 0x00FFFFFF) >= keyLow && (pixel & 0x00FFFFFF) <= keyHigh) continue;
							targetRow[targetX] = pixel;
						}
					}
				}

				// Bump texels between two bump surfaces of the same kind, 16 bits each, nearest pixel when the sizes differ;
				// no color keys. Anything else involving a bump surface is left as it is.
				void CopyBump(SurfaceObject* source, RECT sourceRect, RECT destinationRect) {
					if (bump != source->bump) return;
					const LONG sourceWidth = sourceRect.right - sourceRect.left;
					const LONG sourceHeight = sourceRect.bottom - sourceRect.top;
					const LONG destinationWidth = destinationRect.right - destinationRect.left;
					const LONG destinationHeight = destinationRect.bottom - destinationRect.top;
					std::vector<std::uint32_t> selfCopy;
					const auto* from = reinterpret_cast<const std::uint16_t*>(source->pixels.data());
					if (source == this) {
						selfCopy = pixels;
						from = reinterpret_cast<const std::uint16_t*>(selfCopy.data());
					}
					auto* to = reinterpret_cast<std::uint16_t*>(Bytes());
					for (LONG y = 0; y < destinationHeight; ++y) {
						const LONG targetY = destinationRect.top + y;
						const LONG fromY = sourceRect.top + y * sourceHeight / destinationHeight;
						if (targetY < 0 || targetY >= static_cast<LONG>(height) || fromY < 0 || fromY >= static_cast<LONG>(source->height)) continue;
						for (LONG x = 0; x < destinationWidth; ++x) {
							const LONG targetX = destinationRect.left + x;
							const LONG fromX = sourceRect.left + x * sourceWidth / destinationWidth;
							if (targetX < 0 || targetX >= static_cast<LONG>(width) || fromX < 0 || fromX >= static_cast<LONG>(source->width)) continue;
							to[static_cast<std::size_t>(targetY) * width + targetX] = from[static_cast<std::size_t>(fromY) * source->width + fromX];
						}
					}
				}

				HRESULT DoBlt(LPRECT destination, SurfaceObject* source, LPRECT sourceArea, DWORD flags, LPDDBLTFX effects) {
					Guard lock(layerMutex);
					if (kind == Kind::ZBuffer) {
						if ((flags & DDBLT_DEPTHFILL) != 0 && EnsureBackend()) {
							backend->ClearDepth();
							depthClearPending = false;
						}
						return DD_OK;
					}
					RECT target = destination ? *destination : RECT{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
					if ((flags & DDBLT_COLORFILL) != 0) {
						const bool whole = target.left <= 0 && target.top <= 0 && target.right >= static_cast<LONG>(width) && target.bottom >= static_cast<LONG>(height);
						// A fill of the whole surface needs no copy of what was there.
						if (whole && ClearOnCard(this, effects ? effects->dwFillColor : 0)) return DD_OK;
						if (whole) {
							if (kind == Kind::Primary) {
								mirror = nullptr;
								shownOnCard = false;
							}
							if (this == renderTarget) cpuCurrent = true;
						}
						else {
							readCause = ReadCause::Clear;
							PrepareRead();
							readCause = ReadCause::Other;
						}
						Fill(&target, effects ? effects->dwFillColor : 0);
						Changed();
						return DD_OK;
					}
					if (!source) return DDERR_INVALIDPARAMS;
					RECT from = sourceArea ? *sourceArea : RECT{ 0, 0, static_cast<LONG>(source->width), static_cast<LONG>(source->height) };
					// The frame copied whole onto the primary is just showing it.
					const bool wholeSource = from.left == 0 && from.top == 0 && from.right == static_cast<LONG>(source->width) && from.bottom == static_cast<LONG>(source->height);
					const bool wholeTarget = target.left == 0 && target.top == 0 && target.right == static_cast<LONG>(width) && target.bottom == static_cast<LONG>(height);
					if (kind == Kind::Primary && source == renderTarget && wholeSource && wholeTarget && source->width == width && source->height == height
						&& (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) == 0) {
						mirror = source;
						shownOnCard = false;
						PresentFrame();
						return DD_OK;
					}
					bool keyed = false;
					std::uint32_t low = 0;
					std::uint32_t high = 0;
					if ((flags & DDBLT_KEYSRCOVERRIDE) != 0 && effects) {
						keyed = true;
						low = effects->ddckSrcColorkey.dwColorSpaceLowValue;
						high = effects->ddckSrcColorkey.dwColorSpaceHighValue;
					}
					else if ((flags & DDBLT_KEYSRC) != 0 && source->colorKey) {
						keyed = true;
						low = source->colorKeyLow;
						high = source->colorKeyHigh;
					}
					if (BlitOnCard(this, source, from, target, keyed, low, high)) return DD_OK;
					if (CopyFrameOnCard(this, source, from, target, keyed)) return DD_OK;
					readCause = ReadCause::Blit;
					source->PrepareReadArea(from);
					PrepareRead();
					readCause = ReadCause::Other;
					Copy(source, from, target, keyed, low, high);
					Changed();
					return DD_OK;
				}
				HRESULT DoBltFast(DWORD x, DWORD y, SurfaceObject* source, LPRECT sourceArea, DWORD type) {
					Guard lock(layerMutex);
					if (!source) return DDERR_INVALIDPARAMS;
					RECT from = sourceArea ? *sourceArea : RECT{ 0, 0, static_cast<LONG>(source->width), static_cast<LONG>(source->height) };
					RECT target{ static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + from.right - from.left, static_cast<LONG>(y) + from.bottom - from.top };
					const bool keyed = (type & DDBLTFAST_SRCCOLORKEY) != 0 && source->colorKey;
					if (BlitOnCard(this, source, from, target, keyed, source->colorKeyLow, source->colorKeyHigh)) return DD_OK;
					if (CopyFrameOnCard(this, source, from, target, keyed)) return DD_OK;
					readCause = ReadCause::Blit;
					source->PrepareReadArea(from);
					PrepareRead();
					readCause = ReadCause::Other;
					Copy(source, from, target, keyed, source->colorKeyLow, source->colorKeyHigh);
					Changed();
					return DD_OK;
				}
				HRESULT DoLock(LPRECT area, void* description, bool version4) {
					Guard lock(layerMutex);
					if (kind == Kind::ZBuffer) return DDERR_UNSUPPORTED;
					readCause = ReadCause::Lock;
					PrepareRead();
					readCause = ReadCause::Other;
					DDSURFACEDESC2 filled;
					FillDescription(filled);
					std::size_t offset = 0;
					lockedWithRect = area != nullptr;
					if (area) {
						lockedRect = *area;
						offset = static_cast<std::size_t>(std::max<LONG>(area->top, 0)) * Pitch() + static_cast<std::size_t>(std::max<LONG>(area->left, 0)) * BytesPerPixel();
					}
					filled.lpSurface = Bytes() + offset;
					filled.dwFlags |= DDSD_LPSURFACE;
					if (version4) CopyOut(description, filled);
					else {
						// DDSURFACEDESC is DDSURFACEDESC2 without the last fields; ddsCaps is 4 bytes there instead of 16.
						DDSURFACEDESC older{};
						std::memcpy(&older, &filled, offsetof(DDSURFACEDESC, ddsCaps));
						older.dwSize = sizeof(DDSURFACEDESC);
						older.ddsCaps.dwCaps = filled.ddsCaps.dwCaps;
						CopyOut(description, older);
					}
					return DD_OK;
				}
				HRESULT DoUnlock() {
					Guard lock(layerMutex);
					lockedWithRect = false;
					Changed();
					return DD_OK;
				}
				HRESULT DoGetAttached(DWORD wanted, SurfaceObject** found) {
					Guard lock(layerMutex);
					SurfaceObject* result = nullptr;
					if ((wanted & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) != 0) result = attachedBack;
					if ((wanted & DDSCAPS_ZBUFFER) != 0) result = attachedDepth;
					if (!result) return DDERR_NOTFOUND;
					result->AddObject();
					*found = result;
					return DD_OK;
				}
				HRESULT DoFlip() {
					Guard lock(layerMutex);
					if (kind != Kind::Primary || !attachedBack) return DDERR_NOTFLIPPABLE;
					SurfaceObject* back = attachedBack;
					mirror = back;
					shownOnCard = false;
					if (back == renderTarget) PresentFrame();
					else PresentSurface(back);
					// Like a real flip, the screen keeps the picture just shown. Copied lazily (mirror), it was taken from the
					// back buffer only when the screen was next used (the pointer drawn onto it), by which time the game was
					// often drawing the next frame there: a half-drawn picture now and then (flicker in levels). When both
					// copies of the 3D frame match (the game drew its panels on it, and it was just uploaded and shown),
					// the two buffers swap their memory; the back buffer then holds the old screen, as after a real flip,
					// and its graphics card copy (the frame just shown) counts as the newer one.
					if (back == renderTarget && back->cpuCurrent && back->gpuCurrent && back->pixels.size() == pixels.size()) {
						std::swap(pixels, back->pixels);
						back->cpuCurrent = false;
						back->blitDirty = true;
						mirror = nullptr;
					}
					// The frame only on the card (its panels were blitted there): the card keeps a copy of what was just
					// shown, read into memory only if the game uses the screen before the next flip.
					else if (gpuBlits && back == renderTarget && !back->cpuCurrent && back->gpuCurrent && back->width == width && back->height == height && backend) {
						backend->SaveShownFrame();
						shownOnCard = true;
						mirror = nullptr;
					}
					depthClearPending = true;
					return DD_OK;
				}
				HRESULT DoQuery(REFIID id, void** object);

				// IUnknown (all interfaces)
				STDMETHOD(QueryInterface)(REFIID id, void** object) override { return DoQuery(id, object); }
				STDMETHOD_(ULONG, AddRef)() override { return AddObject(); }
				STDMETHOD_(ULONG, Release)() override { return ReleaseObject(); }

				// Methods both surface versions share.
				STDMETHOD(AddOverlayDirtyRect)(LPRECT) override KF_UNUSED("Surface::AddOverlayDirtyRect")
				STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override KF_UNUSED("Surface::BltBatch")
				STDMETHOD(GetBltStatus)(DWORD) override { return DD_OK; }
				STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER*) override KF_UNUSED("Surface::GetClipper")
				STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override {
					if (!key || (flags & DDCKEY_SRCBLT) == 0 || !colorKey) return DDERR_NOCOLORKEY;
					key->dwColorSpaceLowValue = colorKeyLow;
					key->dwColorSpaceHighValue = colorKeyHigh;
					return DD_OK;
				}
				STDMETHOD(GetDC)(HDC*) override KF_UNUSED("Surface::GetDC")
				STDMETHOD(GetFlipStatus)(DWORD) override { return DD_OK; }
				STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override KF_UNUSED("Surface::GetOverlayPosition")
				STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE*) override { return DDERR_NOPALETTEATTACHED; }
				STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT format) override {
					if (!format) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC2 description;
					FillDescription(description);
					*format = description.ddpfPixelFormat;
					return DD_OK;
				}
				STDMETHOD(IsLost)() override { return DD_OK; }
				STDMETHOD(ReleaseDC)(HDC) override KF_UNUSED("Surface::ReleaseDC")
				STDMETHOD(Restore)() override { return DD_OK; }
				STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER) override { return DD_OK; }
				STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override {
					Guard lock(layerMutex);
					if ((flags & DDCKEY_SRCBLT) == 0) return DD_OK;
					colorKey = key != nullptr;
					if (key) {
						colorKeyLow = key->dwColorSpaceLowValue;
						colorKeyHigh = (flags & DDCKEY_COLORSPACE) != 0 ? key->dwColorSpaceHighValue : key->dwColorSpaceLowValue;
					}
					return DD_OK;
				}
				STDMETHOD(SetOverlayPosition)(LONG, LONG) override KF_UNUSED("Surface::SetOverlayPosition")
				STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE) override KF_UNUSED("Surface::SetPalette")
				STDMETHOD(UpdateOverlayDisplay)(DWORD) override KF_UNUSED("Surface::UpdateOverlayDisplay")
				STDMETHOD(GetDDInterface)(LPVOID*) override KF_UNUSED("Surface::GetDDInterface")
				STDMETHOD(PageLock)(DWORD) override { return DD_OK; }
				STDMETHOD(PageUnlock)(DWORD) override { return DD_OK; }

				// IDirectDrawSurface2
				STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE2 surface) override { return Attach(From(surface)); }
				STDMETHOD(Blt)(LPRECT destination, LPDIRECTDRAWSURFACE2 source, LPRECT sourceArea, DWORD flags, LPDDBLTFX effects) override { return DoBlt(destination, From(source), sourceArea, flags, effects); }
				STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE2 source, LPRECT sourceArea, DWORD type) override { return DoBltFast(x, y, From(source), sourceArea, type); }
				STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE2) override { return DD_OK; }
				STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK) override KF_UNUSED("Surface::EnumAttachedSurfaces")
				STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override KF_UNUSED("Surface::EnumOverlayZOrders")
				STDMETHOD(Flip)(LPDIRECTDRAWSURFACE2, DWORD) override { return DoFlip(); }
				STDMETHOD(GetAttachedSurface)(LPDDSCAPS wanted, LPDIRECTDRAWSURFACE2* found) override {
					if (!wanted || !found) return DDERR_INVALIDPARAMS;
					SurfaceObject* surface = nullptr;
					const HRESULT result = DoGetAttached(wanted->dwCaps, &surface);
					if (SUCCEEDED(result)) *found = static_cast<IDirectDrawSurface2*>(surface);
					return result;
				}
				STDMETHOD(GetCaps)(LPDDSCAPS out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					out->dwCaps = Caps();
					return DD_OK;
				}
				STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC description) override {
					if (!description) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC2 filled;
					FillDescription(filled);
					DDSURFACEDESC older{};
					std::memcpy(&older, &filled, offsetof(DDSURFACEDESC, ddsCaps));
					older.dwSize = sizeof(DDSURFACEDESC);
					older.ddsCaps.dwCaps = filled.ddsCaps.dwCaps;
					CopyOut(description, older);
					return DD_OK;
				}
				STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
				STDMETHOD(Lock)(LPRECT area, LPDDSURFACEDESC description, DWORD, HANDLE) override { return description ? DoLock(area, description, false) : DDERR_INVALIDPARAMS; }
				STDMETHOD(Unlock)(LPVOID) override { return DoUnlock(); }
				STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE2, LPRECT, DWORD, LPDDOVERLAYFX) override KF_UNUSED("Surface::UpdateOverlay")
				STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE2) override KF_UNUSED("Surface::UpdateOverlayZOrder")

				// IDirectDrawSurface4
				STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE4 surface) override { return Attach(From(surface)); }
				STDMETHOD(Blt)(LPRECT destination, LPDIRECTDRAWSURFACE4 source, LPRECT sourceArea, DWORD flags, LPDDBLTFX effects) override { return DoBlt(destination, From(source), sourceArea, flags, effects); }
				STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE4 source, LPRECT sourceArea, DWORD type) override { return DoBltFast(x, y, From(source), sourceArea, type); }
				STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE4) override { return DD_OK; }
				STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK2) override KF_UNUSED("Surface4::EnumAttachedSurfaces")
				STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK2) override KF_UNUSED("Surface4::EnumOverlayZOrders")
				STDMETHOD(Flip)(LPDIRECTDRAWSURFACE4, DWORD) override { return DoFlip(); }
				STDMETHOD(GetAttachedSurface)(LPDDSCAPS2 wanted, LPDIRECTDRAWSURFACE4* found) override {
					if (!wanted || !found) return DDERR_INVALIDPARAMS;
					SurfaceObject* surface = nullptr;
					const HRESULT result = DoGetAttached(wanted->dwCaps, &surface);
					if (SUCCEEDED(result)) *found = static_cast<IDirectDrawSurface4*>(surface);
					return result;
				}
				STDMETHOD(GetCaps)(LPDDSCAPS2 out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					std::memset(out, 0, sizeof(DDSCAPS2));
					out->dwCaps = Caps();
					return DD_OK;
				}
				STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2 description) override {
					if (!description) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC2 filled;
					FillDescription(filled);
					CopyOut(description, filled);
					return DD_OK;
				}
				STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC2) override { return DDERR_ALREADYINITIALIZED; }
				STDMETHOD(Lock)(LPRECT area, LPDDSURFACEDESC2 description, DWORD, HANDLE) override { return description ? DoLock(area, description, true) : DDERR_INVALIDPARAMS; }
				STDMETHOD(Unlock)(LPRECT) override { return DoUnlock(); }
				STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDOVERLAYFX) override KF_UNUSED("Surface4::UpdateOverlay")
				STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE4) override KF_UNUSED("Surface4::UpdateOverlayZOrder")
				STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2, DWORD) override KF_UNUSED("Surface4::SetSurfaceDesc")
				STDMETHOD(SetPrivateData)(REFGUID, LPVOID, DWORD, DWORD) override KF_UNUSED("Surface4::SetPrivateData")
				STDMETHOD(GetPrivateData)(REFGUID, LPVOID, LPDWORD) override KF_UNUSED("Surface4::GetPrivateData")
				STDMETHOD(FreePrivateData)(REFGUID) override KF_UNUSED("Surface4::FreePrivateData")
				STDMETHOD(GetUniquenessValue)(LPDWORD value) override {
					if (value) *value = 1;
					return DD_OK;
				}
				STDMETHOD(ChangeUniquenessValue)() override { return DD_OK; }

				// IDirect3DTexture2
				STDMETHOD(GetHandle)(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE handle) override {
					if (!handle) return DDERR_INVALIDPARAMS;
					*handle = static_cast<D3DTEXTUREHANDLE>(reinterpret_cast<std::uintptr_t>(this));
					return D3D_OK;
				}
				STDMETHOD(PaletteChanged)(DWORD, DWORD) override { return D3D_OK; }
				STDMETHOD(Load)(LPDIRECT3DTEXTURE2 source) override {
					Guard lock(layerMutex);
					SurfaceObject* from = From(source);
					if (!from) return DDERR_INVALIDPARAMS;
					from->PrepareRead();
					Copy(from, { 0, 0, static_cast<LONG>(from->width), static_cast<LONG>(from->height) }, { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) }, false, 0, 0);
					Changed();
					return D3D_OK;
				}

			private:
				HRESULT Attach(SurfaceObject* surface) {
					Guard lock(layerMutex);
					if (!surface) return DDERR_INVALIDPARAMS;
					if (surface->kind == Kind::ZBuffer && surface != attachedDepth) {
						if (attachedDepth) attachedDepth->ReleaseObject();
						attachedDepth = surface;
						surface->AddObject();
					}
					return DD_OK;
				}
		};

		// ---- Showing pictures.
		PresentRect Target(std::uint32_t pictureWidth, std::uint32_t pictureHeight) {
			RECT client{};
			GetClientRect(window, &client);
			const LONG width = client.right - client.left;
			const LONG height = client.bottom - client.top;
			if (stretch || pictureWidth == 0 || pictureHeight == 0) return { 0, 0, width, height };
			// Keep the picture's shape, centered.
			const double scale = std::min<double>(static_cast<double>(width) / pictureWidth, static_cast<double>(height) / pictureHeight);
			const LONG shownWidth = static_cast<LONG>(pictureWidth * scale + 0.5);
			const LONG shownHeight = static_cast<LONG>(pictureHeight * scale + 0.5);
			const LONG left = (width - shownWidth) / 2;
			const LONG top = (height - shownHeight) / 2;
			return { left, top, left + shownWidth, top + shownHeight };
		}
		bool WindowShowable() {
			if (!window || !IsWindow(window) || IsIconic(window)) return false;
			RECT client{};
			return GetClientRect(window, &client) && client.right > client.left && client.bottom > client.top;
		}
		// For the log: what is being shown, the first time each way and then now and then, with whether the memory
		// copy has anything in it (so a black screen tells apart "the game drew nothing" from "it was not shown").
		std::uint32_t framePresents = 0;
		std::uint32_t screenPresents = 0;
		// For the hang watchdog: when a picture was last shown, and which thread shows the frames.
		std::atomic<ULONGLONG> lastShown = 0;
		std::atomic<DWORD> frameThread = 0;
		void LogPresent(PresentSource source, SurfaceObject* surface, const PresentRect& target) {
			lastShown = GetTickCount64();
			if (source == PresentSource::Frame) frameThread = GetCurrentThreadId();
			std::uint32_t& count = source == PresentSource::Frame ? framePresents : screenPresents;
			++count;
			// The 30th and 300th too, so a short video or menu shows whether its pictures have anything in them.
			if (count != 1 && count != 30 && count != 300 && count % 1000 != 0) return;
			std::string contents = "not in memory";
			if (surface->cpuCurrent || surface != renderTarget) {
				std::size_t lit = 0;
				for (std::size_t i = 0; i < surface->pixels.size(); i += 97) if ((surface->pixels[i] & 0x00FFFFFF) != 0) ++lit;
				contents = lit == 0 ? "all black" : std::to_string(lit) + " sampled pixels lit";
			}
			RECT client{};
			GetClientRect(window, &client);
			Logger::Debug(std::string("Renderer: shown ") + std::to_string(count) + (source == PresentSource::Frame ? " frames" : " screens") + " so far ("
				+ std::to_string(surface->width) + "x" + std::to_string(surface->height) + ", " + contents + ", into " + std::to_string(target.right - target.left)
				+ "x" + std::to_string(target.bottom - target.top) + " at " + std::to_string(target.left) + "," + std::to_string(target.top) + " of a "
				+ std::to_string(client.right - client.left) + "x" + std::to_string(client.bottom - client.top) + " window).");
		}
		std::uint32_t skippedPresents = 0;
		void LogSkipped(const char* why) {
			if (++skippedPresents == 1 || skippedPresents % 1000 == 0) Logger::Debug(std::string("Renderer: a picture was not shown (") + why + "), " + std::to_string(skippedPresents) + " so far.");
		}

		// For the log: what Windows tells the game's window about focus and minimizing. The game pauses on these
		// (a video switches between playing and paused on each one), so a lost or doubled one leaves it paused.
		std::mutex watchMutex;
		std::unordered_map<HWND, WNDPROC> watchedWindows;
		std::uint32_t loggedMessages = 0;
		// While another program has focus nothing is shown (as patch.dll does): a full-screen window in the background
		// may not take pictures, and the drivers can wait on it.
		std::atomic<bool> appActive = true;
		// Focus-loss messages kept from the game while it runs in the background (BackgroundRunning); used only on the
		// window's own thread.
		struct HeldMessage {
			UINT message;
			WPARAM wParam;
			LPARAM lParam;
		};
		std::vector<HeldMessage> heldFocusLoss;
		// window_mode: borderless. The game's window is a plain window with no frame over the whole monitor it is on, left
		// where it is when the game loses focus (another program simply comes in front), instead of minimized.
		std::atomic<bool> borderless = true;
		// Set while the window is being put in place, so the move it causes does not start another.
		bool placingWindow = false;
		void PlaceBorderless(HWND handle) {
			if (!borderless || placingWindow || IsIconic(handle)) return;
			MONITORINFO monitor{};
			monitor.cbSize = sizeof(monitor);
			if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
			const RECT& area = monitor.rcMonitor;
			const LONG_PTR style = GetWindowLongPtrA(handle, GWL_STYLE);
			const LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
			const LONG_PTR wantedStyle = (style & ~frame) | WS_POPUP;
			const LONG_PTR extended = GetWindowLongPtrA(handle, GWL_EXSTYLE);
			const LONG_PTR wantedExtended = extended & ~(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
			RECT now{};
			GetWindowRect(handle, &now);
			if (style == wantedStyle && extended == wantedExtended && EqualRect(&now, &area)) return;
			placingWindow = true;
			if (style != wantedStyle) SetWindowLongPtrA(handle, GWL_STYLE, wantedStyle);
			if (extended != wantedExtended) SetWindowLongPtrA(handle, GWL_EXSTYLE, wantedExtended);
			SetWindowPos(handle, HWND_NOTOPMOST, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
			placingWindow = false;
			static int logged = 0;
			if (logged++ < 5) {
				char text[160];
				std::snprintf(text, sizeof(text), "Renderer: the game's window is borderless, %ldx%ld at %ld,%ld (its style was 0x%X).", area.right - area.left, area.bottom - area.top,
					area.left, area.top, static_cast<unsigned>(style));
				Logger::Debug(text);
			}
		}
		LRESULT CALLBACK WatchedWindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
			WNDPROC original = nullptr;
			{
				std::lock_guard lock(watchMutex);
				const auto found = watchedWindows.find(handle);
				if (found != watchedWindows.end()) original = found->second;
				if (message == WM_NCDESTROY && original) {
					SetWindowLongPtrA(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
					watchedWindows.erase(found);
				}
			}
			if (!original) return DefWindowProcA(handle, message, wParam, lParam);
			const char* name = nullptr;
			switch (message) {
				case WM_ACTIVATEAPP: name = "WM_ACTIVATEAPP"; break;
				case WM_ACTIVATE: name = "WM_ACTIVATE"; break;
				case WM_SETFOCUS: name = "WM_SETFOCUS"; break;
				case WM_KILLFOCUS: name = "WM_KILLFOCUS"; break;
				case WM_SIZE: name = "WM_SIZE"; break;
				case WM_SYSCOMMAND: if ((wParam & 0xFFF0) == SC_MINIMIZE || (wParam & 0xFFF0) == SC_RESTORE) name = "WM_SYSCOMMAND"; break;
				default: break;
			}
			if (name && ++loggedMessages <= 300) {
				char text[160];
				std::snprintf(text, sizeof(text), "Renderer: the game's window got %s (wParam 0x%X)%s.", name, static_cast<unsigned>(wParam), IsIconic(handle) ? ", minimized" : "");
				Logger::Trace(text);
			}
			if (message == WM_ACTIVATEAPP) appActive = wParam != 0;
			if (message == WM_ACTIVATEAPP && wParam != 0) GameFixes::OnWindowActivated();
			// The game pauses when it is told it lost focus (0x558C70 runs its "inactive" callbacks on these three with
			// a low word of 0). With background running those messages are held back, so it keeps running. They are
			// handed to it just before focus comes back: what it lets go of on focus loss and sets up again on focus
			// coming back (the menu's 3D view was black when it only heard the second half) stays paired, with no
			// pause between.
			const bool focusMessage = message == WM_ACTIVATEAPP || message == WM_ACTIVATE || message == WM_NCACTIVATE;
			if (focusMessage && LOWORD(wParam) == 0 && BackgroundRunning::KeepsRunning()) {
				heldFocusLoss.push_back({ message, wParam, lParam });
				const LRESULT result = DefWindowProcA(handle, message, wParam, lParam);
				if (message == WM_ACTIVATEAPP && !IsIconic(handle) && !borderless) ShowWindow(handle, SW_MINIMIZE);
				BackgroundRunning::SetSeen(appActive && !IsIconic(handle));
				return result;
			}
			if (focusMessage && !heldFocusLoss.empty()) {
				const auto held = std::move(heldFocusLoss);
				heldFocusLoss.clear();
				for (const HeldMessage& lost : held) CallWindowProcA(original, handle, lost.message, lost.wParam, lost.lParam);
			}
			// No painting by Windows: DirectX 11/12 and Vulkan show their pictures past the window's own pixels, and the
			// window's black background painted into those covered the picture (it showed for a moment after a restore,
			// then went black; Vulkan showed once this was in).
			if (message == WM_ERASEBKGND) return 1;
			if (message == WM_PAINT) {
				ValidateRect(handle, nullptr);
				return 0;
			}
			const LRESULT result = CallWindowProcA(original, handle, message, wParam, lParam);
			// Full screen, the window covers the screen and stays on top, so it is minimized when the game loses focus (as
			// with patch.dll); otherwise it stays over the program the player switched to. Borderless, that program comes
			// in front of it as any window, and the game shows nothing new while it is behind (appActive).
			if (message == WM_ACTIVATEAPP && wParam == 0 && !IsIconic(handle) && !borderless) ShowWindow(handle, SW_MINIMIZE);
			// The game sets its window's size and place again when its screen mode changes; borderless puts it back.
			// 0x02E0 is WM_DPICHANGED: the window moved to a monitor with other scaling (the game is DPI-aware, Renderer.cpp).
			if (message == WM_WINDOWPOSCHANGED || message == WM_DISPLAYCHANGE || message == 0x02E0 || (message == WM_ACTIVATEAPP && wParam != 0)) PlaceBorderless(handle);
			if (message == WM_ACTIVATEAPP || message == WM_SIZE) BackgroundRunning::SetSeen(appActive && !IsIconic(handle));
			return result;
		}
		void WatchWindow(HWND handle) {
			char name[64]{};
			GetClassNameA(handle, name, sizeof(name));
			if (std::strcmp(name, "_BullfrogLibScreen") != 0) return;
			{
				// Released before the calls below, which send the window messages (and its procedure takes this lock).
				std::lock_guard lock(watchMutex);
				if (watchedWindows.contains(handle)) return;
				const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WatchedWindowProc)));
				if (!original) return;
				watchedWindows[handle] = original;
			}
			// The class's black brush is what Windows paints the background with; with none, nothing is painted.
			SetClassLongPtrA(handle, GCLP_HBRBACKGROUND, 0);
			// Not always on top (as patch.dll's NotOnTop does). On top, a game that stopped responding covered
			// everything, Task Manager included, and the PC had to be switched off. A window covering the screen still
			// hides the taskbar while it is in front.
			SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			PlaceBorderless(handle);
		}

		// A video: the player draws the film into a band of the screen (black around it), so only that band is shown,
		// scaled to the window (in the film's shape unless stretching). False when no video is playing or its film's
		// place is not known yet. The surface's memory copy must be current.
		bool PresentVideoArea(SurfaceObject* surface) {
			Renderer::VideoArea area{};
			if (!videoArea || !videoArea(surface->width, surface->height, area)) return false;
			const LONG left = std::clamp<LONG>(area.left, 0, static_cast<LONG>(surface->width));
			const LONG top = std::clamp<LONG>(area.top, 0, static_cast<LONG>(surface->height));
			const LONG right = std::clamp<LONG>(area.left + area.width, left, static_cast<LONG>(surface->width));
			const LONG bottom = std::clamp<LONG>(area.top + area.height, top, static_cast<LONG>(surface->height));
			if (right <= left || bottom <= top || area.shape <= 0.0) return false;
			if (!videoAreaLogged) {
				videoAreaLogged = true;
				Logger::Debug("Renderer: the video's film is " + std::to_string(right - left) + "x" + std::to_string(bottom - top) + " at " + std::to_string(left) + ","
					+ std::to_string(top) + " of the " + std::to_string(surface->width) + "x" + std::to_string(surface->height) + " screen; only that is shown.");
			}
			const PresentRect target = Target(static_cast<std::uint32_t>(area.shape * 10000.0 + 0.5), 10000);
			LogPresent(PresentSource::Screen, surface, target);
			backend->UploadScreen(surface->pixels.data() + static_cast<std::size_t>(top) * surface->width + left, static_cast<std::uint32_t>(right - left),
				static_cast<std::uint32_t>(bottom - top), surface->Pitch());
			backend->Present(PresentSource::Screen, target, vsync);
			lastPresent = PresentSource::Screen;
			return true;
		}
		void PresentFrame() {
			if (!renderTarget) return LogSkipped("no render target");
			if (!appActive) {
				// Time in the background is neither a slow frame nor a hang.
				frameStats.start = 0;
				lastShown = GetTickCount64();
				return LogSkipped("the game is in the background");
			}
			if (!EnsureBackend()) return LogSkipped("no graphics API");
			if (!WindowShowable()) return LogSkipped("the window is minimized, gone or has no size");
			// The 3D view can be attached to the video's back buffer (a video from the menu's Movies list, tested), so
			// the film comes this way too. Only while a video plays: the memory copy is brought up to date first.
			if (videoArea) {
				renderTarget->PrepareRead();
				if (PresentVideoArea(renderTarget)) return;
			}
			LogPresent(PresentSource::Frame, renderTarget, Target(renderTarget->width, renderTarget->height));
			if (!renderTarget->gpuCurrent) {
				UploadFrameTimed(renderTarget->pixels.data(), renderTarget->Pitch());
				renderTarget->gpuCurrent = true;
			}
			const std::int64_t presentStarted = Ticks();
			backend->Present(PresentSource::Frame, Target(renderTarget->width, renderTarget->height), vsync);
			CountShownFrame(renderTarget->width, renderTarget->height, TicksToMs(Ticks() - presentStarted));
			lastPresent = PresentSource::Frame;
		}
		void PresentSurface(SurfaceObject* surface) {
			if (!surface) return;
			if (!appActive) return LogSkipped("the game is in the background");
			if (!EnsureBackend()) return LogSkipped("no graphics API");
			if (!WindowShowable()) return LogSkipped("the window is minimized, gone or has no size");
			if (surface == renderTarget) {
				PresentFrame();
				return;
			}
			surface->PrepareRead();
			if (PresentVideoArea(surface)) return;
			LogPresent(PresentSource::Screen, surface, Target(surface->width, surface->height));
			backend->UploadScreen(surface->pixels.data(), surface->width, surface->height, surface->Pitch());
			backend->Present(PresentSource::Screen, Target(surface->width, surface->height), vsync);
			lastPresent = PresentSource::Screen;
		}

		// ---- The 3D device.
		D3DDEVICEDESC DeviceDescription() {
			D3DDEVICEDESC description{};
			description.dwSize = sizeof(D3DDEVICEDESC);
			description.dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_BCLIPPING | D3DDD_LIGHTINGCAPS | D3DDD_LINECAPS
				| D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH | D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
			description.dcmColorModel = D3DCOLOR_RGB;
			description.dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY
				| D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_HWRASTERIZATION;
			description.dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
			description.dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
			description.bClipping = TRUE;
			description.dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
			description.dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
			description.dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
			description.dlcLightingCaps.dwNumLights = 8;
			D3DPRIMCAPS caps{};
			caps.dwSize = sizeof(D3DPRIMCAPS);
			caps.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
			caps.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_ZBIAS;
			caps.dwZCmpCaps = 0xFF;
			caps.dwSrcBlendCaps = 0x1FFF;
			caps.dwDestBlendCaps = 0x1FFF;
			caps.dwAlphaCmpCaps = 0xFF;
			caps.dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAFLATBLEND
				| D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
			caps.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY | D3DPTEXTURECAPS_BORDER;
			caps.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR
				| D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
			caps.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA | D3DPTBLENDCAPS_MODULATEALPHA
				| D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
			caps.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
			description.dpcLineCaps = caps;
			description.dpcTriCaps = caps;
			description.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
			description.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
			description.dwMaxBufferSize = 0;
			description.dwMaxVertexCount = 65535;
			description.dwMinTextureWidth = 1;
			description.dwMinTextureHeight = 1;
			description.dwMaxTextureWidth = 4096;
			description.dwMaxTextureHeight = 4096;
			description.dwMaxTextureRepeat = 8192;
			description.dwMaxTextureAspectRatio = 4096;
			description.dwMaxAnisotropy = 1;
			description.dwFVFCaps = 3;
			description.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X
				| D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH
				| D3DTEXOPCAPS_BLENDDIFFUSEALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM
				| D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA
				| D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3
				| (bumpMode != 0 ? D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE : 0);
			description.wMaxTextureBlendStages = 3;
			description.wMaxSimultaneousTextures = 3;
			return description;
		}

		class ViewportObject final : public IDirect3DViewport3 {
			public:
				STDMETHOD(QueryInterface)(REFIID id, void** object) override {
					if (!object) return E_POINTER;
					if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirect3DViewport) || IsEqualGUID(id, IID_IDirect3DViewport2) || IsEqualGUID(id, IID_IDirect3DViewport3)) {
						*object = static_cast<IDirect3DViewport3*>(this);
						return D3D_OK;
					}
					*object = nullptr;
					return E_NOINTERFACE;
				}
				// One viewport for the whole game, never freed.
				STDMETHOD_(ULONG, AddRef)() override { return 2; }
				STDMETHOD_(ULONG, Release)() override { return 1; }
				STDMETHOD(Initialize)(LPDIRECT3D) override { return DDERR_ALREADYINITIALIZED; }
				STDMETHOD(GetViewport)(LPD3DVIEWPORT) override KF_UNUSED("Viewport::GetViewport")
				STDMETHOD(SetViewport)(LPD3DVIEWPORT) override { return D3D_OK; }
				STDMETHOD(TransformVertices)(DWORD, LPD3DTRANSFORMDATA, DWORD, LPDWORD) override KF_UNUSED("Viewport::TransformVertices")
				STDMETHOD(LightElements)(DWORD, LPD3DLIGHTDATA) override KF_UNUSED("Viewport::LightElements")
				STDMETHOD(SetBackground)(D3DMATERIALHANDLE) override { return D3D_OK; }
				STDMETHOD(GetBackground)(LPD3DMATERIALHANDLE, LPBOOL) override KF_UNUSED("Viewport::GetBackground")
				STDMETHOD(SetBackgroundDepth)(LPDIRECTDRAWSURFACE) override { return D3D_OK; }
				STDMETHOD(GetBackgroundDepth)(LPDIRECTDRAWSURFACE*, LPBOOL) override KF_UNUSED("Viewport::GetBackgroundDepth")
				STDMETHOD(Clear)(DWORD count, LPD3DRECT rects, DWORD flags) override { return DoClear(count, rects, flags, 0); }
				STDMETHOD(AddLight)(LPDIRECT3DLIGHT) override { return D3D_OK; }
				STDMETHOD(DeleteLight)(LPDIRECT3DLIGHT) override { return D3D_OK; }
				STDMETHOD(NextLight)(LPDIRECT3DLIGHT, LPDIRECT3DLIGHT*, DWORD) override KF_UNUSED("Viewport::NextLight")
				STDMETHOD(GetViewport2)(LPD3DVIEWPORT2) override KF_UNUSED("Viewport::GetViewport2")
				STDMETHOD(SetViewport2)(LPD3DVIEWPORT2) override { return D3D_OK; }
				STDMETHOD(SetBackgroundDepth2)(LPDIRECTDRAWSURFACE4) override { return D3D_OK; }
				STDMETHOD(GetBackgroundDepth2)(LPDIRECTDRAWSURFACE4*, LPBOOL) override KF_UNUSED("Viewport::GetBackgroundDepth2")
				STDMETHOD(Clear2)(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color, D3DVALUE, DWORD) override { return DoClear(count, rects, flags, color); }
			private:
				HRESULT DoClear(DWORD count, LPD3DRECT rects, DWORD flags, D3DCOLOR color) {
					Guard lock(layerMutex);
					if ((flags & D3DCLEAR_ZBUFFER) != 0 && EnsureBackend()) {
						backend->ClearDepth();
						depthClearPending = false;
					}
					if ((flags & D3DCLEAR_TARGET) != 0 && renderTarget) {
						// The whole frame: cleared on the graphics card, with nothing read back or uploaded.
						bool whole = count == 0 || !rects;
						for (DWORD i = 0; rects && i < count; ++i) {
							whole = whole || (rects[i].x1 <= 0 && rects[i].y1 <= 0 && rects[i].x2 >= static_cast<LONG>(renderTarget->width) && rects[i].y2 >= static_cast<LONG>(renderTarget->height));
						}
						if (whole && ClearOnCard(renderTarget, color)) return D3D_OK;
						readCause = ReadCause::Clear;
						renderTarget->PrepareRead();
						readCause = ReadCause::Other;
						if (count == 0 || !rects) renderTarget->Fill(nullptr, color);
						for (DWORD i = 0; rects && i < count; ++i) {
							const RECT area{ rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
							renderTarget->Fill(&area, color);
						}
						renderTarget->Changed();
					}
					return D3D_OK;
				}
		};
		ViewportObject viewport;

		class GammaObject final : public IDirectDrawGammaControl {
			public:
				STDMETHOD(QueryInterface)(REFIID id, void** object) override {
					if (!object) return E_POINTER;
					if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirectDrawGammaControl)) {
						*object = static_cast<IDirectDrawGammaControl*>(this);
						return DD_OK;
					}
					*object = nullptr;
					return E_NOINTERFACE;
				}
				STDMETHOD_(ULONG, AddRef)() override { return 2; }
				STDMETHOD_(ULONG, Release)() override { return 1; }
				// The game's gamma slider changes this ramp. patch.dll ignores it too, so the picture is the same as there.
				STDMETHOD(GetGammaRamp)(DWORD, LPDDGAMMARAMP out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					*out = ramp;
					return DD_OK;
				}
				STDMETHOD(SetGammaRamp)(DWORD, LPDDGAMMARAMP in) override {
					if (in) ramp = *in;
					return DD_OK;
				}
			private:
				DDGAMMARAMP ramp = [] {
					DDGAMMARAMP identity{};
					for (int i = 0; i < 256; ++i) identity.red[i] = identity.green[i] = identity.blue[i] = static_cast<WORD>(i * 257);
					return identity;
				}();
		};
		GammaObject gamma;

		HRESULT SurfaceObject::DoQuery(REFIID id, void** object) {
			if (!object) return E_POINTER;
			*object = nullptr;
			if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirectDrawSurface) || IsEqualGUID(id, IID_IDirectDrawSurface2)) {
				*object = static_cast<IDirectDrawSurface2*>(this);
			}
			else if (IsEqualGUID(id, IID_IDirectDrawSurface4)) {
				*object = static_cast<IDirectDrawSurface4*>(this);
			}
			else if (IsEqualGUID(id, IID_IDirect3DTexture2) || IsEqualGUID(id, IID_IDirect3DTexture)) {
				if (kind != Kind::Texture) return E_NOINTERFACE;
				*object = static_cast<IDirect3DTexture2*>(this);
			}
			else if (IsEqualGUID(id, IID_IDirectDrawGammaControl)) {
				*object = static_cast<IDirectDrawGammaControl*>(&gamma);
				return DD_OK;
			}
			else {
				LogUnused("Surface::QueryInterface (unknown interface)");
				return E_NOINTERFACE;
			}
			AddObject();
			return DD_OK;
		}

		// Render states the device keeps (D3DRENDERSTATETYPE goes up to 255) and the texture stage states (3 stages).
		constexpr std::size_t RenderStateCount = 256;
		constexpr std::size_t StageCount = 3;
		constexpr std::size_t StageStateCount = 32;

		class DeviceObject final : public IDirect3DDevice3 {
			public:
				DeviceObject() {
					renderStates.fill(0);
					renderStates[D3DRENDERSTATE_ZENABLE] = D3DZB_TRUE;
					renderStates[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
					renderStates[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
					renderStates[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
					renderStates[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
					renderStates[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
					renderStates[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
					renderStates[D3DRENDERSTATE_TEXTUREFACTOR] = 0xFFFFFFFF;
					for (std::size_t stage = 0; stage < StageCount; ++stage) {
						auto& states = stageStates[stage];
						states.fill(0);
						states[D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
						states[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
						states[D3DTSS_COLORARG2] = D3DTA_CURRENT;
						states[D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
						states[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
						states[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
						states[D3DTSS_TEXCOORDINDEX] = static_cast<DWORD>(stage);
						states[D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
						states[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
						states[D3DTSS_MAGFILTER] = D3DTFG_POINT;
						states[D3DTSS_MINFILTER] = D3DTFN_POINT;
						states[D3DTSS_MIPFILTER] = D3DTFP_NONE;
					}
				}
				~DeviceObject() {
					for (auto* texture : boundTextures) if (texture) texture->ReleaseObject();
				}

				STDMETHOD(QueryInterface)(REFIID id, void** object) override {
					if (!object) return E_POINTER;
					if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirect3DDevice3)) {
						AddRef();
						*object = static_cast<IDirect3DDevice3*>(this);
						return D3D_OK;
					}
					*object = nullptr;
					return E_NOINTERFACE;
				}
				STDMETHOD_(ULONG, AddRef)() override {
					Guard lock(layerMutex);
					return ++references;
				}
				STDMETHOD_(ULONG, Release)() override {
					Guard lock(layerMutex);
					const ULONG left = --references;
					if (left == 0) delete this;
					return left;
				}
				STDMETHOD(GetCaps)(LPD3DDEVICEDESC hardware, LPD3DDEVICEDESC software) override {
					const D3DDEVICEDESC description = DeviceDescription();
					if (hardware) CopyOut(hardware, description);
					if (software) {
						D3DDEVICEDESC none{};
						none.dwSize = sizeof(none);
						CopyOut(software, none);
					}
					return D3D_OK;
				}
				STDMETHOD(GetStats)(LPD3DSTATS) override KF_UNUSED("Device::GetStats")
				STDMETHOD(AddViewport)(LPDIRECT3DVIEWPORT3) override { return D3D_OK; }
				STDMETHOD(DeleteViewport)(LPDIRECT3DVIEWPORT3) override { return D3D_OK; }
				STDMETHOD(NextViewport)(LPDIRECT3DVIEWPORT3, LPDIRECT3DVIEWPORT3*, DWORD) override KF_UNUSED("Device::NextViewport")
				STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override {
					if (!callback) return DDERR_INVALIDPARAMS;
					DDPIXELFORMAT format;
					FillPixelFormat(format, false);
					if (callback(&format, context) != D3DENUMRET_OK) return D3D_OK;
					FillPixelFormat(format, true);
					if (callback(&format, context) != D3DENUMRET_OK || bumpMode == 0) return D3D_OK;
					// The game keeps the last 16-bit bump format it is offered of the kind it wants (0x5908xx): with
					// luminance first and du/dv only last, du/dv mode ends up with the plain one.
					FillBumpFormat(format, Bump::DuDvLuminance);
					if (callback(&format, context) != D3DENUMRET_OK) return D3D_OK;
					FillBumpFormat(format, Bump::DuDv);
					callback(&format, context);
					return D3D_OK;
				}
				STDMETHOD(BeginScene)() override {
					Guard lock(layerMutex);
					if (depthClearPending && EnsureBackend()) {
						backend->ClearDepth();
						depthClearPending = false;
					}
					return D3D_OK;
				}
				STDMETHOD(EndScene)() override { return D3D_OK; }
				STDMETHOD(GetDirect3D)(LPDIRECT3D3* direct3D) override;
				STDMETHOD(SetCurrentViewport)(LPDIRECT3DVIEWPORT3) override { return D3D_OK; }
				STDMETHOD(GetCurrentViewport)(LPDIRECT3DVIEWPORT3* out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					*out = &viewport;
					return D3D_OK;
				}
				STDMETHOD(SetRenderTarget)(LPDIRECTDRAWSURFACE4 surface, DWORD) override {
					Guard lock(layerMutex);
					if (SurfaceObject* target = SurfaceObject::From(surface)) SetTarget(target);
					return D3D_OK;
				}
				STDMETHOD(GetRenderTarget)(LPDIRECTDRAWSURFACE4* out) override {
					Guard lock(layerMutex);
					if (!out || !renderTarget) return DDERR_INVALIDPARAMS;
					renderTarget->AddObject();
					*out = static_cast<IDirectDrawSurface4*>(renderTarget);
					return D3D_OK;
				}
				STDMETHOD(Begin)(D3DPRIMITIVETYPE, DWORD, DWORD) override KF_UNUSED("Device::Begin")
				STDMETHOD(BeginIndexed)(D3DPRIMITIVETYPE, DWORD, LPVOID, DWORD, DWORD) override KF_UNUSED("Device::BeginIndexed")
				STDMETHOD(Vertex)(LPVOID) override KF_UNUSED("Device::Vertex")
				STDMETHOD(Index)(WORD) override KF_UNUSED("Device::Index")
				STDMETHOD(End)(DWORD) override KF_UNUSED("Device::End")
				STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE state, LPDWORD value) override {
					if (!value || static_cast<std::size_t>(state) >= RenderStateCount) return DDERR_INVALIDPARAMS;
					*value = renderStates[state];
					return D3D_OK;
				}
				STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE state, DWORD value) override {
					Guard lock(layerMutex);
					if (static_cast<std::size_t>(state) >= RenderStateCount) return D3D_OK;
					renderStates[state] = value;
					ApplyOldRenderState(state, value);
					return D3D_OK;
				}
				STDMETHOD(GetLightState)(D3DLIGHTSTATETYPE, LPDWORD value) override {
					if (value) *value = 0;
					return D3D_OK;
				}
				STDMETHOD(SetLightState)(D3DLIGHTSTATETYPE, DWORD) override { return D3D_OK; }
				STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override { return D3D_OK; }
				STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override KF_UNUSED("Device::GetTransform")
				STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, LPD3DMATRIX) override { return D3D_OK; }
				STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE type, DWORD format, LPVOID vertices, DWORD vertexCount, DWORD) override {
					Guard lock(layerMutex);
					indexScratch.resize(vertexCount);
					for (DWORD i = 0; i < vertexCount; ++i) indexScratch[i] = static_cast<std::uint16_t>(i);
					std::vector<std::uint16_t> indices = indexScratch;
					return DoDraw(type, format, vertices, vertexCount, indices.data(), static_cast<DWORD>(indices.size()));
				}
				STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, DWORD format, LPVOID vertices, DWORD vertexCount, LPWORD indices, DWORD indexCount, DWORD) override {
					Guard lock(layerMutex);
					return DoDraw(type, format, vertices, vertexCount, indices, indexCount);
				}
				STDMETHOD(SetClipStatus)(LPD3DCLIPSTATUS) override { return D3D_OK; }
				STDMETHOD(GetClipStatus)(LPD3DCLIPSTATUS) override KF_UNUSED("Device::GetClipStatus")
				STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, DWORD) override KF_UNUSED("Device::DrawPrimitiveStrided")
				STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE, DWORD, LPD3DDRAWPRIMITIVESTRIDEDDATA, DWORD, LPWORD, DWORD, DWORD) override KF_UNUSED("Device::DrawIndexedPrimitiveStrided")
				STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, DWORD, DWORD, DWORD) override KF_UNUSED("Device::DrawPrimitiveVB")
				STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE, LPDIRECT3DVERTEXBUFFER, LPWORD, DWORD, DWORD) override KF_UNUSED("Device::DrawIndexedPrimitiveVB")
				STDMETHOD(ComputeSphereVisibility)(LPD3DVECTOR, LPD3DVALUE, DWORD, DWORD, LPDWORD) override KF_UNUSED("Device::ComputeSphereVisibility")
				STDMETHOD(GetTexture)(DWORD stage, LPDIRECT3DTEXTURE2* out) override {
					Guard lock(layerMutex);
					if (!out || stage >= StageCount) return DDERR_INVALIDPARAMS;
					*out = boundTextures[stage] ? static_cast<IDirect3DTexture2*>(boundTextures[stage]) : nullptr;
					if (boundTextures[stage]) boundTextures[stage]->AddObject();
					return D3D_OK;
				}
				STDMETHOD(SetTexture)(DWORD stage, LPDIRECT3DTEXTURE2 texture) override {
					Guard lock(layerMutex);
					if (stage >= StageCount) return D3D_OK;
					Bind(stage, SurfaceObject::From(texture));
					return D3D_OK;
				}
				STDMETHOD(GetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, LPDWORD value) override {
					if (!value || stage >= StageCount || static_cast<std::size_t>(type) >= StageStateCount) return DDERR_INVALIDPARAMS;
					*value = stageStates[stage][type];
					return D3D_OK;
				}
				STDMETHOD(SetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override {
					Guard lock(layerMutex);
					if (stage >= StageCount || static_cast<std::size_t>(type) >= StageStateCount) return D3D_OK;
					stageStates[stage][type] = value;
					if (type == D3DTSS_ADDRESS) {
						stageStates[stage][D3DTSS_ADDRESSU] = value;
						stageStates[stage][D3DTSS_ADDRESSV] = value;
					}
					return D3D_OK;
				}
				STDMETHOD(ValidateDevice)(LPDWORD passes) override {
					if (passes) *passes = 1;
					return D3D_OK;
				}

				void SetTarget(SurfaceObject* target) {
					if (target == renderTarget) return;
					// The old target's frame goes back into memory before the frame becomes the new one's.
					if (renderTarget && !renderTarget->cpuCurrent && backend) {
						ReadFrameTimed(renderTarget->pixels.data(), renderTarget->Pitch());
						renderTarget->cpuCurrent = true;
					}
					if (renderTarget) {
						renderTarget->gpuCurrent = false;
						renderTarget->blitDirty = true;
					}
					renderTarget = target;
					Logger::Debug("Renderer: 3D draws into a " + std::to_string(target->width) + "x" + std::to_string(target->height)
						+ (target->kind == Kind::BackBuffer ? " back buffer." : " surface."));
					target->cpuCurrent = true;
					target->gpuCurrent = false;
					if (backend) backend->SetFrameSize(target->width, target->height);
				}

			private:
				ULONG references = 1;
				std::array<DWORD, RenderStateCount> renderStates{};
				std::array<std::array<DWORD, StageStateCount>, StageCount> stageStates{};
				std::array<SurfaceObject*, StageCount> boundTextures{};
				// Render::Vertex: inside this class "Vertex" is IDirect3DDevice3::Vertex.
				std::vector<Render::Vertex> vertexScratch;
				std::vector<std::uint16_t> indexScratch;
				std::vector<std::uint32_t> uploadScratch;

				void Bind(DWORD stage, SurfaceObject* texture) {
					if (texture == boundTextures[stage]) return;
					if (texture) texture->AddObject();
					if (boundTextures[stage]) boundTextures[stage]->ReleaseObject();
					boundTextures[stage] = texture;
				}
				// Direct3D 5 render states that Direct3D 6 turns into texture stage states.
				void ApplyOldRenderState(D3DRENDERSTATETYPE state, DWORD value) {
					auto& stage = stageStates[0];
					switch (state) {
						case D3DRENDERSTATE_TEXTUREHANDLE:
							Bind(0, value == 0 ? nullptr : SurfaceObject::Check(reinterpret_cast<SurfaceObject*>(static_cast<std::uintptr_t>(value))));
							break;
						case D3DRENDERSTATE_TEXTUREADDRESS:
							stage[D3DTSS_ADDRESSU] = stage[D3DTSS_ADDRESSV] = value;
							break;
						case D3DRENDERSTATE_TEXTUREADDRESSU: stage[D3DTSS_ADDRESSU] = value; break;
						case D3DRENDERSTATE_TEXTUREADDRESSV: stage[D3DTSS_ADDRESSV] = value; break;
						case D3DRENDERSTATE_TEXTUREMAG: stage[D3DTSS_MAGFILTER] = value == D3DFILTER_NEAREST ? D3DTFG_POINT : D3DTFG_LINEAR; break;
						case D3DRENDERSTATE_TEXTUREMIN: stage[D3DTSS_MINFILTER] = value == D3DFILTER_NEAREST ? D3DTFN_POINT : D3DTFN_LINEAR; break;
						case D3DRENDERSTATE_TEXTUREMAPBLEND: {
							const auto set = [&stage](DWORD colorOp, DWORD colorArg1, DWORD colorArg2, DWORD alphaOp, DWORD alphaArg1, DWORD alphaArg2) {
								stage[D3DTSS_COLOROP] = colorOp;
								stage[D3DTSS_COLORARG1] = colorArg1;
								stage[D3DTSS_COLORARG2] = colorArg2;
								stage[D3DTSS_ALPHAOP] = alphaOp;
								stage[D3DTSS_ALPHAARG1] = alphaArg1;
								stage[D3DTSS_ALPHAARG2] = alphaArg2;
							};
							switch (value) {
								case D3DTBLEND_DECAL:
								case D3DTBLEND_COPY:
									set(D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE); break;
								case D3DTBLEND_MODULATE:
									set(D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE); break;
								case D3DTBLEND_DECALALPHA:
									set(D3DTOP_BLENDTEXTUREALPHA, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_DIFFUSE); break;
								case D3DTBLEND_MODULATEALPHA:
									set(D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE); break;
								case D3DTBLEND_ADD:
									set(D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_DIFFUSE); break;
								default: break;
							}
							break;
						}
						default: break;
					}
				}

				static Blend ToBlend(DWORD value) {
					return value >= D3DBLEND_ZERO && value <= D3DBLEND_SRCALPHASAT ? static_cast<Blend>(value) : Blend::One;
				}
				static Address ToAddress(DWORD value) {
					return value >= D3DTADDRESS_WRAP && value <= D3DTADDRESS_BORDER ? static_cast<Address>(value) : Address::Wrap;
				}
				static void ToFloats(D3DCOLOR color, float out[4]) {
					out[0] = ((color >> 16) & 0xFF) / 255.0f;
					out[1] = ((color >> 8) & 0xFF) / 255.0f;
					out[2] = (color & 0xFF) / 255.0f;
					out[3] = ((color >> 24) & 0xFF) / 255.0f;
				}

				// The game's vertices (any pre-transformed layout) in our vertex format.
				bool ConvertVertices(DWORD format, const void* vertices, DWORD count) {
					if ((format & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) {
						LogUnused("Device draw with untransformed vertices");
						return false;
					}
					std::size_t offset = 16;
					std::size_t diffuseOffset = 0;
					bool hasDiffuse = false;
					if ((format & D3DFVF_NORMAL) != 0) offset += 12;
					if ((format & D3DFVF_RESERVED1) != 0) offset += 4;
					if ((format & D3DFVF_DIFFUSE) != 0) {
						hasDiffuse = true;
						diffuseOffset = offset;
						offset += 4;
					}
					if ((format & D3DFVF_SPECULAR) != 0) offset += 4;
					const std::size_t textureOffset = offset;
					const std::size_t textureCount = std::min<std::size_t>((format & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, 8);
					const std::size_t stride = offset + textureCount * 8;
					vertexScratch.resize(count);
					const auto* from = static_cast<const std::uint8_t*>(vertices);
					for (DWORD i = 0; i < count; ++i, from += stride) {
						Render::Vertex& vertex = vertexScratch[i];
						std::memcpy(&vertex.x, from, 16);
						vertex.diffuse = 0xFFFFFFFF;
						if (hasDiffuse) std::memcpy(&vertex.diffuse, from + diffuseOffset, 4);
						for (std::size_t t = 0; t < 3; ++t) {
							if (t < textureCount) std::memcpy(vertex.uv[t], from + textureOffset + t * 8, 8);
							else vertex.uv[t][0] = vertex.uv[t][1] = 0.0f;
						}
					}
					return true;
				}
				// Triangle lists (strips and fans unrolled) with the culled triangles left out. The vertices are in
				// screen space, so the winding is known here; the backends never cull.
				void BuildTriangles(D3DPRIMITIVETYPE type, const WORD* indices, DWORD indexCount, DWORD vertexCount) {
					indexScratch.clear();
					const DWORD cull = renderStates[D3DRENDERSTATE_CULLMODE];
					const auto add = [&](WORD a, WORD b, WORD c) {
						if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return;
						if (cull == D3DCULL_CW || cull == D3DCULL_CCW) {
							const Render::Vertex& p = vertexScratch[a];
							const Render::Vertex& q = vertexScratch[b];
							const Render::Vertex& r = vertexScratch[c];
							// Positive on screen (y down) means clockwise.
							const float area = (q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y);
							if (cull == D3DCULL_CW && area > 0.0f) return;
							if (cull == D3DCULL_CCW && area < 0.0f) return;
						}
						indexScratch.push_back(a);
						indexScratch.push_back(b);
						indexScratch.push_back(c);
					};
					if (type == D3DPT_TRIANGLELIST) {
						for (DWORD i = 0; i + 2 < indexCount; i += 3) add(indices[i], indices[i + 1], indices[i + 2]);
					}
					else if (type == D3DPT_TRIANGLESTRIP) {
						for (DWORD i = 0; i + 2 < indexCount; ++i) {
							if ((i & 1) == 0) add(indices[i], indices[i + 1], indices[i + 2]);
							else add(indices[i + 1], indices[i], indices[i + 2]);
						}
					}
					else if (type == D3DPT_TRIANGLEFAN) {
						for (DWORD i = 1; i + 1 < indexCount; ++i) add(indices[0], indices[i], indices[i + 1]);
					}
					else LogUnused("Device draw of points or lines");
				}

				void MakeState(DrawState& state) {
					const auto& rs = renderStates;
					state.blend = rs[D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
					DWORD source = rs[D3DRENDERSTATE_SRCBLEND];
					DWORD destination = rs[D3DRENDERSTATE_DESTBLEND];
					if (source == D3DBLEND_BOTHSRCALPHA) {
						source = D3DBLEND_SRCALPHA;
						destination = D3DBLEND_INVSRCALPHA;
					}
					else if (source == D3DBLEND_BOTHINVSRCALPHA) {
						source = D3DBLEND_INVSRCALPHA;
						destination = D3DBLEND_SRCALPHA;
					}
					state.source = ToBlend(source);
					state.destination = ToBlend(destination);
					state.depthTest = rs[D3DRENDERSTATE_ZENABLE] != D3DZB_FALSE;
					state.depthWrite = state.depthTest && rs[D3DRENDERSTATE_ZWRITEENABLE] != 0;
					const DWORD depthFunc = rs[D3DRENDERSTATE_ZFUNC];
					state.depthFunc = depthFunc >= D3DCMP_NEVER && depthFunc <= D3DCMP_ALWAYS ? static_cast<Compare>(depthFunc) : Compare::LessEqual;

					ShaderConstants& constants = state.constants;
					std::memset(&constants, 0, sizeof(constants));
					std::int32_t textureMask = 0;
					bool disabled = false;
					for (std::size_t stage = 0; stage < 4; ++stage) {
						const bool real = stage < StageCount && !disabled;
						const auto& states = stageStates[std::min<std::size_t>(stage, StageCount - 1)];
						if (!real || states[D3DTSS_COLOROP] == D3DTOP_DISABLE) {
							disabled = true;
							constants.colorOp[stage] = D3DTOP_DISABLE;
							constants.alphaOp[stage] = D3DTOP_DISABLE;
							if (stage < StageCount) state.textures[stage] = 0;
							continue;
						}
						constants.colorOp[stage] = static_cast<std::int32_t>(states[D3DTSS_COLOROP]);
						constants.colorArg1[stage] = static_cast<std::int32_t>(states[D3DTSS_COLORARG1]);
						constants.colorArg2[stage] = static_cast<std::int32_t>(states[D3DTSS_COLORARG2]);
						constants.alphaOp[stage] = static_cast<std::int32_t>(states[D3DTSS_ALPHAOP]);
						constants.alphaArg1[stage] = static_cast<std::int32_t>(states[D3DTSS_ALPHAARG1]);
						constants.alphaArg2[stage] = static_cast<std::int32_t>(states[D3DTSS_ALPHAARG2]);
						constants.texCoordIndex[stage] = static_cast<std::int32_t>(std::min<DWORD>(states[D3DTSS_TEXCOORDINDEX] & 0xFFFF, 2));
						SamplerState& sampler = state.samplers[stage];
						sampler.addressU = ToAddress(states[D3DTSS_ADDRESSU]);
						sampler.addressV = ToAddress(states[D3DTSS_ADDRESSV]);
						sampler.linear = states[D3DTSS_MAGFILTER] >= D3DTFG_LINEAR || states[D3DTSS_MINFILTER] >= D3DTFN_LINEAR;
						state.textures[stage] = 0;
						if (SurfaceObject* texture = boundTextures[stage]) {
							state.textures[stage] = UploadTexture(texture);
							if (state.textures[stage] != 0) textureMask |= 1 << stage;
						}
					}
					constants.misc[0] = rs[D3DRENDERSTATE_ALPHATESTENABLE] != 0 ? static_cast<std::int32_t>(rs[D3DRENDERSTATE_ALPHAFUNC]) : 0;
					constants.misc[1] = textureMask;
					ToFloats(rs[D3DRENDERSTATE_TEXTUREFACTOR], constants.textureFactor);
					const float width = renderTarget ? static_cast<float>(renderTarget->width) : 640.0f;
					const float height = renderTarget ? static_cast<float>(renderTarget->height) : 480.0f;
					constants.view[0] = 2.0f / width;
					// Clip space y: +1 at the top (DirectX), or -1 at the top (Vulkan's y points down).
					if (backend->FlipsY()) {
						constants.view[1] = 2.0f / height;
						constants.view[2] = -1.0f;
					}
					else {
						constants.view[1] = -2.0f / height;
						constants.view[2] = 1.0f;
					}
					constants.view[3] = (rs[D3DRENDERSTATE_ALPHAREF] & 0xFF) / 255.0f;
					// Every backend takes z from 0 to 1, as the game gives it.
					constants.depth[0] = 1.0f;
					constants.depth[1] = 0.0f;
					// The bump stage's matrix and luminance scale and offset (floats kept in the state's DWORDs).
					for (std::size_t stage = 0; stage < StageCount; ++stage) {
						const auto op = constants.colorOp[stage];
						if (op != D3DTOP_BUMPENVMAP && op != D3DTOP_BUMPENVMAPLUMINANCE) continue;
						const auto& states = stageStates[stage];
						std::memcpy(&constants.bumpMatrix[0], &states[D3DTSS_BUMPENVMAT00], sizeof(float));
						std::memcpy(&constants.bumpMatrix[1], &states[D3DTSS_BUMPENVMAT01], sizeof(float));
						std::memcpy(&constants.bumpMatrix[2], &states[D3DTSS_BUMPENVMAT10], sizeof(float));
						std::memcpy(&constants.bumpMatrix[3], &states[D3DTSS_BUMPENVMAT11], sizeof(float));
						std::memcpy(&constants.bumpLuminance[0], &states[D3DTSS_BUMPENVLSCALE], sizeof(float));
						std::memcpy(&constants.bumpLuminance[1], &states[D3DTSS_BUMPENVLOFFSET], sizeof(float));
						break;
					}
				}
				std::uint32_t UploadTexture(SurfaceObject* texture) {
					if (texture->kind == Kind::ZBuffer) return 0;
					if (texture->gpuTexture == 0) {
						texture->gpuTexture = backend->CreateTexture(texture->width, texture->height);
						texture->gpuDirty = true;
					}
					if (texture->gpuTexture != 0 && texture->gpuDirty) {
						texture->PrepareRead();
						if (texture->bump != Bump::None) {
							const auto* texels = reinterpret_cast<const std::uint16_t*>(texture->pixels.data());
							uploadScratch.resize(static_cast<std::size_t>(texture->width) * texture->height);
							for (std::size_t i = 0; i < uploadScratch.size(); ++i) uploadScratch[i] = BumpTexel(texels[i], texture->bump);
							backend->UpdateTexture(texture->gpuTexture, uploadScratch.data(), texture->width * 4);
						}
						else if (texture->alpha) backend->UpdateTexture(texture->gpuTexture, texture->pixels.data(), texture->Pitch());
						else {
							// No alpha bits: whatever the game left there must read as opaque.
							uploadScratch.resize(texture->pixels.size());
							for (std::size_t i = 0; i < uploadScratch.size(); ++i) uploadScratch[i] = texture->pixels[i] | 0xFF000000;
							backend->UpdateTexture(texture->gpuTexture, uploadScratch.data(), texture->Pitch());
						}
						texture->gpuDirty = false;
					}
					return texture->gpuTexture;
				}

				HRESULT DoDraw(D3DPRIMITIVETYPE type, DWORD format, const void* vertices, DWORD vertexCount, const WORD* indices, DWORD indexCount) {
					if (!vertices || !indices || vertexCount == 0 || indexCount == 0) return D3D_OK;
					if (!renderTarget || !EnsureBackend()) return D3D_OK;
					if (!ConvertVertices(format, vertices, vertexCount)) return D3D_OK;
					BuildTriangles(type, indices, indexCount, vertexCount);
					if (indexScratch.empty()) return D3D_OK;
					if (!renderTarget->gpuCurrent) {
						UploadFrameTimed(renderTarget->pixels.data(), renderTarget->Pitch());
						renderTarget->gpuCurrent = true;
					}
					DrawState state;
					MakeState(state);
					backend->Draw(state, vertexScratch.data(), vertexCount, indexScratch.data(), static_cast<std::uint32_t>(indexScratch.size()));
					renderTarget->cpuCurrent = false;
					return D3D_OK;
				}
		};

		// ---- Direct3D (versions 1, 2 and 3 in one object).
		class Direct3DObject final : public IDirect3D, public IDirect3D2, public IDirect3D3 {
			public:
				STDMETHOD(QueryInterface)(REFIID id, void** object) override;
				STDMETHOD_(ULONG, AddRef)() override { return 2; }
				STDMETHOD_(ULONG, Release)() override { return 1; }
				STDMETHOD(Initialize)(REFCLSID) override { return DDERR_ALREADYINITIALIZED; }
				STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK callback, LPVOID context) override {
					if (!callback) return DDERR_INVALIDPARAMS;
					D3DDEVICEDESC hardware = DeviceDescription();
					D3DDEVICEDESC software{};
					software.dwSize = sizeof(software);
					static char name[] = "KeeperForge2";
					static char description[] = "KeeperForge2 renderer";
					GUID guid = IID_IDirect3DHALDevice;
					callback(&guid, description, name, &hardware, &software, context);
					return D3D_OK;
				}
				STDMETHOD(CreateLight)(LPDIRECT3DLIGHT*, IUnknown*) override KF_UNUSED("Direct3D::CreateLight")
				STDMETHOD(CreateMaterial)(LPDIRECT3DMATERIAL*, IUnknown*) override KF_UNUSED("Direct3D::CreateMaterial")
				STDMETHOD(CreateMaterial)(LPDIRECT3DMATERIAL2*, IUnknown*) override KF_UNUSED("Direct3D2::CreateMaterial")
				STDMETHOD(CreateMaterial)(LPDIRECT3DMATERIAL3*, LPUNKNOWN) override KF_UNUSED("Direct3D3::CreateMaterial")
				STDMETHOD(CreateViewport)(LPDIRECT3DVIEWPORT* out, IUnknown*) override {
					if (!out) return DDERR_INVALIDPARAMS;
					*out = reinterpret_cast<LPDIRECT3DVIEWPORT>(static_cast<IDirect3DViewport3*>(&viewport));
					return D3D_OK;
				}
				STDMETHOD(CreateViewport)(LPDIRECT3DVIEWPORT2* out, IUnknown*) override {
					if (!out) return DDERR_INVALIDPARAMS;
					*out = reinterpret_cast<LPDIRECT3DVIEWPORT2>(static_cast<IDirect3DViewport3*>(&viewport));
					return D3D_OK;
				}
				STDMETHOD(CreateViewport)(LPDIRECT3DVIEWPORT3* out, LPUNKNOWN) override {
					if (!out) return DDERR_INVALIDPARAMS;
					*out = &viewport;
					return D3D_OK;
				}
				STDMETHOD(FindDevice)(LPD3DFINDDEVICESEARCH, LPD3DFINDDEVICERESULT) override KF_UNUSED("Direct3D::FindDevice")
				STDMETHOD(CreateDevice)(REFCLSID, LPDIRECTDRAWSURFACE, LPDIRECT3DDEVICE2*) override KF_UNUSED("Direct3D2::CreateDevice")
				STDMETHOD(CreateDevice)(REFCLSID, LPDIRECTDRAWSURFACE4 surface, LPDIRECT3DDEVICE3* out, LPUNKNOWN) override {
					Guard lock(layerMutex);
					SurfaceObject* target = SurfaceObject::From(surface);
					if (!target || !out) return DDERR_INVALIDPARAMS;
					auto* device = new DeviceObject();
					device->SetTarget(target);
					*out = device;
					return D3D_OK;
				}
				STDMETHOD(CreateVertexBuffer)(LPD3DVERTEXBUFFERDESC, LPDIRECT3DVERTEXBUFFER*, DWORD, LPUNKNOWN) override KF_UNUSED("Direct3D3::CreateVertexBuffer")
				STDMETHOD(EnumZBufferFormats)(REFCLSID, LPD3DENUMPIXELFORMATSCALLBACK callback, LPVOID context) override {
					if (!callback) return DDERR_INVALIDPARAMS;
					for (const DWORD bits : { 16u, 24u, 32u }) {
						DDPIXELFORMAT format{};
						format.dwSize = sizeof(format);
						format.dwFlags = DDPF_ZBUFFER;
						format.dwZBufferBitDepth = bits;
						format.dwZBitMask = bits == 16 ? 0xFFFF : 0xFFFFFF;
						if (callback(&format, context) != D3DENUMRET_OK) break;
					}
					return D3D_OK;
				}
				STDMETHOD(EvictManagedTextures)() override { return D3D_OK; }
		};
		Direct3DObject direct3D;

		HRESULT DeviceObject::GetDirect3D(LPDIRECT3D3* out) {
			if (!out) return DDERR_INVALIDPARAMS;
			*out = static_cast<IDirect3D3*>(&direct3D);
			return D3D_OK;
		}

		// ---- DirectDraw (versions 1, 2 and 4 in one object).
		class DirectDrawObject final : public IDirectDraw, public IDirectDraw2, public IDirectDraw4 {
			public:
				STDMETHOD(QueryInterface)(REFIID id, void** object) override {
					if (!object) return E_POINTER;
					*object = nullptr;
					if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirectDraw)) *object = static_cast<IDirectDraw*>(this);
					else if (IsEqualGUID(id, IID_IDirectDraw2)) *object = static_cast<IDirectDraw2*>(this);
					else if (IsEqualGUID(id, IID_IDirectDraw4)) *object = static_cast<IDirectDraw4*>(this);
					else if (IsEqualGUID(id, IID_IDirect3D)) *object = static_cast<IDirect3D*>(&direct3D);
					else if (IsEqualGUID(id, IID_IDirect3D2)) *object = static_cast<IDirect3D2*>(&direct3D);
					else if (IsEqualGUID(id, IID_IDirect3D3)) *object = static_cast<IDirect3D3*>(&direct3D);
					else {
						LogUnused("DirectDraw::QueryInterface (unknown interface)");
						return E_NOINTERFACE;
					}
					return DD_OK;
				}
				// One DirectDraw for the whole game, never freed (the game makes and drops several while it starts).
				STDMETHOD_(ULONG, AddRef)() override { return 2; }
				STDMETHOD_(ULONG, Release)() override { return 1; }
				STDMETHOD(Compact)() override { return DD_OK; }
				STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*) override KF_UNUSED("DirectDraw::CreateClipper")
				STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*) override KF_UNUSED("DirectDraw::CreatePalette")
				STDMETHOD(CreateSurface)(LPDDSURFACEDESC description, LPDIRECTDRAWSURFACE* out, IUnknown*) override {
					if (!description || !out) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC2 wider{};
					std::memcpy(&wider, description, std::min<DWORD>(description->dwSize, offsetof(DDSURFACEDESC, ddsCaps)));
					wider.ddsCaps.dwCaps = description->ddsCaps.dwCaps;
					wider.dwFlags = description->dwFlags;
					SurfaceObject* surface = nullptr;
					const HRESULT result = Create(wider, &surface);
					if (SUCCEEDED(result)) *out = reinterpret_cast<LPDIRECTDRAWSURFACE>(static_cast<IDirectDrawSurface2*>(surface));
					return result;
				}
				STDMETHOD(CreateSurface)(LPDDSURFACEDESC2 description, LPDIRECTDRAWSURFACE4* out, IUnknown*) override {
					if (!description || !out) return DDERR_INVALIDPARAMS;
					SurfaceObject* surface = nullptr;
					const HRESULT result = Create(*description, &surface);
					if (SUCCEEDED(result)) *out = static_cast<IDirectDrawSurface4*>(surface);
					return result;
				}
				STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) override KF_UNUSED("DirectDraw::DuplicateSurface")
				STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE4, LPDIRECTDRAWSURFACE4*) override KF_UNUSED("DirectDraw4::DuplicateSurface")
				STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID context, LPDDENUMMODESCALLBACK callback) override {
					if (!callback) return DDERR_INVALIDPARAMS;
					for (const auto& [width, height] : Modes()) {
						DDSURFACEDESC mode{};
						mode.dwSize = sizeof(mode);
						mode.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
						mode.dwWidth = width;
						mode.dwHeight = height;
						mode.lPitch = static_cast<LONG>(width * 4);
						FillPixelFormat(mode.ddpfPixelFormat, false);
						if (callback(&mode, context) != DDENUMRET_OK) break;
					}
					return DD_OK;
				}
				STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC2, LPVOID context, LPDDENUMMODESCALLBACK2 callback) override {
					if (!callback) return DDERR_INVALIDPARAMS;
					for (const auto& [width, height] : Modes()) {
						DDSURFACEDESC2 mode{};
						mode.dwSize = sizeof(mode);
						mode.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
						mode.dwWidth = width;
						mode.dwHeight = height;
						mode.lPitch = static_cast<LONG>(width * 4);
						FillPixelFormat(mode.ddpfPixelFormat, false);
						if (callback(&mode, context) != DDENUMRET_OK) break;
					}
					return DD_OK;
				}
				STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override KF_UNUSED("DirectDraw::EnumSurfaces")
				STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK2) override KF_UNUSED("DirectDraw4::EnumSurfaces")
				STDMETHOD(FlipToGDISurface)() override { return DD_OK; }
				STDMETHOD(GetCaps)(LPDDCAPS hardware, LPDDCAPS software) override {
					DDCAPS caps{};
					caps.dwSize = sizeof(caps);
					caps.dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM;
					caps.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NO2DDURING3DSCENE | DDCAPS2_PRIMARYGAMMA;
					caps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
					caps.dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY;
					caps.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
					caps.dwVidMemTotal = 512u * 1024 * 1024;
					caps.dwVidMemFree = 512u * 1024 * 1024;
					caps.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN
						| DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_VISIBLE | DDSCAPS_ZBUFFER | DDSCAPS_LOCALVIDMEM;
					if (hardware) CopyOut(hardware, caps);
					if (software) {
						DDCAPS none{};
						none.dwSize = sizeof(none);
						CopyOut(software, none);
					}
					return DD_OK;
				}
				STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC mode{};
					mode.dwSize = sizeof(mode);
					mode.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
					mode.dwWidth = modeWidth;
					mode.dwHeight = modeHeight;
					mode.lPitch = static_cast<LONG>(modeWidth * 4);
					FillPixelFormat(mode.ddpfPixelFormat, false);
					CopyOut(out, mode);
					return DD_OK;
				}
				STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2 out) override {
					if (!out) return DDERR_INVALIDPARAMS;
					DDSURFACEDESC2 mode{};
					mode.dwSize = sizeof(mode);
					mode.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
					mode.dwWidth = modeWidth;
					mode.dwHeight = modeHeight;
					mode.lPitch = static_cast<LONG>(modeWidth * 4);
					FillPixelFormat(mode.ddpfPixelFormat, false);
					CopyOut(out, mode);
					return DD_OK;
				}
				STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD) override {
					if (count) *count = 0;
					return DD_OK;
				}
				STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE*) override KF_UNUSED("DirectDraw::GetGDISurface")
				STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE4*) override KF_UNUSED("DirectDraw4::GetGDISurface")
				STDMETHOD(GetMonitorFrequency)(LPDWORD frequency) override {
					if (frequency) *frequency = 60;
					return DD_OK;
				}
				STDMETHOD(GetScanLine)(LPDWORD line) override {
					if (line) *line = 0;
					return DD_OK;
				}
				STDMETHOD(GetVerticalBlankStatus)(LPBOOL blank) override {
					if (blank) *blank = TRUE;
					return DD_OK;
				}
				STDMETHOD(Initialize)(GUID*) override { return DDERR_ALREADYINITIALIZED; }
				STDMETHOD(RestoreDisplayMode)() override { return DD_OK; }
				// The game hands over several windows: while it lists the display modes it uses a hidden "ModeList"
				// window it then destroys (0x5B4E20), and later its own. The backend always draws into the latest one.
				STDMETHOD(SetCooperativeLevel)(HWND handle, DWORD) override {
					Guard lock(layerMutex);
					if (handle && handle != window) {
						window = handle;
						char name[64]{};
						GetClassNameA(handle, name, sizeof(name));
						RECT client{};
						GetClientRect(handle, &client);
						Logger::Debug(std::string("Renderer: the game's window is now \"") + name + "\" (" + std::to_string(client.right - client.left) + "x"
							+ std::to_string(client.bottom - client.top) + ").");
						MoveToWindow();
						WatchWindow(handle);
					}
					return DD_OK;
				}
				STDMETHOD(SetDisplayMode)(DWORD width, DWORD height, DWORD) override { return SetMode(width, height); }
				STDMETHOD(SetDisplayMode)(DWORD width, DWORD height, DWORD, DWORD, DWORD) override { return SetMode(width, height); }
				STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) override { return DD_OK; }
				STDMETHOD(GetAvailableVidMem)(LPDDSCAPS, LPDWORD total, LPDWORD free) override { return VideoMemory(total, free); }
				STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2, LPDWORD total, LPDWORD free) override { return VideoMemory(total, free); }
				STDMETHOD(GetSurfaceFromDC)(HDC, LPDIRECTDRAWSURFACE4*) override KF_UNUSED("DirectDraw4::GetSurfaceFromDC")
				STDMETHOD(RestoreAllSurfaces)() override { return DD_OK; }
				STDMETHOD(TestCooperativeLevel)() override { return DD_OK; }
				STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER identifier, DWORD) override {
					if (!identifier) return DDERR_INVALIDPARAMS;
					std::memset(identifier, 0, sizeof(*identifier));
					std::snprintf(identifier->szDriver, sizeof(identifier->szDriver), "KeeperForge2");
					std::snprintf(identifier->szDescription, sizeof(identifier->szDescription), "KeeperForge2 (%s)", ApiName(Current()));
					return DD_OK;
				}

			private:
				static HRESULT VideoMemory(LPDWORD total, LPDWORD free) {
					if (total) *total = 512u * 1024 * 1024;
					if (free) *free = 512u * 1024 * 1024;
					return DD_OK;
				}
				static std::vector<std::pair<std::uint32_t, std::uint32_t>> Modes() {
					std::vector<std::pair<std::uint32_t, std::uint32_t>> modes = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 1600, 1200 } };
					const std::pair<std::uint32_t, std::uint32_t> desktop{ static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN)), static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN)) };
					if (desktop.first >= 640 && desktop.second >= 480 && !std::ranges::contains(modes, desktop)) modes.push_back(desktop);
					return modes;
				}
				static HRESULT SetMode(DWORD width, DWORD height) {
					Guard lock(layerMutex);
					if (width == 0 || height == 0) return DDERR_INVALIDMODE;
					modeWidth = width;
					modeHeight = height;
					depthClearPending = true;
					// The log then tells for each screen mode (a video, the menu, a level) whether pictures reach the window.
					framePresents = 0;
					screenPresents = 0;
					Logger::Info("Renderer: screen mode " + std::to_string(width) + "x" + std::to_string(height) + ".");
					return DD_OK;
				}
				static HRESULT Create(const DDSURFACEDESC2& description, SurfaceObject** out) {
					Guard lock(layerMutex);
					const DWORD flags = description.dwFlags;
					const DWORD caps = (flags & DDSD_CAPS) != 0 ? description.ddsCaps.dwCaps : 0;
					const bool hasFormat = (flags & DDSD_PIXELFORMAT) != 0;
					const DDPIXELFORMAT& format = description.ddpfPixelFormat;
					std::uint32_t width = (flags & DDSD_WIDTH) != 0 ? description.dwWidth : modeWidth;
					std::uint32_t height = (flags & DDSD_HEIGHT) != 0 ? description.dwHeight : modeHeight;
					if ((flags & DDSD_ZBUFFERBITDEPTH) != 0 || (caps & DDSCAPS_ZBUFFER) != 0 || (hasFormat && (format.dwFlags & DDPF_ZBUFFER) != 0)) {
						*out = new SurfaceObject(Kind::ZBuffer, width, height, caps | DDSCAPS_ZBUFFER, false);
						return DD_OK;
					}
					// No FourCC (video) surfaces: the game then plays its videos through plain surfaces.
					if (hasFormat && (format.dwFlags & DDPF_FOURCC) != 0) return DDERR_INVALIDPIXELFORMAT;
					if (hasFormat && (format.dwFlags & DDPF_BUMPDUDV) != 0) {
						if (bumpMode == 0 || format.dwBumpBitCount != 16) return DDERR_INVALIDPIXELFORMAT;
						auto* surface = new SurfaceObject((caps & DDSCAPS_TEXTURE) != 0 ? Kind::Texture : Kind::Plain, width, height, caps, false);
						surface->bump = (format.dwFlags & DDPF_BUMPLUMINANCE) != 0 ? Bump::DuDvLuminance : Bump::DuDv;
						*out = surface;
						return DD_OK;
					}
					if (hasFormat && ((format.dwFlags & DDPF_RGB) == 0 || format.dwRGBBitCount != 32)) return DDERR_INVALIDPIXELFORMAT;
					const bool alpha = hasFormat && (format.dwFlags & DDPF_ALPHAPIXELS) != 0;
					if ((caps & DDSCAPS_PRIMARYSURFACE) != 0) {
						auto* front = new SurfaceObject(Kind::Primary, modeWidth, modeHeight, caps & ~(DDSCAPS_BACKBUFFER), false);
						if ((flags & DDSD_BACKBUFFERCOUNT) != 0 && description.dwBackBufferCount > 0) {
							front->attachedBack = new SurfaceObject(Kind::BackBuffer, modeWidth, modeHeight, (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_BACKBUFFER, false);
						}
						primary = front;
						Logger::Debug("Renderer: the game made its screen, " + std::to_string(modeWidth) + "x" + std::to_string(modeHeight)
							+ (front->attachedBack ? " with a back buffer." : " without a back buffer."));
						*out = front;
						return DD_OK;
					}
					*out = new SurfaceObject((caps & DDSCAPS_TEXTURE) != 0 ? Kind::Texture : Kind::Plain, width, height, caps, alpha);
					return DD_OK;
				}
		};
		DirectDrawObject directDraw;

		HRESULT Direct3DObject::QueryInterface(REFIID id, void** object) {
			if (!object) return E_POINTER;
			*object = nullptr;
			if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IDirect3D)) *object = static_cast<IDirect3D*>(this);
			else if (IsEqualGUID(id, IID_IDirect3D2)) *object = static_cast<IDirect3D2*>(this);
			else if (IsEqualGUID(id, IID_IDirect3D3)) *object = static_cast<IDirect3D3*>(this);
			else if (IsEqualGUID(id, IID_IDirectDraw)) *object = static_cast<IDirectDraw*>(&directDraw);
			else if (IsEqualGUID(id, IID_IDirectDraw2)) *object = static_cast<IDirectDraw2*>(&directDraw);
			else if (IsEqualGUID(id, IID_IDirectDraw4)) *object = static_cast<IDirectDraw4*>(&directDraw);
			else return E_NOINTERFACE;
			return D3D_OK;
		}

		// ---- Backends.
		// config.yml anti_aliasing. Not for the hidden test window, which only checks the API can start.
		AntiAliasing antiAliasingSetting = DefaultAntiAliasing;
		std::unique_ptr<Backend> StartBackend(Api api, HWND target, bool probe = false) {
			auto made = CreateBackend(api);
			if (!made) return nullptr;
			if (!probe) made->SetAntiAliasing(antiAliasingSetting);
			if (!made->Init(target)) return nullptr;
			return made;
		}
		// Surfaces whose picture is only on the card (CopyFrameOnCard), into memory, before their backend goes.
		void BringCardPicturesHome() {
			std::vector<SurfaceObject*> waiting;
			for (auto* source : blitSources) if (source->cardOnly) waiting.push_back(source);
			for (auto* source : waiting) source->PrepareRead();
		}
		// Hands every surface the new backend needs to it: the frame (from memory) and, as they are used, the textures.
		void AdoptBackend(std::unique_ptr<Backend> made) {
			backend = std::move(made);
			for (auto* texture : textures) {
				texture->gpuTexture = 0;
				texture->gpuDirty = true;
			}
			// Their pictures belonged to the old backend; made again when next blitted.
			for (auto* source : blitSources) {
				source->blitTexture = 0;
				source->blitDirty = true;
				// Read into memory before (BringCardPicturesHome) when the old backend could still give it.
				source->cardOnly = false;
			}
			blitSources.clear();
			// The old backend's copy of the screen is gone (MoveToWindow and Switch read it into memory first).
			if (primary) primary->shownOnCard = false;
			if (renderTarget) {
				backend->SetFrameSize(renderTarget->width, renderTarget->height);
				renderTarget->gpuCurrent = false;
				renderTarget->cpuCurrent = true;
			}
			depthClearPending = true;
		}
		// The window changed: the backend's swap chain belongs to the old one. The picture and textures go back into
		// memory and the backend starts again on the new window when it is next needed.
		void MoveToWindow() {
			if (!backend) return;
			if (renderTarget && !renderTarget->cpuCurrent) {
				backend->ReadFrame(renderTarget->pixels.data(), renderTarget->Pitch());
				renderTarget->cpuCurrent = true;
			}
			if (primary) primary->PrepareRead();
			BringCardPicturesHome();
			for (auto* texture : textures) texture->gpuTexture = 0;
			backend.reset();
			backendFailed = false;
			Logger::Debug("Renderer: the game moved to another window.");
		}
		// The hang watchdog: the game once stopped for good (the PC had to be switched off; the log just ended, with no
		// crash report). Once a second it checks whether a thread has held the renderer's lock for more than 5 s, or
		// whether nothing was shown for 10 s while the game is in front, and logs where the threads involved are, so
		// the next hang can be found. Each case is logged once until it clears.
		DWORD WINAPI Watchdog(LPVOID) {
			DWORD reportedOwner = 0;
			ULONGLONG reportedSince = 0;
			bool reportedStill = false;
			for (;;) {
				Sleep(1000);
				const DWORD owner = lockOwner;
				const ULONGLONG since = lockSince;
				// Read after the lock's time: a lock taken in between is not counted as held for ages.
				const ULONGLONG now = GetTickCount64();
				if (owner != 0 && now > since && now - since > 5000 && !(owner == reportedOwner && since == reportedSince)) {
					reportedOwner = owner;
					reportedSince = since;
					Logger::Warning("Renderer: thread " + std::to_string(owner) + " has held the renderer's lock for " + std::to_string((now - since) / 1000) + " s. It is "
						+ CrashHandler::DescribeThread(owner));
					const DWORD shower = frameThread;
					if (shower != 0 && shower != owner) Logger::Warning("Renderer: the thread that shows frames (" + std::to_string(shower) + ") is " + CrashHandler::DescribeThread(shower));
				}
				const ULONGLONG shown = lastShown;
				const bool inFront = appActive && window && IsWindow(window) && !IsIconic(window);
				if (shown != 0 && now - shown < 10000) reportedStill = false;
				else if (shown != 0 && inFront && !reportedStill) {
					reportedStill = true;
					const DWORD shower = frameThread;
					Logger::Warning("Renderer: nothing was shown for " + std::to_string((now - shown) / 1000) + " s while the game is in front (a long load does this too). "
						+ (owner != 0 ? "Thread " + std::to_string(owner) + " holds the renderer's lock. " : std::string("No thread holds the renderer's lock. "))
						+ "The thread that shows frames (" + std::to_string(shower) + ") is " + (shower != 0 ? CrashHandler::DescribeThread(shower) : std::string("not known")));
				}
			}
		}
		void StartWatchdog() {
			static bool started = false;
			if (started) return;
			started = true;
			if (HANDLE thread = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr)) CloseHandle(thread);
		}
		bool ClearOnCard(SurfaceObject* surface, std::uint32_t color) {
			// Not while a video plays: its pictures are taken from the memory copy.
			if (!gpuClears || !surface || surface != renderTarget || surface->kind == Kind::Primary || videoArea) return false;
			if (!EnsureBackend()) return false;
			backend->ClearFrame(color);
			// The memory copy is now out of date; it is read back only if the game reads or partly writes the frame.
			surface->cpuCurrent = false;
			surface->gpuCurrent = true;
			++frameStats.gpuClears;
			return true;
		}
		// The source's memory picture as the backend's texture, made again only when it or the color key changed.
		// Keyed pixels get alpha 0 (the draw's alpha test drops them); every other pixel is opaque.
		std::vector<std::uint32_t> blitScratch;
		bool UploadBlitPicture(SurfaceObject* source, bool keyed, std::uint32_t keyLow, std::uint32_t keyHigh) {
			if (source->blitTexture == 0) {
				source->blitTexture = backend->CreateTexture(source->width, source->height);
				if (source->blitTexture == 0) return false;
				source->blitDirty = true;
				blitSources.insert(source);
			}
			if (!source->blitDirty && source->blitKeyed == keyed && (!keyed || (source->blitKeyLow == keyLow && source->blitKeyHigh == keyHigh))) return true;
			blitScratch.resize(source->pixels.size());
			for (std::size_t i = 0; i < blitScratch.size(); ++i) {
				const std::uint32_t pixel = source->pixels[i];
				const std::uint32_t color = pixel & 0x00FFFFFF;
				blitScratch[i] = keyed && color >= keyLow && color <= keyHigh ? 0 : pixel | 0xFF000000;
			}
			backend->UpdateTexture(source->blitTexture, blitScratch.data(), source->width * 4);
			source->blitDirty = false;
			source->blitKeyed = keyed;
			source->blitKeyLow = keyLow;
			source->blitKeyHigh = keyHigh;
			++frameStats.blitUploads;
			return true;
		}
		bool BlitOnCard(SurfaceObject* target, SurfaceObject* source, const RECT& from, const RECT& to, bool keyed, std::uint32_t keyLow, std::uint32_t keyHigh) {
			if (!gpuBlits || !target || target != renderTarget || target->kind == Kind::Primary || videoArea || !backend) return false;
			if (!source || source == target || source->kind == Kind::Primary || source->kind == Kind::ZBuffer) return false;
			// Only while the card has the newest frame: when only memory has it, the blit there is cheaper than
			// uploading the whole frame for it.
			if (!target->gpuCurrent) return false;
			// Parts outside the source are skipped in memory; the card would stretch its edge instead.
			if (from.left < 0 || from.top < 0 || from.right > static_cast<LONG>(source->width) || from.bottom > static_cast<LONG>(source->height)) return false;
			if (from.right <= from.left || from.bottom <= from.top || to.right <= to.left || to.bottom <= to.top) return true;
			// A picture only on the card (copied there from the frame) is drawn from there as it is; a keyed blit needs it
			// in memory to mark its keyed pixels.
			if (!source->cardOnly || keyed) source->PrepareRead();
			keyLow &= 0x00FFFFFF;
			keyHigh &= 0x00FFFFFF;
			if (!UploadBlitPicture(source, keyed, keyLow, keyHigh)) return false;

			// One texture stage that takes the texture as it is, no blending, no depth, point sampling; with a key the
			// alpha test (alpha greater than 0) drops the keyed pixels.
			DrawState state;
			state.blend = false;
			state.depthTest = false;
			state.depthWrite = false;
			state.depthFunc = Compare::Always;
			ShaderConstants& constants = state.constants;
			std::memset(&constants, 0, sizeof(constants));
			for (int stage = 0; stage < 4; ++stage) {
				constants.colorOp[stage] = D3DTOP_DISABLE;
				constants.alphaOp[stage] = D3DTOP_DISABLE;
			}
			constants.colorOp[0] = D3DTOP_SELECTARG1;
			constants.colorArg1[0] = D3DTA_TEXTURE;
			constants.alphaOp[0] = D3DTOP_SELECTARG1;
			constants.alphaArg1[0] = D3DTA_TEXTURE;
			constants.misc[0] = keyed ? D3DCMP_GREATER : 0;
			constants.misc[1] = 1;
			const float width = static_cast<float>(target->width);
			const float height = static_cast<float>(target->height);
			// As DeviceObject::MakeState sets them for the game's own draws.
			constants.view[0] = 2.0f / width;
			constants.view[1] = backend->FlipsY() ? 2.0f / height : -2.0f / height;
			constants.view[2] = backend->FlipsY() ? -1.0f : 1.0f;
			constants.view[3] = 0.0f;
			constants.depth[0] = 1.0f;
			constants.depth[1] = 0.0f;
			state.samplers[0] = { Address::Clamp, Address::Clamp, false };
			state.textures[0] = source->blitTexture;

			// The shaders add half a pixel (Direct3D 6 puts pixel centers on whole numbers), so corners half a pixel
			// back cover exactly the destination's pixels; nearest sampling then picks the same source pixels as the
			// copy in memory (for a stretched blit, within one pixel).
			const float left = static_cast<float>(to.left) - 0.5f;
			const float top = static_cast<float>(to.top) - 0.5f;
			const float right = static_cast<float>(to.right) - 0.5f;
			const float bottom = static_cast<float>(to.bottom) - 0.5f;
			const float u0 = static_cast<float>(from.left) / static_cast<float>(source->width);
			const float v0 = static_cast<float>(from.top) / static_cast<float>(source->height);
			const float u1 = static_cast<float>(from.right) / static_cast<float>(source->width);
			const float v1 = static_cast<float>(from.bottom) / static_cast<float>(source->height);
			Render::Vertex corners[4]{};
			const float places[4][4] = { { left, top, u0, v0 }, { right, top, u1, v0 }, { left, bottom, u0, v1 }, { right, bottom, u1, v1 } };
			for (int i = 0; i < 4; ++i) {
				corners[i].x = places[i][0];
				corners[i].y = places[i][1];
				corners[i].z = 0.0f;
				corners[i].rhw = 1.0f;
				corners[i].diffuse = 0xFFFFFFFF;
				corners[i].uv[0][0] = places[i][2];
				corners[i].uv[0][1] = places[i][3];
			}
			const std::uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 };
			backend->Draw(state, corners, 4, indices, 6);
			// The frame changed on the card only.
			target->cpuCurrent = false;
			++frameStats.gpuBlits;
			return true;
		}
		bool CopyFrameOnCard(SurfaceObject* target, SurfaceObject* source, const RECT& from, const RECT& to, bool keyed) {
			if (!gpuBlits || !backend || videoArea || keyed) return false;
			if (!source || source != renderTarget || source->cpuCurrent || !source->gpuCurrent) return false;
			if (!target || target == source || target->kind == Kind::Primary || target->kind == Kind::ZBuffer) return false;
			// Pixel for pixel, from inside the frame to inside the target.
			const LONG width = from.right - from.left;
			const LONG height = from.bottom - from.top;
			if (width <= 0 || height <= 0 || to.right - to.left != width || to.bottom - to.top != height) return false;
			if (to.left < 0 || to.top < 0 || to.right > static_cast<LONG>(target->width) || to.bottom > static_cast<LONG>(target->height)) return false;
			if (from.left < 0 || from.top < 0 || from.right > static_cast<LONG>(source->width) || from.bottom > static_cast<LONG>(source->height)) return false;
			// The texture holds no more than the frame can read back (ReadTexture).
			if (target->width > source->width || target->height > source->height) return false;
			const bool whole = to.left == 0 && to.top == 0 && width == static_cast<LONG>(target->width) && height == static_cast<LONG>(target->height);
			if (!whole && !target->cardOnly) {
				// Part of it: the rest of its picture must be in the texture first (once; it stays on the card after).
				target->PrepareRead();
				if (!UploadBlitPicture(target, false, 0, 0)) return false;
			}
			else if (target->blitTexture == 0) {
				target->blitTexture = backend->CreateTexture(target->width, target->height);
				if (target->blitTexture == 0) return false;
				blitSources.insert(target);
			}
			backend->CopyFrameToTexture(target->blitTexture, static_cast<std::uint32_t>(from.left), static_cast<std::uint32_t>(from.top),
				static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(to.left), static_cast<std::uint32_t>(to.top));
			// The texture is the picture now, unkeyed; memory is not. A 3D texture made from it is made again.
			target->cardOnly = true;
			target->blitDirty = false;
			target->blitKeyed = false;
			if (target->kind == Kind::Texture) target->gpuDirty = true;
			++frameStats.gpuCopies;
			static int logged = 0;
			if (logged < 3) {
				++logged;
				char text[160];
				std::snprintf(text, sizeof(text), "Renderer: copied %ldx%ld at %ld,%ld out of the %ux%u frame into a surface on the graphics card.",
					width, height, from.left, from.top, source->width, source->height);
				Logger::Debug(text);
			}
			return true;
		}
		bool EnsureBackend() {
			if (backend) return true;
			if (backendFailed || !window || !IsWindow(window)) return false;
			// The chosen API, then the others in order, so the game always gets a picture.
			std::vector<Api> order{ selectedApi };
			for (const Api api : { Api::D3D11, Api::Vulkan, Api::D3D12 }) if (api != selectedApi) order.push_back(api);
			for (const Api api : order) {
				if (auto made = StartBackend(api, window)) {
					if (api != selectedApi) Logger::Warning(std::string("Renderer: ") + ApiName(selectedApi) + " could not start on the game's window, using " + ApiName(api) + " instead.");
					selectedApi = api;
					AdoptBackend(std::move(made));
					Logger::Info(std::string("Renderer: drawing with ") + backend->Name() + ".");
					StartWatchdog();
					// For the log: whether the window can be seen at all, and what kind of window it is.
					RECT area{};
					GetWindowRect(window, &area);
					char text[220];
					std::snprintf(text, sizeof(text), "Renderer: the window is %s, %s, style 0x%X, extended style 0x%X, at %ld,%ld to %ld,%ld%s.",
						IsWindowVisible(window) ? "visible" : "hidden", IsIconic(window) ? "minimized" : "not minimized",
						static_cast<unsigned>(GetWindowLongA(window, GWL_STYLE)), static_cast<unsigned>(GetWindowLongA(window, GWL_EXSTYLE)),
						area.left, area.top, area.right, area.bottom, GetForegroundWindow() == window ? ", in front" : ", not in front");
					Logger::Debug(text);
					return true;
				}
			}
			backendFailed = true;
			Logger::Error("Renderer: no graphics API could start, the game cannot show anything.");
			return false;
		}

		// A hidden window to try an API on before the game commits to it.
		HWND MakeProbeWindow() {
			static const wchar_t* className = L"KeeperForge2RendererProbe";
			static bool registered = false;
			if (!registered) {
				WNDCLASSW windowClass{};
				windowClass.lpfnWndProc = DefWindowProcW;
				windowClass.hInstance = GetModuleHandleW(nullptr);
				windowClass.lpszClassName = className;
				windowClass.style = CS_OWNDC;
				registered = RegisterClassW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			}
			return CreateWindowExW(0, className, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		}
	}

	bool Select(Api api) {
		Guard lock(layerMutex);
		HWND probe = MakeProbeWindow();
		if (!probe) return false;
		bool works = false;
		{
			auto made = StartBackend(api, probe, true);
			works = made != nullptr;
		}
		DestroyWindow(probe);
		if (works) selectedApi = api;
		return works;
	}

	HRESULT WINAPI DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
		if (!out) return DDERR_INVALIDPARAMS;
		Guard lock(layerMutex);
		if (!inUse) Logger::Debug("Renderer: the game made its DirectDraw through KeeperForge2.");
		inUse = true;
		*out = static_cast<IDirectDraw*>(&directDraw);
		return DD_OK;
	}

	HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context) {
		if (!callback) return DDERR_INVALIDPARAMS;
		static char name[] = "KeeperForge2";
		static char description[] = "KeeperForge2";
		callback(nullptr, description, name, context);
		return DD_OK;
	}

	bool IsInUse() {
		Guard lock(layerMutex);
		return inUse;
	}

	Api Current() {
		Guard lock(layerMutex);
		return selectedApi;
	}

	bool Switch(Api api) {
		Guard lock(layerMutex);
		if (!backend) {
			// Nothing drawn yet: the new one simply starts when it is first needed.
			if (!Select(api)) return false;
			backendFailed = false;
			return true;
		}
		if (api == selectedApi) return true;
		// Everything the old backend holds goes back into memory first.
		if (renderTarget && !renderTarget->cpuCurrent) {
			backend->ReadFrame(renderTarget->pixels.data(), renderTarget->Pitch());
			renderTarget->cpuCurrent = true;
		}
		if (primary) primary->PrepareRead();
		BringCardPicturesHome();
		for (auto* texture : textures) texture->gpuTexture = 0;
		const Api old = selectedApi;
		backend.reset();
		Api started = api;
		auto made = StartBackend(api, window);
		if (!made) {
			Logger::Warning(std::string("Renderer: ") + ApiName(api) + " could not start, staying with " + ApiName(old) + ".");
			started = old;
			made = StartBackend(old, window);
			if (!made) {
				// The next draw tries every API in turn.
				backendFailed = false;
				return false;
			}
		}
		selectedApi = started;
		AdoptBackend(std::move(made));
		Logger::Info(std::string("Renderer: now drawing with ") + backend->Name() + ".");
		if (lastPresent == PresentSource::Screen && primary) PresentSurface(primary);
		else PresentFrame();
		return started == api;
	}

	void SetStretch(bool value) {
		Guard lock(layerMutex);
		stretch = value;
	}

	void WindowPointToPicture(HWND handle, std::int32_t pictureWidth, std::int32_t pictureHeight, std::int32_t& x, std::int32_t& y) {
		Guard lock(layerMutex);
		RECT client{};
		GetClientRect(handle, &client);
		PresentRect shown{ 0, 0, static_cast<std::int32_t>(client.right), static_cast<std::int32_t>(client.bottom) };
		if (handle == window) shown = Target(static_cast<std::uint32_t>(pictureWidth), static_cast<std::uint32_t>(pictureHeight));
		const auto width = shown.right - shown.left;
		const auto height = shown.bottom - shown.top;
		if (width <= 0 || height <= 0) return;
		x = MulDiv(x - shown.left, pictureWidth, width);
		y = MulDiv(y - shown.top, pictureHeight, height);
	}

	void SetVideoArea(Renderer::VideoAreaFn area) {
		Guard lock(layerMutex);
		videoArea = area;
		videoAreaLogged = false;
	}

	void SetVsync(bool value) {
		Guard lock(layerMutex);
		vsync = value;
	}
	void SetBorderless(bool value) { borderless = value; }

	void SetBumpMapping(int mode) { bumpMode = mode; }

	void SetGpuBlits(bool blits, bool clears) {
		Guard lock(layerMutex);
		gpuBlits = blits;
		gpuClears = clears;
	}

	void SetAntiAliasing(const AntiAliasing& value) {
		Guard lock(layerMutex);
		antiAliasingSetting = value;
	}
}
