///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxcradray.cpp                                                             //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Implements the RadRay-owned DXC extension factory, wire parser, and       //
// compiler-owned result lifetime.                                           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/microcom.h"
#include "dxc/dxcapi_radrayext.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/Unicode.h"
#include "dxcompilerobj_radray.h"

#ifdef interface
#undef interface
#endif

#include "clang/AST/Attr.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/HlslTypes.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Parse/ParseHLSL.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/LiteralSupport.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Pragma.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Basic/SourceManager.h"

#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/DxilRootSignature/DxilRootSignature.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/DXIL/DxilSignature.h"
#include "dxc/DXIL/DxilSignatureElement.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

#ifdef ENABLE_SPIRV_CODEGEN
#include "clang/SPIRV/SpirvInstruction.h"
#include "clang/SPIRV/SpirvModule.h"
#include "clang/SPIRV/SpirvType.h"
#include "DeclResultIdMapper.h"
#include "SpirvEmitter.h"
#include "StageVar.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

HRESULT CreateDxcCompiler(REFIID riid, _Out_ LPVOID *ppv);
HRESULT CreateDxcUtils(REFIID riid, _Out_ LPVOID *ppv);

namespace {

using std::array;
using std::string;
using std::string_view;
using std::vector;
using std::wstring;
using radray::shader::IRadRayDxcCompiler;
using radray::shader::IRadRayDxcResult;
using radray::shader::RadRayDxcAbiInfo;
using radray::shader::RadRayDxcBlobView;
using radray::shader::RadRayDxcCompileStatus;
using radray::shader::RadRayDxcDiagnosticView;
using radray::shader::RadRayDxcHash128;
using radray::shader::RadRayDxcIncludePathListView;
using radray::shader::RadRayDxcLaneView;
using radray::shader::RadRayDxcTarget;

// Toolchain identity gates artifact trust on the RadRay side. Bump both values
// together whenever compiler output semantics change, then regenerate goldens.
constexpr uint8_t kToolchainIdentity[16] = {
    0x12, 0x02, 0x09, 0x01, 0x72, 0x61, 0x64, 0x72,
    0x61, 0x79, 0x2d, 0x31, 0x2e, 0x39, 0x2e, 0x36};
constexpr uint64_t kMetadataToolchainIdentity = 0x0000000001090212ull;
constexpr uint32_t kMaxCollectionCount = 4096;

struct Hash128 {
  uint8_t Bytes[16]{};
};

bool operator==(const Hash128 &lhs, const Hash128 &rhs) noexcept {
  return std::memcmp(lhs.Bytes, rhs.Bytes, sizeof(lhs.Bytes)) == 0;
}

bool IsZero(const Hash128 &value) noexcept {
  for (const uint8_t byte : value.Bytes) {
    if (byte != 0)
      return false;
  }
  return true;
}

void HashByte(uint64_t &first, uint64_t &second, uint8_t value) noexcept {
  first ^= value;
  first *= 1099511628211ull;
  second ^= static_cast<uint64_t>(value) + 0x9e3779b97f4a7c15ull;
  second *= 14029467366897019727ull;
}

Hash128 Digest(const vector<uint8_t> &bytes, uint64_t salt) noexcept {
  uint64_t first = 1469598103934665603ull ^ salt;
  uint64_t second = 1099511628211ull + salt;
  for (const uint8_t byte : bytes)
    HashByte(first, second, byte);
  Hash128 result{};
  for (uint32_t index = 0; index < 8; ++index) {
    result.Bytes[index] = static_cast<uint8_t>(first >> (index * 8));
    result.Bytes[index + 8] = static_cast<uint8_t>(second >> (index * 8));
  }
  return result;
}

void AppendByte(vector<uint8_t> &output, uint8_t value) {
  output.push_back(value);
}

void AppendU16(vector<uint8_t> &output, uint16_t value) {
  AppendByte(output, static_cast<uint8_t>(value));
  AppendByte(output, static_cast<uint8_t>(value >> 8));
}

void AppendU32(vector<uint8_t> &output, uint32_t value) {
  AppendByte(output, static_cast<uint8_t>(value));
  AppendByte(output, static_cast<uint8_t>(value >> 8));
  AppendByte(output, static_cast<uint8_t>(value >> 16));
  AppendByte(output, static_cast<uint8_t>(value >> 24));
}

void AppendBytes(vector<uint8_t> &output, const vector<uint8_t> &value) {
  output.insert(output.end(), value.begin(), value.end());
}

void AppendString(vector<uint8_t> &output, string_view value) {
  AppendU32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

class WireReader {
public:
  explicit WireReader(RadRayDxcBlobView view)
      : _data(view.Data), _size(view.Size) {}

  bool ReadU8(uint8_t &value) noexcept {
    if (_offset >= _size)
      return false;
    value = _data[_offset++];
    return true;
  }

  bool ReadU16(uint16_t &value) noexcept {
    uint8_t first = 0;
    uint8_t second = 0;
    if (!ReadU8(first) || !ReadU8(second))
      return false;
    value = static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8));
    return true;
  }

  bool ReadU32(uint32_t &value) noexcept {
    uint8_t bytes[4]{};
    for (uint8_t &byte : bytes) {
      if (!ReadU8(byte))
        return false;
    }
    value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) |
            (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
  }

  bool ReadBytes(vector<uint8_t> &value) {
    uint32_t size = 0;
    if (!ReadU32(size) || size > Remaining())
      return false;
    value.assign(_data + _offset, _data + _offset + size);
    _offset += size;
    return true;
  }

  bool ReadString(string &value) {
    uint32_t size = 0;
    if (!ReadU32(size) || size > Remaining())
      return false;
    value.assign(reinterpret_cast<const char *>(_data + _offset), size);
    _offset += size;
    return true;
  }

  bool ReadHash(Hash128 &value) noexcept {
    if (Remaining() < sizeof(value.Bytes))
      return false;
    std::memcpy(value.Bytes, _data + _offset, sizeof(value.Bytes));
    _offset += sizeof(value.Bytes);
    return true;
  }

  size_t Remaining() const noexcept { return _size - _offset; }

private:
  const uint8_t *_data{nullptr};
  size_t _size{0};
  size_t _offset{0};
};

bool IsLogicalSourceName(string_view value) noexcept {
  if (value.empty() || value.front() == '/' || value.front() == '\\' ||
      (value.size() > 1 && value[1] == ':')) {
    return false;
  }
  size_t begin = 0;
  while (begin < value.size()) {
    const size_t separator = value.find('/', begin);
    const size_t end = separator == string_view::npos ? value.size() : separator;
    const string_view segment = value.substr(begin, end - begin);
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find('\\') != string_view::npos) {
      return false;
    }
    if (separator == string_view::npos)
      break;
    begin = separator + 1;
  }
  return true;
}

struct NameValue {
  string Name;
  string Value;
};

struct CompileRequest {
  string SourceName;
  vector<uint8_t> RootSource;
  vector<NameValue> Defines;
  vector<NameValue> Assignments;
  uint8_t Targets{0};
  uint32_t ShaderModel{60};
  uint8_t Optimize{1};
  uint8_t DebugInfo{0};
  uint8_t AllResourcesBound{0};
  uint8_t WarningPolicy{0};
  uint32_t SpirvTargetEnv{0};
  uint32_t HlslVersion{2021};
  uint32_t Reserved{0};
  Hash128 ExpectedContract{};
};

struct DiscoveryRequest {
  string SourceName;
  vector<uint8_t> RootSource;
  vector<NameValue> Defines;
  uint8_t Targets{0};
  uint32_t ShaderModel{60};
  uint8_t Optimize{1};
  uint8_t DebugInfo{0};
  uint8_t AllResourcesBound{0};
  uint8_t WarningPolicy{0};
  uint32_t SpirvTargetEnv{0};
  uint32_t HlslVersion{2021};
  uint32_t Reserved{0};
};

bool ReadNameValues(WireReader &reader, vector<NameValue> &values) {
  uint32_t count = 0;
  if (!reader.ReadU32(count) || count > kMaxCollectionCount)
    return false;
  values.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    NameValue value;
    if (!reader.ReadString(value.Name) || !reader.ReadString(value.Value) ||
        value.Name.empty()) {
      return false;
    }
    values.push_back(std::move(value));
  }
  return true;
}

bool HasDuplicateNames(const vector<NameValue> &values) noexcept {
  for (size_t left = 0; left < values.size(); ++left) {
    for (size_t right = left + 1; right < values.size(); ++right) {
      if (values[left].Name == values[right].Name)
        return true;
    }
  }
  return false;
}

// Reject policy fields the compiler does not honor instead of silently
// ignoring them; every accepted value must change compiler behavior.
bool ValidateCompilePolicy(uint32_t shaderModel, uint8_t optimize,
                           uint8_t debugInfo, uint8_t allResourcesBound,
                           uint8_t warningPolicy, uint32_t spirvTargetEnv,
                           uint32_t hlslVersion, uint32_t reserved,
                           string &error) {
  if (shaderModel < 60 || shaderModel > 69) {
    error = "compile policy requests an unsupported shader model";
    return false;
  }
  if (optimize > 1 || debugInfo > 1 || allResourcesBound > 1) {
    error = "compile policy boolean field is out of range";
    return false;
  }
  if (warningPolicy > 1) {
    error = "compile policy warning policy is out of range";
    return false;
  }
  if (spirvTargetEnv != 0) {
    error = "compile policy requests an unsupported SPIR-V target environment";
    return false;
  }
  if (hlslVersion != 2016 && hlslVersion != 2017 && hlslVersion != 2018 &&
      hlslVersion != 2021) {
    error = "compile policy requests an unsupported HLSL version";
    return false;
  }
  if (reserved != 0) {
    error = "compile policy reserved field must be zero";
    return false;
  }
  return true;
}

bool ReadCompileRequest(RadRayDxcBlobView view, CompileRequest &request,
                        string &error) {
  WireReader reader{view};
  uint32_t magic = 0;
  uint16_t schema = 0;
  if (!reader.ReadU32(magic) || !reader.ReadU16(schema) ||
      magic != radray::shader::kRadRayDxcShaderWireMagic ||
      schema != radray::shader::kRadRayDxcShaderWireSchemaVersion) {
    error = "compile request wire header is invalid";
    return false;
  }
  if (!reader.ReadString(request.SourceName) ||
      !reader.ReadBytes(request.RootSource) ||
      !IsLogicalSourceName(request.SourceName) || request.RootSource.empty()) {
    error = "compile request source identity or root source is invalid";
    return false;
  }
  if (!reader.ReadU8(request.Targets) || request.Targets == 0 ||
      (request.Targets & ~uint8_t{3}) != 0 ||
      !reader.ReadU32(request.ShaderModel) || !reader.ReadU8(request.Optimize) ||
      !reader.ReadU8(request.DebugInfo) || !reader.ReadU8(request.AllResourcesBound) ||
      !reader.ReadU8(request.WarningPolicy) || !reader.ReadU32(request.SpirvTargetEnv) ||
      !reader.ReadU32(request.HlslVersion) || !reader.ReadU32(request.Reserved) ||
      !reader.ReadHash(request.ExpectedContract) ||
      !ReadNameValues(reader, request.Defines) ||
      !ReadNameValues(reader, request.Assignments) || reader.Remaining() != 0) {
    error = "compile request policy or collection payload is invalid";
    return false;
  }
  if (HasDuplicateNames(request.Defines) || HasDuplicateNames(request.Assignments)) {
    error = "compile request contains duplicate define or assignment names";
    return false;
  }
  if (!ValidateCompilePolicy(request.ShaderModel, request.Optimize,
                             request.DebugInfo, request.AllResourcesBound,
                             request.WarningPolicy, request.SpirvTargetEnv,
                             request.HlslVersion, request.Reserved, error)) {
    return false;
  }
  return true;
}

bool ReadDiscoveryRequest(RadRayDxcBlobView view, DiscoveryRequest &request,
                          string &error) {
  WireReader reader{view};
  uint32_t magic = 0;
  uint16_t schema = 0;
  if (!reader.ReadU32(magic) || !reader.ReadU16(schema) ||
      magic != radray::shader::kRadRayDxcDiscoveryWireMagic ||
      schema != radray::shader::kRadRayDxcDiscoveryWireSchemaVersion ||
      !reader.ReadString(request.SourceName) ||
      !reader.ReadBytes(request.RootSource) ||
      !IsLogicalSourceName(request.SourceName) || request.RootSource.empty() ||
      !reader.ReadU8(request.Targets) || request.Targets == 0 ||
      (request.Targets & ~uint8_t{3}) != 0 ||
      !reader.ReadU32(request.ShaderModel) || !reader.ReadU8(request.Optimize) ||
      !reader.ReadU8(request.DebugInfo) ||
      !reader.ReadU8(request.AllResourcesBound) ||
      !reader.ReadU8(request.WarningPolicy) ||
      !reader.ReadU32(request.SpirvTargetEnv) ||
      !reader.ReadU32(request.HlslVersion) || !reader.ReadU32(request.Reserved) ||
      !ReadNameValues(reader, request.Defines) || reader.Remaining() != 0) {
    error = "discovery request wire payload is invalid";
    return false;
  }
  if (HasDuplicateNames(request.Defines)) {
    error = "discovery request contains duplicate define names";
    return false;
  }
  if (!ValidateCompilePolicy(request.ShaderModel, request.Optimize,
                             request.DebugInfo, request.AllResourcesBound,
                             request.WarningPolicy, request.SpirvTargetEnv,
                             request.HlslVersion, request.Reserved, error)) {
    return false;
  }
  return true;
}

DiscoveryRequest MakeDiscoveryRequest(const CompileRequest &request) {
  DiscoveryRequest discovery;
  discovery.SourceName = request.SourceName;
  discovery.RootSource = request.RootSource;
  discovery.Defines = request.Defines;
  discovery.Targets = request.Targets;
  discovery.ShaderModel = request.ShaderModel;
  discovery.Optimize = request.Optimize;
  discovery.DebugInfo = request.DebugInfo;
  discovery.AllResourcesBound = request.AllResourcesBound;
  discovery.WarningPolicy = request.WarningPolicy;
  discovery.SpirvTargetEnv = request.SpirvTargetEnv;
  discovery.HlslVersion = request.HlslVersion;
  discovery.Reserved = request.Reserved;
  return discovery;
}

enum class ShaderStage : uint8_t { Vertex = 0, Pixel = 1, Compute = 2 };
enum class ShaderKind : uint8_t { Graphics = 0, Compute = 1 };

struct KeywordGroup {
  string Name;
  vector<string> Values;
};

struct EntryPoint {
  string Name;
  ShaderStage Stage{ShaderStage::Vertex};
};

struct ContractData {
  ShaderKind Kind{ShaderKind::Graphics};
  vector<KeywordGroup> KeywordGroups;
  vector<EntryPoint> EntryPoints;
  Hash128 Hash{};
};

constexpr uint32_t kMetadataNoParent = 0xffffffffu;
constexpr uint32_t kMetadataNoType = 0xffffffffu;

// Logical resource kind. Derived from the HLSL declaration type in the AST, so
// both lanes publish the same kind for the same declaration by construction; a
// lane's lowered form is only used to cross-check this value.
enum class MetadataBindingKind : uint32_t {
  Unknown = 0,
  CBuffer = 1,
  TypedBuffer = 2,
  RWTypedBuffer = 3,
  StructuredBuffer = 4,
  RWStructuredBuffer = 5,
  RawBuffer = 6,
  RWRawBuffer = 7,
  Texture = 8,
  RWTexture = 9,
  Sampler = 10,
};

// Where the RootSignature policy places a declaration. Table is also the value
// published when the source declares no policy at all.
enum class MetadataBindingPlacement : uint32_t {
  Table = 0,
  RootDescriptor = 1,
  StaticSampler = 2,
};

// Policy parameter classes, flattened from the versioned RootSignature desc.
enum class MetadataPolicyKind : uint32_t {
  Table = 0,
  RootDescriptor = 1,
  RootConstants = 2,
  StaticSampler = 3,
};

constexpr uint32_t kMetadataNoSampler = 0xffffffffu;

struct MetadataBindingFact {
  string Name;
  uint32_t Group{0};
  uint32_t Binding{0};
  uint32_t RegisterClass{0};
  uint32_t Type{0};
  uint32_t Count{1};
  uint32_t StageMask{0};
  uint32_t Flags{0};
  uint32_t Placement{static_cast<uint32_t>(MetadataBindingPlacement::Table)};
  // Index into MetadataFacts::Samplers. Only the SPIR-V lane fills it: a D3
  // static sampler stays inside the serialized RootSignature carrier.
  uint32_t SamplerIndex{kMetadataNoSampler};
  // Lane-local root struct that owns this declaration's CPU payload.
  uint32_t TypeIndex{kMetadataNoType};
  // The declaration's D3 register, recorded on both lanes. It is not published on
  // the wire; it exists so the cross-stage merge can reject a declaration whose
  // register depends on the stage, which would make one name describe different
  // resources on the two targets.
  bool HasDeclarationRegister{false};
  uint32_t DeclarationRegisterSpace{0};
  uint32_t DeclarationRegisterNumber{0};
};

// Immutable sampler state in Vulkan semantics. The numeric values mirror the
// Vulkan enums so the consumer needs no translation table; the D3 static
// sampler state is translated here because the policy mapping is compiler owned.
struct MetadataSamplerFact {
  uint32_t MagFilter{0};
  uint32_t MinFilter{0};
  uint32_t MipmapMode{0};
  uint32_t AddressModeU{0};
  uint32_t AddressModeV{0};
  uint32_t AddressModeW{0};
  float MipLodBias{0.0f};
  uint32_t AnisotropyEnable{0};
  float MaxAnisotropy{0.0f};
  uint32_t CompareEnable{0};
  uint32_t CompareOp{0};
  float MinLod{0.0f};
  float MaxLod{0.0f};
  uint32_t BorderColor{0};
  uint32_t ReductionMode{0};
  uint32_t Flags{0};
};

bool SameSamplerFact(const MetadataSamplerFact &left,
                     const MetadataSamplerFact &right) noexcept {
  return left.MagFilter == right.MagFilter &&
         left.MinFilter == right.MinFilter &&
         left.MipmapMode == right.MipmapMode &&
         left.AddressModeU == right.AddressModeU &&
         left.AddressModeV == right.AddressModeV &&
         left.AddressModeW == right.AddressModeW &&
         left.MipLodBias == right.MipLodBias &&
         left.AnisotropyEnable == right.AnisotropyEnable &&
         left.MaxAnisotropy == right.MaxAnisotropy &&
         left.CompareEnable == right.CompareEnable &&
         left.CompareOp == right.CompareOp && left.MinLod == right.MinLod &&
         left.MaxLod == right.MaxLod && left.BorderColor == right.BorderColor &&
         left.ReductionMode == right.ReductionMode && left.Flags == right.Flags;
}

// AST-level declaration record. This is the only authority for logical kind,
// array count and the DX register that associates a declaration with a policy
// parameter, so no lane has to reconstruct any of it from lowered output.
struct MetadataDeclarationFact {
  string Name;
  uint32_t Kind{static_cast<uint32_t>(MetadataBindingKind::Unknown)};
  uint32_t Count{1};
  bool HasRegister{false};
  uint32_t RegisterSpace{0};
  uint32_t RegisterNumber{0};
  bool HasVkPushConstant{false};
};

// One RootSignature policy parameter, flattened to what the ADR-0051 mapping
// table consumes. Descriptor table ranges become one entry per range.
struct MetadataPolicyParameter {
  uint32_t Kind{static_cast<uint32_t>(MetadataPolicyKind::Table)};
  uint32_t RegisterClass{0};
  uint32_t RegisterSpace{0};
  uint32_t BaseRegister{0};
  uint32_t RegisterCount{1};
  uint32_t StageMask{0};
  uint32_t Num32BitValues{0};
  uint32_t SamplerIndex{kMetadataNoSampler};
};

struct MetadataTypeFact {
  string Name;
  uint32_t ParentIndex{kMetadataNoParent};
  uint32_t Kind{0};
  uint32_t ElementCount{1};
  uint32_t Offset{0};
  uint32_t Size{0};
  uint32_t Stride{0};
  uint32_t Flags{0};
  uint32_t TypeIndex{kMetadataNoType};
  string UnderlyingType;
};

struct MetadataRootConstantFact {
  // Canonical declaration name. Push handles are built from it, so it is part
  // of the wire payload rather than a compiler-internal label.
  string Name;
  uint32_t RegisterSpace{0};
  uint32_t Register{0};
  uint32_t Offset{0};
  uint32_t Size{0};
  uint32_t StageMask{0};
  uint32_t Flags{0};
  // Lane-local root struct for the payload, or no type for policy-only facts.
  uint32_t TypeIndex{kMetadataNoType};
};

struct MetadataVertexInputFact {
  string Semantic;
  uint32_t SemanticIndex{0};
  uint32_t Location{0};
  uint32_t ComponentType{0};
  uint32_t ComponentCount{0};
  uint32_t Flags{0};
};

struct MetadataFacts {
  vector<MetadataBindingFact> Bindings;
  vector<MetadataTypeFact> Types;
  vector<MetadataRootConstantFact> RootConstants;
  vector<MetadataVertexInputFact> VertexInputs;
  // Immutable sampler states, in RootSignature static sampler order. SPIR-V
  // only: the DXIL lane keeps the serialized carrier as its single authority.
  vector<MetadataSamplerFact> Samplers;
  vector<uint8_t> SerializedRootSignature;
  Hash128 RootSignatureHash{};
  bool HasRootSignature{false};
};

