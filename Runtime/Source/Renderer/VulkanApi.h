#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// The part of Vulkan 1.0 (plus the surface and swap chain extensions) the Vulkan backend uses, declared here so the
// build needs no Vulkan SDK. Layouts and values follow the Vulkan specification. vulkan-1.dll is loaded at run time.
// On 32-bit Windows, non-dispatchable handles are 64-bit integers.
namespace KeeperForge2::Render::Vk {
	using Bool32 = std::uint32_t;
	using Flags = std::uint32_t;
	using DeviceSize = std::uint64_t;
	using Instance = struct InstanceObject*;
	using PhysicalDevice = struct PhysicalDeviceObject*;
	using Device = struct DeviceObject*;
	using Queue = struct QueueObject*;
	using CommandBuffer = struct CommandBufferObject*;
	using SurfaceKHR = std::uint64_t;
	using SwapchainKHR = std::uint64_t;
	using Image = std::uint64_t;
	using ImageView = std::uint64_t;
	using Buffer = std::uint64_t;
	using DeviceMemory = std::uint64_t;
	using Sampler = std::uint64_t;
	using RenderPass = std::uint64_t;
	using Framebuffer = std::uint64_t;
	using ShaderModule = std::uint64_t;
	using DescriptorSetLayout = std::uint64_t;
	using PipelineLayout = std::uint64_t;
	using Pipeline = std::uint64_t;
	using PipelineCache = std::uint64_t;
	using DescriptorPool = std::uint64_t;
	using DescriptorSet = std::uint64_t;
	using CommandPool = std::uint64_t;
	using Fence = std::uint64_t;
	using Semaphore = std::uint64_t;

	enum Result : std::int32_t {
		Success = 0,
		NotReady = 1,
		Timeout = 2,
		Suboptimal = 1000001003,
		ErrorOutOfDate = -1000001004,
		ErrorOutOfPoolMemory = -1000069000,
		ErrorFragmentedPool = -12,
	};
	enum StructureType : std::int32_t {
		ApplicationInfo = 0,
		InstanceCreateInfo = 1,
		DeviceQueueCreateInfo = 2,
		DeviceCreateInfo = 3,
		SubmitInfo = 4,
		MemoryAllocateInfo = 5,
		FenceCreateInfo = 8,
		SemaphoreCreateInfo = 9,
		BufferCreateInfo = 12,
		ImageCreateInfo = 14,
		ImageViewCreateInfo = 15,
		ShaderModuleCreateInfo = 16,
		PipelineShaderStageCreateInfo = 18,
		PipelineVertexInputStateCreateInfo = 19,
		PipelineInputAssemblyStateCreateInfo = 20,
		PipelineViewportStateCreateInfo = 22,
		PipelineRasterizationStateCreateInfo = 23,
		PipelineMultisampleStateCreateInfo = 24,
		PipelineDepthStencilStateCreateInfo = 25,
		PipelineColorBlendStateCreateInfo = 26,
		PipelineDynamicStateCreateInfo = 27,
		GraphicsPipelineCreateInfo = 28,
		PipelineLayoutCreateInfo = 30,
		SamplerCreateInfo = 31,
		DescriptorSetLayoutCreateInfo = 32,
		DescriptorPoolCreateInfo = 33,
		DescriptorSetAllocateInfo = 34,
		WriteDescriptorSet = 35,
		FramebufferCreateInfo = 37,
		RenderPassCreateInfo = 38,
		CommandPoolCreateInfo = 39,
		CommandBufferAllocateInfo = 40,
		CommandBufferBeginInfo = 42,
		RenderPassBeginInfo = 43,
		ImageMemoryBarrier = 45,
		SwapchainCreateInfoKHR = 1000001000,
		PresentInfoKHR = 1000001001,
		Win32SurfaceCreateInfoKHR = 1000009000,
	};
	enum Format : std::int32_t {
		FormatUndefined = 0,
		FormatR8Unorm = 9,
		FormatR8G8Unorm = 16,
		FormatR8G8B8A8Unorm = 37,
		FormatB8G8R8A8Unorm = 44,
		FormatR32G32Sfloat = 103,
		FormatR32G32B32A32Sfloat = 109,
		FormatD16Unorm = 124,
		FormatX8D24UnormPack32 = 125,
		FormatD32Sfloat = 126,
		FormatD24UnormS8Uint = 129,
	};
	enum ImageLayout : std::int32_t {
		LayoutUndefined = 0,
		LayoutColorAttachment = 2,
		LayoutDepthStencilAttachment = 3,
		LayoutShaderReadOnly = 5,
		LayoutTransferSrc = 6,
		LayoutTransferDst = 7,
		LayoutPresentSrc = 1000001002,
	};
	enum PresentMode : std::int32_t { PresentImmediate = 0, PresentMailbox = 1, PresentFifo = 2 };

