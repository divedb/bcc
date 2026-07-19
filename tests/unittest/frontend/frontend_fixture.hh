#pragma once

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bcc/ast/ast_context.hh"
#include "bcc/ast/ast_dumper.hh"
#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/parse/parser.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/sema/sema.hh"
#include "gtest/gtest.h"

namespace bcc {

/// Collects every diagnostic's kind and formatted message.
class CollectingDiagConsumer final : public DiagnosticConsumer {
 public:
  struct Entry {
    diag::DiagKind kind;
    DiagSeverity severity;
    std::string message;
  };

  void HandleDiagnostic(const DiagnosticInfo& info) override {
    entries.push_back(
        {info.GetDiagKind(), info.GetSeverity(), info.FormatMessage()});
  }

  std::vector<Entry> entries;
};

/// Runs a source string through the full PP -> Parser -> Sema pipeline and
/// exposes the AST and diagnostics for assertions.
class FrontendTest : public ::testing::Test {
 protected:
  void Parse(std::string_view code) {
    fm_ = std::make_unique<FileManager>();
    sm_ = std::make_unique<SourceManager>(*fm_);
    consumer_ = std::make_unique<CollectingDiagConsumer>();
    diags_ = std::make_unique<DiagnosticsEngine>(consumer_.get(), sm_.get());
    sm_->SetMainFileID(sm_->CreateFileID("test.c", std::string(code)));
    pp_ = std::make_unique<Preprocessor>(*sm_, *diags_);
    pp_->EnterMainFile();
    ctx_ = std::make_unique<ASTContext>();
    sema_ = std::make_unique<Sema>(*pp_, *ctx_);
    parser_ = std::make_unique<Parser>(*pp_, *sema_);
    parser_->ParseTranslationUnit();
  }

  unsigned NumErrors() const { return diags_->NumErrors(); }
  unsigned NumWarnings() const { return diags_->NumWarnings(); }

  bool HasDiag(diag::DiagKind kind) const {
    for (const auto& e : consumer_->entries) {
      if (e.kind == kind) return true;
    }
    return false;
  }

  /// The first declaration in the TU with the given name, or nullptr.
  const NamedDecl* FindDecl(std::string_view name) const {
    for (const Decl* d : ctx_->GetTranslationUnit()->GetDecls()) {
      if (const auto* nd = d->As<NamedDecl>()) {
        if (nd->GetName() == name) return nd;
      }
    }
    return nullptr;
  }

  /// The declared type of the named value declaration, rendered as a string.
  std::string TypeOf(std::string_view name) const {
    const NamedDecl* d = FindDecl(name);
    if (!d) return "<not found>";
    if (const auto* vd = d->As<ValueDecl>()) {
      return vd->GetType().GetAsString();
    }
    return "<not a value>";
  }

  std::string DumpAST() const {
    std::ostringstream os;
    ASTDumper dumper(os);
    dumper.Dump(ctx_->GetTranslationUnit());
    return os.str();
  }

  std::unique_ptr<FileManager> fm_;
  std::unique_ptr<SourceManager> sm_;
  std::unique_ptr<CollectingDiagConsumer> consumer_;
  std::unique_ptr<DiagnosticsEngine> diags_;
  std::unique_ptr<Preprocessor> pp_;
  std::unique_ptr<ASTContext> ctx_;
  std::unique_ptr<Sema> sema_;
  std::unique_ptr<Parser> parser_;
};

}  // namespace bcc
