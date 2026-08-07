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

struct MetadataStageSpan {
  ShaderStage Stage{ShaderStage::Vertex};
  string_view EntryName;
  size_t Begin{0};
  size_t End{0};
};

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
};

struct MetadataStructDecl {
  string Name;
  string Body;
};

struct MetadataFieldDecl {
  string Type;
  string Name;
  uint32_t ArrayCount{1};
};

struct MetadataLayoutInfo {
  uint32_t Size{0};
  uint32_t Align{4};
};

size_t MetadataSkipSpace(string_view text, size_t position) noexcept {
  while (position < text.size() &&
         std::isspace(static_cast<unsigned char>(text[position])) != 0)
    ++position;
  return position;
}

bool MetadataParseUnsigned(string_view text, size_t &position,
                           uint32_t &value) noexcept {
  position = MetadataSkipSpace(text, position);
  const size_t begin = position;
  uint64_t parsed = 0;
  while (position < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[position])) != 0) {
    parsed = parsed * 10 + static_cast<uint32_t>(text[position] - '0');
    if (parsed > std::numeric_limits<uint32_t>::max())
      return false;
    ++position;
  }
  if (position == begin)
    return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool MetadataParseBindingArguments(string_view text, size_t begin,
                                   uint32_t &binding,
                                   uint32_t &group) noexcept {
  const size_t open = text.find('(', begin);
  if (open == string_view::npos)
    return false;
  size_t cursor = open + 1;
  if (!MetadataParseUnsigned(text, cursor, binding))
    return false;
  cursor = MetadataSkipSpace(text, cursor);
  if (cursor >= text.size() || text[cursor] != ',')
    return false;
  ++cursor;
  return MetadataParseUnsigned(text, cursor, group);
}

bool MetadataParseRegister(string_view text, uint32_t &binding,
                           uint32_t &group) noexcept {
  const size_t registerPosition = text.find("register(");
  if (registerPosition == string_view::npos)
    return false;
  size_t cursor = registerPosition + 9;
  cursor = MetadataSkipSpace(text, cursor);
  if (cursor >= text.size() || (text[cursor] != 'b' && text[cursor] != 't' &&
                                text[cursor] != 's' && text[cursor] != 'u'))
    return false;
  ++cursor;
  if (!MetadataParseUnsigned(text, cursor, binding))
    return false;
  group = 0;
  cursor = MetadataSkipSpace(text, cursor);
  if (cursor < text.size() && text[cursor] == ',') {
    ++cursor;
    const size_t spacePosition = text.find("space", cursor);
    if (spacePosition == string_view::npos)
      return false;
    cursor = spacePosition + 5;
    cursor = MetadataSkipSpace(text, cursor);
    if (cursor < text.size() && text[cursor] == '=')
      ++cursor;
    if (!MetadataParseUnsigned(text, cursor, group))
      return false;
  }
  return true;
}

bool MetadataParseTargetBinding(string_view text, RadRayDxcTarget target,
                               uint32_t &binding, uint32_t &group) noexcept {
  if (target == RadRayDxcTarget::SPIRV) {
    const size_t macro = text.find("VK_BINDING(");
    const size_t attribute = text.find("vk::binding(");
    if (macro != string_view::npos)
      return MetadataParseBindingArguments(text, macro, binding, group);
    if (attribute != string_view::npos)
      return MetadataParseBindingArguments(text, attribute, binding, group);
  }
  return MetadataParseRegister(text, binding, group);
}

string MetadataReadIdentifier(string_view text, size_t &position) {
  position = MetadataSkipSpace(text, position);
  if (position >= text.size() || !IsIdentifierStart(text[position]))
    return {};
  const size_t begin = position++;
  while (position < text.size() && IsIdentifierChar(text[position]))
    ++position;
  return string{text.substr(begin, position - begin)};
}

bool MetadataIsWordAt(string_view text, size_t position,
                      string_view word) noexcept {
  if (position > text.size() || text.substr(position, word.size()) != word)
    return false;
  const bool left = position == 0 || !IsIdentifierChar(text[position - 1]);
  const size_t end = position + word.size();
  const bool right = end >= text.size() || !IsIdentifierChar(text[end]);
  return left && right;
}

uint32_t MetadataStageBit(ShaderStage stage) noexcept {
  return 1u << static_cast<uint8_t>(stage);
}

const char *MetadataStageName(ShaderStage stage) noexcept {
  switch (stage) {
  case ShaderStage::Vertex:
    return "vertex";
  case ShaderStage::Pixel:
    return "pixel";
  case ShaderStage::Compute:
    return "compute";
  }
  return "";
}

