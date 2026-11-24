#include "Backend.h"
#include "VulkanApi.h"
#include "VulkanShaders.h"
#include "Config.h"
#include "Logger.h"
#include "StringUtils.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Vulkan 1.0. Commands are recorded into one of two command buffers (two frames in flight); everything a frame writes
// from the CPU (vertices, constants, pixels) goes into that frame's own buffer, which is reused once the GPU is done
// with it. When the game wants the frame back in memory (ReadFrame) the work so far is sent and waited for.
namespace KeeperForge2::Render {
	namespace {
		using namespace Vk;

		constexpr std::uint32_t SpirvMagic = 0x07230203;
		constexpr DeviceSize RingSize = 32ull * 1024 * 1024;
		constexpr std::uint32_t SetsPerFrame = 4096;
		// The ray tracer's descriptor sets per frame (one per trace; when they run out, the frame's work so far is sent and
		// they start over), and its bindings: tree, triangles, the trace's result, the blurred result, lights (storage
		// buffers), the frame (a sampled picture), the reflections, the bounced light before and after smoothing (storage).
		constexpr std::uint32_t RaySetsPerFrame = 64;
		constexpr std::uint32_t RayBindings = 9;
		constexpr std::uint32_t RayFrameBinding = 5;
		constexpr DeviceSize UniformAlignment = 256;
		constexpr std::uint64_t Forever = ~0ull;

		DeviceSize AlignUp(DeviceSize value, DeviceSize alignment) { return (value + alignment - 1) / alignment * alignment; }

		class VulkanBackend final : public Backend {
			public:
				~VulkanBackend() override { Shutdown(); }
				const char* Name() const override { return "Vulkan"; }
				// Vulkan's clip space y points down, the same way as the game's rows.
				bool FlipsY() const override { return true; }

				bool Init(HWND target) override {
					window = target;
					if (VulkanShaders::DrawVertex[0] != SpirvMagic || VulkanShaders::DrawFragment[0] != SpirvMagic) {
						return Fail("shaders are not built yet (run Runtime/Shaders/compile_vulkan_shaders.bat)");
					}
					library = LoadLibraryW(L"vulkan-1.dll");
					if (!library) return Fail("is not installed (no vulkan-1.dll)");
					getInstanceProc = reinterpret_cast<GetInstanceProcAddrFn>(GetProcAddress(library, "vkGetInstanceProcAddr"));
					if (!getInstanceProc || !LoadGlobal(vk.CreateInstance, "vkCreateInstance")) return Fail("loader is missing vkCreateInstance");
					ApplicationInfoData application{ ApplicationInfo, nullptr, "Dungeon Keeper 2", 1, "KeeperForge2", 1, ApiVersion10 };
					const char* instanceExtensions[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
					InstanceCreateInfoData instanceInfo{ InstanceCreateInfo, nullptr, 0, &application, 0, nullptr, 2, instanceExtensions };
					if (vk.CreateInstance(&instanceInfo, nullptr, &instance) != Success) return Fail("could not make an instance");
					if (!LoadInstanceFunctions()) return Fail("loader is missing functions");
					Win32SurfaceCreateInfoData surfaceInfo{ Win32SurfaceCreateInfoKHR, nullptr, 0, GetModuleHandleW(nullptr), window };
					if (vk.CreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != Success) return Fail("could not reach the game's window");
					if (!PickDevice()) return false;
					const float priority = 1.0f;
					DeviceQueueCreateInfoData queueInfo{ DeviceQueueCreateInfo, nullptr, 0, queueFamily, 1, &priority };
					const char* deviceExtensions[] = { "VK_KHR_swapchain" };
					// VkPhysicalDeviceFeatures (PhysicalDeviceFeatureCount Bool32s): only samplerAnisotropy is turned on, when the
					// device has it and texture_filtering asks for it. A device with it allows at least 16 (Vulkan's minimum limit).
					Bool32 features[PhysicalDeviceFeatureCount]{};
					if (anisotropy > 1) {
						Bool32 offered[PhysicalDeviceFeatureCount]{};
						vk.GetPhysicalDeviceFeatures(physical, offered);
						features[FeatureSamplerAnisotropy] = offered[FeatureSamplerAnisotropy];
						anisotropyEnabled = offered[FeatureSamplerAnisotropy] != 0;
						if (!anisotropyEnabled) Logger::Info("Renderer: this Vulkan device has no anisotropic filtering; textures use trilinear filtering.");
					}
					DeviceCreateInfoData deviceInfo{ DeviceCreateInfo, nullptr, 0, 1, &queueInfo, 0, nullptr, 1, deviceExtensions, features };
					if (vk.CreateDevice(physical, &deviceInfo, nullptr, &device) != Success) return Fail("could not make a device");
					if (!LoadDeviceFunctions()) return Fail("device is missing functions");
					vk.GetDeviceQueue(device, queueFamily, 0, &queue);
					vk.GetPhysicalDeviceMemoryProperties(physical, &memory);
					PickDepthFormat();
					const AntiAliasing asked = antiAliasing;
					ChooseAntiAliasing();
					if (!MakePipelineObjects() || !MakeFrameSlots()) return false;
					if (!MakeSwapchain(false)) return false;
					const std::uint32_t white = 0xFFFFFFFF;
					whiteTexture = CreateTexture(1, 1);
					UpdateTexture(whiteTexture, &white, 4);
					if (!MakePostObjects()) return false;
					if (antiAliasing == asked) Logger::Info("Renderer: Vulkan anti-aliasing: " + AntiAliasingName(antiAliasing) + ".");
					else if (postShadersMissing) Logger::Warning("Renderer: Vulkan's anti-aliasing shaders are not built yet (run Runtime/Shaders/compile_vulkan_shaders.bat); it runs without " + AntiAliasingName(asked) + ".");
					else Logger::Warning("Renderer: Vulkan cannot do " + AntiAliasingName(asked) + " here; it uses " + AntiAliasingName(antiAliasing) + ".");
					return whiteTexture != 0;
				}

				bool SetFrameSize(std::uint32_t width, std::uint32_t height) override {
					if (width == frameWidth && height == frameHeight && frameColor.image) return true;
					FlushAndWait();
					vk.DeviceWaitIdle(device);
					if (framebuffer) vk.DestroyFramebuffer(device, framebuffer, nullptr);
					framebuffer = 0;
					DestroyPostTargets();
					DestroyImage(frameColor);
					DestroyImage(frameDepth);
					DestroyImage(msaaColor);
					DestroyImage(shownColor);
					DestroyBuffer(readback);
					frameUnresolved = false;
					frameWidth = width;
					frameHeight = height;
					// Sampled by the anti-aliasing passes and the MSAA copy.
					if (!MakeImage(frameColor, width, height, FormatB8G8R8A8Unorm, ImageUsageColorAttachment | ImageUsageTransferSrc | ImageUsageTransferDst | ImageUsageSampled, AspectColor)) return false;
					// MSAA: the draws go into a multisampled color and depth; frameColor holds the frame resolved.
					if (!MakeImage(frameDepth, width, height, depthFormat, ImageUsageDepthStencilAttachment, depthAspect, msaaSamples)) return false;
					if (msaaSamples > 1 && !MakeImage(msaaColor, width, height, FormatB8G8R8A8Unorm, ImageUsageColorAttachment | ImageUsageTransferSrc, AspectColor, msaaSamples)) return false;
					const ImageView views[] = { msaaColor.image ? msaaColor.view : frameColor.view, frameDepth.view };
					FramebufferCreateInfoData framebufferInfo{ FramebufferCreateInfo, nullptr, 0, renderPass, 2, views, width, height, 1 };
					if (vk.CreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer) != Success) return false;
					if (!MakeBuffer(readback, static_cast<DeviceSize>(width) * height * 4, BufferUsageTransferDst, MemoryHostVisible | MemoryHostCoherent, MemoryHostCached)) return false;
					CommandBuffer commands = Commands();
					Barrier(commands, frameColor.image, AspectColor, LayoutUndefined, LayoutColorAttachment, 0, AccessColorAttachmentWrite, StageTopOfPipe, StageColorAttachmentOutput);
					if (msaaColor.image) Barrier(commands, msaaColor.image, AspectColor, LayoutUndefined, LayoutColorAttachment, 0, AccessColorAttachmentWrite, StageTopOfPipe, StageColorAttachmentOutput);
					// SaveShownFrame's copy, resting ready to copy from.
					if (!MakeImage(shownColor, width, height, FormatB8G8R8A8Unorm, ImageUsageTransferSrc | ImageUsageTransferDst, AspectColor)) return false;
					Barrier(commands, shownColor.image, AspectColor, LayoutUndefined, LayoutTransferSrc, 0, AccessTransferRead, StageTopOfPipe, StageTransfer);
					Barrier(commands, frameDepth.image, depthAspect, LayoutUndefined, LayoutDepthStencilAttachment, 0, AccessDepthStencilWrite, StageTopOfPipe, StageEarlyFragmentTests);
					if (postReady && !MakePostTargets()) {
						Logger::Warning("Renderer: Vulkan could not make the anti-aliasing's pictures; the frame is shown without it.");
						DestroyPostTargets();
					}
					ClearDepth();
					return true;
				}

