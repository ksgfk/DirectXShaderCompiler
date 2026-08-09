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

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/HlslTypes.h"
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
#include "dxc/DxilRootSignature/DxilRootSignature.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/DXIL/DxilSignature.h"
#include "dxc/DXIL/DxilSignatureElement.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

#ifdef ENABLE_SPIRV_CODEGEN
#include "clang/SPIRV/SpirvInstruction.h"
#include "clang/SPIRV/SpirvModule.h"
#include "clang/SPIRV/SpirvType.h"
#include "SpirvEmitter.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
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

constexpr uint8_t kToolchainIdentity[16] = {
    0x08, 0x02, 0x09, 0x01, 0x72, 0x61, 0x64, 0x72,
    0x61, 0x79, 0x2d, 0x31, 0x2e, 0x39, 0x2e, 0x33};
constexpr uint64_t kMetadataToolchainIdentity = 0x0000000001090209ull;
constexpr uint32_t kMaxCollectionCount = 4096;
constexpr uint32_t kMaxEntryCount = 16;

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

enum class MetadataBindingKind : uint32_t {
  CBuffer = 1,
  Buffer = 2,
  RWBuffer = 3,
  Texture = 4,
  RWTexture = 5,
  Sampler = 6,
};

constexpr uint32_t kMetadataImmutableSampler = 1u << 0;

struct MetadataBindingFact {
  string Name;
  uint32_t Group{0};
  uint32_t Binding{0};
  uint32_t RegisterClass{0};
  uint32_t Type{0};
  uint32_t Count{1};
  uint32_t StageMask{0};
  uint32_t Flags{0};
  bool HasExplicitBinding{false};
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
  uint32_t RegisterSpace{0};
  uint32_t Register{0};
  uint32_t Offset{0};
  uint32_t Size{0};
  uint32_t StageMask{0};
  uint32_t Flags{0};
};

struct MetadataVertexInputFact {
  string Semantic;
  uint32_t SemanticIndex{0};
  uint32_t Location{0};
  uint32_t ComponentType{0};
  uint32_t ComponentCount{0};
  uint32_t Flags{0};
};

struct MetadataRootBindingFact {
  uint32_t RegisterClass{0};
  uint32_t Binding{0};
  uint32_t Group{0};
};