vector<MetadataStageSpan> FindMetadataStageSpans(
    string_view source, const ContractData &contract) {
  vector<MetadataStageSpan> result;
  for (const EntryPoint &entry : contract.EntryPoints) {
    const string marker = string{"[shader(\""} +
                           MetadataStageName(entry.Stage) + "\")]";
    const size_t begin = source.find(marker);
    if (begin != string_view::npos)
      result.push_back({entry.Stage, entry.Name, begin, source.size()});
  }
  std::sort(result.begin(), result.end(),
            [](const MetadataStageSpan &lhs,
               const MetadataStageSpan &rhs) noexcept {
              return lhs.Begin < rhs.Begin;
            });
  for (size_t index = 1; index < result.size(); ++index)
    result[index - 1].End = result[index].Begin;
  return result;
}

string FindMetadataRootSignature(string_view source, size_t stageBegin) {
  constexpr string_view prefix = "[RootSignature(\"";
  size_t cursor = stageBegin;
  while (cursor > 0) {
    while (cursor > 0 &&
           std::isspace(static_cast<unsigned char>(source[cursor - 1])) != 0)
      --cursor;
    if (cursor == 0 || source[cursor - 1] != ']')
      break;
    const size_t open = source.rfind('[', cursor - 1);
    if (open == string_view::npos)
      break;
    const string_view attribute = Trim(source.substr(open, cursor - open));
    if (StartsWith(attribute, prefix) && EndsWith(attribute, "\")]" ) &&
        attribute.size() >= prefix.size() + 3)
      return string{attribute.substr(prefix.size(),
                                     attribute.size() - prefix.size() - 3)};
    cursor = open;
  }
  return {};
}

bool ParseMetadataRootBinding(string_view signature, size_t begin,
                              string_view token,
                              MetadataRootBindingFact &result) noexcept {
  size_t cursor = begin + token.size();
  cursor = MetadataSkipSpace(signature, cursor);
  if (cursor >= signature.size() ||
      (signature[cursor] != 'b' && signature[cursor] != 't' &&
       signature[cursor] != 'u' && signature[cursor] != 's'))
    return false;
  switch (signature[cursor]) {
  case 'b':
    result.RegisterClass = 0;
    break;
  case 't':
    result.RegisterClass = 1;
    break;
  case 'u':
    result.RegisterClass = 2;
    break;
  case 's':
    result.RegisterClass = 3;
    break;
  default:
    return false;
  }
  ++cursor;
  if (!MetadataParseUnsigned(signature, cursor, result.Binding))
    return false;

  const size_t close = signature.find(')', cursor);
  if (close == string_view::npos)
    return false;
  const size_t space = signature.find("space", cursor);
  if (space != string_view::npos && space < close) {
    cursor = space + 5;
    cursor = MetadataSkipSpace(signature, cursor);
    if (cursor < close && signature[cursor] == '=')
      ++cursor;
    if (!MetadataParseUnsigned(signature, cursor, result.Group))
      return false;
  }
  return true;
}

vector<MetadataRootBindingFact> ParseMetadataRootBindings(
    string_view signature) {
  vector<MetadataRootBindingFact> result;
  constexpr string_view tokens[] = {
      "CBV(", "SRV(", "UAV(", "Sampler(", "StaticSampler("};
  for (const string_view token : tokens) {
    size_t cursor = 0;
    while ((cursor = signature.find(token, cursor)) != string_view::npos) {
      MetadataRootBindingFact binding;
      if (!ParseMetadataRootBinding(signature, cursor, token, binding))
        return {};
      result.push_back(binding);
      cursor += token.size();
    }
  }
  return result;
}