				std::uint32_t CreateTexture(std::uint32_t width, std::uint32_t height, std::uint32_t levels = 1) override {
					ImageResource texture;
					// Copied from as well, for ReadTexture.
					if (!MakeImage(texture, width, height, FormatB8G8R8A8Unorm, ImageUsageSampled | ImageUsageTransferDst | ImageUsageTransferSrc, AspectColor, 1, levels)) return 0;
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, texture.image, AspectColor, LayoutUndefined, LayoutShaderReadOnly, 0, AccessShaderRead, StageTopOfPipe, StageFragmentShader);
					const std::uint32_t handle = nextTexture++;
					textures[handle] = texture;
					return handle;
				}
				void UpdateTexture(std::uint32_t texture, const void* pixels, std::uint32_t pitch, std::uint32_t level = 0) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					const ImageResource image = found->second;
					if (level >= image.levels) return;
					const std::uint32_t width = MipLevelSize(image.width, level);
					const std::uint32_t height = MipLevelSize(image.height, level);
					const DeviceSize size = static_cast<DeviceSize>(width) * height * 4;
					std::uint8_t* written = nullptr;
					const DeviceSize offset = RingAllocate(size, 16, written);
					if (!written) return;
					for (std::uint32_t y = 0; y < height; ++y) std::memcpy(written + static_cast<std::size_t>(y) * width * 4, static_cast<const std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(width) * 4);
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, image.image, AspectColor, LayoutShaderReadOnly, LayoutTransferDst, AccessShaderRead, AccessTransferWrite, StageFragmentShader, StageTransfer);
					CopyIn(commands, Slot().ring.buffer, offset, image, level);
					Barrier(commands, image.image, AspectColor, LayoutTransferDst, LayoutShaderReadOnly, AccessTransferWrite, AccessShaderRead, StageTransfer, StageFragmentShader);
				}
				void CopyFrameToTexture(std::uint32_t texture, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t toX, std::uint32_t toY) override {
					const auto found = textures.find(texture);
					if (found == textures.end() || !frameColor.image) return;
					const ImageResource image = found->second;
					Resolve();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
					Barrier(commands, image.image, AspectColor, LayoutShaderReadOnly, LayoutTransferDst, AccessShaderRead, AccessTransferWrite, StageFragmentShader, StageTransfer);
					// Same size on both sides, so the blit is a plain copy.
					ImageBlit blit{};
					blit.srcSubresource = { AspectColor, 0, 0, 1 };
					blit.srcOffsets[0] = { static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0 };
					blit.srcOffsets[1] = { static_cast<std::int32_t>(x + width), static_cast<std::int32_t>(y + height), 1 };
					blit.dstSubresource = { AspectColor, 0, 0, 1 };
					blit.dstOffsets[0] = { static_cast<std::int32_t>(toX), static_cast<std::int32_t>(toY), 0 };
					blit.dstOffsets[1] = { static_cast<std::int32_t>(toX + width), static_cast<std::int32_t>(toY + height), 1 };
					vk.CmdBlitImage(commands, frameColor.image, LayoutTransferSrc, image.image, LayoutTransferDst, 1, &blit, 0);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					Barrier(commands, image.image, AspectColor, LayoutTransferDst, LayoutShaderReadOnly, AccessTransferWrite, AccessShaderRead, StageTransfer, StageFragmentShader);
				}
				void ReadTexture(std::uint32_t texture, void* pixels, std::uint32_t pitch) override {
					const auto found = textures.find(texture);
					if (found == textures.end() || !readback.mapped) return;
					const ImageResource image = found->second;
					if (static_cast<DeviceSize>(image.width) * image.height * 4 > readback.size) return;
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, image.image, AspectColor, LayoutShaderReadOnly, LayoutTransferSrc, AccessShaderRead, AccessTransferRead, StageFragmentShader, StageTransfer);
					BufferImageCopy region{ 0, 0, 0, { AspectColor, 0, 0, 1 }, { 0, 0, 0 }, { image.width, image.height, 1 } };
					vk.CmdCopyImageToBuffer(commands, image.image, LayoutTransferSrc, readback.buffer, 1, &region);
					Barrier(commands, image.image, AspectColor, LayoutTransferSrc, LayoutShaderReadOnly, AccessTransferRead, AccessShaderRead, StageTransfer, StageFragmentShader);
					FlushAndWait();
					for (std::uint32_t row = 0; row < image.height; ++row) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch, static_cast<const std::uint8_t*>(readback.mapped) + static_cast<std::size_t>(row) * image.width * 4,
							static_cast<std::size_t>(image.width) * 4);
					}
				}
				void DestroyTexture(std::uint32_t texture) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					// Commands already recorded or sent may still use it: it goes once this frame's work is done. The frame is
					// started first: a slot frees its list when it starts, so an image put on a slot that had not started yet
					// (right after a present) was freed while the frame just sent still used it (device lost, seen with
					// gpu_blits, whose pictures the game makes and drops every frame).
					Commands();
					Slot().doomed.push_back(found->second);
					textures.erase(found);
				}

				void UploadFrame(const void* pixels, std::uint32_t pitch) override {
					if (!frameColor.image) return;
					const DeviceSize size = static_cast<DeviceSize>(frameWidth) * frameHeight * 4;
					std::uint8_t* written = nullptr;
					const DeviceSize offset = RingAllocate(size, 16, written);
					if (!written) return;
					for (std::uint32_t y = 0; y < frameHeight; ++y) std::memcpy(written + static_cast<std::size_t>(y) * frameWidth * 4, static_cast<const std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(frameWidth) * 4);
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferDst, AccessColorAttachmentWrite, AccessTransferWrite, StageColorAttachmentOutput, StageTransfer);
					CopyIn(commands, Slot().ring.buffer, offset, frameColor);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferDst, LayoutColorAttachment, AccessTransferWrite, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					if (msaaColor.image) CopyIntoMultisampled();
				}
				void ReadFrameArea(void* pixels, std::uint32_t pitch, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) override {
					if (!frameColor.image || !readback.mapped) return;
					Resolve();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
					// The area alone, packed from the start of the readback buffer.
					BufferImageCopy region{ 0, 0, 0, { AspectColor, 0, 0, 1 }, { static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0 }, { width, height, 1 } };
					vk.CmdCopyImageToBuffer(commands, frameColor.image, LayoutTransferSrc, readback.buffer, 1, &region);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					FlushAndWait();
					for (std::uint32_t row = 0; row < height; ++row) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y + row) * pitch + static_cast<std::size_t>(x) * 4,
							static_cast<const std::uint8_t*>(readback.mapped) + static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4);
					}
				}
				void SaveShownFrame() override {
					if (!frameColor.image || !shownColor.image) return;
					Resolve();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
					Barrier(commands, shownColor.image, AspectColor, LayoutTransferSrc, LayoutTransferDst, AccessTransferRead, AccessTransferWrite, StageTransfer, StageTransfer);
					// The same size, so a blit is a plain copy.
					ImageBlit blit{};
					blit.srcSubresource = { AspectColor, 0, 0, 1 };
					blit.srcOffsets[1] = { static_cast<std::int32_t>(frameWidth), static_cast<std::int32_t>(frameHeight), 1 };
					blit.dstSubresource = { AspectColor, 0, 0, 1 };
					blit.dstOffsets[1] = blit.srcOffsets[1];
					vk.CmdBlitImage(commands, frameColor.image, LayoutTransferSrc, shownColor.image, LayoutTransferDst, 1, &blit, 0);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					Barrier(commands, shownColor.image, AspectColor, LayoutTransferDst, LayoutTransferSrc, AccessTransferWrite, AccessTransferRead, StageTransfer, StageTransfer);
				}
				void ReadShownFrame(void* pixels, std::uint32_t pitch) override {
					if (!shownColor.image || !readback.mapped) return;
					// It rests ready to copy from.
					CommandBuffer commands = Commands();
					EndPass();
					BufferImageCopy region{ 0, 0, 0, { AspectColor, 0, 0, 1 }, { 0, 0, 0 }, { frameWidth, frameHeight, 1 } };
					vk.CmdCopyImageToBuffer(commands, shownColor.image, LayoutTransferSrc, readback.buffer, 1, &region);
					FlushAndWait();
					for (std::uint32_t y = 0; y < frameHeight; ++y) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<const std::uint8_t*>(readback.mapped) + static_cast<std::size_t>(y) * frameWidth * 4, static_cast<std::size_t>(frameWidth) * 4);
					}
				}
				void ReadFrame(void* pixels, std::uint32_t pitch) override {
					if (!frameColor.image || !readback.mapped) return;
					Resolve();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
					BufferImageCopy region{ 0, 0, 0, { AspectColor, 0, 0, 1 }, { 0, 0, 0 }, { frameWidth, frameHeight, 1 } };
					vk.CmdCopyImageToBuffer(commands, frameColor.image, LayoutTransferSrc, readback.buffer, 1, &region);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					FlushAndWait();
					for (std::uint32_t y = 0; y < frameHeight; ++y) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<const std::uint8_t*>(readback.mapped) + static_cast<std::size_t>(y) * frameWidth * 4, static_cast<std::size_t>(frameWidth) * 4);
					}
				}
				void ClearDepth() override {
					if (!framebuffer) return;
					CommandBuffer commands = Commands();
					BeginPass();
					ClearAttachment clear{};
					clear.aspectMask = depthAspect;
					clear.clearValue.depthStencil = { 1.0f, 0 };
					ClearRect rect{ { { 0, 0 }, { frameWidth, frameHeight } }, 0, 1 };
					vk.CmdClearAttachments(commands, 1, &clear, 1, &rect);
				}
				void ClearFrame(std::uint32_t pixel) override {
					if (!framebuffer) return;
					CommandBuffer commands = Commands();
					BeginPass();
					// Attachment 0 is the color the draws go into (the multisampled one with MSAA).
					ClearAttachment clear{};
					clear.aspectMask = AspectColor;
					clear.colorAttachment = 0;
					PixelToFloats(pixel, clear.clearValue.color.float32);
					ClearRect rect{ { { 0, 0 }, { frameWidth, frameHeight } }, 0, 1 };
					vk.CmdClearAttachments(commands, 1, &clear, 1, &rect);
					if (msaaColor.image) frameUnresolved = true;
				}

				void Draw(const DrawState& state, const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount) override {
					if (!framebuffer) return;
					const DeviceSize vertexBytes = AlignUp(static_cast<DeviceSize>(vertexCount) * sizeof(Vertex), UniformAlignment);
					const DeviceSize indexBytes = AlignUp(static_cast<DeviceSize>(indexCount) * sizeof(std::uint16_t), UniformAlignment);
					const DeviceSize total = vertexBytes + indexBytes + UniformAlignment;
					// Both the buffer space and a descriptor set must fit in this frame, or the work is sent first.
					if (Slot().setsUsed >= SetsPerFrame || AlignUp(Slot().ringUsed, UniformAlignment) + total > Slot().ring.size) FlushAndWait();
					std::uint8_t* written = nullptr;
					const DeviceSize offset = RingAllocate(total, UniformAlignment, written);
					if (!written) return;
					std::memcpy(written, vertices, static_cast<std::size_t>(vertexCount) * sizeof(Vertex));
					std::memcpy(written + vertexBytes, indices, static_cast<std::size_t>(indexCount) * sizeof(std::uint16_t));
					std::memcpy(written + vertexBytes + indexBytes, &state.constants, sizeof(ShaderConstants));
					FrameSlot& frame = Slot();
					DescriptorSet set = 0;
					DescriptorSetAllocateInfoData allocateInfo{ DescriptorSetAllocateInfo, nullptr, frame.descriptors, 1, &setLayout };
					if (vk.AllocateDescriptorSets(device, &allocateInfo, &set) != Success) return;
					++frame.setsUsed;
					DescriptorBufferInfo uniform{ frame.ring.buffer, offset + vertexBytes + indexBytes, sizeof(ShaderConstants) };
					DescriptorImageInfo images[3];
					for (int stage = 0; stage < 3; ++stage) {
						auto found = textures.find(state.textures[stage] != 0 ? state.textures[stage] : whiteTexture);
						if (found == textures.end()) found = textures.find(whiteTexture);
						images[stage] = { SamplerFor(state.samplers[stage]), found->second.view, LayoutShaderReadOnly };
					}
					WriteDescriptorSetData writes[2]{};
					writes[0] = { WriteDescriptorSet, nullptr, set, 0, 0, 1, DescriptorUniformBuffer, nullptr, &uniform, nullptr };
					writes[1] = { WriteDescriptorSet, nullptr, set, 1, 0, 3, DescriptorCombinedImageSampler, images, nullptr, nullptr };
					vk.UpdateDescriptorSets(device, 2, writes, 0, nullptr);

					// None when it could not be made (such as after the device was lost): binding none is not allowed.
					const Pipeline pipeline = PipelineFor(state);
					if (!pipeline) return;
					CommandBuffer commands = Commands();
					BeginPass();
					vk.CmdBindPipeline(commands, 0, pipeline);
					vk.CmdBindDescriptorSets(commands, 0, pipelineLayout, 0, 1, &set, 0, nullptr);
					const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					const Rect2D scissor{ { 0, 0 }, { frameWidth, frameHeight } };
					vk.CmdSetViewport(commands, 0, 1, &viewport);
					vk.CmdSetScissor(commands, 0, 1, &scissor);
					const DeviceSize vertexOffset = offset;
					vk.CmdBindVertexBuffers(commands, 0, 1, &frame.ring.buffer, &vertexOffset);
					vk.CmdBindIndexBuffer(commands, frame.ring.buffer, offset + vertexBytes, 0);
					vk.CmdDrawIndexed(commands, indexCount, 1, 0, 0, 0);
					if (msaaColor.image) frameUnresolved = true;
				}

				// ---- The ray tracer (RayScene.h, docs/game/ray-tracing.md): two compute passes over the scene uploaded into
				// this frame's ring (tracing at the result's size, then the edge-keeping blur), the result copied into the texture.
				bool CanRayTrace() const override {
					return !rayFailed && VulkanShaders::RayAoCompute[0] == SpirvMagic && VulkanShaders::RayBlurCompute[0] == SpirvMagic;
				}
				bool TraceAmbientOcclusion(const RaySceneData& scene, const RayTraceSettings& settings, std::uint32_t texture, std::uint32_t reflectionTexture,
					std::uint32_t bounceTexture) override {
					if (!CanRayTrace() || scene.nodeCount == 0 || settings.width == 0 || settings.height == 0) return false;
					if (!rayReady && !MakeRayObjects()) {
						rayFailed = true;
						return false;
					}
					const auto found = textures.find(texture);
					if (found == textures.end() || found->second.width != settings.width || found->second.height != settings.height) return false;
					const ImageResource image = found->second;
					// The reflections' texture: the same size, or none (then their buffer is filled but not copied).
					// The reflections' and the bounced light's textures: the same size, or none (then their buffers are filled
					// but not copied).
					const auto sameSize = [&](std::uint32_t handle) {
						const auto other = handle != 0 ? textures.find(handle) : textures.end();
						return other != textures.end() && other->second.width == settings.width && other->second.height == settings.height ? other->second : ImageResource{};
					};
					const ImageResource reflectionImage = sameSize(reflectionTexture);
					const ImageResource bounceImage = sameSize(bounceTexture);
					if (!MakeRayBuffers(settings.width, settings.height)) return false;

					// The tree, the triangles and the lights in one piece of the ring (one request, so a full ring cannot split
					// them). A buffer with nothing in it still gets a little room: a descriptor's range cannot be 0.
					const DeviceSize nodeBytes = static_cast<DeviceSize>(scene.nodeCount) * sizeof(RayNode);
					const DeviceSize triangleBytes = (std::max<DeviceSize>)(static_cast<DeviceSize>(scene.triangleCount) * sizeof(RayTriangle), 16);
					const DeviceSize lightBytes = (std::max<DeviceSize>)(static_cast<DeviceSize>(scene.lightCount) * sizeof(RayLight), 32);
					const DeviceSize triangleStart = AlignUp(nodeBytes, UniformAlignment);
					const DeviceSize lightStart = AlignUp(triangleStart + triangleBytes, UniformAlignment);
					// The set is taken first: when this frame's sets are used up, its work so far is sent and they start over,
					// which also starts the ring over, so the upload comes after.
					DescriptorSet set = 0;
					for (int attempt = 0; attempt < 2 && set == 0; ++attempt) {
						Commands();
						if (attempt > 0) FlushAndWait();
						DescriptorSetAllocateInfoData allocateInfo{ DescriptorSetAllocateInfo, nullptr, Slot().rayDescriptors, 1, &raySetLayout };
						if (vk.AllocateDescriptorSets(device, &allocateInfo, &set) != Success) set = 0;
					}
					if (set == 0) return false;
					// A ring that fills up here is sent and started over too (its piece then starts at 0), and the set with it:
					// the set is then taken again (at 0 without a restart, one set is spent for nothing).
					std::uint8_t* written = nullptr;
					const DeviceSize offset = RingAllocate(lightStart + lightBytes, UniformAlignment, written);
					if (!written) return false;
					if (offset == 0) {
						DescriptorSetAllocateInfoData again{ DescriptorSetAllocateInfo, nullptr, Slot().rayDescriptors, 1, &raySetLayout };
						if (vk.AllocateDescriptorSets(device, &again, &set) != Success) return false;
					}
					std::memcpy(written, scene.nodes, static_cast<std::size_t>(nodeBytes));
					std::memcpy(written + triangleStart, scene.triangles, static_cast<std::size_t>(scene.triangleCount) * sizeof(RayTriangle));
					std::memset(written + lightStart, 0, static_cast<std::size_t>(lightBytes));
					if (scene.lightCount > 0) std::memcpy(written + lightStart, scene.lights, static_cast<std::size_t>(scene.lightCount) * sizeof(RayLight));
					FrameSlot& frame = Slot();
					const DescriptorBufferInfo buffers[RayBindings] = {
						{ frame.ring.buffer, offset, nodeBytes },
						{ frame.ring.buffer, offset + triangleStart, triangleBytes },
						{ rayRaw.buffer, 0, rayRaw.size },
						{ rayResult.buffer, 0, rayResult.size },
						{ frame.ring.buffer, offset + lightStart, lightBytes },
						{},
						{ rayReflections.buffer, 0, rayReflections.size },
						{ rayBounceRaw.buffer, 0, rayBounceRaw.size },
						{ rayBounce.buffer, 0, rayBounce.size },
					};
					const DescriptorImageInfo frameImage{ raySampler, frameColor.view, LayoutShaderReadOnly };
					WriteDescriptorSetData writes[RayBindings]{};
					for (std::uint32_t binding = 0; binding < RayBindings; ++binding) {
						if (binding == RayFrameBinding) writes[binding] = { WriteDescriptorSet, nullptr, set, binding, 0, 1, DescriptorCombinedImageSampler, &frameImage, nullptr, nullptr };
						else writes[binding] = { WriteDescriptorSet, nullptr, set, binding, 0, 1, DescriptorStorageBuffer, nullptr, &buffers[binding], nullptr };
					}
					vk.UpdateDescriptorSets(device, RayBindings, writes, 0, nullptr);

					// The frame is read for reflections: all its draws resolved into frameColor (MSAA), which is lent to the
					// compute pass as a picture and given back after.
					Resolve();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutShaderReadOnly, AccessColorAttachmentWrite, AccessShaderRead,
						StageColorAttachmentOutput, StageComputeShader);
					// The last trace's copy and blur are done with these buffers before they are written again.
					MemoryBarrierData before{ MemoryBarrier, nullptr, AccessShaderRead | AccessShaderWrite | AccessTransferRead, AccessShaderWrite };
					vk.CmdPipelineBarrier(commands, StageComputeShader | StageTransfer, StageComputeShader, 0, 1, &before, 0, nullptr, 0, nullptr);
					const std::uint32_t groupsX = (settings.width + 7) / 8;
					const std::uint32_t groupsY = (settings.height + 7) / 8;
					vk.CmdBindPipeline(commands, BindPointCompute, rayPipelines[0]);
					vk.CmdBindDescriptorSets(commands, BindPointCompute, rayLayout, 0, 1, &set, 0, nullptr);
					vk.CmdPushConstants(commands, rayLayout, ShaderCompute, 0, sizeof(RayTraceSettings), &settings);
					vk.CmdDispatch(commands, groupsX, groupsY, 1);
					MemoryBarrierData traced{ MemoryBarrier, nullptr, AccessShaderWrite, AccessShaderRead };
					vk.CmdPipelineBarrier(commands, StageComputeShader, StageComputeShader, 0, 1, &traced, 0, nullptr, 0, nullptr);
					vk.CmdBindPipeline(commands, BindPointCompute, rayPipelines[1]);
					vk.CmdDispatch(commands, groupsX, groupsY, 1);
					MemoryBarrierData blurred{ MemoryBarrier, nullptr, AccessShaderWrite, AccessTransferRead };
					vk.CmdPipelineBarrier(commands, StageComputeShader, StageTransfer, 0, 1, &blurred, 0, nullptr, 0, nullptr);
					Barrier(commands, frameColor.image, AspectColor, LayoutShaderReadOnly, LayoutColorAttachment, AccessShaderRead, AccessColorAttachmentWrite,
						StageComputeShader, StageColorAttachmentOutput);
					Barrier(commands, image.image, AspectColor, LayoutShaderReadOnly, LayoutTransferDst, AccessShaderRead, AccessTransferWrite, StageFragmentShader, StageTransfer);
					CopyIn(commands, rayResult.buffer, 0, image);
					Barrier(commands, image.image, AspectColor, LayoutTransferDst, LayoutShaderReadOnly, AccessTransferWrite, AccessShaderRead, StageTransfer, StageFragmentShader);
					for (const auto& [from, into] : { std::pair{ rayReflections.buffer, reflectionImage }, std::pair{ rayBounce.buffer, bounceImage } }) {
						if (!into.image) continue;
						Barrier(commands, into.image, AspectColor, LayoutShaderReadOnly, LayoutTransferDst, AccessShaderRead, AccessTransferWrite, StageFragmentShader, StageTransfer);
						CopyIn(commands, from, 0, into);
						Barrier(commands, into.image, AspectColor, LayoutTransferDst, LayoutShaderReadOnly, AccessTransferWrite, AccessShaderRead, StageTransfer, StageFragmentShader);
					}
					return true;
				}

				void UploadScreen(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) override {
					if (!screen.image || screen.width != width || screen.height != height) {
						FlushAndWait();
						vk.DeviceWaitIdle(device);
						DestroyImage(screen);
						if (!MakeImage(screen, width, height, FormatB8G8R8A8Unorm, ImageUsageTransferSrc | ImageUsageTransferDst, AspectColor)) return;
						CommandBuffer commands = Commands();
						EndPass();
						Barrier(commands, screen.image, AspectColor, LayoutUndefined, LayoutTransferSrc, 0, AccessTransferRead, StageTopOfPipe, StageTransfer);
					}
					const DeviceSize size = static_cast<DeviceSize>(width) * height * 4;
					std::uint8_t* written = nullptr;
					const DeviceSize offset = RingAllocate(size, 16, written);
					if (!written) return;
					for (std::uint32_t y = 0; y < height; ++y) std::memcpy(written + static_cast<std::size_t>(y) * width * 4, static_cast<const std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(width) * 4);
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, screen.image, AspectColor, LayoutTransferSrc, LayoutTransferDst, AccessTransferRead, AccessTransferWrite, StageTransfer, StageTransfer);
					CopyIn(commands, Slot().ring.buffer, offset, screen);
					Barrier(commands, screen.image, AspectColor, LayoutTransferDst, LayoutTransferSrc, AccessTransferWrite, AccessTransferRead, StageTransfer, StageTransfer);
				}

				void Present(PresentSource source, const PresentRect& target, bool vsync) override {
					RECT client{};
					GetClientRect(window, &client);
					const std::uint32_t width = static_cast<std::uint32_t>(std::max<LONG>(client.right - client.left, 0));
					const std::uint32_t height = static_cast<std::uint32_t>(std::max<LONG>(client.bottom - client.top, 0));
					if (width == 0 || height == 0) return;
					if (!swapchain || swapOutdated || width != swapExtent.width || height != swapExtent.height || vsync != swapVsync) {
						swapVsync = vsync;
						if (!MakeSwapchain(true)) return;
					}
					// The frame resolved (MSAA) and smoothed (FXAA, SMAA) before anything of the present is recorded:
					// either may send the work so far when the frame's descriptor sets run out.
					bool smoothed = false;
					if (source == PresentSource::Frame && frameColor.image) {
						Resolve();
						if (outputFramebuffer) smoothed = RunPost();
					}
					CommandBuffer commands = Commands();
					EndPass();
					FrameSlot& frame = Slot();
					std::uint32_t index = 0;
					// Waits as long as it takes. A 100 ms limit was tried and the screen stayed black, though no call failed
					// (not proven to be the cause).
					// The freeze in the background is avoided by the layer, which shows nothing while the game is there.
					const Result acquired = vk.AcquireNextImageKHR(device, swapchain, Forever, frame.imageReady, 0, &index);
					if (acquired != Success) Report("getting a window image", acquired);
					if (acquired == Timeout || acquired == NotReady) return;
					if (acquired == ErrorOutOfDate) {
						swapOutdated = true;
						return;
					}
					if (acquired != Success && acquired != Suboptimal) return;
					const Image shown = swapImages[index];
					Barrier(commands, shown, AspectColor, LayoutUndefined, LayoutTransferDst, 0, AccessTransferWrite, StageTransfer, StageTransfer);
					ClearColorValue black{};
					black.float32[3] = 1.0f;
					const ImageSubresourceRange whole{ AspectColor, 0, 1, 0, 1 };
					vk.CmdClearColorImage(commands, shown, LayoutTransferDst, &black, 1, &whole);
					// Copies are not kept in order by themselves: without this the black could land after the picture.
					Barrier(commands, shown, AspectColor, LayoutTransferDst, LayoutTransferDst, AccessTransferWrite, AccessTransferWrite, StageTransfer, StageTransfer);
					// aaOutput is left ready to copy from by its pass.
					const ImageResource* from = source == PresentSource::Frame ? (smoothed ? &aaOutput : &frameColor) : &screen;
					if (from->image) {
						if (from == &frameColor) Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
						ImageBlit blit{};
						blit.srcSubresource = { AspectColor, 0, 0, 1 };
						blit.srcOffsets[1] = { static_cast<std::int32_t>(from->width), static_cast<std::int32_t>(from->height), 1 };
						blit.dstSubresource = { AspectColor, 0, 0, 1 };
						blit.dstOffsets[0] = { std::clamp<std::int32_t>(target.left, 0, static_cast<std::int32_t>(swapExtent.width)), std::clamp<std::int32_t>(target.top, 0, static_cast<std::int32_t>(swapExtent.height)), 0 };
						blit.dstOffsets[1] = { std::clamp<std::int32_t>(target.right, 0, static_cast<std::int32_t>(swapExtent.width)), std::clamp<std::int32_t>(target.bottom, 0, static_cast<std::int32_t>(swapExtent.height)), 1 };
						if (blit.dstOffsets[1].x > blit.dstOffsets[0].x && blit.dstOffsets[1].y > blit.dstOffsets[0].y) {
							vk.CmdBlitImage(commands, from->image, LayoutTransferSrc, shown, LayoutTransferDst, 1, &blit, 1);
						}
						if (from == &frameColor) Barrier(commands, frameColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					}
					Barrier(commands, shown, AspectColor, LayoutTransferDst, LayoutPresentSrc, AccessTransferWrite, 0, StageTransfer, StageBottomOfPipe);
					Submit(frame.imageReady, renderDone[index]);
					PresentInfoData present{ PresentInfoKHR, nullptr, 1, &renderDone[index], 1, &swapchain, &index, nullptr };
					const Result presented = vk.QueuePresentKHR(queue, &present);
					if (presented != Success) Report("showing a frame", presented);
					if (presented == ErrorOutOfDate || presented == Suboptimal) swapOutdated = true;
					current = (current + 1) % 2;
				}

			private:
				struct ImageResource {
					Image image = 0;
					DeviceMemory memory = 0;
					ImageView view = 0;
					std::uint32_t width = 0;
					std::uint32_t height = 0;
					std::uint32_t levels = 1;
				};
				struct BufferResource {
					Buffer buffer = 0;
					DeviceMemory memory = 0;
					void* mapped = nullptr;
					DeviceSize size = 0;
				};
				struct FrameSlot {
					CommandPool pool = 0;
					CommandBuffer commands = nullptr;
					Fence fence = 0;
					Semaphore imageReady = 0;
					BufferResource ring;
					DeviceSize ringUsed = 0;
					DescriptorPool descriptors = 0;
					std::uint32_t setsUsed = 0;
					// The ray tracer's sets (made with the tracer, reset with the frame).
					DescriptorPool rayDescriptors = 0;
					bool submitted = false;
					bool recording = false;
					std::vector<ImageResource> doomed;
				};

				HWND window = nullptr;
				HMODULE library = nullptr;
				GetInstanceProcAddrFn getInstanceProc = nullptr;
				Functions vk{};
				Instance instance = nullptr;
				PhysicalDevice physical = nullptr;
				Device device = nullptr;
				Queue queue = nullptr;
				std::uint32_t queueFamily = 0;
				PhysicalDeviceMemoryProperties memory{};
				SurfaceKHR surface = 0;
				SwapchainKHR swapchain = 0;
				std::vector<Image> swapImages;
				std::vector<Semaphore> renderDone;
				Extent2D swapExtent{};
				bool swapVsync = false;
				bool swapOutdated = false;
				Format depthFormat = FormatD32Sfloat;
				Flags depthAspect = AspectDepth;
				RenderPass renderPass = 0;
				DescriptorSetLayout setLayout = 0;
				PipelineLayout pipelineLayout = 0;
				ShaderModule vertexModule = 0;
				ShaderModule fragmentModule = 0;
				std::unordered_map<std::uint32_t, Pipeline> pipelines;
				Sampler samplers[SamplerKinds]{};
				// samplerAnisotropy was turned on for the device (texture_filtering).
				bool anisotropyEnabled = false;
				ImageResource frameColor;
				ImageResource frameDepth;
				ImageResource screen;
				Framebuffer framebuffer = 0;
				BufferResource readback;
				std::uint32_t frameWidth = 0;
				std::uint32_t frameHeight = 0;
				FrameSlot slots[2];
				std::uint32_t current = 0;
				bool inPass = false;
				std::unordered_map<std::uint32_t, ImageResource> textures;
				std::uint32_t nextTexture = 1;
				std::uint32_t whiteTexture = 0;

				// ---- The ray tracer: its layout, the two compute pipelines (trace, blur), and its buffers on the card (the
				// trace's per-pixel result and depth, and the blurred pixels copied into the texture), made for one size.
				bool rayReady = false;
				bool rayFailed = false;
				DescriptorSetLayout raySetLayout = 0;
				PipelineLayout rayLayout = 0;
				ShaderModule rayModules[2]{};
				Pipeline rayPipelines[2]{};
				struct DeviceBuffer {
					Buffer buffer = 0;
					DeviceMemory memory = 0;
					DeviceSize size = 0;
				};
				DeviceBuffer rayRaw;
				DeviceBuffer rayResult;
				DeviceBuffer rayReflections;
				DeviceBuffer rayBounceRaw;
				DeviceBuffer rayBounce;
				Sampler raySampler = 0;
				std::uint32_t rayWidth = 0;
				std::uint32_t rayHeight = 0;
				bool MakeRayObjects() {
					if (!LoadDevice(vk.CreateComputePipelines, "vkCreateComputePipelines") || !LoadDevice(vk.CmdDispatch, "vkCmdDispatch")) {
						Logger::Warning("Ray tracing: Vulkan has no compute entry points here; ray tracing is off.");
						return false;
					}
					DescriptorSetLayoutBinding bindings[RayBindings]{};
					for (std::uint32_t binding = 0; binding < RayBindings; ++binding) {
						bindings[binding] = { binding, binding == RayFrameBinding ? DescriptorCombinedImageSampler : DescriptorStorageBuffer, 1, ShaderCompute, nullptr };
					}
					// Linear, clamped: the frame read where reflections land.
					SamplerCreateInfoData samplerInfo{ SamplerCreateInfo, nullptr, 0, 1, 1, 0, 2, 2, 2, 0.0f, 0, 1.0f, 0, 0, 0.0f, 0.0f, 0, 0 };
					if (vk.CreateSampler(device, &samplerInfo, nullptr, &raySampler) != Success) {
						Logger::Warning("Ray tracing: Vulkan could not make the tracer's sampler; ray tracing is off.");
						return false;
					}
					DescriptorSetLayoutCreateInfoData layoutInfo{ DescriptorSetLayoutCreateInfo, nullptr, 0, RayBindings, bindings };
					const PushConstantRange constants{ ShaderCompute, 0, sizeof(RayTraceSettings) };
					PipelineLayoutCreateInfoData pipelineLayoutInfo{ PipelineLayoutCreateInfo, nullptr, 0, 1, &raySetLayout, 1, &constants };
					if (vk.CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &raySetLayout) != Success
						|| vk.CreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &rayLayout) != Success) {
						Logger::Warning("Ray tracing: Vulkan could not make the tracer's layout; ray tracing is off.");
						return false;
					}
					const struct { const std::uint32_t* words; std::size_t size; } code[2] = {
						{ VulkanShaders::RayAoCompute, sizeof(VulkanShaders::RayAoCompute) },
						{ VulkanShaders::RayBlurCompute, sizeof(VulkanShaders::RayBlurCompute) },
					};
					for (int i = 0; i < 2; ++i) {
						ShaderModuleCreateInfoData moduleInfo{ ShaderModuleCreateInfo, nullptr, 0, code[i].size, code[i].words };
						if (vk.CreateShaderModule(device, &moduleInfo, nullptr, &rayModules[i]) != Success) {
							Logger::Warning("Ray tracing: Vulkan could not load the tracer's shaders; ray tracing is off.");
							return false;
						}
						ComputePipelineCreateInfoData pipelineInfo{ ComputePipelineCreateInfo, nullptr, 0,
							{ PipelineShaderStageCreateInfo, nullptr, 0, ShaderCompute, rayModules[i], "main", nullptr }, rayLayout, 0, -1 };
						if (vk.CreateComputePipelines(device, 0, 1, &pipelineInfo, nullptr, &rayPipelines[i]) != Success) {
							Logger::Warning("Ray tracing: Vulkan could not make the tracer's pipelines; ray tracing is off.");
							return false;
						}
					}
					const DescriptorPoolSize sizes[2] = { { DescriptorStorageBuffer, (RayBindings - 1) * RaySetsPerFrame }, { DescriptorCombinedImageSampler, RaySetsPerFrame } };
					for (FrameSlot& slot : slots) {
						DescriptorPoolCreateInfoData poolInfo{ DescriptorPoolCreateInfo, nullptr, 0, RaySetsPerFrame, 2, sizes };
						if (vk.CreateDescriptorPool(device, &poolInfo, nullptr, &slot.rayDescriptors) != Success) {
							Logger::Warning("Ray tracing: Vulkan could not make the tracer's descriptor pools; ray tracing is off.");
							return false;
						}
					}
					rayReady = true;
					Logger::Info("Ray tracing: Vulkan's tracer is ready (compute shaders).");
					return true;
				}
				bool MakeDeviceBuffer(DeviceBuffer& out, DeviceSize size, Flags usage) {
					BufferCreateInfoData info{ BufferCreateInfo, nullptr, 0, size, usage, 0, 0, nullptr };
					if (vk.CreateBuffer(device, &info, nullptr, &out.buffer) != Success) return false;
					MemoryRequirements requirements{};
					vk.GetBufferMemoryRequirements(device, out.buffer, &requirements);
					const std::uint32_t type = FindMemory(requirements.memoryTypeBits, MemoryDeviceLocal, 0);
					MemoryAllocateInfoData allocate{ MemoryAllocateInfo, nullptr, requirements.size, type };
					if (type == ~0u || vk.AllocateMemory(device, &allocate, nullptr, &out.memory) != Success) {
						DestroyDeviceBuffer(out);
						return false;
					}
					vk.BindBufferMemory(device, out.buffer, out.memory, 0);
					out.size = size;
					return true;
				}
				void DestroyDeviceBuffer(DeviceBuffer& buffer) {
					if (buffer.buffer) vk.DestroyBuffer(device, buffer.buffer, nullptr);
					if (buffer.memory) vk.FreeMemory(device, buffer.memory, nullptr);
					buffer = {};
				}
				bool MakeRayBuffers(std::uint32_t width, std::uint32_t height) {
					if (width == rayWidth && height == rayHeight && rayRaw.buffer && rayResult.buffer && rayReflections.buffer && rayBounceRaw.buffer && rayBounce.buffer) return true;
					// The old ones may still be in use by frames in flight.
					FlushAndWait();
					vk.DeviceWaitIdle(device);
					DestroyDeviceBuffer(rayRaw);
					DestroyDeviceBuffer(rayResult);
					DestroyDeviceBuffer(rayReflections);
					DestroyDeviceBuffer(rayBounceRaw);
					DestroyDeviceBuffer(rayBounce);
					rayWidth = rayHeight = 0;
					const DeviceSize pixels = static_cast<DeviceSize>(width) * height;
					if (!MakeDeviceBuffer(rayRaw, pixels * 8, BufferUsageStorage) || !MakeDeviceBuffer(rayResult, pixels * 4, BufferUsageStorage | BufferUsageTransferSrc)
						|| !MakeDeviceBuffer(rayReflections, pixels * 4, BufferUsageStorage | BufferUsageTransferSrc)
						|| !MakeDeviceBuffer(rayBounceRaw, pixels * 16, BufferUsageStorage) || !MakeDeviceBuffer(rayBounce, pixels * 4, BufferUsageStorage | BufferUsageTransferSrc)) {
						Logger::Warning("Ray tracing: Vulkan could not make the tracer's buffers for " + std::to_string(width) + "x" + std::to_string(height) + ".");
						DestroyDeviceBuffer(rayRaw);
						DestroyDeviceBuffer(rayResult);
						DestroyDeviceBuffer(rayReflections);
						DestroyDeviceBuffer(rayBounceRaw);
						DestroyDeviceBuffer(rayBounce);
						return false;
					}
					rayWidth = width;
					rayHeight = height;
					return true;
				}
				void DestroyRayObjects() {
					DestroyDeviceBuffer(rayRaw);
					DestroyDeviceBuffer(rayResult);
					DestroyDeviceBuffer(rayReflections);
					DestroyDeviceBuffer(rayBounceRaw);
					DestroyDeviceBuffer(rayBounce);
					if (raySampler) vk.DestroySampler(device, raySampler, nullptr);
					raySampler = 0;
					for (Pipeline& pipeline : rayPipelines) {
						if (pipeline) vk.DestroyPipeline(device, pipeline, nullptr);
						pipeline = 0;
					}
					for (ShaderModule& module : rayModules) {
						if (module) vk.DestroyShaderModule(device, module, nullptr);
						module = 0;
					}
					if (rayLayout) vk.DestroyPipelineLayout(device, rayLayout, nullptr);
					if (raySetLayout) vk.DestroyDescriptorSetLayout(device, raySetLayout, nullptr);
					rayLayout = 0;
					raySetLayout = 0;
					for (FrameSlot& slot : slots) {
						if (slot.rayDescriptors) vk.DestroyDescriptorPool(device, slot.rayDescriptors, nullptr);
						slot.rayDescriptors = 0;
					}
					rayReady = false;
				}

				// ---- Anti-aliasing (see AntiAliasing in Backend.h).
				// MSAA: samples per pixel (1 = off), the multisampled color the draws go into, and whether it holds
				// draws frameColor does not have yet.
				std::uint32_t msaaSamples = 1;
				ImageResource msaaColor;
				// SaveShownFrame's copy of the frame (not anti-aliasing, but made with the frame).
				ImageResource shownColor;
				bool frameUnresolved = false;
				Pipeline copyPipeline = 0;
				// FXAA uses pass 0; SMAA passes 0-2 are edges, weights and blending.
				bool postShadersMissing = false;
				bool postReady = false;
				DescriptorSetLayout postSetLayout = 0;
				PipelineLayout postLayout = 0;
				Sampler linearSampler = 0;
				Sampler pointSampler = 0;
				std::vector<ShaderModule> postModules;
				Pipeline postPipelines[3]{};
				RenderPass outputPass = 0;
				RenderPass edgesPass = 0;
				RenderPass weightsPass = 0;
				ImageResource aaOutput;
				ImageResource smaaEdges;
				ImageResource smaaWeights;
				Framebuffer outputFramebuffer = 0;
				Framebuffer edgesFramebuffer = 0;
				Framebuffer weightsFramebuffer = 0;
				struct PostInput {
					ImageView view;
					Sampler sampler;
				};
				struct Spirv {
					const std::uint32_t* words;
					std::size_t size;
				};
				template<std::size_t Count> static Spirv Code(const std::uint32_t (&words)[Count]) { return { words, sizeof(words) }; }
				static bool Built(const Spirv& code) { return code.size >= 4 * 5 && code.words[0] == SpirvMagic; }

				// The shaders this setting needs: [0] vertex and [1] fragment of each pass in turn.
				std::vector<Spirv> PostCode() const {
					using namespace VulkanShaders;
					switch (antiAliasing.mode) {
						case AntiAliasingMode::Msaa: return { Code(PostVertex), Code(CopyFragment) };
						case AntiAliasingMode::Fxaa: return { Code(PostVertex), Code(FxaaFragment) };
						// The preset is in the constants (MakePostConstants), so one build of each pass serves them all.
						case AntiAliasingMode::Smaa:
							return {
								Code(PostVertex), Code(SmaaEdgeFragment),
								Code(PostVertex), Code(SmaaWeightFragment),
								Code(PostVertex), Code(SmaaBlendFragment),
							};
						default: return {};
					}
				}
				// Before the render pass is made (its sample count depends on it): turns MSAA down to what the card can
				// do, and everything off if its shaders are not built yet.
				void ChooseAntiAliasing() {
					if (antiAliasing.mode == AntiAliasingMode::Off) return;
					for (const Spirv& code : PostCode()) {
						if (!Built(code)) {
							postShadersMissing = true;
							antiAliasing = {};
							return;
						}
					}
					if (antiAliasing.mode != AntiAliasingMode::Msaa) return;
					std::uint32_t samples = antiAliasing.level;
					while (samples > 1 && !SupportsSamples(samples)) samples /= 2;
					if (samples > 1) {
						msaaSamples = samples;
						antiAliasing.level = samples;
					}
					else antiAliasing = {};
				}
				bool SupportsSamples(std::uint32_t samples) {
					ImageFormatProperties color{};
					ImageFormatProperties depth{};
					// Image type 2D, optimal tiling. VkSampleCountFlagBits has the bit of value n for n samples.
					return vk.GetPhysicalDeviceImageFormatProperties(physical, FormatB8G8R8A8Unorm, 1, 0, ImageUsageColorAttachment | ImageUsageTransferSrc, 0, &color) == Success
						&& (color.sampleCounts & samples) != 0
						&& vk.GetPhysicalDeviceImageFormatProperties(physical, depthFormat, 1, 0, ImageUsageDepthStencilAttachment, 0, &depth) == Success
						&& (depth.sampleCounts & samples) != 0;
				}
				ShaderModule MakeModule(const Spirv& code) {
					ShaderModuleCreateInfoData info{ ShaderModuleCreateInfo, nullptr, 0, code.size, code.words };
					ShaderModule made = 0;
					if (vk.CreateShaderModule(device, &info, nullptr, &made) != Success) return 0;
					postModules.push_back(made);
					return made;
				}
				// A pass drawing one color picture: cleared or not, left in finalLayout for what reads it next.
				bool MakeColorPass(Format format, ImageLayout finalLayout, bool clear, RenderPass& out) {
					const AttachmentDescription attachment{ 0, format, 1, clear ? 1 : 2, 0, 2, 1, LayoutUndefined, finalLayout };
					const AttachmentReference reference{ 0, LayoutColorAttachment };
					const SubpassDescription subpass{ 0, 0, 0, nullptr, 1, &reference, nullptr, nullptr, 0, nullptr };
					// After whatever read the picture last frame; before the passes and the copy that read it.
					const SubpassDependency dependencies[2] = {
						{ SubpassExternal, 0, StageColorAttachmentOutput | StageFragmentShader | StageTransfer, StageColorAttachmentOutput, 0, AccessColorAttachmentWrite, 0 },
						{ 0, SubpassExternal, StageColorAttachmentOutput, StageFragmentShader | StageTransfer, AccessColorAttachmentWrite, AccessShaderRead | AccessTransferRead, 0 },
					};
					RenderPassCreateInfoData info{ RenderPassCreateInfo, nullptr, 0, 1, &attachment, 1, &subpass, 2, dependencies };
					return vk.CreateRenderPass(device, &info, nullptr, &out) == Success;
				}
				// A pass of one triangle covering the target, with the post layout. In the frame's own pass (the MSAA
				// copy) its depth attachment is neither tested nor written.
				Pipeline MakePostPipeline(ShaderModule vertex, ShaderModule fragment, RenderPass pass, std::uint32_t samples) {
					if (!vertex || !fragment || !pass) return 0;
					const PipelineShaderStageCreateInfoData stages[] = {
						{ PipelineShaderStageCreateInfo, nullptr, 0, ShaderVertex, vertex, "main", nullptr },
						{ PipelineShaderStageCreateInfo, nullptr, 0, ShaderFragment, fragment, "main", nullptr },
					};
					PipelineVertexInputStateCreateInfoData vertexInput{ PipelineVertexInputStateCreateInfo, nullptr, 0, 0, nullptr, 0, nullptr };
					PipelineInputAssemblyStateCreateInfoData assembly{ PipelineInputAssemblyStateCreateInfo, nullptr, 0, 3, 0 };
					PipelineViewportStateCreateInfoData viewport{ PipelineViewportStateCreateInfo, nullptr, 0, 1, nullptr, 1, nullptr };
					PipelineRasterizationStateCreateInfoData raster{};
					raster.sType = PipelineRasterizationStateCreateInfo;
					raster.lineWidth = 1.0f;
					PipelineMultisampleStateCreateInfoData multisample{ PipelineMultisampleStateCreateInfo, nullptr, 0, samples, 0, 0.0f, nullptr, 0, 0 };
					PipelineDepthStencilStateCreateInfoData depth{};
					depth.sType = PipelineDepthStencilStateCreateInfo;
					depth.maxDepthBounds = 1.0f;
					PipelineColorBlendAttachmentState blend{ 0, 1, 0, 0, 1, 0, 0, 0xF };
					PipelineColorBlendStateCreateInfoData blending{ PipelineColorBlendStateCreateInfo, nullptr, 0, 0, 0, 1, &blend, { 0, 0, 0, 0 } };
					const std::int32_t dynamicStates[] = { 0, 1 };
					PipelineDynamicStateCreateInfoData dynamic{ PipelineDynamicStateCreateInfo, nullptr, 0, 2, dynamicStates };
					GraphicsPipelineCreateInfoData info{ GraphicsPipelineCreateInfo, nullptr, 0, 2, stages, &vertexInput, &assembly, nullptr, &viewport, &raster,
						&multisample, &depth, &blending, &dynamic, postLayout, pass, 0, 0, -1 };
					Pipeline made = 0;
					if (vk.CreateGraphicsPipelines(device, 0, 1, &info, nullptr, &made) != Success) return 0;
					return made;
				}
				// Everything the anti-aliasing needs that does not depend on the frame's size. False only when the
				// renderer cannot run at all (MSAA without its copy); FXAA or SMAA that cannot be made are turned off.
				bool MakePostObjects() {
					if (antiAliasing.mode == AntiAliasingMode::Off) return true;
					SamplerCreateInfoData samplerInfo{ SamplerCreateInfo, nullptr, 0, 1, 1, 0, 2, 2, 2, 0.0f, 0, 1.0f, 0, 0, 0.0f, 0.0f, 0, 0 };
					if (vk.CreateSampler(device, &samplerInfo, nullptr, &linearSampler) != Success) return Fail("could not make its anti-aliasing samplers");
					samplerInfo.magFilter = samplerInfo.minFilter = 0;
					if (vk.CreateSampler(device, &samplerInfo, nullptr, &pointSampler) != Success) return Fail("could not make its anti-aliasing samplers");
					const DescriptorSetLayoutBinding bindings[] = {
						{ 0, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
						{ 1, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
						{ 2, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
					};
					DescriptorSetLayoutCreateInfoData layoutInfo{ DescriptorSetLayoutCreateInfo, nullptr, 0, 3, bindings };
					if (vk.CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &postSetLayout) != Success) return Fail("could not make its anti-aliasing layout");
					const PushConstantRange constants{ ShaderVertex | ShaderFragment, 0, sizeof(PostConstants) };
					PipelineLayoutCreateInfoData pipelineLayoutInfo{ PipelineLayoutCreateInfo, nullptr, 0, 1, &postSetLayout, 1, &constants };
					if (vk.CreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &postLayout) != Success) return Fail("could not make its anti-aliasing layout");
					const std::vector<Spirv> code = PostCode();
					if (antiAliasing.mode == AntiAliasingMode::Msaa) {
						copyPipeline = MakePostPipeline(MakeModule(code[0]), MakeModule(code[1]), renderPass, msaaSamples);
						return copyPipeline != 0 || Fail("could not make its MSAA copy pipeline");
					}
					const bool smaa = antiAliasing.mode == AntiAliasingMode::Smaa;
					bool made = MakeColorPass(FormatB8G8R8A8Unorm, LayoutTransferSrc, false, outputPass);
					if (smaa) {
						made = made && MakeColorPass(FormatR8G8Unorm, LayoutShaderReadOnly, true, edgesPass) && MakeColorPass(FormatR8G8B8A8Unorm, LayoutShaderReadOnly, true, weightsPass);
						if (made) {
							postPipelines[0] = MakePostPipeline(MakeModule(code[0]), MakeModule(code[1]), edgesPass, 1);
							postPipelines[1] = MakePostPipeline(MakeModule(code[2]), MakeModule(code[3]), weightsPass, 1);
							postPipelines[2] = MakePostPipeline(MakeModule(code[4]), MakeModule(code[5]), outputPass, 1);
						}
						made = made && postPipelines[0] && postPipelines[1] && postPipelines[2];
					}
					else {
						if (made) postPipelines[0] = MakePostPipeline(MakeModule(code[0]), MakeModule(code[1]), outputPass, 1);
						made = made && postPipelines[0];
					}
					if (made) postReady = true;
					else {
						Logger::Warning("Renderer: Vulkan could not set up " + AntiAliasingName(antiAliasing) + ".");
						antiAliasing = {};
					}
					return true;
				}
				bool MakePostTargets() {
					const auto framebufferFor = [&](RenderPass pass, const ImageResource& image, Framebuffer& out) {
						FramebufferCreateInfoData info{ FramebufferCreateInfo, nullptr, 0, pass, 1, &image.view, frameWidth, frameHeight, 1 };
						return vk.CreateFramebuffer(device, &info, nullptr, &out) == Success;
					};
					if (!MakeImage(aaOutput, frameWidth, frameHeight, FormatB8G8R8A8Unorm, ImageUsageColorAttachment | ImageUsageTransferSrc, AspectColor)) return false;
					if (antiAliasing.mode == AntiAliasingMode::Smaa) {
						if (!MakeImage(smaaEdges, frameWidth, frameHeight, FormatR8G8Unorm, ImageUsageColorAttachment | ImageUsageSampled, AspectColor)
							|| !MakeImage(smaaWeights, frameWidth, frameHeight, FormatR8G8B8A8Unorm, ImageUsageColorAttachment | ImageUsageSampled, AspectColor)
							|| !framebufferFor(edgesPass, smaaEdges, edgesFramebuffer) || !framebufferFor(weightsPass, smaaWeights, weightsFramebuffer)) {
							return false;
						}
					}
					// Last: Present takes a framebuffer here as the sign that everything is ready.
					return framebufferFor(outputPass, aaOutput, outputFramebuffer);
				}
				void DestroyPostTargets() {
					for (Framebuffer* made : { &outputFramebuffer, &edgesFramebuffer, &weightsFramebuffer }) {
						if (*made) vk.DestroyFramebuffer(device, *made, nullptr);
						*made = 0;
					}
					DestroyImage(aaOutput);
					DestroyImage(smaaEdges);
					DestroyImage(smaaWeights);
				}
				DescriptorSet PostSet(const PostInput (&inputs)[3]) {
					FrameSlot& frame = Slot();
					DescriptorSet set = 0;
					DescriptorSetAllocateInfoData allocateInfo{ DescriptorSetAllocateInfo, nullptr, frame.descriptors, 1, &postSetLayout };
					if (vk.AllocateDescriptorSets(device, &allocateInfo, &set) != Success) return 0;
					++frame.setsUsed;
					DescriptorImageInfo images[3];
					for (int i = 0; i < 3; ++i) images[i] = { inputs[i].sampler, inputs[i].view, LayoutShaderReadOnly };
					// Three in a row from binding 0 fill bindings 0-2.
					WriteDescriptorSetData write{ WriteDescriptorSet, nullptr, set, 0, 0, 3, DescriptorCombinedImageSampler, images, nullptr, nullptr };
					vk.UpdateDescriptorSets(device, 1, &write, 0, nullptr);
					return set;
				}
				void FrameViewport(CommandBuffer commands) {
					const Viewport viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					const Rect2D scissor{ { 0, 0 }, { frameWidth, frameHeight } };
					vk.CmdSetViewport(commands, 0, 1, &viewport);
					vk.CmdSetScissor(commands, 0, 1, &scissor);
				}
				void PostPass(CommandBuffer commands, RenderPass pass, Framebuffer target, Pipeline pipeline, const PostInput (&inputs)[3], const PostConstants& constants) {
					const DescriptorSet set = PostSet(inputs);
					if (!set) return;
					ClearValue clear{};
					RenderPassBeginInfoData begin{ RenderPassBeginInfo, nullptr, pass, target, { { 0, 0 }, { frameWidth, frameHeight } }, 1, &clear };
					vk.CmdBeginRenderPass(commands, &begin, 0);
					vk.CmdBindPipeline(commands, 0, pipeline);
					vk.CmdBindDescriptorSets(commands, 0, postLayout, 0, 1, &set, 0, nullptr);
					FrameViewport(commands);
					vk.CmdPushConstants(commands, postLayout, ShaderVertex | ShaderFragment, 0, sizeof(PostConstants), &constants);
					vk.CmdDraw(commands, 3, 1, 0, 0);
					vk.CmdEndRenderPass(commands);
				}
				// FXAA or SMAA from frameColor into aaOutput, which its pass leaves ready to copy from.
				bool RunPost() {
					if (Slot().setsUsed + 3 > SetsPerFrame) FlushAndWait();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutShaderReadOnly, AccessColorAttachmentWrite | AccessTransferWrite, AccessShaderRead,
						StageColorAttachmentOutput | StageTransfer, StageFragmentShader);
					const PostConstants constants = MakePostConstants(antiAliasing, frameWidth, frameHeight);
					const PostInput frameLinear{ frameColor.view, linearSampler };
					const PostInput framePoint{ frameColor.view, pointSampler };
					if (antiAliasing.mode == AntiAliasingMode::Smaa) {
						PostPass(commands, edgesPass, edgesFramebuffer, postPipelines[0], { frameLinear, frameLinear, frameLinear }, constants);
						const PostInput edges{ smaaEdges.view, pointSampler };
						PostPass(commands, weightsPass, weightsFramebuffer, postPipelines[1], { edges, edges, edges }, constants);
						PostPass(commands, outputPass, outputFramebuffer, postPipelines[2], { frameLinear, { smaaWeights.view, linearSampler }, { smaaWeights.view, linearSampler } }, constants);
					}
					else PostPass(commands, outputPass, outputFramebuffer, postPipelines[0], { frameLinear, framePoint, framePoint }, constants);
					Barrier(commands, frameColor.image, AspectColor, LayoutShaderReadOnly, LayoutColorAttachment, AccessShaderRead, AccessColorAttachmentWrite, StageFragmentShader, StageColorAttachmentOutput);
					return true;
				}
				// MSAA: brings frameColor up to date with the draws. Both are left as color attachments.
				void Resolve() {
					if (!msaaColor.image || !frameUnresolved) return;
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, msaaColor.image, AspectColor, LayoutColorAttachment, LayoutTransferSrc, AccessColorAttachmentWrite, AccessTransferRead, StageColorAttachmentOutput, StageTransfer);
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutTransferDst, AccessColorAttachmentWrite | AccessShaderRead, AccessTransferWrite,
						StageColorAttachmentOutput | StageFragmentShader | StageTransfer, StageTransfer);
					const ImageResolve region{ { AspectColor, 0, 0, 1 }, { 0, 0, 0 }, { AspectColor, 0, 0, 1 }, { 0, 0, 0 }, { frameWidth, frameHeight, 1 } };
					vk.CmdResolveImage(commands, msaaColor.image, LayoutTransferSrc, frameColor.image, LayoutTransferDst, 1, &region);
					Barrier(commands, msaaColor.image, AspectColor, LayoutTransferSrc, LayoutColorAttachment, AccessTransferRead, AccessColorAttachmentWrite, StageTransfer, StageColorAttachmentOutput);
					Barrier(commands, frameColor.image, AspectColor, LayoutTransferDst, LayoutColorAttachment, AccessTransferWrite, AccessColorAttachmentWrite | AccessTransferRead,
						StageTransfer, StageColorAttachmentOutput | StageTransfer);
					frameUnresolved = false;
				}
				// MSAA: frameColor (just filled from memory) into every sample of the multisampled frame.
				void CopyIntoMultisampled() {
					if (Slot().setsUsed + 1 > SetsPerFrame) FlushAndWait();
					CommandBuffer commands = Commands();
					EndPass();
					Barrier(commands, frameColor.image, AspectColor, LayoutColorAttachment, LayoutShaderReadOnly, AccessColorAttachmentWrite | AccessTransferWrite, AccessShaderRead,
						StageColorAttachmentOutput | StageTransfer, StageFragmentShader);
					const PostInput framePoint{ frameColor.view, pointSampler };
					const DescriptorSet set = PostSet({ framePoint, framePoint, framePoint });
					if (set) {
						const PostConstants constants = MakePostConstants(antiAliasing, frameWidth, frameHeight);
						BeginPass();
						vk.CmdBindPipeline(commands, 0, copyPipeline);
						vk.CmdBindDescriptorSets(commands, 0, postLayout, 0, 1, &set, 0, nullptr);
						FrameViewport(commands);
						vk.CmdPushConstants(commands, postLayout, ShaderVertex | ShaderFragment, 0, sizeof(PostConstants), &constants);
						vk.CmdDraw(commands, 3, 1, 0, 0);
						EndPass();
					}
					Barrier(commands, frameColor.image, AspectColor, LayoutShaderReadOnly, LayoutColorAttachment, AccessShaderRead, AccessColorAttachmentWrite, StageFragmentShader, StageColorAttachmentOutput);
					frameUnresolved = false;
				}

				bool Fail(const std::string& what) {
					Logger::Warning("Renderer: Vulkan " + what + ".");
					return false;
				}
				// A failed call while running, logged the first few times for each kind.
				std::unordered_map<std::string, int> reported;
				void Report(const std::string& what, Result result) {
					if (++reported[what] > 3) return;
					Logger::Warning("Renderer: Vulkan failed " + what + " (VkResult " + std::to_string(static_cast<int>(result)) + ").");
				}
				template<typename T> bool LoadGlobal(T& target, const char* name) {
					target = reinterpret_cast<T>(getInstanceProc(nullptr, name));
					return target != nullptr;
				}
				template<typename T> bool LoadInstance(T& target, const char* name) {
					target = reinterpret_cast<T>(getInstanceProc(instance, name));
					return target != nullptr;
				}
				template<typename T> bool LoadDevice(T& target, const char* name) {
					target = reinterpret_cast<T>(vk.GetDeviceProcAddr(device, name));
					return target != nullptr;
				}
				bool LoadInstanceFunctions() {
					return LoadInstance(vk.DestroyInstance, "vkDestroyInstance") && LoadInstance(vk.EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices")
						&& LoadInstance(vk.GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties")
						&& LoadInstance(vk.GetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures")
						&& LoadInstance(vk.GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties")
						&& LoadInstance(vk.GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties")
						&& LoadInstance(vk.GetPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties")
						&& LoadInstance(vk.GetPhysicalDeviceImageFormatProperties, "vkGetPhysicalDeviceImageFormatProperties")
						&& LoadInstance(vk.GetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR")
						&& LoadInstance(vk.GetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")
						&& LoadInstance(vk.GetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR")
						&& LoadInstance(vk.GetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR")
						&& LoadInstance(vk.CreateWin32SurfaceKHR, "vkCreateWin32SurfaceKHR") && LoadInstance(vk.DestroySurfaceKHR, "vkDestroySurfaceKHR")
						&& LoadInstance(vk.CreateDevice, "vkCreateDevice") && LoadInstance(vk.GetDeviceProcAddr, "vkGetDeviceProcAddr");
				}
				bool LoadDeviceFunctions() {
					return LoadDevice(vk.DestroyDevice, "vkDestroyDevice") && LoadDevice(vk.GetDeviceQueue, "vkGetDeviceQueue")
						&& LoadDevice(vk.CreateSwapchainKHR, "vkCreateSwapchainKHR") && LoadDevice(vk.DestroySwapchainKHR, "vkDestroySwapchainKHR")
						&& LoadDevice(vk.GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR") && LoadDevice(vk.AcquireNextImageKHR, "vkAcquireNextImageKHR")
						&& LoadDevice(vk.QueuePresentKHR, "vkQueuePresentKHR") && LoadDevice(vk.QueueSubmit, "vkQueueSubmit")
						&& LoadDevice(vk.DeviceWaitIdle, "vkDeviceWaitIdle") && LoadDevice(vk.CreateCommandPool, "vkCreateCommandPool")
						&& LoadDevice(vk.DestroyCommandPool, "vkDestroyCommandPool") && LoadDevice(vk.AllocateCommandBuffers, "vkAllocateCommandBuffers")
						&& LoadDevice(vk.ResetCommandBuffer, "vkResetCommandBuffer") && LoadDevice(vk.BeginCommandBuffer, "vkBeginCommandBuffer")
						&& LoadDevice(vk.EndCommandBuffer, "vkEndCommandBuffer") && LoadDevice(vk.CreateFence, "vkCreateFence")
						&& LoadDevice(vk.DestroyFence, "vkDestroyFence") && LoadDevice(vk.WaitForFences, "vkWaitForFences")
						&& LoadDevice(vk.ResetFences, "vkResetFences") && LoadDevice(vk.CreateSemaphoreObject, "vkCreateSemaphore")
						&& LoadDevice(vk.DestroySemaphore, "vkDestroySemaphore") && LoadDevice(vk.CreateImage, "vkCreateImage")
						&& LoadDevice(vk.DestroyImage, "vkDestroyImage") && LoadDevice(vk.GetImageMemoryRequirements, "vkGetImageMemoryRequirements")
						&& LoadDevice(vk.AllocateMemory, "vkAllocateMemory") && LoadDevice(vk.FreeMemory, "vkFreeMemory")
						&& LoadDevice(vk.BindImageMemory, "vkBindImageMemory") && LoadDevice(vk.CreateImageView, "vkCreateImageView")
						&& LoadDevice(vk.DestroyImageView, "vkDestroyImageView") && LoadDevice(vk.CreateBuffer, "vkCreateBuffer")
						&& LoadDevice(vk.DestroyBuffer, "vkDestroyBuffer") && LoadDevice(vk.GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
						&& LoadDevice(vk.BindBufferMemory, "vkBindBufferMemory") && LoadDevice(vk.MapMemory, "vkMapMemory")
						&& LoadDevice(vk.UnmapMemory, "vkUnmapMemory") && LoadDevice(vk.CreateSampler, "vkCreateSampler")
						&& LoadDevice(vk.DestroySampler, "vkDestroySampler") && LoadDevice(vk.CreateRenderPass, "vkCreateRenderPass")
						&& LoadDevice(vk.DestroyRenderPass, "vkDestroyRenderPass") && LoadDevice(vk.CreateFramebuffer, "vkCreateFramebuffer")
						&& LoadDevice(vk.DestroyFramebuffer, "vkDestroyFramebuffer") && LoadDevice(vk.CreateShaderModule, "vkCreateShaderModule")
						&& LoadDevice(vk.DestroyShaderModule, "vkDestroyShaderModule") && LoadDevice(vk.CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout")
						&& LoadDevice(vk.DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") && LoadDevice(vk.CreatePipelineLayout, "vkCreatePipelineLayout")
						&& LoadDevice(vk.DestroyPipelineLayout, "vkDestroyPipelineLayout") && LoadDevice(vk.CreateGraphicsPipelines, "vkCreateGraphicsPipelines")
						&& LoadDevice(vk.DestroyPipeline, "vkDestroyPipeline") && LoadDevice(vk.CreateDescriptorPool, "vkCreateDescriptorPool")
						&& LoadDevice(vk.DestroyDescriptorPool, "vkDestroyDescriptorPool") && LoadDevice(vk.ResetDescriptorPool, "vkResetDescriptorPool")
						&& LoadDevice(vk.AllocateDescriptorSets, "vkAllocateDescriptorSets") && LoadDevice(vk.UpdateDescriptorSets, "vkUpdateDescriptorSets")
						&& LoadDevice(vk.CmdBeginRenderPass, "vkCmdBeginRenderPass") && LoadDevice(vk.CmdEndRenderPass, "vkCmdEndRenderPass")
						&& LoadDevice(vk.CmdBindPipeline, "vkCmdBindPipeline") && LoadDevice(vk.CmdBindDescriptorSets, "vkCmdBindDescriptorSets")
						&& LoadDevice(vk.CmdBindVertexBuffers, "vkCmdBindVertexBuffers") && LoadDevice(vk.CmdBindIndexBuffer, "vkCmdBindIndexBuffer")
						&& LoadDevice(vk.CmdDrawIndexed, "vkCmdDrawIndexed") && LoadDevice(vk.CmdSetViewport, "vkCmdSetViewport")
						&& LoadDevice(vk.CmdSetScissor, "vkCmdSetScissor") && LoadDevice(vk.CmdPipelineBarrier, "vkCmdPipelineBarrier")
						&& LoadDevice(vk.CmdCopyBufferToImage, "vkCmdCopyBufferToImage") && LoadDevice(vk.CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer")
						&& LoadDevice(vk.CmdBlitImage, "vkCmdBlitImage") && LoadDevice(vk.CmdClearColorImage, "vkCmdClearColorImage")
						&& LoadDevice(vk.CmdClearAttachments, "vkCmdClearAttachments") && LoadDevice(vk.CmdDraw, "vkCmdDraw")
						&& LoadDevice(vk.CmdPushConstants, "vkCmdPushConstants") && LoadDevice(vk.CmdResolveImage, "vkCmdResolveImage");
				}

				// A graphics queue that can show on the game's window, on a discrete GPU if there is one.
				bool PickDevice() {
					std::uint32_t count = 0;
					vk.EnumeratePhysicalDevices(instance, &count, nullptr);
					std::vector<PhysicalDevice> devices(count);
					if (count == 0 || vk.EnumeratePhysicalDevices(instance, &count, devices.data()) != Success) return Fail("found no GPU");
					int bestScore = -1;
					for (PhysicalDevice candidate : devices) {
						std::uint32_t familyCount = 0;
						vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
						std::vector<QueueFamilyProperties> families(familyCount);
						vk.GetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
						for (std::uint32_t family = 0; family < familyCount; ++family) {
							Bool32 presents = 0;
							vk.GetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface, &presents);
							if ((families[family].queueFlags & QueueGraphics) == 0 || !presents) continue;
							// VkPhysicalDeviceProperties: deviceType at +16, deviceName at +20.
							alignas(8) std::uint8_t properties[2048]{};
							vk.GetPhysicalDeviceProperties(candidate, properties);
							std::uint32_t type = 0;
							std::memcpy(&type, properties + 16, 4);
							const int score = type == PhysicalDeviceDiscrete ? 2 : 1;
							if (score > bestScore) {
								bestScore = score;
								physical = candidate;
								queueFamily = family;
								deviceName.assign(reinterpret_cast<const char*>(properties + 20), strnlen(reinterpret_cast<const char*>(properties + 20), 256));
							}
							break;
						}
					}
					if (!physical) return Fail("found no GPU that can show on the game's window");
					Logger::Info("Renderer: Vulkan uses " + deviceName + ".");
					if (StringUtils::ToLower(Config::GetString("ray_tracing", "off")) == "on") ProbeRayTracing();
					return true;
				}
				// ray_tracing: on (first step): whether this 32-bit process gets the card's ray tracing through Vulkan. Only reads
				// and logs; nothing is turned on.
				void ProbeRayTracing() {
					// VkPhysicalDeviceProperties: apiVersion at +0, driverVersion at +4. VkExtensionProperties: a 256-character name,
					// then the spec version.
					alignas(8) std::uint8_t properties[2048]{};
					vk.GetPhysicalDeviceProperties(physical, properties);
					std::uint32_t apiVersion = 0;
					std::memcpy(&apiVersion, properties, 4);
					using EnumerateExtensionsFn = std::int32_t(__stdcall*)(PhysicalDevice, const char*, std::uint32_t*, void*);
					const auto enumerate = reinterpret_cast<EnumerateExtensionsFn>(getInstanceProc(instance, "vkEnumerateDeviceExtensionProperties"));
					if (enumerate == nullptr) {
						Logger::Warning("Ray tracing: Vulkan's loader has no vkEnumerateDeviceExtensionProperties; cannot check.");
						return;
					}
					constexpr std::size_t ExtensionSize = 260;
					std::uint32_t count = 0;
					enumerate(physical, nullptr, &count, nullptr);
					std::vector<std::uint8_t> extensions(static_cast<std::size_t>(count) * ExtensionSize);
					if (count == 0 || enumerate(physical, nullptr, &count, extensions.data()) != Success) {
						Logger::Warning("Ray tracing: could not list the Vulkan device's extensions.");
						return;
					}
					const auto has = [&](const char* name) {
						for (std::uint32_t i = 0; i < count; ++i) {
							if (std::strncmp(reinterpret_cast<const char*>(extensions.data() + i * ExtensionSize), name, 256) == 0) return true;
						}
						return false;
					};
					const char* wanted[] = { "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_ray_tracing_pipeline",
						"VK_KHR_deferred_host_operations", "VK_KHR_buffer_device_address", "VK_KHR_spirv_1_4", "VK_EXT_descriptor_indexing" };
					std::string found;
					std::string missing;
					for (const char* name : wanted) {
						auto& list = has(name) ? found : missing;
						if (!list.empty()) list += ", ";
						list += name;
					}
					const bool usable = has("VK_KHR_acceleration_structure") && has("VK_KHR_ray_query") && has("VK_KHR_deferred_host_operations");
					Logger::Info("Ray tracing: Vulkan " + std::to_string(apiVersion >> 22) + "." + std::to_string((apiVersion >> 12) & 0x3FF) + " on " + deviceName
						+ " (32-bit process), " + std::to_string(count) + " device extensions. Has: " + (found.empty() ? "none" : found) + ". Missing: "
						+ (missing.empty() ? "none" : missing) + ".");
					Logger::Info(std::string("Ray tracing: ") + (usable ? "this card can ray trace for KeeperForge2 through Vulkan (ray queries)."
						: "this card or driver does not offer ray queries to this 32-bit game through Vulkan."));
				}
				std::string deviceName;
				void PickDepthFormat() {
					const std::pair<Format, Flags> candidates[] = {
						{ FormatD32Sfloat, AspectDepth }, { FormatX8D24UnormPack32, AspectDepth }, { FormatD24UnormS8Uint, AspectDepth | AspectStencil }, { FormatD16Unorm, AspectDepth },
					};
					for (const auto& [format, aspect] : candidates) {
						FormatProperties properties{};
						vk.GetPhysicalDeviceFormatProperties(physical, format, &properties);
						if ((properties.optimalTilingFeatures & FormatFeatureDepthStencilAttachment) != 0) {
							depthFormat = format;
							depthAspect = aspect;
							return;
						}
					}
				}
				std::uint32_t FindMemory(std::uint32_t allowed, Flags required, Flags preferred) const {
					for (int pass = 0; pass < 2; ++pass) {
						const Flags wanted = pass == 0 ? required | preferred : required;
						for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
							if ((allowed & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
						}
					}
					return ~0u;
				}
				bool MakeImage(ImageResource& out, std::uint32_t width, std::uint32_t height, Format format, Flags usage, Flags aspect, std::uint32_t samples = 1,
					std::uint32_t levels = 1) {
					if (levels < 1) levels = 1;
					ImageCreateInfoData info{ ImageCreateInfo, nullptr, 0, 1, format, { width, height, 1 }, levels, 1, samples, 0, usage, 0, 0, nullptr, LayoutUndefined };
					if (vk.CreateImage(device, &info, nullptr, &out.image) != Success) return false;
					MemoryRequirements requirements{};
					vk.GetImageMemoryRequirements(device, out.image, &requirements);
					const std::uint32_t type = FindMemory(requirements.memoryTypeBits, MemoryDeviceLocal, 0);
					MemoryAllocateInfoData allocate{ MemoryAllocateInfo, nullptr, requirements.size, type };
					if (type == ~0u || vk.AllocateMemory(device, &allocate, nullptr, &out.memory) != Success) {
						DestroyImage(out);
						return false;
					}
					vk.BindImageMemory(device, out.image, out.memory, 0);
					ImageViewCreateInfoData view{ ImageViewCreateInfo, nullptr, 0, out.image, 1, format, { 0, 0, 0, 0 }, { aspect, 0, levels, 0, 1 } };
					if (vk.CreateImageView(device, &view, nullptr, &out.view) != Success) {
						DestroyImage(out);
						return false;
					}
					out.width = width;
					out.height = height;
					out.levels = levels;
					return true;
				}
				void DestroyImage(ImageResource& image) {
					if (image.view) vk.DestroyImageView(device, image.view, nullptr);
					if (image.image) vk.DestroyImage(device, image.image, nullptr);
					if (image.memory) vk.FreeMemory(device, image.memory, nullptr);
					image = {};
				}
				bool MakeBuffer(BufferResource& out, DeviceSize size, Flags usage, Flags required, Flags preferred) {
					BufferCreateInfoData info{ BufferCreateInfo, nullptr, 0, size, usage, 0, 0, nullptr };
					if (vk.CreateBuffer(device, &info, nullptr, &out.buffer) != Success) return false;
					MemoryRequirements requirements{};
					vk.GetBufferMemoryRequirements(device, out.buffer, &requirements);
					const std::uint32_t type = FindMemory(requirements.memoryTypeBits, required, preferred);
					MemoryAllocateInfoData allocate{ MemoryAllocateInfo, nullptr, requirements.size, type };
					if (type == ~0u || vk.AllocateMemory(device, &allocate, nullptr, &out.memory) != Success) {
						DestroyBuffer(out);
						return false;
					}
					vk.BindBufferMemory(device, out.buffer, out.memory, 0);
					if (vk.MapMemory(device, out.memory, 0, size, 0, &out.mapped) != Success) {
						DestroyBuffer(out);
						return false;
					}
					out.size = size;
					return true;
				}
				void DestroyBuffer(BufferResource& buffer) {
					if (buffer.mapped) vk.UnmapMemory(device, buffer.memory);
					if (buffer.buffer) vk.DestroyBuffer(device, buffer.buffer, nullptr);
					if (buffer.memory) vk.FreeMemory(device, buffer.memory, nullptr);
					buffer = {};
				}

				bool MakePipelineObjects() {
					// With MSAA both attachments are multisampled (frameColor then only holds the frame resolved).
					const AttachmentDescription attachments[] = {
						{ 0, FormatB8G8R8A8Unorm, msaaSamples, 0, 0, 2, 1, LayoutColorAttachment, LayoutColorAttachment },
						{ 0, depthFormat, msaaSamples, 0, 0, 0, 0, LayoutDepthStencilAttachment, LayoutDepthStencilAttachment },
					};
					const AttachmentReference color{ 0, LayoutColorAttachment };
					const AttachmentReference depth{ 1, LayoutDepthStencilAttachment };
					const SubpassDescription subpass{ 0, 0, 0, nullptr, 1, &color, nullptr, &depth, 0, nullptr };
					// Work before the pass (copies into the frame, earlier passes) is done before it draws.
					const SubpassDependency dependency{ SubpassExternal, 0, StageTransfer | StageColorAttachmentOutput | StageLateFragmentTests,
						StageColorAttachmentOutput | StageEarlyFragmentTests | StageFragmentShader, AccessTransferWrite | AccessColorAttachmentWrite | AccessDepthStencilWrite,
						AccessColorAttachmentRead | AccessColorAttachmentWrite | AccessDepthStencilRead | AccessDepthStencilWrite | AccessShaderRead, 0 };
					RenderPassCreateInfoData passInfo{ RenderPassCreateInfo, nullptr, 0, 2, attachments, 1, &subpass, 1, &dependency };
					if (vk.CreateRenderPass(device, &passInfo, nullptr, &renderPass) != Success) return Fail("could not make its render pass");
					// texture0-2 are bindings 1-3; one descriptor write of three fills them in order.
					const DescriptorSetLayoutBinding bindings[] = {
						{ 0, DescriptorUniformBuffer, 1, ShaderVertex | ShaderFragment, nullptr },
						{ 1, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
						{ 2, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
						{ 3, DescriptorCombinedImageSampler, 1, ShaderFragment, nullptr },
					};
					DescriptorSetLayoutCreateInfoData layoutInfo{ DescriptorSetLayoutCreateInfo, nullptr, 0, 4, bindings };
					if (vk.CreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != Success) return Fail("could not make its descriptor layout");
					PipelineLayoutCreateInfoData pipelineLayoutInfo{ PipelineLayoutCreateInfo, nullptr, 0, 1, &setLayout, 0, nullptr };
					if (vk.CreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != Success) return Fail("could not make its pipeline layout");
					ShaderModuleCreateInfoData vertexInfo{ ShaderModuleCreateInfo, nullptr, 0, sizeof(VulkanShaders::DrawVertex), VulkanShaders::DrawVertex };
					ShaderModuleCreateInfoData fragmentInfo{ ShaderModuleCreateInfo, nullptr, 0, sizeof(VulkanShaders::DrawFragment), VulkanShaders::DrawFragment };
					if (vk.CreateShaderModule(device, &vertexInfo, nullptr, &vertexModule) != Success || vk.CreateShaderModule(device, &fragmentInfo, nullptr, &fragmentModule) != Success) {
						return Fail("could not load its shaders");
					}
					return true;
				}
				bool MakeFrameSlots() {
					for (FrameSlot& frame : slots) {
						CommandPoolCreateInfoData poolInfo{ CommandPoolCreateInfo, nullptr, 2, queueFamily };
						if (vk.CreateCommandPool(device, &poolInfo, nullptr, &frame.pool) != Success) return Fail("could not make a command pool");
						CommandBufferAllocateInfoData allocate{ CommandBufferAllocateInfo, nullptr, frame.pool, 0, 1 };
						if (vk.AllocateCommandBuffers(device, &allocate, &frame.commands) != Success) return Fail("could not make a command buffer");
						FenceCreateInfoData fenceInfo{ FenceCreateInfo, nullptr, 0 };
						SemaphoreCreateInfoData semaphoreInfo{ SemaphoreCreateInfo, nullptr, 0 };
						if (vk.CreateFence(device, &fenceInfo, nullptr, &frame.fence) != Success || vk.CreateSemaphoreObject(device, &semaphoreInfo, nullptr, &frame.imageReady) != Success) {
							return Fail("could not make its sync objects");
						}
						if (!MakeBuffer(frame.ring, RingSize, BufferUsageVertex | BufferUsageIndex | BufferUsageUniform | BufferUsageStorage | BufferUsageTransferSrc, MemoryHostVisible | MemoryHostCoherent, 0)) {
							return Fail("could not make its upload buffers");
						}
						const DescriptorPoolSize sizes[] = { { DescriptorUniformBuffer, SetsPerFrame }, { DescriptorCombinedImageSampler, SetsPerFrame * 3 } };
						DescriptorPoolCreateInfoData descriptorInfo{ DescriptorPoolCreateInfo, nullptr, 0, SetsPerFrame, 2, sizes };
						if (vk.CreateDescriptorPool(device, &descriptorInfo, nullptr, &frame.descriptors) != Success) return Fail("could not make its descriptor pool");
					}
					return true;
				}
				bool MakeSwapchain(bool replace) {
					if (replace) {
						FlushAndWait();
						vk.DeviceWaitIdle(device);
					}
					SurfaceCapabilities caps{};
					vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
					if ((caps.supportedUsageFlags & ImageUsageTransferDst) == 0) return Fail("cannot copy into the window's images");
					RECT client{};
					GetClientRect(window, &client);
					Extent2D extent = caps.currentExtent;
					if (extent.width == 0xFFFFFFFF) {
						extent.width = static_cast<std::uint32_t>(std::max<LONG>(client.right - client.left, 1));
						extent.height = static_cast<std::uint32_t>(std::max<LONG>(client.bottom - client.top, 1));
					}
					if (extent.width == 0 || extent.height == 0) return false;
					std::uint32_t formatCount = 0;
					vk.GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
					std::vector<SurfaceFormat> formats(formatCount);
					vk.GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
					if (formats.empty()) return Fail("found no format for the window");
					SurfaceFormat chosen = formats[0];
					for (const SurfaceFormat& format : formats) if (format.format == FormatB8G8R8A8Unorm) chosen = format;
					if (chosen.format == FormatUndefined) chosen.format = FormatB8G8R8A8Unorm;
					std::uint32_t modeCount = 0;
					vk.GetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
					std::vector<PresentMode> modes(modeCount);
					vk.GetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes.data());
					PresentMode mode = PresentFifo;
					if (!swapVsync) {
						if (std::ranges::contains(modes, PresentMailbox)) mode = PresentMailbox;
						else if (std::ranges::contains(modes, PresentImmediate)) mode = PresentImmediate;
					}
					std::uint32_t imageCount = std::max<std::uint32_t>(caps.minImageCount, 2);
					if (caps.maxImageCount > 0) imageCount = std::min<std::uint32_t>(imageCount, caps.maxImageCount);
					Flags composite = CompositeOpaque;
					if ((caps.supportedCompositeAlpha & CompositeOpaque) == 0) {
						for (Flags bit = 1; bit <= 8; bit <<= 1) if ((caps.supportedCompositeAlpha & bit) != 0) { composite = bit; break; }
					}
					const SwapchainKHR old = swapchain;
					SwapchainCreateInfoData info{ SwapchainCreateInfoKHR, nullptr, 0, surface, imageCount, chosen.format, chosen.colorSpace, extent, 1,
						ImageUsageTransferDst | ImageUsageColorAttachment, 0, 0, nullptr, caps.currentTransform, composite, mode, 1, old };
					SwapchainKHR made = 0;
					const Result result = vk.CreateSwapchainKHR(device, &info, nullptr, &made);
					if (old) vk.DestroySwapchainKHR(device, old, nullptr);
					swapchain = 0;
					for (Semaphore semaphore : renderDone) vk.DestroySemaphore(device, semaphore, nullptr);
					renderDone.clear();
					swapImages.clear();
					if (result != Success) return Fail("could not make a swap chain on the game's window");
					swapchain = made;
					std::uint32_t count = 0;
					vk.GetSwapchainImagesKHR(device, swapchain, &count, nullptr);
					swapImages.resize(count);
					vk.GetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
					SemaphoreCreateInfoData semaphoreInfo{ SemaphoreCreateInfo, nullptr, 0 };
					renderDone.resize(count);
					for (Semaphore& semaphore : renderDone) vk.CreateSemaphoreObject(device, &semaphoreInfo, nullptr, &semaphore);
					swapExtent = extent;
					swapOutdated = false;
					Logger::Debug("Renderer: Vulkan swap chain " + std::to_string(extent.width) + "x" + std::to_string(extent.height) + ", " + std::to_string(count)
						+ " images, format " + std::to_string(chosen.format) + ", present mode " + std::to_string(mode) + ".");
					return true;
				}

				FrameSlot& Slot() { return slots[current]; }
				// The current frame's command buffer, started if it is not being recorded yet.
				CommandBuffer Commands() {
					FrameSlot& frame = Slot();
					if (frame.recording) return frame.commands;
					if (frame.submitted) {
						vk.WaitForFences(device, 1, &frame.fence, 1, Forever);
						frame.submitted = false;
					}
					for (ImageResource& image : frame.doomed) DestroyImage(image);
					frame.doomed.clear();
					frame.ringUsed = 0;
					frame.setsUsed = 0;
					vk.ResetDescriptorPool(device, frame.descriptors, 0);
					if (frame.rayDescriptors) vk.ResetDescriptorPool(device, frame.rayDescriptors, 0);
					vk.ResetCommandBuffer(frame.commands, 0);
					CommandBufferBeginInfoData begin{ CommandBufferBeginInfo, nullptr, 1, nullptr };
					vk.BeginCommandBuffer(frame.commands, &begin);
					frame.recording = true;
					return frame.commands;
				}
				void BeginPass() {
					if (inPass || !framebuffer) return;
					RenderPassBeginInfoData begin{ RenderPassBeginInfo, nullptr, renderPass, framebuffer, { { 0, 0 }, { frameWidth, frameHeight } }, 0, nullptr };
					vk.CmdBeginRenderPass(Commands(), &begin, 0);
					inPass = true;
				}
				void EndPass() {
					if (!inPass) return;
					vk.CmdEndRenderPass(Slot().commands);
					inPass = false;
				}
				void Submit(Semaphore wait, Semaphore signal) {
					FrameSlot& frame = Slot();
					if (!frame.recording) Commands();
					EndPass();
					vk.EndCommandBuffer(frame.commands);
					vk.ResetFences(device, 1, &frame.fence);
					const Flags waitStage = StageTransfer;
					SubmitInfoData submit{ SubmitInfo, nullptr, wait ? 1u : 0u, wait ? &wait : nullptr, wait ? &waitStage : nullptr, 1, &frame.commands, signal ? 1u : 0u, signal ? &signal : nullptr };
					const Result submitted = vk.QueueSubmit(queue, 1, &submit, frame.fence);
					if (submitted != Success) Report("sending work to the GPU", submitted);
					frame.recording = false;
					frame.submitted = true;
				}
				// Sends what this frame recorded and waits for it, so its buffers can be used again right away.
				void FlushAndWait() {
					FrameSlot& frame = Slot();
					if (!frame.recording) return;
					Submit(0, 0);
					vk.WaitForFences(device, 1, &frame.fence, 1, Forever);
					frame.submitted = false;
				}
				// Room in this frame's upload buffer. Sends the work so far and starts over when it is full, and grows the
				// buffer for a request bigger than it.
				DeviceSize RingAllocate(DeviceSize size, DeviceSize alignment, std::uint8_t*& written) {
					written = nullptr;
					Commands();
					FrameSlot* frame = &Slot();
					DeviceSize offset = AlignUp(frame->ringUsed, alignment);
					if (offset + size > frame->ring.size) {
						FlushAndWait();
						if (size > frame->ring.size) {
							vk.DeviceWaitIdle(device);
							DestroyBuffer(frame->ring);
							if (!MakeBuffer(frame->ring, AlignUp(size, RingSize), BufferUsageVertex | BufferUsageIndex | BufferUsageUniform | BufferUsageStorage | BufferUsageTransferSrc, MemoryHostVisible | MemoryHostCoherent, 0)) {
								Fail("ran out of upload memory");
								return 0;
							}
						}
						Commands();
						frame = &Slot();
						offset = 0;
					}
					frame->ringUsed = offset + size;
					written = static_cast<std::uint8_t*>(frame->ring.mapped) + offset;
					return offset;
				}
				void CopyIn(CommandBuffer commands, Buffer from, DeviceSize offset, const ImageResource& image, std::uint32_t level = 0) {
					BufferImageCopy region{ offset, 0, 0, { AspectColor, level, 0, 1 }, { 0, 0, 0 }, { MipLevelSize(image.width, level), MipLevelSize(image.height, level), 1 } };
					vk.CmdCopyBufferToImage(commands, from, image.image, LayoutTransferDst, 1, &region);
				}
				void Barrier(CommandBuffer commands, Image image, Flags aspect, ImageLayout from, ImageLayout to, Flags sourceAccess, Flags targetAccess, Flags sourceStage, Flags targetStage) {
					// Every mip level (VK_REMAINING_MIP_LEVELS): textures with mips are moved as a whole, the others have one.
					ImageMemoryBarrierData barrier{ ImageMemoryBarrier, nullptr, sourceAccess, targetAccess, from, to, QueueFamilyIgnored, QueueFamilyIgnored, image,
						{ aspect, 0, RemainingMipLevels, 0, 1 } };
					vk.CmdPipelineBarrier(commands, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
				}

				static std::int32_t ToVk(Blend blend) {
					switch (blend) {
						case Blend::Zero: return 0;
						case Blend::One: return 1;
						case Blend::SrcColor: return 2;
						case Blend::InvSrcColor: return 3;
						case Blend::DestColor: return 4;
						case Blend::InvDestColor: return 5;
						case Blend::SrcAlpha: return 6;
						case Blend::InvSrcAlpha: return 7;
						case Blend::DestAlpha: return 8;
						case Blend::InvDestAlpha: return 9;
						case Blend::SrcAlphaSat: return 14;
					}
					return 1;
				}
				static std::int32_t ToVkAlpha(Blend blend) {
					switch (blend) {
						case Blend::SrcColor: return 6;
						case Blend::InvSrcColor: return 7;
						case Blend::DestColor: return 8;
						case Blend::InvDestColor: return 9;
						case Blend::SrcAlphaSat: return 1;
						default: return ToVk(blend);
					}
				}
				Pipeline PipelineFor(const DrawState& state) {
					const std::uint32_t key = state.PipelineKey();
					auto found = pipelines.find(key);
					if (found != pipelines.end()) return found->second;
					const PipelineShaderStageCreateInfoData stages[] = {
						{ PipelineShaderStageCreateInfo, nullptr, 0, ShaderVertex, vertexModule, "main", nullptr },
						{ PipelineShaderStageCreateInfo, nullptr, 0, ShaderFragment, fragmentModule, "main", nullptr },
					};
					const VertexInputBindingDescription binding{ 0, sizeof(Vertex), 0 };
					const VertexInputAttributeDescription attributes[] = {
						{ 0, 0, FormatR32G32B32A32Sfloat, 0 }, { 1, 0, FormatR8G8B8A8Unorm, 16 },
						{ 2, 0, FormatR32G32Sfloat, 20 }, { 3, 0, FormatR32G32Sfloat, 28 }, { 4, 0, FormatR32G32Sfloat, 36 },
					};
					PipelineVertexInputStateCreateInfoData vertexInput{ PipelineVertexInputStateCreateInfo, nullptr, 0, 1, &binding, 5, attributes };
					PipelineInputAssemblyStateCreateInfoData assembly{ PipelineInputAssemblyStateCreateInfo, nullptr, 0, 3, 0 };
					PipelineViewportStateCreateInfoData viewport{ PipelineViewportStateCreateInfo, nullptr, 0, 1, nullptr, 1, nullptr };
					// No culling here: the layer already dropped the culled triangles.
					PipelineRasterizationStateCreateInfoData raster{};
					raster.sType = PipelineRasterizationStateCreateInfo;
					raster.lineWidth = 1.0f;
					PipelineMultisampleStateCreateInfoData multisample{ PipelineMultisampleStateCreateInfo, nullptr, 0, msaaSamples, 0, 0.0f, nullptr, 0, 0 };
					PipelineDepthStencilStateCreateInfoData depth{};
					depth.sType = PipelineDepthStencilStateCreateInfo;
					depth.depthTestEnable = state.depthTest ? 1 : 0;
					depth.depthWriteEnable = state.depthWrite ? 1 : 0;
					// VkCompareOp is D3DCMPFUNC minus one.
					depth.depthCompareOp = static_cast<std::int32_t>(state.depthFunc) - 1;
					depth.maxDepthBounds = 1.0f;
					PipelineColorBlendAttachmentState blend{ state.blend ? 1u : 0u, ToVk(state.source), ToVk(state.destination), 0, ToVkAlpha(state.source), ToVkAlpha(state.destination), 0, 0xF };
					PipelineColorBlendStateCreateInfoData blending{ PipelineColorBlendStateCreateInfo, nullptr, 0, 0, 0, 1, &blend, { 0, 0, 0, 0 } };
					const std::int32_t dynamicStates[] = { 0, 1 };
					PipelineDynamicStateCreateInfoData dynamic{ PipelineDynamicStateCreateInfo, nullptr, 0, 2, dynamicStates };
					GraphicsPipelineCreateInfoData info{ GraphicsPipelineCreateInfo, nullptr, 0, 2, stages, &vertexInput, &assembly, nullptr, &viewport, &raster,
						&multisample, &depth, &blending, &dynamic, pipelineLayout, renderPass, 0, 0, -1 };
					Pipeline made = 0;
					if (vk.CreateGraphicsPipelines(device, 0, 1, &info, nullptr, &made) != Success) Fail("could not make a pipeline");
					pipelines[key] = made;
					return made;
				}
				Sampler SamplerFor(const SamplerState& state) {
					Sampler& made = samplers[state.Key()];
					if (!made) {
						// VkSamplerAddressMode is D3DTEXTUREADDRESS minus one.
						const std::int32_t filter = state.linear ? 1 : 0;
						// Mip levels are used (mipmap mode as the filter, no upper limit on the level: VK_LOD_CLAMP_NONE), and
						// texture_filtering's anisotropy for the linear ones when the device has it.
						const bool anisotropic = state.linear && anisotropy > 1 && anisotropyEnabled;
						SamplerCreateInfoData info{ SamplerCreateInfo, nullptr, 0, filter, filter, filter, static_cast<std::int32_t>(state.addressU) - 1,
							static_cast<std::int32_t>(state.addressV) - 1, 2, 0.0f, anisotropic ? 1u : 0u, anisotropic ? static_cast<float>(anisotropy) : 1.0f, 0, 0,
							0.0f, LodClampNone, 0, 0 };
						vk.CreateSampler(device, &info, nullptr, &made);
					}
					return made;
				}

				void Shutdown() {
					if (device) {
						vk.DeviceWaitIdle(device);
						DestroyRayObjects();
						for (auto& [handle, texture] : textures) DestroyImage(texture);
						textures.clear();
						for (FrameSlot& frame : slots) {
							for (ImageResource& image : frame.doomed) DestroyImage(image);
							DestroyBuffer(frame.ring);
							if (frame.descriptors) vk.DestroyDescriptorPool(device, frame.descriptors, nullptr);
							if (frame.fence) vk.DestroyFence(device, frame.fence, nullptr);
							if (frame.imageReady) vk.DestroySemaphore(device, frame.imageReady, nullptr);
							if (frame.pool) vk.DestroyCommandPool(device, frame.pool, nullptr);
						}
						for (auto& [key, pipeline] : pipelines) if (pipeline) vk.DestroyPipeline(device, pipeline, nullptr);
						for (Sampler sampler : samplers) if (sampler) vk.DestroySampler(device, sampler, nullptr);
						if (framebuffer) vk.DestroyFramebuffer(device, framebuffer, nullptr);
						DestroyImage(frameColor);
						DestroyImage(frameDepth);
						DestroyImage(msaaColor);
						DestroyImage(shownColor);
						DestroyImage(screen);
						DestroyBuffer(readback);
						DestroyPostTargets();
						for (Pipeline pipeline : postPipelines) if (pipeline) vk.DestroyPipeline(device, pipeline, nullptr);
						if (copyPipeline) vk.DestroyPipeline(device, copyPipeline, nullptr);
						for (ShaderModule module : postModules) vk.DestroyShaderModule(device, module, nullptr);
						for (RenderPass pass : { outputPass, edgesPass, weightsPass }) if (pass) vk.DestroyRenderPass(device, pass, nullptr);
						if (postLayout) vk.DestroyPipelineLayout(device, postLayout, nullptr);
						if (postSetLayout) vk.DestroyDescriptorSetLayout(device, postSetLayout, nullptr);
						if (linearSampler) vk.DestroySampler(device, linearSampler, nullptr);
						if (pointSampler) vk.DestroySampler(device, pointSampler, nullptr);
						if (vertexModule) vk.DestroyShaderModule(device, vertexModule, nullptr);
						if (fragmentModule) vk.DestroyShaderModule(device, fragmentModule, nullptr);
						if (pipelineLayout) vk.DestroyPipelineLayout(device, pipelineLayout, nullptr);
						if (setLayout) vk.DestroyDescriptorSetLayout(device, setLayout, nullptr);
						if (renderPass) vk.DestroyRenderPass(device, renderPass, nullptr);
						for (Semaphore semaphore : renderDone) vk.DestroySemaphore(device, semaphore, nullptr);
						if (swapchain) vk.DestroySwapchainKHR(device, swapchain, nullptr);
						vk.DestroyDevice(device, nullptr);
						device = nullptr;
					}
					if (instance) {
						if (surface) vk.DestroySurfaceKHR(instance, surface, nullptr);
						vk.DestroyInstance(instance, nullptr);
						instance = nullptr;
					}
					if (library) FreeLibrary(library);
					library = nullptr;
				}
		};
	}

	std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }
}