struct Diagnostic {
  uint32_t Code{0};
  string Message;
};

Hash128 MakeContractHash(const ContractData &contract);

class RadRayContractCollector;

class RadRayContractPPCallbacks final : public clang::PPCallbacks {
public:
  explicit RadRayContractPPCallbacks(RadRayContractCollector &collector)
      : _collector(collector) {}

  void If(clang::SourceLocation, clang::SourceRange,
          ConditionValueKind) override;
  void Ifdef(clang::SourceLocation, const clang::Token &,
             const clang::MacroDefinition &) override;
  void Ifndef(clang::SourceLocation, const clang::Token &,
              const clang::MacroDefinition &) override;
  void Endif(clang::SourceLocation, clang::SourceLocation) override;

private:
  RadRayContractCollector &_collector;
};

class RadRayKeywordPragmaHandler final : public clang::PragmaHandler {
public:
  explicit RadRayKeywordPragmaHandler(RadRayContractCollector &collector)
      : clang::PragmaHandler("radray_keyword_group"), _collector(collector) {}

  void HandlePragma(clang::Preprocessor &pp,
                    clang::PragmaIntroducerKind introducer,
                    clang::Token &firstToken) override;

private:
  RadRayContractCollector &_collector;
};

// Captures the RootSignature parser's messages instead of letting them reach
// the compile. The DXIL lane already reports them through codegen, so routing
// them here keeps one stable RadRay diagnostic code per failure and avoids a
// duplicate message, while still surfacing the parser's detail.
class RadRayRootSignatureDiagnosticConsumer final
    : public clang::DiagnosticConsumer {
public:
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level != clang::DiagnosticsEngine::Error &&
        level != clang::DiagnosticsEngine::Fatal)
      return;
    llvm::SmallString<256> text;
    info.FormatDiagnostic(text);
    if (!_message.empty())
      _message += "; ";
    _message.append(text.begin(), text.end());
  }

  const string &Message() const noexcept { return _message; }

private:
  string _message;
};

class RadRayContractAstVisitor
    : public clang::RecursiveASTVisitor<RadRayContractAstVisitor> {
public:
  explicit RadRayContractAstVisitor(RadRayContractCollector &collector)
      : _collector(collector) {}

  bool VisitFunctionDecl(clang::FunctionDecl *decl);
  bool VisitVarDecl(clang::VarDecl *decl);
  bool VisitHLSLBufferDecl(clang::HLSLBufferDecl *decl);

private:
  RadRayContractCollector &_collector;
  vector<const clang::FunctionDecl *> _seen;
};

#ifdef ENABLE_SPIRV_CODEGEN
class RadRaySpirvResourceUseVisitor
    : public clang::RecursiveASTVisitor<RadRaySpirvResourceUseVisitor> {
public:
  RadRaySpirvResourceUseVisitor(
      clang::spirv::DeclResultIdMapper &mapper,
      vector<clang::spirv::SpirvVariable *> &resources)
      : _mapper(mapper), _resources(resources) {}

  void TraverseEntry(const clang::FunctionDecl *entry) {
    TraverseFunction(entry);
    for (size_t index = 0; index < _pending.size(); ++index)
      TraverseFunction(_pending[index]);
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *reference) {
    clang::spirv::SpirvInstruction *instruction =
        _mapper.getDeclEvalInfoIfRegistered(reference->getDecl());
    auto *variable = llvm::dyn_cast_or_null<clang::spirv::SpirvVariable>(
        instruction);
    if (variable != nullptr &&
        std::find(_resources.begin(), _resources.end(), variable) ==
            _resources.end())
      _resources.push_back(variable);
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *call) {
    const clang::FunctionDecl *callee = call->getDirectCallee();
    if (callee != nullptr && callee->hasBody() &&
        std::find(_pending.begin(), _pending.end(), callee->getCanonicalDecl()) ==
            _pending.end())
      _pending.push_back(callee->getCanonicalDecl());
    return true;
  }

private:
  void TraverseFunction(const clang::FunctionDecl *function) {
    if (function == nullptr || !function->hasBody())
      return;
    const clang::FunctionDecl *canonical = function->getCanonicalDecl();
    if (std::find(_seen.begin(), _seen.end(), canonical) != _seen.end())
      return;
    _seen.push_back(canonical);
    TraverseStmt(function->getBody());
  }

  clang::spirv::DeclResultIdMapper &_mapper;
  vector<clang::spirv::SpirvVariable *> &_resources;
  vector<const clang::FunctionDecl *> _seen;
  vector<const clang::FunctionDecl *> _pending;
};

class RadRaySpirvEntryPointFinder
    : public clang::RecursiveASTVisitor<RadRaySpirvEntryPointFinder> {
public:
  explicit RadRaySpirvEntryPointFinder(string_view entryPointName)
      : _entryPointName(entryPointName) {}

  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    if (_entryPoint == nullptr && decl->hasBody() &&
        decl->getNameAsString() == _entryPointName)
      _entryPoint = decl;
    return true;
  }

  const clang::FunctionDecl *EntryPoint() const noexcept { return _entryPoint; }

private:
  string_view _entryPointName;
  const clang::FunctionDecl *_entryPoint{nullptr};
};
#endif

class RadRayContractConsumer final : public clang::ASTConsumer {
public:
  explicit RadRayContractConsumer(RadRayContractCollector &collector)
      : _collector(collector) {}

  void HandleTranslationUnit(clang::ASTContext &context) override;

private:
  RadRayContractCollector &_collector;
};

class RadRayContractCollector final {
public:
  explicit RadRayContractCollector(string sourceName,
                                   RadRayDxcTarget target = RadRayDxcTarget::DXIL,
                                   ShaderStage stage = ShaderStage::Vertex)
      : _sourceName(std::move(sourceName)), _target(target), _stage(stage) {}

  void Install(clang::Preprocessor &pp) {
    _sourceManager = &pp.getSourceManager();
    pp.AddPragmaHandler(new RadRayKeywordPragmaHandler(*this));
    pp.addPPCallbacks(
        std::make_unique<RadRayContractPPCallbacks>(*this));
  }

  void AddKeywordGroup(KeywordGroup group, clang::SourceLocation location,
                       bool isMainFile, uint32_t conditionDepth);
  void AddEntry(clang::FunctionDecl &decl, string stage);
  void RecordDeclaration(const clang::NamedDecl &decl,
                         MetadataBindingKind kind, uint32_t count);
  void RecordRootSignature(const clang::FunctionDecl &decl,
                           const clang::HLSLRootSignatureAttr &attribute);
  const MetadataDeclarationFact *FindDeclaration(string_view name) const {
    for (const MetadataDeclarationFact &declaration : _declarations)
      if (declaration.Name == name)
        return &declaration;
    return nullptr;
  }
  bool HasRootSignaturePolicy() const noexcept { return _hasRootSignature; }
  const vector<uint8_t> &SerializedRootSignature() const noexcept {
    return _rootSignatureBytes;
  }
  void EnterCondition(clang::SourceLocation location) {
    _conditionStarts.push_back(location);
    ++_conditionDepth;
  }
  void LeaveCondition(clang::SourceLocation location) {
    if (!_conditionStarts.empty()) {
      _conditions.emplace_back(_conditionStarts.back(), location);
      _conditionStarts.pop_back();
    }
    if (_conditionDepth != 0)
      --_conditionDepth;
  }
  uint32_t ConditionDepth() const noexcept { return _conditionDepth; }
  bool IsInCondition(clang::SourceLocation location) const;
  void AddDiagnostic(uint32_t code, string message) {
    _diagnostics.push_back({code, std::move(message)});
  }

  void VisitAst(clang::ASTContext &context) {
    RadRayContractAstVisitor visitor(*this);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
  }

  bool Finalize(ContractData &contract, vector<Diagnostic> &diagnostics) const;

  // Applies the ADR-0051 mapping table to the declarations this lane found
  // live. Called once per lane, after the lane published its bindings.
  void ApplyRootSignaturePolicy();

  bool AppendDiagnostics(vector<Diagnostic> &diagnostics) const {
    diagnostics.insert(diagnostics.end(), _diagnostics.begin(), _diagnostics.end());
    return _diagnostics.empty();
  }

  const MetadataFacts &Metadata() const noexcept { return _metadata; }
  void CollectDxilModule(llvm::Module &module);
#ifdef ENABLE_SPIRV_CODEGEN
  void CollectSpirvAction(clang::EmitSpirvAction &action,
                          string_view entryPointName);
  void CollectSpirvVertexInputs(clang::spirv::SpirvEmitter &emitter);
#endif

private:
  friend class RadRayContractPPCallbacks;
  friend class RadRayKeywordPragmaHandler;
  friend class RadRayContractAstVisitor;

  bool FlattenRootSignaturePolicy(
      const hlsl::DxilVersionedRootSignatureDesc &desc);
  const MetadataPolicyParameter *FindPolicyParameter(uint32_t registerClass,
                                                    uint32_t registerSpace,
                                                    uint32_t registerNumber) const;

  string _sourceName;
  vector<KeywordGroup> _keywordGroups;
  vector<EntryPoint> _entryPoints;
  vector<Diagnostic> _diagnostics;
  clang::SourceManager *_sourceManager{nullptr};
  vector<clang::SourceLocation> _conditionStarts;
  vector<std::pair<clang::SourceLocation, clang::SourceLocation>> _conditions;
  uint32_t _conditionDepth{0};
  RadRayDxcTarget _target{RadRayDxcTarget::DXIL};
  ShaderStage _stage{ShaderStage::Vertex};
  MetadataFacts _metadata;
  vector<string> _dxilImplicitResourceNames;
  // AST-owned policy frontend state. Both lanes read exactly these facts, so a
  // lane never needs a second compile to learn the policy.
  vector<MetadataDeclarationFact> _declarations;
  vector<MetadataPolicyParameter> _policy;
  vector<MetadataSamplerFact> _policySamplers;
  string _rootSignatureSource;
  vector<uint8_t> _rootSignatureBytes;
  Hash128 _rootSignatureHash{};
  bool _hasRootSignatureSource{false};
  bool _hasRootSignature{false};
#ifdef ENABLE_SPIRV_CODEGEN
  bool _sawPushConstant{false};
#endif
};

void RadRayContractPPCallbacks::If(clang::SourceLocation location,
                                   clang::SourceRange,
                                   ConditionValueKind) {
  _collector.EnterCondition(location);
}

void RadRayContractPPCallbacks::Ifdef(clang::SourceLocation location,
                                      const clang::Token &,
                                      const clang::MacroDefinition &) {
  _collector.EnterCondition(location);
}

void RadRayContractPPCallbacks::Ifndef(clang::SourceLocation location,
                                       const clang::Token &,
                                       const clang::MacroDefinition &) {
  _collector.EnterCondition(location);
}

void RadRayContractPPCallbacks::Endif(clang::SourceLocation location,
                                      clang::SourceLocation) {
  _collector.LeaveCondition(location);
}

void RadRayKeywordPragmaHandler::HandlePragma(
    clang::Preprocessor &pp, clang::PragmaIntroducerKind,
    clang::Token &firstToken) {
  const clang::SourceLocation location = firstToken.getLocation();
  const bool isMainFile =
      pp.getSourceManager().isWrittenInMainFile(location);
  const uint32_t conditionDepth = _collector.ConditionDepth();

  clang::Token token;
  pp.LexUnexpandedToken(token);
  if (token.isNot(clang::tok::identifier)) {
    _collector.AddDiagnostic(3, "keyword group pragma is malformed");
    pp.DiscardUntilEndOfDirective();
    return;
  }

  KeywordGroup group;
  group.Name = token.getIdentifierInfo()->getName().str();
  while (true) {
    pp.LexUnexpandedToken(token);
    if (token.is(clang::tok::eod))
      break;
    if (token.isNot(clang::tok::string_literal)) {
      _collector.AddDiagnostic(3, "keyword group pragma is malformed");
      pp.DiscardUntilEndOfDirective();
      return;
    }
    clang::StringLiteralParser parser(llvm::ArrayRef<clang::Token>(&token, 1),
                                      pp);
    if (parser.hadError || !parser.isAscii() || parser.GetString().empty()) {
      _collector.AddDiagnostic(3, "keyword group pragma is malformed");
      pp.DiscardUntilEndOfDirective();
      return;
    }
    group.Values.emplace_back(parser.GetString().str());
  }

  if (group.Name.empty() || group.Values.empty()) {
    _collector.AddDiagnostic(3, "keyword group pragma is malformed");
    return;
  }
  _collector.AddKeywordGroup(std::move(group), location, isMainFile,
                             conditionDepth);
}

// Resource object type name, matching the keyword table clang itself uses to
// assign a resource class (CGHLSLMS KeywordToClass): the canonical record name
// for a plain object, the template name for a templated one.
string HlslResourceTypeName(const clang::ASTContext &context,
                            clang::QualType type) {
  type = type.getCanonicalType();
  if (const clang::RecordType *record = type->getAsStructureType())
    return record->getDecl()->getName().str();
  if (const clang::RecordType *record = type->getAs<clang::RecordType>())
    if (const auto *specialization =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                record->getDecl()))
      return specialization->getName().str();
  return string();
}

// Logical kind for a declaration type name. Object types outside the RadRay
// contract stay Unknown: they only become a diagnostic if the declaration turns
// out to be live, so an unused exotic declaration is not a contract failure.
MetadataBindingKind HlslResourceKindFromTypeName(const string &name) {
  if (name == "SamplerState" || name == "SamplerComparisonState")
    return MetadataBindingKind::Sampler;
  if (name == "ConstantBuffer")
    return MetadataBindingKind::CBuffer;
  if (name == "Buffer")
    return MetadataBindingKind::TypedBuffer;
  if (name == "RWBuffer" || name == "RasterizerOrderedBuffer")
    return MetadataBindingKind::RWTypedBuffer;
  if (name == "StructuredBuffer")
    return MetadataBindingKind::StructuredBuffer;
  if (name == "RWStructuredBuffer" || name == "AppendStructuredBuffer" ||
      name == "ConsumeStructuredBuffer" ||
      name == "RasterizerOrderedStructuredBuffer")
    return MetadataBindingKind::RWStructuredBuffer;
  if (name == "ByteAddressBuffer")
    return MetadataBindingKind::RawBuffer;
  if (name == "RWByteAddressBuffer" ||
      name == "RasterizerOrderedByteAddressBuffer")
    return MetadataBindingKind::RWRawBuffer;
  static const char *const kShapes[] = {"1D",      "1DArray", "2D",
                                        "2DArray", "2DMS",    "2DMSArray",
                                        "3D",      "Cube",    "CubeArray"};
  for (const char *shape : kShapes) {
    if (name == string("Texture") + shape)
      return MetadataBindingKind::Texture;
    if (name == string("RWTexture") + shape ||
        name == string("RasterizerOrderedTexture") + shape)
      return MetadataBindingKind::RWTexture;
  }
  return MetadataBindingKind::Unknown;
}

// Flattened declaration array count, 0 for an unbounded or non-constant extent.
uint32_t HlslResourceArrayCount(const clang::ASTContext &context,
                                clang::QualType &type) {
  uint32_t count = 1;
  while (const clang::ArrayType *array = context.getAsArrayType(type)) {
    const auto *constant = llvm::dyn_cast<clang::ConstantArrayType>(array);
    if (constant == nullptr) {
      count = 0;
    } else if (count != 0) {
      const uint64_t extent = constant->getSize().getZExtValue();
      const uint64_t total = static_cast<uint64_t>(count) * extent;
      count = total == 0 || total > kMaxCollectionCount
                  ? 0u
                  : static_cast<uint32_t>(total);
    }
    type = array->getElementType();
  }
  return count;
}

bool RadRayContractAstVisitor::VisitVarDecl(clang::VarDecl *decl) {
  if (!decl->isFileVarDecl())
    return true;
  const clang::ASTContext &context = decl->getASTContext();
  clang::QualType type = decl->getType();
  const uint32_t count = HlslResourceArrayCount(context, type);
  if (!hlsl::IsHLSLResourceType(type))
    return true;
  _collector.RecordDeclaration(
      *decl, HlslResourceKindFromTypeName(HlslResourceTypeName(context, type)),
      count);
  return true;
}

bool RadRayContractAstVisitor::VisitHLSLBufferDecl(
    clang::HLSLBufferDecl *decl) {
  // tbuffer stays Unknown: it is a texture buffer on the DXIL side and has no
  // place in the contract's logical kinds.
  _collector.RecordDeclaration(*decl,
                               decl->isCBuffer()
                                   ? MetadataBindingKind::CBuffer
                                   : MetadataBindingKind::Unknown,
                               1);
  return true;
}

bool RadRayContractAstVisitor::VisitFunctionDecl(clang::FunctionDecl *decl) {
  const clang::HLSLShaderAttr *attribute =
      decl->getAttr<clang::HLSLShaderAttr>();
  if (attribute == nullptr || !decl->hasBody())
    return true;

  const clang::FunctionDecl *canonical = decl->getCanonicalDecl();
  if (std::find(_seen.begin(), _seen.end(), canonical) != _seen.end())
    return true;
  _seen.push_back(canonical);
  _collector.AddEntry(*decl, attribute->getStage().str());
  // The policy is a translation-unit fact: every stage compile of the same
  // Variant parses the same attribute, so the lanes cannot drift apart.
  if (const clang::HLSLRootSignatureAttr *rootSignature =
          decl->getAttr<clang::HLSLRootSignatureAttr>())
    _collector.RecordRootSignature(*decl, *rootSignature);
  return true;
}

void RadRayContractConsumer::HandleTranslationUnit(
    clang::ASTContext &context) {
  _collector.VisitAst(context);
}

class RadRayContractAction final : public clang::WrapperFrontendAction {
public:
  RadRayContractAction(std::unique_ptr<clang::FrontendAction> action,
                       RadRayContractCollector &collector)
      : clang::WrapperFrontendAction(action.release()),
        _collector(collector) {}

protected:
  bool BeginSourceFileAction(clang::CompilerInstance &compiler,
                             clang::StringRef filename) override {
    _collector.Install(compiler.getPreprocessor());
    return clang::WrapperFrontendAction::BeginSourceFileAction(compiler,
                                                                filename);
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    clang::StringRef filename) override {
    std::unique_ptr<clang::ASTConsumer> wrapped =
        clang::WrapperFrontendAction::CreateASTConsumer(compiler, filename);
    if (!wrapped)
      return nullptr;
    vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    consumers.push_back(std::move(wrapped));
    consumers.push_back(std::make_unique<RadRayContractConsumer>(_collector));
    return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
  }

private:
  RadRayContractCollector &_collector;
};

class RadRayFrontendObserver final : public RadRayCompilerObserver {
public:
  explicit RadRayFrontendObserver(
      string sourceName, RadRayDxcTarget target = RadRayDxcTarget::DXIL,
      ShaderStage stage = ShaderStage::Vertex, bool syntaxOnly = false,
      string entryPointName = {})
      : _collector(std::move(sourceName), target, stage),
        _entryPointName(std::move(entryPointName)),
        _syntaxOnly(syntaxOnly) {}

  std::unique_ptr<clang::FrontendAction> WrapAction(
      std::unique_ptr<clang::FrontendAction> action) override {
    return std::make_unique<RadRayContractAction>(std::move(action),
                                                   _collector);
  }

  bool UseSyntaxOnly() const override { return _syntaxOnly; }

  void OnDxilModule(llvm::Module &module) override {
    _collector.CollectDxilModule(module);
  }

#ifdef ENABLE_SPIRV_CODEGEN
  void OnSpirvActionComplete(clang::EmitSpirvAction &action) override {
    _collector.CollectSpirvAction(action, _entryPointName);
  }
#endif

  bool Finalize(ContractData &contract, vector<Diagnostic> &diagnostics) const {
    return _collector.Finalize(contract, diagnostics);
  }

  bool AppendDiagnostics(vector<Diagnostic> &diagnostics) const {
    return _collector.AppendDiagnostics(diagnostics);
  }

  const MetadataFacts &Metadata() const noexcept { return _collector.Metadata(); }

private:
  RadRayContractCollector _collector;
  string _entryPointName;
  bool _syntaxOnly{false};
};