	constexpr std::uint32_t QueueGraphics = 0x1;
	constexpr std::uint32_t MemoryDeviceLocal = 0x1;
	constexpr std::uint32_t MemoryHostVisible = 0x2;
	constexpr std::uint32_t MemoryHostCoherent = 0x4;
	constexpr std::uint32_t MemoryHostCached = 0x8;
	constexpr std::uint32_t ImageUsageTransferSrc = 0x1;
	constexpr std::uint32_t ImageUsageTransferDst = 0x2;
	constexpr std::uint32_t ImageUsageSampled = 0x4;
	constexpr std::uint32_t ImageUsageColorAttachment = 0x10;
	constexpr std::uint32_t ImageUsageDepthStencilAttachment = 0x20;
	constexpr std::uint32_t BufferUsageTransferSrc = 0x1;
	constexpr std::uint32_t BufferUsageTransferDst = 0x2;
	constexpr std::uint32_t BufferUsageUniform = 0x10;
	constexpr std::uint32_t BufferUsageIndex = 0x40;
	constexpr std::uint32_t BufferUsageVertex = 0x80;
	constexpr std::uint32_t AspectColor = 0x1;
	constexpr std::uint32_t AspectDepth = 0x2;
	constexpr std::uint32_t AspectStencil = 0x4;
	constexpr std::uint32_t StageTopOfPipe = 0x1;
	constexpr std::uint32_t StageVertexInput = 0x4;
	constexpr std::uint32_t StageVertexShader = 0x8;
	constexpr std::uint32_t StageFragmentShader = 0x80;
	constexpr std::uint32_t StageEarlyFragmentTests = 0x100;
	constexpr std::uint32_t StageLateFragmentTests = 0x200;
	constexpr std::uint32_t StageColorAttachmentOutput = 0x400;
	constexpr std::uint32_t StageTransfer = 0x1000;
	constexpr std::uint32_t StageBottomOfPipe = 0x2000;
	constexpr std::uint32_t StageHost = 0x4000;
	constexpr std::uint32_t AccessShaderRead = 0x20;
	constexpr std::uint32_t AccessColorAttachmentRead = 0x80;
	constexpr std::uint32_t AccessColorAttachmentWrite = 0x100;
	constexpr std::uint32_t AccessDepthStencilRead = 0x200;
	constexpr std::uint32_t AccessDepthStencilWrite = 0x400;
	constexpr std::uint32_t AccessTransferRead = 0x800;
	constexpr std::uint32_t AccessTransferWrite = 0x1000;
	constexpr std::uint32_t AccessHostRead = 0x2000;
	constexpr std::uint32_t AccessMemoryRead = 0x8000;
	constexpr std::uint32_t FormatFeatureDepthStencilAttachment = 0x200;
	constexpr std::uint32_t QueueFamilyIgnored = 0xFFFFFFFF;
	constexpr std::uint32_t SubpassExternal = 0xFFFFFFFF;
	constexpr std::uint32_t CompositeOpaque = 0x1;
	constexpr std::uint32_t TransformIdentity = 0x1;
	constexpr std::uint32_t DescriptorCombinedImageSampler = 1;
	constexpr std::uint32_t DescriptorUniformBuffer = 6;
	constexpr std::uint32_t ShaderVertex = 0x1;
	constexpr std::uint32_t ShaderFragment = 0x10;
	constexpr std::uint32_t PhysicalDeviceDiscrete = 2;
	constexpr std::uint32_t ApiVersion10 = 1u << 22;

	struct Extent2D { std::uint32_t width, height; };
	struct Extent3D { std::uint32_t width, height, depth; };
	struct Offset2D { std::int32_t x, y; };
	struct Offset3D { std::int32_t x, y, z; };
	struct Rect2D { Offset2D offset; Extent2D extent; };

