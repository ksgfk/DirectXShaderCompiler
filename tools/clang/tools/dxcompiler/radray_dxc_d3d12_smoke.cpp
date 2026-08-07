///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// radray_dxc_d3d12_smoke.cpp                                                //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Verifies the default DXIL compiler output with an independent D3D12 draw. //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxc/dxcapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 64;
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

bool CheckHr(HRESULT hr, const char *operation) {
  if (SUCCEEDED(hr))
    return true;
  std::fprintf(stderr, "%s failed: 0x%08lx\n", operation,
               static_cast<unsigned long>(hr));
  return false;
}

bool CompileShader(IDxcCompiler3 *compiler, const char *source,
                   const wchar_t *entryPoint, const wchar_t *profile,
                   ComPtr<IDxcBlob> *output) {
  const DxcBuffer sourceBuffer{source, std::strlen(source), DXC_CP_UTF8};
  const wchar_t *arguments[] = {L"-E", entryPoint, L"-T", profile,
                                L"-HV", L"2021"};
  ComPtr<IDxcResult> result;
  if (!CheckHr(compiler->Compile(
                   &sourceBuffer, arguments,
                   static_cast<UINT32>(sizeof(arguments) / sizeof(arguments[0])),
                   nullptr, IID_PPV_ARGS(&result)),
               "IDxcCompiler3::Compile")) {
    return false;
  }

  HRESULT status = E_FAIL;
  if (!CheckHr(result->GetStatus(&status), "IDxcResult::GetStatus") ||
      FAILED(status)) {
    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors),
                                    nullptr)) &&
        errors != nullptr && errors->GetStringLength() != 0) {
      std::fprintf(stderr, "%s\n", errors->GetStringPointer());
    }
    return false;
  }
  ComPtr<IDxcBlob> object;
  if (!CheckHr(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object),
                                nullptr),
               "IDxcResult::GetOutput(DXC_OUT_OBJECT)")) {
    return false;
  }
  *output = object;
  return *output != nullptr;
}

bool CreateHardwareDevice(ComPtr<IDXGIFactory1> &factory,
                           ComPtr<ID3D12Device> &device) {
  if (!CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
               "CreateDXGIFactory1")) {
    return false;
  }

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumHr = factory->EnumAdapters1(index, &adapter);
    if (enumHr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(enumHr))
      continue;

    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description)) ||
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&device)))) {
      std::fwprintf(stdout, L"Using D3D12 adapter: %ls\n",
                    description.Description);
      return true;
    }
  }

  std::fprintf(stderr, "no hardware D3D12 adapter supports feature level 11_0\n");
  return false;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size) {
  D3D12_RESOURCE_DESC description{};
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Width = size;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.SampleDesc.Count = 1;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return description;
}