bool ValidateMetadataRootBindings(
    string_view signature, const vector<MetadataBindingFact> &bindings,
    vector<Diagnostic> &diagnostics) {
  const vector<MetadataRootBindingFact> rootBindings =
      ParseMetadataRootBindings(signature);
  constexpr string_view tokens[] = {
      "CBV(", "SRV(", "UAV(", "Sampler(", "StaticSampler("};
  bool hasResourceToken = false;
  for (const string_view token : tokens) {
    if (signature.find(token) != string_view::npos) {
      hasResourceToken = true;
      break;
    }
  }
  if (!hasResourceToken)
    return bindings.empty();
  if (rootBindings.empty() && !signature.empty()) {
    diagnostics.push_back(
        {2106, "DXIL RootSignature contains no parseable resource bindings"});
    return false;
  }

  for (const MetadataBindingFact &binding : bindings) {
    const auto found = std::find_if(
        rootBindings.begin(), rootBindings.end(),
        [&](const MetadataRootBindingFact &root) noexcept {
          return root.RegisterClass == binding.RegisterClass &&
                 root.Binding == binding.Binding && root.Group == binding.Group;
        });
    if (found == rootBindings.end()) {
      diagnostics.push_back(
          {2106, "DXIL RootSignature does not contain an active resource"});
      return false;
    }
  }
  for (const MetadataRootBindingFact &root : rootBindings) {
    const auto found = std::find_if(
        bindings.begin(), bindings.end(),
        [&](const MetadataBindingFact &binding) noexcept {
          return root.RegisterClass == binding.RegisterClass &&
                 root.Binding == binding.Binding && root.Group == binding.Group;
        });
    if (found == bindings.end()) {
      diagnostics.push_back(
          {2106, "DXIL RootSignature contains an inactive resource"});
      return false;
    }
  }
  return true;
}

bool ParseMetadataResourceLine(string_view line, RadRayDxcTarget target,
                               MetadataBindingFact &result) {
  line = Trim(line);
  if (line.empty() || line.front() == '#' || StartsWith(line, "struct ") ||
      line.front() == '[')
    return false;

  string_view typeToken;
  MetadataBindingKind kind = MetadataBindingKind::Texture;
  constexpr string_view tokens[] = {
      "RWStructuredBuffer", "StructuredBuffer", "RWByteAddressBuffer",
      "ByteAddressBuffer",  "RWTexture",       "Texture",
      "SamplerState",       "SamplerComparisonState", "ConstantBuffer",
      "cbuffer"};
  constexpr MetadataBindingKind kinds[] = {
      MetadataBindingKind::RWBuffer, MetadataBindingKind::Buffer,
      MetadataBindingKind::RWBuffer, MetadataBindingKind::Buffer,
      MetadataBindingKind::RWTexture, MetadataBindingKind::Texture,
      MetadataBindingKind::Sampler, MetadataBindingKind::Sampler,
      MetadataBindingKind::CBuffer, MetadataBindingKind::CBuffer};
  size_t typePosition = string_view::npos;
  for (size_t index = 0; index < std::size(tokens); ++index) {
    const size_t candidate = line.find(tokens[index]);
    if (candidate != string_view::npos &&
        (typePosition == string_view::npos || candidate < typePosition)) {
      typePosition = candidate;
      typeToken = tokens[index];
      kind = kinds[index];
    }
  }
  if (typePosition == string_view::npos)
    return false;

  size_t namePosition = typePosition + typeToken.size();
  const size_t templateEnd = line.find('>', namePosition);
  if (templateEnd != string_view::npos &&
      line.find('<', namePosition) < templateEnd)
    namePosition = templateEnd + 1;
  result.Name = MetadataReadIdentifier(line, namePosition);
  if (result.Name.empty())
    return false;
  namePosition = MetadataSkipSpace(line, namePosition);
  if (namePosition < line.size() && line[namePosition] == '[') {
    ++namePosition;
    if (!MetadataParseUnsigned(line, namePosition, result.Count) ||
        line.find(']', namePosition) == string_view::npos)
      return false;
  }
  result.Type = static_cast<uint32_t>(kind);
  switch (kind) {
  case MetadataBindingKind::CBuffer:
    result.RegisterClass = 0;
    break;
  case MetadataBindingKind::Buffer:
  case MetadataBindingKind::Texture:
    result.RegisterClass = 1;
    break;
  case MetadataBindingKind::RWBuffer:
  case MetadataBindingKind::RWTexture:
    result.RegisterClass = 2;
    break;
  case MetadataBindingKind::Sampler:
    result.RegisterClass = 3;
    break;
  }
  result.HasExplicitBinding = MetadataParseTargetBinding(
      line, target, result.Binding, result.Group);
  return true;
}

bool MetadataUsesIdentifier(string_view text, string_view name) noexcept {
  size_t cursor = 0;
  while ((cursor = text.find(name, cursor)) != string_view::npos) {
    const bool left = cursor == 0 || !IsIdentifierChar(text[cursor - 1]);
    const size_t end = cursor + name.size();
    const bool right = end >= text.size() || !IsIdentifierChar(text[end]);
    if (left && right)
      return true;
    cursor = end;
  }
  return false;
}

