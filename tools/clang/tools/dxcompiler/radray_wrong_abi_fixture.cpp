#include <windows.h>

#define DXC_API_IMPORT __declspec(dllexport)
#include "dxc/dxcapi_radrayext.h"
#undef DXC_API_IMPORT

#include <atomic>
#include <new>

namespace {

class WrongAbiCompiler final : public radray::shader::IRadRayDxcCompiler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                            void **object) override {
    if (object == nullptr)
      return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) ||
        IsEqualIID(iid, radray::shader::IID_IRadRayDxcCompiler)) {
      *object = static_cast<radray::shader::IRadRayDxcCompiler *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++_references;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG references = --_references;
    if (references == 0)
      delete this;
    return references;
  }

  HRESULT STDMETHODCALLTYPE GetAbiInfo(
      radray::shader::RadRayDxcAbiInfo *info) override {
    if (info == nullptr)
      return E_POINTER;
    *info = {};
    info->AbiVersion = 99;
    info->MetadataSchemaVersion =
        radray::shader::kRadRayDxcMetadataSchemaVersion;
    info->ToolchainMajor = 1;
    info->ToolchainMinor = 9;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DiscoverSourceContract(
      radray::shader::RadRayDxcBlobView,
      radray::shader::IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CompileVariant(
      radray::shader::RadRayDxcBlobView,
      radray::shader::IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    return E_NOTIMPL;
  }

private:
  std::atomic<ULONG> _references{1};
};

} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DxcCreateInstance(
    REFCLSID classId, REFIID interfaceId, LPVOID *object) {
  if (object == nullptr)
    return E_POINTER;
  *object = nullptr;
  if (!IsEqualCLSID(classId, radray::shader::CLSID_RadRayDxcCompiler))
    return REGDB_E_CLASSNOTREG;

  auto *compiler = new (std::nothrow) WrongAbiCompiler();
  if (compiler == nullptr)
    return E_OUTOFMEMORY;
  const HRESULT result = compiler->QueryInterface(interfaceId, object);
  compiler->Release();
  return result;
}
