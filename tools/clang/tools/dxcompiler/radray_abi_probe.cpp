///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// radray_abi_probe.cpp                                                       //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Verifies the RadRay extension entry point in a built DXC binary.           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include "dxc/dxcapi_radrayext.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using DxcCreateInstanceFn = HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *);

void AppendU16(std::vector<uint8_t> &bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32(std::vector<uint8_t> &bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void AppendString(std::vector<uint8_t> &bytes, const char *value) {
  const size_t size = std::strlen(value);
  AppendU32(bytes, static_cast<uint32_t>(size));
  bytes.insert(bytes.end(), value, value + size);
}

bool CheckDiscovery(radray::shader::IRadRayDxcCompiler *compiler) {
  constexpr char source[] =
      "[shader(\"vertex\")] float4 VSMain(float3 position : POSITION) : SV_Position {"
      " return float4(position, 1.0); }\n";
  std::vector<uint8_t> request;
  AppendU32(request, radray::shader::kRadRayDxcDiscoveryWireMagic);
  AppendU16(request, radray::shader::kRadRayDxcDiscoveryWireSchemaVersion);
  AppendString(request, "probe.hlsl");
  AppendU32(request, static_cast<uint32_t>(std::strlen(source)));
  request.insert(request.end(), source, source + std::strlen(source));
  request.push_back(static_cast<uint8_t>(radray::shader::RadRayDxcTarget::DXIL));

  radray::shader::RadRayDxcBlobView input{
      request.data(), static_cast<uint32_t>(request.size())};
  radray::shader::IRadRayDxcResult *result = nullptr;
  const HRESULT hr = compiler->DiscoverSourceContract(input, &result);
  if (FAILED(hr) || result == nullptr) {
    fwprintf(stderr, L"fork discovery call failed: 0x%08lx\n",
             static_cast<unsigned long>(hr));
    return false;
  }
  radray::shader::RadRayDxcCompileStatus status{};
  radray::shader::RadRayDxcBlobView contract{};
  const HRESULT statusHr = result->GetStatus(&status);
  const HRESULT contractHr = result->GetContractBlob(&contract);
  const bool valid = SUCCEEDED(statusHr) &&
                     status == radray::shader::RadRayDxcCompileStatus::Success &&
                     SUCCEEDED(contractHr) && contract.Data != nullptr &&
                     contract.Size != 0;
  if (!valid) {
    fwprintf(stderr, L"fork discovery result is invalid: status=0x%08lx contract=0x%08lx\n",
             static_cast<unsigned long>(statusHr),
             static_cast<unsigned long>(contractHr));
  }
  result->Release();
  return valid;
}

bool CheckFork(const wchar_t *path) {
  HMODULE module = LoadLibraryW(path);
  if (module == nullptr) {
    fwprintf(stderr, L"failed to load fork DLL: %ls\n", path);
    return false;
  }

  auto createInstance = reinterpret_cast<DxcCreateInstanceFn>(
      GetProcAddress(module, "DxcCreateInstance"));
  if (createInstance == nullptr) {
    fwprintf(stderr, L"fork DLL has no DxcCreateInstance: %ls\n", path);
    FreeLibrary(module);
    return false;
  }

  radray::shader::IRadRayDxcCompiler *compiler = nullptr;
  HRESULT hr = createInstance(radray::shader::CLSID_RadRayDxcCompiler,
                              radray::shader::IID_IRadRayDxcCompiler,
                              reinterpret_cast<void **>(&compiler));
  if (FAILED(hr) || compiler == nullptr) {
    fwprintf(stderr, L"fork DLL rejected RadRay CLSID: 0x%08lx\n",
             static_cast<unsigned long>(hr));
    FreeLibrary(module);
    return false;
  }

  radray::shader::RadRayDxcAbiInfo info = {};
  hr = compiler->GetAbiInfo(&info);
  const bool identityPresent =
      std::memcmp(info.ToolchainIdentity.Bytes,
                  radray::shader::RadRayDxcHash128{}.Bytes,
                  sizeof(info.ToolchainIdentity.Bytes)) != 0;
  const bool valid = SUCCEEDED(hr) &&
                     info.AbiVersion == radray::shader::kRadRayDxcAbiVersion &&
                     info.MetadataSchemaVersion ==
                         radray::shader::kRadRayDxcMetadataSchemaVersion &&
                     identityPresent;
  if (valid && !CheckDiscovery(compiler)) {
    compiler->Release();
    FreeLibrary(module);
    return false;
  }
  compiler->Release();
  FreeLibrary(module);
  if (!valid) {
    fwprintf(stderr, L"fork DLL returned invalid RadRay ABI: 0x%08lx\n",
             static_cast<unsigned long>(hr));
  }
  return valid;
}

bool CheckStockRejects(const wchar_t *path) {
  HMODULE module = LoadLibraryW(path);
  if (module == nullptr) {
    fwprintf(stderr, L"failed to load stock DLL: %ls\n", path);
    return false;
  }

  auto createInstance = reinterpret_cast<DxcCreateInstanceFn>(
      GetProcAddress(module, "DxcCreateInstance"));
  if (createInstance == nullptr) {
    fwprintf(stderr, L"stock DLL has no DxcCreateInstance: %ls\n", path);
    FreeLibrary(module);
    return false;
  }

  void *object = nullptr;
  const HRESULT hr = createInstance(radray::shader::CLSID_RadRayDxcCompiler,
                                    radray::shader::IID_IRadRayDxcCompiler,
                                    &object);
  const bool rejected = FAILED(hr) && object == nullptr;
  if (!rejected) {
    fwprintf(stderr,
             L"stock DLL unexpectedly accepted RadRay CLSID: 0x%08lx\n",
             static_cast<unsigned long>(hr));
    if (object != nullptr)
      static_cast<IUnknown *>(object)->Release();
  }
  FreeLibrary(module);
  return rejected;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2 || argc > 3) {
    fwprintf(stderr, L"usage: radray_abi_probe <fork-dll> [stock-dll]\n");
    return 2;
  }
  if (!CheckFork(argv[1]))
    return 1;
  if (argc == 3 && !CheckStockRejects(argv[2]))
    return 1;
  return 0;
}