bool EndsWith(string_view value, string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ParseStage(string_view value, ShaderStage &stage) noexcept {
  if (value == "vertex") {
    stage = ShaderStage::Vertex;
    return true;
  }
  if (value == "pixel") {
    stage = ShaderStage::Pixel;
    return true;
  }
  if (value == "compute") {
    stage = ShaderStage::Compute;
    return true;
  }
  return false;
}

void RadRayContractCollector::AddKeywordGroup(
    KeywordGroup group, clang::SourceLocation, bool isMainFile,
    uint32_t conditionDepth) {
  if (!isMainFile || EndsWith(_sourceName, ".hlsli") || conditionDepth != 0) {
    AddDiagnostic(6,
                  "keyword group pragma must be in the root source outside conditions");
    return;
  }

  std::sort(group.Values.begin(), group.Values.end());
  for (const KeywordGroup &existing : _keywordGroups) {
    if (existing.Name == group.Name) {
      AddDiagnostic(4, "keyword group is declared more than once");
      return;
    }
    for (const string &left : existing.Values) {
      for (const string &right : group.Values) {
        if (left == right) {
          AddDiagnostic(15, "keyword value is repeated across groups");
          return;
        }
      }
    }
  }
  for (size_t index = 1; index < group.Values.size(); ++index) {
    if (group.Values[index - 1] == group.Values[index]) {
      AddDiagnostic(5, "keyword value is declared more than once");
      return;
    }
  }
  _keywordGroups.push_back(std::move(group));
}

bool RadRayContractCollector::IsInCondition(
    clang::SourceLocation location) const {
  if (!_sourceManager || location.isInvalid())
    return false;
  for (const auto &condition : _conditions) {
    if (condition.first.isInvalid() || condition.second.isInvalid())
      continue;
    if (!_sourceManager->isBeforeInTranslationUnit(location, condition.first) &&
        !_sourceManager->isBeforeInTranslationUnit(condition.second, location))
      return true;
  }
  return false;
}

void RadRayContractCollector::AddEntry(clang::FunctionDecl &decl,
                                        string stageName) {
  ShaderStage stage{};
  if (!ParseStage(stageName, stage)) {
    AddDiagnostic(9, "shader stage is not supported");
    return;
  }
  if (decl.getName().empty()) {
    AddDiagnostic(8, "stage entry function name is invalid");
    return;
  }
  if (IsInCondition(decl.getLocation())) {
    AddDiagnostic(7, "stage entry is inside conditional compilation");
    return;
  }
  _entryPoints.push_back({decl.getName().str(), stage});
}

void RadRayContractCollector::RecordDeclaration(const clang::NamedDecl &decl,
                                                MetadataBindingKind kind,
                                                uint32_t count) {
  const string name = decl.getNameAsString();
  if (name.empty())
    return;
  MetadataDeclarationFact fact;
  fact.Name = name;
  fact.Kind = static_cast<uint32_t>(kind);
  fact.Count = count;
  for (const hlsl::UnusualAnnotation *annotation :
       decl.getUnusualAnnotations()) {
    const auto *assignment =
        llvm::dyn_cast<hlsl::RegisterAssignment>(annotation);
    if (assignment == nullptr || assignment->RegisterType == 0)
      continue;
    fact.HasRegister = true;
    fact.RegisterNumber = assignment->RegisterNumber;
    fact.RegisterSpace = assignment->RegisterSpace.hasValue()
                             ? assignment->RegisterSpace.getValue()
                             : 0u;
  }
  // Only observable on the SPIR-V lane, where the attribute is in the language;
  // the DXIL lane associates a push declaration through its register instead.
  fact.HasVkPushConstant = decl.hasAttr<clang::VKPushConstantAttr>();
  // DXC has already assigned a register to every live resource by the time a
  // lane reports it, so the authored annotation is the only way to tell an
  // implicit assignment apart. Recorded before dead-resource removal, checked
  // after it.
  if (!fact.HasRegister &&
      std::find(_dxilImplicitResourceNames.begin(),
                _dxilImplicitResourceNames.end(), name) ==
          _dxilImplicitResourceNames.end())
    _dxilImplicitResourceNames.push_back(name);
  if (FindDeclaration(name) != nullptr)
    return;
  _declarations.push_back(std::move(fact));
}

bool RadRayContractCollector::Finalize(
    ContractData &contract, vector<Diagnostic> &diagnostics) const {
  diagnostics.insert(diagnostics.end(), _diagnostics.begin(), _diagnostics.end());
  if (!_diagnostics.empty())
    return false;

  contract = {};
  contract.KeywordGroups = _keywordGroups;
  contract.EntryPoints = _entryPoints;

  uint32_t vertexCount = 0;
  uint32_t pixelCount = 0;
  uint32_t computeCount = 0;
  for (const EntryPoint &entry : contract.EntryPoints) {
    vertexCount += entry.Stage == ShaderStage::Vertex;
    pixelCount += entry.Stage == ShaderStage::Pixel;
    computeCount += entry.Stage == ShaderStage::Compute;
  }
  if (computeCount != 0 && (vertexCount != 0 || pixelCount != 0)) {
    diagnostics.push_back(
        {14, "graphics and compute entries cannot share a source unit"});
    return false;
  }
  if (computeCount == 0 && vertexCount == 0) {
    diagnostics.push_back({10, "graphics source has no vertex entry"});
    return false;
  }
  if (computeCount == 0 && vertexCount > 1) {
    diagnostics.push_back({11, "graphics source has multiple vertex entries"});
    return false;
  }
  if (pixelCount > 1) {
    diagnostics.push_back({12, "graphics source has multiple pixel entries"});
    return false;
  }
  if (computeCount > 1) {
    diagnostics.push_back({13, "compute source has multiple entries"});
    return false;
  }

  contract.Kind = computeCount == 0 ? ShaderKind::Graphics : ShaderKind::Compute;
  std::sort(contract.KeywordGroups.begin(), contract.KeywordGroups.end(),
            [](const KeywordGroup &lhs, const KeywordGroup &rhs) {
              return lhs.Name < rhs.Name;
            });
  std::sort(contract.EntryPoints.begin(), contract.EntryPoints.end(),
            [](const EntryPoint &lhs, const EntryPoint &rhs) {
              return std::make_pair(lhs.Stage, lhs.Name) <
                     std::make_pair(rhs.Stage, rhs.Name);
            });
  contract.Hash = MakeContractHash(contract);
  return true;
}

uint32_t MetadataStageBitFor(ShaderStage stage) noexcept {
  return 1u << static_cast<uint8_t>(stage);
}

// D3D register class of a logical kind: b, t, u or s. Read-write kinds live in
// the u namespace, read-only ones in t.
uint32_t MetadataRegisterNamespace(MetadataBindingKind kind) noexcept {
  switch (kind) {
  case MetadataBindingKind::CBuffer:
    return 0;
  case MetadataBindingKind::TypedBuffer:
  case MetadataBindingKind::StructuredBuffer:
  case MetadataBindingKind::RawBuffer:
  case MetadataBindingKind::Texture:
    return 1;
  case MetadataBindingKind::RWTypedBuffer:
  case MetadataBindingKind::RWStructuredBuffer:
  case MetadataBindingKind::RWRawBuffer:
  case MetadataBindingKind::RWTexture:
    return 2;
  case MetadataBindingKind::Sampler:
    return 3;
  case MetadataBindingKind::Unknown:
    break;
  }
  return 0xffffffffu;
}

MetadataBindingFact *AddMetadataBinding(MetadataFacts &facts, string name,
                                        uint32_t group, uint32_t binding,
                                        MetadataBindingKind kind, uint32_t count,
                                        ShaderStage stage, uint32_t flags = 0) {
  if (name.empty())
    return nullptr;
  const auto found = std::find_if(
      facts.Bindings.begin(), facts.Bindings.end(),
      [&](const MetadataBindingFact &value) noexcept {
        return value.Name == name && value.Group == group &&
               value.Binding == binding &&
               value.Type == static_cast<uint32_t>(kind);
      });
  if (found != facts.Bindings.end()) {
    found->StageMask |= MetadataStageBitFor(stage);
    found->Count = std::max(found->Count, std::max(1u, count));
    found->Flags |= flags;
    return &*found;
  }
  facts.Bindings.push_back({std::move(name), group, binding,
                             MetadataRegisterNamespace(kind),
                             static_cast<uint32_t>(kind), std::max(1u, count),
                             MetadataStageBitFor(stage), flags});
  return &facts.Bindings.back();
}

uint32_t DxilScalarByteSize(const llvm::Type *type) noexcept {
  if (type == nullptr)
    return 0;
  if (type->isIntegerTy())
    return std::max(1u, type->getIntegerBitWidth() / 8u);
  if (type->isHalfTy())
    return 2;
  if (type->isFloatTy())
    return 4;
  if (type->isDoubleTy())
    return 8;
  return 0;
}

uint32_t DxilTypeSize(const llvm::Type *type,
                      const hlsl::DxilTypeSystem &typeSystem) noexcept {
  if (type == nullptr)
    return 0;
  if (const auto *structType = llvm::dyn_cast<llvm::StructType>(type)) {
    const hlsl::DxilStructAnnotation *annotation =
        typeSystem.GetStructAnnotation(structType);
    if (annotation != nullptr && annotation->GetCBufferSize() != 0)
      return annotation->GetCBufferSize();
    uint32_t size = 0;
    for (const llvm::Type *field : structType->elements())
      size += DxilTypeSize(field, typeSystem);
    return size;
  }
  if (const auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(type))
    return DxilTypeSize(arrayType->getElementType(), typeSystem) *
           static_cast<uint32_t>(arrayType->getNumElements());
  if (const auto *vectorType = llvm::dyn_cast<llvm::VectorType>(type))
    return DxilScalarByteSize(vectorType->getElementType()) *
           static_cast<uint32_t>(vectorType->getNumElements());
  return DxilScalarByteSize(type);
}

uint32_t DxilAnnotatedTypeSize(
    const llvm::Type *type, const hlsl::DxilFieldAnnotation &fieldAnnotation,
    const hlsl::DxilTypeSystem &typeSystem) noexcept {
  if (fieldAnnotation.HasMatrixAnnotation()) {
    const hlsl::DxilMatrixAnnotation &matrix =
        fieldAnnotation.GetMatrixAnnotation();
    const llvm::Type *elementType = type;
    if (const auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(elementType))
      elementType = arrayType->getElementType();
    if (const auto *vectorType = llvm::dyn_cast<llvm::VectorType>(elementType))
      elementType = vectorType->getElementType();
    const uint32_t scalarSize = DxilScalarByteSize(elementType);
    const uint32_t registerCount =
        matrix.Orientation == hlsl::MatrixOrientation::RowMajor
            ? matrix.Rows
            : matrix.Cols;
    return registerCount * 16u * std::max(1u, scalarSize / 4u);
  }
  return DxilTypeSize(type, typeSystem);
}

void CollectReachableDxilStructs(
    const llvm::Type *type, vector<const llvm::StructType *> &reachable) {
  if (type == nullptr)
    return;
  if (const auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(type)) {
    CollectReachableDxilStructs(arrayType->getElementType(), reachable);
    return;
  }
  const auto *structType = llvm::dyn_cast<llvm::StructType>(type);
  if (structType == nullptr)
    return;
  if (std::find(reachable.begin(), reachable.end(), structType) !=
      reachable.end())
    return;
  reachable.push_back(structType);
  for (const llvm::Type *field : structType->elements())
    CollectReachableDxilStructs(field, reachable);
}

uint32_t DxilFieldKind(const llvm::Type *type,
                       const hlsl::DxilFieldAnnotation &annotation) noexcept {
  if (annotation.HasMatrixAnnotation())
    return 3u;
  if (llvm::isa<llvm::StructType>(type))
    return 4u;
  if (llvm::isa<llvm::ArrayType>(type))
    return 5u;
  if (llvm::isa<llvm::VectorType>(type))
    return 2u;
  return 1u;
}

bool ResolveMetadataTypeIndices(vector<MetadataTypeFact> &types) {
  for (MetadataTypeFact &type : types) {
    if (type.UnderlyingType.empty())
      continue;
    const auto found = std::find_if(
        types.begin(), types.end(),
        [&](const MetadataTypeFact &candidate) noexcept {
          return candidate.ParentIndex == kMetadataNoParent &&
                 candidate.Kind == 4u && candidate.Name == type.UnderlyingType;
        });
    if (found != types.end()) {
      type.TypeIndex = static_cast<uint32_t>(found - types.begin());
    } else if (type.Kind == 4u) {
      return false;
    }
  }
  return true;
}

string DxilStructName(const llvm::StructType *type) {
  string name = type->getName().str();
  for (const string_view prefix : {string_view{"hostlayout.struct."},
                                   string_view{"struct."},
                                   string_view{"hostlayout."}}) {
    if (name.compare(0, prefix.size(), prefix) == 0) {
      name.erase(0, prefix.size());
      break;
    }
  }
  return name;
}

uint32_t CollectDxilTypeFacts(const hlsl::DxilTypeSystem &typeSystem,
                              const llvm::StructType *root,
                              MetadataFacts &facts) {
  vector<const llvm::StructType *> ordered;
  const auto collectType = [&](const llvm::Type *type, const auto &self) -> void {
    if (const auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      self(array->getElementType(), self);
      return;
    }
    const auto *structure = llvm::dyn_cast<llvm::StructType>(type);
    if (structure == nullptr ||
        std::find(ordered.begin(), ordered.end(), structure) != ordered.end())
      return;
    for (const llvm::Type *field : structure->elements())
      self(field, self);
    ordered.push_back(structure);
  };
  collectType(root, collectType);

  for (const llvm::StructType *type : ordered) {
    const string typeName = DxilStructName(type);
    const auto existing = std::find_if(
        facts.Types.begin(), facts.Types.end(),
        [&](const MetadataTypeFact &fact) noexcept {
          return fact.ParentIndex == kMetadataNoParent && fact.Kind == 4u &&
                 fact.Name == typeName;
        });
    if (existing != facts.Types.end())
      continue;
    const hlsl::DxilStructAnnotation *annotation =
        typeSystem.GetStructAnnotation(type);
    const uint32_t size = annotation != nullptr && annotation->GetCBufferSize() != 0
                              ? annotation->GetCBufferSize()
                              : DxilTypeSize(type, typeSystem);
    const uint32_t parentIndex = static_cast<uint32_t>(facts.Types.size());
    facts.Types.push_back({typeName, kMetadataNoParent, 4u, 1u, 0u, size,
                           size, 0u, kMetadataNoType, {}});
    const uint32_t parentSize = facts.Types[parentIndex].Size;
    vector<uint32_t> offsets;
    offsets.reserve(type->getNumElements());
    uint32_t runningOffset = 0;
    for (uint32_t fieldIndex = 0; fieldIndex < type->getNumElements();
         ++fieldIndex) {
      const hlsl::DxilFieldAnnotation *fieldAnnotation = nullptr;
      if (annotation != nullptr && fieldIndex < annotation->GetNumFields())
        fieldAnnotation = &annotation->GetFieldAnnotation(fieldIndex);
      uint32_t offset = runningOffset;
      if (fieldAnnotation != nullptr && fieldAnnotation->HasCBufferOffset())
        offset = fieldAnnotation->GetCBufferOffset();
      offsets.push_back(offset);
      runningOffset = offset + DxilTypeSize(type->getElementType(fieldIndex),
                                            typeSystem);
    }
    for (uint32_t fieldIndex = 0; fieldIndex < type->getNumElements();
         ++fieldIndex) {
      const hlsl::DxilFieldAnnotation *fieldAnnotation = nullptr;
      if (annotation != nullptr && fieldIndex < annotation->GetNumFields())
        fieldAnnotation = &annotation->GetFieldAnnotation(fieldIndex);
      hlsl::DxilFieldAnnotation emptyAnnotation;
      if (fieldAnnotation == nullptr)
        fieldAnnotation = &emptyAnnotation;
      const llvm::Type *fieldType = type->getElementType(fieldIndex);
      const uint32_t offset = offsets[fieldIndex];
      const uint32_t nextOffset = fieldIndex + 1 < offsets.size()
                                      ? offsets[fieldIndex + 1]
                                      : parentSize;
      const uint32_t size = nextOffset >= offset
                                ? nextOffset - offset
                                : DxilAnnotatedTypeSize(fieldType,
                                                        *fieldAnnotation,
                                                        typeSystem);
      uint32_t stride = DxilAnnotatedTypeSize(fieldType, *fieldAnnotation,
                                               typeSystem);
      uint32_t elementCount = 1;
      string underlying;
      uint32_t kind = DxilFieldKind(fieldType, *fieldAnnotation);
      if (kind == 3u)
        stride = size;
      if (const auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(fieldType)) {
        elementCount = static_cast<uint32_t>(arrayType->getNumElements());
        stride = DxilTypeSize(arrayType->getElementType(), typeSystem);
        if (const auto *elementStruct =
                llvm::dyn_cast<llvm::StructType>(arrayType->getElementType()))
          underlying = DxilStructName(elementStruct);
      } else if (const auto *fieldStruct =
                     llvm::dyn_cast<llvm::StructType>(fieldType)) {
        underlying = DxilStructName(fieldStruct);
      }
      if (kind == 3u) {
        elementCount = 1u;
        stride = size;
      }
      string name = fieldAnnotation->HasFieldName()
                        ? fieldAnnotation->GetFieldName()
                        : ("field" + std::to_string(fieldIndex));
      facts.Types.push_back({std::move(name), parentIndex, kind, elementCount,
                             offset, size, stride, 0u, kMetadataNoType,
                             std::move(underlying)});
    }
  }

  if (!ResolveMetadataTypeIndices(facts.Types))
    return kMetadataNoType;
  const string rootName = DxilStructName(root);
  const auto owner = std::find_if(
      facts.Types.begin(), facts.Types.end(),
      [&](const MetadataTypeFact &fact) noexcept {
        return fact.ParentIndex == kMetadataNoParent && fact.Kind == 4u &&
               fact.Name == rootName;
      });
  return owner == facts.Types.end()
             ? kMetadataNoType
             : static_cast<uint32_t>(owner - facts.Types.begin());
}

uint32_t DxilRegisterClass(hlsl::DXIL::ResourceClass resourceClass) noexcept {
  switch (resourceClass) {
  case hlsl::DXIL::ResourceClass::CBuffer:
    return 0;
  case hlsl::DXIL::ResourceClass::SRV:
    return 1;
  case hlsl::DXIL::ResourceClass::UAV:
    return 2;
  case hlsl::DXIL::ResourceClass::Sampler:
    return 3;
  }
  return 0xffffffffu;
}

// Logical kind implied by the DXIL lowering. The declaration is the contract's
// authority; this is the cross-check that catches a lowering which disagrees
// with the declared type, and the fallback for a resource with no declaration.
MetadataBindingKind DxilBindingKind(const hlsl::DxilResourceBase &resource) {
  if (resource.GetClass() == hlsl::DXIL::ResourceClass::CBuffer)
    return MetadataBindingKind::CBuffer;
  if (resource.GetClass() == hlsl::DXIL::ResourceClass::Sampler)
    return MetadataBindingKind::Sampler;
  const hlsl::DXIL::ResourceKind kind = resource.GetKind();
  const bool rw = resource.GetClass() == hlsl::DXIL::ResourceClass::UAV;
  switch (kind) {
  case hlsl::DXIL::ResourceKind::TypedBuffer:
    return rw ? MetadataBindingKind::RWTypedBuffer
              : MetadataBindingKind::TypedBuffer;
  case hlsl::DXIL::ResourceKind::StructuredBuffer:
    return rw ? MetadataBindingKind::RWStructuredBuffer
              : MetadataBindingKind::StructuredBuffer;
  case hlsl::DXIL::ResourceKind::RawBuffer:
    return rw ? MetadataBindingKind::RWRawBuffer
              : MetadataBindingKind::RawBuffer;
  default:
    break;
  }
  if (hlsl::DxilResource::IsAnyTexture(kind))
    return rw ? MetadataBindingKind::RWTexture : MetadataBindingKind::Texture;
  return MetadataBindingKind::Unknown;
}

MetadataBindingFact *AddDxilResourceFact(
    const hlsl::DxilResourceBase &resource,
    const MetadataDeclarationFact *declaration, MetadataFacts &facts,
    ShaderStage stage, vector<Diagnostic> &diagnostics) {
  const uint32_t registerClass = DxilRegisterClass(resource.GetClass());
  if (registerClass == 0xffffffffu)
    return nullptr;
  const string name = resource.GetGlobalName();
  const MetadataBindingKind lowered = DxilBindingKind(resource);
  const MetadataBindingKind kind =
      declaration != nullptr
          ? static_cast<MetadataBindingKind>(declaration->Kind)
          : lowered;
  if (kind == MetadataBindingKind::Unknown) {
    diagnostics.push_back(
        {2118, "resource '" + name +
                   "' has a declaration type outside the RadRay shader contract"});
    return nullptr;
  }
  if (lowered != MetadataBindingKind::Unknown && lowered != kind) {
    diagnostics.push_back(
        {2119, "resource '" + name +
                   "' logical kind disagrees with its DXIL lowering"});
    return nullptr;
  }
  const uint32_t binding = resource.GetLowerBound();
  const uint32_t group = resource.GetSpaceID() == UINT_MAX
                             ? 0u
                             : resource.GetSpaceID();
  const uint32_t count = declaration != nullptr && declaration->Count != 0
                             ? declaration->Count
                             : resource.GetRangeSize();
  MetadataBindingFact *fact =
      AddMetadataBinding(facts, name, group, binding, kind, count, stage);
  if (fact != nullptr)
    fact->RegisterClass = registerClass;
  return fact;
}

uint32_t DxilRootRangeClass(hlsl::DxilDescriptorRangeType type) noexcept {
  switch (type) {
  case hlsl::DxilDescriptorRangeType::CBV:
    return 0;
  case hlsl::DxilDescriptorRangeType::SRV:
    return 1;
  case hlsl::DxilDescriptorRangeType::UAV:
    return 2;
  case hlsl::DxilDescriptorRangeType::Sampler:
    return 3;
  default:
    return 0xffffffffu;
  }
}

// Maps the author-declared root parameter visibility to wire stage bits.
// Compute contracts always collapse to the compute stage bit.
uint32_t RootVisibilityStageMask(hlsl::DxilShaderVisibility visibility,
                                 ShaderStage stage) noexcept {
  if (stage == ShaderStage::Compute)
    return MetadataStageBitFor(ShaderStage::Compute);
  switch (visibility) {
  case hlsl::DxilShaderVisibility::Vertex:
    return MetadataStageBitFor(ShaderStage::Vertex);
  case hlsl::DxilShaderVisibility::Pixel:
    return MetadataStageBitFor(ShaderStage::Pixel);
  case hlsl::DxilShaderVisibility::All:
    return MetadataStageBitFor(ShaderStage::Vertex) |
           MetadataStageBitFor(ShaderStage::Pixel);
  default:
    return MetadataStageBitFor(stage);
  }
}

// Vulkan-semantics translation of a D3D static sampler. The stored numbers are
// the official Vulkan enumerant values so the backend can consume them without
// a second mapping table; the RadRay side static_asserts them against volk.
MetadataSamplerFact
MetadataSamplerFromStatic(const hlsl::DxilStaticSamplerDesc &desc) {
  const auto addressMode = [](hlsl::DxilTextureAddressMode mode) -> uint32_t {
    switch (mode) {
    case hlsl::DxilTextureAddressMode::Wrap:
      return 0; // VK_SAMPLER_ADDRESS_MODE_REPEAT
    case hlsl::DxilTextureAddressMode::Mirror:
      return 1; // VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
    case hlsl::DxilTextureAddressMode::Clamp:
      return 2; // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
    case hlsl::DxilTextureAddressMode::Border:
      return 3; // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
    case hlsl::DxilTextureAddressMode::MirrorOnce:
      return 4; // VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
    default:
      return 0;
    }
  };
  const auto borderColor = [](hlsl::DxilStaticBorderColor color) -> uint32_t {
    switch (color) {
    case hlsl::DxilStaticBorderColor::TransparentBlack:
      return 0; // VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    case hlsl::DxilStaticBorderColor::OpaqueBlack:
      return 2; // VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
    case hlsl::DxilStaticBorderColor::OpaqueWhite:
      return 4; // VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
    case hlsl::DxilStaticBorderColor::OpaqueBlackUint:
      return 3; // VK_BORDER_COLOR_INT_OPAQUE_BLACK
    case hlsl::DxilStaticBorderColor::OpaqueWhiteUint:
      return 5; // VK_BORDER_COLOR_INT_OPAQUE_WHITE
    default:
      return 0;
    }
  };
  // D3D filter bit layout: mip at bit 0, mag at bit 2, min at bit 4, the
  // anisotropic bit at 0x40 and the reduction type at bits 7-8.
  const uint32_t filter = static_cast<uint32_t>(desc.Filter);
  const bool anisotropic = (filter & 0x40u) != 0u;
  const uint32_t reduction = (filter >> 7) & 0x3u;
  const auto filterType = [&](uint32_t shift) -> uint32_t {
    return anisotropic || ((filter >> shift) & 0x3u) != 0u ? 1u : 0u;
  };

  MetadataSamplerFact fact;
  fact.MipmapMode = filterType(0);
  fact.MagFilter = filterType(2);
  fact.MinFilter = filterType(4);
  fact.AddressModeU = addressMode(desc.AddressU);
  fact.AddressModeV = addressMode(desc.AddressV);
  fact.AddressModeW = addressMode(desc.AddressW);
  fact.MipLodBias = desc.MipLODBias;
  fact.AnisotropyEnable = anisotropic ? 1u : 0u;
  // Vulkan requires maxAnisotropy >= 1.0 whenever the feature is enabled and
  // ignores it otherwise.
  fact.MaxAnisotropy =
      anisotropic ? std::max(1.0f, static_cast<float>(desc.MaxAnisotropy))
                  : 1.0f;
  fact.CompareEnable = reduction == 1u ? 1u : 0u;
  fact.CompareOp =
      desc.ComparisonFunc == hlsl::DxilComparisonFunc::None
          ? 0u
          : static_cast<uint32_t>(desc.ComparisonFunc) - 1u;
  fact.MinLod = desc.MinLOD;
  // An open-ended D3D clamp becomes VK_LOD_CLAMP_NONE.
  fact.MaxLod = desc.MaxLOD >= DxilFloat32Max ? 1000.0f : desc.MaxLOD;
  fact.BorderColor = borderColor(desc.BorderColor);
  fact.ReductionMode = reduction == 2u ? 1u : reduction == 3u ? 2u : 0u;
  fact.Flags = 0u;
  return fact;
}

uint32_t
MetadataRootDescriptorClass(hlsl::DxilRootParameterType type) noexcept {
  switch (type) {
  case hlsl::DxilRootParameterType::CBV:
    return 0;
  case hlsl::DxilRootParameterType::SRV:
    return 1;
  case hlsl::DxilRootParameterType::UAV:
    return 2;
  default:
    return 0xffffffffu;
  }
}

// True when a policy parameter covers a single register slot. Unbounded ranges
// (NumDescriptors == UINT_MAX) cover everything from the base upwards.
bool MetadataPolicyCovers(const MetadataPolicyParameter &parameter,
                         uint32_t registerClass, uint32_t registerSpace,
                         uint32_t registerNumber) noexcept {
  if (parameter.RegisterClass != registerClass ||
      parameter.RegisterSpace != registerSpace ||
      registerNumber < parameter.BaseRegister)
    return false;
  if (parameter.RegisterCount == UINT_MAX)
    return true;
  return registerNumber - parameter.BaseRegister < parameter.RegisterCount;
}

bool MetadataPolicyOverlaps(const MetadataPolicyParameter &lhs,
                            const MetadataPolicyParameter &rhs) noexcept {
  if (lhs.RegisterClass != rhs.RegisterClass ||
      lhs.RegisterSpace != rhs.RegisterSpace)
    return false;
  const auto last = [](const MetadataPolicyParameter &value) -> uint64_t {
    if (value.RegisterCount == UINT_MAX || value.RegisterCount == 0)
      return UINT_MAX;
    return static_cast<uint64_t>(value.BaseRegister) + value.RegisterCount - 1;
  };
  return lhs.BaseRegister <= last(rhs) && rhs.BaseRegister <= last(lhs);
}

void RadRayContractCollector::RecordRootSignature(
    const clang::FunctionDecl &decl,
    const clang::HLSLRootSignatureAttr &attribute) {
  const string source = attribute.getSignatureName().str();
  if (_hasRootSignatureSource) {
    // Every stage compile of one Variant sees the whole translation unit, so the
    // policy has to be a single translation-unit fact for the lanes to agree.
    if (source != _rootSignatureSource)
      AddDiagnostic(2105, "translation unit declares more than one distinct "
                          "[RootSignature] policy");
    return;
  }
  _hasRootSignatureSource = true;
  _rootSignatureSource = source;

  clang::ASTContext &context = decl.getASTContext();
  const hlsl::DxilRootSignatureVersion version =
      context.getLangOpts().RootSigMinor == 0
          ? hlsl::DxilRootSignatureVersion::Version_1_0
          : hlsl::DxilRootSignatureVersion::Version_1_1;
  // A private engine: on the DXIL lane codegen parses the same attribute and
  // would report the identical message, so routing the parser here keeps one
  // stable RadRay code per failure instead of a duplicated clang error.
  RadRayRootSignatureDiagnosticConsumer consumer;
  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> ids(new clang::DiagnosticIDs());
  llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> options(
      new clang::DiagnosticOptions());
  clang::DiagnosticsEngine diagnostics(ids, options.get(), &consumer, false);
  diagnostics.setSourceManager(&context.getSourceManager());
  hlsl::RootSignatureHandle handle;
  clang::CompileRootSignature(
      source, diagnostics, decl.getLocation(), version,
      hlsl::DxilRootSignatureCompilationFlags::GlobalRootSignature, &handle);
  if (handle.IsEmpty() || diagnostics.hasErrorOccurred()) {
    AddDiagnostic(2117, "[RootSignature] could not be compiled: " +
                            (consumer.Message().empty()
                                 ? string("unknown root signature error")
                                 : consumer.Message()));
    return;
  }
  handle.EnsureSerializedAvailable();
  const uint8_t *serialized = handle.GetSerializedBytes();
  const uint32_t serializedSize = handle.GetSerializedSize();
  if (serialized == nullptr || serializedSize == 0) {
    AddDiagnostic(2117, "[RootSignature] produced no serialized payload");
    return;
  }
  const hlsl::DxilVersionedRootSignatureDesc *desc = handle.GetDesc();
  if (desc == nullptr) {
    AddDiagnostic(2117, "[RootSignature] produced no decoded description");
    return;
  }
  if (!FlattenRootSignaturePolicy(*desc))
    return;
  _rootSignatureBytes.assign(serialized, serialized + serializedSize);
  _rootSignatureHash = Digest(_rootSignatureBytes, 0x52534947ull);
  _hasRootSignature = true;
}

bool RadRayContractCollector::FlattenRootSignaturePolicy(
    const hlsl::DxilVersionedRootSignatureDesc &root) {
  _policy.clear();
  _policySamplers.clear();
  bool ok = true;
  const auto addRange = [&](uint32_t rangeType, uint32_t baseRegister,
                            uint32_t registerSpace, uint32_t numDescriptors,
                            uint32_t stageMask) {
    const uint32_t registerClass = DxilRootRangeClass(
        static_cast<hlsl::DxilDescriptorRangeType>(rangeType));
    if (registerClass == 0xffffffffu) {
      AddDiagnostic(2117, "[RootSignature] descriptor range type is outside the "
                          "RadRay shader contract");
      ok = false;
      return;
    }
    MetadataPolicyParameter parameter;
    parameter.Kind = static_cast<uint32_t>(MetadataPolicyKind::Table);
    parameter.RegisterClass = registerClass;
    parameter.RegisterSpace = registerSpace;
    parameter.BaseRegister = baseRegister;
    parameter.RegisterCount = numDescriptors;
    parameter.StageMask = stageMask;
    _policy.push_back(parameter);
  };
  const auto addParameters = [&](const auto *parameters,
                                 uint32_t numParameters) {
    for (uint32_t index = 0; index < numParameters; ++index) {
      const auto &parameter = parameters[index];
      const uint32_t stageMask =
          RootVisibilityStageMask(parameter.ShaderVisibility, _stage);
      if (parameter.ParameterType ==
          hlsl::DxilRootParameterType::DescriptorTable) {
        for (uint32_t rangeIndex = 0;
             rangeIndex < parameter.DescriptorTable.NumDescriptorRanges;
             ++rangeIndex) {
          const auto &range =
              parameter.DescriptorTable.pDescriptorRanges[rangeIndex];
          addRange(static_cast<uint32_t>(range.RangeType),
                   range.BaseShaderRegister, range.RegisterSpace,
                   range.NumDescriptors, stageMask);
        }
        continue;
      }
      if (parameter.ParameterType ==
          hlsl::DxilRootParameterType::Constants32Bit) {
        MetadataPolicyParameter constants;
        constants.Kind =
            static_cast<uint32_t>(MetadataPolicyKind::RootConstants);
        constants.RegisterClass = 0u;
        constants.RegisterSpace = parameter.Constants.RegisterSpace;
        constants.BaseRegister = parameter.Constants.ShaderRegister;
        constants.RegisterCount = 1u;
        constants.StageMask = stageMask;
        constants.Num32BitValues = parameter.Constants.Num32BitValues;
        _policy.push_back(constants);
        continue;
      }
      const uint32_t registerClass =
          MetadataRootDescriptorClass(parameter.ParameterType);
      if (registerClass == 0xffffffffu) {
        AddDiagnostic(2117, "[RootSignature] root parameter type is outside the "
                            "RadRay shader contract");
        ok = false;
        continue;
      }
      MetadataPolicyParameter descriptor;
      descriptor.Kind =
          static_cast<uint32_t>(MetadataPolicyKind::RootDescriptor);
      descriptor.RegisterClass = registerClass;
      descriptor.RegisterSpace = parameter.Descriptor.RegisterSpace;
      descriptor.BaseRegister = parameter.Descriptor.ShaderRegister;
      descriptor.RegisterCount = 1u;
      descriptor.StageMask = stageMask;
      _policy.push_back(descriptor);
    }
  };
  const auto addStaticSamplers = [&](const hlsl::DxilStaticSamplerDesc *samplers,
                                     uint32_t numSamplers) {
    for (uint32_t index = 0; index < numSamplers; ++index) {
      const hlsl::DxilStaticSamplerDesc &sampler = samplers[index];
      MetadataPolicyParameter parameter;
      parameter.Kind = static_cast<uint32_t>(MetadataPolicyKind::StaticSampler);
      parameter.RegisterClass = 3u;
      parameter.RegisterSpace = sampler.RegisterSpace;
      parameter.BaseRegister = sampler.ShaderRegister;
      parameter.RegisterCount = 1u;
      parameter.StageMask =
          RootVisibilityStageMask(sampler.ShaderVisibility, _stage);
      // The whole static sampler table is published, referenced or not, so the
      // sampler index of a given slot does not depend on shader liveness.
      parameter.SamplerIndex = static_cast<uint32_t>(_policySamplers.size());
      _policySamplers.push_back(MetadataSamplerFromStatic(sampler));
      _policy.push_back(parameter);
    }
  };

  if (root.Version == hlsl::DxilRootSignatureVersion::Version_1_1) {
    addParameters(root.Desc_1_1.pParameters, root.Desc_1_1.NumParameters);
    addStaticSamplers(root.Desc_1_1.pStaticSamplers,
                      root.Desc_1_1.NumStaticSamplers);
  } else {
    addParameters(root.Desc_1_0.pParameters, root.Desc_1_0.NumParameters);
    addStaticSamplers(root.Desc_1_0.pStaticSamplers,
                      root.Desc_1_0.NumStaticSamplers);
  }

  // Overlapping register ranges would make the placement of a binding
  // ambiguous. D3D12 rejects them too, so this only ever fires before the
  // runtime would.
  for (size_t left = 0; left + 1 < _policy.size(); ++left)
    for (size_t right = left + 1; right < _policy.size(); ++right)
      if (MetadataPolicyOverlaps(_policy[left], _policy[right])) {
        AddDiagnostic(2104, "[RootSignature] covers the same register range "
                            "more than once");
        return false;
      }
  return ok;
}

const MetadataPolicyParameter *RadRayContractCollector::FindPolicyParameter(
    uint32_t registerClass, uint32_t registerSpace,
    uint32_t registerNumber) const {
  for (const MetadataPolicyParameter &parameter : _policy)
    if (MetadataPolicyCovers(parameter, registerClass, registerSpace,
                             registerNumber))
      return &parameter;
  return nullptr;
}

void RadRayContractCollector::ApplyRootSignaturePolicy() {
  const uint32_t stageBit = MetadataStageBitFor(_stage);
  // Stamped before any policy work and on both lanes, so the cross-stage merge sees
  // it whether or not a policy exists.
  for (MetadataBindingFact &binding : _metadata.Bindings) {
    const MetadataDeclarationFact *declaration = FindDeclaration(binding.Name);
    if (declaration == nullptr || !declaration->HasRegister)
      continue;
    binding.HasDeclarationRegister = true;
    binding.DeclarationRegisterSpace = declaration->RegisterSpace;
    binding.DeclarationRegisterNumber = declaration->RegisterNumber;
  }
  if (!_hasRootSignature) {
    // No policy: everything stays in a descriptor table and the lane's own
    // observations are published unchanged. Push blocks still gain their
    // declaration name, which is a translation-unit fact either way.
    for (MetadataRootConstantFact &constants : _metadata.RootConstants) {
      if (!constants.Name.empty())
        continue;
      for (const MetadataDeclarationFact &declaration : _declarations)
        if (declaration.HasRegister &&
            declaration.RegisterSpace == constants.RegisterSpace &&
            declaration.RegisterNumber == constants.Register)
          constants.Name = declaration.Name;
    }
    return;
  }

  // The hash identifies the policy itself, so it is taken over the frontend's
  // raw serialized bytes on both lanes even though only D3D ships a carrier.
  _metadata.RootSignatureHash = _rootSignatureHash;
  _metadata.HasRootSignature = true;
  if (_target == RadRayDxcTarget::DXIL) {
    // D3D's explicit topology lives in the serialized carrier, which stays the
    // single authority for static samplers there.
    _metadata.SerializedRootSignature = _rootSignatureBytes;
  } else {
    // Vulkan builds its immutable samplers from the records instead.
    _metadata.Samplers = _policySamplers;
  }

  // What this lane actually observed as a push block, checked against the
  // policy before the policy-derived records replace it.
  const vector<MetadataRootConstantFact> observed = _metadata.RootConstants;
  const vector<MetadataBindingFact> observedBindings = _metadata.Bindings;
  _metadata.RootConstants.clear();

  vector<MetadataBindingFact> bindings;
  bindings.reserve(_metadata.Bindings.size());
  for (MetadataBindingFact &binding : _metadata.Bindings) {
    const MetadataDeclarationFact *declaration = FindDeclaration(binding.Name);
    const MetadataPolicyParameter *parameter =
        declaration != nullptr && declaration->HasRegister
            ? FindPolicyParameter(binding.RegisterClass,
                                  declaration->RegisterSpace,
                                  declaration->RegisterNumber)
            : nullptr;
    if (parameter == nullptr) {
      // The DXIL lane sees every resource the policy has to cover. A resource
      // with no D3D register cannot be addressed by the policy at all, so on the
      // SPIR-V lane it is target-only and stays in a descriptor table.
      if (_target == RadRayDxcTarget::DXIL)
        AddDiagnostic(2121, "resource '" + binding.Name +
                                "' is not covered by the [RootSignature] policy");
      bindings.push_back(std::move(binding));
      continue;
    }
    if ((parameter->StageMask & stageBit) == 0u) {
      AddDiagnostic(2123, "resource '" + binding.Name +
                              "' is used by a stage the [RootSignature] policy "
                              "does not make it visible to");
      continue;
    }
    const MetadataPolicyKind kind =
        static_cast<MetadataPolicyKind>(parameter->Kind);
    const MetadataBindingKind bindingKind =
        static_cast<MetadataBindingKind>(binding.Type);
    if (kind == MetadataPolicyKind::RootConstants) {
      // A push block is not a descriptor on either lane. The record itself is
      // rebuilt from the policy below.
      if (_target != RadRayDxcTarget::DXIL)
        AddDiagnostic(2124, "resource '" + binding.Name +
                                "' is root constants in the policy but a "
                                "descriptor on the SPIR-V lane; it needs "
                                "[[vk::push_constant]]");
      continue;
    }
    if (kind == MetadataPolicyKind::StaticSampler) {
      if (bindingKind != MetadataBindingKind::Sampler) {
        AddDiagnostic(2122, "resource '" + binding.Name +
                                "' is not a sampler but the policy places it in "
                                "a static sampler slot");
        continue;
      }
      // D3D keeps a static sampler inside the serialized carrier, so it owns no
      // table slot; Vulkan binds it as an immutable sampler in the set.
      if (_target == RadRayDxcTarget::DXIL) {
        binding.Placement =
            static_cast<uint32_t>(MetadataBindingPlacement::StaticSampler);
      } else {
        binding.Placement =
            static_cast<uint32_t>(MetadataBindingPlacement::Table);
        binding.SamplerIndex = parameter->SamplerIndex;
      }
      bindings.push_back(std::move(binding));
      continue;
    }
    if (kind == MetadataPolicyKind::RootDescriptor) {
      const bool addressable = bindingKind == MetadataBindingKind::CBuffer ||
                               bindingKind ==
                                   MetadataBindingKind::StructuredBuffer ||
                               bindingKind ==
                                   MetadataBindingKind::RWStructuredBuffer ||
                               bindingKind == MetadataBindingKind::RawBuffer ||
                               bindingKind == MetadataBindingKind::RWRawBuffer;
      if (!addressable || binding.Count != 1u) {
        AddDiagnostic(2122, "resource '" + binding.Name +
                                "' cannot be a root descriptor: only a single "
                                "buffer or constant buffer can be bound by "
                                "address");
        continue;
      }
      binding.Placement =
          static_cast<uint32_t>(MetadataBindingPlacement::RootDescriptor);
      bindings.push_back(std::move(binding));
      continue;
    }
    binding.Placement = static_cast<uint32_t>(MetadataBindingPlacement::Table);
    bindings.push_back(std::move(binding));
  }
  _metadata.Bindings = std::move(bindings);

  for (const MetadataRootConstantFact &fact : observed) {
    const MetadataPolicyParameter *parameter =
        FindPolicyParameter(0u, fact.RegisterSpace, fact.Register);
    if (parameter == nullptr ||
        parameter->Kind !=
            static_cast<uint32_t>(MetadataPolicyKind::RootConstants)) {
      AddDiagnostic(2124, "push constant block at b" +
                              std::to_string(fact.Register) + " space" +
                              std::to_string(fact.RegisterSpace) +
                              " is not declared as RootConstants by the policy");
      continue;
    }
    if (fact.Size > parameter->Num32BitValues * 4u)
      AddDiagnostic(2124, "push constant block at b" +
                              std::to_string(fact.Register) + " space" +
                              std::to_string(fact.RegisterSpace) +
                              " is larger than the RootConstants the policy "
                              "declares");
  }

  // Root constants come from the policy, not from liveness, so both lanes
  // publish the same records for a given stage. A parameter no declaration
  // matches has no push handle and is skipped.
  for (const MetadataPolicyParameter &parameter : _policy) {
    if (parameter.Kind !=
            static_cast<uint32_t>(MetadataPolicyKind::RootConstants) ||
        (parameter.StageMask & stageBit) == 0u)
      continue;
    const MetadataDeclarationFact *declaration = nullptr;
    for (const MetadataDeclarationFact &candidate : _declarations)
      if (candidate.HasRegister &&
          candidate.RegisterSpace == parameter.RegisterSpace &&
          candidate.RegisterNumber == parameter.BaseRegister &&
          candidate.Kind == static_cast<uint32_t>(MetadataBindingKind::CBuffer))
        declaration = &candidate;
    if (declaration == nullptr)
      continue;
    if (_target != RadRayDxcTarget::DXIL && !declaration->HasVkPushConstant) {
      AddDiagnostic(2124, "'" + declaration->Name +
                              "' is root constants in the policy, so it needs "
                              "[[vk::push_constant]] to lower the same way on "
                              "both targets");
      continue;
    }
    MetadataRootConstantFact fact;
    fact.Name = declaration->Name;
    fact.RegisterSpace = parameter.RegisterSpace;
    fact.Register = parameter.BaseRegister;
    fact.Offset = 0u;
    fact.Size = parameter.Num32BitValues * 4u;
    fact.StageMask = stageBit;
    fact.Flags = 0u;
    for (const MetadataBindingFact &binding : observedBindings)
      if (binding.RegisterClass == 0u && binding.HasDeclarationRegister &&
          binding.DeclarationRegisterSpace == parameter.RegisterSpace &&
          binding.DeclarationRegisterNumber == parameter.BaseRegister)
        fact.TypeIndex = binding.TypeIndex;
    for (const MetadataRootConstantFact &observedFact : observed)
      if (observedFact.RegisterSpace == parameter.RegisterSpace &&
          observedFact.Register == parameter.BaseRegister &&
          observedFact.TypeIndex != kMetadataNoType) {
        if (fact.TypeIndex != kMetadataNoType &&
            fact.TypeIndex != observedFact.TypeIndex)
          AddDiagnostic(2124, "root constant payload owner is inconsistent");
        else
          fact.TypeIndex = observedFact.TypeIndex;
      }
    _metadata.RootConstants.push_back(std::move(fact));
  }
}

void RadRayContractCollector::CollectDxilModule(llvm::Module &module) {
  hlsl::DxilModule &dxil = module.GetOrCreateDxilModule();
  dxil.RemoveUnusedResources();
  // DXC has already assigned a lower bound to every surviving resource. Use the
  // AST-side record to distinguish an authored register() from that automatic
  // assignment, but only after dead-resource removal so unused declarations do
  // not become contract failures.
  const auto requireExplicitBinding =
      [&](const hlsl::DxilResourceBase &resource) -> bool {
    const string name = resource.GetGlobalName();
    const bool implicit =
        name == "$Globals" ||
        std::find(_dxilImplicitResourceNames.begin(),
                  _dxilImplicitResourceNames.end(), name) !=
            _dxilImplicitResourceNames.end();
    if (!implicit)
      if (resource.GetLowerBound() != UINT_MAX)
        return true;
    if (implicit)
      AddDiagnostic(2111, "DXIL resource '" + name +
                              "' is missing an explicit register() binding");
    else
      AddDiagnostic(2116, "DXIL live resource '" + name +
                              "' has no assigned register");
    return false;
  };
  const auto addResource =
      [&](const hlsl::DxilResourceBase &resource) -> MetadataBindingFact * {
    return AddDxilResourceFact(
        resource, FindDeclaration(resource.GetGlobalName()), _metadata, _stage,
        _diagnostics);
  };
  for (const auto &resource : dxil.GetCBuffers()) {
    if (!requireExplicitBinding(*resource))
      continue;
    MetadataBindingFact *fact = addResource(*resource);
    const llvm::Type *resourceType = resource->GetHLSLType();
    if (resourceType != nullptr && resourceType->isPointerTy())
      resourceType = resourceType->getPointerElementType();
    while (const auto *array =
               llvm::dyn_cast_or_null<llvm::ArrayType>(resourceType))
      resourceType = array->getElementType();
    if (const auto *wrapper =
            llvm::dyn_cast_or_null<llvm::StructType>(resourceType)) {
      if (wrapper->getNumElements() == 1 &&
          llvm::isa<llvm::StructType>(wrapper->getElementType(0)))
        resourceType = wrapper->getElementType(0);
    }
    const auto *type =
        llvm::dyn_cast_or_null<llvm::StructType>(resourceType);
    const uint32_t typeIndex =
        type == nullptr
            ? kMetadataNoType
            : CollectDxilTypeFacts(dxil.GetTypeSystem(), type, _metadata);
    if (fact != nullptr)
      fact->TypeIndex = typeIndex;
    if (fact != nullptr && typeIndex == kMetadataNoType)
      _diagnostics.push_back(
          {2107, "DXIL constant buffer has no valid root type metadata"});
  }
  for (const auto &resource : dxil.GetSamplers())
    if (requireExplicitBinding(*resource))
      addResource(*resource);
  for (const auto &resource : dxil.GetSRVs())
    if (requireExplicitBinding(*resource))
      addResource(*resource);
  for (const auto &resource : dxil.GetUAVs())
    if (requireExplicitBinding(*resource))
      addResource(*resource);

  if (_stage == ShaderStage::Vertex) {
    const hlsl::DxilSignature &input = dxil.GetInputSignature();
    _metadata.VertexInputs.clear();
    uint32_t fallbackLocation = 0;
    for (const auto &element : input.GetElements()) {
      const string semantic = element->GetSemanticName().str();
      if (semantic.empty() || (semantic.size() >= 3 && semantic.compare(0, 3, "SV_") == 0))
        continue;
      const int startRow = element->GetStartRow();
      const uint32_t location = startRow < 0
                                    ? fallbackLocation
                                    : static_cast<uint32_t>(startRow);
      fallbackLocation = location + std::max(1u, element->GetRows());
      uint32_t componentType = 0;
      const hlsl::CompType component = element->GetCompType();
      if (component.IsFloatTy())
        componentType = 1;
      else if (component.IsSIntTy())
        componentType = 2;
      else if (component.IsUIntTy())
        componentType = 3;
      if (componentType == 0)
        continue;
      const uint32_t rows = std::max(1u, element->GetRows());
      const uint32_t columns = std::max(1u, element->GetCols());
      for (uint32_t row = 0; row < rows; ++row)
        _metadata.VertexInputs.push_back(
            {semantic, element->GetSemanticStartIndex() + row, location + row,
             componentType, columns, 0});
    }
  }

  // The frontend already parsed the policy off the AST. Codegen serializes the
  // same attribute independently, so requiring the two blobs to match is what
  // makes the AST-side policy trustworthy for the SPIR-V lane as well.
  const vector<uint8_t> &serializedRoot = dxil.GetSerializedRootSignature();
  if (!serializedRoot.empty()) {
    if (_hasRootSignature) {
      if (serializedRoot != _rootSignatureBytes)
        AddDiagnostic(2106, "DXIL RootSignature does not match the "
                            "[RootSignature] the frontend parsed");
    } else if (!_hasRootSignatureSource) {
      AddDiagnostic(2106, "DXIL emitted a RootSignature the frontend never saw");
    }
  }
  ApplyRootSignaturePolicy();
}

#ifdef ENABLE_SPIRV_CODEGEN
const clang::spirv::SpirvType *UnwrapSpirvPointer(
    const clang::spirv::SpirvType *type) {
  // Instructions in a module that failed codegen may never have been lowered,
  // so the SPIR-V type can legitimately be absent here.
  if (const auto *pointer =
          llvm::dyn_cast_or_null<clang::spirv::SpirvPointerType>(type))
    return pointer->getPointeeType();
  return type;
}

uint32_t SpirvTypeSize(const clang::spirv::SpirvType *type) {
  type = UnwrapSpirvPointer(type);
  if (type == nullptr)
    return 0;
  if (const auto *numerical = llvm::dyn_cast<clang::spirv::NumericalType>(type))
    return std::max(1u, numerical->getBitwidth() / 8u);
  if (llvm::isa<clang::spirv::BoolType>(type))
    return 4;
  if (const auto *vector = llvm::dyn_cast<clang::spirv::VectorType>(type))
    return SpirvTypeSize(vector->getElementType()) * vector->getElementCount();
  if (const auto *matrix = llvm::dyn_cast<clang::spirv::MatrixType>(type))
    return SpirvTypeSize(matrix->getVecType()) * matrix->getVecCount();
  if (const auto *array = llvm::dyn_cast<clang::spirv::ArrayType>(type))
    return SpirvTypeSize(array->getElementType()) * array->getElementCount();
  if (const auto *runtimeArray =
          llvm::dyn_cast<clang::spirv::RuntimeArrayType>(type))
    return runtimeArray->getStride().hasValue()
               ? runtimeArray->getStride().getValue()
               : SpirvTypeSize(runtimeArray->getElementType());
  if (const auto *structure = llvm::dyn_cast<clang::spirv::StructType>(type)) {
    uint32_t size = 0;
    for (const auto &field : structure->getFields()) {
      const uint32_t offset = field.offset.hasValue() ? field.offset.getValue() : size;
      const uint32_t fieldSize = field.sizeInBytes.hasValue()
                                     ? field.sizeInBytes.getValue()
                                     : SpirvTypeSize(field.type);
      size = std::max(size, offset + fieldSize);
    }
    return size;
  }
  return 0;
}

string SpirvStructName(const clang::spirv::StructType *structure) {
  string name = structure->getStructName().str();
  for (const string_view prefix : {string_view{"type.ConstantBuffer."},
                                   string_view{"type."}}) {
    if (name.compare(0, prefix.size(), prefix) == 0) {
      name.erase(0, prefix.size());
      break;
    }
  }
  return name;
}

uint32_t CollectSpirvStructFacts(
    const clang::spirv::StructType *structure, MetadataFacts &facts) {
  if (structure == nullptr)
    return kMetadataNoType;
  const auto existing = std::find_if(
      facts.Types.begin(), facts.Types.end(),
      [&](const MetadataTypeFact &fact) noexcept {
        return fact.ParentIndex == kMetadataNoParent && fact.Kind == 4u &&
               fact.Name == SpirvStructName(structure);
      });
  if (existing != facts.Types.end())
    return static_cast<uint32_t>(existing - facts.Types.begin());
  for (const auto &field : structure->getFields()) {
    const clang::spirv::SpirvType *fieldType =
        UnwrapSpirvPointer(field.type);
    if (const auto *array = llvm::dyn_cast<clang::spirv::ArrayType>(fieldType))
      fieldType = array->getElementType();
    else if (const auto *runtimeArray =
                 llvm::dyn_cast<clang::spirv::RuntimeArrayType>(fieldType))
      fieldType = runtimeArray->getElementType();
    CollectSpirvStructFacts(
        llvm::dyn_cast<clang::spirv::StructType>(fieldType), facts);
  }
  const uint32_t parentIndex = static_cast<uint32_t>(facts.Types.size());
  const uint32_t size = SpirvTypeSize(structure);
  facts.Types.push_back({SpirvStructName(structure), kMetadataNoParent,
                         4u, 1u, 0u, size, size, 0u, kMetadataNoType, {}});
  const auto fields = structure->getFields();
  for (size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex) {
    const auto &field = fields[fieldIndex];
    const clang::spirv::SpirvType *fieldType = field.type;
    uint32_t kind = 1;
    uint32_t count = 1;
    uint32_t stride = SpirvTypeSize(fieldType);
    string underlying;
    if (field.matrixStride.hasValue() &&
        llvm::isa<clang::spirv::MatrixType>(fieldType)) {
      kind = 3;
      const auto *matrix = llvm::cast<clang::spirv::MatrixType>(fieldType);
      stride = SpirvTypeSize(matrix);
    } else if (const auto *array =
                   llvm::dyn_cast<clang::spirv::ArrayType>(fieldType)) {
      kind = 5;
      count = array->getElementCount();
      stride = array->getStride().hasValue()
                   ? array->getStride().getValue()
                   : SpirvTypeSize(array->getElementType());
      if (const auto *elementStruct = llvm::dyn_cast<clang::spirv::StructType>(
              array->getElementType()))
        underlying = SpirvStructName(elementStruct);
    } else if (const auto *fieldStruct =
                   llvm::dyn_cast<clang::spirv::StructType>(fieldType)) {
      kind = 4;
      underlying = SpirvStructName(fieldStruct);
    } else if (llvm::isa<clang::spirv::VectorType>(fieldType)) {
      kind = 2;
    }
    const uint32_t offset = field.offset.hasValue() ? field.offset.getValue() : 0;
    const uint32_t sizeInBytes = field.sizeInBytes.hasValue()
                                     ? field.sizeInBytes.getValue()
                                     : stride * count;
    facts.Types.push_back({field.name, parentIndex, kind, count, offset,
                           sizeInBytes, stride, 0u, kMetadataNoType,
                           std::move(underlying)});
  }
  if (!ResolveMetadataTypeIndices(facts.Types))
    return kMetadataNoType;
  return parentIndex;
}

// The final stage-IO location is only decided during module finalization, so
// the wire fact has to be read back from the emitted decorations rather than
// recomputed from the AST declaration order.
void RadRayContractCollector::CollectSpirvVertexInputs(
    clang::spirv::SpirvEmitter &emitter) {
  clang::spirv::SpirvModule *module = emitter.getSpirvBuilder().getModule();
  if (module == nullptr)
    return;

  vector<std::pair<clang::spirv::SpirvInstruction *, uint32_t>> locations;
  for (clang::spirv::SpirvDecoration *decoration :
       module->getDecorationsForRadRay()) {
    if (decoration == nullptr ||
        decoration->getDecoration() != spv::Decoration::Location ||
        decoration->getParams().empty())
      continue;
    locations.emplace_back(decoration->getTarget(),
                           decoration->getParams().front());
  }

  _metadata.VertexInputs.clear();
  for (const clang::spirv::StageVar &stageVar :
       emitter.getDeclResultIdMapper().getStageVarsForRadRay()) {
    if (stageVar.isSpirvBuitin() ||
        stageVar.getStorageClass() != spv::StorageClass::Input)
      continue;
    const hlsl::SigPoint *sigPoint = stageVar.getSigPoint();
    if (sigPoint == nullptr ||
        sigPoint->GetKind() != hlsl::DXIL::SigPointKind::VSIn)
      continue;
    clang::spirv::SpirvVariable *instruction = stageVar.getSpirvInstr();
    if (instruction == nullptr)
      continue;
    const clang::spirv::SemanticInfo &semanticInfo = stageVar.getSemanticInfo();
    string semantic = semanticInfo.name.str();
    if (semantic.empty() ||
        (semantic.size() >= 3 && semantic.compare(0, 3, "SV_") == 0))
      continue;

    const auto location = std::find_if(
        locations.begin(), locations.end(),
        [&](const std::pair<clang::spirv::SpirvInstruction *, uint32_t> &value)
            noexcept { return value.first == instruction; });
    if (location == locations.end()) {
      AddDiagnostic(2114, "SPIR-V vertex input '" + semantic +
                              "' has no Location decoration");
      continue;
    }

    const clang::spirv::SpirvType *valueType =
        UnwrapSpirvPointer(instruction->getResultType());
    uint32_t componentCount = 1;
    uint32_t semanticRows = 1;
    uint32_t locationStride = 1;
    if (const auto *matrix =
            llvm::dyn_cast_or_null<clang::spirv::MatrixType>(valueType)) {
      unsigned int rowCount = 0;
      unsigned int columnCount = 0;
      hlsl::GetHLSLMatRowColCount(stageVar.getAstType(), rowCount,
                                  columnCount);
      if (rowCount == 0 || columnCount == 0) {
        AddDiagnostic(2115, "SPIR-V vertex input '" + semantic +
                                "' has invalid matrix dimensions");
        continue;
      }
      semanticRows = rowCount;
      componentCount = columnCount;
      const uint32_t locationCount = stageVar.getLocationCount();
      locationStride = std::max(1u, locationCount / semanticRows);
      valueType = matrix->getElementType();
    } else if (const auto *vector =
                   llvm::dyn_cast_or_null<clang::spirv::VectorType>(valueType)) {
      componentCount = vector->getElementCount();
      valueType = vector->getElementType();
    }
    uint32_t componentType = 0;
    if (llvm::dyn_cast_or_null<clang::spirv::FloatType>(valueType) != nullptr)
      componentType = 1;
    else if (const auto *integer =
                 llvm::dyn_cast_or_null<clang::spirv::IntegerType>(valueType))
      componentType = integer->isSignedInt() ? 2u : 3u;
    if (componentType == 0) {
      AddDiagnostic(2115, "SPIR-V vertex input '" + semantic +
                              "' uses a type that cannot be described on the "
                              "wire");
      continue;
    }

    for (uint32_t row = 0; row < semanticRows; ++row)
      _metadata.VertexInputs.push_back(
          {semantic, semanticInfo.index + row,
           location->second + row * locationStride, componentType,
           componentCount, 0});
  }
}

// Coarse family of a logical kind. A structured and a raw buffer lower to the
// same SPIR-V storage buffer, so the declaration decides between them and the
// lowered shape only has to agree on the family.
uint32_t MetadataKindFamily(MetadataBindingKind kind) noexcept {
  switch (kind) {
  case MetadataBindingKind::CBuffer:
    return 1;
  case MetadataBindingKind::TypedBuffer:
    return 2;
  case MetadataBindingKind::RWTypedBuffer:
    return 3;
  case MetadataBindingKind::StructuredBuffer:
  case MetadataBindingKind::RawBuffer:
    return 4;
  case MetadataBindingKind::RWStructuredBuffer:
  case MetadataBindingKind::RWRawBuffer:
    return 5;
  case MetadataBindingKind::Texture:
    return 6;
  case MetadataBindingKind::RWTexture:
    return 7;
  case MetadataBindingKind::Sampler:
    return 8;
  case MetadataBindingKind::Unknown:
    break;
  }
  return 0;
}

void RadRayContractCollector::CollectSpirvAction(
    clang::EmitSpirvAction &action, string_view entryPointName) {
  clang::spirv::SpirvEmitter *emitter = action.getSpirvEmitter();
  if (emitter == nullptr || emitter->getSpirvBuilder().getModule() == nullptr)
    return;
  // The observer hook fires even when SPIR-V codegen has already failed, and a
  // half-built module has instructions whose types were never lowered. DXC's
  // own error is authoritative here, so collecting from the wreckage would only
  // trade a real diagnostic for a crash.
  if (emitter->getDiagnosticsEngine().hasErrorOccurred())
    return;
  clang::spirv::SpirvModule *module = emitter->getSpirvBuilder().getModule();

  vector<clang::spirv::SpirvVariable *> activeResources;
  if (!entryPointName.empty()) {
    RadRaySpirvEntryPointFinder entryFinder(entryPointName);
    entryFinder.TraverseDecl(emitter->getASTContext().getTranslationUnitDecl());
    // Without the entry point the reachability filter degrades to "every
    // resource in the translation unit", which silently widens the contract.
    if (entryFinder.EntryPoint() == nullptr) {
      AddDiagnostic(2112, "SPIR-V entry point '" + string(entryPointName) +
                              "' was not found in the compiled module");
      return;
    }
    RadRaySpirvResourceUseVisitor resourceVisitor(
        emitter->getDeclResultIdMapper(), activeResources);
    resourceVisitor.TraverseEntry(entryFinder.EntryPoint());
  }
  for (clang::spirv::SpirvVariableLike *variableLike : module->getVariables()) {
    auto *variable = llvm::dyn_cast<clang::spirv::SpirvVariable>(variableLike);
    if (variable == nullptr)
      continue;
    const clang::spirv::SpirvType *valueType =
        UnwrapSpirvPointer(variable->getResultType());
    uint32_t count = 1;
    while (const auto *array = llvm::dyn_cast<clang::spirv::ArrayType>(valueType)) {
      count *= array->getElementCount();
      valueType = array->getElementType();
    }
    const bool pushConstant =
        variable->getStorageClass() == spv::StorageClass::PushConstant;
    if (!entryPointName.empty() && !pushConstant &&
        std::find(activeResources.begin(), activeResources.end(), variable) ==
            activeResources.end())
      continue;
    const uint32_t stageMask = MetadataStageBitFor(_stage);
    const string variableName = variable->getDebugName().str();
    const MetadataDeclarationFact *declaration = FindDeclaration(variableName);
    if (pushConstant) {
      const uint32_t size = SpirvTypeSize(valueType);
      const auto *structure =
          llvm::dyn_cast<clang::spirv::StructType>(valueType);
      const uint32_t typeIndex =
          CollectSpirvStructFacts(structure, _metadata);
      if (declaration == nullptr || !declaration->HasRegister) {
        AddDiagnostic(2120, "push constant block '" + variableName +
                                "' needs a register() annotation to be placed "
                                "on both targets");
      } else if (!_sawPushConstant) {
        MetadataRootConstantFact fact;
        fact.Name = declaration->Name;
        fact.RegisterSpace = declaration->RegisterSpace;
        fact.Register = declaration->RegisterNumber;
        fact.Offset = 0u;
        fact.Size = size;
        fact.StageMask = stageMask;
        fact.Flags = 1u;
        fact.TypeIndex = typeIndex;
        _metadata.RootConstants.push_back(std::move(fact));
        _sawPushConstant = true;
        if (typeIndex == kMetadataNoType)
          _diagnostics.push_back(
              {2107, "SPIR-V push constant has no valid root type metadata"});
      } else {
        _diagnostics.push_back(
            {2103, "SPIR-V source contains multiple push constant blocks"});
      }
      continue;
    }

    // The logical kind comes from the declaration; the SPIR-V shape is only the
    // cross-check that the lowering landed in the same family.
    MetadataBindingKind lowered = MetadataBindingKind::Unknown;
    bool isBinding = variable->hasBinding();
    if (clang::spirv::SpirvType::isSampler(valueType))
      lowered = MetadataBindingKind::Sampler;
    else if (clang::spirv::SpirvType::isRWTexture(valueType))
      lowered = MetadataBindingKind::RWTexture;
    else if (clang::spirv::SpirvType::isTexture(valueType))
      lowered = MetadataBindingKind::Texture;
    else if (clang::spirv::SpirvType::isRWBuffer(valueType))
      lowered = MetadataBindingKind::RWTypedBuffer;
    else if (clang::spirv::SpirvType::isBuffer(valueType))
      lowered = MetadataBindingKind::TypedBuffer;
    else if (llvm::isa<clang::spirv::StructType>(valueType)) {
      const auto *structure = llvm::cast<clang::spirv::StructType>(valueType);
      if (structure->getInterfaceType() ==
              clang::spirv::StructInterfaceType::StorageBuffer &&
          (variable->getStorageClass() == spv::StorageClass::Uniform ||
           variable->getStorageClass() == spv::StorageClass::StorageBuffer))
        lowered = structure->isReadOnly()
                      ? MetadataBindingKind::StructuredBuffer
                      : MetadataBindingKind::RWStructuredBuffer;
      else if (structure->getInterfaceType() ==
                   clang::spirv::StructInterfaceType::UniformBuffer &&
               variable->getStorageClass() == spv::StorageClass::Uniform)
        lowered = MetadataBindingKind::CBuffer;
      else
        isBinding = false;
    }
    else
      isBinding = false;
    if (!isBinding)
      continue;
    const MetadataBindingKind bindingKind =
        declaration != nullptr
            ? static_cast<MetadataBindingKind>(declaration->Kind)
            : lowered;
    if (bindingKind == MetadataBindingKind::Unknown) {
      AddDiagnostic(2118, "resource '" + variableName +
                              "' has a declaration type outside the RadRay "
                              "shader contract");
      continue;
    }
    if (lowered != MetadataBindingKind::Unknown &&
        MetadataKindFamily(lowered) != MetadataKindFamily(bindingKind)) {
      AddDiagnostic(2119, "resource '" + variableName +
                              "' logical kind disagrees with its SPIR-V "
                              "lowering");
      continue;
    }
    // DXC assigns a set/binding pair either way, so the authored attribute on
    // the ResourceVar is the only place an implicit assignment is still
    // distinguishable from a declared one. register() alone is not accepted: DXC
    // would then derive the Vulkan slot from the DXIL register via the -fvk-*-shift
    // policy, making the wire binding a function of toolchain defaults rather
    // than of the source.
    const auto &resourceVars =
        emitter->getDeclResultIdMapper().getResourceVarsForRadRay();
    const auto *resourceVar = std::find_if(
        resourceVars.begin(), resourceVars.end(),
        [&](const clang::spirv::ResourceVar &value) noexcept {
          return value.getSpirvInstr() == variable;
        });
    if (resourceVar == resourceVars.end() ||
        resourceVar->getBinding() == nullptr) {
      AddDiagnostic(2113, "SPIR-V resource '" + variableName +
                              "' is missing an explicit [[vk::binding]] binding");
      continue;
    }
    const int32_t group = variable->getDescriptorSetNo();
    const int32_t binding = variable->getBindingNo();
    if (group < 0 || binding < 0)
      continue;
    // Group/Binding are the Vulkan set and binding here. The policy is written
    // in D3D register terms, so its lookup goes through the declaration table
    // rather than through these numbers.
    const uint32_t declarationCount =
        declaration != nullptr && declaration->Count != 0 ? declaration->Count
                                                        : count;
    MetadataBindingFact *fact =
        AddMetadataBinding(_metadata, variableName,
                           static_cast<uint32_t>(group),
                           static_cast<uint32_t>(binding), bindingKind,
                           declarationCount, _stage);
    if (bindingKind == MetadataBindingKind::CBuffer) {
      const auto *structure =
          llvm::dyn_cast<clang::spirv::StructType>(valueType);
      const uint32_t typeIndex =
          CollectSpirvStructFacts(structure, _metadata);
      if (fact != nullptr)
        fact->TypeIndex = typeIndex;
      if (fact != nullptr && typeIndex == kMetadataNoType)
        _diagnostics.push_back(
            {2107, "SPIR-V constant buffer has no valid root type metadata"});
    }
  }

  ApplyRootSignaturePolicy();

  if (_stage == ShaderStage::Vertex)
    CollectSpirvVertexInputs(*emitter);
}
#endif

Hash128 MakeContractHash(const ContractData &contract) {
  uint64_t first = 1469598103934665603ull;
  uint64_t second = 1099511628211ull;
  auto addText = [&](string_view value) {
    for (const char character : value)
      HashByte(first, second, static_cast<uint8_t>(character));
    HashByte(first, second, 0xff);
  };
  addText(contract.Kind == ShaderKind::Graphics ? "graphics" : "compute");
  for (const KeywordGroup &group : contract.KeywordGroups) {
    addText(group.Name);
    for (const string &value : group.Values)
      addText(value);
  }
  for (const EntryPoint &entry : contract.EntryPoints) {
    addText(entry.Name);
    HashByte(first, second, static_cast<uint8_t>(entry.Stage));
  }
  Hash128 hash{};
  for (uint32_t index = 0; index < 8; ++index) {
    hash.Bytes[index] = static_cast<uint8_t>(first >> (index * 8));
    hash.Bytes[index + 8] = static_cast<uint8_t>(second >> (index * 8));
  }
  return hash;
}

vector<uint8_t> EncodeContract(const ContractData &contract) {
  vector<uint8_t> bytes;
  AppendU32(bytes, radray::shader::kRadRayDxcContractWireMagic);
  AppendU16(bytes, radray::shader::kRadRayDxcContractWireSchemaVersion);
  AppendByte(bytes, static_cast<uint8_t>(contract.Kind));
  AppendByte(bytes, 0);
  AppendU16(bytes, 0);
  AppendU32(bytes, static_cast<uint32_t>(contract.KeywordGroups.size()));
  for (const KeywordGroup &group : contract.KeywordGroups) {
    AppendString(bytes, group.Name);
    AppendU32(bytes, static_cast<uint32_t>(group.Values.size()));
    for (const string &value : group.Values)
      AppendString(bytes, value);
  }
  AppendU32(bytes, static_cast<uint32_t>(contract.EntryPoints.size()));
  for (const EntryPoint &entry : contract.EntryPoints) {
    AppendString(bytes, entry.Name);
    AppendByte(bytes, static_cast<uint8_t>(entry.Stage));
    AppendByte(bytes, 0);
    AppendU16(bytes, 0);
  }
  bytes.insert(bytes.end(), contract.Hash.Bytes,
               contract.Hash.Bytes + sizeof(contract.Hash.Bytes));
  return bytes;
}

// Discovery parses the whole translation unit with one library profile, so the
// requested shader model has to reach it: language features gated on SM 6.2+
// (for example ResourceDescriptorHeap) are otherwise rejected at discovery even
// though the concrete stage compile would accept them. There is no lib_6_0, so
// an SM 6.0 request maps to the lowest existing library profile.
wstring LibraryProfileForShaderModel(uint32_t shaderModel) {
  const uint32_t minor = std::max(1u, shaderModel - 60u);
  wstring profile{L"lib_6_"};
  profile.push_back(static_cast<wchar_t>(L'0' + minor));
  return profile;
}

wstring ProfileForStage(ShaderStage stage, uint32_t shaderModel) {
  const wchar_t *prefix = L"";
  switch (stage) {
  case ShaderStage::Vertex:
    prefix = L"vs_6_";
    break;
  case ShaderStage::Pixel:
    prefix = L"ps_6_";
    break;
  case ShaderStage::Compute:
    prefix = L"cs_6_";
    break;
  }
  wstring profile{prefix};
  profile.push_back(static_cast<wchar_t>(L'0' + (shaderModel - 60u)));
  return profile;
}

bool ToWide(string_view value, wstring &result) {
  if (value.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;
  return Unicode::UTF8ToWideString(value.data(), value.size(), &result);
}

bool AppendIncludePathArguments(
    RadRayDxcIncludePathListView includePaths,
    vector<wstring> &arguments,
    string &error) {
  if (includePaths.Count > kMaxCollectionCount ||
      (includePaths.Count != 0 && includePaths.Paths == nullptr)) {
    error = "include path list is invalid";
    return false;
  }
  arguments.reserve(arguments.size() + static_cast<size_t>(includePaths.Count) * 2);
  for (uint32_t index = 0; index < includePaths.Count; ++index) {
    const RadRayDxcBlobView path = includePaths.Paths[index];
    if (path.Data == nullptr || path.Size == 0 ||
        path.Size >= static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        std::memchr(path.Data, 0, path.Size) != nullptr) {
      error = "include path contains an invalid UTF-8 blob";
      return false;
    }
    const auto *data = reinterpret_cast<const char *>(path.Data);
    wstring widePath;
    if (!ToWide(string_view{data, path.Size}, widePath)) {
      error = "include path is not valid UTF-8";
      return false;
    }
    arguments.emplace_back(L"-I");
    arguments.push_back(std::move(widePath));
  }
  return true;
}

bool CheckDxcResult(
    const CComPtr<IDxcResult> &result,
    uint32_t errorCode,
    string_view fallback,
    vector<Diagnostic> &diagnostics) {
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
    CComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors != nullptr && errors->GetStringLength() != 0) {
      const char *text = static_cast<const char *>(errors->GetBufferPointer());
      diagnostics.push_back({errorCode, string{text, text + errors->GetStringLength()}});
    } else {
      diagnostics.push_back({errorCode, string{fallback}});
    }
    return false;
  }
  return true;
}

