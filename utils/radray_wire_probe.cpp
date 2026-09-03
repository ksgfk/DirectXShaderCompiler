// Standalone probe for the RadRay DXC extension. Drives DiscoverSourceContract
// plus CompileVariant against a source file and prints the decoded schema 7 wire
// so the compiler-side contract can be verified without the RadRay tree.
//
// Build (from the fork root, with a VS developer prompt or -I to the SDK):
//   cl /std:c++17 /EHsc /nologo /Iinclude utils\radray_wire_probe.cpp \
//      /Fe:build_radray\Release\bin\radray_wire_probe.exe \
//      /link build_radray\Release\lib\dxcompiler.lib

#include <windows.h>

#include "dxc/dxcapi_radrayext.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using radray::shader::CLSID_RadRayDxcCompiler;
using radray::shader::IRadRayDxcCompiler;
using radray::shader::IRadRayDxcResult;
using radray::shader::RadRayDxcBlobView;
using radray::shader::RadRayDxcCompileStatus;
using radray::shader::RadRayDxcDiagnosticView;
using radray::shader::RadRayDxcIncludePathListView;
using radray::shader::RadRayDxcLaneView;
using radray::shader::RadRayDxcTarget;

namespace {

void AppendU8(std::vector<uint8_t> &bytes, uint8_t value) {
  bytes.push_back(value);
}

void AppendU16(std::vector<uint8_t> &bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32(std::vector<uint8_t> &bytes, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void AppendBlob(std::vector<uint8_t> &bytes, const std::string &value) {
  AppendU32(bytes, static_cast<uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

struct Wire {
  const uint8_t *Data{nullptr};
  uint32_t Size{0};

  uint32_t U32(uint32_t offset) const {
    uint32_t value = 0;
    std::memcpy(&value, Data + offset, sizeof(value));
    return value;
  }
  uint16_t U16(uint32_t offset) const {
    uint16_t value = 0;
    std::memcpy(&value, Data + offset, sizeof(value));
    return value;
  }
  float F32(uint32_t offset) const {
    float value = 0.0f;
    std::memcpy(&value, Data + offset, sizeof(value));
    return value;
  }
  std::string Text(uint32_t offset) const {
    const uint32_t start = U32(offset);
    const uint32_t size = U32(offset + 4);
    if (start + size > Size)
      return std::string("<bad-name-range>");
    return std::string(reinterpret_cast<const char *>(Data + start), size);
  }
};

const char *KindName(uint32_t kind) {
  static const char *const kNames[] = {
      "Unknown",        "CBuffer",           "TypedBuffer",
      "RWTypedBuffer",  "StructuredBuffer",  "RWStructuredBuffer",
      "RawBuffer",      "RWRawBuffer",       "Texture",
      "RWTexture",      "Sampler"};
  return kind < sizeof(kNames) / sizeof(kNames[0]) ? kNames[kind] : "?";
}

const char *PlacementName(uint32_t placement) {
  switch (placement) {
  case 0:
    return "Table";
  case 1:
    return "RootDescriptor";
  case 2:
    return "StaticSampler";
  default:
    return "?";
  }
}
std::string PayloadName(const Wire &wire, uint32_t typeOffset,
                        uint32_t typeBytes, uint32_t typeIndex) {
  constexpr uint32_t kNoType = 0xffffffffu;
  constexpr uint32_t kTypeStride = 40u;
  if (typeIndex == kNoType)
    return std::string("none");
  if ((typeBytes % kTypeStride) != 0 ||
      typeIndex >= typeBytes / kTypeStride)
    return std::string("bad-index");
  return wire.Text(typeOffset + typeIndex * kTypeStride);
}

bool SameLaneMetadata(IRadRayDxcResult *left, IRadRayDxcResult *right,
                      RadRayDxcTarget target) {
  RadRayDxcLaneView leftLane{};
  RadRayDxcLaneView rightLane{};
  const HRESULT leftStatus = left->GetTargetLane(target, &leftLane);
  const HRESULT rightStatus = right->GetTargetLane(target, &rightLane);
  if (SUCCEEDED(leftStatus) != SUCCEEDED(rightStatus))
    return false;
  if (FAILED(leftStatus))
    return true;
  return leftLane.Metadata.Size == rightLane.Metadata.Size &&
         std::memcmp(leftLane.Metadata.Data, rightLane.Metadata.Data,
                     leftLane.Metadata.Size) == 0;
}

void PrintLane(const char *label, RadRayDxcLaneView lane) {
  Wire wire{lane.Metadata.Data, lane.Metadata.Size};
  std::printf("\n=== %s lane: metadata %u bytes, bytecode %u bytes ===\n", label,
              lane.Metadata.Size, lane.Bytecode.Size);
  if (wire.Size < 152) {
    std::printf("  metadata too small for a schema 7 envelope\n");
    return;
  }
  std::printf("  schema=%u headerSize=%u total=%u target=%u stageMask=0x%x\n",
              wire.U16(4), wire.U16(6), wire.U32(8), wire.Data[12],
              wire.Data[13]);
  const uint32_t bindingOffset = wire.U32(24);
  const uint32_t bindingBytes = wire.U32(28);
  const uint32_t typeOffset = wire.U32(32);
  const uint32_t typeBytes = wire.U32(36);
  const uint32_t rootConstantOffset = wire.U32(40);
  const uint32_t rootConstantBytes = wire.U32(44);
  const uint32_t samplerOffset = wire.U32(56);
  const uint32_t samplerBytes = wire.U32(60);
  const uint32_t rootSignatureBytes = wire.U32(68);
  std::printf("  rootSignature=%u bytes\n", rootSignatureBytes);

  std::printf("  bindings (%u):\n", bindingBytes / 44);
  for (uint32_t offset = bindingOffset; offset < bindingOffset + bindingBytes;
       offset += 44) {
    const std::string payload =
        PayloadName(wire, typeOffset, typeBytes, wire.U32(offset + 40));
    std::printf("    %-16s group=%u binding=%u kind=%-18s count=%u "
                "stages=0x%x placement=%-14s sampler=%d flags=0x%x "
                "payload=%s\n",
                wire.Text(offset).c_str(), wire.U32(offset + 8),
                wire.U32(offset + 12), KindName(wire.U32(offset + 16)),
                wire.U32(offset + 20), wire.U32(offset + 24),
                PlacementName(wire.U32(offset + 28)),
                static_cast<int32_t>(wire.U32(offset + 32)),
                wire.U32(offset + 36), payload.c_str());
  }

  std::printf("  root constants (%u):\n", rootConstantBytes / 36);
  for (uint32_t offset = rootConstantOffset;
       offset < rootConstantOffset + rootConstantBytes; offset += 36) {
    const std::string payload =
        PayloadName(wire, typeOffset, typeBytes, wire.U32(offset + 32));
    std::printf("    %-16s space=%u register=%u offset=%u size=%u stages=0x%x "
                "flags=0x%x payload=%s\n",
                wire.Text(offset).c_str(), wire.U32(offset + 8),
                wire.U32(offset + 12), wire.U32(offset + 16),
                wire.U32(offset + 20), wire.U32(offset + 24),
                wire.U32(offset + 28), payload.c_str());
  }

  std::printf("  samplers (%u):\n", samplerBytes / 64);
  for (uint32_t offset = samplerOffset; offset < samplerOffset + samplerBytes;
       offset += 64) {
    std::printf("    mag=%u min=%u mip=%u addr=%u/%u/%u bias=%.1f aniso=%u/%.1f "
                "cmp=%u/%u lod=%.1f..%.1f border=%u reduction=%u flags=0x%x\n",
                wire.U32(offset), wire.U32(offset + 4), wire.U32(offset + 8),
                wire.U32(offset + 12), wire.U32(offset + 16),
                wire.U32(offset + 20), wire.F32(offset + 24),
                wire.U32(offset + 28), wire.F32(offset + 32),
                wire.U32(offset + 36), wire.U32(offset + 40),
                wire.F32(offset + 44), wire.F32(offset + 48),
                wire.U32(offset + 52), wire.U32(offset + 56),
                wire.U32(offset + 60));
  }
}

void PrintDiagnostics(IRadRayDxcResult *result) {
  uint32_t count = 0;
  if (FAILED(result->GetDiagnosticCount(&count)) || count == 0)
    return;
  std::printf("  diagnostics (%u):\n", count);
  for (uint32_t index = 0; index < count; ++index) {
    RadRayDxcDiagnosticView diagnostic{};
    if (FAILED(result->GetDiagnostic(index, &diagnostic)))
      continue;
    std::printf("    [%u] %.*s\n", diagnostic.Code,
                static_cast<int>(diagnostic.MessageUtf8.Size),
                reinterpret_cast<const char *>(diagnostic.MessageUtf8.Data));
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: radray_wire_probe <source.hlsl> [shaderModel] [targetMask]\n"
                "  targetMask: 1 = DXIL, 2 = SPIR-V, 3 = both (default)\n");
    return 2;
  }
  const uint32_t shaderModel =
      argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 60u;

  std::string source;
  if (FILE *file = std::fopen(argv[1], "rb")) {
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
      source.append(buffer, read);
    std::fclose(file);
  } else {
    std::printf("cannot open %s\n", argv[1]);
    return 2;
  }

  IRadRayDxcCompiler *compiler = nullptr;
  if (FAILED(DxcCreateInstance(CLSID_RadRayDxcCompiler,
                               radray::shader::IID_IRadRayDxcCompiler,
                               reinterpret_cast<void **>(&compiler)))) {
    std::printf("cannot create the RadRay DXC compiler\n");
    return 2;
  }

  const std::string sourceName = "probe.hlsl";
  const uint8_t targets =
      argc >= 4 ? static_cast<uint8_t>(std::atoi(argv[3])) : uint8_t{3};
  std::vector<uint8_t> discovery;
  AppendU32(discovery, radray::shader::kRadRayDxcDiscoveryWireMagic);
  AppendU16(discovery, radray::shader::kRadRayDxcDiscoveryWireSchemaVersion);
  AppendBlob(discovery, sourceName);
  AppendBlob(discovery, source);
  AppendU8(discovery, targets);
  AppendU32(discovery, shaderModel);
  AppendU8(discovery, 1); // optimize
  AppendU8(discovery, 0); // debug info
  AppendU8(discovery, 0); // all resources bound
  AppendU8(discovery, 1); // warnings are errors
  AppendU32(discovery, 0);
  AppendU32(discovery, 2021);
  AppendU32(discovery, 0);
  AppendU32(discovery, 0); // no defines

  RadRayDxcIncludePathListView includePaths{};
  IRadRayDxcResult *discovered = nullptr;
  RadRayDxcBlobView discoveryView{discovery.data(),
                                  static_cast<uint32_t>(discovery.size())};
  if (FAILED(compiler->DiscoverSourceContract(discoveryView, includePaths,
                                              &discovered))) {
    std::printf("discovery call failed\n");
    return 2;
  }
  RadRayDxcCompileStatus status{};
  discovered->GetStatus(&status);
  std::printf("discovery status=%u\n", static_cast<uint32_t>(status));
  PrintDiagnostics(discovered);
  RadRayDxcBlobView contract{};
  if (FAILED(discovered->GetContractBlob(&contract)) || contract.Size < 16) {
    std::printf("discovery returned no contract\n");
    return 1;
  }
  // The contract hash is the trailing 16 bytes of the contract blob.
  std::vector<uint8_t> contractHash(contract.Data + contract.Size - 16,
                                    contract.Data + contract.Size);

  std::vector<uint8_t> compile;
  AppendU32(compile, radray::shader::kRadRayDxcShaderWireMagic);
  AppendU16(compile, radray::shader::kRadRayDxcShaderWireSchemaVersion);
  AppendBlob(compile, sourceName);
  AppendBlob(compile, source);
  AppendU8(compile, targets);
  AppendU32(compile, shaderModel);
  AppendU8(compile, 1);
  AppendU8(compile, 0);
  AppendU8(compile, 0);
  AppendU8(compile, 1);
  AppendU32(compile, 0);
  AppendU32(compile, 2021);
  AppendU32(compile, 0);
  compile.insert(compile.end(), contractHash.begin(), contractHash.end());
  AppendU32(compile, 0); // no defines
  AppendU32(compile, 0); // no keyword assignments

  IRadRayDxcResult *compiled = nullptr;
  RadRayDxcBlobView compileView{compile.data(),
                                static_cast<uint32_t>(compile.size())};
  if (FAILED(compiler->CompileVariant(compileView, includePaths, &compiled))) {
    std::printf("compile call failed\n");
    return 2;
  }
  compiled->GetStatus(&status);
  std::printf("compile status=%u\n", static_cast<uint32_t>(status));
  PrintDiagnostics(compiled);
  if (status != RadRayDxcCompileStatus::Success)
    return 1;
  IRadRayDxcResult *recompiled = nullptr;
  if (FAILED(
          compiler->CompileVariant(compileView, includePaths, &recompiled))) {
    std::printf("repeat compile call failed\n");
    return 2;
  }
  RadRayDxcCompileStatus repeatedStatus{};
  recompiled->GetStatus(&repeatedStatus);
  const bool deterministic =
      repeatedStatus == RadRayDxcCompileStatus::Success &&
      SameLaneMetadata(compiled, recompiled, RadRayDxcTarget::DXIL) &&
      SameLaneMetadata(compiled, recompiled, RadRayDxcTarget::SPIRV);
  std::printf("metadata deterministic=%s\n",
              deterministic ? "yes" : "no");
  if (!deterministic)
    return 1;

  RadRayDxcLaneView lane{};
  if (SUCCEEDED(compiled->GetTargetLane(RadRayDxcTarget::DXIL, &lane)))
    PrintLane("DXIL", lane);
  if (SUCCEEDED(compiled->GetTargetLane(RadRayDxcTarget::SPIRV, &lane)))
    PrintLane("SPIRV", lane);
  return 0;
}
