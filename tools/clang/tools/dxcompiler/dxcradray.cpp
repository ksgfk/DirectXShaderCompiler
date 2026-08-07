///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxcradray.cpp                                                             //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Implements the RadRay-owned DXC extension factory and ABI handshake.      //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/WinIncludes.h"
#include "dxc/dxcapi_radrayext.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/microcom.h"

#include <cstring>

namespace {

using radray::shader::IRadRayDxcCompiler;
using radray::shader::RadRayDxcAbiInfo;
using radray::shader::RadRayDxcBlobView;

constexpr uint8_t kToolchainIdentity[16] = {
    0x72, 0x61, 0x64, 0x72, 0x61, 0x79, 0x2d, 0x64,
    0x78, 0x63, 0x2d, 0x31, 0x2e, 0x39, 0x2e, 0x32};

class RadRayDxcCompiler final : public IRadRayDxcCompiler {
private:
  DXC_MICROCOM_TM_REF_FIELDS()

public:
  DXC_MICROCOM_TM_ADDREF_RELEASE_IMPL()
  DXC_MICROCOM_TM_CTOR(RadRayDxcCompiler)

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                            void **ppvObject) override {
    return DoBasicQueryInterface<IRadRayDxcCompiler>(this, iid, ppvObject);
  }

  HRESULT STDMETHODCALLTYPE GetAbiInfo(RadRayDxcAbiInfo *info) override {
    if (info == nullptr)
      return E_POINTER;
    *info = {};
    info->AbiVersion = radray::shader::kRadRayDxcAbiVersion;
    info->MetadataSchemaVersion =
        radray::shader::kRadRayDxcMetadataSchemaVersion;
    info->ToolchainMajor = 1;
    info->ToolchainMinor = 9;
    std::memcpy(info->ToolchainIdentity.Bytes, kToolchainIdentity,
                sizeof(kToolchainIdentity));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DiscoverSourceContract(
      RadRayDxcBlobView request,
      radray::shader::IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    if (request.Data == nullptr || request.Size == 0)
      return E_INVALIDARG;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CompileVariant(
      RadRayDxcBlobView request,
      radray::shader::IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    if (request.Data == nullptr || request.Size == 0)
      return E_INVALIDARG;
    return E_NOTIMPL;
  }
};

} // namespace

HRESULT CreateRadRayDxcCompiler(REFIID riid, LPVOID *ppv) {
  if (ppv == nullptr)
    return E_POINTER;
  *ppv = nullptr;

  try {
    CComPtr<RadRayDxcCompiler> result(
        RadRayDxcCompiler::Alloc(DxcGetThreadMallocNoRef()));
    IFROOM(result.p);
    return result.p->QueryInterface(riid, ppv);
  }
  CATCH_CPP_RETURN_HRESULT();
}