vector<MetadataStructDecl> ParseMetadataStructs(string_view source) {
  vector<MetadataStructDecl> result;
  size_t cursor = 0;
  while ((cursor = source.find("struct", cursor)) != string_view::npos) {
    if (!MetadataIsWordAt(source, cursor, "struct")) {
      ++cursor;
      continue;
    }
    size_t namePosition = cursor + 6;
    string name = MetadataReadIdentifier(source, namePosition);
    const size_t open = source.find('{', namePosition);
    if (name.empty() || open == string_view::npos) {
      cursor += 6;
      continue;
    }
    uint32_t depth = 1;
    size_t close = open + 1;
    for (; close < source.size() && depth != 0; ++close) {
      if (source[close] == '{')
        ++depth;
      else if (source[close] == '}')
        --depth;
    }
    if (depth != 0)
      break;
    result.push_back({std::move(name), string{source.substr(open + 1,
                                                             close - open - 2)}});
    cursor = close;
  }
  return result;
}

vector<MetadataFieldDecl> ParseMetadataFields(string_view body) {
  vector<MetadataFieldDecl> result;
  size_t begin = 0;
  while (begin < body.size()) {
    const size_t end = body.find(';', begin);
    const string_view statement = Trim(body.substr(
        begin, end == string_view::npos ? body.size() - begin : end - begin));
    begin = end == string_view::npos ? body.size() : end + 1;
    if (statement.empty() || statement.find('{') != string_view::npos)
      continue;
    size_t cursor = 0;
    string type = MetadataReadIdentifier(statement, cursor);
    string name = MetadataReadIdentifier(statement, cursor);
    if (type.empty() || name.empty())
      continue;
    uint32_t arrayCount = 1;
    cursor = MetadataSkipSpace(statement, cursor);
    if (cursor < statement.size() && statement[cursor] == '[') {
      ++cursor;
      if (!MetadataParseUnsigned(statement, cursor, arrayCount))
        continue;
    }
    result.push_back({std::move(type), std::move(name), arrayCount});
  }
  return result;
}

struct MetadataVertexFieldDecl {
  string Type;
  string Semantic;
  uint32_t SemanticIndex{0};
};

bool ParseMetadataSemantic(string_view text, string &semantic,
                           uint32_t &semanticIndex) noexcept {
  size_t cursor = 0;
  semantic = MetadataReadIdentifier(Trim(text), cursor);
  if (semantic.empty())
    return false;
  size_t suffix = semantic.size();
  while (suffix != 0 &&
         std::isdigit(static_cast<unsigned char>(semantic[suffix - 1])) != 0)
    --suffix;
  semanticIndex = 0;
  for (size_t index = suffix; index < semantic.size(); ++index) {
    semanticIndex = semanticIndex * 10u +
                    static_cast<uint32_t>(semantic[index] - '0');
  }
  semantic.resize(suffix);
  return !semantic.empty();
}

vector<MetadataVertexFieldDecl> ParseMetadataVertexFields(string_view body) {
  vector<MetadataVertexFieldDecl> result;
  size_t begin = 0;
  while (begin < body.size()) {
    const size_t end = body.find(';', begin);
    const string_view statement = Trim(body.substr(
        begin, end == string_view::npos ? body.size() - begin : end - begin));
    begin = end == string_view::npos ? body.size() : end + 1;
    const size_t colon = statement.find(':');
    if (statement.empty() || colon == string_view::npos)
      continue;
    size_t cursor = 0;
    const string type = MetadataReadIdentifier(statement.substr(0, colon), cursor);
    if (type.empty())
      continue;
    const string semanticText = string{Trim(statement.substr(colon + 1))};
    string semantic;
    uint32_t semanticIndex = 0;
    if (!ParseMetadataSemantic(semanticText, semantic, semanticIndex))
      continue;
    result.push_back({type, std::move(semantic), semanticIndex});
  }
  return result;
}

bool ParseMetadataVertexComponentShape(string_view type, uint32_t &componentType,
                                       uint32_t &componentCount) noexcept {
  string_view suffix;
  if (StartsWith(type, "float") || StartsWith(type, "half")) {
    componentType = 1;
    suffix = StartsWith(type, "float") ? type.substr(5) : type.substr(4);
  } else if (StartsWith(type, "int")) {
    componentType = 2;
    suffix = type.substr(3);
  } else if (StartsWith(type, "uint")) {
    componentType = 3;
    suffix = type.substr(4);
  } else {
    return false;
  }
  if (suffix.empty()) {
    componentCount = 1;
    return true;
  }
  if (suffix.size() != 1 || suffix.front() < '1' || suffix.front() > '4')
    return false;
  componentCount = static_cast<uint32_t>(suffix.front() - '0');
  return true;
}

