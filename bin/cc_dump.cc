// cc_dump — a minimal C front-end driver: preprocess, parse, and semantically
// analyze a translation unit, reporting diagnostics.
//
// Supported flags:
//   -I<dir>        add a system (angled) include search path
//   -fsyntax-only  check only (default mode; accepted for compat)
//   -ast-dump      print the AST after a successful parse
//   -emit-ir       lower the AST to bcc IR and print it (LLVM syntax)

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/ast/ast_context.hh"
#include "bcc/ast/ast_dumper.hh"
#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/basic/text_diagnostic_printer.hh"
#include "bcc/codegen/codegen_module.hh"
#include "bcc/ir/ir_printer.hh"
#include "bcc/ir/module.hh"
#include "bcc/parse/parser.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/sema/sema.hh"

int main(int argc, char* argv[]) {
  bool ast_dump = false;
  bool emit_ir = false;
  std::vector<std::string> include_dirs;
  const char* input_path = nullptr;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-ast-dump") {
      ast_dump = true;
    } else if (arg == "-emit-ir") {
      emit_ir = true;
    } else if (arg == "-fsyntax-only") {
      // Default mode.
    } else if (arg.rfind("-I", 0) == 0) {
      include_dirs.emplace_back(arg.substr(2));
    } else {
      input_path = argv[i];
    }
  }

  bcc::FileManager fm;
  bcc::SourceManager sm(fm);
  bcc::TextDiagnosticPrinter printer(std::cerr);
  bcc::DiagnosticsEngine diag(&printer, &sm);
  bcc::HeaderSearch header_search(fm);
  for (const std::string& dir : include_dirs) {
    header_search.AddAngledSearchPath(dir);
  }

  const bcc::FileEntry* entry =
      input_path ? fm.GetFile(input_path) : fm.GetStdin();
  if (!entry) {
    diag.Report(bcc::diag::err_no_input_file);
    return 1;
  }

  sm.SetMainFileID(sm.CreateFileID(*entry));

  bcc::Preprocessor pp(sm, diag, header_search);
  pp.EnterMainFile();

  bcc::ASTContext ctx;
  bcc::Sema sema(pp, ctx);
  bcc::Parser parser(pp, sema);
  parser.ParseTranslationUnit();

  if (ast_dump) {
    bcc::ASTDumper dumper(std::cout, &sm);
    dumper.Dump(ctx.GetTranslationUnit());
  }

  if (emit_ir && !diag.HasErrors()) {
    bcc::ir::Module module;
    bcc::codegen::CodeGenModule cgm(ctx, diag, module);
    cgm.EmitTranslationUnit();
    if (!diag.HasErrors()) {
      bcc::ir::IRPrinter printer(std::cout);
      printer.Print(module);
    }
  }

  return diag.HasErrors() ? 1 : 0;
}
