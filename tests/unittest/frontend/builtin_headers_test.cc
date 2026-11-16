// Tests for the compiler's built-in headers (lib/Headers): each test
// includes a real shipped header through the FrontendTest fixture (which
// registers the built-in search path) and asserts the types and macros it
// must provide for bcc's x86-64 SysV target.
//
// Type assertions query the typedef declarations themselves (TypeOf on the
// typedef name yields its underlying type); file-scope variables obey C's
// constant-initializer rule, so non-constant expressions (offsetof) are
// exercised inside function bodies.

#ifdef BCC_BUILTIN_HEADERS_DIR

#include <string>

#include "frontend_fixture.hh"

namespace bcc {
namespace {

TEST_F(FrontendTest, StddefDefinesConventionalTypes) {
  Parse("#include <stddef.h>\n"
        "size_t a;\n"
        "ptrdiff_t b;\n"
        "wchar_t c;\n"
        "max_align_t d;\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("size_t"), "unsigned long");
  EXPECT_EQ(TypeOf("ptrdiff_t"), "long");
  EXPECT_EQ(TypeOf("wchar_t"), "int");
}

TEST_F(FrontendTest, StddefNullAndOffsetofUsable) {
  Parse("#include <stddef.h>\n"
        "struct S { int a; double b; };\n"
        "void* p = NULL;\n"
        "size_t use(void) { return offsetof(struct S, b); }\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("p"), "void *");
  EXPECT_EQ(TypeOf("use"), "size_t (void)");
}

TEST_F(FrontendTest, StddefNeedProtocolSelectsSingleType) {
  Parse("#define __need_size_t\n"
        "#include <stddef.h>\n"
        "size_t a;\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("size_t"), "unsigned long");
  // Partial mode must not have pulled in the other definitions.
  EXPECT_EQ(FindDecl("ptrdiff_t"), nullptr);
}

TEST_F(FrontendTest, StddefNeedProtocolAfterFullInclude) {
  Parse("#include <stddef.h>\n"
        "#define __need_wint_t\n"
        "#include <stddef.h>\n"
        "wint_t w;\n");

  // The __need_ re-include is processed despite the full-mode guard.
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("wint_t"), "unsigned int");
}

TEST_F(FrontendTest, StdboolDefinesBoolTrueFalse) {
  Parse("#include <stdbool.h>\n"
        "bool b = true;\n"
        "bool f = false;\n"
        "_Bool t = 1;\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("b"), "_Bool");
  EXPECT_EQ(TypeOf("f"), "_Bool");
}

TEST_F(FrontendTest, LimitsConstantsFitTheirTypes) {
  Parse("#include <limits.h>\n"
        "int imin = INT_MIN;\n"
        "int imax = INT_MAX;\n"
        "long lmax = LONG_MAX;\n"
        "unsigned long ulmax = ULONG_MAX;\n"
        "long long llmin = LLONG_MIN;\n");

  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(FrontendTest, FloatConstantsFitTheirTypes) {
  Parse("#include <float.h>\n"
        "float f = FLT_MAX;\n"
        "double d = DBL_MAX;\n"
        "long double ld = LDBL_MAX;\n"
        "int radix = FLT_RADIX;\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("f"), "float");
  EXPECT_EQ(TypeOf("d"), "double");
  EXPECT_EQ(TypeOf("ld"), "long double");
}

TEST_F(FrontendTest, StdintDefinesExactWidthTypes) {
  Parse("#include <stdint.h>\n"
        "int8_t a;\n"
        "uint16_t b;\n"
        "int32_t c;\n"
        "uint64_t d;\n"
        "intptr_t e;\n"
        "uintptr_t f;\n"
        "intmax_t g;\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("int8_t"), "signed char");
  EXPECT_EQ(TypeOf("uint16_t"), "unsigned short");
  EXPECT_EQ(TypeOf("int32_t"), "int");
  EXPECT_EQ(TypeOf("uint64_t"), "unsigned long");
  EXPECT_EQ(TypeOf("intptr_t"), "long");
  EXPECT_EQ(TypeOf("uintptr_t"), "unsigned long");
  EXPECT_EQ(TypeOf("intmax_t"), "long");
}

TEST_F(FrontendTest, StdintLimitMacrosUsable) {
  Parse("#include <stdint.h>\n"
        "int fits = (INT32_MAX > INT16_MAX);\n"
        "unsigned long smax = SIZE_MAX;\n"
        "long pmax = PTRDIFF_MAX;\n");

  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(FrontendTest, StdargDefinesVaListTypes) {
  Parse("#include <stdarg.h>\n"
        "va_list ap;\n"
        "__gnuc_va_list gap;\n");

  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(FrontendTest, Iso646AlternativeSpellings) {
  Parse("#include <iso646.h>\n"
        "int r = (1 and 2) or not 0;\n"
        "int x = 6 bitand 3;\n");

  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(FrontendTest, BuiltinHeadersCompose) {
  Parse("#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <limits.h>\n"
        "size_t n = sizeof(uint64_t);\n"
        "bool ok = (sizeof(size_t) == 8);\n"
        "int guard = (CHAR_BIT == 8);\n");

  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("uint64_t"), "unsigned long");
}

}  // namespace
}  // namespace bcc

#endif  // BCC_BUILTIN_HEADERS_DIR
