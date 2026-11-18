#include "Backend.h"
#include "ShaderSources.h"
#include "Logger.h"
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

// DirectX 12, in the same way as the Vulkan backend: two frames in flight, each with its own command allocator, upload
// buffer and shader-visible descriptors; reading the frame back sends the work so far and waits for it.
namespace KeeperForge2::Render {
	using Microsoft::WRL::ComPtr;
	bool CompileHlsl(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& out);

	namespace {
		constexpr UINT64 RingSize = 32ull * 1024 * 1024;
		// Each of the two frames' upload buffers may grow to this (the game is 32-bit: they take its address space).
		constexpr UINT64 MaxRingSize = 128ull * 1024 * 1024;
		constexpr UINT DescriptorsPerFrame = 3 * 8192;
		constexpr UINT TextureDescriptors = 8192;
		constexpr UINT PresentSamplerIndex = SamplerKinds;
		constexpr UINT64 ConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		// Render target views: the frame, the two back buffers, then the anti-aliasing's targets.
		constexpr UINT FrameTarget = 0;
		constexpr UINT MsaaTarget = 3;
		constexpr UINT OutputTarget = 4;
		constexpr UINT EdgesTarget = 5;
		constexpr UINT WeightsTarget = 6;
		constexpr UINT TargetCount = 7;
		// The texture heap's fixed views; game textures take the rest.
		constexpr UINT FrameView0 = 0;
		constexpr UINT ScreenView = 1;
		constexpr UINT OutputView = 2;
		constexpr UINT EdgesView = 3;
		constexpr UINT WeightsView = 4;
		constexpr UINT FixedViews = 5;

		UINT64 AlignUp(UINT64 value, UINT64 alignment) { return (value + alignment - 1) / alignment * alignment; }
		std::string HexResult(HRESULT result) {
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
			return text;
		}

