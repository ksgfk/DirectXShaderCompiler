///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxcradray.cpp                                                             //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//                                                                           //
// Implements the RadRay-owned DXC extension factory, wire parser, and       //
// compiler-owned result lifetime.                                           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/WinIncludes.h"
#include "dxc/dxcapi_radrayext.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/microcom.h"

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
using radray::shader::RadRayDxcLaneView;
using radray::shader::RadRayDxcTarget;

constexpr uint8_t kToolchainIdentity[16] = {
    0x07, 0x02, 0x09, 0x01, 0x72, 0x61, 0x64, 0x72,
    0x61, 0x79, 0x2d, 0x31, 0x2e, 0x39, 0x2e, 0x31};
constexpr uint64_t kMetadataToolchainIdentity = 0x0000000001090207ull;
constexpr uint32_t kMaxCollectionCount = 4096;
constexpr uint32_t kMaxEntryCount = 16;
constexpr uint32_t kMaxRecursionDepth = 64;

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

struct IncludeSource {
  string Name;
  vector<uint8_t> Bytes;
};

struct CompileRequest {
  string SourceName;
  vector<uint8_t> RootSource;
  vector<IncludeSource> Includes;
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
  uint32_t includeCount = 0;
  if (!reader.ReadU32(includeCount) || includeCount > kMaxCollectionCount) {
    error = "compile request include count is invalid";
    return false;
  }
  request.Includes.reserve(includeCount);
  for (uint32_t index = 0; index < includeCount; ++index) {
    IncludeSource include;
    if (!reader.ReadString(include.Name) || !reader.ReadBytes(include.Bytes) ||
        !IsLogicalSourceName(include.Name)) {
      error = "compile request include is invalid";
      return false;
    }
    request.Includes.push_back(std::move(include));
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

bool ReadDiscoveryRequest(RadRayDxcBlobView view, string &sourceName,
                          vector<uint8_t> &source, RadRayDxcTarget &target,
                          string &error) {
  WireReader reader{view};
  uint32_t magic = 0;
  uint16_t schema = 0;
  uint8_t targetValue = 0;
  if (!reader.ReadU32(magic) || !reader.ReadU16(schema) ||
      magic != radray::shader::kRadRayDxcDiscoveryWireMagic ||
      schema != radray::shader::kRadRayDxcDiscoveryWireSchemaVersion ||
      !reader.ReadString(sourceName) || !reader.ReadBytes(source) ||
      !reader.ReadU8(targetValue) || reader.Remaining() != 0 ||
      !IsLogicalSourceName(sourceName) || source.empty() || targetValue > 1) {
    error = "discovery request wire payload is invalid";
    return false;
  }
  target = static_cast<RadRayDxcTarget>(targetValue);
  return true;
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

struct Diagnostic {
  uint32_t Code{0};
  string Message;
};

string_view Trim(string_view value) noexcept {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
    value.remove_suffix(1);
  return value;
}

bool StartsWith(string_view value, string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(string_view value, string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

string CleanSource(string_view source) {
  string output{source};
  bool inBlockComment = false;
  bool inString = false;
  for (size_t index = 0; index < output.size(); ++index) {
    const char current = output[index];
    const char next = index + 1 < output.size() ? output[index + 1] : '\0';
    if (inBlockComment) {
      if (current == '*' && next == '/') {
        output[index] = ' ';
        output[index + 1] = ' ';
        ++index;
        inBlockComment = false;
      } else if (current != '\n' && current != '\r') {
        output[index] = ' ';
      }
      continue;
    }
    if (inString) {
      if (current == '\\' && index + 1 < output.size() && output[index + 1] != '\n')
        ++index;
      else if (current == '"')
        inString = false;
      continue;
    }
    if (current == '"') {
      inString = true;
    } else if (current == '/' && next == '/') {
      output[index] = ' ';
      ++index;
      while (index < output.size() && output[index] != '\n' && output[index] != '\r')
        output[index++] = ' ';
      if (index < output.size())
        --index;
    } else if (current == '/' && next == '*') {
      output[index] = ' ';
      output[index + 1] = ' ';
      ++index;
      inBlockComment = true;
    }
  }
  return output;
}

bool IsIdentifierStart(char value) noexcept {
  return std::isalpha(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool IsIdentifierChar(char value) noexcept {
  return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool IsConditionalAt(string_view source, size_t position) noexcept {
  uint32_t depth = 0;
  size_t lineStart = 0;
  while (lineStart < position) {
    const size_t lineEnd = source.find_first_of("\r\n", lineStart);
    const size_t end = lineEnd == string_view::npos ? source.size() : lineEnd;
    const string_view line = Trim(source.substr(lineStart, end - lineStart));
    if (StartsWith(line, "#if") && !StartsWith(line, "#endif"))
      ++depth;
    else if (StartsWith(line, "#endif") && depth != 0)
      --depth;
    if (lineEnd == string_view::npos)
      break;
    lineStart = lineEnd + 1;
    if (lineStart < source.size() && source[lineEnd] == '\r' && source[lineStart] == '\n')
      ++lineStart;
  }
  return depth != 0;
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

bool ParseStageAttributes(string_view source, vector<EntryPoint> &entries,
                          vector<Diagnostic> &diagnostics) {
  size_t cursor = 0;
  while ((cursor = source.find("[shader(\"", cursor)) != string_view::npos) {
    const size_t stageBegin = cursor + 9;
    const size_t stageEnd = source.find("\")]", stageBegin);
    if (stageEnd == string_view::npos) {
      diagnostics.push_back({8, "stage attribute is not closed"});
      return false;
    }
    if (IsConditionalAt(source, cursor)) {
      diagnostics.push_back({7, "stage entry is inside conditional compilation"});
      return false;
    }
    ShaderStage stage{};
    if (!ParseStage(source.substr(stageBegin, stageEnd - stageBegin), stage)) {
      diagnostics.push_back({9, "shader stage is not supported"});
      return false;
    }
    size_t functionSearch = stageEnd + 3;
    while (functionSearch < source.size() && std::isspace(static_cast<unsigned char>(source[functionSearch])) != 0)
      ++functionSearch;
    while (functionSearch < source.size() && source[functionSearch] == '[') {
      const size_t attributeEnd = source.find(']', functionSearch + 1);
      if (attributeEnd == string_view::npos) {
        diagnostics.push_back({8, "entry attribute is not closed"});
        return false;
      }
      functionSearch = attributeEnd + 1;
      while (functionSearch < source.size() && std::isspace(static_cast<unsigned char>(source[functionSearch])) != 0)
        ++functionSearch;
    }
    const size_t openParen = source.find('(', functionSearch);
    if (openParen == string_view::npos) {
      diagnostics.push_back({8, "stage attribute has no entry function"});
      return false;
    }
    size_t nameEnd = openParen;
    while (nameEnd > stageEnd + 2 && std::isspace(static_cast<unsigned char>(source[nameEnd - 1])) != 0)
      --nameEnd;
    size_t nameBegin = nameEnd;
    while (nameBegin > stageEnd + 2 && IsIdentifierChar(source[nameBegin - 1]))
      --nameBegin;
    if (nameBegin == nameEnd || !IsIdentifierStart(source[nameBegin])) {
      diagnostics.push_back({8, "stage entry function name is invalid"});
      return false;
    }
    entries.push_back({string{source.substr(nameBegin, nameEnd - nameBegin)}, stage});
    cursor = openParen + 1;
  }
  return true;
}

bool ParseKeywordPragma(string_view line, KeywordGroup &group) {
  constexpr string_view prefix = "#pragma radray_keyword_group";
  line = Trim(line);
  if (!StartsWith(line, prefix))
    return false;
  line = Trim(line.substr(prefix.size()));
  const size_t nameEnd = line.find_first_of(" \t");
  if (nameEnd == string_view::npos)
    return false;
  group.Name = string{line.substr(0, nameEnd)};
  line.remove_prefix(nameEnd);
  while (!(line = Trim(line)).empty()) {
    if (line.front() != '"')
      return false;
    line.remove_prefix(1);
    const size_t valueEnd = line.find('"');
    if (valueEnd == string_view::npos || valueEnd == 0)
      return false;
    group.Values.emplace_back(line.substr(0, valueEnd));
    line.remove_prefix(valueEnd + 1);
  }
  return !group.Name.empty() && !group.Values.empty();
}

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

bool DiscoverContract(string_view sourceName, const vector<uint8_t> &source,
                      ContractData &contract, vector<Diagnostic> &diagnostics) {
  if (!IsLogicalSourceName(sourceName) || source.empty()) {
    diagnostics.push_back({1, "source name or source bytes are invalid"});
    return false;
  }
  const string text(reinterpret_cast<const char *>(source.data()), source.size());
  const string cleaned = CleanSource(text);
  size_t lineStart = 0;
  while (lineStart <= cleaned.size()) {
    const size_t lineEnd = cleaned.find_first_of("\r\n", lineStart);
    const size_t end = lineEnd == string::npos ? cleaned.size() : lineEnd;
    const string_view line = string_view{cleaned}.substr(lineStart, end - lineStart);
    if (line.find("#pragma radray_keyword_group") != string_view::npos) {
      if (EndsWith(sourceName, ".hlsli") || IsConditionalAt(cleaned, lineStart)) {
        diagnostics.push_back({6, "keyword group pragma must be in the root source outside conditions"});
        return false;
      }
      KeywordGroup group;
      if (!ParseKeywordPragma(line, group)) {
        diagnostics.push_back({3, "keyword group pragma is malformed"});
        return false;
      }
      std::sort(group.Values.begin(), group.Values.end());
      for (const KeywordGroup &existing : contract.KeywordGroups) {
        if (existing.Name == group.Name) {
          diagnostics.push_back({4, "keyword group is declared more than once"});
          return false;
        }
        for (const string &left : existing.Values) {
          for (const string &right : group.Values) {
            if (left == right) {
              diagnostics.push_back({15, "keyword value is repeated across groups"});
              return false;
            }
          }
        }
      }
      for (size_t index = 1; index < group.Values.size(); ++index) {
        if (group.Values[index - 1] == group.Values[index]) {
          diagnostics.push_back({5, "keyword value is declared more than once"});
          return false;
        }
      }
      contract.KeywordGroups.push_back(std::move(group));
    }
    if (lineEnd == string::npos)
      break;
    lineStart = lineEnd + 1;
    if (lineStart < cleaned.size() && cleaned[lineEnd] == '\r' && cleaned[lineStart] == '\n')
      ++lineStart;
  }
  if (!ParseStageAttributes(cleaned, contract.EntryPoints, diagnostics))
    return false;
  uint32_t vertexCount = 0;
  uint32_t pixelCount = 0;
  uint32_t computeCount = 0;
  for (const EntryPoint &entry : contract.EntryPoints) {
    vertexCount += entry.Stage == ShaderStage::Vertex;
    pixelCount += entry.Stage == ShaderStage::Pixel;
    computeCount += entry.Stage == ShaderStage::Compute;
  }
  if (computeCount != 0 && (vertexCount != 0 || pixelCount != 0)) {
    diagnostics.push_back({14, "graphics and compute entries cannot share a source unit"});
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

bool IsMacroEnabled(const CompileRequest &request, RadRayDxcTarget target,
                    string_view name) noexcept {
  if (target == RadRayDxcTarget::SPIRV && name == "__spirv__")
    return true;
  for (const NameValue &define : request.Defines) {
    if (define.Name == name)
      return define.Value != "0";
  }
  for (const NameValue &assignment : request.Assignments) {
    if (assignment.Name == name)
      return assignment.Value != "0";
  }
  return false;
}

bool EvaluateCondition(string_view line, const CompileRequest &request,
                       RadRayDxcTarget target, bool &value) noexcept {
  line = Trim(line);
  if (StartsWith(line, "#ifdef")) {
    const string_view name = Trim(line.substr(6));
    value = IsMacroEnabled(request, target, name);
    return !name.empty();
  }
  if (StartsWith(line, "#ifndef")) {
    const string_view name = Trim(line.substr(7));
    value = !IsMacroEnabled(request, target, name);
    return !name.empty();
  }
  if (StartsWith(line, "#if !defined(")) {
    const size_t begin = line.find('(') + 1;
    const size_t end = line.find(')', begin);
    if (begin == 0 || end == string_view::npos)
      return false;
    value = !IsMacroEnabled(request, target, Trim(line.substr(begin, end - begin)));
    return true;
  }
  if (StartsWith(line, "#if defined(")) {
    const size_t begin = line.find('(') + 1;
    const size_t end = line.find(')', begin);
    if (begin == 0 || end == string_view::npos)
      return false;
    value = IsMacroEnabled(request, target, Trim(line.substr(begin, end - begin)));
    return true;
  }
  return false;
}

string NormalizeInclude(string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  if (value.size() >= 2 && value.front() == '<' && value.back() == '>')
    value = value.substr(1, value.size() - 2);
  while (StartsWith(value, "./"))
    value.erase(0, 2);
  return value;
}

bool ExpandSourceInternal(string_view source, const CompileRequest &request,
                          RadRayDxcTarget target, string &output,
                          vector<IncludeSource> &opened, uint32_t depth) {
  if (depth > kMaxRecursionDepth)
    return false;
  struct ConditionFrame {
    bool ParentActive{true};
    bool Condition{true};
    bool ElseSeen{false};
  };
  vector<ConditionFrame> conditions;
  bool active = true;
  size_t lineStart = 0;
  while (lineStart <= source.size()) {
    const size_t lineEnd = source.find_first_of("\r\n", lineStart);
    const size_t end = lineEnd == string_view::npos ? source.size() : lineEnd;
    const string_view line = source.substr(lineStart, end - lineStart);
    const string_view trimmed = Trim(line);
    if (StartsWith(trimmed, "#if ") || StartsWith(trimmed, "#ifdef") ||
        StartsWith(trimmed, "#ifndef")) {
      bool condition = true;
      EvaluateCondition(trimmed, request, target, condition);
      conditions.push_back({active, condition, false});
      active = active && condition;
      output.append(line);
    } else if (StartsWith(trimmed, "#else")) {
      if (conditions.empty() || conditions.back().ElseSeen)
        return false;
      ConditionFrame &frame = conditions.back();
      frame.ElseSeen = true;
      active = frame.ParentActive && !frame.Condition;
      output.append(line);
    } else if (StartsWith(trimmed, "#endif")) {
      if (conditions.empty())
        return false;
      const ConditionFrame frame = conditions.back();
      conditions.pop_back();
      active = frame.ParentActive;
      output.append(line);
    } else if (active && StartsWith(trimmed, "#include")) {
      const size_t begin = trimmed.find_first_of("<\"");
      if (begin == string_view::npos)
        return false;
      const char closing = trimmed[begin] == '<' ? '>' : '"';
      const size_t endName = trimmed.find(closing, begin + 1);
      if (endName == string_view::npos)
        return false;
      IncludeSource *found = nullptr;
      const string logicalName = NormalizeInclude(string{trimmed.substr(begin + 1, endName - begin - 1)});
      for (IncludeSource &include : const_cast<vector<IncludeSource> &>(request.Includes)) {
        if (include.Name == logicalName) {
          found = &include;
          break;
        }
      }
      if (found == nullptr)
        return false;
      opened.push_back(*found);
      string expanded;
      if (!ExpandSourceInternal(
              string_view{reinterpret_cast<const char *>(found->Bytes.data()), found->Bytes.size()},
              request, target, expanded, opened, depth + 1)) {
        return false;
      }
      output.append(expanded);
    } else {
      output.append(line);
    }
    if (lineEnd == string_view::npos)
      break;
    output.push_back('\n');
    lineStart = lineEnd + 1;
    if (lineStart < source.size() && source[lineEnd] == '\r' && source[lineStart] == '\n')
      ++lineStart;
  }
  return conditions.empty();
}

bool ExpandSource(const CompileRequest &request, RadRayDxcTarget target,
                  string &output, vector<IncludeSource> &opened) {
  return ExpandSourceInternal(
      string_view{reinterpret_cast<const char *>(request.RootSource.data()), request.RootSource.size()},
      request, target, output, opened, 0);
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

wstring ToWide(string_view value) {
  wstring result;
  result.reserve(value.size());
  for (const char character : value)
    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
  return result;
}

struct StageOutput {
  vector<uint8_t> Bytecode;
};

bool CompileStage(const CompileRequest &request, RadRayDxcTarget target,
                  const EntryPoint &entry, string_view expandedSource,
                  StageOutput &output, vector<Diagnostic> &diagnostics) {
  CComPtr<IDxcCompiler3> compiler;
  if (FAILED(CreateDxcCompiler(IID_PPV_ARGS(&compiler)))) {
    diagnostics.push_back({2002, "fork DXC compiler instance creation failed"});
    return false;
  }
  vector<wstring> argumentStorage;
  argumentStorage.emplace_back(L"-E");
  argumentStorage.emplace_back(ToWide(entry.Name));
  argumentStorage.emplace_back(L"-T");
  argumentStorage.emplace_back(ProfileForStage(entry.Stage));
  argumentStorage.emplace_back(L"-HV");
  argumentStorage.emplace_back(ToWide(std::to_string(request.HlslVersion)));
  argumentStorage.emplace_back(request.Optimize != 0 ? L"-O3" : L"-Od");
  if (request.DebugInfo != 0)
    argumentStorage.emplace_back(L"-Zi");
  if (request.AllResourcesBound != 0)
    argumentStorage.emplace_back(L"-all_resources_bound");
  if (target == RadRayDxcTarget::SPIRV) {
    argumentStorage.emplace_back(L"-spirv");
    argumentStorage.emplace_back(L"-fspv-target-env=vulkan1.2");
  }
  for (const NameValue &define : request.Defines)
    argumentStorage.emplace_back(ToWide("-D" + define.Name + "=" + define.Value));
  for (const NameValue &assignment : request.Assignments)
    argumentStorage.emplace_back(ToWide("-D" + assignment.Name + "=" + assignment.Value));
  vector<LPCWSTR> arguments;
  arguments.reserve(argumentStorage.size());
  for (const wstring &argument : argumentStorage)
    arguments.push_back(argument.c_str());

  DxcBuffer source{};
  source.Ptr = expandedSource.data();
  source.Size = expandedSource.size();
  source.Encoding = DXC_CP_UTF8;
  CComPtr<IDxcResult> result;
  if (FAILED(compiler->Compile(
          &source, arguments.data(), static_cast<uint32_t>(arguments.size()), nullptr,
          IID_PPV_ARGS(&result)))) {
    diagnostics.push_back({2003, "fork DXC Compile call failed"});
    return false;
  }
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
    CComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors != nullptr && errors->GetStringLength() != 0) {
      const char *text = static_cast<const char *>(errors->GetBufferPointer());
      diagnostics.push_back({2004, string{text, text + errors->GetStringLength()}});
    } else {
      diagnostics.push_back({2004, "fork DXC rejected the typed compile request"});
    }
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
  WireRange Bytecode;
  uint64_t ToolchainIdentity;
  uint8_t Contract[16];
  uint8_t CompileInput[16];
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
#pragma pack(pop)

static_assert(sizeof(WireEnvelope) == 144);
static_assert(sizeof(WireEntryRecord) == 24);

vector<uint8_t> EncodeCompileInput(const CompileRequest &request,
                                   RadRayDxcTarget target,
                                   const vector<IncludeSource> &opened) {
  vector<uint8_t> bytes;
  AppendU32(bytes, radray::shader::kRadRayDxcShaderWireMagic);
  AppendU16(bytes, radray::shader::kRadRayDxcShaderWireSchemaVersion);
  AppendString(bytes, request.SourceName);
  AppendU32(bytes, static_cast<uint32_t>(request.RootSource.size()));
  AppendBytes(bytes, request.RootSource);
  AppendU32(bytes, static_cast<uint32_t>(opened.size()));
  for (const IncludeSource &include : opened) {
    AppendString(bytes, include.Name);
    AppendU32(bytes, static_cast<uint32_t>(include.Bytes.size()));
    AppendBytes(bytes, include.Bytes);
  }
  AppendByte(bytes, target == RadRayDxcTarget::DXIL ? 1 : 2);
  AppendU32(bytes, request.ShaderModel);
  AppendByte(bytes, request.Optimize);
  AppendByte(bytes, request.DebugInfo);
  AppendByte(bytes, request.AllResourcesBound);
  AppendByte(bytes, request.WarningPolicy);
  AppendU32(bytes, request.SpirvTargetEnv);
  AppendU32(bytes, request.HlslVersion);
  AppendU32(bytes, request.Reserved);
  bytes.insert(bytes.end(), 16, 0);
  AppendU32(bytes, static_cast<uint32_t>(request.Defines.size()));
  for (const NameValue &define : request.Defines) {
    AppendString(bytes, define.Name);
    AppendString(bytes, define.Value);
  }
  AppendU32(bytes, static_cast<uint32_t>(request.Assignments.size()));
  for (const NameValue &assignment : request.Assignments) {
    AppendString(bytes, assignment.Name);
    AppendString(bytes, assignment.Value);
  }
  bytes.insert(bytes.end(), kToolchainIdentity,
               kToolchainIdentity + sizeof(kToolchainIdentity));
  return bytes;
}

vector<uint8_t> BuildMetadata(const CompileRequest &request,
                              const ContractData &contract,
                              RadRayDxcTarget target,
                              const vector<StageOutput> &stages,
                              const Hash128 &compileInput) {
  const uint32_t entryOffset = sizeof(WireEnvelope);
  const uint32_t entryBytes = static_cast<uint32_t>(
      contract.EntryPoints.size() * sizeof(WireEntryRecord));
  const uint32_t nameOffset = entryOffset + entryBytes;
  uint32_t nameBytes = 0;
  for (const EntryPoint &entry : contract.EntryPoints)
    nameBytes += static_cast<uint32_t>(entry.Name.size());
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
  envelope.BindingRecords = {0, 0};
  envelope.TypeRecords = {0, 0};
  envelope.RootConstantRecords = {0, 0};
  envelope.Bytecode = {bytecodeOffset, bytecodeSize};
  envelope.ToolchainIdentity = kMetadataToolchainIdentity;
  std::memcpy(envelope.Contract, contract.Hash.Bytes, sizeof(envelope.Contract));
  std::memcpy(envelope.CompileInput, compileInput.Bytes, sizeof(envelope.CompileInput));

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
  }

  vector<uint8_t> layoutBytes;
  layoutBytes.reserve(entryBytes + nameBytes);
  const auto *entryData = reinterpret_cast<const uint8_t *>(entries.data());
  layoutBytes.insert(layoutBytes.end(), entryData, entryData + entryBytes);
  for (const EntryPoint &entry : contract.EntryPoints)
    layoutBytes.insert(layoutBytes.end(), entry.Name.begin(), entry.Name.end());
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

  vector<uint8_t> metadata(envelope.TotalSize, 0);
  std::memcpy(metadata.data(), &envelope, sizeof(envelope));
  if (!entries.empty())
    std::memcpy(metadata.data() + entryOffset, entries.data(), entryBytes);
  currentNameOffset = nameOffset;
  for (const EntryPoint &entry : contract.EntryPoints) {
    std::memcpy(metadata.data() + currentNameOffset, entry.Name.data(), entry.Name.size());
    currentNameOffset += static_cast<uint32_t>(entry.Name.size());
  }
  uint32_t currentBytecodeOffset = bytecodeOffset;
  for (const StageOutput &stage : stages) {
    std::memcpy(metadata.data() + currentBytecodeOffset, stage.Bytecode.data(), stage.Bytecode.size());
    currentBytecodeOffset += static_cast<uint32_t>(stage.Bytecode.size());
  }
  return metadata;
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
      RadRayDxcBlobView request, IRadRayDxcResult **result) override {
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
    string sourceName;
    vector<uint8_t> source;
    RadRayDxcTarget target{};
    string error;
    ContractData contract;
    vector<Diagnostic> diagnostics;
    if (!ReadDiscoveryRequest(request, sourceName, source, target, error) ||
        !DiscoverContract(sourceName, source, contract, diagnostics)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      if (!error.empty())
        output->AddDiagnostic(2000, std::move(error));
      for (Diagnostic &diagnostic : diagnostics)
        output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
      return PublishResult(output, result);
    }
    output->SetStatus(RadRayDxcCompileStatus::Success);
    output->SetContract(EncodeContract(contract));
    return PublishResult(output, result);
  }

  HRESULT STDMETHODCALLTYPE CompileVariant(
      RadRayDxcBlobView request, IRadRayDxcResult **result) override {
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
    ContractData contract;
    vector<Diagnostic> discoveryDiagnostics;
    if (!DiscoverContract(parsed.SourceName, parsed.RootSource, contract,
                          discoveryDiagnostics)) {
      output->SetStatus(RadRayDxcCompileStatus::InvalidRequest);
      for (Diagnostic &diagnostic : discoveryDiagnostics)
        output->AddDiagnostic(diagnostic.Code, std::move(diagnostic.Message));
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

    for (const RadRayDxcTarget target : {RadRayDxcTarget::DXIL, RadRayDxcTarget::SPIRV}) {
      const uint8_t bit = target == RadRayDxcTarget::DXIL ? 1 : 2;
      if ((parsed.Targets & bit) == 0)
        continue;
      string expanded;
      vector<IncludeSource> opened;
      if (!ExpandSource(parsed, target, expanded, opened)) {
        output->SetStatus(RadRayDxcCompileStatus::TargetFailure);
        output->ClearLanes();
        output->AddDiagnostic(2009, "typed include expansion failed");
        return PublishResult(output, result);
      }
      vector<StageOutput> stages;
      vector<Diagnostic> compileDiagnostics;
      bool success = true;
      for (const EntryPoint &entry : contract.EntryPoints) {
        StageOutput stage;
        if (!CompileStage(parsed, target, entry, expanded, stage, compileDiagnostics)) {
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
      const vector<uint8_t> inputBytes = EncodeCompileInput(parsed, target, opened);
      const Hash128 inputHash = Digest(inputBytes, 0x434f4d50494c45ull + bit);
      vector<uint8_t> bytecode;
      for (const StageOutput &stage : stages)
        AppendBytes(bytecode, stage.Bytecode);
      vector<uint8_t> metadata = BuildMetadata(parsed, contract, target, stages, inputHash);
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