bool CollectContractWithFrontend(
    const DiscoveryRequest &request, RadRayDxcTarget target,
    RadRayDxcIncludePathListView includePaths, ContractData &contract,
    vector<Diagnostic> &diagnostics) {
  CComPtr<IDxcUtils> utils;
  if (FAILED(CreateDxcUtils(IID_PPV_ARGS(&utils)))) {
    diagnostics.push_back({2002, "fork DXC utils instance creation failed"});
    return false;
  }

  vector<wstring> argumentStorage;
  argumentStorage.emplace_back(L"-HV");
  wstring hlslVersion;
  if (!ToWide(std::to_string(request.HlslVersion), hlslVersion)) {
    diagnostics.push_back({2000, "HLSL version argument is not valid UTF-8"});
    return false;
  }
  argumentStorage.push_back(std::move(hlslVersion));
  argumentStorage.emplace_back(L"-T");
  argumentStorage.push_back(LibraryProfileForShaderModel(request.ShaderModel));
  argumentStorage.emplace_back(L"-Vd");
  if (request.AllResourcesBound != 0)
    argumentStorage.emplace_back(L"-all_resources_bound");
  if (request.WarningPolicy != 0)
    argumentStorage.emplace_back(L"-WX");
  if (target == RadRayDxcTarget::DXIL) {
    // Discovery parses the same source as the stage compiles, so it has to
    // tolerate the [[vk::*]] authorization attributes this lane ignores.
    argumentStorage.emplace_back(L"-Wno-ignored-attributes");
  }
  if (target == RadRayDxcTarget::SPIRV) {
    argumentStorage.emplace_back(L"-spirv");
    argumentStorage.emplace_back(L"-fspv-target-env=vulkan1.2");
  }
  for (const NameValue &define : request.Defines) {
    const string argument = "-D" + define.Name + "=" + define.Value;
    wstring wideArgument;
    if (!ToWide(argument, wideArgument)) {
      diagnostics.push_back({2000, "define argument is not valid UTF-8"});
      return false;
    }
    argumentStorage.push_back(std::move(wideArgument));
  }
  string pathError;
  if (!AppendIncludePathArguments(includePaths, argumentStorage, pathError)) {
    diagnostics.push_back({2000, std::move(pathError)});
    return false;
  }

  vector<LPCWSTR> arguments;
  arguments.reserve(argumentStorage.size());
  for (const wstring &argument : argumentStorage)
    arguments.push_back(argument.c_str());

  wstring sourceNameWide;
  if (!ToWide(request.SourceName, sourceNameWide)) {
    diagnostics.push_back({2000, "source name is not valid UTF-8"});
    return false;
  }
  CComPtr<IDxcCompilerArgs> compilerArgs;
  if (FAILED(utils->BuildArguments(
          sourceNameWide.c_str(), nullptr, nullptr, arguments.data(),
          static_cast<uint32_t>(arguments.size()), nullptr, 0,
          &compilerArgs))) {
    diagnostics.push_back({2003, "fork DXC argument construction failed"});
    return false;
  }

  CComPtr<IDxcIncludeHandler> includeHandler;
  if (FAILED(utils->CreateDefaultIncludeHandler(&includeHandler))) {
    diagnostics.push_back({2003, "fork DXC default include handler creation failed"});
    return false;
  }

  DxcBuffer sourceBuffer{};
  sourceBuffer.Ptr = request.RootSource.data();
  sourceBuffer.Size = request.RootSource.size();
  sourceBuffer.Encoding = DXC_CP_UTF8;
  RadRayFrontendObserver observer(request.SourceName, target,
                                  ShaderStage::Vertex, true);
  CComPtr<IDxcResult> result;
  if (FAILED(CompileDxcWithRadRayObserver(
          &sourceBuffer, compilerArgs->GetArguments(), compilerArgs->GetCount(),
          includeHandler.p, &observer, IID_PPV_ARGS(&result)))) {
    diagnostics.push_back({2003, "fork DXC frontend contract call failed"});
    return false;
  }
  if (!CheckDxcResult(result, 2004,
                      "fork DXC rejected the frontend contract request",
                      diagnostics))
    return false;
  return observer.Finalize(contract, diagnostics);
}

