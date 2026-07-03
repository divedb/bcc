// bcc-as: a standalone x86-64 AT&T-syntax assembler producing ELF64 objects.
//
//   bcc-as [-o output.o] [input.s]
//
// With no input file, reads from stdin; with no -o, writes to "a.out".

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "bcc/as/elf.hh"
#include "bcc/as/lexer.hh"
#include "bcc/as/mccontext.hh"
#include "bcc/as/parser.hh"

namespace {

bool ReadAll(std::istream& is, std::string& out) {
  std::ostringstream ss;
  ss << is.rdbuf();
  out = ss.str();
  return static_cast<bool>(is);
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string output_path = "a.out";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o") {
      if (i + 1 >= argc) {
        std::cerr << "bcc-as: error: -o requires an argument\n";
        return 1;
      }
      output_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: bcc-as [-o output.o] [input.s]\n";
      return 0;
    } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
      std::cerr << "bcc-as: error: unknown option '" << arg << "'\n";
      return 1;
    } else {
      input_path = arg;
    }
  }

  std::string source;
  std::string display_name;
  if (input_path.empty() || input_path == "-") {
    display_name = "<stdin>";
    ReadAll(std::cin, source);
  } else {
    display_name = input_path;
    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs) {
      std::cerr << "bcc-as: error: cannot open '" << input_path << "'\n";
      return 1;
    }
    ReadAll(ifs, source);
  }

  bcc::as::Lexer lexer(source);
  bcc::as::MCContext ctx;
  bcc::as::AsmDiag diag(display_name, lexer, std::cerr);
  bcc::as::Parser parser(lexer, ctx, diag);

  parser.Run();
  ctx.RelaxBranches();
  if (diag.has_error()) {
    std::cerr << "bcc-as: " << diag.num_errors() << " error(s); no output written\n";
    return 1;
  }

  bcc::as::ElfWriter writer(ctx.sections(), ctx.symtab());
  if (!writer.WriteFile(output_path)) {
    std::cerr << "bcc-as: error: " << writer.error() << "\n";
    return 1;
  }
  return 0;
}
