#include "Backend.h"
#include "ShaderSources.h"
#include "Logger.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace KeeperForge2::Render {
	using Microsoft::WRL::ComPtr;
	namespace {

		std::string HexResult(HRESULT result) {
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
			return text;
		}

		// d3dcompiler_47.dll ships with Windows 10 and later; loaded only when a DirectX renderer starts.
		using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
		CompileFn LoadCompiler() {
			static CompileFn compile = [] {
				HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
				return module ? reinterpret_cast<CompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
			}();
			return compile;
		}
	}

	// Shared with the DirectX 12 backend (same HLSL).
	bool CompileHlsl(const char* source, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
		const CompileFn compile = LoadCompiler();
		if (!compile) {
			Logger::Warning("Renderer: d3dcompiler_47.dll is missing, DirectX cannot compile its shaders.");
			return false;
		}
		ComPtr<ID3DBlob> errors;
		const HRESULT result = compile(source, std::strlen(source), "KeeperForge2", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
		if (FAILED(result)) {
			Logger::Warning(std::string("Renderer: shader ") + entry + " did not compile: " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : HexResult(result)));
			return false;
		}
		return true;
	}

	namespace {
		class D3D11Backend final : public Backend {
			public:
				~D3D11Backend() override {
					if (context) {
						context->ClearState();
						context->Flush();
					}
				}
				const char* Name() const override { return "DirectX 11"; }
				bool FlipsY() const override { return false; }

				bool Init(HWND target) override {
					window = target;
					const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
					HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
						D3D11_SDK_VERSION, &device, nullptr, &context);
					if (FAILED(result)) return Fail("no DirectX 11 device", result);
					if (!MakeSwapChain()) return false;
					ComPtr<ID3DBlob> drawVertex, drawPixel, presentVertex, presentPixel;
					if (!CompileHlsl(Shaders::DrawHlsl, "VertexMain", "vs_4_0", drawVertex) || !CompileHlsl(Shaders::DrawHlsl, "PixelMain", "ps_4_0", drawPixel)
						|| !CompileHlsl(Shaders::PresentHlsl, "PresentVertex", "vs_4_0", presentVertex) || !CompileHlsl(Shaders::PresentHlsl, "PresentPixel", "ps_4_0", presentPixel)) {
						return false;
					}
					device->CreateVertexShader(drawVertex->GetBufferPointer(), drawVertex->GetBufferSize(), nullptr, &vertexShader);
					device->CreatePixelShader(drawPixel->GetBufferPointer(), drawPixel->GetBufferSize(), nullptr, &pixelShader);
					device->CreateVertexShader(presentVertex->GetBufferPointer(), presentVertex->GetBufferSize(), nullptr, &presentVertexShader);
					device->CreatePixelShader(presentPixel->GetBufferPointer(), presentPixel->GetBufferSize(), nullptr, &presentPixelShader);
					const D3D11_INPUT_ELEMENT_DESC layout[] = {
						{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
						{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
						{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					};
					result = device->CreateInputLayout(layout, ARRAYSIZE(layout), drawVertex->GetBufferPointer(), drawVertex->GetBufferSize(), &inputLayout);
					if (FAILED(result) || !vertexShader || !pixelShader || !presentVertexShader || !presentPixelShader) return Fail("could not make its shaders", result);

					D3D11_BUFFER_DESC constants{};
					constants.ByteWidth = sizeof(ShaderConstants);
					constants.Usage = D3D11_USAGE_DYNAMIC;
					constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					device->CreateBuffer(&constants, nullptr, &constantBuffer);
					constants.ByteWidth = 16;
					device->CreateBuffer(&constants, nullptr, &presentConstants);
					if (!GrowBuffer(vertexBuffer, vertexCapacity, 4u * 1024 * 1024, D3D11_BIND_VERTEX_BUFFER) || !GrowBuffer(indexBuffer, indexCapacity, 1024u * 1024, D3D11_BIND_INDEX_BUFFER)) {
						return Fail("could not make its buffers", E_OUTOFMEMORY);
					}

					D3D11_RASTERIZER_DESC raster{};
					raster.FillMode = D3D11_FILL_SOLID;
					raster.CullMode = D3D11_CULL_NONE;
					raster.DepthClipEnable = TRUE;
					device->CreateRasterizerState(&raster, &rasterizer);
					D3D11_SAMPLER_DESC presentSamplerDesc{};
					presentSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
					presentSamplerDesc.AddressU = presentSamplerDesc.AddressV = presentSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
					presentSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
					device->CreateSamplerState(&presentSamplerDesc, &presentSampler);
					D3D11_BLEND_DESC opaque{};
					opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
					device->CreateBlendState(&opaque, &presentBlend);
					D3D11_DEPTH_STENCIL_DESC noDepth{};
					device->CreateDepthStencilState(&noDepth, &presentDepth);

					const std::uint32_t white = 0xFFFFFFFF;
					whiteTexture = CreateTexture(1, 1);
					UpdateTexture(whiteTexture, &white, 4);
					SetUpAntiAliasing();
					return true;
				}

				bool SetFrameSize(std::uint32_t width, std::uint32_t height) override {
					if (width == frameWidth && height == frameHeight && frameTexture) return true;
					frameWidth = width;
					frameHeight = height;
					frameTexture.Reset();
					frameView.Reset();
					frameShaderView.Reset();
					frameStaging.Reset();
					shownTexture.Reset();
					depthTexture.Reset();
					depthView.Reset();
					msaaColor.Reset();
					msaaView.Reset();
					frameUnresolved = false;
					for (PostTarget* target : { &aaOutput, &smaaEdges, &smaaWeights }) *target = {};
					D3D11_TEXTURE2D_DESC description{};
					description.Width = width;
					description.Height = height;
					description.MipLevels = 1;
					description.ArraySize = 1;
					description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = 1;
					description.Usage = D3D11_USAGE_DEFAULT;
					description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
					if (FAILED(device->CreateTexture2D(&description, nullptr, &frameTexture))) return false;
					device->CreateRenderTargetView(frameTexture.Get(), nullptr, &frameView);
					device->CreateShaderResourceView(frameTexture.Get(), nullptr, &frameShaderView);
					description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					if (FAILED(device->CreateTexture2D(&description, nullptr, &shownTexture))) return false;
					description.Usage = D3D11_USAGE_STAGING;
					description.BindFlags = 0;
					description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					device->CreateTexture2D(&description, nullptr, &frameStaging);
					description.Format = DXGI_FORMAT_D32_FLOAT;
					description.Usage = D3D11_USAGE_DEFAULT;
					description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
					description.CPUAccessFlags = 0;
					// MSAA: the draws go into a multisampled color and depth; frameTexture holds the frame resolved.
					description.SampleDesc.Count = msaaSamples;
					if (FAILED(device->CreateTexture2D(&description, nullptr, &depthTexture))) return false;
					device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depthView);
					if (msaaSamples > 1) {
						description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
						description.BindFlags = D3D11_BIND_RENDER_TARGET;
						if (FAILED(device->CreateTexture2D(&description, nullptr, &msaaColor))) return false;
						device->CreateRenderTargetView(msaaColor.Get(), nullptr, &msaaView);
					}
					if (postShaders) {
						const bool smaa = antiAliasing.mode == AntiAliasingMode::Smaa;
						if (!MakePostTarget(aaOutput, DXGI_FORMAT_B8G8R8A8_UNORM) || (smaa && (!MakePostTarget(smaaEdges, DXGI_FORMAT_R8G8_UNORM) || !MakePostTarget(smaaWeights, DXGI_FORMAT_R8G8B8A8_UNORM)))) {
							Logger::Warning("Renderer: DirectX 11 could not make the anti-aliasing's pictures; the frame is shown without it.");
							for (PostTarget* target : { &aaOutput, &smaaEdges, &smaaWeights }) *target = {};
						}
					}
					ClearDepth();
					return true;
				}

				std::uint32_t CreateTexture(std::uint32_t width, std::uint32_t height) override {
					D3D11_TEXTURE2D_DESC description{};
					description.Width = width;
					description.Height = height;
					description.MipLevels = 1;
					description.ArraySize = 1;
					description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = 1;
					description.Usage = D3D11_USAGE_DEFAULT;
					description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					TextureEntry entry;
					if (FAILED(device->CreateTexture2D(&description, nullptr, &entry.texture))) return 0;
					device->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.view);
					const std::uint32_t handle = nextTexture++;
					textures[handle] = std::move(entry);
					return handle;
				}
				void UpdateTexture(std::uint32_t texture, const void* pixels, std::uint32_t pitch) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					context->UpdateSubresource(found->second.texture.Get(), 0, nullptr, pixels, pitch, 0);
				}
				void DestroyTexture(std::uint32_t texture) override { textures.erase(texture); }
				void CopyFrameToTexture(std::uint32_t texture, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t toX, std::uint32_t toY) override {
					const auto found = textures.find(texture);
					if (found == textures.end() || !frameTexture) return;
					Resolve();
					const D3D11_BOX box{ x, y, 0, x + width, y + height, 1 };
					context->CopySubresourceRegion(found->second.texture.Get(), 0, toX, toY, 0, frameTexture.Get(), 0, &box);
				}
				void ReadTexture(std::uint32_t texture, void* pixels, std::uint32_t pitch) override {
					const auto found = textures.find(texture);
					if (found == textures.end()) return;
					// Rare (see Backend.h), so its own staging copy each time.
					D3D11_TEXTURE2D_DESC description{};
					found->second.texture->GetDesc(&description);
					description.Usage = D3D11_USAGE_STAGING;
					description.BindFlags = 0;
					description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					description.MiscFlags = 0;
					ComPtr<ID3D11Texture2D> staging;
					if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) return;
					context->CopyResource(staging.Get(), found->second.texture.Get());
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
					for (std::uint32_t row = 0; row < description.Height; ++row) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch, static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
							static_cast<std::size_t>(description.Width) * 4);
					}
					context->Unmap(staging.Get(), 0);
				}

				void UploadFrame(const void* pixels, std::uint32_t pitch) override {
					if (!frameTexture) return;
					context->UpdateSubresource(frameTexture.Get(), 0, nullptr, pixels, pitch, 0);
					if (msaaView) CopyIntoMultisampled();
				}
				void ReadFrame(void* pixels, std::uint32_t pitch) override {
					if (!frameStaging) return;
					Resolve();
					ReadInto(frameTexture.Get(), pixels, pitch);
				}
				void ReadFrameArea(void* pixels, std::uint32_t pitch, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) override {
					if (!frameStaging) return;
					Resolve();
					// Into the same place of the staging copy, then just those rows and columns out.
					const D3D11_BOX box{ x, y, 0, x + width, y + height, 1 };
					context->CopySubresourceRegion(frameStaging.Get(), 0, x, y, 0, frameTexture.Get(), 0, &box);
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(frameStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
					for (std::uint32_t row = y; row < y + height; ++row) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(row) * pitch + static_cast<std::size_t>(x) * 4,
							static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch + static_cast<std::size_t>(x) * 4, static_cast<std::size_t>(width) * 4);
					}
					context->Unmap(frameStaging.Get(), 0);
				}
				void SaveShownFrame() override {
					if (!shownTexture) return;
					Resolve();
					context->CopyResource(shownTexture.Get(), frameTexture.Get());
				}
				void ReadShownFrame(void* pixels, std::uint32_t pitch) override {
					if (frameStaging && shownTexture) ReadInto(shownTexture.Get(), pixels, pitch);
				}
				// A frame-sized picture on the card into memory, through the staging copy.
				void ReadInto(ID3D11Texture2D* from, void* pixels, std::uint32_t pitch) {
					context->CopyResource(frameStaging.Get(), from);
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(frameStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
					for (std::uint32_t y = 0; y < frameHeight; ++y) {
						std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(y) * pitch, static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, static_cast<std::size_t>(frameWidth) * 4);
					}
					context->Unmap(frameStaging.Get(), 0);
				}
				void ClearDepth() override {
					if (depthView) context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
				}
				void ClearFrame(std::uint32_t pixel) override {
					float rgba[4];
					PixelToFloats(pixel, rgba);
					if (frameView) context->ClearRenderTargetView(frameView.Get(), rgba);
					if (msaaView) context->ClearRenderTargetView(msaaView.Get(), rgba);
					frameUnresolved = false;
				}

				void Draw(const DrawState& state, const Vertex* vertices, std::uint32_t vertexCount, const std::uint16_t* indices, std::uint32_t indexCount) override {
					if (!frameView) return;
					UINT firstVertex = 0;
					UINT firstIndex = 0;
					if (!Append(vertexBuffer, vertexCapacity, vertexOffset, D3D11_BIND_VERTEX_BUFFER, vertices, vertexCount * sizeof(Vertex), sizeof(Vertex), firstVertex)
						|| !Append(indexBuffer, indexCapacity, indexOffset, D3D11_BIND_INDEX_BUFFER, indices, indexCount * sizeof(std::uint16_t), sizeof(std::uint16_t), firstIndex)) {
						return;
					}
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (SUCCEEDED(context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
						std::memcpy(mapped.pData, &state.constants, sizeof(ShaderConstants));
						context->Unmap(constantBuffer.Get(), 0);
					}
					const UINT stride = sizeof(Vertex);
					const UINT offset = 0;
					context->IASetInputLayout(inputLayout.Get());
					context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
					context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
					context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context->VSSetShader(vertexShader.Get(), nullptr, 0);
					context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
					context->PSSetShader(pixelShader.Get(), nullptr, 0);
					context->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
					ID3D11ShaderResourceView* views[3];
					ID3D11SamplerState* samplers[3];
					for (int stage = 0; stage < 3; ++stage) {
						const auto found = textures.find(state.textures[stage] != 0 ? state.textures[stage] : whiteTexture);
						views[stage] = found != textures.end() ? found->second.view.Get() : nullptr;
						samplers[stage] = Sampler(state.samplers[stage]);
					}
					context->PSSetShaderResources(0, 3, views);
					context->PSSetSamplers(0, 3, samplers);
					context->RSSetState(rasterizer.Get());
					const float factor[4] = { 0, 0, 0, 0 };
					context->OMSetBlendState(BlendFor(state), factor, 0xFFFFFFFF);
					context->OMSetDepthStencilState(DepthFor(state), 0);
					ID3D11RenderTargetView* drawTarget = msaaView ? msaaView.Get() : frameView.Get();
					context->OMSetRenderTargets(1, &drawTarget, depthView.Get());
					D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					context->RSSetViewports(1, &viewport);
					context->DrawIndexed(indexCount, firstIndex, static_cast<INT>(firstVertex));
					if (msaaView) frameUnresolved = true;
				}

				void UploadScreen(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) override {
					if (!screenTexture || width != screenWidth || height != screenHeight) {
						screenTexture.Reset();
						screenView.Reset();
						D3D11_TEXTURE2D_DESC description{};
						description.Width = width;
						description.Height = height;
						description.MipLevels = 1;
						description.ArraySize = 1;
						description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
						description.SampleDesc.Count = 1;
						description.Usage = D3D11_USAGE_DEFAULT;
						description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
						if (FAILED(device->CreateTexture2D(&description, nullptr, &screenTexture))) return;
						device->CreateShaderResourceView(screenTexture.Get(), nullptr, &screenView);
						screenWidth = width;
						screenHeight = height;
					}
					context->UpdateSubresource(screenTexture.Get(), 0, nullptr, pixels, pitch, 0);
				}

				void Present(PresentSource source, const PresentRect& target, bool vsync) override {
					if (!swapChain || !ResizeToWindow()) return;
					ID3D11ShaderResourceView* view = screenView.Get();
					if (source == PresentSource::Frame) {
						Resolve();
						view = aaOutput.shaderView ? RunPost() : frameShaderView.Get();
					}
					const float black[4] = { 0, 0, 0, 1 };
					context->OMSetRenderTargets(1, backBufferView.GetAddressOf(), nullptr);
					context->ClearRenderTargetView(backBufferView.Get(), black);
					if (view) {
						D3D11_MAPPED_SUBRESOURCE mapped{};
						if (SUCCEEDED(context->Map(presentConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							const float rect[4] = {
								static_cast<float>(target.left) / backBufferWidth * 2.0f - 1.0f,
								1.0f - static_cast<float>(target.top) / backBufferHeight * 2.0f,
								static_cast<float>(target.right) / backBufferWidth * 2.0f - 1.0f,
								1.0f - static_cast<float>(target.bottom) / backBufferHeight * 2.0f,
							};
							std::memcpy(mapped.pData, rect, sizeof(rect));
							context->Unmap(presentConstants.Get(), 0);
						}
						D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(backBufferWidth), static_cast<float>(backBufferHeight), 0.0f, 1.0f };
						context->RSSetViewports(1, &viewport);
						context->RSSetState(rasterizer.Get());
						context->IASetInputLayout(nullptr);
						context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
						context->VSSetShader(presentVertexShader.Get(), nullptr, 0);
						context->VSSetConstantBuffers(0, 1, presentConstants.GetAddressOf());
						context->PSSetShader(presentPixelShader.Get(), nullptr, 0);
						context->PSSetShaderResources(0, 1, &view);
						context->PSSetSamplers(0, 1, presentSampler.GetAddressOf());
						const float factor[4] = { 0, 0, 0, 0 };
						context->OMSetBlendState(presentBlend.Get(), factor, 0xFFFFFFFF);
						context->OMSetDepthStencilState(presentDepth.Get(), 0);
						context->Draw(4, 0);
						// The frame is drawn into again next; it cannot stay bound as a shader input.
						ID3D11ShaderResourceView* none = nullptr;
						context->PSSetShaderResources(0, 1, &none);
					}
					const HRESULT result = swapChain->Present(vsync ? 1 : 0, 0);
					if (FAILED(result) && !reportedPresent) {
						reportedPresent = true;
						Logger::Warning("Renderer: DirectX 11 could not show a frame (" + HexResult(result) + ").");
					}
					// Not failures, but Windows saying the window cannot be seen (0x087A0001 is "occluded").
					if (SUCCEEDED(result) && result != S_OK && reportedStatus++ < 3) Logger::Debug("Renderer: DirectX 11 showed a frame with status " + HexResult(result) + ".");
				}

			private:
				struct TextureEntry {
					ComPtr<ID3D11Texture2D> texture;
					ComPtr<ID3D11ShaderResourceView> view;
				};
				HWND window = nullptr;
				int reportedStatus = 0;
				ComPtr<ID3D11Device> device;
				ComPtr<ID3D11DeviceContext> context;
				ComPtr<IDXGISwapChain> swapChain;
				ComPtr<ID3D11RenderTargetView> backBufferView;
				std::uint32_t backBufferWidth = 0;
				std::uint32_t backBufferHeight = 0;
				ComPtr<ID3D11VertexShader> vertexShader;
				ComPtr<ID3D11PixelShader> pixelShader;
				ComPtr<ID3D11VertexShader> presentVertexShader;
				ComPtr<ID3D11PixelShader> presentPixelShader;
				ComPtr<ID3D11InputLayout> inputLayout;
				ComPtr<ID3D11Buffer> constantBuffer;
				ComPtr<ID3D11Buffer> presentConstants;
				ComPtr<ID3D11Buffer> vertexBuffer;
				ComPtr<ID3D11Buffer> indexBuffer;
				UINT vertexCapacity = 0;
				UINT indexCapacity = 0;
				UINT vertexOffset = 0;
				UINT indexOffset = 0;
				ComPtr<ID3D11RasterizerState> rasterizer;
				ComPtr<ID3D11SamplerState> presentSampler;
				ComPtr<ID3D11BlendState> presentBlend;
				ComPtr<ID3D11DepthStencilState> presentDepth;
				std::unordered_map<std::uint32_t, ComPtr<ID3D11BlendState>> blendStates;
				std::unordered_map<std::uint32_t, ComPtr<ID3D11DepthStencilState>> depthStates;
				ComPtr<ID3D11SamplerState> samplers[SamplerKinds];
				ComPtr<ID3D11Texture2D> frameTexture;
				ComPtr<ID3D11RenderTargetView> frameView;
				ComPtr<ID3D11ShaderResourceView> frameShaderView;
				ComPtr<ID3D11Texture2D> frameStaging;
				// SaveShownFrame's copy of the frame.
				ComPtr<ID3D11Texture2D> shownTexture;
				ComPtr<ID3D11Texture2D> depthTexture;
				ComPtr<ID3D11DepthStencilView> depthView;
				std::uint32_t frameWidth = 0;
				std::uint32_t frameHeight = 0;
				ComPtr<ID3D11Texture2D> screenTexture;
				ComPtr<ID3D11ShaderResourceView> screenView;
				std::uint32_t screenWidth = 0;
				std::uint32_t screenHeight = 0;
				std::unordered_map<std::uint32_t, TextureEntry> textures;
				std::uint32_t nextTexture = 1;
				std::uint32_t whiteTexture = 0;
				bool reportedPresent = false;

				// ---- Anti-aliasing (see AntiAliasing in Backend.h).
				struct PostTarget {
					ComPtr<ID3D11Texture2D> texture;
					ComPtr<ID3D11RenderTargetView> targetView;
					ComPtr<ID3D11ShaderResourceView> shaderView;
				};
				// MSAA: samples per pixel (1 = off), the multisampled color the draws go into, and whether it holds
				// draws frameTexture does not have yet.
				UINT msaaSamples = 1;
				ComPtr<ID3D11Texture2D> msaaColor;
				ComPtr<ID3D11RenderTargetView> msaaView;
				bool frameUnresolved = false;
				ComPtr<ID3D11PixelShader> copyPixelShader;
				// FXAA uses pass 0; SMAA passes 0-2 are edges, weights and blending.
				bool postShaders = false;
				ComPtr<ID3D11VertexShader> postVertex[3];
				ComPtr<ID3D11PixelShader> postPixel[3];
				ComPtr<ID3D11Buffer> postConstants;
				ComPtr<ID3D11SamplerState> pointSampler;
				PostTarget aaOutput;
				PostTarget smaaEdges;
				PostTarget smaaWeights;

				// Turns the asked-for anti-aliasing down (MSAA samples the card cannot do) or off (shaders that do not
				// compile), and says in the log what is used.
				void SetUpAntiAliasing() {
					const AntiAliasing asked = antiAliasing;
					if (asked.mode == AntiAliasingMode::Msaa) {
						UINT samples = asked.level;
						while (samples > 1 && !SupportsSamples(samples)) samples /= 2;
						ComPtr<ID3DBlob> copy;
						if (samples > 1 && CompileHlsl(Shaders::PresentHlsl, "CopyPixel", "ps_4_0", copy)) device->CreatePixelShader(copy->GetBufferPointer(), copy->GetBufferSize(), nullptr, &copyPixelShader);
						if (samples > 1 && copyPixelShader) {
							msaaSamples = samples;
							antiAliasing.level = samples;
							D3D11_RASTERIZER_DESC raster{};
							raster.FillMode = D3D11_FILL_SOLID;
							raster.CullMode = D3D11_CULL_NONE;
							raster.DepthClipEnable = TRUE;
							raster.MultisampleEnable = TRUE;
							rasterizer.Reset();
							device->CreateRasterizerState(&raster, &rasterizer);
						}
						else antiAliasing = {};
					}
					else if (asked.mode != AntiAliasingMode::Off) {
						if (MakePostShaders()) postShaders = true;
						else antiAliasing = {};
					}
					if (antiAliasing == asked) Logger::Info("Renderer: DirectX 11 anti-aliasing: " + AntiAliasingName(antiAliasing) + ".");
					else Logger::Warning("Renderer: DirectX 11 cannot do " + AntiAliasingName(asked) + " here; it uses " + AntiAliasingName(antiAliasing) + ".");
				}
				bool SupportsSamples(UINT samples) {
					UINT colorLevels = 0;
					UINT depthLevels = 0;
					return SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &colorLevels)) && colorLevels > 0
						&& SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, samples, &depthLevels)) && depthLevels > 0;
				}
				bool MakePostShaders() {
					const std::string source = PostHlsl(antiAliasing);
					const bool smaa = antiAliasing.mode == AntiAliasingMode::Smaa;
					const char* vertexEntries[3] = { "PostVertex", nullptr, nullptr };
					const char* pixelEntries[3] = { "FxaaPixel", nullptr, nullptr };
					if (smaa) {
						vertexEntries[1] = vertexEntries[2] = "PostVertex";
						pixelEntries[0] = "SmaaEdgePixel";
						pixelEntries[1] = "SmaaWeightPixel";
						pixelEntries[2] = "SmaaBlendPixel";
					}
					for (int pass = 0; pass < (smaa ? 3 : 1); ++pass) {
						ComPtr<ID3DBlob> vertex;
						ComPtr<ID3DBlob> pixel;
						if (!CompileHlsl(source.c_str(), vertexEntries[pass], "vs_4_0", vertex) || !CompileHlsl(source.c_str(), pixelEntries[pass], "ps_4_0", pixel)) return false;
						if (FAILED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &postVertex[pass]))
							|| FAILED(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &postPixel[pass]))) {
							return false;
						}
					}
					D3D11_BUFFER_DESC constants{};
					constants.ByteWidth = sizeof(PostConstants);
					constants.Usage = D3D11_USAGE_DYNAMIC;
					constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					if (FAILED(device->CreateBuffer(&constants, nullptr, &postConstants))) return false;
					D3D11_SAMPLER_DESC point{};
					point.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
					point.AddressU = point.AddressV = point.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
					point.MaxLOD = D3D11_FLOAT32_MAX;
					return SUCCEEDED(device->CreateSamplerState(&point, &pointSampler));
				}
				bool MakePostTarget(PostTarget& target, DXGI_FORMAT format) {
					D3D11_TEXTURE2D_DESC description{};
					description.Width = frameWidth;
					description.Height = frameHeight;
					description.MipLevels = 1;
					description.ArraySize = 1;
					description.Format = format;
					description.SampleDesc.Count = 1;
					description.Usage = D3D11_USAGE_DEFAULT;
					description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
					return SUCCEEDED(device->CreateTexture2D(&description, nullptr, &target.texture))
						&& SUCCEEDED(device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.targetView))
						&& SUCCEEDED(device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.shaderView));
				}
				// MSAA: brings frameTexture up to date with the draws.
				void Resolve() {
					if (!msaaColor || !frameUnresolved) return;
					context->ResolveSubresource(frameTexture.Get(), 0, msaaColor.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
					frameUnresolved = false;
				}
				// MSAA: frameTexture (just filled from memory) into every sample of the multisampled frame.
				void CopyIntoMultisampled() {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(presentConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
					const float whole[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
					std::memcpy(mapped.pData, whole, sizeof(whole));
					context->Unmap(presentConstants.Get(), 0);
					context->OMSetRenderTargets(1, msaaView.GetAddressOf(), nullptr);
					D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					context->RSSetViewports(1, &viewport);
					context->RSSetState(rasterizer.Get());
					context->IASetInputLayout(nullptr);
					context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					context->VSSetShader(presentVertexShader.Get(), nullptr, 0);
					context->VSSetConstantBuffers(0, 1, presentConstants.GetAddressOf());
					context->PSSetShader(copyPixelShader.Get(), nullptr, 0);
					ID3D11ShaderResourceView* view = frameShaderView.Get();
					context->PSSetShaderResources(0, 1, &view);
					const float factor[4] = { 0, 0, 0, 0 };
					context->OMSetBlendState(presentBlend.Get(), factor, 0xFFFFFFFF);
					context->OMSetDepthStencilState(presentDepth.Get(), 0);
					context->Draw(4, 0);
					ID3D11ShaderResourceView* none = nullptr;
					context->PSSetShaderResources(0, 1, &none);
					frameUnresolved = false;
				}
				// FXAA or SMAA from frameTexture into aaOutput, which is returned to be shown.
				ID3D11ShaderResourceView* RunPost() {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(postConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return frameShaderView.Get();
					const PostConstants constants = MakePostConstants(antiAliasing, frameWidth, frameHeight);
					std::memcpy(mapped.pData, &constants, sizeof(constants));
					context->Unmap(postConstants.Get(), 0);
					D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, 1.0f };
					context->RSSetViewports(1, &viewport);
					context->RSSetState(rasterizer.Get());
					context->IASetInputLayout(nullptr);
					context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context->VSSetConstantBuffers(0, 1, postConstants.GetAddressOf());
					context->PSSetConstantBuffers(0, 1, postConstants.GetAddressOf());
					ID3D11SamplerState* postSamplers[2] = { presentSampler.Get(), pointSampler.Get() };
					context->PSSetSamplers(0, 2, postSamplers);
					const float factor[4] = { 0, 0, 0, 0 };
					context->OMSetBlendState(presentBlend.Get(), factor, 0xFFFFFFFF);
					context->OMSetDepthStencilState(presentDepth.Get(), 0);
					if (antiAliasing.mode == AntiAliasingMode::Smaa) {
						const float clear[4] = { 0, 0, 0, 0 };
						context->ClearRenderTargetView(smaaEdges.targetView.Get(), clear);
						context->ClearRenderTargetView(smaaWeights.targetView.Get(), clear);
						PostPass(0, smaaEdges, { frameShaderView.Get(), nullptr, nullptr });
						PostPass(1, smaaWeights, { smaaEdges.shaderView.Get(), nullptr, nullptr });
						PostPass(2, aaOutput, { frameShaderView.Get(), smaaWeights.shaderView.Get(), nullptr });
					}
					else PostPass(0, aaOutput, { frameShaderView.Get(), nullptr, nullptr });
					ID3D11ShaderResourceView* none[3] = {};
					context->PSSetShaderResources(0, 3, none);
					context->OMSetRenderTargets(0, nullptr, nullptr);
					return aaOutput.shaderView.Get();
				}
				void PostPass(int pass, PostTarget& target, std::initializer_list<ID3D11ShaderResourceView*> inputs) {
					// The target first: one pass's target is the next one's input, and cannot be both at once.
					context->OMSetRenderTargets(1, target.targetView.GetAddressOf(), nullptr);
					ID3D11ShaderResourceView* views[3] = {};
					std::copy(inputs.begin(), inputs.end(), views);
					context->PSSetShaderResources(0, 3, views);
					context->VSSetShader(postVertex[pass].Get(), nullptr, 0);
					context->PSSetShader(postPixel[pass].Get(), nullptr, 0);
					context->Draw(3, 0);
				}

				bool Fail(const char* what, HRESULT result) {
					Logger::Warning(std::string("Renderer: DirectX 11 ") + what + " (" + HexResult(result) + ").");
					return false;
				}

				bool MakeSwapChain() {
					ComPtr<IDXGIDevice> dxgiDevice;
					ComPtr<IDXGIAdapter> adapter;
					ComPtr<IDXGIFactory2> factory;
					if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
						return Fail("could not reach DXGI", E_FAIL);
					}
					RECT client{};
					GetClientRect(window, &client);
					DXGI_SWAP_CHAIN_DESC1 description{};
					description.Width = std::max<LONG>(client.right - client.left, 1);
					description.Height = std::max<LONG>(client.bottom - client.top, 1);
					description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					description.SampleDesc.Count = 1;
					description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					description.Scaling = DXGI_SCALING_STRETCH;
					// Flip first; the older blit swap chain only where flip is not available (before Windows 10). (Both
					// showed nothing while DDrawCompat's hooks were in, see docs/game/renderer.md.)
					description.BufferCount = 2;
					description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
					ComPtr<IDXGISwapChain1> made;
					HRESULT result = factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &made);
					if (FAILED(result)) {
						description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
						result = factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &made);
					}
					if (FAILED(result)) {
						description.BufferCount = 1;
						description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
						result = factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &made);
					}
					if (FAILED(result)) return Fail("could not make a swap chain on the game's window", result);
					// For the log: the graphics card, the screen Windows puts the window on, and the kind of swap chain.
					DXGI_ADAPTER_DESC card{};
					adapter->GetDesc(&card);
					DXGI_OUTPUT_DESC screen{};
					ComPtr<IDXGIOutput> output;
					const bool onScreen = SUCCEEDED(made->GetContainingOutput(&output)) && SUCCEEDED(output->GetDesc(&screen));
					char text[256];
					std::snprintf(text, sizeof(text), "Renderer: DirectX 11 uses %ls, %s swap chain %ux%u, on %ls.", card.Description,
						description.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? "blit" : "flip", description.Width, description.Height,
						onScreen ? screen.DeviceName : L"no screen Windows could name");
					Logger::Info(text);
					// The game handles its own window; DXGI must not switch it to full screen or resize it.
					factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
					made.As(&swapChain);
					return MakeBackBufferView();
				}
				bool MakeBackBufferView() {
					ComPtr<ID3D11Texture2D> buffer;
					if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
					D3D11_TEXTURE2D_DESC description{};
					buffer->GetDesc(&description);
					backBufferWidth = description.Width;
					backBufferHeight = description.Height;
					return SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &backBufferView));
				}
				bool ResizeToWindow() {
					RECT client{};
					GetClientRect(window, &client);
					const UINT width = static_cast<UINT>(std::max<LONG>(client.right - client.left, 1));
					const UINT height = static_cast<UINT>(std::max<LONG>(client.bottom - client.top, 1));
					if (width == backBufferWidth && height == backBufferHeight && backBufferView) return true;
					context->OMSetRenderTargets(0, nullptr, nullptr);
					backBufferView.Reset();
					if (FAILED(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) return false;
					return MakeBackBufferView();
				}

				bool GrowBuffer(ComPtr<ID3D11Buffer>& buffer, UINT& capacity, UINT size, UINT bind) {
					D3D11_BUFFER_DESC description{};
					description.ByteWidth = size;
					description.Usage = D3D11_USAGE_DYNAMIC;
					description.BindFlags = bind;
					description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					buffer.Reset();
					if (FAILED(device->CreateBuffer(&description, nullptr, &buffer))) {
						capacity = 0;
						return false;
					}
					capacity = size;
					return true;
				}
				// Writes data after what the buffer already holds this time round (starting over, and dropping the
				// old contents, when it is full). first is where it starts, in elements.
				bool Append(ComPtr<ID3D11Buffer>& buffer, UINT& capacity, UINT& used, UINT bind, const void* data, std::size_t size, UINT element, UINT& first) {
					if (size > capacity) {
						UINT grown = capacity;
						while (grown < size) grown *= 2;
						if (!GrowBuffer(buffer, capacity, grown, bind)) return false;
						used = capacity;
					}
					// Keep each start a whole number of elements in.
					used = (used + element - 1) / element * element;
					D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
					if (used + size > capacity) {
						used = 0;
						mode = D3D11_MAP_WRITE_DISCARD;
					}
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(buffer.Get(), 0, mode, 0, &mapped))) return false;
					std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + used, data, size);
					context->Unmap(buffer.Get(), 0);
					first = used / element;
					used += static_cast<UINT>(size);
					return true;
				}

				static D3D11_BLEND AlphaFactor(Blend blend) {
					switch (blend) {
						case Blend::SrcColor: return D3D11_BLEND_SRC_ALPHA;
						case Blend::InvSrcColor: return D3D11_BLEND_INV_SRC_ALPHA;
						case Blend::DestColor: return D3D11_BLEND_DEST_ALPHA;
						case Blend::InvDestColor: return D3D11_BLEND_INV_DEST_ALPHA;
						default: return static_cast<D3D11_BLEND>(blend);
					}
				}
				ID3D11BlendState* BlendFor(const DrawState& state) {
					const std::uint32_t key = (state.blend ? 1u : 0u) | static_cast<std::uint32_t>(state.source) << 1 | static_cast<std::uint32_t>(state.destination) << 5;
					auto& made = blendStates[key];
					if (!made) {
						D3D11_BLEND_DESC description{};
						auto& target = description.RenderTarget[0];
						target.BlendEnable = state.blend;
						// D3DBLEND and D3D11_BLEND number their factors the same way.
						target.SrcBlend = static_cast<D3D11_BLEND>(state.source);
						target.DestBlend = static_cast<D3D11_BLEND>(state.destination);
						target.BlendOp = D3D11_BLEND_OP_ADD;
						target.SrcBlendAlpha = AlphaFactor(state.source);
						target.DestBlendAlpha = AlphaFactor(state.destination);
						target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
						target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
						device->CreateBlendState(&description, &made);
					}
					return made.Get();
				}
				ID3D11DepthStencilState* DepthFor(const DrawState& state) {
					const std::uint32_t key = (state.depthTest ? 1u : 0u) | (state.depthWrite ? 2u : 0u) | static_cast<std::uint32_t>(state.depthFunc) << 2;
					auto& made = depthStates[key];
					if (!made) {
						D3D11_DEPTH_STENCIL_DESC description{};
						description.DepthEnable = state.depthTest;
						description.DepthWriteMask = state.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
						// D3DCMPFUNC and D3D11_COMPARISON_FUNC number the same way.
						description.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(state.depthFunc);
						device->CreateDepthStencilState(&description, &made);
					}
					return made.Get();
				}
				ID3D11SamplerState* Sampler(const SamplerState& state) {
					auto& made = samplers[state.Key()];
					if (!made) {
						D3D11_SAMPLER_DESC description{};
						description.Filter = state.linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
						// D3DTEXTUREADDRESS and D3D11_TEXTURE_ADDRESS_MODE number the same way.
						description.AddressU = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(state.addressU);
						description.AddressV = static_cast<D3D11_TEXTURE_ADDRESS_MODE>(state.addressV);
						description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
						description.MaxLOD = D3D11_FLOAT32_MAX;
						description.ComparisonFunc = D3D11_COMPARISON_NEVER;
						device->CreateSamplerState(&description, &made);
					}
					return made.Get();
				}
		};
	}

	std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }
}