		using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
		using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = from;
			barrier.Transition.StateAfter = to;
			return barrier;
		}
		D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type) {
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = type;
			return heap;
		}
		D3D12_RESOURCE_DESC TextureDescription(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = width;
			description.Height = height;
			description.DepthOrArraySize = 1;
			description.MipLevels = 1;
			description.Format = format;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			description.Flags = flags;
			return description;
		}
		D3D12_RESOURCE_DESC BufferDescription(UINT64 size) {
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			description.Width = size;
			description.Height = 1;
			description.DepthOrArraySize = 1;
			description.MipLevels = 1;
			description.Format = DXGI_FORMAT_UNKNOWN;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return description;
		}

		class D3D12Backend final : public Backend {
			public:
				~D3D12Backend() override {
					if (queue && fence) WaitIdle();
					if (fenceEvent) CloseHandle(fenceEvent);
				}
				const char* Name() const override { return "DirectX 12"; }
				bool FlipsY() const override { return false; }

				bool Init(HWND target) override {
					window = target;
					HMODULE library = LoadLibraryW(L"d3d12.dll");
					if (!library) return Fail("is not available on this Windows", E_NOINTERFACE);
					const auto createDevice = reinterpret_cast<CreateDeviceFn>(GetProcAddress(library, "D3D12CreateDevice"));
					serializeRootSignature = reinterpret_cast<SerializeRootSignatureFn>(GetProcAddress(library, "D3D12SerializeRootSignature"));
					if (!createDevice || !serializeRootSignature) return Fail("is missing its entry points", E_NOINTERFACE);
					HRESULT result = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
					if (FAILED(result)) return Fail("found no device", result);
					D3D12_COMMAND_QUEUE_DESC queueDescription{};
					queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
					if (FAILED(result = device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)))) return Fail("could not make a queue", result);
					if (FAILED(result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return Fail("could not make a fence", result);
					fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
					for (FrameSlot& frame : slots) {
						if (FAILED(result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) return Fail("could not make a command allocator", result);
						if (!MakeRing(frame, RingSize)) return Fail("could not make its upload buffers", E_OUTOFMEMORY);
						D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DescriptorsPerFrame, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
						if (FAILED(result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&frame.views)))) return Fail("could not make a descriptor heap", result);
					}
					if (FAILED(result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return Fail("could not make a command list", result);
					commands->Close();
					viewSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					targetSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
					samplerSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
					D3D12_DESCRIPTOR_HEAP_DESC targets{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, TargetCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
					D3D12_DESCRIPTOR_HEAP_DESC depths{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
					D3D12_DESCRIPTOR_HEAP_DESC staticViews{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TextureDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
					D3D12_DESCRIPTOR_HEAP_DESC samplerHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SamplerKinds + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
					if (FAILED(device->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&targetHeap))) || FAILED(device->CreateDescriptorHeap(&depths, IID_PPV_ARGS(&depthHeap)))
						|| FAILED(device->CreateDescriptorHeap(&staticViews, IID_PPV_ARGS(&textureHeap))) || FAILED(device->CreateDescriptorHeap(&samplerHeap, IID_PPV_ARGS(&samplers)))) {
						return Fail("could not make its descriptor heaps", E_OUTOFMEMORY);
					}
					// The first descriptors of the texture heap are the frame, the screen and the anti-aliasing's pictures.
					for (UINT i = TextureDescriptors; i > FixedViews; --i) freeViews.push_back(i - 1);
					MakeSamplers();
					if (!MakeRootSignatures() || !MakePresentPipeline()) return false;
					if (!MakeSwapChain()) return false;
					const std::uint32_t white = 0xFFFFFFFF;
					whiteTexture = CreateTexture(1, 1);
					UpdateTexture(whiteTexture, &white, 4);
					SetUpAntiAliasing();
					return whiteTexture != 0;
				}

				bool SetFrameSize(std::uint32_t width, std::uint32_t height) override {
					if (width == frameWidth && height == frameHeight && frameColor) return true;
					FlushAndWait();
					// The anti-aliasing's pictures may still be read by the other frame's work.
					WaitIdle();
					frameColor.Reset();
					frameDepth.Reset();
					readback.Reset();
					shownColor.Reset();
					msaaColor.Reset();
					aaOutput.Reset();
					smaaEdges.Reset();
					smaaWeights.Reset();
					frameUnresolved = false;
					frameWidth = width;
					frameHeight = height;
					const D3D12_HEAP_PROPERTIES gpu = Heap(D3D12_HEAP_TYPE_DEFAULT);
					D3D12_RESOURCE_DESC color = TextureDescription(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
					D3D12_CLEAR_VALUE black{};
					black.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &color, D3D12_RESOURCE_STATE_RENDER_TARGET, &black, IID_PPV_ARGS(&frameColor)))) return false;
					// SaveShownFrame's copy, resting ready to copy from.
					const D3D12_RESOURCE_DESC shown = TextureDescription(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
					if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &shown, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&shownColor)))) return false;
					D3D12_RESOURCE_DESC depth = TextureDescription(width, height, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
					// MSAA: the draws go into a multisampled color and depth; frameColor holds the frame resolved.
					depth.SampleDesc.Count = msaaSamples;
					D3D12_CLEAR_VALUE farthest{};
					farthest.Format = DXGI_FORMAT_D32_FLOAT;
					farthest.DepthStencil.Depth = 1.0f;
					if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, &farthest, IID_PPV_ARGS(&frameDepth)))) return false;
					device->CreateRenderTargetView(frameColor.Get(), nullptr, TargetHandle(FrameTarget));
					device->CreateDepthStencilView(frameDepth.Get(), nullptr, depthHeap->GetCPUDescriptorHandleForHeapStart());
					device->CreateShaderResourceView(frameColor.Get(), nullptr, TextureView(FrameView0));
					if (msaaSamples > 1) {
						D3D12_RESOURCE_DESC multisampled = color;
						multisampled.SampleDesc.Count = msaaSamples;
						if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &multisampled, D3D12_RESOURCE_STATE_RENDER_TARGET, &black, IID_PPV_ARGS(&msaaColor)))) return false;
						device->CreateRenderTargetView(msaaColor.Get(), nullptr, TargetHandle(MsaaTarget));
					}
					if (postReady) {
						const bool smaa = antiAliasing.mode == AntiAliasingMode::Smaa;
						if (!MakePostTarget(aaOutput, DXGI_FORMAT_B8G8R8A8_UNORM, OutputTarget, OutputView)
							|| (smaa && (!MakePostTarget(smaaEdges, DXGI_FORMAT_R8G8_UNORM, EdgesTarget, EdgesView) || !MakePostTarget(smaaWeights, DXGI_FORMAT_R8G8B8A8_UNORM, WeightsTarget, WeightsView)))) {
							Logger::Warning("Renderer: DirectX 12 could not make the anti-aliasing's pictures; the frame is shown without it.");
							aaOutput.Reset();
							smaaEdges.Reset();
							smaaWeights.Reset();
						}
					}
					framePitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
					const D3D12_HEAP_PROPERTIES back = Heap(D3D12_HEAP_TYPE_READBACK);
					D3D12_RESOURCE_DESC buffer = BufferDescription(static_cast<UINT64>(framePitch) * height);
					if (FAILED(device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;
					ClearDepth();
					return true;
				}

				std::uint32_t CreateTexture(std::uint32_t width, std::uint32_t height) override {
					if (freeViews.empty()) return 0;
					TextureEntry entry;
					const D3D12_HEAP_PROPERTIES gpu = Heap(D3D12_HEAP_TYPE_DEFAULT);
					D3D12_RESOURCE_DESC description = TextureDescription(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
					if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&entry.texture)))) return 0;
					entry.width = width;
					entry.height = height;
					entry.view = freeViews.back();
					freeViews.pop_back();
					device->CreateShaderResourceView(entry.texture.Get(), nullptr, TextureView(entry.view));
					const std::uint32_t handle = nextTexture++;
					textures[handle] = std::move(entry);
					return handle;
				}
				void UpdateTexture(std::uint32_t texture, const void* pixels, std::uint32_t pitch) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					CopyPixels(found->second.texture.Get(), found->second.width, found->second.height, pixels, pitch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				void DestroyTexture(std::uint32_t texture) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					// Recorded or sent work may still read it: it goes when this frame's work is done. The frame is started
					// first: a slot frees its list when it starts, so a texture put on a slot that had not started yet (right
					// after a present) was freed while the frame just sent still used it (device removed, seen with
					// gpu_blits, whose pictures the game makes and drops every frame).
					Commands();
					Slot().doomed.push_back(std::move(found->second));
					textures.erase(found);
				}

				void UploadFrame(const void* pixels, std::uint32_t pitch) override {
					if (!frameColor) return;
					CopyPixels(frameColor.Get(), frameWidth, frameHeight, pixels, pitch, D3D12_RESOURCE_STATE_RENDER_TARGET);
					if (msaaColor) CopyIntoMultisampled();
				}
				void ReadFrame(void* pixels, std::uint32_t pitch) override {
					if (!frameColor || !readback) return;
					Resolve(Commands());
					ReadResource(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, pixels, pitch);
				}
				void ReadFrameArea(void* pixels, std::uint32_t pitch, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) override {
					if (!frameColor || !readback) return;
					ID3D12GraphicsCommandList* list = Commands();
					Resolve(list);
					D3D12_RESOURCE_BARRIER barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
					list->ResourceBarrier(1, &barrier);
					// The area alone, from the start of the readback buffer (it fits: it is no bigger than the frame).
					const UINT areaPitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
					D3D12_TEXTURE_COPY_LOCATION to{};
					to.pResource = readback.Get();
					to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					to.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, areaPitch };
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = frameColor.Get();
					from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					const D3D12_BOX box{ x, y, 0, x + width, y + height, 1 };
					list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
					barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(1, &barrier);
					FlushAndWait();
					void* mapped = nullptr;
					const D3D12_RANGE read{ 0, static_cast<SIZE_T>(areaPitch) * height };
					if (FAILED(readback->Map(0, &read, &mapped))) return;
					for (std::uint32_t row = 0; row < height; ++row) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y + row) * pitch + static_cast<std::size_t>(x) * 4,
							static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(row) * areaPitch, static_cast<std::size_t>(width) * 4);
					}
					const D3D12_RANGE written{ 0, 0 };
					readback->Unmap(0, &written);
				}
				void SaveShownFrame() override {
					if (!frameColor || !shownColor) return;
					ID3D12GraphicsCommandList* list = Commands();
					Resolve(list);
					D3D12_RESOURCE_BARRIER barriers[2] = {
						Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
						Transition(shownColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
					};
					list->ResourceBarrier(2, barriers);
					list->CopyResource(shownColor.Get(), frameColor.Get());
					barriers[0] = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					barriers[1] = Transition(shownColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
					list->ResourceBarrier(2, barriers);
				}
				void ReadShownFrame(void* pixels, std::uint32_t pitch) override {
					// It rests ready to copy from.
					if (shownColor && readback) ReadResource(shownColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, pixels, pitch);
				}
				// A frame-sized picture on the card into memory: copied to the readback buffer, the work so far sent and
				// waited for. resting is the state it is in and is left in.
				// The frame's size unless given (a smaller texture, ReadTexture); it fits the readback buffer either way.
				void ReadResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES resting, void* pixels, std::uint32_t pitch, std::uint32_t width = 0, std::uint32_t height = 0) {
					if (width == 0 || height == 0) {
						width = frameWidth;
						height = frameHeight;
					}
					const UINT rowPitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
					ID3D12GraphicsCommandList* list = Commands();
					D3D12_RESOURCE_BARRIER barrier = Transition(resource, resting, D3D12_RESOURCE_STATE_COPY_SOURCE);
					if (resting != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
					D3D12_TEXTURE_COPY_LOCATION to{};
					to.pResource = readback.Get();
					to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					to.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, rowPitch };
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = resource;
					from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
					barrier = Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, resting);
					if (resting != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
					FlushAndWait();
					void* mapped = nullptr;
					const D3D12_RANGE read{ 0, static_cast<SIZE_T>(rowPitch) * height };
					if (FAILED(readback->Map(0, &read, &mapped))) return;
					for (std::uint32_t y = 0; y < height; ++y) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * rowPitch, static_cast<std::size_t>(width) * 4);
					}
					const D3D12_RANGE written{ 0, 0 };
					readback->Unmap(0, &written);
				}
				void CopyFrameToTexture(std::uint32_t texture, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t toX, std::uint32_t toY) override {
					const auto found = textures.find(texture);
					if (found == textures.end() || !frameColor) return;
					ID3D12GraphicsCommandList* list = Commands();
					Resolve(list);
					ID3D12Resource* target = found->second.texture.Get();
					D3D12_RESOURCE_BARRIER barriers[2] = {
						Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
						Transition(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
					};
					list->ResourceBarrier(2, barriers);
					D3D12_TEXTURE_COPY_LOCATION to{};
					to.pResource = target;
					to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = frameColor.Get();
					from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					const D3D12_BOX box{ x, y, 0, x + width, y + height, 1 };
					list->CopyTextureRegion(&to, toX, toY, 0, &from, &box);
					barriers[0] = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					barriers[1] = Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					list->ResourceBarrier(2, barriers);
				}
				void ReadTexture(std::uint32_t texture, void* pixels, std::uint32_t pitch) override {
					const auto found = textures.find(texture);
					if (found == textures.end() || !readback) return;
					const TextureEntry& entry = found->second;
					// The readback buffer is the frame's size.
					if (entry.width > frameWidth || entry.height > frameHeight) return;
					ReadResource(entry.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, pixels, pitch, entry.width, entry.height);
				}
				void ClearDepth() override {
					if (!frameDepth) return;
					Commands()->ClearDepthStencilView(depthHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				}
				void ClearFrame(std::uint32_t pixel) override {
					if (!frameColor) return;
					float rgba[4];
					PixelToFloats(pixel, rgba);
					// Both rest as render targets.
					ID3D12GraphicsCommandList* list = Commands();
					list->ClearRenderTargetView(TargetHandle(FrameTarget), rgba, 0, nullptr);
					if (msaaColor) list->ClearRenderTargetView(TargetHandle(MsaaTarget), rgba, 0, nullptr);
					frameUnresolved = false;
				}

				void Draw(const DrawState& state, const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount) override {
					if (!frameColor) return;
					// None when it could not be made (after the device was removed it cannot): drawing with none crashed.
					ID3D12PipelineState* pipeline = PipelineFor(state);
					if (!pipeline) return;
					const UINT64 vertexBytes = AlignUp(static_cast<UINT64>(vertexCount) * sizeof(Vertex), ConstantAlignment);
					const UINT64 indexBytes = AlignUp(static_cast<UINT64>(indexCount) * sizeof(std::uint16_t), ConstantAlignment);
					const UINT64 total = vertexBytes + indexBytes + ConstantAlignment;
					if (Slot().viewsUsed + 3 > DescriptorsPerFrame) {
						FlushAndWait();
						NoteWait("too many draws for one frame");
					}
					std::uint8_t* written = nullptr;
					const UINT64 offset = RingAllocate(total, ConstantAlignment, written);
					if (!written) return;
					std::memcpy(written, vertices, static_cast<std::size_t>(vertexCount) * sizeof(Vertex));
					std::memcpy(written + vertexBytes, indices, static_cast<std::size_t>(indexCount) * sizeof(std::uint16_t));
					std::memcpy(written + vertexBytes + indexBytes, &state.constants, sizeof(ShaderConstants));
					ID3D12GraphicsCommandList* list = Commands();
					FrameSlot& frame = Slot();
					const UINT first = frame.viewsUsed;
					frame.viewsUsed += 3;
					for (UINT stage = 0; stage < 3; ++stage) {
						auto found = textures.find(state.textures[stage] != 0 ? state.textures[stage] : whiteTexture);
						if (found == textures.end()) found = textures.find(whiteTexture);
						device->CopyDescriptorsSimple(1, FrameView(frame, first + stage), TextureView(found->second.view), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					}
					const D3D12_GPU_VIRTUAL_ADDRESS base = frame.ring->GetGPUVirtualAddress() + offset;
					list->SetGraphicsRootSignature(drawSignature.Get());
					list->SetPipelineState(pipeline);
					list->SetGraphicsRootConstantBufferView(0, base + vertexBytes + indexBytes);
					list->SetGraphicsRootDescriptorTable(1, FrameGpuView(frame, first));
					for (UINT stage = 0; stage < 3; ++stage) list->SetGraphicsRootDescriptorTable(2 + stage, SamplerGpu(state.samplers[stage].Key()));
					const D3D12_VERTEX_BUFFER_VIEW vertexView{ base, static_cast<UINT>(vertexCount * sizeof(Vertex)), sizeof(Vertex) };
					const D3D12_INDEX_BUFFER_VIEW indexView{ base + vertexBytes, static_cast<UINT>(indexCount * sizeof(std::uint16_t)), DXGI_FORMAT_R16_UINT };
					list->IASetVertexBuffers(0, 1, &vertexView);
					list->IASetIndexBuffer(&indexView);
					list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					const D3D12_CPU_DESCRIPTOR_HANDLE target = TargetHandle(msaaColor ? MsaaTarget : FrameTarget);
					const D3D12_CPU_DESCRIPTOR_HANDLE depth = depthHeap->GetCPUDescriptorHandleForHeapStart();
					list->OMSetRenderTargets(1, &target, FALSE, &depth);
					const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(frameWidth), static_cast<LONG>(frameHeight) };
					list->RSSetViewports(1, &viewport);
					list->RSSetScissorRects(1, &scissor);
					list->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
					if (msaaColor) frameUnresolved = true;
				}

				void UploadScreen(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) override {
					if (!screen || width != screenWidth || height != screenHeight) {
						FlushAndWait();
						screen.Reset();
						const D3D12_HEAP_PROPERTIES gpu = Heap(D3D12_HEAP_TYPE_DEFAULT);
						D3D12_RESOURCE_DESC description = TextureDescription(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
						if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&screen)))) return;
						device->CreateShaderResourceView(screen.Get(), nullptr, TextureView(ScreenView));
						screenWidth = width;
						screenHeight = height;
					}
					CopyPixels(screen.Get(), width, height, pixels, pitch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}

				void Present(PresentSource source, const PresentRect& target, bool vsync) override {
					if (!swapChain || !ResizeToWindow()) return;
					// The frame resolved (MSAA) and smoothed (FXAA, SMAA) first, before any room for the present is taken.
					ID3D12Resource* picture = source == PresentSource::Frame ? frameColor.Get() : screen.Get();
					UINT pictureView = source == PresentSource::Frame ? FrameView0 : ScreenView;
					if (source == PresentSource::Frame && frameColor) {
						Resolve(Commands());
						if (aaOutput && RunPost()) {
							picture = aaOutput.Get();
							pictureView = OutputView;
						}
					}
					// The rectangle and the picture's descriptor are taken first: if this frame's room runs out, the work
					// so far is sent before anything of the present is recorded.
					if (Slot().viewsUsed + 1 > DescriptorsPerFrame || AlignUp(Slot().ringUsed, ConstantAlignment) + ConstantAlignment > Slot().ringSize) FlushAndWait();
					std::uint8_t* written = nullptr;
					const UINT64 offset = RingAllocate(ConstantAlignment, ConstantAlignment, written);
					ID3D12GraphicsCommandList* list = Commands();
					FrameSlot& frame = Slot();
					const UINT index = swapChain->GetCurrentBackBufferIndex();
					ID3D12Resource* shown = backBuffers[index].Get();
					D3D12_RESOURCE_BARRIER barrier = Transition(shown, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(1, &barrier);
					D3D12_CPU_DESCRIPTOR_HANDLE view = targetHeap->GetCPUDescriptorHandleForHeapStart();
					view.ptr += static_cast<SIZE_T>(1 + index) * targetSize;
					const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
					list->OMSetRenderTargets(1, &view, FALSE, nullptr);
					list->ClearRenderTargetView(view, black, 0, nullptr);
					if (picture && written) {
						if (picture == frameColor.Get()) {
							barrier = Transition(picture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
							list->ResourceBarrier(1, &barrier);
						}
						const UINT slot = frame.viewsUsed++;
						device->CopyDescriptorsSimple(1, FrameView(frame, slot), TextureView(pictureView), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
						{
							const float rect[4] = {
								static_cast<float>(target.left) / backBufferWidth * 2.0f - 1.0f,
								1.0f - static_cast<float>(target.top) / backBufferHeight * 2.0f,
								static_cast<float>(target.right) / backBufferWidth * 2.0f - 1.0f,
								1.0f - static_cast<float>(target.bottom) / backBufferHeight * 2.0f,
							};
							std::memcpy(written, rect, sizeof(rect));
							list->SetGraphicsRootSignature(presentSignature.Get());
							list->SetPipelineState(presentPipeline.Get());
							list->SetGraphicsRootConstantBufferView(0, frame.ring->GetGPUVirtualAddress() + offset);
							list->SetGraphicsRootDescriptorTable(1, FrameGpuView(frame, slot));
							list->SetGraphicsRootDescriptorTable(2, SamplerGpu(PresentSamplerIndex));
							const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferWidth), static_cast<float>(backBufferHeight), 0.0f, 1.0f };
							const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(backBufferWidth), static_cast<LONG>(backBufferHeight) };
							list->RSSetViewports(1, &viewport);
							list->RSSetScissorRects(1, &scissor);
							list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
							list->DrawInstanced(4, 1, 0, 0);
						}
						if (picture == frameColor.Get()) {
							barrier = Transition(picture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
							list->ResourceBarrier(1, &barrier);
						}
					}
					barrier = Transition(shown, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
					list->ResourceBarrier(1, &barrier);
					Submit();
					const HRESULT result = swapChain->Present(vsync ? 1 : 0, 0);
					if (FAILED(result) && !reportedPresent) {
						reportedPresent = true;
						Logger::Warning("Renderer: DirectX 12 could not show a frame (" + HexResult(result) + ").");
					}
					current = (current + 1) % 2;
				}

			private:
				static constexpr UINT BackBufferCount = 2;
				struct TextureEntry {
					ComPtr<ID3D12Resource> texture;
					std::uint32_t width = 0;
					std::uint32_t height = 0;
					UINT view = 0;
				};
				struct FrameSlot {
					ComPtr<ID3D12CommandAllocator> allocator;
					ComPtr<ID3D12Resource> ring;
					std::uint8_t* ringData = nullptr;
					UINT64 ringSize = 0;
					UINT64 ringUsed = 0;
					ComPtr<ID3D12DescriptorHeap> views;
					UINT viewsUsed = 0;
					UINT64 fenceValue = 0;
					std::vector<TextureEntry> doomed;
				};

				HWND window = nullptr;
				SerializeRootSignatureFn serializeRootSignature = nullptr;
				ComPtr<ID3D12Device> device;
				ComPtr<ID3D12CommandQueue> queue;
				ComPtr<ID3D12GraphicsCommandList> commands;
				bool recording = false;
				ComPtr<ID3D12Fence> fence;
				UINT64 fenceCounter = 0;
				HANDLE fenceEvent = nullptr;
				FrameSlot slots[2];
				UINT current = 0;
				ComPtr<IDXGISwapChain3> swapChain;
				ComPtr<ID3D12Resource> backBuffers[BackBufferCount];
				std::uint32_t backBufferWidth = 0;
				std::uint32_t backBufferHeight = 0;
				ComPtr<ID3D12DescriptorHeap> targetHeap;
				ComPtr<ID3D12DescriptorHeap> depthHeap;
				ComPtr<ID3D12DescriptorHeap> textureHeap;
				ComPtr<ID3D12DescriptorHeap> samplers;
				UINT viewSize = 0;
				UINT targetSize = 0;
				UINT samplerSize = 0;
				std::vector<UINT> freeViews;
				ComPtr<ID3D12RootSignature> drawSignature;
				ComPtr<ID3D12RootSignature> presentSignature;
				ComPtr<ID3DBlob> drawVertex;
				ComPtr<ID3DBlob> drawPixel;
				ComPtr<ID3D12PipelineState> presentPipeline;
				std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> pipelines;
				ComPtr<ID3D12Resource> frameColor;
				ComPtr<ID3D12Resource> frameDepth;
				ComPtr<ID3D12Resource> readback;
				ComPtr<ID3D12Resource> shownColor;
				std::uint32_t frameWidth = 0;
				std::uint32_t frameHeight = 0;
				UINT framePitch = 0;
				ComPtr<ID3D12Resource> screen;
				std::uint32_t screenWidth = 0;
				std::uint32_t screenHeight = 0;
				std::unordered_map<std::uint32_t, TextureEntry> textures;
				std::uint32_t nextTexture = 1;
				std::uint32_t whiteTexture = 0;
				bool reportedPresent = false;
				std::uint64_t midFrameWaits = 0;
				std::uint64_t nextWaitReport = 1;
				std::uint32_t slowPipelines = 0;

				// ---- Anti-aliasing (see AntiAliasing in Backend.h).
				// MSAA: samples per pixel (1 = off), the multisampled color the draws go into, and whether it holds
				// draws frameColor does not have yet.
				UINT msaaSamples = 1;
				ComPtr<ID3D12Resource> msaaColor;
				bool frameUnresolved = false;
				ComPtr<ID3D12PipelineState> copyPipeline;
				// FXAA uses pass 0; SMAA passes 0-2 are edges, weights and blending. They all rest in the pixel shader
				// resource state.
				bool postReady = false;
				ComPtr<ID3D12RootSignature> postSignature;
				ComPtr<ID3D12PipelineState> postPipelines[3];
				ComPtr<ID3D12Resource> aaOutput;
				ComPtr<ID3D12Resource> smaaEdges;
				ComPtr<ID3D12Resource> smaaWeights;

				D3D12_CPU_DESCRIPTOR_HANDLE TargetHandle(UINT index) const {
					D3D12_CPU_DESCRIPTOR_HANDLE handle = targetHeap->GetCPUDescriptorHandleForHeapStart();
					handle.ptr += static_cast<SIZE_T>(index) * targetSize;
					return handle;
				}
				// Turns the asked-for anti-aliasing down (MSAA samples the card cannot do) or off (shaders that do not
				// compile), and says in the log what is used.
				void SetUpAntiAliasing() {
					const AntiAliasing asked = antiAliasing;
					if (asked.mode == AntiAliasingMode::Msaa) {
						UINT samples = asked.level;
						while (samples > 1 && !SupportsSamples(samples)) samples /= 2;
						if (samples > 1 && MakeCopyPipeline(samples)) {
							msaaSamples = samples;
							antiAliasing.level = samples;
						}
						else antiAliasing = {};
					}
					else if (asked.mode != AntiAliasingMode::Off) {
						if (MakePostPipelines()) postReady = true;
						else antiAliasing = {};
					}
					if (antiAliasing == asked) Logger::Info("Renderer: DirectX 12 anti-aliasing: " + AntiAliasingName(antiAliasing) + ".");
					else Logger::Warning("Renderer: DirectX 12 cannot do " + AntiAliasingName(asked) + " here; it uses " + AntiAliasingName(antiAliasing) + ".");
				}
				bool SupportsSamples(UINT samples) {
					for (const DXGI_FORMAT format : { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D32_FLOAT }) {
						D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{ format, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0 };
						if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) || levels.NumQualityLevels == 0) return false;
					}
					return true;
				}
				// MSAA: the present's signature and vertex shader, drawing the frame pixel for pixel into every sample.
				bool MakeCopyPipeline(UINT samples) {
					ComPtr<ID3DBlob> vertex;
					ComPtr<ID3DBlob> pixel;
					if (!CompileHlsl(Shaders::PresentHlsl, "PresentVertex", "vs_5_0", vertex) || !CompileHlsl(Shaders::PresentHlsl, "CopyPixel", "ps_5_0", pixel)) return false;
					D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
					description.pRootSignature = presentSignature.Get();
					description.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
					description.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
					description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					description.SampleMask = UINT_MAX;
					description.RasterizerState = Rasterizer();
					description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					description.NumRenderTargets = 1;
					description.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = samples;
					const HRESULT result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&copyPipeline));
					return SUCCEEDED(result) || Fail("could not make its MSAA copy pipeline", result);
				}
				bool MakePostPipelines() {
					// b0 constants, t0-t2 inputs, and the samplers fixed in the signature: s0 linear, s1 point, clamped.
					D3D12_DESCRIPTOR_RANGE inputs{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
					D3D12_ROOT_PARAMETER parameters[2]{};
					parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
					parameters[0].Descriptor = { 0, 0 };
					parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
					parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					parameters[1].DescriptorTable = { 1, &inputs };
					parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					D3D12_STATIC_SAMPLER_DESC fixedSamplers[2]{};
					for (UINT i = 0; i < 2; ++i) {
						fixedSamplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
						fixedSamplers[i].AddressU = fixedSamplers[i].AddressV = fixedSamplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						fixedSamplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
						fixedSamplers[i].MaxLOD = D3D12_FLOAT32_MAX;
						fixedSamplers[i].ShaderRegister = i;
						fixedSamplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					}
					if (!MakeSignature(parameters, 2, D3D12_ROOT_SIGNATURE_FLAG_NONE, postSignature, fixedSamplers, 2)) return false;
					const std::string source = PostHlsl(antiAliasing);
					const bool smaa = antiAliasing.mode == AntiAliasingMode::Smaa;
					const char* vertexEntries[3] = { "PostVertex", nullptr, nullptr };
					const char* pixelEntries[3] = { "FxaaPixel", nullptr, nullptr };
					DXGI_FORMAT formats[3] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
					if (smaa) {
						vertexEntries[1] = vertexEntries[2] = "PostVertex";
						pixelEntries[0] = "SmaaEdgePixel";
						pixelEntries[1] = "SmaaWeightPixel";
						pixelEntries[2] = "SmaaBlendPixel";
						formats[0] = DXGI_FORMAT_R8G8_UNORM;
						formats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
						formats[2] = DXGI_FORMAT_B8G8R8A8_UNORM;
					}
					for (int pass = 0; pass < (smaa ? 3 : 1); ++pass) {
						ComPtr<ID3DBlob> vertex;
						ComPtr<ID3DBlob> pixel;
						if (!CompileHlsl(source.c_str(), vertexEntries[pass], "vs_5_0", vertex) || !CompileHlsl(source.c_str(), pixelEntries[pass], "ps_5_0", pixel)) return false;
						D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
						description.pRootSignature = postSignature.Get();
						description.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
						description.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
						description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
						description.SampleMask = UINT_MAX;
						description.RasterizerState = Rasterizer();
						description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
						description.NumRenderTargets = 1;
						description.RTVFormats[0] = formats[pass];
						description.SampleDesc.Count = 1;
						const HRESULT result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&postPipelines[pass]));
						if (FAILED(result)) return Fail("could not make an anti-aliasing pipeline", result);
					}
					return true;
				}
				bool MakePostTarget(ComPtr<ID3D12Resource>& texture, DXGI_FORMAT format, UINT target, UINT view) {
					const D3D12_HEAP_PROPERTIES gpu = Heap(D3D12_HEAP_TYPE_DEFAULT);
					D3D12_RESOURCE_DESC description = TextureDescription(frameWidth, frameHeight, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
					D3D12_CLEAR_VALUE zero{};
					zero.Format = format;
					if (FAILED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &zero, IID_PPV_ARGS(&texture)))) return false;
					device->CreateRenderTargetView(texture.Get(), nullptr, TargetHandle(target));
					device->CreateShaderResourceView(texture.Get(), nullptr, TextureView(view));
					return true;
				}
				// MSAA: brings frameColor up to date with the draws. Both are left as render targets.
				void Resolve(ID3D12GraphicsCommandList* list) {
					if (!msaaColor || !frameUnresolved) return;
					D3D12_RESOURCE_BARRIER barriers[2] = {
						Transition(msaaColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
						Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST),
					};
					list->ResourceBarrier(2, barriers);
					list->ResolveSubresource(frameColor.Get(), 0, msaaColor.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
					barriers[0] = Transition(msaaColor.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					barriers[1] = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(2, barriers);
					frameUnresolved = false;
				}
				// MSAA: frameColor (just filled from memory) into every sample of the multisampled frame.
				void CopyIntoMultisampled() {
					if (Slot().viewsUsed + 1 > DescriptorsPerFrame) FlushAndWait();
					std::uint8_t* written = nullptr;
					const UINT64 offset = RingAllocate(ConstantAlignment, ConstantAlignment, written);
					if (!written) return;
					const float whole[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
					std::memcpy(written, whole, sizeof(whole));
					ID3D12GraphicsCommandList* list = Commands();
					FrameSlot& frame = Slot();
					D3D12_RESOURCE_BARRIER barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					list->ResourceBarrier(1, &barrier);
					const UINT slot = frame.viewsUsed++;
					device->CopyDescriptorsSimple(1, FrameView(frame, slot), TextureView(FrameView0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					const D3D12_CPU_DESCRIPTOR_HANDLE target = TargetHandle(MsaaTarget);
					list->OMSetRenderTargets(1, &target, FALSE, nullptr);
					list->SetGraphicsRootSignature(presentSignature.Get());
					list->SetPipelineState(copyPipeline.Get());
					list->SetGraphicsRootConstantBufferView(0, frame.ring->GetGPUVirtualAddress() + offset);
					list->SetGraphicsRootDescriptorTable(1, FrameGpuView(frame, slot));
					list->SetGraphicsRootDescriptorTable(2, SamplerGpu(PresentSamplerIndex));
					const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(frameWidth), static_cast<LONG>(frameHeight) };
					list->RSSetViewports(1, &viewport);
					list->RSSetScissorRects(1, &scissor);
					list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					list->DrawInstanced(4, 1, 0, 0);
					barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(1, &barrier);
					frameUnresolved = false;
				}
				// FXAA or SMAA from frameColor into aaOutput. False if there was no room for it this frame.
				bool RunPost() {
					if (Slot().viewsUsed + 9 > DescriptorsPerFrame) FlushAndWait();
					std::uint8_t* written = nullptr;
					const UINT64 offset = RingAllocate(ConstantAlignment, ConstantAlignment, written);
					if (!written) return false;
					const PostConstants constants = MakePostConstants(antiAliasing, frameWidth, frameHeight);
					std::memcpy(written, &constants, sizeof(constants));
					ID3D12GraphicsCommandList* list = Commands();
					D3D12_RESOURCE_BARRIER barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					list->ResourceBarrier(1, &barrier);
					list->SetGraphicsRootSignature(postSignature.Get());
					list->SetGraphicsRootConstantBufferView(0, Slot().ring->GetGPUVirtualAddress() + offset);
					const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(frameWidth), static_cast<LONG>(frameHeight) };
					list->RSSetViewports(1, &viewport);
					list->RSSetScissorRects(1, &scissor);
					list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					if (antiAliasing.mode == AntiAliasingMode::Smaa) {
						PostPass(list, 0, smaaEdges.Get(), EdgesTarget, { FrameView0, FrameView0, FrameView0 }, true);
						PostPass(list, 1, smaaWeights.Get(), WeightsTarget, { EdgesView, EdgesView, EdgesView }, true);
						PostPass(list, 2, aaOutput.Get(), OutputTarget, { FrameView0, WeightsView, WeightsView }, false);
					}
					else PostPass(list, 0, aaOutput.Get(), OutputTarget, { FrameView0, FrameView0, FrameView0 }, false);
					barrier = Transition(frameColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(1, &barrier);
					return true;
				}
				void PostPass(ID3D12GraphicsCommandList* list, int pass, ID3D12Resource* target, UINT targetIndex, const UINT (&inputs)[3], bool clear) {
					D3D12_RESOURCE_BARRIER barrier = Transition(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
					list->ResourceBarrier(1, &barrier);
					const D3D12_CPU_DESCRIPTOR_HANDLE handle = TargetHandle(targetIndex);
					list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
					if (clear) {
						const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						list->ClearRenderTargetView(handle, zero, 0, nullptr);
					}
					FrameSlot& frame = Slot();
					const UINT first = frame.viewsUsed;
					frame.viewsUsed += 3;
					for (UINT i = 0; i < 3; ++i) device->CopyDescriptorsSimple(1, FrameView(frame, first + i), TextureView(inputs[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					list->SetPipelineState(postPipelines[pass].Get());
					list->SetGraphicsRootDescriptorTable(1, FrameGpuView(frame, first));
					list->DrawInstanced(3, 1, 0, 0);
					barrier = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					list->ResourceBarrier(1, &barrier);
				}

				bool Fail(const char* what, HRESULT result) {
					Logger::Warning(std::string("Renderer: DirectX 12 ") + what + " (" + HexResult(result) + ").");
					return false;
				}
				FrameSlot& Slot() { return slots[current]; }
				D3D12_CPU_DESCRIPTOR_HANDLE TextureView(UINT index) const {
					D3D12_CPU_DESCRIPTOR_HANDLE handle = textureHeap->GetCPUDescriptorHandleForHeapStart();
					handle.ptr += static_cast<SIZE_T>(index) * viewSize;
					return handle;
				}
				D3D12_CPU_DESCRIPTOR_HANDLE FrameView(FrameSlot& frame, UINT index) const {
					D3D12_CPU_DESCRIPTOR_HANDLE handle = frame.views->GetCPUDescriptorHandleForHeapStart();
					handle.ptr += static_cast<SIZE_T>(index) * viewSize;
					return handle;
				}
				D3D12_GPU_DESCRIPTOR_HANDLE FrameGpuView(FrameSlot& frame, UINT index) const {
					D3D12_GPU_DESCRIPTOR_HANDLE handle = frame.views->GetGPUDescriptorHandleForHeapStart();
					handle.ptr += static_cast<UINT64>(index) * viewSize;
					return handle;
				}
				D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpu(UINT index) const {
					D3D12_GPU_DESCRIPTOR_HANDLE handle = samplers->GetGPUDescriptorHandleForHeapStart();
					handle.ptr += static_cast<UINT64>(index) * samplerSize;
					return handle;
				}

				bool MakeRing(FrameSlot& frame, UINT64 size) {
					frame.ring.Reset();
					frame.ringData = nullptr;
					const D3D12_HEAP_PROPERTIES upload = Heap(D3D12_HEAP_TYPE_UPLOAD);
					D3D12_RESOURCE_DESC description = BufferDescription(size);
					if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.ring)))) return false;
					const D3D12_RANGE none{ 0, 0 };
					void* mapped = nullptr;
					if (FAILED(frame.ring->Map(0, &none, &mapped))) return false;
					frame.ringData = static_cast<std::uint8_t*>(mapped);
					frame.ringSize = size;
					return true;
				}
				// Every kind of sampler the draws can ask for, and the present's (linear, clamped).
				void MakeSamplers() {
					for (UINT key = 0; key <= SamplerKinds; ++key) {
						D3D12_SAMPLER_DESC description{};
						if (key == PresentSamplerIndex) {
							description.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
							description.AddressU = description.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						}
						else {
							description.Filter = (key & 16) != 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
							// SamplerState::Key: address U - 1 in bits 0-1, V - 1 in bits 2-3; D3D12 numbers its modes as Direct3D 6 does.
							description.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>((key & 3) + 1);
							description.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(((key >> 2) & 3) + 1);
						}
						description.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						description.MaxLOD = D3D12_FLOAT32_MAX;
						description.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
						D3D12_CPU_DESCRIPTOR_HANDLE handle = samplers->GetCPUDescriptorHandleForHeapStart();
						handle.ptr += static_cast<SIZE_T>(key) * samplerSize;
						device->CreateSampler(&description, handle);
					}
				}
				bool MakeSignature(const D3D12_ROOT_PARAMETER* parameters, UINT count, D3D12_ROOT_SIGNATURE_FLAGS flags, ComPtr<ID3D12RootSignature>& out,
					const D3D12_STATIC_SAMPLER_DESC* fixedSamplers = nullptr, UINT fixedCount = 0) {
					D3D12_ROOT_SIGNATURE_DESC description{ count, parameters, fixedCount, fixedSamplers, flags };
					ComPtr<ID3DBlob> blob;
					ComPtr<ID3DBlob> errors;
					HRESULT result = serializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
					if (FAILED(result)) return Fail("could not describe its root signature", result);
					result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
					return SUCCEEDED(result) || Fail("could not make its root signature", result);
				}
				bool MakeRootSignatures() {
					// Draws: b0 constants, t0-t2 textures, s0, s1, s2 each from its own table.
					D3D12_DESCRIPTOR_RANGE textureRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
					D3D12_DESCRIPTOR_RANGE samplerRanges[3] = {
						{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 1, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 2, 0, 0 },
					};
					D3D12_ROOT_PARAMETER draw[5]{};
					draw[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
					draw[0].Descriptor = { 0, 0 };
					draw[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
					draw[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					draw[1].DescriptorTable = { 1, &textureRange };
					draw[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					for (int stage = 0; stage < 3; ++stage) {
						draw[2 + stage].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
						draw[2 + stage].DescriptorTable = { 1, &samplerRanges[stage] };
						draw[2 + stage].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					}
					if (!MakeSignature(draw, 5, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, drawSignature)) return false;
					// Present: b0 rectangle, t0 picture, s0.
					D3D12_DESCRIPTOR_RANGE pictureRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
					D3D12_ROOT_PARAMETER present[3]{};
					present[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
					present[0].Descriptor = { 0, 0 };
					present[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
					present[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					present[1].DescriptorTable = { 1, &pictureRange };
					present[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					present[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					present[2].DescriptorTable = { 1, &samplerRanges[0] };
					present[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
					return MakeSignature(present, 3, D3D12_ROOT_SIGNATURE_FLAG_NONE, presentSignature);
				}
				static D3D12_BLEND AlphaFactor(Blend blend) {
					switch (blend) {
						case Blend::SrcColor: return D3D12_BLEND_SRC_ALPHA;
						case Blend::InvSrcColor: return D3D12_BLEND_INV_SRC_ALPHA;
						case Blend::DestColor: return D3D12_BLEND_DEST_ALPHA;
						case Blend::InvDestColor: return D3D12_BLEND_INV_DEST_ALPHA;
						default: return static_cast<D3D12_BLEND>(blend);
					}
				}
				static D3D12_RASTERIZER_DESC Rasterizer() {
					D3D12_RASTERIZER_DESC raster{};
					raster.FillMode = D3D12_FILL_MODE_SOLID;
					raster.CullMode = D3D12_CULL_MODE_NONE;
					raster.DepthClipEnable = TRUE;
					return raster;
				}
				bool MakePresentPipeline() {
					ComPtr<ID3DBlob> vertex;
					ComPtr<ID3DBlob> pixel;
					if (!CompileHlsl(Shaders::DrawHlsl, "VertexMain", "vs_5_0", drawVertex) || !CompileHlsl(Shaders::DrawHlsl, "PixelMain", "ps_5_0", drawPixel)
						|| !CompileHlsl(Shaders::PresentHlsl, "PresentVertex", "vs_5_0", vertex) || !CompileHlsl(Shaders::PresentHlsl, "PresentPixel", "ps_5_0", pixel)) {
						return false;
					}
					D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
					description.pRootSignature = presentSignature.Get();
					description.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
					description.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
					description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					description.SampleMask = UINT_MAX;
					description.RasterizerState = Rasterizer();
					description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					description.NumRenderTargets = 1;
					description.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = 1;
					const HRESULT result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&presentPipeline));
					return SUCCEEDED(result) || Fail("could not make its present pipeline", result);
				}
				ID3D12PipelineState* PipelineFor(const DrawState& state) {
					auto& made = pipelines[state.PipelineKey()];
					if (made) return made.Get();
					const D3D12_INPUT_ELEMENT_DESC layout[] = {
						{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
						{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
					};
					D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
					description.pRootSignature = drawSignature.Get();
					description.VS = { drawVertex->GetBufferPointer(), drawVertex->GetBufferSize() };
					description.PS = { drawPixel->GetBufferPointer(), drawPixel->GetBufferSize() };
					auto& target = description.BlendState.RenderTarget[0];
					target.BlendEnable = state.blend;
					// D3DBLEND and D3D12_BLEND number their factors the same way.
					target.SrcBlend = static_cast<D3D12_BLEND>(state.source);
					target.DestBlend = static_cast<D3D12_BLEND>(state.destination);
					target.BlendOp = D3D12_BLEND_OP_ADD;
					target.SrcBlendAlpha = AlphaFactor(state.source);
					target.DestBlendAlpha = AlphaFactor(state.destination);
					target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
					target.LogicOp = D3D12_LOGIC_OP_NOOP;
					target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					description.SampleMask = UINT_MAX;
					description.RasterizerState = Rasterizer();
					description.DepthStencilState.DepthEnable = state.depthTest;
					description.DepthStencilState.DepthWriteMask = state.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
					// D3DCMPFUNC and D3D12_COMPARISON_FUNC number the same way.
					description.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(state.depthFunc);
					description.InputLayout = { layout, ARRAYSIZE(layout) };
					description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					description.NumRenderTargets = 1;
					description.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
					description.SampleDesc.Count = msaaSamples;
					description.RasterizerState.MultisampleEnable = msaaSamples > 1;
					const ULONGLONG started = GetTickCount64();
					const HRESULT result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&made));
					if (FAILED(result)) Fail("could not make a pipeline", result);
					// For the log: a new kind of draw (first seen when something new happens in the game) makes its
					// pipeline on the spot; a slow one is a stutter.
					const ULONGLONG took = GetTickCount64() - started;
					if (took >= 20 && ++slowPipelines <= 20) {
						Logger::Debug("Renderer: DirectX 12 took " + std::to_string(took) + " ms to make a pipeline for a new kind of draw ("
							+ std::to_string(pipelines.size()) + " made so far).");
					}
					return made.Get();
				}

				bool MakeSwapChain() {
					ComPtr<IDXGIFactory4> factory;
					HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
					if (FAILED(result)) return Fail("could not reach DXGI", result);
					RECT client{};
					GetClientRect(window, &client);
					DXGI_SWAP_CHAIN_DESC1 description{};
					description.Width = std::max<LONG>(client.right - client.left, 1);
					description.Height = std::max<LONG>(client.bottom - client.top, 1);
					description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = 1;
					description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					description.BufferCount = BackBufferCount;
					description.Scaling = DXGI_SCALING_STRETCH;
					description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
					ComPtr<IDXGISwapChain1> made;
					result = factory->CreateSwapChainForHwnd(queue.Get(), window, &description, nullptr, nullptr, &made);
					if (FAILED(result)) return Fail("could not make a swap chain on the game's window", result);
					factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
					// For the log: the graphics card and the screen Windows puts the window on.
					DXGI_ADAPTER_DESC card{};
					ComPtr<IDXGIAdapter> adapter;
					if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) adapter->GetDesc(&card);
					DXGI_OUTPUT_DESC screen{};
					ComPtr<IDXGIOutput> output;
					const bool onScreen = SUCCEEDED(made->GetContainingOutput(&output)) && SUCCEEDED(output->GetDesc(&screen));
					char text[256];
					std::snprintf(text, sizeof(text), "Renderer: DirectX 12 uses %ls, flip swap chain %ux%u, on %ls.", card.Description, description.Width,
						description.Height, onScreen ? screen.DeviceName : L"no screen Windows could name");
					Logger::Info(text);
					made.As(&swapChain);
					return MakeBackBufferViews();
				}
				bool MakeBackBufferViews() {
					DXGI_SWAP_CHAIN_DESC1 description{};
					swapChain->GetDesc1(&description);
					backBufferWidth = description.Width;
					backBufferHeight = description.Height;
					for (UINT i = 0; i < BackBufferCount; ++i) {
						if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])))) return false;
						D3D12_CPU_DESCRIPTOR_HANDLE view = targetHeap->GetCPUDescriptorHandleForHeapStart();
						view.ptr += static_cast<SIZE_T>(1 + i) * targetSize;
						device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, view);
					}
					return true;
				}
				bool ResizeToWindow() {
					RECT client{};
					GetClientRect(window, &client);
					const UINT width = static_cast<UINT>(std::max<LONG>(client.right - client.left, 1));
					const UINT height = static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1));
					if (width == backBufferWidth && height == backBufferHeight) return true;
					// The back buffers must not be in use (or referenced) when they are resized.
					FlushAndWait();
					WaitIdle();
					for (auto& buffer : backBuffers) buffer.Reset();
					if (FAILED(swapChain->ResizeBuffers(BackBufferCount, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
					return MakeBackBufferViews();
				}

				// The command list for this frame, started (after the GPU finished with the frame's buffers) if needed.
				ID3D12GraphicsCommandList* Commands() {
					if (recording) return commands.Get();
					FrameSlot& frame = Slot();
					WaitFor(frame.fenceValue);
					for (const TextureEntry& entry : frame.doomed) freeViews.push_back(entry.view);
					frame.doomed.clear();
					frame.ringUsed = 0;
					frame.viewsUsed = 0;
					frame.allocator->Reset();
					commands->Reset(frame.allocator.Get(), nullptr);
					ID3D12DescriptorHeap* heaps[] = { frame.views.Get(), samplers.Get() };
					commands->SetDescriptorHeaps(2, heaps);
					recording = true;
					return commands.Get();
				}
				void Submit() {
					if (!recording) Commands();
					commands->Close();
					ID3D12CommandList* lists[] = { commands.Get() };
					queue->ExecuteCommandLists(1, lists);
					Slot().fenceValue = ++fenceCounter;
					queue->Signal(fence.Get(), fenceCounter);
					recording = false;
				}
				void FlushAndWait() {
					if (!recording) return;
					Submit();
					WaitFor(Slot().fenceValue);
				}
				void WaitFor(UINT64 value) {
					if (value == 0 || fence->GetCompletedValue() >= value) return;
					fence->SetEventOnCompletion(value, fenceEvent);
					WaitForSingleObject(fenceEvent, INFINITE);
				}
				// For the log: waits for the graphics card in the middle of a frame (each one is a stutter), at 1, 10, 100...
				void NoteWait(const char* why) {
					++midFrameWaits;
					if (midFrameWaits != nextWaitReport) return;
					nextWaitReport *= 10;
					Logger::Debug(std::string("Renderer: DirectX 12 waited for the graphics card in the middle of a frame (") + why + "), "
						+ std::to_string(midFrameWaits) + " times so far.");
				}
				void WaitIdle() {
					queue->Signal(fence.Get(), ++fenceCounter);
					WaitFor(fenceCounter);
				}
				UINT64 RingAllocate(UINT64 size, UINT64 alignment, std::uint8_t*& written) {
					written = nullptr;
					Commands();
					UINT64 offset = AlignUp(Slot().ringUsed, alignment);
					if (offset + size > Slot().ringSize) {
						// The frame's uploads did not fit: its work so far is sent and waited for, in the middle of the frame
						// (a 4K frame alone is 33 MB, and busy scenes upload many textures). The buffer grows so later frames
						// fit, up to MaxRingSize.
						FlushAndWait();
						const UINT64 before = Slot().ringSize;
						const UINT64 needed = AlignUp(size, RingSize);
						const UINT64 grown = (std::max)(needed, (std::min)(before * 2, MaxRingSize));
						if (grown > before) {
							if (MakeRing(Slot(), grown)) {
								char text[160];
								std::snprintf(text, sizeof(text), "Renderer: DirectX 12 upload buffer grown to %llu MB (a frame did not fit and waited for the graphics card).",
									static_cast<unsigned long long>(grown / (1024 * 1024)));
								Logger::Debug(text);
							}
							else if (needed > before || !MakeRing(Slot(), before)) {
								Fail("ran out of upload memory", E_OUTOFMEMORY);
								return 0;
							}
						}
						else NoteWait("its upload buffer was full");
						Commands();
						offset = 0;
					}
					Slot().ringUsed = offset + size;
					written = Slot().ringData + offset;
					return offset;
				}
				// A whole texture from memory, through this frame's upload buffer; the resource is left in state "after".
				void CopyPixels(ID3D12Resource* resource, std::uint32_t width, std::uint32_t height, const void* pixels, std::uint32_t pitch, D3D12_RESOURCE_STATES after,
					DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, UINT bytesPerPixel = 4) {
					const UINT rowPitch = static_cast<UINT>(AlignUp(static_cast<UINT64>(width) * bytesPerPixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
					std::uint8_t* written = nullptr;
					const UINT64 offset = RingAllocate(static_cast<UINT64>(rowPitch) * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, written);
					if (!written) return;
					for (std::uint32_t y = 0; y < height; ++y) std::memcpy(written + static_cast<std::size_t>(y) * rowPitch, static_cast<const std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(width) * bytesPerPixel);
					ID3D12GraphicsCommandList* list = Commands();
					D3D12_RESOURCE_BARRIER barrier = Transition(resource, after, D3D12_RESOURCE_STATE_COPY_DEST);
					list->ResourceBarrier(1, &barrier);
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = Slot().ring.Get();
					from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					from.PlacedFootprint.Offset = offset;
					from.PlacedFootprint.Footprint = { format, width, height, 1, rowPitch };
					D3D12_TEXTURE_COPY_LOCATION to{};
					to.pResource = resource;
					to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
					barrier = Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, after);
					list->ResourceBarrier(1, &barrier);
				}
		};
	}

	std::unique_ptr<Backend> CreateD3D12Backend() { return std::make_unique<D3D12Backend>(); }
}