struct StageOutput {
  vector<uint8_t> Bytecode;
  vector<uint8_t> RootSignature;
  MetadataFacts Facts;
};

bool CompileStage(const CompileRequest &request, RadRayDxcTarget target,
                  const EntryPoint &entry,
                  RadRayDxcIncludePathListView includePaths,
                  StageOutput &output, vector<Diagnostic> &diagnostics) {
  CComPtr<IDxcUtils> utils;
  if (FAILED(CreateDxcUtils(IID_PPV_ARGS(&utils)))) {
    diagnostics.push_back({2002, "fork DXC utils instance creation failed"});
    return false;
  }
  vector<wstring> argumentStorage;
  argumentStorage.emplace_back(L"-HV");
  wstring hlslVersion;
  if (!ToWide(std::to_string(request.HlslVersion), hlslVersion)) {
    diagnostics.push_back({2000, "HLSL version argument is not valid UTF-8"});
    return false;
  }
  argumentStorage.push_back(std::move(hlslVersion));
  argumentStorage.emplace_back(request.Optimize != 0 ? L"-O3" : L"-Od");
  if (request.DebugInfo != 0)
    argumentStorage.emplace_back(L"-Zi");
  if (request.AllResourcesBound != 0)
    argumentStorage.emplace_back(L"-all_resources_bound");
  if (request.WarningPolicy != 0)
    argumentStorage.emplace_back(L"-WX");
  if (target == RadRayDxcTarget::DXIL) {
    argumentStorage.emplace_back(L"-Qstrip_rootsignature");
    // A push block is authorized with [[vk::push_constant]], which this lane
    // ignores by design. Without this the warning becomes an error under -WX and
    // the same source could not compile for both targets.
    argumentStorage.emplace_back(L"-Wno-ignored-attributes");
  }
  if (target == RadRayDxcTarget::SPIRV) {
    argumentStorage.emplace_back(L"-spirv");
    argumentStorage.emplace_back(L"-fspv-target-env=vulkan1.2");
  }
  for (const NameValue &define : request.Defines) {
    const string argument = "-D" + define.Name + "=" + define.Value;
    wstring wideArgument;
    if (!ToWide(argument, wideArgument)) {
      diagnostics.push_back({2000, "define argument is not valid UTF-8"});
      return false;
    }
    argumentStorage.push_back(std::move(wideArgument));
  }
  for (const NameValue &assignment : request.Assignments) {
    const string argument = "-D" + assignment.Name + "=" + assignment.Value;
    wstring wideArgument;
    if (!ToWide(argument, wideArgument)) {
      diagnostics.push_back({2000, "keyword assignment argument is not valid UTF-8"});
      return false;
    }
    argumentStorage.push_back(std::move(wideArgument));
  }
  string pathError;
  if (!AppendIncludePathArguments(includePaths, argumentStorage, pathError)) {
    diagnostics.push_back({2000, std::move(pathError)});
    return false;
  }
  vector<LPCWSTR> arguments;
  arguments.reserve(argumentStorage.size());
  for (const wstring &argument : argumentStorage)
    arguments.push_back(argument.c_str());

  wstring sourceName;
  wstring entryName;
  if (!ToWide(request.SourceName, sourceName) || !ToWide(entry.Name, entryName)) {
    diagnostics.push_back({2000, "source or entry name is not valid UTF-8"});
    return false;
  }
  const wstring stageProfile = ProfileForStage(entry.Stage, request.ShaderModel);
  CComPtr<IDxcCompilerArgs> compilerArgs;
  if (FAILED(utils->BuildArguments(
          sourceName.c_str(), entryName.c_str(), stageProfile.c_str(),
          arguments.data(), static_cast<uint32_t>(arguments.size()), nullptr, 0,
          &compilerArgs))) {
    diagnostics.push_back({2003, "fork DXC argument construction failed"});
    return false;
  }
  CComPtr<IDxcIncludeHandler> includeHandler;
  if (FAILED(utils->CreateDefaultIncludeHandler(&includeHandler))) {
    diagnostics.push_back({2003, "fork DXC default include handler creation failed"});
    return false;
  }
  DxcBuffer source{};
  source.Ptr = request.RootSource.data();
  source.Size = request.RootSource.size();
  source.Encoding = DXC_CP_UTF8;
  RadRayFrontendObserver observer(request.SourceName, target, entry.Stage,
                                  false, entry.Name);
  CComPtr<IDxcResult> result;
  if (FAILED(CompileDxcWithRadRayObserver(
          &source, compilerArgs->GetArguments(), compilerArgs->GetCount(),
          includeHandler.p, &observer, IID_PPV_ARGS(&result)))) {
    diagnostics.push_back({2003, "fork DXC Compile call failed"});
    return false;
  }
  if (!CheckDxcResult(result, 2004, "fork DXC rejected the typed compile request", diagnostics)) {
    return false;
  }
  CComPtr<IDxcBlob> object;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) ||
      object == nullptr || object->GetBufferSize() == 0) {
    diagnostics.push_back({2005, "fork DXC returned no object blob"});
    return false;
  }
  const auto *data = static_cast<const uint8_t *>(object->GetBufferPointer());
  output.Bytecode.assign(data, data + object->GetBufferSize());

  output.Facts = observer.Metadata();
  if (!observer.AppendDiagnostics(diagnostics))
    return false;
  if (target == RadRayDxcTarget::DXIL) {
    CComPtr<IDxcBlob> rootSignature;
    if (SUCCEEDED(result->GetOutput(
            DXC_OUT_ROOT_SIGNATURE, IID_PPV_ARGS(&rootSignature), nullptr)) &&
        rootSignature != nullptr && rootSignature->GetBufferSize() != 0) {
      const auto *rootData = static_cast<const uint8_t *>(
          rootSignature->GetBufferPointer());
      output.RootSignature.assign(
          rootData, rootData + rootSignature->GetBufferSize());
    }
    // The frontend parsed the policy off the AST and already compared its
    // serialized form against the module's copy. This is the container-level
    // witness of the same invariant: the emitted container has to wrap exactly
    // the bytes the frontend produced.
    if (output.Facts.HasRootSignature != !output.RootSignature.empty()) {
      diagnostics.push_back({
          2106,
          "DXIL RootSignature observer state does not match DXC output"});
      return false;
    }
    if (output.Facts.HasRootSignature) {
      const hlsl::DxilContainerHeader *container = hlsl::IsDxilContainerLike(
          output.RootSignature.data(), output.RootSignature.size());
      const hlsl::DxilPartHeader *part =
          container != nullptr &&
                  hlsl::IsValidDxilContainer(container,
                                             output.RootSignature.size())
              ? hlsl::GetDxilPartByType(container, hlsl::DFCC_RootSignature)
              : nullptr;
      if (part == nullptr) {
        diagnostics.push_back({
            2106,
            "DXIL RootSignature output is not a container with a policy part"});
        return false;
      }
      const auto *partData =
          reinterpret_cast<const uint8_t *>(hlsl::GetDxilPartData(part));
      if (part->PartSize != output.Facts.SerializedRootSignature.size() ||
          std::memcmp(partData, output.Facts.SerializedRootSignature.data(),
                      part->PartSize) != 0) {
        diagnostics.push_back({
            2106,
            "DXIL RootSignature container does not match the frontend's policy"});
        return false;
      }
      // The container is what the wire carries on this target; the hash keeps
      // identifying the policy, not the container that wraps it.
      output.Facts.SerializedRootSignature = output.RootSignature;
    }
  }
  return true;
}

