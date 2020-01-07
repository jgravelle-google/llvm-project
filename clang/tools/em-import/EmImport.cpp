//===--- tools/clang-check/ClangCheck.cpp - Clang check tool --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements a clang-check tool that runs clang based on the info
//  stored in a compilation database.
//
//  This tool uses the Clang Tooling infrastructure, see
//    http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html
//  for details on setting it up with LLVM source tree.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/CodeGen/ObjectFilePCHContainerOperations.h"
#include "clang/Driver/Options.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Frontend/FixItRewriter.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/StaticAnalyzer/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"

using namespace clang::driver;
using namespace clang::tooling;
using namespace llvm;

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp(
    "\tFor example, to run clang-check on all files in a subtree of the\n"
    "\tsource tree, use:\n"
    "\n"
    "\t  find path/in/subtree -name '*.cpp'|xargs clang-check\n"
    "\n"
    "\tor using a specific build path:\n"
    "\n"
    "\t  find path/in/subtree -name '*.cpp'|xargs clang-check -p build/path\n"
    "\n"
    "\tNote, that path/in/subtree and current directory should follow the\n"
    "\trules described above.\n"
    "\n"
);

static cl::OptionCategory ClangCheckCategory("clang-check options");
static const opt::OptTable &Options = getDriverOptTable();
static cl::opt<std::string>
  Outfile("o", cl::desc("Output file"), cl::cat(ClangCheckCategory));

namespace {
using namespace clang;

class MyConsumer : public ASTConsumer,
                   public RecursiveASTVisitor<MyConsumer> {
  typedef RecursiveASTVisitor<MyConsumer> base;
  llvm::raw_ostream *os;
  std::error_code EC;
  std::unique_ptr<ASTNameGenerator> mangler;
public:
  MyConsumer() {
    if (Outfile != "") {
      os = new llvm::raw_fd_ostream(Outfile, EC);
    } else {
      os = &llvm::outs();
    }
  }
  ~MyConsumer() {
    if (Outfile != "") {
      delete os;
    }
  }
  void HandleTranslationUnit(ASTContext &Context) override {
    auto *D = Context.getTranslationUnitDecl();
    mangler = std::make_unique<ASTNameGenerator>(Context);
    TraverseDecl(D);
  }
  const std::string prefix = "EM_IMPORT:";
  const int prefixLen = prefix.size();

  void doTheThing(Decl* D, std::string className = "") {
    if (auto FD = dyn_cast_or_null<FunctionDecl>(D)) {
      auto name = FD->getQualifiedNameAsString();
      if (auto* Attr = FD->getAttr<AnnotateAttr>()) {
        auto note = Attr->getAnnotation();
        if (note.startswith(prefix)) {
          auto data = StringRef(note.data() + prefixLen, note.size() - prefixLen);
          auto pair = data.split(":");
          auto kind = pair.first;
          auto importName = pair.second;
          auto T = FD->getType()->castAs<FunctionProtoType>();
          *os << "(" << kind;
          if (kind != "func") {
            assert(className != "");
            *os << " \"" << className << "\"";
          }
          *os << " " << mangler->getName(FD);
          if (kind != "constructor") {
            *os << " \"" << importName << "\"";
          }
          *os << " (";
          for (unsigned i = 0; i < T->getNumParams(); ++i) {
            auto Arg = T->getParamType(i);
            if (i != 0) { *os << " "; }
            *os << '"' << Arg.getAsString() << '"';
          }
          auto Ret = T->getReturnType();
          *os << ") \"" << Ret.getAsString() << "\")\n";
        }
      }
    }
  }
  bool TraverseDecl(Decl *D) {
    if (auto *TD = dyn_cast_or_null<RecordDecl>(D)) {
      if (auto *Attr = D->getAttr<AnnotateAttr>()) {
        auto note = Attr->getAnnotation();
        if (note.startswith(prefix)) {
          auto data = StringRef(note.data() + prefixLen, note.size() - prefixLen);
          auto pair = data.split(":");
          auto className = pair.second;
          for (auto *sub : TD->decls()) {
            doTheThing(sub, className);
          }
        }
      }
    } else if (auto FD = dyn_cast_or_null<FunctionDecl>(D)) {
      if (auto *Attr = D->getAttr<AnnotateAttr>()) {
        auto note = Attr->getAnnotation();
        if (note.startswith("EM_IMPORT:func:")) {
          doTheThing(D);
        }
      }
    }
    return base::TraverseDecl(D);
  }
};

class ClangCheckActionFactory {
public:
  std::unique_ptr<clang::ASTConsumer> newASTConsumer() {
    return std::make_unique<MyConsumer>();
  }
};

} // namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  // Initialize targets for clang module support.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  CommonOptionsParser OptionsParser(argc, argv, ClangCheckCategory);
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  // Clear adjusters because -fsyntax-only is inserted by the default chain.
  Tool.clearArgumentsAdjusters();
  Tool.appendArgumentsAdjuster(getClangStripOutputAdjuster());
  Tool.appendArgumentsAdjuster(getClangStripDependencyFileAdjuster());

  // Running the analyzer requires --analyze. Other modes can work with the
  // -fsyntax-only option.
  Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster("-fsyntax-only", ArgumentInsertPosition::BEGIN));

  ClangCheckActionFactory CheckFactory;
  std::unique_ptr<FrontendActionFactory> FrontendFactory =
    newFrontendActionFactory(&CheckFactory);

  return Tool.run(FrontendFactory.get());
}
