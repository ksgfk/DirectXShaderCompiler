///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// radray_abi_probe.cpp                                                       //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Verifies the RadRay extension entry point in a built DXC binary.           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/WinIncludes.h"
#include "dxc/dxcapi_radrayext.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using DxcCreateInstanceFn = HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *);

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