#pragma pack(push, 1)
struct WireRange {
  uint32_t Offset;
  uint32_t Size;
};

struct WireEnvelope {
  uint32_t Magic;
  uint16_t SchemaVersion;
  uint16_t HeaderSize;
  uint32_t TotalSize;
  uint8_t Target;
  uint8_t StageMask;
  uint16_t Flags;
  WireRange EntryRecords;
  WireRange BindingRecords;
  WireRange TypeRecords;
  WireRange RootConstantRecords;
  WireRange VertexInputRecords;
  // Immutable sampler states in policy order, referenced by
  // WireBindingRecord::SamplerIndex. Vulkan only.
  WireRange SamplerRecords;
  WireRange RootSignature;
  WireRange Bytecode;
  uint64_t ToolchainIdentity;
  uint8_t Contract[16];
  uint8_t BytecodeDigest[16];
  // Digest of the target-independent layout the compiler published. The
  // resolved, target-typed layout is derived from it and digested separately.
  uint8_t BasePipelineLayoutDigest[16];
  uint8_t GpuArtifact[16];
};

struct WireEntryRecord {
  WireRange Name;
  uint8_t Stage;
  uint8_t Flags;
  uint16_t Reserved;
  uint32_t InterfaceOffset;
  uint32_t InterfaceSize;
  uint32_t Reserved2;
};

