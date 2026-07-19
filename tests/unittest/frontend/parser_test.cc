#include "frontend_fixture.hh"

namespace bcc {
namespace {

using ParserTest = FrontendTest;

//===----------------------------------------------------------------------===//
// Declarations and declarators.
//===----------------------------------------------------------------------===//

TEST_F(ParserTest, SimpleVariableDeclarations) {
  Parse("int x; unsigned long y; const char c; double d;");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("x"), "int");
  EXPECT_EQ(TypeOf("y"), "unsigned long");
  EXPECT_EQ(TypeOf("c"), "const char");
  EXPECT_EQ(TypeOf("d"), "double");
}

TEST_F(ParserTest, PointerDeclarators) {
  Parse("int *p; const char *s; int **pp; int *const cp;");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("p"), "int *");
  EXPECT_EQ(TypeOf("s"), "const char *");
  EXPECT_EQ(TypeOf("pp"), "int **");
  EXPECT_EQ(TypeOf("cp"), "int *const");
}

TEST_F(ParserTest, ArrayDeclarators) {
  Parse("int a[10]; char m[3][4]; int *ap[5]; int (*pa)[5];");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("a"), "int [10]");
  EXPECT_EQ(TypeOf("m"), "char [3][4]");
  EXPECT_EQ(TypeOf("ap"), "int *[5]");
  EXPECT_EQ(TypeOf("pa"), "int (*)[5]");
}

TEST_F(ParserTest, FunctionDeclarators) {
  Parse("int f(void); int g(int, char *); int (*fp)(int); "
        "int (*fpa[4])(void); long h();");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("f"), "int (void)");
  EXPECT_EQ(TypeOf("g"), "int (int, char *)");
  EXPECT_EQ(TypeOf("fp"), "int (*)(int)");
  EXPECT_EQ(TypeOf("fpa"), "int (*[4])(void)");
  EXPECT_EQ(TypeOf("h"), "long ()");
}

TEST_F(ParserTest, VariadicFunction) {
  Parse("int printf(const char *fmt, ...);");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("printf"), "int (const char *, ...)");
}

TEST_F(ParserTest, TypedefDisambiguation) {
  Parse("typedef int T; T x; int T2; T *ptr;");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("x"), "T");
  // T * y parses as a declaration (pointer declarator), not multiplication.
  EXPECT_EQ(TypeOf("ptr"), "T *");
}

TEST_F(ParserTest, TypedefNameShadowedByDeclarator) {
  // The second T is a declarator-id, not a type specifier.
  Parse("typedef int T; void f(void) { int T; T = 3; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, StructDefinitionAndFields) {
  Parse("struct point { int x, y; unsigned flags : 3; };");
  EXPECT_EQ(NumErrors(), 0u);
  const auto* record = FindDecl("point")->As<RecordDecl>();
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->IsCompleteDefinition());
  ASSERT_EQ(record->GetFields().size(), 3u);
  EXPECT_TRUE(record->GetFields()[2]->IsBitField());
  EXPECT_EQ(record->GetFields()[2]->GetBitWidth(), 3u);
}

