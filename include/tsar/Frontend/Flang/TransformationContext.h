//===- TransformationContext.h - TSAR Transformation Engine (Flang) C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file defines Flang-based source level transformation engine which.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_TRANSFORMATION_CONTEXT_H
#define TSAR_FLANG_TRANSFORMATION_CONTEXT_H

#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Flang/Rewriter.h"
#include <flang/Parser/parsing.h>
#include <flang/Semantics/semantics.h>
#include <llvm/ADT/StringMap.h>

namespace llvm {
class Module;
class DICompileUnit;
}

namespace tsar {
class FlangTransformationContext : public TransformationContextBase {
  using MangledToSourceMapT = llvm::StringMap<Fortran::semantics::Symbol *>;

public:
  static bool classof(const TransformationContextBase *Ctx) noexcept {
    return Ctx->getKind() == TC_Flang;
  }

  FlangTransformationContext(Fortran::parser::Parsing &Parsing,
                             Fortran::parser::Options &Options,
                             Fortran::semantics::SemanticsContext &Context,
                             const llvm::Module &M,
                             const llvm::DICompileUnit &CU)
      : TransformationContextBase(TC_Flang), mParsing(&Parsing),
        mOptions(&Options), mContext(&Context) {
    initialize(M, CU);
  }

  bool hasInstance() const override {
    auto *This{const_cast<FlangTransformationContext *>(this)};
    return This->mParsing && This->mParsing->parseTree().has_value() &&
           !This->mParsing->messages().AnyFatalError() && This->mOptions &&
           This->mContext && !This->mContext->AnyFatalError();
  }

  bool hasModification() const override {
    return hasInstance() && mRewriter->hasModification();
  }

  std::pair<std::string, bool>
  release(const FilenameAdjuster &FA = getDumpFilenameAdjuster()) override;

  auto &getParsing() noexcept {
    assert(hasInstance() && "Transformation context must be configured!");
    return *mParsing;
  }
  const auto &getParsing() const noexcept {
    assert(hasInstance() && "Transformation context must be configured!");
    return *mParsing;
  }

  const auto &getOptions() const noexcept {
    assert(hasInstance() && "Transformation context must be configured!");
    return *mOptions;
  }

  auto &getContext() noexcept {
    assert(hasInstance() && "Transformation context must be configured!");
    return *mContext;
  }
  const auto &getContext() const noexcept {
    assert(hasInstance() && "Transformation context must be configured!");
    return *mContext;
  }

  auto &getRewriter() {
    assert(hasInstance() && "Transformation context is not configured!");
    return *mRewriter;
  }

  const auto &getRewriter() const {
    assert(hasInstance() && "Transformation context is not configured!");
    return *mRewriter;
  }

  /// Return a declaration for a mangled name.
  ///
  /// \pre Transformation instance must be configured.
  Fortran::semantics::Symbol * getDeclForMangledName(llvm::StringRef Name) {
    assert(hasInstance() && "Transformation context is not configured!");
    auto I = mGlobals.find(Name);
    return (I != mGlobals.end()) ? I->getValue() : nullptr;
  }

private:
  void initialize(const llvm::Module &M, const llvm::DICompileUnit &CU);

  Fortran::parser::Parsing *mParsing{nullptr};
  Fortran::parser::Options *mOptions{nullptr};
  Fortran::semantics::SemanticsContext *mContext{nullptr};
  MangledToSourceMapT mGlobals;
  std::unique_ptr<FlangRewriter> mRewriter{nullptr};
};
}
#endif//TSAR_FLANG_TRANSFORMATION_CONTEXT_H