	struct ApplicationInfoData {
		StructureType sType; const void* pNext; const char* pApplicationName; std::uint32_t applicationVersion;
		const char* pEngineName; std::uint32_t engineVersion; std::uint32_t apiVersion;
	};
	struct InstanceCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; const ApplicationInfoData* pApplicationInfo;
		std::uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; std::uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
	};
	struct Win32SurfaceCreateInfoData { StructureType sType; const void* pNext; Flags flags; HINSTANCE hinstance; HWND hwnd; };
	struct QueueFamilyProperties { Flags queueFlags; std::uint32_t queueCount; std::uint32_t timestampValidBits; Extent3D minImageTransferGranularity; };
	struct MemoryType { Flags propertyFlags; std::uint32_t heapIndex; };
	struct MemoryHeap { DeviceSize size; Flags flags; };
	struct PhysicalDeviceMemoryProperties { std::uint32_t memoryTypeCount; MemoryType memoryTypes[32]; std::uint32_t memoryHeapCount; MemoryHeap memoryHeaps[16]; };
	struct FormatProperties { Flags linearTilingFeatures; Flags optimalTilingFeatures; Flags bufferFeatures; };
	struct DeviceQueueCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t queueFamilyIndex; std::uint32_t queueCount; const float* pQueuePriorities;
	};
	struct DeviceCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t queueCreateInfoCount; const DeviceQueueCreateInfoData* pQueueCreateInfos;
		std::uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; std::uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
		const void* pEnabledFeatures;
	};
	struct SurfaceCapabilities {
		std::uint32_t minImageCount; std::uint32_t maxImageCount; Extent2D currentExtent; Extent2D minImageExtent; Extent2D maxImageExtent;
		std::uint32_t maxImageArrayLayers; Flags supportedTransforms; Flags currentTransform; Flags supportedCompositeAlpha; Flags supportedUsageFlags;
	};
	struct SurfaceFormat { Format format; std::int32_t colorSpace; };
	struct SwapchainCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; SurfaceKHR surface; std::uint32_t minImageCount; Format imageFormat; std::int32_t imageColorSpace;
		Extent2D imageExtent; std::uint32_t imageArrayLayers; Flags imageUsage; std::int32_t imageSharingMode; std::uint32_t queueFamilyIndexCount;
		const std::uint32_t* pQueueFamilyIndices; Flags preTransform; Flags compositeAlpha; PresentMode presentMode; Bool32 clipped; SwapchainKHR oldSwapchain;
	};
	struct ImageCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::int32_t imageType; Format format; Extent3D extent; std::uint32_t mipLevels;
		std::uint32_t arrayLayers; std::uint32_t samples; std::int32_t tiling; Flags usage; std::int32_t sharingMode; std::uint32_t queueFamilyIndexCount;
		const std::uint32_t* pQueueFamilyIndices; ImageLayout initialLayout;
	};
	struct MemoryRequirements { DeviceSize size; DeviceSize alignment; std::uint32_t memoryTypeBits; };
	struct MemoryAllocateInfoData { StructureType sType; const void* pNext; DeviceSize allocationSize; std::uint32_t memoryTypeIndex; };
	struct ComponentMapping { std::int32_t r, g, b, a; };
	struct ImageSubresourceRange { Flags aspectMask; std::uint32_t baseMipLevel; std::uint32_t levelCount; std::uint32_t baseArrayLayer; std::uint32_t layerCount; };
	struct ImageViewCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; Image image; std::int32_t viewType; Format format; ComponentMapping components; ImageSubresourceRange subresourceRange;
	};
	struct BufferCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; DeviceSize size; Flags usage; std::int32_t sharingMode; std::uint32_t queueFamilyIndexCount; const std::uint32_t* pQueueFamilyIndices;
	};
	struct SamplerCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::int32_t magFilter; std::int32_t minFilter; std::int32_t mipmapMode; std::int32_t addressModeU;
		std::int32_t addressModeV; std::int32_t addressModeW; float mipLodBias; Bool32 anisotropyEnable; float maxAnisotropy; Bool32 compareEnable;
		std::int32_t compareOp; float minLod; float maxLod; std::int32_t borderColor; Bool32 unnormalizedCoordinates;
	};
	struct AttachmentDescription {
		Flags flags; Format format; std::uint32_t samples; std::int32_t loadOp; std::int32_t storeOp; std::int32_t stencilLoadOp; std::int32_t stencilStoreOp;
		ImageLayout initialLayout; ImageLayout finalLayout;
	};
	struct AttachmentReference { std::uint32_t attachment; ImageLayout layout; };
	struct SubpassDescription {
		Flags flags; std::int32_t pipelineBindPoint; std::uint32_t inputAttachmentCount; const AttachmentReference* pInputAttachments;
		std::uint32_t colorAttachmentCount; const AttachmentReference* pColorAttachments; const AttachmentReference* pResolveAttachments;
		const AttachmentReference* pDepthStencilAttachment; std::uint32_t preserveAttachmentCount; const std::uint32_t* pPreserveAttachments;
	};
	struct SubpassDependency {
		std::uint32_t srcSubpass; std::uint32_t dstSubpass; Flags srcStageMask; Flags dstStageMask; Flags srcAccessMask; Flags dstAccessMask; Flags dependencyFlags;
	};
	struct RenderPassCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t attachmentCount; const AttachmentDescription* pAttachments;
		std::uint32_t subpassCount; const SubpassDescription* pSubpasses; std::uint32_t dependencyCount; const SubpassDependency* pDependencies;
	};
	struct FramebufferCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; RenderPass renderPass; std::uint32_t attachmentCount; const ImageView* pAttachments;
		std::uint32_t width; std::uint32_t height; std::uint32_t layers;
	};
	struct ShaderModuleCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::size_t codeSize; const std::uint32_t* pCode; };
	struct DescriptorSetLayoutBinding { std::uint32_t binding; std::uint32_t descriptorType; std::uint32_t descriptorCount; Flags stageFlags; const Sampler* pImmutableSamplers; };
	struct DescriptorSetLayoutCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::uint32_t bindingCount; const DescriptorSetLayoutBinding* pBindings; };
	struct PipelineLayoutCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t setLayoutCount; const DescriptorSetLayout* pSetLayouts;
		std::uint32_t pushConstantRangeCount; const void* pPushConstantRanges;
	};
	struct PipelineShaderStageCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; Flags stage; ShaderModule module; const char* pName; const void* pSpecializationInfo;
	};
	struct VertexInputBindingDescription { std::uint32_t binding; std::uint32_t stride; std::int32_t inputRate; };
	struct VertexInputAttributeDescription { std::uint32_t location; std::uint32_t binding; Format format; std::uint32_t offset; };
	struct PipelineVertexInputStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t vertexBindingDescriptionCount; const VertexInputBindingDescription* pVertexBindingDescriptions;
		std::uint32_t vertexAttributeDescriptionCount; const VertexInputAttributeDescription* pVertexAttributeDescriptions;
	};
	struct PipelineInputAssemblyStateCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::int32_t topology; Bool32 primitiveRestartEnable; };
	struct Viewport { float x, y, width, height, minDepth, maxDepth; };
	struct PipelineViewportStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t viewportCount; const Viewport* pViewports; std::uint32_t scissorCount; const Rect2D* pScissors;
	};
	struct PipelineRasterizationStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; Bool32 depthClampEnable; Bool32 rasterizerDiscardEnable; std::int32_t polygonMode; Flags cullMode;
		std::int32_t frontFace; Bool32 depthBiasEnable; float depthBiasConstantFactor; float depthBiasClamp; float depthBiasSlopeFactor; float lineWidth;
	};
	struct PipelineMultisampleStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t rasterizationSamples; Bool32 sampleShadingEnable; float minSampleShading;
		const std::uint32_t* pSampleMask; Bool32 alphaToCoverageEnable; Bool32 alphaToOneEnable;
	};
	struct StencilOpState { std::int32_t failOp, passOp, depthFailOp, compareOp; std::uint32_t compareMask, writeMask, reference; };
	struct PipelineDepthStencilStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; Bool32 depthTestEnable; Bool32 depthWriteEnable; std::int32_t depthCompareOp;
		Bool32 depthBoundsTestEnable; Bool32 stencilTestEnable; StencilOpState front; StencilOpState back; float minDepthBounds; float maxDepthBounds;
	};
	struct PipelineColorBlendAttachmentState {
		Bool32 blendEnable; std::int32_t srcColorBlendFactor; std::int32_t dstColorBlendFactor; std::int32_t colorBlendOp;
		std::int32_t srcAlphaBlendFactor; std::int32_t dstAlphaBlendFactor; std::int32_t alphaBlendOp; Flags colorWriteMask;
	};
	struct PipelineColorBlendStateCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; Bool32 logicOpEnable; std::int32_t logicOp; std::uint32_t attachmentCount;
		const PipelineColorBlendAttachmentState* pAttachments; float blendConstants[4];
	};
	struct PipelineDynamicStateCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::uint32_t dynamicStateCount; const std::int32_t* pDynamicStates; };
	struct GraphicsPipelineCreateInfoData {
		StructureType sType; const void* pNext; Flags flags; std::uint32_t stageCount; const PipelineShaderStageCreateInfoData* pStages;
		const PipelineVertexInputStateCreateInfoData* pVertexInputState; const PipelineInputAssemblyStateCreateInfoData* pInputAssemblyState;
		const void* pTessellationState; const PipelineViewportStateCreateInfoData* pViewportState; const PipelineRasterizationStateCreateInfoData* pRasterizationState;
		const PipelineMultisampleStateCreateInfoData* pMultisampleState; const PipelineDepthStencilStateCreateInfoData* pDepthStencilState;
		const PipelineColorBlendStateCreateInfoData* pColorBlendState; const PipelineDynamicStateCreateInfoData* pDynamicState; PipelineLayout layout;
		RenderPass renderPass; std::uint32_t subpass; Pipeline basePipelineHandle; std::int32_t basePipelineIndex;
	};
	struct CommandPoolCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::uint32_t queueFamilyIndex; };
	struct CommandBufferAllocateInfoData { StructureType sType; const void* pNext; CommandPool commandPool; std::int32_t level; std::uint32_t commandBufferCount; };
	struct CommandBufferBeginInfoData { StructureType sType; const void* pNext; Flags flags; const void* pInheritanceInfo; };
	union ClearColorValue { float float32[4]; std::int32_t int32[4]; std::uint32_t uint32[4]; };
	struct ClearDepthStencilValue { float depth; std::uint32_t stencil; };
	union ClearValue { ClearColorValue color; ClearDepthStencilValue depthStencil; };
	struct RenderPassBeginInfoData {
		StructureType sType; const void* pNext; RenderPass renderPass; Framebuffer framebuffer; Rect2D renderArea; std::uint32_t clearValueCount; const ClearValue* pClearValues;
	};
	struct ImageMemoryBarrierData {
		StructureType sType; const void* pNext; Flags srcAccessMask; Flags dstAccessMask; ImageLayout oldLayout; ImageLayout newLayout;
		std::uint32_t srcQueueFamilyIndex; std::uint32_t dstQueueFamilyIndex; Image image; ImageSubresourceRange subresourceRange;
	};
	struct ImageSubresourceLayers { Flags aspectMask; std::uint32_t mipLevel; std::uint32_t baseArrayLayer; std::uint32_t layerCount; };
	struct BufferImageCopy {
		DeviceSize bufferOffset; std::uint32_t bufferRowLength; std::uint32_t bufferImageHeight; ImageSubresourceLayers imageSubresource; Offset3D imageOffset; Extent3D imageExtent;
	};
	struct ImageBlit { ImageSubresourceLayers srcSubresource; Offset3D srcOffsets[2]; ImageSubresourceLayers dstSubresource; Offset3D dstOffsets[2]; };
	struct SubmitInfoData {
		StructureType sType; const void* pNext; std::uint32_t waitSemaphoreCount; const Semaphore* pWaitSemaphores; const Flags* pWaitDstStageMask;
		std::uint32_t commandBufferCount; const CommandBuffer* pCommandBuffers; std::uint32_t signalSemaphoreCount; const Semaphore* pSignalSemaphores;
	};
	struct PresentInfoData {
		StructureType sType; const void* pNext; std::uint32_t waitSemaphoreCount; const Semaphore* pWaitSemaphores; std::uint32_t swapchainCount;
		const SwapchainKHR* pSwapchains; const std::uint32_t* pImageIndices; Result* pResults;
	};
	struct FenceCreateInfoData { StructureType sType; const void* pNext; Flags flags; };
	struct SemaphoreCreateInfoData { StructureType sType; const void* pNext; Flags flags; };
	struct DescriptorPoolSize { std::uint32_t type; std::uint32_t descriptorCount; };
	struct DescriptorPoolCreateInfoData { StructureType sType; const void* pNext; Flags flags; std::uint32_t maxSets; std::uint32_t poolSizeCount; const DescriptorPoolSize* pPoolSizes; };
	struct DescriptorSetAllocateInfoData { StructureType sType; const void* pNext; DescriptorPool descriptorPool; std::uint32_t descriptorSetCount; const DescriptorSetLayout* pSetLayouts; };
	struct DescriptorBufferInfo { Buffer buffer; DeviceSize offset; DeviceSize range; };
	struct DescriptorImageInfo { Sampler sampler; ImageView imageView; ImageLayout imageLayout; };
	struct WriteDescriptorSetData {
		StructureType sType; const void* pNext; DescriptorSet dstSet; std::uint32_t dstBinding; std::uint32_t dstArrayElement; std::uint32_t descriptorCount;
		std::uint32_t descriptorType; const DescriptorImageInfo* pImageInfo; const DescriptorBufferInfo* pBufferInfo; const void* pTexelBufferView;
	};
	struct ClearAttachment { Flags aspectMask; std::uint32_t colorAttachment; ClearValue clearValue; };
	struct ClearRect { Rect2D rect; std::uint32_t baseArrayLayer; std::uint32_t layerCount; };
	struct PushConstantRange { Flags stageFlags; std::uint32_t offset; std::uint32_t size; };
	struct ImageFormatProperties { Extent3D maxExtent; std::uint32_t maxMipLevels; std::uint32_t maxArrayLayers; Flags sampleCounts; DeviceSize maxResourceSize; };
	static_assert(sizeof(ImageFormatProperties) == 32);
	struct ImageResolve { ImageSubresourceLayers srcSubresource; Offset3D srcOffset; ImageSubresourceLayers dstSubresource; Offset3D dstOffset; Extent3D extent; };

	using VoidFunction = void(__stdcall*)();
	using GetInstanceProcAddrFn = VoidFunction(__stdcall*)(Instance, const char*);
	using GetDeviceProcAddrFn = VoidFunction(__stdcall*)(Device, const char*);

	// Every entry point the backend calls, filled from vulkan-1.dll.
	struct Functions {
		Result(__stdcall* CreateInstance)(const InstanceCreateInfoData*, const void*, Instance*);
		void(__stdcall* DestroyInstance)(Instance, const void*);
		Result(__stdcall* EnumeratePhysicalDevices)(Instance, std::uint32_t*, PhysicalDevice*);
		void(__stdcall* GetPhysicalDeviceProperties)(PhysicalDevice, void*);
		void(__stdcall* GetPhysicalDeviceQueueFamilyProperties)(PhysicalDevice, std::uint32_t*, QueueFamilyProperties*);
		void(__stdcall* GetPhysicalDeviceMemoryProperties)(PhysicalDevice, PhysicalDeviceMemoryProperties*);
		void(__stdcall* GetPhysicalDeviceFormatProperties)(PhysicalDevice, Format, FormatProperties*);
		// Image type, tiling, usage, flags.
		Result(__stdcall* GetPhysicalDeviceImageFormatProperties)(PhysicalDevice, Format, std::int32_t, std::int32_t, Flags, Flags, ImageFormatProperties*);
		Result(__stdcall* GetPhysicalDeviceSurfaceSupportKHR)(PhysicalDevice, std::uint32_t, SurfaceKHR, Bool32*);
		Result(__stdcall* GetPhysicalDeviceSurfaceCapabilitiesKHR)(PhysicalDevice, SurfaceKHR, SurfaceCapabilities*);
		Result(__stdcall* GetPhysicalDeviceSurfaceFormatsKHR)(PhysicalDevice, SurfaceKHR, std::uint32_t*, SurfaceFormat*);
		Result(__stdcall* GetPhysicalDeviceSurfacePresentModesKHR)(PhysicalDevice, SurfaceKHR, std::uint32_t*, PresentMode*);
		Result(__stdcall* CreateWin32SurfaceKHR)(Instance, const Win32SurfaceCreateInfoData*, const void*, SurfaceKHR*);
		void(__stdcall* DestroySurfaceKHR)(Instance, SurfaceKHR, const void*);
		Result(__stdcall* CreateDevice)(PhysicalDevice, const DeviceCreateInfoData*, const void*, Device*);
		GetDeviceProcAddrFn GetDeviceProcAddr;

		void(__stdcall* DestroyDevice)(Device, const void*);
		void(__stdcall* GetDeviceQueue)(Device, std::uint32_t, std::uint32_t, Queue*);
		Result(__stdcall* CreateSwapchainKHR)(Device, const SwapchainCreateInfoData*, const void*, SwapchainKHR*);
		void(__stdcall* DestroySwapchainKHR)(Device, SwapchainKHR, const void*);
		Result(__stdcall* GetSwapchainImagesKHR)(Device, SwapchainKHR, std::uint32_t*, Image*);
		Result(__stdcall* AcquireNextImageKHR)(Device, SwapchainKHR, std::uint64_t, Semaphore, Fence, std::uint32_t*);
		Result(__stdcall* QueuePresentKHR)(Queue, const PresentInfoData*);
		Result(__stdcall* QueueSubmit)(Queue, std::uint32_t, const SubmitInfoData*, Fence);
		Result(__stdcall* DeviceWaitIdle)(Device);
		Result(__stdcall* CreateCommandPool)(Device, const CommandPoolCreateInfoData*, const void*, CommandPool*);
		void(__stdcall* DestroyCommandPool)(Device, CommandPool, const void*);
		Result(__stdcall* AllocateCommandBuffers)(Device, const CommandBufferAllocateInfoData*, CommandBuffer*);
		Result(__stdcall* ResetCommandBuffer)(CommandBuffer, Flags);
		Result(__stdcall* BeginCommandBuffer)(CommandBuffer, const CommandBufferBeginInfoData*);
		Result(__stdcall* EndCommandBuffer)(CommandBuffer);
		Result(__stdcall* CreateFence)(Device, const FenceCreateInfoData*, const void*, Fence*);
		void(__stdcall* DestroyFence)(Device, Fence, const void*);
		Result(__stdcall* WaitForFences)(Device, std::uint32_t, const Fence*, Bool32, std::uint64_t);
		Result(__stdcall* ResetFences)(Device, std::uint32_t, const Fence*);
		// Not "CreateSemaphore": Windows.h makes that a macro.
		Result(__stdcall* CreateSemaphoreObject)(Device, const SemaphoreCreateInfoData*, const void*, Semaphore*);
		void(__stdcall* DestroySemaphore)(Device, Semaphore, const void*);
		Result(__stdcall* CreateImage)(Device, const ImageCreateInfoData*, const void*, Image*);
		void(__stdcall* DestroyImage)(Device, Image, const void*);
		void(__stdcall* GetImageMemoryRequirements)(Device, Image, MemoryRequirements*);
		Result(__stdcall* AllocateMemory)(Device, const MemoryAllocateInfoData*, const void*, DeviceMemory*);
		void(__stdcall* FreeMemory)(Device, DeviceMemory, const void*);
		Result(__stdcall* BindImageMemory)(Device, Image, DeviceMemory, DeviceSize);
		Result(__stdcall* CreateImageView)(Device, const ImageViewCreateInfoData*, const void*, ImageView*);
		void(__stdcall* DestroyImageView)(Device, ImageView, const void*);
		Result(__stdcall* CreateBuffer)(Device, const BufferCreateInfoData*, const void*, Buffer*);
		void(__stdcall* DestroyBuffer)(Device, Buffer, const void*);
		void(__stdcall* GetBufferMemoryRequirements)(Device, Buffer, MemoryRequirements*);
		Result(__stdcall* BindBufferMemory)(Device, Buffer, DeviceMemory, DeviceSize);
		Result(__stdcall* MapMemory)(Device, DeviceMemory, DeviceSize, DeviceSize, Flags, void**);
		void(__stdcall* UnmapMemory)(Device, DeviceMemory);
		Result(__stdcall* CreateSampler)(Device, const SamplerCreateInfoData*, const void*, Sampler*);
		void(__stdcall* DestroySampler)(Device, Sampler, const void*);
		Result(__stdcall* CreateRenderPass)(Device, const RenderPassCreateInfoData*, const void*, RenderPass*);
		void(__stdcall* DestroyRenderPass)(Device, RenderPass, const void*);
		Result(__stdcall* CreateFramebuffer)(Device, const FramebufferCreateInfoData*, const void*, Framebuffer*);
		void(__stdcall* DestroyFramebuffer)(Device, Framebuffer, const void*);
		Result(__stdcall* CreateShaderModule)(Device, const ShaderModuleCreateInfoData*, const void*, ShaderModule*);
		void(__stdcall* DestroyShaderModule)(Device, ShaderModule, const void*);
		Result(__stdcall* CreateDescriptorSetLayout)(Device, const DescriptorSetLayoutCreateInfoData*, const void*, DescriptorSetLayout*);
		void(__stdcall* DestroyDescriptorSetLayout)(Device, DescriptorSetLayout, const void*);
		Result(__stdcall* CreatePipelineLayout)(Device, const PipelineLayoutCreateInfoData*, const void*, PipelineLayout*);
		void(__stdcall* DestroyPipelineLayout)(Device, PipelineLayout, const void*);
		Result(__stdcall* CreateGraphicsPipelines)(Device, PipelineCache, std::uint32_t, const GraphicsPipelineCreateInfoData*, const void*, Pipeline*);
		void(__stdcall* DestroyPipeline)(Device, Pipeline, const void*);
		Result(__stdcall* CreateDescriptorPool)(Device, const DescriptorPoolCreateInfoData*, const void*, DescriptorPool*);
		void(__stdcall* DestroyDescriptorPool)(Device, DescriptorPool, const void*);
		Result(__stdcall* ResetDescriptorPool)(Device, DescriptorPool, Flags);
		Result(__stdcall* AllocateDescriptorSets)(Device, const DescriptorSetAllocateInfoData*, DescriptorSet*);
		void(__stdcall* UpdateDescriptorSets)(Device, std::uint32_t, const WriteDescriptorSetData*, std::uint32_t, const void*);
		void(__stdcall* CmdBeginRenderPass)(CommandBuffer, const RenderPassBeginInfoData*, std::int32_t);
		void(__stdcall* CmdEndRenderPass)(CommandBuffer);
		void(__stdcall* CmdBindPipeline)(CommandBuffer, std::int32_t, Pipeline);
		void(__stdcall* CmdBindDescriptorSets)(CommandBuffer, std::int32_t, PipelineLayout, std::uint32_t, std::uint32_t, const DescriptorSet*, std::uint32_t, const std::uint32_t*);
		void(__stdcall* CmdBindVertexBuffers)(CommandBuffer, std::uint32_t, std::uint32_t, const Buffer*, const DeviceSize*);
		void(__stdcall* CmdBindIndexBuffer)(CommandBuffer, Buffer, DeviceSize, std::int32_t);
		void(__stdcall* CmdDrawIndexed)(CommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t);
		void(__stdcall* CmdSetViewport)(CommandBuffer, std::uint32_t, std::uint32_t, const Viewport*);
		void(__stdcall* CmdSetScissor)(CommandBuffer, std::uint32_t, std::uint32_t, const Rect2D*);
		void(__stdcall* CmdPipelineBarrier)(CommandBuffer, Flags, Flags, Flags, std::uint32_t, const void*, std::uint32_t, const void*, std::uint32_t, const ImageMemoryBarrierData*);
		void(__stdcall* CmdCopyBufferToImage)(CommandBuffer, Buffer, Image, ImageLayout, std::uint32_t, const BufferImageCopy*);
		void(__stdcall* CmdCopyImageToBuffer)(CommandBuffer, Image, ImageLayout, Buffer, std::uint32_t, const BufferImageCopy*);
		void(__stdcall* CmdBlitImage)(CommandBuffer, Image, ImageLayout, Image, ImageLayout, std::uint32_t, const ImageBlit*, std::int32_t);
		void(__stdcall* CmdClearColorImage)(CommandBuffer, Image, ImageLayout, const ClearColorValue*, std::uint32_t, const ImageSubresourceRange*);
		void(__stdcall* CmdClearAttachments)(CommandBuffer, std::uint32_t, const ClearAttachment*, std::uint32_t, const ClearRect*);
		// Anti-aliasing.
		void(__stdcall* CmdDraw)(CommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
		void(__stdcall* CmdPushConstants)(CommandBuffer, PipelineLayout, Flags, std::uint32_t, std::uint32_t, const void*);
		void(__stdcall* CmdResolveImage)(CommandBuffer, Image, ImageLayout, Image, ImageLayout, std::uint32_t, const ImageResolve*);
	};
}