struct WireBindingRecord {
  WireRange Name;
  uint32_t Group;
  uint32_t Binding;
  uint32_t Type;
  uint32_t Count;
  uint32_t StageMask;
  // Where the policy puts this binding: a descriptor table, a root descriptor
  // bound by address, or a D3D static sampler slot that owns no table entry.
  uint32_t Placement;
  // Index into SamplerRecords, or 0xffffffff when the binding is not an
  // immutable sampler.
  uint32_t SamplerIndex;
  uint32_t Flags;
  // Lane-local owner in TypeRecords. Only CBuffer bindings may carry one.
  uint32_t TypeIndex;
};

struct WireTypeRecord {
  WireRange Name;
  uint32_t ParentIndex;
  uint32_t Kind;
  uint32_t ElementCount;
  uint32_t Offset;
  uint32_t Size;
  uint32_t Stride;
  uint32_t Flags;
  uint32_t TypeIndex;
};

struct WireRootConstantRecord {
  // Declaration name of the push block. The push handle table is keyed on it.
  WireRange Name;
  uint32_t RegisterSpace;
  uint32_t Register;
  uint32_t Offset;
  uint32_t Size;
  uint32_t StageMask;
  uint32_t Flags;
  // Lane-local payload owner in TypeRecords, or 0xffffffff when no live
  // payload tree exists for this policy declaration.
  uint32_t TypeIndex;
};

// A static sampler state in Vulkan terms. Every field holds the official Vulkan
// enumerant value, so the backend consumes it without a second mapping table.
struct WireSamplerRecord {
  uint32_t MagFilter;
  uint32_t MinFilter;
  uint32_t MipmapMode;
  uint32_t AddressModeU;
  uint32_t AddressModeV;
  uint32_t AddressModeW;
  float MipLodBias;
  uint32_t AnisotropyEnable;
  float MaxAnisotropy;
  uint32_t CompareEnable;
  uint32_t CompareOp;
  float MinLod;
  float MaxLod;
  uint32_t BorderColor;
  uint32_t ReductionMode;
  uint32_t Flags;
};

struct WireVertexInputRecord {
  WireRange Semantic;
  uint32_t SemanticIndex;
  uint32_t Location;
  uint32_t ComponentType;
  uint32_t ComponentCount;
  uint32_t Flags;
};
#pragma pack(pop)

static_assert(sizeof(WireEnvelope) == 152);
static_assert(sizeof(WireEntryRecord) == 24);
static_assert(sizeof(WireBindingRecord) == 44);
static_assert(offsetof(WireBindingRecord, TypeIndex) == 40);
static_assert(sizeof(WireTypeRecord) == 40);
static_assert(sizeof(WireRootConstantRecord) == 36);
static_assert(offsetof(WireRootConstantRecord, TypeIndex) == 32);
static_assert(sizeof(WireSamplerRecord) == 64);
static_assert(sizeof(WireVertexInputRecord) == 28);

// Stages only report the struct types reachable from their own live resources
// (DXIL runs RemoveUnusedResources, SPIR-V filters by entry-point use), so the
// per-stage Types arrays are merged as a union keyed by root struct name.
// 2107 now means two stages saw the *same* struct with a different layout.
bool MergeTypeFacts(vector<MetadataTypeFact> &target,
                    const vector<MetadataTypeFact> &source,
                    vector<uint32_t> &rootRemap,
                    vector<Diagnostic> &diagnostics) {
  const auto fail = [&diagnostics]() {
    diagnostics.push_back({2107, "frontend stages disagree on type metadata"});
    return false;
  };
  rootRemap.assign(source.size(), kMetadataNoType);
  for (size_t index = 0; index < source.size();) {
    const MetadataTypeFact &root = source[index];
    if (root.ParentIndex != kMetadataNoParent)
      return fail();
    const size_t blockBegin = index;
    size_t blockEnd = index + 1;
    while (blockEnd < source.size() &&
           source[blockEnd].ParentIndex == static_cast<uint32_t>(blockBegin))
      ++blockEnd;
    const auto found = std::find_if(
        target.begin(), target.end(),
        [&](const MetadataTypeFact &candidate) noexcept {
          return candidate.ParentIndex == kMetadataNoParent &&
                 candidate.Name == root.Name;
        });
    uint32_t targetRoot = kMetadataNoType;
    if (found == target.end()) {
      targetRoot = static_cast<uint32_t>(target.size());
      target.push_back(root);
      target.back().TypeIndex = kMetadataNoType;
      for (size_t field = blockBegin + 1; field < blockEnd; ++field) {
        target.push_back(source[field]);
        target.back().ParentIndex = targetRoot;
        target.back().TypeIndex = kMetadataNoType;
      }
    } else {
      targetRoot = static_cast<uint32_t>(found - target.begin());
      vector<size_t> targetFields;
      for (size_t entry = 0; entry < target.size(); ++entry)
        if (target[entry].ParentIndex == targetRoot)
          targetFields.push_back(entry);
      const size_t fieldCount = blockEnd - blockBegin - 1;
      if (targetFields.size() != fieldCount ||
          found->Kind != root.Kind ||
          found->ElementCount != root.ElementCount ||
          found->Offset != root.Offset || found->Size != root.Size ||
          found->Stride != root.Stride ||
          found->UnderlyingType != root.UnderlyingType)
        return fail();
      for (size_t field = 0; field < fieldCount; ++field) {
        const MetadataTypeFact &left = target[targetFields[field]];
        const MetadataTypeFact &right = source[blockBegin + 1 + field];
        if (left.Name != right.Name || left.Kind != right.Kind ||
            left.ElementCount != right.ElementCount ||
            left.Offset != right.Offset || left.Size != right.Size ||
            left.Stride != right.Stride ||
            left.UnderlyingType != right.UnderlyingType)
          return fail();
      }
    }
    rootRemap[blockBegin] = targetRoot;
    index = blockEnd;
  }
  if (!ResolveMetadataTypeIndices(target))
    return fail();
  return true;
}

bool MergeMetadataFacts(const ContractData &contract,
                        RadRayDxcTarget target,
                        const vector<StageOutput> &stages,
                        MetadataFacts &facts,
                        vector<Diagnostic> &diagnostics) {
  (void)contract;
  facts = {};
  for (const StageOutput &stage : stages) {
    const MetadataFacts &source = stage.Facts;
    // The policy is a translation-unit fact, so a stage that saw one must have
    // seen the same one, down to the serialized bytes and the sampler table.
    if (source.HasRootSignature) {
      // Only D3D ships the serialized carrier; Vulkan describes the same policy
      // through placements and sampler records.
      if (target == RadRayDxcTarget::DXIL &&
          source.SerializedRootSignature.empty()) {
        diagnostics.push_back(
            {2106, "RootSignature metadata has no serialized output"});
        return false;
      }
      if (facts.HasRootSignature) {
        if (!(facts.RootSignatureHash == source.RootSignatureHash) ||
            facts.SerializedRootSignature != source.SerializedRootSignature) {
          diagnostics.push_back(
              {2105,
               "stages disagree on the [RootSignature] policy they declare"});
          return false;
        }
        if (facts.Samplers.size() != source.Samplers.size()) {
          diagnostics.push_back(
              {2105, "stages disagree on the policy's static sampler table"});
          return false;
        }
        for (size_t index = 0; index < facts.Samplers.size(); ++index)
          if (!SameSamplerFact(facts.Samplers[index], source.Samplers[index])) {
            diagnostics.push_back(
                {2105, "stages disagree on the policy's static sampler table"});
            return false;
          }
      }
      facts.HasRootSignature = true;
      facts.RootSignatureHash = source.RootSignatureHash;
      facts.SerializedRootSignature = source.SerializedRootSignature;
      facts.Samplers = source.Samplers;
    }

    vector<uint32_t> rootRemap;
    if (!MergeTypeFacts(facts.Types, source.Types, rootRemap, diagnostics))
      return false;
    const auto remapOwner =
        [&](uint32_t sourceIndex, uint32_t &targetIndex) -> bool {
      if (sourceIndex == kMetadataNoType) {
        targetIndex = kMetadataNoType;
        return true;
      }
      if (sourceIndex >= rootRemap.size() ||
          rootRemap[sourceIndex] == kMetadataNoType) {
        diagnostics.push_back(
            {2107, "declaration owner does not reference root type metadata"});
        return false;
      }
      targetIndex = rootRemap[sourceIndex];
      return true;
    };

    for (const MetadataBindingFact &sourceBinding : source.Bindings) {
      MetadataBindingFact binding = sourceBinding;
      if (!remapOwner(sourceBinding.TypeIndex, binding.TypeIndex))
        return false;
      const auto found = std::find_if(
          facts.Bindings.begin(), facts.Bindings.end(),
          [&](const MetadataBindingFact &value) noexcept {
            return value.Name == binding.Name;
          });
      if (found == facts.Bindings.end()) {
        facts.Bindings.push_back(std::move(binding));
        continue;
      }
      if (found->Group != binding.Group || found->Binding != binding.Binding ||
          found->RegisterClass != binding.RegisterClass ||
          found->Type != binding.Type || found->Count != binding.Count ||
          found->Placement != binding.Placement ||
          found->SamplerIndex != binding.SamplerIndex ||
          found->TypeIndex != binding.TypeIndex ||
          found->HasDeclarationRegister != binding.HasDeclarationRegister ||
          (binding.HasDeclarationRegister &&
           (found->DeclarationRegisterSpace != binding.DeclarationRegisterSpace ||
            found->DeclarationRegisterNumber != binding.DeclarationRegisterNumber))) {
        diagnostics.push_back(
            {2109, "frontend stages disagree on a resource binding"});
        return false;
      }
      found->StageMask |= binding.StageMask;
      found->Flags |= binding.Flags;
    }

    for (const MetadataRootConstantFact &sourceConstant :
         source.RootConstants) {
      MetadataRootConstantFact constant = sourceConstant;
      if (!remapOwner(sourceConstant.TypeIndex, constant.TypeIndex))
        return false;
      const auto found = std::find_if(
          facts.RootConstants.begin(), facts.RootConstants.end(),
          [&](const MetadataRootConstantFact &value) noexcept {
            return value.Name == constant.Name &&
                   value.RegisterSpace == constant.RegisterSpace &&
                   value.Register == constant.Register;
          });
      if (found == facts.RootConstants.end()) {
        facts.RootConstants.push_back(std::move(constant));
        continue;
      }
      if (found->Offset != constant.Offset || found->Size != constant.Size ||
          found->Flags != constant.Flags ||
          (found->TypeIndex != kMetadataNoType &&
           constant.TypeIndex != kMetadataNoType &&
           found->TypeIndex != constant.TypeIndex)) {
        diagnostics.push_back(
            {2124, "frontend stages disagree on a push constant block"});
        return false;
      }
      if (found->TypeIndex == kMetadataNoType)
        found->TypeIndex = constant.TypeIndex;
      found->StageMask |= constant.StageMask;
    }

    for (const MetadataVertexInputFact &input : source.VertexInputs) {
      const auto found = std::find_if(
          facts.VertexInputs.begin(), facts.VertexInputs.end(),
          [&](const MetadataVertexInputFact &value) noexcept {
            return value.Semantic == input.Semantic &&
                   value.SemanticIndex == input.SemanticIndex;
          });
      if (found == facts.VertexInputs.end())
        facts.VertexInputs.push_back(input);
      else if (found->Location != input.Location ||
               found->ComponentType != input.ComponentType ||
               found->ComponentCount != input.ComponentCount) {
        diagnostics.push_back(
            {2108, "frontend stages disagree on vertex input metadata"});
        return false;
      }
    }
  }

  if (target == RadRayDxcTarget::SPIRV && facts.RootConstants.size() > 1) {
    diagnostics.push_back(
        {2103, "SPIR-V source contains multiple push constant blocks"});
    return false;
  }
  for (MetadataRootConstantFact &constant : facts.RootConstants)
    constant.StageMask &= 0x7u;
  return true;
}

bool BuildMetadata(const CompileRequest &request, const ContractData &contract,
                   RadRayDxcTarget target, const vector<StageOutput> &stages,
                   vector<Diagnostic> &diagnostics, vector<uint8_t> &metadata) {
  MetadataFacts facts;
  if (!MergeMetadataFacts(contract, target, stages, facts, diagnostics))
    return false;
  const auto isRootStruct = [&](uint32_t typeIndex) noexcept {
    return typeIndex < facts.Types.size() &&
           facts.Types[typeIndex].ParentIndex == kMetadataNoParent &&
           facts.Types[typeIndex].Kind == 4u;
  };
  for (const MetadataBindingFact &binding : facts.Bindings) {
    const bool cbuffer =
        binding.Type == static_cast<uint32_t>(MetadataBindingKind::CBuffer);
    if ((cbuffer && !isRootStruct(binding.TypeIndex)) ||
        (!cbuffer && binding.TypeIndex != kMetadataNoType)) {
      diagnostics.push_back(
          {2109, "resource binding has an invalid payload owner"});
      return false;
    }
  }
  for (const MetadataRootConstantFact &constant : facts.RootConstants) {
    if (constant.TypeIndex == kMetadataNoType)
      continue;
    if (!isRootStruct(constant.TypeIndex) ||
        facts.Types[constant.TypeIndex].Size > constant.Size) {
      diagnostics.push_back(
          {2124, "root constant has an invalid payload owner"});
      return false;
    }
  }

  const uint32_t entryOffset = sizeof(WireEnvelope);
  const uint32_t entryBytes = static_cast<uint32_t>(
      contract.EntryPoints.size() * sizeof(WireEntryRecord));
  const uint32_t bindingOffset = entryOffset + entryBytes;
  const uint32_t bindingBytes = static_cast<uint32_t>(
      facts.Bindings.size() * sizeof(WireBindingRecord));
  const uint32_t typeOffset = bindingOffset + bindingBytes;
  const uint32_t typeBytes = static_cast<uint32_t>(
      facts.Types.size() * sizeof(WireTypeRecord));
  const uint32_t rootConstantOffset = typeOffset + typeBytes;
  const uint32_t rootConstantBytes = static_cast<uint32_t>(
      facts.RootConstants.size() * sizeof(WireRootConstantRecord));
  const uint32_t vertexInputOffset = rootConstantOffset + rootConstantBytes;
  const uint32_t vertexInputBytes = static_cast<uint32_t>(
      facts.VertexInputs.size() * sizeof(WireVertexInputRecord));
  const uint32_t samplerOffset = vertexInputOffset + vertexInputBytes;
  const uint32_t samplerBytes = static_cast<uint32_t>(
      facts.Samplers.size() * sizeof(WireSamplerRecord));
  const uint32_t nameOffset = samplerOffset + samplerBytes;
  uint32_t nameBytes = 0;
  for (const EntryPoint &entry : contract.EntryPoints)
    nameBytes += static_cast<uint32_t>(entry.Name.size());
  for (const MetadataBindingFact &binding : facts.Bindings)
    nameBytes += static_cast<uint32_t>(binding.Name.size());
  for (const MetadataTypeFact &type : facts.Types)
    nameBytes += static_cast<uint32_t>(type.Name.size());
  for (const MetadataRootConstantFact &constant : facts.RootConstants)
    nameBytes += static_cast<uint32_t>(constant.Name.size());
  for (const MetadataVertexInputFact &input : facts.VertexInputs)
    nameBytes += static_cast<uint32_t>(input.Semantic.size());
  const uint32_t rootSignatureOffset = nameOffset + nameBytes;
  const uint32_t rootSignatureSize = static_cast<uint32_t>(
      facts.SerializedRootSignature.size());
  const uint32_t bytecodeOffset = rootSignatureOffset + rootSignatureSize;
  uint32_t bytecodeSize = 0;
  for (const StageOutput &stage : stages)
    bytecodeSize += static_cast<uint32_t>(stage.Bytecode.size());

  WireEnvelope envelope{};
  envelope.Magic = radray::shader::kRadRayDxcShaderWireMagic;
  envelope.SchemaVersion = radray::shader::kRadRayDxcMetadataSchemaVersion;
  envelope.HeaderSize = sizeof(WireEnvelope);
  envelope.TotalSize = bytecodeOffset + bytecodeSize;
  envelope.Target = target == RadRayDxcTarget::DXIL ? 0 : 1;
  envelope.EntryRecords = {entryOffset, entryBytes};
  envelope.BindingRecords = {bindingOffset, bindingBytes};
  envelope.TypeRecords = {typeOffset, typeBytes};
  envelope.RootConstantRecords = {rootConstantOffset, rootConstantBytes};
  envelope.VertexInputRecords = {vertexInputOffset, vertexInputBytes};
  envelope.SamplerRecords = {samplerOffset, samplerBytes};
  envelope.RootSignature = {rootSignatureOffset, rootSignatureSize};
  envelope.Bytecode = {bytecodeOffset, bytecodeSize};
  envelope.ToolchainIdentity = kMetadataToolchainIdentity;
  std::memcpy(envelope.Contract, contract.Hash.Bytes, sizeof(envelope.Contract));

  vector<WireEntryRecord> entries;
  entries.reserve(contract.EntryPoints.size());
  uint32_t currentNameOffset = nameOffset;
  uint32_t currentInterfaceOffset = 0;
  for (size_t index = 0; index < contract.EntryPoints.size(); ++index) {
    const EntryPoint &entry = contract.EntryPoints[index];
    WireEntryRecord record{};
    record.Name = {currentNameOffset, static_cast<uint32_t>(entry.Name.size())};
    record.Stage = static_cast<uint8_t>(entry.Stage);
    record.InterfaceOffset = currentInterfaceOffset;
    record.InterfaceSize = static_cast<uint32_t>(stages[index].Bytecode.size());
    currentNameOffset += static_cast<uint32_t>(entry.Name.size());
    currentInterfaceOffset += record.InterfaceSize;
    entries.push_back(record);
    envelope.StageMask |= static_cast<uint8_t>(1u << static_cast<uint8_t>(entry.Stage));
  }

  vector<WireBindingRecord> bindings;
  bindings.reserve(facts.Bindings.size());
  for (const MetadataBindingFact &fact : facts.Bindings) {
    WireBindingRecord record{};
    record.Name = {currentNameOffset, static_cast<uint32_t>(fact.Name.size())};
    record.Group = fact.Group;
    record.Binding = fact.Binding;
    record.Type = fact.Type;
    record.Count = fact.Count;
    record.StageMask = fact.StageMask;
    record.Placement = fact.Placement;
    record.SamplerIndex = fact.SamplerIndex;
    record.Flags = fact.Flags;
    record.TypeIndex = fact.TypeIndex;
    bindings.push_back(record);
    currentNameOffset += static_cast<uint32_t>(fact.Name.size());
  }

  vector<WireTypeRecord> types;
  types.reserve(facts.Types.size());
  for (const MetadataTypeFact &fact : facts.Types) {
    WireTypeRecord record{};
    record.Name = {currentNameOffset, static_cast<uint32_t>(fact.Name.size())};
    record.ParentIndex = fact.ParentIndex;
    record.Kind = fact.Kind;
    record.ElementCount = fact.ElementCount;
    record.Offset = fact.Offset;
    record.Size = fact.Size;
    record.Stride = fact.Stride;
    record.Flags = fact.Flags;
    record.TypeIndex = fact.TypeIndex;
    types.push_back(record);
    currentNameOffset += static_cast<uint32_t>(fact.Name.size());
  }

  vector<WireRootConstantRecord> rootConstants;
  rootConstants.reserve(facts.RootConstants.size());
  for (const MetadataRootConstantFact &fact : facts.RootConstants) {
    WireRootConstantRecord record{};
    record.Name = {currentNameOffset, static_cast<uint32_t>(fact.Name.size())};
    record.RegisterSpace = fact.RegisterSpace;
    record.Register = fact.Register;
    record.Offset = fact.Offset;
    record.Size = fact.Size;
    record.StageMask = fact.StageMask;
    record.Flags = fact.Flags;
    record.TypeIndex = fact.TypeIndex;
    rootConstants.push_back(record);
    currentNameOffset += static_cast<uint32_t>(fact.Name.size());
  }

  vector<WireVertexInputRecord> vertexInputs;
  vertexInputs.reserve(facts.VertexInputs.size());
  for (const MetadataVertexInputFact &fact : facts.VertexInputs) {
    WireVertexInputRecord record{};
    record.Semantic = {currentNameOffset,
                       static_cast<uint32_t>(fact.Semantic.size())};
    record.SemanticIndex = fact.SemanticIndex;
    record.Location = fact.Location;
    record.ComponentType = fact.ComponentType;
    record.ComponentCount = fact.ComponentCount;
    record.Flags = fact.Flags;
    vertexInputs.push_back(record);
    currentNameOffset += static_cast<uint32_t>(fact.Semantic.size());
  }

  vector<WireSamplerRecord> samplers;
  samplers.reserve(facts.Samplers.size());
  for (const MetadataSamplerFact &fact : facts.Samplers) {
    WireSamplerRecord record{};
    record.MagFilter = fact.MagFilter;
    record.MinFilter = fact.MinFilter;
    record.MipmapMode = fact.MipmapMode;
    record.AddressModeU = fact.AddressModeU;
    record.AddressModeV = fact.AddressModeV;
    record.AddressModeW = fact.AddressModeW;
    record.MipLodBias = fact.MipLodBias;
    record.AnisotropyEnable = fact.AnisotropyEnable;
    record.MaxAnisotropy = fact.MaxAnisotropy;
    record.CompareEnable = fact.CompareEnable;
    record.CompareOp = fact.CompareOp;
    record.MinLod = fact.MinLod;
    record.MaxLod = fact.MaxLod;
    record.BorderColor = fact.BorderColor;
    record.ReductionMode = fact.ReductionMode;
    record.Flags = fact.Flags;
    samplers.push_back(record);
  }

  envelope.TotalSize = bytecodeOffset + bytecodeSize;
  vector<uint8_t> layoutBytes;
  layoutBytes.reserve(bindingBytes + rootConstantBytes + vertexInputBytes + nameBytes);
  AppendByte(layoutBytes, facts.HasRootSignature ? 1u : 0u);
  if (facts.HasRootSignature)
    for (const uint8_t value : facts.RootSignatureHash.Bytes)
      AppendByte(layoutBytes, value);
  for (const WireBindingRecord &binding : bindings) {
    const auto *data = reinterpret_cast<const uint8_t *>(&binding);
    layoutBytes.insert(
        layoutBytes.end(), data,
        data + offsetof(WireBindingRecord, TypeIndex));
  }
  for (const WireRootConstantRecord &constant : rootConstants) {
    const auto *data = reinterpret_cast<const uint8_t *>(&constant);
    layoutBytes.insert(
        layoutBytes.end(), data,
        data + offsetof(WireRootConstantRecord, TypeIndex));
  }
  if (!vertexInputs.empty()) {
    const auto *data = reinterpret_cast<const uint8_t *>(vertexInputs.data());
    layoutBytes.insert(layoutBytes.end(), data, data + vertexInputBytes);
  }
  if (!samplers.empty()) {
    const auto *data = reinterpret_cast<const uint8_t *>(samplers.data());
    layoutBytes.insert(layoutBytes.end(), data, data + samplerBytes);
  }
  for (const EntryPoint &entry : contract.EntryPoints)
    layoutBytes.insert(layoutBytes.end(), entry.Name.begin(), entry.Name.end());
  for (const MetadataBindingFact &binding : facts.Bindings)
    layoutBytes.insert(layoutBytes.end(), binding.Name.begin(), binding.Name.end());
  for (const MetadataTypeFact &type : facts.Types)
    layoutBytes.insert(layoutBytes.end(), type.Name.begin(), type.Name.end());
  for (const MetadataRootConstantFact &constant : facts.RootConstants)
    layoutBytes.insert(layoutBytes.end(), constant.Name.begin(),
                       constant.Name.end());
  for (const MetadataVertexInputFact &input : facts.VertexInputs)
    layoutBytes.insert(layoutBytes.end(), input.Semantic.begin(), input.Semantic.end());
  const Hash128 bytecodeHash = [&]() {
    vector<uint8_t> bytes;
    bytes.reserve(bytecodeSize);
    for (const StageOutput &stage : stages)
      AppendBytes(bytes, stage.Bytecode);
    return Digest(bytes, 0x42495445ull);
  }();
  const Hash128 pipelineHash = Digest(layoutBytes, 0x4c41594f5554ull + envelope.Target);
  vector<uint8_t> gpuInput = layoutBytes;
  for (const StageOutput &stage : stages)
    AppendBytes(gpuInput, stage.Bytecode);
  const Hash128 gpuHash = Digest(gpuInput, 0x475055ull + envelope.Target);
  std::memcpy(envelope.BytecodeDigest, bytecodeHash.Bytes, sizeof(envelope.BytecodeDigest));
  std::memcpy(envelope.BasePipelineLayoutDigest, pipelineHash.Bytes,
              sizeof(envelope.BasePipelineLayoutDigest));
  std::memcpy(envelope.GpuArtifact, gpuHash.Bytes, sizeof(envelope.GpuArtifact));

  metadata.assign(envelope.TotalSize, 0);
  std::memcpy(metadata.data(), &envelope, sizeof(envelope));
  if (!entries.empty())
    std::memcpy(metadata.data() + entryOffset, entries.data(), entryBytes);
  if (!bindings.empty())
    std::memcpy(metadata.data() + bindingOffset, bindings.data(), bindingBytes);
  if (!types.empty())
    std::memcpy(metadata.data() + typeOffset, types.data(), typeBytes);
  if (!rootConstants.empty())
    std::memcpy(metadata.data() + rootConstantOffset, rootConstants.data(),
                rootConstantBytes);
  if (!vertexInputs.empty())
    std::memcpy(metadata.data() + vertexInputOffset, vertexInputs.data(),
                vertexInputBytes);
  if (!samplers.empty())
    std::memcpy(metadata.data() + samplerOffset, samplers.data(), samplerBytes);
  currentNameOffset = nameOffset;
  for (const EntryPoint &entry : contract.EntryPoints) {
    std::memcpy(metadata.data() + currentNameOffset, entry.Name.data(), entry.Name.size());
    currentNameOffset += static_cast<uint32_t>(entry.Name.size());
  }
  for (const MetadataBindingFact &binding : facts.Bindings) {
    std::memcpy(metadata.data() + currentNameOffset, binding.Name.data(),
                binding.Name.size());
    currentNameOffset += static_cast<uint32_t>(binding.Name.size());
  }
  for (const MetadataTypeFact &type : facts.Types) {
    std::memcpy(metadata.data() + currentNameOffset, type.Name.data(),
                type.Name.size());
    currentNameOffset += static_cast<uint32_t>(type.Name.size());
  }
  for (const MetadataRootConstantFact &constant : facts.RootConstants) {
    std::memcpy(metadata.data() + currentNameOffset, constant.Name.data(),
                constant.Name.size());
    currentNameOffset += static_cast<uint32_t>(constant.Name.size());
  }
  for (const MetadataVertexInputFact &input : facts.VertexInputs) {
    std::memcpy(metadata.data() + currentNameOffset, input.Semantic.data(),
                input.Semantic.size());
    currentNameOffset += static_cast<uint32_t>(input.Semantic.size());
  }
  if (!facts.SerializedRootSignature.empty()) {
    std::memcpy(metadata.data() + rootSignatureOffset,
                facts.SerializedRootSignature.data(),
                facts.SerializedRootSignature.size());
  }
  uint32_t currentBytecodeOffset = bytecodeOffset;
  for (const StageOutput &stage : stages) {
    std::memcpy(metadata.data() + currentBytecodeOffset, stage.Bytecode.data(), stage.Bytecode.size());
    currentBytecodeOffset += static_cast<uint32_t>(stage.Bytecode.size());
  }
  return true;
}

