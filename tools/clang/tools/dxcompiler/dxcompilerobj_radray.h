#ifndef DXC_DXCOMPILEROBJ_RADRAY_H
#define DXC_DXCOMPILEROBJ_RADRAY_H

#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/SPIRV/EmitSpirvAction.h"
#include "dxc/dxcapi.h"
#include "llvm/IR/Module.h"

#include <memory>

// This header is private to the dxcompiler target. It is intentionally not
// installed with the SDK and must not become part of the public DXC ABI.
class RadRayCompilerObserver {
public:
  virtual ~RadRayCompilerObserver() = default;

  virtual std::unique_ptr<clang::FrontendAction> WrapAction(
      std::unique_ptr<clang::FrontendAction> action) = 0;

  virtual bool UseSyntaxOnly() const { return false; }

  virtual void OnDxilModule(llvm::Module &module) = 0;

#ifdef ENABLE_SPIRV_CODEGEN
  virtual void OnSpirvActionComplete(clang::EmitSpirvAction &action) = 0;
#endif
};

HRESULT CompileDxcWithRadRayObserver(
    const DxcBuffer *source,
    LPCWSTR *arguments,
    UINT32 argumentCount,
    IDxcIncludeHandler *includeHandler,
    RadRayCompilerObserver *observer,
    REFIID resultIid,
    LPVOID *result);

#endif
