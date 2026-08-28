#ifndef __DXC_API_RADRAY_EXT__
#define __DXC_API_RADRAY_EXT__

#include "dxc/dxcapi.h"

#include <cstdint>

namespace radray::shader {

inline constexpr CLSID CLSID_RadRayDxcCompiler{
    0x8e3d0d81,
    0x4d5d,
    0x4e31,
    {0x9a, 0x1b, 0x7c, 0x22, 0x91, 0x55, 0xb4, 0x10}};

inline constexpr IID IID_IRadRayDxcCompiler{
    0x3f5c7a84,
    0x1f6c,
    0x5a3e,
    {0x9d, 0x8c, 0x49, 0x5f, 0x8c, 0x22, 0x80, 0x31}};

inline constexpr IID IID_IRadRayDxcResult{
    0x51b1f89a,
    0xe0d8,
    0x47c2,
    {0xb7, 0x6f, 0x4e, 0xd1, 0x58, 0x3b, 0x8a, 0x72}};

inline constexpr uint32_t kRadRayDxcAbiVersion = 3;
inline constexpr uint32_t kRadRayDxcLegacyMetadataSchemaVersion = 4;
// Schema 6 replaces schema 5 outright: the binding kinds are logical rather
// than register-class shaped, bindings carry a policy placement, root constants
// carry their declaration name and Vulkan sampler states travel as records.
inline constexpr uint32_t kRadRayDxcMetadataSchemaVersion = 6;
inline constexpr uint32_t kRadRayDxcShaderWireMagic = 0x59524452u;
inline constexpr uint16_t kRadRayDxcShaderWireSchemaVersion = 2;
inline constexpr uint32_t kRadRayDxcDiscoveryWireMagic = 0x44524452u;
inline constexpr uint16_t kRadRayDxcDiscoveryWireSchemaVersion = 3;
inline constexpr uint32_t kRadRayDxcContractWireMagic = 0x54434452u;
inline constexpr uint16_t kRadRayDxcContractWireSchemaVersion = 1;

enum class RadRayDxcTarget : uint32_t {
  DXIL = 0,
  SPIRV = 1,
};

enum class RadRayDxcCompileStatus : uint32_t {
  Success = 0,
  InvalidRequest = 1,
  ContractMismatch = 2,
  TargetFailure = 3,
};

struct RadRayDxcHash128 {
  uint8_t Bytes[16]{};
};

struct RadRayDxcBlobView {
  // The view remains valid until the owning IRadRayDxcResult is released.
  const uint8_t *Data{nullptr};
  uint32_t Size{0};
};

struct RadRayDxcIncludePathListView {
  // Each element is an explicitly sized UTF-8 path borrowed for one synchronous call.
  const RadRayDxcBlobView *Paths{nullptr};
  uint32_t Count{0};
};

struct RadRayDxcAbiInfo {
  uint32_t AbiVersion{kRadRayDxcAbiVersion};
  uint32_t MetadataSchemaVersion{kRadRayDxcMetadataSchemaVersion};
  uint32_t ToolchainMajor{0};
  uint32_t ToolchainMinor{0};
  RadRayDxcHash128 ToolchainIdentity{};
};

struct RadRayDxcLaneView {
  // Both views are borrowed from the owning IRadRayDxcResult.
  RadRayDxcTarget Target{RadRayDxcTarget::DXIL};
  RadRayDxcBlobView Bytecode{};
  RadRayDxcBlobView Metadata{};
};

struct RadRayDxcDiagnosticView {
  uint32_t Code{0};
  RadRayDxcBlobView MessageUtf8{};
};

static_assert(sizeof(RadRayDxcHash128) == 16);
static_assert(sizeof(RadRayDxcBlobView) == 16);
static_assert(sizeof(RadRayDxcIncludePathListView) == 16);
static_assert(sizeof(RadRayDxcAbiInfo) == 32);
static_assert(sizeof(RadRayDxcLaneView) == 40);
static_assert(sizeof(RadRayDxcDiagnosticView) == 24);

struct IRadRayDxcResult;

CROSS_PLATFORM_UUIDOF(IRadRayDxcCompiler,
                       "3F5C7A84-1F6C-5A3E-9D8C-495F8C228031")
struct IRadRayDxcCompiler : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  GetAbiInfo(_Out_ RadRayDxcAbiInfo *info) = 0;

  virtual HRESULT STDMETHODCALLTYPE DiscoverSourceContract(
      _In_ RadRayDxcBlobView request,
      _In_ RadRayDxcIncludePathListView includePaths,
      _COM_Outptr_ IRadRayDxcResult **result) = 0;

  virtual HRESULT STDMETHODCALLTYPE CompileVariant(
      _In_ RadRayDxcBlobView request,
      _In_ RadRayDxcIncludePathListView includePaths,
      _COM_Outptr_ IRadRayDxcResult **result) = 0;
};

CROSS_PLATFORM_UUIDOF(IRadRayDxcResult,
                       "51B1F89A-E0D8-47C2-B76F-4ED1583B8A72")
struct IRadRayDxcResult : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  GetStatus(_Out_ RadRayDxcCompileStatus *status) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  GetAbiInfo(_Out_ RadRayDxcAbiInfo *info) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  GetContractBlob(_Out_ RadRayDxcBlobView *blob) = 0;

  virtual HRESULT STDMETHODCALLTYPE GetTargetLane(
      _In_ RadRayDxcTarget target, _Out_ RadRayDxcLaneView *lane) = 0;

  virtual HRESULT STDMETHODCALLTYPE
  GetDiagnosticCount(_Out_ uint32_t *count) = 0;

  virtual HRESULT STDMETHODCALLTYPE GetDiagnostic(
      _In_ uint32_t index, _Out_ RadRayDxcDiagnosticView *diagnostic) = 0;
};

} // namespace radray::shader

#endif