class RadRayDxcResult : public IRadRayDxcResult {
private:
  DXC_MICROCOM_TM_REF_FIELDS()

  struct LaneStorage {
    bool Present{false};
    vector<uint8_t> Bytecode;
    vector<uint8_t> Metadata;
  };

  struct DiagnosticStorage {
    uint32_t Code{0};
    string Message;
  };

  RadRayDxcCompileStatus _status{RadRayDxcCompileStatus::InvalidRequest};
  RadRayDxcAbiInfo _abi{};
  vector<uint8_t> _contract;
  LaneStorage _lanes[2];
  vector<DiagnosticStorage> _diagnostics;

public:
  DXC_MICROCOM_TM_ADDREF_RELEASE_IMPL()
  DXC_MICROCOM_TM_CTOR(RadRayDxcResult)

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppvObject) override {
    return DoBasicQueryInterface<IRadRayDxcResult>(this, iid, ppvObject);
  }

  HRESULT STDMETHODCALLTYPE GetStatus(RadRayDxcCompileStatus *status) override {
    if (status == nullptr)
      return E_POINTER;
    *status = _status;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetAbiInfo(RadRayDxcAbiInfo *info) override {
    if (info == nullptr)
      return E_POINTER;
    *info = _abi;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetContractBlob(RadRayDxcBlobView *blob) override {
    if (blob == nullptr)
      return E_POINTER;
    blob->Data = _contract.empty() ? nullptr : _contract.data();
    blob->Size = static_cast<uint32_t>(_contract.size());
    return _contract.empty() ? E_BOUNDS : S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTargetLane(RadRayDxcTarget target,
                                           RadRayDxcLaneView *lane) override {
    if (lane == nullptr)
      return E_POINTER;
    if (target != RadRayDxcTarget::DXIL && target != RadRayDxcTarget::SPIRV)
      return E_INVALIDARG;
    const LaneStorage &storage = _lanes[static_cast<uint32_t>(target)];
    if (!storage.Present)
      return E_BOUNDS;
    lane->Target = target;
    lane->Bytecode = {storage.Bytecode.data(), static_cast<uint32_t>(storage.Bytecode.size())};
    lane->Metadata = {storage.Metadata.data(), static_cast<uint32_t>(storage.Metadata.size())};
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDiagnosticCount(uint32_t *count) override {
    if (count == nullptr)
      return E_POINTER;
    *count = static_cast<uint32_t>(_diagnostics.size());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDiagnostic(uint32_t index,
                                           RadRayDxcDiagnosticView *diagnostic) override {
    if (diagnostic == nullptr)
      return E_POINTER;
    if (index >= _diagnostics.size())
      return E_BOUNDS;
    const DiagnosticStorage &stored = _diagnostics[index];
    diagnostic->Code = stored.Code;
    diagnostic->MessageUtf8 = {
        reinterpret_cast<const uint8_t *>(stored.Message.data()),
        static_cast<uint32_t>(stored.Message.size())};
    return S_OK;
  }

  void SetStatus(RadRayDxcCompileStatus status) noexcept { _status = status; }

  void SetAbi() noexcept {
    _abi = {};
    _abi.AbiVersion = radray::shader::kRadRayDxcAbiVersion;
    _abi.MetadataSchemaVersion = radray::shader::kRadRayDxcMetadataSchemaVersion;
    _abi.ToolchainMajor = 1;
    _abi.ToolchainMinor = 9;
    std::memcpy(_abi.ToolchainIdentity.Bytes, kToolchainIdentity,
                sizeof(kToolchainIdentity));
  }

  void SetContract(vector<uint8_t> contract) { _contract = std::move(contract); }

  void SetLane(RadRayDxcTarget target, vector<uint8_t> bytecode,
               vector<uint8_t> metadata) {
    LaneStorage &storage = _lanes[static_cast<uint32_t>(target)];
    storage.Present = true;
    storage.Bytecode = std::move(bytecode);
    storage.Metadata = std::move(metadata);
  }

  void ClearLanes() noexcept {
    for (LaneStorage &lane : _lanes) {
      lane.Present = false;
      lane.Bytecode.clear();
      lane.Metadata.clear();
    }
  }

  void AddDiagnostic(uint32_t code, string message) {
    _diagnostics.push_back({code, std::move(message)});
  }
};

template <typename T>
CComPtr<T> AllocateComObject() {
  return CComPtr<T>(T::Alloc(DxcGetThreadMallocNoRef()));
}

HRESULT PublishResult(CComPtr<RadRayDxcResult> &result,
                      IRadRayDxcResult **output) {
  if (result.p == nullptr)
    return E_OUTOFMEMORY;
  return result->QueryInterface(radray::shader::IID_IRadRayDxcResult,
                                reinterpret_cast<void **>(output));
}

bool ValidateAssignments(const CompileRequest &request,
                         const ContractData &contract, string &error) {
  for (const NameValue &assignment : request.Assignments) {
    const auto group = std::find_if(
        contract.KeywordGroups.begin(), contract.KeywordGroups.end(),
        [&](const KeywordGroup &value) { return value.Name == assignment.Name; });
    if (group == contract.KeywordGroups.end()) {
      error = "keyword assignment names an unknown group";
      return false;
    }
    if (std::find(group->Values.begin(), group->Values.end(), assignment.Value) ==
        group->Values.end()) {
      error = "keyword assignment value is outside its declared domain";
      return false;
    }
  }
  for (const KeywordGroup &group : contract.KeywordGroups) {
    const size_t count = static_cast<size_t>(std::count_if(
        request.Assignments.begin(), request.Assignments.end(),
        [&](const NameValue &value) { return value.Name == group.Name; }));
    if (count != 1) {
      error = "concrete compile requires one assignment for every keyword group";
      return false;
    }
  }
  return true;
}

class RadRayDxcCompiler : public IRadRayDxcCompiler {
private:
  DXC_MICROCOM_TM_REF_FIELDS()

public:
  DXC_MICROCOM_TM_ADDREF_RELEASE_IMPL()
  DXC_MICROCOM_TM_CTOR(RadRayDxcCompiler)

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppvObject) override {
    return DoBasicQueryInterface<IRadRayDxcCompiler>(this, iid, ppvObject);
  }

  HRESULT STDMETHODCALLTYPE GetAbiInfo(RadRayDxcAbiInfo *info) override {
    if (info == nullptr)
      return E_POINTER;
    *info = {};
    info->AbiVersion = radray::shader::kRadRayDxcAbiVersion;
    info->MetadataSchemaVersion = radray::shader::kRadRayDxcMetadataSchemaVersion;
    info->ToolchainMajor = 1;
    info->ToolchainMinor = 9;
    std::memcpy(info->ToolchainIdentity.Bytes, kToolchainIdentity,
                sizeof(kToolchainIdentity));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DiscoverSourceContract(
      RadRayDxcBlobView request,
      RadRayDxcIncludePathListView includePaths,
      IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    if (request.Data == nullptr || request.Size == 0)
      return E_INVALIDARG;
    DxcThreadMalloc threadMalloc(m_pMalloc);
    CComPtr<RadRayDxcResult> output = AllocateComObject<RadRayDxcResult>();
    if (output.p == nullptr)
      return E_OUTOFMEMORY;
    output->SetAbi();
    DiscoveryRequest parsed;
    string error;
    ContractData contract;
    vector<Diagnostic> diagnostics;
    if (!ReadDiscoveryRequest(request, parsed, error)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      if (!error.empty())
        output->AddDiagnostic(2000, std::move(error));
      for (Diagnostic &diagnostic : diagnostics)
        output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
      return PublishResult(output, result);
    }
    bool hasContract = false;
    if ((parsed.Targets & 1u) != 0) {
      if (!CollectContractWithFrontend(parsed, RadRayDxcTarget::DXIL,
                                       includePaths, contract, diagnostics)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        for (Diagnostic &diagnostic : diagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      hasContract = true;
    }
    if ((parsed.Targets & 2u) != 0) {
      ContractData spirvContract;
      if (!CollectContractWithFrontend(parsed, RadRayDxcTarget::SPIRV,
                                       includePaths, spirvContract,
                                       diagnostics)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        for (Diagnostic &diagnostic : diagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      if (hasContract && EncodeContract(contract) != EncodeContract(spirvContract)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        output->AddDiagnostic(
            2011, "DXIL and SPIR-V frontends discovered different source contracts");
        return PublishResult(output, result);
      }
      if (!hasContract) {
        contract = std::move(spirvContract);
        hasContract = true;
      }
    }
    if (!hasContract) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      output->AddDiagnostic(2000, "discovery request did not select a target");
      return PublishResult(output, result);
    }
    output->SetStatus(RadRayDxcCompileStatus::Success);
    output->SetContract(EncodeContract(contract));
    return PublishResult(output, result);
  }

  HRESULT STDMETHODCALLTYPE CompileVariant(
      RadRayDxcBlobView request,
      RadRayDxcIncludePathListView includePaths,
      IRadRayDxcResult **result) override {
    if (result == nullptr)
      return E_POINTER;
    *result = nullptr;
    if (request.Data == nullptr || request.Size == 0)
      return E_INVALIDARG;
    DxcThreadMalloc threadMalloc(m_pMalloc);
    CComPtr<RadRayDxcResult> output = AllocateComObject<RadRayDxcResult>();
    if (output.p == nullptr)
      return E_OUTOFMEMORY;
    output->SetAbi();
    CompileRequest parsed;
    string error;
    if (!ReadCompileRequest(request, parsed, error)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      output->AddDiagnostic(2000, std::move(error));
      return PublishResult(output, result);
    }
    vector<wstring> validatedPathStorage;
    if (!AppendIncludePathArguments(includePaths, validatedPathStorage, error)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      output->AddDiagnostic(2000, std::move(error));
      return PublishResult(output, result);
    }
    ContractData contract;
    vector<Diagnostic> discoveryDiagnostics;
    const DiscoveryRequest discoveryRequest = MakeDiscoveryRequest(parsed);
    bool hasContract = false;
    if ((parsed.Targets & 1u) != 0) {
      if (!CollectContractWithFrontend(discoveryRequest,
                                       RadRayDxcTarget::DXIL, includePaths,
                                       contract, discoveryDiagnostics)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        for (Diagnostic &diagnostic : discoveryDiagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      hasContract = true;
    }
    if ((parsed.Targets & 2u) != 0) {
      ContractData otherContract;
      if (!CollectContractWithFrontend(
              discoveryRequest, RadRayDxcTarget::SPIRV, includePaths,
              otherContract, discoveryDiagnostics)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        for (Diagnostic &diagnostic : discoveryDiagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      if (hasContract && EncodeContract(contract) != EncodeContract(otherContract)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        output->AddDiagnostic(
            2011, "DXIL and SPIR-V frontends discovered different source contracts");
        return PublishResult(output, result);
      }
      if (!hasContract) {
        contract = std::move(otherContract);
        hasContract = true;
      }
    }
    if (!hasContract) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      output->AddDiagnostic(2000, "compile request did not select a target");
      return PublishResult(output, result);
    }
    output->SetContract(EncodeContract(contract));
    if (!ValidateAssignments(parsed, contract, error)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      output->AddDiagnostic(2010, std::move(error));
      return PublishResult(output, result);
    }
    if (!IsZero(parsed.ExpectedContract) &&
        !(parsed.ExpectedContract == contract.Hash)) {
      output->SetStatus(RadRayDxcCompileStatus::ContractMismatch);
      output->AddDiagnostic(2006, "expected ContractHash does not match discovered source contract");
      return PublishResult(output, result);
    }

    DiscoveryRequest concreteRequest = MakeDiscoveryRequest(parsed);
    concreteRequest.Defines.reserve(
        concreteRequest.Defines.size() + parsed.Assignments.size());
    for (const NameValue &assignment : parsed.Assignments)
      concreteRequest.Defines.push_back(assignment);
    for (const RadRayDxcTarget target : {RadRayDxcTarget::DXIL,
                                         RadRayDxcTarget::SPIRV}) {
      const uint8_t bit = target == RadRayDxcTarget::DXIL ? 1 : 2;
      if ((parsed.Targets & bit) == 0)
        continue;
      ContractData concreteContract;
      vector<Diagnostic> concreteDiagnostics;
      if (!CollectContractWithFrontend(concreteRequest, target, includePaths,
                                       concreteContract, concreteDiagnostics)) {
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
        for (Diagnostic &diagnostic : concreteDiagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      if (EncodeContract(concreteContract) != EncodeContract(contract)) {
        output->SetStatus(RadRayDxcCompileStatus::ContractMismatch);
        output->AddDiagnostic(
            2007, "concrete assignments changed the frontend source contract");
        return PublishResult(output, result);
      }
    }

    for (const RadRayDxcTarget target : {RadRayDxcTarget::DXIL, RadRayDxcTarget::SPIRV}) {
      const uint8_t bit = target == RadRayDxcTarget::DXIL ? 1 : 2;
      if ((parsed.Targets & bit) == 0)
        continue;
      vector<Diagnostic> compileDiagnostics;
      vector<StageOutput> stages;
      bool success = true;
      for (const EntryPoint &entry : contract.EntryPoints) {
        StageOutput stage;
        if (!CompileStage(parsed, target, entry, includePaths, stage,
                          compileDiagnostics)) {
          success = false;
          break;
        }
        stages.push_back(std::move(stage));
      }
      if (!success) {
        output->SetStatus(RadRayDxcCompileStatus::TargetFailure);
        output->ClearLanes();
        for (Diagnostic &diagnostic : compileDiagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      vector<uint8_t> bytecode;
      for (const StageOutput &stage : stages)
        AppendBytes(bytecode, stage.Bytecode);
      vector<uint8_t> metadata;
      if (!BuildMetadata(parsed, contract, target, stages, compileDiagnostics, metadata)) {
        output->SetStatus(RadRayDxcCompileStatus::TargetFailure);
        output->ClearLanes();
        for (Diagnostic &diagnostic : compileDiagnostics)
          output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
        return PublishResult(output, result);
      }
      output->SetLane(target, std::move(bytecode), std::move(metadata));
    }
    output->SetStatus(RadRayDxcCompileStatus::Success);
    return PublishResult(output, result);
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