struct MetadataFacts {
  vector<MetadataBindingFact> Bindings;
  vector<MetadataTypeFact> Types;
  vector<MetadataRootConstantFact> RootConstants;
  vector<MetadataVertexInputFact> VertexInputs;
  vector<MetadataRootBindingFact> RootBindings;
  vector<string> StaticSamplerNames;
  Hash128 RootSignatureHash{};
  bool HasRootSignature{false};
  bool HasStaticSamplerPolicy{false};
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

class RadRayContractAstVisitor
    : public clang::RecursiveASTVisitor<RadRayContractAstVisitor> {
public:
  explicit RadRayContractAstVisitor(RadRayContractCollector &collector)
      : _collector(collector) {}

  bool VisitFunctionDecl(clang::FunctionDecl *decl);

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
  void AddVertexInput(string semantic, uint32_t semanticIndex,
                      uint32_t componentType, uint32_t componentCount,
                      uint32_t location);
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

  const MetadataFacts &Metadata() const noexcept { return _metadata; }
  void CollectDxilModule(llvm::Module &module);
#ifdef ENABLE_SPIRV_CODEGEN
  void CollectSpirvAction(clang::EmitSpirvAction &action,
                          string_view entryPointName);
#endif

private:
  friend class RadRayContractPPCallbacks;
  friend class RadRayKeywordPragmaHandler;
  friend class RadRayContractAstVisitor;

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
  if (attribute->getStage() == "vertex") {
    uint32_t location = 0;
    for (const clang::ParmVarDecl *parameter : decl->parameters()) {
      string semantic;
      uint32_t semanticIndex = 0;
      for (const hlsl::UnusualAnnotation *annotation :
           parameter->getUnusualAnnotations()) {
        const auto *semanticDecl = llvm::dyn_cast<hlsl::SemanticDecl>(annotation);
        if (semanticDecl == nullptr)
          continue;
        semantic = semanticDecl->SemanticName.str();
        size_t suffix = semantic.size();
        while (suffix != 0 &&
               std::isdigit(static_cast<unsigned char>(semantic[suffix - 1])) != 0)
          --suffix;
        for (size_t index = suffix; index < semantic.size(); ++index)
          semanticIndex = semanticIndex * 10u +
                          static_cast<uint32_t>(semantic[index] - '0');
        semantic.resize(suffix);
        break;
      }
      if (semantic.empty() ||
          (semantic.size() >= 3 && semantic.compare(0, 3, "SV_") == 0))
        continue;

      clang::QualType elementType = hlsl::GetElementTypeOrType(parameter->getType());
      if (hlsl::IsHLSLVecType(parameter->getType()))
        elementType = hlsl::GetHLSLVecElementType(parameter->getType());
      uint32_t componentType = 0;
      if (const auto *builtin = elementType->getAs<clang::BuiltinType>()) {
        if (builtin->isFloatingPoint())
          componentType = 1;
        else if (builtin->isSignedInteger())
          componentType = 2;
        else if (builtin->isUnsignedInteger())
          componentType = 3;
      }
      const uint32_t componentCount = hlsl::IsHLSLVecType(parameter->getType())
                                          ? hlsl::GetHLSLVecSize(parameter->getType())
                                          : 1u;
      if (componentType != 0 && componentCount != 0)
        _collector.AddVertexInput(std::move(semantic), semanticIndex,
                                  componentType, componentCount, location++);
    }
  }
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

void RadRayContractCollector::AddVertexInput(string semantic,
                                              uint32_t semanticIndex,
                                              uint32_t componentType,
                                              uint32_t componentCount,
                                              uint32_t location) {
  const auto duplicate = std::find_if(
      _metadata.VertexInputs.begin(), _metadata.VertexInputs.end(),
      [&](const MetadataVertexInputFact &value) noexcept {
        return value.Semantic == semantic && value.SemanticIndex == semanticIndex;
      });
  if (duplicate == _metadata.VertexInputs.end()) {
    _metadata.VertexInputs.push_back({std::move(semantic), semanticIndex,
                                      location, componentType, componentCount,
                                      0});
  }
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

uint32_t MetadataRegisterNamespace(MetadataBindingKind kind) noexcept {
  switch (kind) {
  case MetadataBindingKind::CBuffer:
    return 0;
  case MetadataBindingKind::Buffer:
  case MetadataBindingKind::Texture:
    return 1;
  case MetadataBindingKind::RWBuffer:
  case MetadataBindingKind::RWTexture:
    return 2;
  case MetadataBindingKind::Sampler:
    return 3;
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
                            MetadataStageBitFor(stage), flags, true});
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

void CollectDxilTypeFacts(const hlsl::DxilTypeSystem &typeSystem,
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
    const hlsl::DxilStructAnnotation *annotation =
        typeSystem.GetStructAnnotation(type);
    const uint32_t size = annotation != nullptr && annotation->GetCBufferSize() != 0
                              ? annotation->GetCBufferSize()
                              : DxilTypeSize(type, typeSystem);
    const uint32_t parentIndex = static_cast<uint32_t>(facts.Types.size());
    facts.Types.push_back({DxilStructName(type), kMetadataNoParent, 4u, 1u,
                           0u, size, size, 0u, kMetadataNoType, {}});
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

  ResolveMetadataTypeIndices(facts.Types);
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

MetadataBindingKind DxilBindingKind(const hlsl::DxilResourceBase &resource) {
  if (resource.GetClass() == hlsl::DXIL::ResourceClass::CBuffer)
    return MetadataBindingKind::CBuffer;
  if (resource.GetClass() == hlsl::DXIL::ResourceClass::Sampler)
    return MetadataBindingKind::Sampler;
  const auto kind = resource.GetKind();
  const bool texture = hlsl::DxilResource::IsAnyTexture(kind);
  const bool rw = resource.GetClass() == hlsl::DXIL::ResourceClass::UAV;
  if (rw)
    return texture ? MetadataBindingKind::RWTexture
                   : MetadataBindingKind::RWBuffer;
  return texture ? MetadataBindingKind::Texture : MetadataBindingKind::Buffer;
}

void AddDxilResourceFact(const hlsl::DxilResourceBase &resource,
                         MetadataFacts &facts, ShaderStage stage) {
  const uint32_t registerClass = DxilRegisterClass(resource.GetClass());
  if (registerClass == 0xffffffffu)
    return;
  const MetadataBindingKind kind = DxilBindingKind(resource);
  const bool hasExplicitBinding = resource.GetLowerBound() != UINT_MAX;
  const uint32_t binding = hasExplicitBinding ? resource.GetLowerBound()
                                               : resource.GetID();
  const uint32_t group = resource.GetSpaceID() == UINT_MAX
                             ? 0u
                             : resource.GetSpaceID();
  MetadataBindingFact *fact = AddMetadataBinding(
      facts, resource.GetGlobalName(), group, binding, kind,
      resource.GetRangeSize(), stage);
  if (fact != nullptr) {
    fact->RegisterClass = registerClass;
    fact->HasExplicitBinding = hasExplicitBinding;
  }
}

void AddDxilRootBinding(vector<MetadataRootBindingFact> &rootBindings,
                        uint32_t registerClass, uint32_t binding,
                        uint32_t group, uint32_t count = 1) {
  const uint32_t boundedCount = std::min(count, kMaxCollectionCount);
  for (uint32_t index = 0; index < std::max(1u, boundedCount); ++index)
    rootBindings.push_back({registerClass, binding + index, group});
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

void ValidateDxilRootSignature(const hlsl::DxilVersionedRootSignatureDesc &root,
                               MetadataFacts &facts,
                               ShaderStage stage,
                               vector<Diagnostic> &diagnostics) {
  vector<MetadataRootBindingFact> rootBindings;
  vector<MetadataRootBindingFact> staticSamplerBindings;
  const auto addParameter = [&](hlsl::DxilRootParameterType type,
                                uint32_t registerClass, uint32_t binding,
                                uint32_t group) {
    if (type != hlsl::DxilRootParameterType::Constants32Bit)
      AddDxilRootBinding(rootBindings, registerClass, binding, group);
  };
  const auto addRange = [&](const hlsl::DxilDescriptorRange &range) {
    const uint32_t registerClass = DxilRootRangeClass(range.RangeType);
    if (registerClass != 0xffffffffu)
      AddDxilRootBinding(rootBindings, registerClass,
                         range.BaseShaderRegister, range.RegisterSpace,
                         range.NumDescriptors);
  };
  const auto addRange1 = [&](const hlsl::DxilDescriptorRange1 &range) {
    const uint32_t registerClass = DxilRootRangeClass(range.RangeType);
    if (registerClass != 0xffffffffu)
      AddDxilRootBinding(rootBindings, registerClass,
                         range.BaseShaderRegister, range.RegisterSpace,
                         range.NumDescriptors);
  };

  if (root.Version == hlsl::DxilRootSignatureVersion::Version_1_1) {
    const auto &desc = root.Desc_1_1;
    for (uint32_t index = 0; index < desc.NumParameters; ++index) {
      const hlsl::DxilRootParameter1 &parameter = desc.pParameters[index];
      if (parameter.ParameterType == hlsl::DxilRootParameterType::DescriptorTable) {
        for (uint32_t rangeIndex = 0;
             rangeIndex < parameter.DescriptorTable.NumDescriptorRanges;
             ++rangeIndex)
          addRange1(parameter.DescriptorTable.pDescriptorRanges[rangeIndex]);
      } else if (parameter.ParameterType ==
                 hlsl::DxilRootParameterType::Constants32Bit) {
        facts.RootConstants.push_back(
            {parameter.Constants.RegisterSpace, parameter.Constants.ShaderRegister,
             0u, parameter.Constants.Num32BitValues * 4u,
             MetadataStageBitFor(stage), 0u});
      } else {
        const uint32_t registerClass =
            parameter.ParameterType == hlsl::DxilRootParameterType::CBV
                ? 0u
                : parameter.ParameterType == hlsl::DxilRootParameterType::SRV
                      ? 1u
                      : 2u;
        addParameter(parameter.ParameterType, registerClass,
                     parameter.Descriptor.ShaderRegister,
                     parameter.Descriptor.RegisterSpace);
      }
    }
    for (uint32_t index = 0; index < desc.NumStaticSamplers; ++index) {
      const auto &sampler = desc.pStaticSamplers[index];
      AddDxilRootBinding(rootBindings, 3u, sampler.ShaderRegister,
                         sampler.RegisterSpace);
      AddDxilRootBinding(staticSamplerBindings, 3u, sampler.ShaderRegister,
                         sampler.RegisterSpace);
    }
  } else {
    const auto &desc = root.Desc_1_0;
    for (uint32_t index = 0; index < desc.NumParameters; ++index) {
      const hlsl::DxilRootParameter &parameter = desc.pParameters[index];
      if (parameter.ParameterType == hlsl::DxilRootParameterType::DescriptorTable) {
        for (uint32_t rangeIndex = 0;
             rangeIndex < parameter.DescriptorTable.NumDescriptorRanges;
             ++rangeIndex)
          addRange(parameter.DescriptorTable.pDescriptorRanges[rangeIndex]);
      } else if (parameter.ParameterType ==
                 hlsl::DxilRootParameterType::Constants32Bit) {
        facts.RootConstants.push_back(
            {parameter.Constants.RegisterSpace, parameter.Constants.ShaderRegister,
             0u, parameter.Constants.Num32BitValues * 4u,
             MetadataStageBitFor(stage), 0u});
      } else {
        const uint32_t registerClass =
            parameter.ParameterType == hlsl::DxilRootParameterType::CBV
                ? 0u
                : parameter.ParameterType == hlsl::DxilRootParameterType::SRV
                      ? 1u
                      : 2u;
        addParameter(parameter.ParameterType, registerClass,
                     parameter.Descriptor.ShaderRegister,
                     parameter.Descriptor.RegisterSpace);
      }
    }
    for (uint32_t index = 0; index < desc.NumStaticSamplers; ++index) {
      const auto &sampler = desc.pStaticSamplers[index];
      AddDxilRootBinding(rootBindings, 3u, sampler.ShaderRegister,
                         sampler.RegisterSpace);
      AddDxilRootBinding(staticSamplerBindings, 3u, sampler.ShaderRegister,
                         sampler.RegisterSpace);
    }
  }

  for (const MetadataBindingFact &binding : facts.Bindings) {
    if (binding.RegisterClass != 3u)
      continue;
    const bool isStaticSampler = std::any_of(
        staticSamplerBindings.begin(), staticSamplerBindings.end(),
        [&](const MetadataRootBindingFact &rootBinding) noexcept {
          return rootBinding.Group == binding.Group &&
                 rootBinding.Binding == binding.Binding;
        });
    if (!isStaticSampler ||
        std::find(facts.StaticSamplerNames.begin(),
                  facts.StaticSamplerNames.end(),
                  binding.Name) != facts.StaticSamplerNames.end())
      continue;
    facts.StaticSamplerNames.push_back(binding.Name);
  }
  facts.HasStaticSamplerPolicy = !staticSamplerBindings.empty();

  for (const MetadataRootBindingFact &rootBinding : rootBindings) {
    const auto found = std::find_if(
        facts.RootBindings.begin(), facts.RootBindings.end(),
        [&](const MetadataRootBindingFact &value) noexcept {
          return value.RegisterClass == rootBinding.RegisterClass &&
                 value.Group == rootBinding.Group &&
                 value.Binding == rootBinding.Binding;
        });
    if (found != facts.RootBindings.end())
      continue;
    facts.RootBindings.push_back(rootBinding);
  }

  const auto hasDuplicateStaticSampler = [&]() noexcept {
    for (size_t left = 0; left < rootBindings.size(); ++left)
      for (size_t right = left + 1; right < rootBindings.size(); ++right)
        if (rootBindings[left].RegisterClass == 3u &&
            rootBindings[left].RegisterClass == rootBindings[right].RegisterClass &&
            rootBindings[left].Group == rootBindings[right].Group &&
            rootBindings[left].Binding == rootBindings[right].Binding)
          return true;
    return false;
  };
  if (hasDuplicateStaticSampler())
    diagnostics.push_back({2104, "DXIL RootSignature contains duplicate static sampler"});
}

void RadRayContractCollector::CollectDxilModule(llvm::Module &module) {
  hlsl::DxilModule &dxil = module.GetOrCreateDxilModule();
  dxil.RemoveUnusedResources();
  for (const auto &resource : dxil.GetCBuffers()) {
    AddDxilResourceFact(*resource, _metadata, _stage);
    const llvm::Type *resourceType = resource->GetHLSLType();
    if (resourceType != nullptr && resourceType->isPointerTy())
      resourceType = resourceType->getPointerElementType();
    if (const auto *wrapper = llvm::dyn_cast_or_null<llvm::StructType>(resourceType)) {
      if (wrapper->getName().startswith("hostlayout.") &&
          wrapper->getNumElements() == 1)
        resourceType = wrapper->getElementType(0);
    }
    if (const auto *type = llvm::dyn_cast<llvm::StructType>(resourceType))
      CollectDxilTypeFacts(dxil.GetTypeSystem(), type, _metadata);
  }
  for (const auto &resource : dxil.GetSamplers())
    AddDxilResourceFact(*resource, _metadata, _stage);
  for (const auto &resource : dxil.GetSRVs())
    AddDxilResourceFact(*resource, _metadata, _stage);
  for (const auto &resource : dxil.GetUAVs())
    AddDxilResourceFact(*resource, _metadata, _stage);

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
      _metadata.VertexInputs.push_back(
          {semantic, element->GetSemanticStartIndex(), location,
           componentType, std::max(1u, element->GetRows() * element->GetCols()), 0});
    }
  }

  const vector<uint8_t> &serializedRoot = dxil.GetSerializedRootSignature();
  if (serializedRoot.empty())
    return;
  _metadata.HasRootSignature = true;
  _metadata.RootSignatureHash = Digest(serializedRoot, 0x52534947ull);
  const hlsl::DxilVersionedRootSignatureDesc *root = nullptr;
  hlsl::DeserializeRootSignature(serializedRoot.data(),
                                 static_cast<uint32_t>(serializedRoot.size()),
                                 &root);
  if (root == nullptr) {
    _diagnostics.push_back({2106, "DXIL RootSignature could not be decoded"});
    return;
  }
  ValidateDxilRootSignature(*root, _metadata, _stage, _diagnostics);
  hlsl::DeleteRootSignature(root);
}

#ifdef ENABLE_SPIRV_CODEGEN
const clang::spirv::SpirvType *UnwrapSpirvPointer(
    const clang::spirv::SpirvType *type) {
  if (const auto *pointer = llvm::dyn_cast<clang::spirv::SpirvPointerType>(type))
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

void CollectSpirvStructFacts(const clang::spirv::StructType *structure,
                             MetadataFacts &facts) {
  if (structure == nullptr)
    return;
  const auto existing = std::find_if(
      facts.Types.begin(), facts.Types.end(),
      [&](const MetadataTypeFact &fact) noexcept {
        return fact.ParentIndex == kMetadataNoParent && fact.Kind == 4u &&
               fact.Name == SpirvStructName(structure);
      });
  if (existing != facts.Types.end())
    return;
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
  ResolveMetadataTypeIndices(facts.Types);
}

void RadRayContractCollector::CollectSpirvAction(
    clang::EmitSpirvAction &action, string_view entryPointName) {
  clang::spirv::SpirvEmitter *emitter = action.getSpirvEmitter();
  if (emitter == nullptr || emitter->getSpirvBuilder().getModule() == nullptr)
    return;
  clang::spirv::SpirvModule *module = emitter->getSpirvBuilder().getModule();

  vector<clang::spirv::SpirvVariable *> activeResources;
  if (!entryPointName.empty()) {
    RadRaySpirvEntryPointFinder entryFinder(entryPointName);
    entryFinder.TraverseDecl(emitter->getASTContext().getTranslationUnitDecl());
    if (entryFinder.EntryPoint() != nullptr) {
      RadRaySpirvResourceUseVisitor resourceVisitor(
          emitter->getDeclResultIdMapper(), activeResources);
      resourceVisitor.TraverseEntry(entryFinder.EntryPoint());
    }
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
    if (pushConstant) {
      const uint32_t size = SpirvTypeSize(valueType);
      if (!_sawPushConstant) {
        _metadata.RootConstants.push_back({0u, 0u, 0u, size, stageMask, 1u});
        _sawPushConstant = true;
      } else {
        _diagnostics.push_back(
            {2103, "SPIR-V source contains multiple push constant blocks"});
      }
      if (const auto *structure =
              llvm::dyn_cast<clang::spirv::StructType>(valueType))
        CollectSpirvStructFacts(structure, _metadata);
      continue;
    }

    MetadataBindingKind bindingKind{};
    bool isBinding = variable->hasBinding();
    if (clang::spirv::SpirvType::isSampler(valueType))
      bindingKind = MetadataBindingKind::Sampler;
    else if (clang::spirv::SpirvType::isRWTexture(valueType))
      bindingKind = MetadataBindingKind::RWTexture;
    else if (clang::spirv::SpirvType::isTexture(valueType))
      bindingKind = MetadataBindingKind::Texture;
    else if (clang::spirv::SpirvType::isRWBuffer(valueType))
      bindingKind = MetadataBindingKind::RWBuffer;
    else if (clang::spirv::SpirvType::isBuffer(valueType))
      bindingKind = MetadataBindingKind::Buffer;
    else if (llvm::isa<clang::spirv::StructType>(valueType)) {
      const auto *structure = llvm::cast<clang::spirv::StructType>(valueType);
      if (structure->getInterfaceType() ==
              clang::spirv::StructInterfaceType::StorageBuffer &&
          (variable->getStorageClass() == spv::StorageClass::Uniform ||
           variable->getStorageClass() == spv::StorageClass::StorageBuffer))
        bindingKind = structure->isReadOnly() ? MetadataBindingKind::Buffer
                                              : MetadataBindingKind::RWBuffer;
      else if (structure->getInterfaceType() ==
                   clang::spirv::StructInterfaceType::UniformBuffer &&
               variable->getStorageClass() == spv::StorageClass::Uniform)
        bindingKind = MetadataBindingKind::CBuffer;
    }
    else
      isBinding = false;
    if (!isBinding)
      continue;
    const int32_t group = variable->getDescriptorSetNo();
    const int32_t binding = variable->getBindingNo();
    if (group < 0 || binding < 0)
      continue;
    AddMetadataBinding(_metadata, variable->getDebugName().str(),
                       static_cast<uint32_t>(group),
                       static_cast<uint32_t>(binding), bindingKind, count, _stage);
    if (bindingKind == MetadataBindingKind::CBuffer) {
      const auto *structure =
          llvm::dyn_cast<clang::spirv::StructType>(valueType);
      CollectSpirvStructFacts(structure, _metadata);
    }
  }
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

const wchar_t *ProfileForStage(ShaderStage stage) noexcept {
  switch (stage) {
  case ShaderStage::Vertex:
    return L"vs_6_0";
  case ShaderStage::Pixel:
    return L"ps_6_0";
  case ShaderStage::Compute:
    return L"cs_6_0";
  }
  return L"";
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
  argumentStorage.emplace_back(L"lib_6_1");
  argumentStorage.emplace_back(L"-Vd");
  if (request.AllResourcesBound != 0)
    argumentStorage.emplace_back(L"-all_resources_bound");
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
  MetadataFacts Facts;
};

bool TryCollectDxilRootPolicy(const CompileRequest &request,
                              const EntryPoint &entry, IDxcUtils *utils,
                              IDxcIncludeHandler *includeHandler,
                              const vector<wstring> &spirvArguments,
                              MetadataFacts &facts) {
  vector<wstring> argumentStorage;
  argumentStorage.reserve(spirvArguments.size());
  for (const wstring &argument : spirvArguments) {
    if (argument == L"-spirv" ||
        argument == L"-fspv-target-env=vulkan1.2")
      continue;
    argumentStorage.push_back(argument);
  }
  vector<LPCWSTR> arguments;
  arguments.reserve(argumentStorage.size());
  for (const wstring &argument : argumentStorage)
    arguments.push_back(argument.c_str());

  wstring sourceName;
  wstring entryName;
  if (!ToWide(request.SourceName, sourceName) ||
      !ToWide(entry.Name, entryName))
    return false;
  CComPtr<IDxcCompilerArgs> compilerArgs;
  if (FAILED(utils->BuildArguments(
          sourceName.c_str(), entryName.c_str(), ProfileForStage(entry.Stage),
          arguments.data(), static_cast<uint32_t>(arguments.size()), nullptr, 0,
          &compilerArgs)))
    return false;

  DxcBuffer source{};
  source.Ptr = request.RootSource.data();
  source.Size = request.RootSource.size();
  source.Encoding = DXC_CP_UTF8;
  RadRayFrontendObserver observer(request.SourceName, RadRayDxcTarget::DXIL,
                                   entry.Stage);
  CComPtr<IDxcResult> result;
  if (FAILED(CompileDxcWithRadRayObserver(
          &source, compilerArgs->GetArguments(), compilerArgs->GetCount(),
          includeHandler, &observer, IID_PPV_ARGS(&result))))
    return false;
  vector<Diagnostic> ignoredDiagnostics;
  if (!CheckDxcResult(result, 2004,
                      "fork DXC rejected the DXIL root policy probe",
                      ignoredDiagnostics))
    return false;
  facts = observer.Metadata();
  return facts.HasRootSignature;
}

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
  CComPtr<IDxcCompilerArgs> compilerArgs;
  if (FAILED(utils->BuildArguments(
          sourceName.c_str(), entryName.c_str(), ProfileForStage(entry.Stage),
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
  if (target == RadRayDxcTarget::SPIRV) {
    MetadataFacts dxilRootPolicy;
    const bool hasRootPolicy =
        TryCollectDxilRootPolicy(request, entry, utils.p, includeHandler.p,
                                 argumentStorage, dxilRootPolicy);
    if (hasRootPolicy &&
        dxilRootPolicy.HasStaticSamplerPolicy) {
      output.Facts.HasStaticSamplerPolicy = true;
      for (const string &name : dxilRootPolicy.StaticSamplerNames)
        if (std::find(output.Facts.StaticSamplerNames.begin(),
                      output.Facts.StaticSamplerNames.end(),
                      name) == output.Facts.StaticSamplerNames.end())
          output.Facts.StaticSamplerNames.push_back(name);
      for (MetadataBindingFact &binding : output.Facts.Bindings) {
        if (binding.Type !=
            static_cast<uint32_t>(MetadataBindingKind::Sampler))
          continue;
        if (dxilRootPolicy.StaticSamplerNames.empty() ||
            std::find(dxilRootPolicy.StaticSamplerNames.begin(),
                      dxilRootPolicy.StaticSamplerNames.end(),
                      binding.Name) != dxilRootPolicy.StaticSamplerNames.end())
          binding.Flags |= kMetadataImmutableSampler;
      }
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
  WireRange Bytecode;
  uint64_t ToolchainIdentity;
  uint8_t Contract[16];
  uint8_t BytecodeDigest[16];
  uint8_t PipelineLayoutDigest[16];
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
  uint32_t Flags;
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
  uint32_t RegisterSpace;
  uint32_t Register;
  uint32_t Offset;
  uint32_t Size;
  uint32_t StageMask;
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

static_assert(sizeof(WireEnvelope) == 136);
static_assert(sizeof(WireEntryRecord) == 24);
static_assert(sizeof(WireBindingRecord) == 32);
static_assert(sizeof(WireTypeRecord) == 40);
static_assert(sizeof(WireRootConstantRecord) == 24);
static_assert(sizeof(WireVertexInputRecord) == 28);

bool MergeMetadataFacts(const ContractData &contract,
                        RadRayDxcTarget target,
                        const vector<StageOutput> &stages,
                        MetadataFacts &facts,
                        vector<Diagnostic> &diagnostics) {
  facts = {};
  for (const StageOutput &stage : stages) {
    const MetadataFacts &source = stage.Facts;
    if (source.HasStaticSamplerPolicy) {
      facts.HasStaticSamplerPolicy = true;
      for (const string &name : source.StaticSamplerNames)
        if (std::find(facts.StaticSamplerNames.begin(),
                      facts.StaticSamplerNames.end(), name) ==
            facts.StaticSamplerNames.end())
          facts.StaticSamplerNames.push_back(name);
    }
    if (source.HasRootSignature) {
      if (facts.HasRootSignature &&
          !(facts.RootSignatureHash == source.RootSignatureHash)) {
        diagnostics.push_back(
            {2105, "graphics stages declare different RootSignature attributes"});
        return false;
      }
      facts.HasRootSignature = true;
      facts.RootSignatureHash = source.RootSignatureHash;
    }

    for (const MetadataRootBindingFact &rootBinding : source.RootBindings) {
      const auto found = std::find_if(
          facts.RootBindings.begin(), facts.RootBindings.end(),
          [&](const MetadataRootBindingFact &value) noexcept {
            return value.RegisterClass == rootBinding.RegisterClass &&
                   value.Group == rootBinding.Group &&
                   value.Binding == rootBinding.Binding;
          });
      if (found == facts.RootBindings.end())
        facts.RootBindings.push_back(rootBinding);
    }

    for (const MetadataBindingFact &binding : source.Bindings) {
      const auto found = std::find_if(
          facts.Bindings.begin(), facts.Bindings.end(),
          [&](const MetadataBindingFact &value) noexcept {
            return value.Name == binding.Name;
          });
      if (found == facts.Bindings.end()) {
        facts.Bindings.push_back(binding);
      } else if (found->Group != binding.Group ||
                 found->Binding != binding.Binding ||
                 found->Type != binding.Type ||
                 found->Count != binding.Count) {
        diagnostics.push_back({2109, "frontend stages disagree on a resource binding"});
        return false;
      } else {
        found->StageMask |= binding.StageMask;
        found->Flags |= binding.Flags;
      }
    }

    if (facts.Types.empty()) {
      facts.Types = source.Types;
    } else if (!source.Types.empty()) {
      if (facts.Types.size() != source.Types.size()) {
        diagnostics.push_back({2107, "frontend stages disagree on type metadata"});
        return false;
      }
      for (size_t index = 0; index < facts.Types.size(); ++index) {
        const MetadataTypeFact &left = facts.Types[index];
        const MetadataTypeFact &right = source.Types[index];
        if (left.Name != right.Name || left.ParentIndex != right.ParentIndex ||
            left.Kind != right.Kind || left.ElementCount != right.ElementCount ||
            left.Offset != right.Offset || left.Size != right.Size ||
            left.Stride != right.Stride || left.TypeIndex != right.TypeIndex) {
          diagnostics.push_back({2107, "frontend stages disagree on type metadata"});
          return false;
        }
      }
    }

    for (const MetadataRootConstantFact &constant : source.RootConstants) {
      const auto found = std::find_if(
          facts.RootConstants.begin(), facts.RootConstants.end(),
          [&](const MetadataRootConstantFact &value) noexcept {
            return value.RegisterSpace == constant.RegisterSpace &&
                   value.Register == constant.Register &&
                   value.Offset == constant.Offset && value.Size == constant.Size &&
                   value.Flags == constant.Flags;
          });
      if (found == facts.RootConstants.end())
        facts.RootConstants.push_back(constant);
      else
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
        diagnostics.push_back({2108, "frontend stages disagree on vertex input metadata"});
        return false;
      }
    }
  }

  if (target == RadRayDxcTarget::SPIRV && facts.RootConstants.size() > 1) {
    diagnostics.push_back(
        {2103, "SPIR-V source contains multiple push constant blocks"});
    return false;
  }
  if (target == RadRayDxcTarget::SPIRV && facts.HasStaticSamplerPolicy) {
    for (MetadataBindingFact &binding : facts.Bindings) {
      if (binding.Type !=
          static_cast<uint32_t>(MetadataBindingKind::Sampler))
        continue;
      if (facts.StaticSamplerNames.empty() ||
          std::find(facts.StaticSamplerNames.begin(),
                    facts.StaticSamplerNames.end(),
                    binding.Name) != facts.StaticSamplerNames.end())
        binding.Flags |= kMetadataImmutableSampler;
    }
  }
  if (target == RadRayDxcTarget::DXIL && facts.HasRootSignature) {
    const auto hasRootBinding = [](const MetadataRootBindingFact &rootBinding,
                                   const MetadataBindingFact &binding) noexcept {
      return rootBinding.RegisterClass == binding.RegisterClass &&
             rootBinding.Group == binding.Group &&
             rootBinding.Binding == binding.Binding;
    };
    const bool hasNonSamplerRootBinding = std::any_of(
        facts.RootBindings.begin(), facts.RootBindings.end(),
        [](const MetadataRootBindingFact &rootBinding) noexcept {
          return rootBinding.RegisterClass != 3u;
        });
    if (hasNonSamplerRootBinding) {
      for (const MetadataBindingFact &binding : facts.Bindings) {
        if (!binding.HasExplicitBinding &&
            binding.Type != static_cast<uint32_t>(MetadataBindingKind::Sampler))
          continue;
        const auto found = std::find_if(
            facts.RootBindings.begin(), facts.RootBindings.end(),
            [&](const MetadataRootBindingFact &rootBinding) noexcept {
              return hasRootBinding(rootBinding, binding);
            });
        if (found == facts.RootBindings.end()) {
          diagnostics.push_back(
              {2106, "DXIL RootSignature does not contain an active resource"});
          return false;
        }
      }
    }
    for (MetadataBindingFact &binding : facts.Bindings)
      if (binding.Type == static_cast<uint32_t>(MetadataBindingKind::Sampler) &&
          std::any_of(facts.RootBindings.begin(), facts.RootBindings.end(),
                      [&](const MetadataRootBindingFact &rootBinding) noexcept {
                        return hasRootBinding(rootBinding, binding);
                      }))
        binding.Flags |= kMetadataImmutableSampler;
    for (const MetadataRootBindingFact &rootBinding : facts.RootBindings) {
      const auto found = std::find_if(
          facts.Bindings.begin(), facts.Bindings.end(),
          [&](const MetadataBindingFact &binding) noexcept {
            return hasRootBinding(rootBinding, binding);
          });
      if (found == facts.Bindings.end()) {
        diagnostics.push_back(
            {2106, "DXIL RootSignature contains an inactive resource"});
        return false;
      }
    }
  }
  for (MetadataRootConstantFact &constant : facts.RootConstants)
    constant.StageMask &= 0x7u;
  if (target == RadRayDxcTarget::DXIL && facts.HasRootSignature) {
    uint32_t allStageMask = 0;
    for (const EntryPoint &entry : contract.EntryPoints)
      allStageMask |= MetadataStageBitFor(entry.Stage);
    for (MetadataRootConstantFact &constant : facts.RootConstants)
      constant.StageMask = allStageMask;
  }
  return true;
}

bool BuildMetadata(const CompileRequest &request, const ContractData &contract,
                   RadRayDxcTarget target, const vector<StageOutput> &stages,
                   vector<Diagnostic> &diagnostics, vector<uint8_t> &metadata) {
  MetadataFacts facts;
  if (!MergeMetadataFacts(contract, target, stages, facts, diagnostics))
    return false;

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
  const uint32_t nameOffset = vertexInputOffset + vertexInputBytes;
  uint32_t nameBytes = 0;
  for (const EntryPoint &entry : contract.EntryPoints)
    nameBytes += static_cast<uint32_t>(entry.Name.size());
  for (const MetadataBindingFact &binding : facts.Bindings)
    nameBytes += static_cast<uint32_t>(binding.Name.size());
  for (const MetadataTypeFact &type : facts.Types)
    nameBytes += static_cast<uint32_t>(type.Name.size());
  for (const MetadataVertexInputFact &input : facts.VertexInputs)
    nameBytes += static_cast<uint32_t>(input.Semantic.size());
  const uint32_t bytecodeOffset = nameOffset + nameBytes;
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
    record.Flags = fact.Flags;
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
  for (const MetadataRootConstantFact &fact : facts.RootConstants)
    rootConstants.push_back({fact.RegisterSpace, fact.Register, fact.Offset,
                             fact.Size, fact.StageMask, fact.Flags});

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

  envelope.TotalSize = bytecodeOffset + bytecodeSize;
  vector<uint8_t> layoutBytes;
  layoutBytes.reserve(bindingBytes + rootConstantBytes + vertexInputBytes + nameBytes);
  if (!bindings.empty()) {
    const auto *data = reinterpret_cast<const uint8_t *>(bindings.data());
    layoutBytes.insert(layoutBytes.end(), data, data + bindingBytes);
  }
  if (!rootConstants.empty()) {
    const auto *data = reinterpret_cast<const uint8_t *>(rootConstants.data());
    layoutBytes.insert(layoutBytes.end(), data, data + rootConstantBytes);
  }
  if (!vertexInputs.empty()) {
    const auto *data = reinterpret_cast<const uint8_t *>(vertexInputs.data());
    layoutBytes.insert(layoutBytes.end(), data, data + vertexInputBytes);
  }
  for (const EntryPoint &entry : contract.EntryPoints)
    layoutBytes.insert(layoutBytes.end(), entry.Name.begin(), entry.Name.end());
  for (const MetadataBindingFact &binding : facts.Bindings)
    layoutBytes.insert(layoutBytes.end(), binding.Name.begin(), binding.Name.end());
  for (const MetadataTypeFact &type : facts.Types)
    layoutBytes.insert(layoutBytes.end(), type.Name.begin(), type.Name.end());
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
  std::memcpy(envelope.PipelineLayoutDigest, pipelineHash.Bytes,
              sizeof(envelope.PipelineLayoutDigest));
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
  for (const MetadataVertexInputFact &input : facts.VertexInputs) {
    std::memcpy(metadata.data() + currentNameOffset, input.Semantic.data(),
                input.Semantic.size());
    currentNameOffset += static_cast<uint32_t>(input.Semantic.size());
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
        output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
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