bool BuildMetadataVertexInputs(string_view source,
                               const ContractData &contract,
                               MetadataFacts &facts,
                               vector<Diagnostic> &diagnostics) {
  const auto vertexEntry = std::find_if(
      contract.EntryPoints.begin(), contract.EntryPoints.end(),
      [](const EntryPoint &entry) noexcept {
        return entry.Stage == ShaderStage::Vertex;
      });
  if (vertexEntry == contract.EntryPoints.end())
    return true;

  const size_t functionName = source.find(vertexEntry->Name);
  const size_t open = functionName == string_view::npos
                          ? string_view::npos
                          : source.find('(', functionName + vertexEntry->Name.size());
  if (open == string_view::npos) {
    diagnostics.push_back({2107, "vertex entry input signature is unavailable"});
    return false;
  }
  size_t close = open + 1;
  uint32_t depth = 1;
  for (; close < source.size() && depth != 0; ++close) {
    if (source[close] == '(')
      ++depth;
    else if (source[close] == ')')
      --depth;
  }
  if (depth != 0) {
    diagnostics.push_back({2107, "vertex entry input signature is malformed"});
    return false;
  }

  const vector<MetadataStructDecl> structs = ParseMetadataStructs(source);
  const string_view parameters = source.substr(open + 1, close - open - 2);
  uint32_t location = 0;
  size_t begin = 0;
  while (begin <= parameters.size()) {
    const size_t end = parameters.find(',', begin);
    const string_view parameter = Trim(parameters.substr(
        begin, end == string_view::npos ? parameters.size() - begin : end - begin));
    begin = end == string_view::npos ? parameters.size() + 1 : end + 1;
    if (parameter.empty())
      continue;

    vector<MetadataVertexFieldDecl> fields;
    if (parameter.find(':') != string_view::npos) {
      string statement{parameter};
      statement.push_back(';');
      fields = ParseMetadataVertexFields(statement);
    } else {
      size_t cursor = 0;
      const string type = MetadataReadIdentifier(parameter, cursor);
      const auto structure = std::find_if(
          structs.begin(), structs.end(),
          [&](const MetadataStructDecl &value) noexcept {
            return value.Name == type;
          });
      if (structure != structs.end())
        fields = ParseMetadataVertexFields(structure->Body);
    }

    for (const MetadataVertexFieldDecl &field : fields) {
      if (StartsWith(field.Semantic, "SV_"))
        continue;
      uint32_t componentType = 0;
      uint32_t componentCount = 0;
      if (!ParseMetadataVertexComponentShape(field.Type, componentType,
                                              componentCount)) {
        diagnostics.push_back({2107, "vertex input type is not representable"});
        return false;
      }
      const auto duplicate = std::find_if(
          facts.VertexInputs.begin(), facts.VertexInputs.end(),
          [&](const MetadataVertexInputFact &value) noexcept {
            return value.Semantic == field.Semantic &&
                   value.SemanticIndex == field.SemanticIndex;
          });
      if (duplicate != facts.VertexInputs.end()) {
        diagnostics.push_back({2108, "vertex input semantic is duplicated"});
        return false;
      }
      facts.VertexInputs.push_back({field.Semantic, field.SemanticIndex,
                                    location++, componentType, componentCount,
                                    0});
    }
  }
  return true;
}

MetadataLayoutInfo MetadataPrimitiveLayout(string_view type) noexcept {
  if (type == "float" || type == "int" || type == "uint" ||
      type == "bool")
    return {4, 4};
  if (type == "float2" || type == "int2" || type == "uint2")
    return {8, 8};
  if (type == "float3" || type == "int3" || type == "uint3")
    return {12, 16};
  if (type == "float4" || type == "int4" || type == "uint4")
    return {16, 16};
  if (type == "float3x3")
    return {48, 16};
  if (type == "float4x4")
    return {64, 16};
  return {0, 4};
}