TEST_F(ParserTest, AnonymousStructMember) {
  Parse("struct outer { struct { int inner; }; int tail; } o;"
        "int use(void) { extern struct outer o; return o.inner; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, EnumDefinition) {
  Parse("enum color { RED, GREEN = 5, BLUE };");
  EXPECT_EQ(NumErrors(), 0u);
  const auto* ed = FindDecl("color")->As<EnumDecl>();
  ASSERT_NE(ed, nullptr);
  ASSERT_EQ(ed->GetEnumerators().size(), 3u);
  EXPECT_EQ(ed->GetEnumerators()[0]->GetValue(), 0);
  EXPECT_EQ(ed->GetEnumerators()[1]->GetValue(), 5);
  EXPECT_EQ(ed->GetEnumerators()[2]->GetValue(), 6);
}

TEST_F(ParserTest, FunctionDefinitionWithBody) {
  Parse("int add(int a, int b) { return a + b; }");
  EXPECT_EQ(NumErrors(), 0u);
  const auto* fd = FindDecl("add")->As<FunctionDecl>();
  ASSERT_NE(fd, nullptr);
  EXPECT_TRUE(fd->IsDefined());
  ASSERT_EQ(fd->GetParams().size(), 2u);
  EXPECT_EQ(fd->GetParams()[0]->GetName(), "a");
}

TEST_F(ParserTest, StaticAssertDeclaration) {
  Parse("_Static_assert(1 + 1 == 2, \"math works\");");
  EXPECT_EQ(NumErrors(), 0u);
  Parse("_Static_assert(0, \"nope\");");
  EXPECT_TRUE(HasDiag(diag::err_static_assert_failed));
}

//===----------------------------------------------------------------------===//
// Expressions.
//===----------------------------------------------------------------------===//

TEST_F(ParserTest, PrecedenceAndAssociativity) {
  Parse("int x = 2 + 3 * 4;");
  EXPECT_EQ(NumErrors(), 0u);
  // 2 + (3 * 4): the '+' is the root.
  std::string dump = DumpAST();
  std::size_t plus = dump.find("'+'");
  std::size_t star = dump.find("'*'");
  ASSERT_NE(plus, std::string::npos);
  ASSERT_NE(star, std::string::npos);
  EXPECT_LT(plus, star);
}

TEST_F(ParserTest, AssignmentIsRightAssociative) {
  Parse("void f(void) { int a, b, c; a = b = c = 1; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, ConditionalOperator) {
  Parse("int x = 1 ? 2 : 3;");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, CastVsParenExpr) {
  Parse("typedef int T; int a = (T)1.5; int f(int c) { return (c) + 1; }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("CStyleCastExpr"), std::string::npos);
}

TEST_F(ParserTest, CompoundLiteral) {
  Parse("struct p { int x, y; }; struct p q = (struct p){1, 2};");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("CompoundLiteralExpr"), std::string::npos);
}

TEST_F(ParserTest, SizeofForms) {
  Parse("unsigned long a = sizeof(int); unsigned long b = sizeof a;"
        "unsigned long c = sizeof(int[4]); unsigned long d = _Alignof(double);");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, GenericSelection) {
  Parse("int i; int x = _Generic(i, int: 1, double: 2, default: 3);");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, StringConcatenation) {
  Parse("const char *s = \"foo\" \"bar\";");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("\"foobar\""), std::string::npos);
}

TEST_F(ParserTest, ExpectedExpressionError) {
  Parse("int x = +;");
  EXPECT_TRUE(HasDiag(diag::err_expected_expression));
}

TEST_F(ParserTest, UnbalancedParenNote) {
  Parse("int x = (1 + 2;");
  EXPECT_TRUE(HasDiag(diag::err_expected_rparen));
  EXPECT_TRUE(HasDiag(diag::note_to_match_this_lparen));
}

//===----------------------------------------------------------------------===//
// Statements.
//===----------------------------------------------------------------------===//

TEST_F(ParserTest, AllStatementKinds) {
  Parse(R"(
    int f(int n) {
      int total = 0;
      if (n > 0) total++; else total--;
      while (n > 0) n--;
      do { n++; } while (n < 3);
      for (int i = 0; i < 10; i++) { if (i == 5) continue; total += i; }
      switch (n) {
        case 0: total = 1; break;
        case 1: case 2: total = 2; break;
        default: total = 3;
      }
      goto done;
    done:
      return total;
    }
  )");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, ForScopeIsolation) {
  // The loop variable of one for statement is not visible after it.
  Parse("void f(void) { for (int i = 0; i < 3; i++) {} i = 1; }");
  EXPECT_TRUE(HasDiag(diag::err_undeclared_var_use));
}

TEST_F(ParserTest, LabelAndGoto) {
  Parse("void f(void) { goto out; out: ; }");
  EXPECT_EQ(NumErrors(), 0u);
  Parse("void f(void) { goto nowhere; }");
  EXPECT_TRUE(HasDiag(diag::err_undeclared_label_use));
}

TEST_F(ParserTest, DanglingElseBindsToInnermostIf) {
  Parse("void f(int a, int b) { if (a) if (b) a = 1; else a = 2; }");
  EXPECT_EQ(NumErrors(), 0u);
  // The else belongs to the inner if: the outer IfStmt has no else.
  std::string dump = DumpAST();
  std::size_t first_if = dump.find("IfStmt");
  ASSERT_NE(first_if, std::string::npos);
  EXPECT_EQ(dump.find("IfStmt has_else", first_if), dump.find("IfStmt", first_if + 1));
}

//===----------------------------------------------------------------------===//
// Initializers.
//===----------------------------------------------------------------------===//

TEST_F(ParserTest, ArraySizeDeductionFromInitList) {
  Parse("int a[] = {1, 2, 3};");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("a"), "int [3]");
}

TEST_F(ParserTest, DesignatedArrayInit) {
  Parse("int a[] = {1, [4] = 9};");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("a"), "int [5]");
}

TEST_F(ParserTest, DesignatedFieldInit) {
  Parse("struct p { int x, y; }; struct p q = {.y = 2, .x = 1};");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, StringArrayInit) {
  Parse("char s[] = \"hi\"; char t[2] = \"hi\"; char u[10] = \"hi\";");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("s"), "char [3]");
}

TEST_F(ParserTest, NestedBracedInit) {
  Parse("int m[2][2] = {{1, 2}, {3, 4}}; int f[2][2] = {1, 2, 3, 4};");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(ParserTest, ExcessInitializerWarning) {
  Parse("int a[2] = {1, 2, 3};");
  EXPECT_TRUE(HasDiag(diag::err_excess_initializers));
}

}  // namespace
}  // namespace bcc
