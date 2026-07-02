#include <iostream>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/basic/text_diagnostic_printer.hh"

namespace {

// Emits "N warning[s] and M error[s] generated." to match compiler convention.
void PrintSummary(std::ostream& os, const bcc::DiagnosticsEngine& engine) {
  unsigned nw = engine.NumWarnings();
  unsigned ne = engine.NumErrors();

  if (nw == 0 && ne == 0) return;

  if (nw > 0) {
    os << nw << (nw == 1 ? " warning" : " warnings");

    if (ne > 0) os << " and ";
  }

  if (ne > 0) os << ne << (ne == 1 ? " error" : " errors");

  os << " generated.\n";
}

}  // namespace

int main() {
  // main.c                            a.h
  // ──────────────────────────        ──────────────────────────────
  //   1: #include "./a.h"             1: // helper macros
  //   2:                              2:
  //   3: int main() {                 3: #define SQUARE(x) (x * x
  //   4:     const char *p1 = 42;
  //   5:     char *p2 = p1;
  //   6:
  //   7:     int x = SQUARE(42);
  //   8: }

  constexpr std::string_view kMainContent =
      "#include \"./a.h\"\n"
      "\n"
      "int main() {\n"
      "    const char *p1 = 42;\n"
      "    char *p2 = p1;\n"
      "\n"
      "    int x = SQUARE(42);\n"
      "}\n";

  constexpr std::string_view kHeaderContent =
      "// helper macros\n"
      "\n"
      "#define SQUARE(x) (x * x\n";

  bcc::FileManager fm;
  bcc::SourceManager sm(fm);

  bcc::FileID main_fid = sm.CreateFileID("main.c", std::string(kMainContent));
  bcc::FileID hdr_fid = sm.CreateFileID("./a.h", std::string(kHeaderContent));
  sm.SetMainFileID(main_fid);

  bcc::TextDiagnosticPrinter printer(std::cerr);
  bcc::DiagnosticsEngine engine(&printer, &sm);

  // Promote -Wint-conversion to an error, like -Werror=int-conversion.
  engine.GetOptions().SetGroupSeverity("int-conversion",
                                       bcc::DiagSeverity::kError);

  // main.c:4:     const char *p1 = 42;
  //                               ^    ~~
  {
    auto loc = sm.TranslateLineCol(main_fid, 4, 17);      // *p1
    auto r_begin = sm.TranslateLineCol(main_fid, 4, 22);  // 42
    auto r_end = sm.TranslateLineCol(main_fid, 4, 24);    // past 42
    engine.Report(loc, bcc::diag::warn_int_conversion)
        << "const char *" << "int" << bcc::SourceRange(r_begin, r_end);
  }

  // main.c:5:     char *p2 = p1;
  //                      ^    ~~
  {
    auto loc = sm.TranslateLineCol(main_fid, 5, 11);      // *p2
    auto r_begin = sm.TranslateLineCol(main_fid, 5, 16);  // p1
    auto r_end = sm.TranslateLineCol(main_fid, 5, 18);    // past p1
    engine.Report(loc,
                  bcc::diag::warn_incompatible_ptr_types_discards_qualifiers)
        << "char *" << "const char *" << bcc::SourceRange(r_begin, r_end);
  }

  // main.c:7:     int x = SQUARE(42);   — expected ')'
  {
    auto loc = sm.TranslateLineCol(main_fid, 7, 23);  // past SQUARE(42)
    engine.Report(loc, bcc::diag::err_expected_rparen);
  }

  // main.c:7:     int x = SQUARE(42);   — to match this '('
  {
    auto loc = sm.TranslateLineCol(main_fid, 7, 13);  // S of SQUARE
    engine.Report(loc, bcc::diag::note_to_match_this_lparen);
  }

  // ./a.h:3:  #define SQUARE(x) (x * x   — expanded from macro 'SQUARE'
  {
    auto loc = sm.TranslateLineCol(hdr_fid, 3, 19);  // ( in macro body
    engine.Report(loc, bcc::diag::note_expanded_from_macro) << "SQUARE";
  }

  PrintSummary(std::cerr, engine);

  return engine.HasErrors() ? 1 : 0;
}