MetadataLayoutInfo FindMetadataStructLayout(
    string_view type, const vector<MetadataStructDecl> &structs,
    vector<bool> &visiting, vector<MetadataLayoutInfo> &layouts) {
  const auto found = std::find_if(
      structs.begin(), structs.end(),
      [&](const MetadataStructDecl &value) noexcept { return value.Name == type; });
  if (found == structs.end())
    return MetadataPrimitiveLayout(type);
  const size_t index = static_cast<size_t>(found - structs.begin());
  if (layouts[index].Size != 0)
    return layouts[index];
  if (visiting[index])
    return {};
  visiting[index] = true;
  uint32_t offset = 0;
  uint32_t alignment = 16;
  for (const MetadataFieldDecl &field : ParseMetadataFields(found->Body)) {
    const MetadataLayoutInfo fieldLayout = FindMetadataStructLayout(
        field.Type, structs, visiting, layouts);
    alignment = std::max(alignment, fieldLayout.Align);
    offset = (offset + fieldLayout.Align - 1) / fieldLayout.Align *
             fieldLayout.Align;
    const uint64_t size = static_cast<uint64_t>(fieldLayout.Size) *
                          field.ArrayCount;
    offset = size > std::numeric_limits<uint32_t>::max() - offset
                 ? 0
                 : offset + static_cast<uint32_t>(size);
  }
  const uint32_t size = (offset + alignment - 1) / alignment * alignment;
  layouts[index] = {size == 0 ? 16u : size, alignment};
  visiting[index] = false;
  return layouts[index];
}

