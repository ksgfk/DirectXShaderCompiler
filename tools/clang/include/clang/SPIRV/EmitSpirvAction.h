//===-- EmitSpirvAction.h - FrontendAction for Emitting SPIR-V --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_SPIRV_EMITSPIRVACTION_H
#define LLVM_CLANG_SPIRV_EMITSPIRVACTION_H

#include "clang/Frontend/FrontendAction.h"

namespace clang {

namespace spirv {
class SpirvEmitter;
}

class EmitSpirvAction : public ASTFrontendAction {
public:
  EmitSpirvAction() {}

  spirv::SpirvEmitter *getSpirvEmitter() const { return emitter; }

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override;

private:
  spirv::SpirvEmitter *emitter{nullptr};
};

} // end namespace clang

#endif