bool RunSmoke() {
  static constexpr char source[] = R"hlsl(
struct VSOutput {
  float4 position : SV_Position;
};

VSOutput VSMain(float3 position : POSITION) {
  VSOutput output;
  output.position = float4(position, 1.0f);
  return output;
}

float4 PSMain() : SV_Target0 {
  return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)hlsl";

  ComPtr<IDxcCompiler3> compiler;
  if (!CheckHr(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
               "DxcCreateInstance(CLSID_DxcCompiler)")) {
    return false;
  }
  ComPtr<IDxcBlob> vertexShader;
  ComPtr<IDxcBlob> pixelShader;
  if (!CompileShader(compiler.Get(), source, L"VSMain", L"vs_6_0",
                     &vertexShader) ||
      !CompileShader(compiler.Get(), source, L"PSMain", L"ps_6_0",
                     &pixelShader)) {
    return false;
  }

  ComPtr<IDXGIFactory1> factory;
  ComPtr<ID3D12Device> device;
  if (!CreateHardwareDevice(factory, device))
    return false;

  D3D12_COMMAND_QUEUE_DESC queueDescription{};
  queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  if (!CheckHr(device->CreateCommandQueue(&queueDescription,
                                          IID_PPV_ARGS(&queue)),
               "ID3D12Device::CreateCommandQueue")) {
    return false;
  }

  D3D12_ROOT_SIGNATURE_DESC rootDescription{};
  rootDescription.Flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> rootBlob;
  ComPtr<ID3DBlob> rootErrors;
  if (!CheckHr(D3D12SerializeRootSignature(
                   &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob,
                   &rootErrors),
               "D3D12SerializeRootSignature")) {
    return false;
  }
  ComPtr<ID3D12RootSignature> rootSignature;
  if (!CheckHr(device->CreateRootSignature(
                   0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                   IID_PPV_ARGS(&rootSignature)),
               "ID3D12Device::CreateRootSignature")) {
    return false;
  }

  D3D12_INPUT_ELEMENT_DESC inputElement{};
  inputElement.SemanticName = "POSITION";
  inputElement.Format = DXGI_FORMAT_R32G32B32_FLOAT;
  inputElement.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

  D3D12_BLEND_DESC blendDescription{};
  blendDescription.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  D3D12_RASTERIZER_DESC rasterizerDescription{};
  rasterizerDescription.FillMode = D3D12_FILL_MODE_SOLID;
  rasterizerDescription.CullMode = D3D12_CULL_MODE_NONE;
  rasterizerDescription.DepthClipEnable = TRUE;
  D3D12_DEPTH_STENCIL_DESC depthDescription{};

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
  pipelineDescription.pRootSignature = rootSignature.Get();
  pipelineDescription.VS = {vertexShader->GetBufferPointer(),
                            vertexShader->GetBufferSize()};
  pipelineDescription.PS = {pixelShader->GetBufferPointer(),
                            pixelShader->GetBufferSize()};
  pipelineDescription.BlendState = blendDescription;
  pipelineDescription.SampleMask = std::numeric_limits<UINT>::max();
  pipelineDescription.RasterizerState = rasterizerDescription;
  pipelineDescription.DepthStencilState = depthDescription;
  pipelineDescription.InputLayout = {&inputElement, 1};
  pipelineDescription.PrimitiveTopologyType =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineDescription.NumRenderTargets = 1;
  pipelineDescription.RTVFormats[0] = kRenderTargetFormat;
  pipelineDescription.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  if (!CheckHr(device->CreateGraphicsPipelineState(
                   &pipelineDescription, IID_PPV_ARGS(&pipeline)),
               "ID3D12Device::CreateGraphicsPipelineState")) {
    return false;
  }

  D3D12_RESOURCE_DESC renderTargetDescription{};
  renderTargetDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  renderTargetDescription.Width = kWidth;
  renderTargetDescription.Height = kHeight;
  renderTargetDescription.DepthOrArraySize = 1;
  renderTargetDescription.MipLevels = 1;
  renderTargetDescription.Format = kRenderTargetFormat;
  renderTargetDescription.SampleDesc.Count = 1;
  renderTargetDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clearValue{};
  clearValue.Format = kRenderTargetFormat;
  clearValue.Color[0] = 0.0f;
  clearValue.Color[1] = 0.0f;
  clearValue.Color[2] = 0.0f;
  clearValue.Color[3] = 1.0f;
  ComPtr<ID3D12Resource> renderTarget;
  const D3D12_HEAP_PROPERTIES defaultHeap =
      HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  if (!CheckHr(device->CreateCommittedResource(
                   &defaultHeap, D3D12_HEAP_FLAG_NONE,
                   &renderTargetDescription, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   &clearValue, IID_PPV_ARGS(&renderTarget)),
               "ID3D12Device::CreateCommittedResource(render target)")) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
  rtvHeapDescription.NumDescriptors = 1;
  rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  if (!CheckHr(device->CreateDescriptorHeap(&rtvHeapDescription,
                                            IID_PPV_ARGS(&rtvHeap)),
               "ID3D12Device::CreateDescriptorHeap")) {
    return false;
  }
  device->CreateRenderTargetView(renderTarget.Get(), nullptr,
                                 rtvHeap->GetCPUDescriptorHandleForHeapStart());

  struct Vertex {
    float x;
    float y;
    float z;
  };
  const Vertex vertices[] = {{-1.0f, -1.0f, 0.0f},
                             {-1.0f, 3.0f, 0.0f},
                             {3.0f, -1.0f, 0.0f}};
  ComPtr<ID3D12Resource> vertexBuffer;
  const D3D12_RESOURCE_DESC vertexBufferDescription =
      BufferDescription(sizeof(vertices));
  const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
  if (!CheckHr(device->CreateCommittedResource(
                   &uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexBufferDescription,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                   IID_PPV_ARGS(&vertexBuffer)),
               "ID3D12Device::CreateCommittedResource(vertex buffer)")) {
    return false;
  }
  void *vertexData = nullptr;
  if (!CheckHr(vertexBuffer->Map(0, nullptr, &vertexData),
               "ID3D12Resource::Map(vertex buffer)")) {
    return false;
  }
  std::memcpy(vertexData, vertices, sizeof(vertices));
  vertexBuffer->Unmap(0, nullptr);
  D3D12_VERTEX_BUFFER_VIEW vertexView{};
  vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
  vertexView.SizeInBytes = sizeof(vertices);
  vertexView.StrideInBytes = sizeof(Vertex);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 rowSize = 0;
  UINT64 readbackSize = 0;
  device->GetCopyableFootprints(&renderTargetDescription, 0, 1, 0,
                                &footprint, &rows, &rowSize, &readbackSize);
  if (readbackSize == 0) {
    std::fprintf(stderr, "invalid D3D12 readback footprint\n");
    return false;
  }
  ComPtr<ID3D12Resource> readback;
  const D3D12_RESOURCE_DESC readbackDescription =
      BufferDescription(readbackSize);
  const D3D12_HEAP_PROPERTIES readbackHeap =
      HeapProperties(D3D12_HEAP_TYPE_READBACK);
  if (!CheckHr(device->CreateCommittedResource(
                   &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                   IID_PPV_ARGS(&readback)),
               "ID3D12Device::CreateCommittedResource(readback)")) {
    return false;
  }

  ComPtr<ID3D12CommandAllocator> allocator;
  if (!CheckHr(device->CreateCommandAllocator(
                   D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "ID3D12Device::CreateCommandAllocator")) {
    return false;
  }
  ComPtr<ID3D12GraphicsCommandList> commandList;
  if (!CheckHr(device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                   pipeline.Get(), IID_PPV_ARGS(&commandList)),
               "ID3D12Device::CreateCommandList")) {
    return false;
  }
  commandList->SetGraphicsRootSignature(rootSignature.Get());
  const D3D12_VIEWPORT viewport = {
      0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight),
      0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, static_cast<LONG>(kWidth),
                              static_cast<LONG>(kHeight)};
  commandList->RSSetViewports(1, &viewport);
  commandList->RSSetScissorRects(1, &scissor);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      rtvHeap->GetCPUDescriptorHandleForHeapStart();
  commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
  const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
  commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commandList->IASetVertexBuffers(0, 1, &vertexView);
  commandList->DrawInstanced(3, 1, 0, 0);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = renderTarget.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  commandList->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
  sourceLocation.pResource = renderTarget.Get();
  sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  sourceLocation.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
  destinationLocation.pResource = readback.Get();
  destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destinationLocation.PlacedFootprint = footprint;
  commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0,
                                  &sourceLocation, nullptr);
  if (!CheckHr(commandList->Close(), "ID3D12GraphicsCommandList::Close"))
    return false;

  ID3D12CommandList *commandLists[] = {commandList.Get()};
  queue->ExecuteCommandLists(1, commandLists);
  ComPtr<ID3D12Fence> fence;
  if (!CheckHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence)),
               "ID3D12Device::CreateFence")) {
    return false;
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) {
    std::fprintf(stderr, "CreateEventW failed: %lu\n",
                 static_cast<unsigned long>(GetLastError()));
    return false;
  }
  constexpr UINT64 kFenceValue = 1;
  bool completed = CheckHr(queue->Signal(fence.Get(), kFenceValue),
                           "ID3D12CommandQueue::Signal");
  if (completed && fence->GetCompletedValue() < kFenceValue) {
    completed = CheckHr(fence->SetEventOnCompletion(kFenceValue, event),
                        "ID3D12Fence::SetEventOnCompletion") &&
                WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    if (!completed)
      std::fprintf(stderr, "D3D12 fence wait timed out or failed\n");
  }
  CloseHandle(event);
  if (!completed)
    return false;

  D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackSize)};
  uint8_t *readbackData = nullptr;
  if (!CheckHr(readback->Map(0, &readRange,
                             reinterpret_cast<void **>(&readbackData)),
               "ID3D12Resource::Map(readback)")) {
    return false;
  }
  const size_t centerOffset =
      static_cast<size_t>(footprint.Offset) +
      static_cast<size_t>(footprint.Footprint.RowPitch) * (kHeight / 2) +
      static_cast<size_t>(kWidth / 2) * 4;
  const uint8_t expected[] = {255, 0, 255, 255};
  const bool pixelMatches =
      std::memcmp(readbackData + centerOffset, expected, sizeof(expected)) == 0;
  uint8_t actual[sizeof(expected)]{};
  std::memcpy(actual, readbackData + centerOffset, sizeof(actual));
  D3D12_RANGE writeRange{0, 0};
  readback->Unmap(0, &writeRange);
  if (!pixelMatches) {
    std::fprintf(stderr, "unexpected center pixel: %u %u %u %u\n", actual[0],
                 actual[1], actual[2], actual[3]);
    return false;
  }
  std::fprintf(stdout, "D3D12 DXIL draw/readback passed\n");
  return true;
}

} // namespace

int main() { return RunSmoke() ? 0 : 1; }