void AddMetadataTypeFacts(const vector<MetadataStructDecl> &structs,
                          size_t structIndex, uint32_t parent,
                          vector<bool> &visiting,
                          vector<MetadataLayoutInfo> &layouts,
                          vector<MetadataTypeFact> &output) {
  if (structIndex >= structs.size() || visiting[structIndex])
    return;
  const MetadataLayoutInfo layout = FindMetadataStructLayout(
      structs[structIndex].Name, structs, visiting, layouts);
  visiting[structIndex] = true;
  const uint32_t ownIndex = static_cast<uint32_t>(output.size());
  output.push_back({structs[structIndex].Name, parent,
                    4u, 1u, 0u, layout.Size, layout.Size, 0u,
                    kMetadataNoType, {}});
  uint32_t offset = 0;
  for (const MetadataFieldDecl &field :
       ParseMetadataFields(structs[structIndex].Body)) {
    const MetadataLayoutInfo fieldLayout = FindMetadataStructLayout(
        field.Type, structs, visiting, layouts);
    offset = (offset + fieldLayout.Align - 1) / fieldLayout.Align *
             fieldLayout.Align;
    const bool isStruct = std::any_of(
        structs.begin(), structs.end(),
        [&](const MetadataStructDecl &value) noexcept {
          return value.Name == field.Type;
        });
    uint32_t kind = isStruct ? 4u : 1u;
    if (field.Type.find('x') != string_view::npos)
      kind = 3u;
    else if (field.Type.size() > 1 &&
             std::isdigit(static_cast<unsigned char>(field.Type.back())) != 0)
      kind = 2u;
    if (field.ArrayCount > 1)
      kind = 5u;
    const uint32_t size = fieldLayout.Size * field.ArrayCount;
    output.push_back({field.Name, ownIndex, kind, field.ArrayCount, offset,
                      size, fieldLayout.Size, 0u, kMetadataNoType, {}});
    output.back().UnderlyingType = field.Type;
    offset += size;
  }
  visiting[structIndex] = false;
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

bool ParseMetadataRootConstants(string_view source,
                                const vector<MetadataStageSpan> &stages,
                                RadRayDxcTarget target,
                                MetadataFacts &facts) {
  if (target == RadRayDxcTarget::DXIL) {
    size_t cursor = 0;
    while ((cursor = source.find("RootConstants(", cursor)) !=
           string_view::npos) {
      const size_t begin = cursor + 14;
      const size_t end = source.find(')', begin);
      if (end == string_view::npos)
        return false;
      const string_view args = source.substr(begin, end - begin);
      const size_t countPosition = args.find("num32BitConstants=");
      const size_t registerPosition = args.find('b');
      const size_t spacePosition = args.find("space=");
      if (countPosition == string_view::npos ||
          registerPosition == string_view::npos ||
          spacePosition == string_view::npos)
        return false;
      size_t countCursor = countPosition + 18;
      size_t registerCursor = registerPosition + 1;
      size_t spaceCursor = spacePosition + 6;
      uint32_t count = 0;
      uint32_t binding = 0;
      uint32_t group = 0;
      if (!MetadataParseUnsigned(args, countCursor, count) ||
          !MetadataParseUnsigned(args, registerCursor, binding) ||
          !MetadataParseUnsigned(args, spaceCursor, group) || count == 0)
        return false;
      uint32_t stageMask = 0;
      for (const MetadataStageSpan &stage : stages)
        stageMask |= MetadataStageBit(stage.Stage);
      facts.RootConstants.push_back(
          {group, binding, 0, count * 4, stageMask, 0});
      cursor = end + 1;
    }
  }

  if (target != RadRayDxcTarget::SPIRV)
    return true;
  uint32_t pushCount = 0;
  size_t cursor = 0;
  while ((cursor = source.find("vk::push_constant", cursor)) !=
         string_view::npos) {
    ++pushCount;
    cursor += 18;
  }
  cursor = 0;
  while ((cursor = source.find("VK_PUSH_CONSTANT", cursor)) !=
         string_view::npos) {
    ++pushCount;
    cursor += 16;
  }
  if (pushCount == 0)
    return true;
  if (pushCount != 1 || !facts.RootConstants.empty())
    return false;
  uint32_t stageMask = 0;
  for (const MetadataStageSpan &stage : stages)
    stageMask |= MetadataStageBit(stage.Stage);
  facts.RootConstants.push_back({0, 0, 0, 16, stageMask, 1});
  return true;
}

bool BuildMetadataFacts(string_view source, const ContractData &contract,
                        RadRayDxcTarget target, MetadataFacts &facts,
                        vector<Diagnostic> &diagnostics) {
  const string cleaned = CleanSource(source);
  const vector<MetadataStageSpan> stages =
      FindMetadataStageSpans(cleaned, contract);
  if (stages.size() != contract.EntryPoints.size()) {
    diagnostics.push_back(
        {2101, "metadata builder could not locate every discovered entry point"});
    return false;
  }

  string explicitRootSignature;
  bool hasExplicitRootSignature = false;
  if (target == RadRayDxcTarget::DXIL) {
    for (const MetadataStageSpan &stage : stages) {
      const string signature = FindMetadataRootSignature(cleaned, stage.Begin);
      if (signature.empty())
        continue;
      if (!hasExplicitRootSignature) {
        explicitRootSignature = signature;
        hasExplicitRootSignature = true;
      } else if (contract.Kind == ShaderKind::Graphics &&
                 signature != explicitRootSignature) {
        diagnostics.push_back(
            {2105, "graphics stages declare different RootSignature attributes"});
        return false;
      }
    }
  }

  vector<MetadataBindingFact> declarations;
  uint32_t braceDepth = 0;
  size_t lineStart = 0;
  string pendingAttribute;
  while (lineStart <= cleaned.size()) {
    const size_t lineEnd = cleaned.find_first_of("\r\n", lineStart);
    const size_t boundedEnd = lineEnd == string::npos ? cleaned.size() : lineEnd;
    string line{Trim(string_view{cleaned}.substr(lineStart,
                                                   boundedEnd - lineStart))};
    if (line.find("VK_BINDING(") != string_view::npos ||
        line.find("vk::binding(") != string_view::npos)
      pendingAttribute += line;
    for (const char character : line) {
      if (character == '{')
        ++braceDepth;
      else if (character == '}' && braceDepth != 0)
        --braceDepth;
    }
    if (braceDepth == 0 && line.find(';') != string_view::npos) {
      if (!pendingAttribute.empty()) {
        line = pendingAttribute + line;
        pendingAttribute.clear();
      }
      MetadataBindingFact binding;
      if (ParseMetadataResourceLine(line, target, binding))
        declarations.push_back(std::move(binding));
    }
    if (lineEnd == string_view::npos)
      break;
    lineStart = lineEnd + 1;
    if (lineStart < cleaned.size() && cleaned[lineEnd] == '\r' &&
        cleaned[lineStart] == '\n')
      ++lineStart;
  }

  uint32_t nextBinding[4] = {};
  for (MetadataBindingFact &declaration : declarations) {
    uint32_t stageMask = 0;
    for (const MetadataStageSpan &stage : stages) {
      if (MetadataUsesIdentifier(
              cleaned.substr(stage.Begin, stage.End - stage.Begin),
              declaration.Name))
        stageMask |= MetadataStageBit(stage.Stage);
    }
    if (stageMask == 0)
      continue;
    declaration.StageMask = stageMask;
    if (!declaration.HasExplicitBinding) {
      while (std::any_of(
          declarations.begin(), declarations.end(),
          [&](const MetadataBindingFact &value) noexcept {
            return value.HasExplicitBinding &&
                   value.RegisterClass == declaration.RegisterClass &&
                   value.Group == declaration.Group &&
                   value.Binding == nextBinding[declaration.RegisterClass];
          }))
        ++nextBinding[declaration.RegisterClass];
      declaration.Binding = nextBinding[declaration.RegisterClass]++;
    }
    const auto existing = std::find_if(
        facts.Bindings.begin(), facts.Bindings.end(),
        [&](const MetadataBindingFact &value) noexcept {
          return value.Name == declaration.Name;
        });
    if (existing != facts.Bindings.end()) {
      diagnostics.push_back({2102, "duplicate active binding declaration"});
      return false;
    }
    facts.Bindings.push_back(std::move(declaration));
  }

  if (hasExplicitRootSignature &&
      !ValidateMetadataRootBindings(explicitRootSignature, facts.Bindings,
                                    diagnostics))
    return false;

  const vector<MetadataStructDecl> structs = ParseMetadataStructs(cleaned);
  vector<bool> visiting(structs.size(), false);
  vector<MetadataLayoutInfo> layouts(structs.size());
  for (size_t index = 0; index < structs.size(); ++index)
    AddMetadataTypeFacts(structs, index, kMetadataNoParent, visiting, layouts,
                         facts.Types);
  if (!ResolveMetadataTypeIndices(facts.Types)) {
    diagnostics.push_back({2107, "metadata type tree contains an unresolved struct reference"});
    return false;
  }
  if (!ParseMetadataRootConstants(cleaned, stages, target, facts)) {
    diagnostics.push_back(
        {2103, "shader source contains an invalid push/root constant declaration"});
    return false;
  }
  if (!BuildMetadataVertexInputs(cleaned, contract, facts, diagnostics))
    return false;

  vector<uint32_t> staticSamplerRegisters;
  size_t staticSamplerCursor = 0;
  while ((staticSamplerCursor = cleaned.find("StaticSampler(",
                                             staticSamplerCursor)) !=
         string_view::npos) {
    size_t registerCursor = staticSamplerCursor + 14;
    registerCursor = MetadataSkipSpace(cleaned, registerCursor);
    if (registerCursor >= cleaned.size() || cleaned[registerCursor] != 's') {
      diagnostics.push_back({2104, "static sampler declaration is malformed"});
      return false;
    }
    ++registerCursor;
    uint32_t registerIndex = 0;
    if (!MetadataParseUnsigned(cleaned, registerCursor, registerIndex) ||
        std::find(staticSamplerRegisters.begin(), staticSamplerRegisters.end(),
                  registerIndex) != staticSamplerRegisters.end()) {
      diagnostics.push_back(
          {2104, "static sampler register is duplicated or malformed"});
      return false;
    }
    staticSamplerRegisters.push_back(registerIndex);
    staticSamplerCursor = registerCursor;
  }
  const size_t activeSamplerCount = static_cast<size_t>(std::count_if(
      facts.Bindings.begin(), facts.Bindings.end(),
      [](const MetadataBindingFact &binding) noexcept {
        return binding.Type ==
               static_cast<uint32_t>(MetadataBindingKind::Sampler);
      }));
  if (staticSamplerRegisters.size() > activeSamplerCount) {
    diagnostics.push_back(
        {2104, "static sampler has no matching active sampler declaration"});
    return false;
  }
  if (!staticSamplerRegisters.empty()) {
    for (MetadataBindingFact &binding : facts.Bindings) {
      if (binding.Type ==
          static_cast<uint32_t>(MetadataBindingKind::Sampler))
        binding.Flags |= kMetadataImmutableSampler;
    }
  }
  return true;
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
      const IncludeSource *found = nullptr;
      const string logicalName = NormalizeInclude(string{trimmed.substr(begin + 1, endName - begin - 1)});
      for (const IncludeSource &include : request.Includes) {
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
  WireRange VertexInputRecords;
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

static_assert(sizeof(WireEnvelope) == 152);
static_assert(sizeof(WireEntryRecord) == 24);
static_assert(sizeof(WireBindingRecord) == 32);
static_assert(sizeof(WireTypeRecord) == 40);
static_assert(sizeof(WireRootConstantRecord) == 24);
static_assert(sizeof(WireVertexInputRecord) == 28);

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

bool BuildMetadata(const CompileRequest &request, const ContractData &contract,
                   RadRayDxcTarget target, const vector<StageOutput> &stages,
                   const Hash128 &compileInput, vector<Diagnostic> &diagnostics,
                   vector<uint8_t> &metadata) {
  MetadataFacts facts;
  const string rootSource(reinterpret_cast<const char *>(request.RootSource.data()),
                          request.RootSource.size());
  if (!BuildMetadataFacts(rootSource, contract, target, facts, diagnostics))
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
      vector<uint8_t> metadata;
      if (!BuildMetadata(parsed, contract, target, stages, inputHash,
                         compileDiagnostics, metadata)) {
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
