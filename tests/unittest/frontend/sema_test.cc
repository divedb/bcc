#include "frontend_fixture.hh"

namespace bcc {
namespace {

using SemaTest = FrontendTest;

//===----------------------------------------------------------------------===//
// Name lookup and redeclaration.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, UndeclaredIdentifier) {
  Parse("void f(void) { x = 1; }");
  EXPECT_TRUE(HasDiag(diag::err_undeclared_var_use));
}

TEST_F(SemaTest, RedefinitionInSameScope) {
  Parse("void f(void) { int x; int x; }");
  EXPECT_TRUE(HasDiag(diag::err_redefinition));
}

TEST_F(SemaTest, ShadowingInNestedScope) {
  Parse("int x; void f(void) { int x; { int x; } }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, FileScopeRedeclarationMerges) {
  Parse("int x; int x; extern int x;");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, ConflictingRedeclaration) {
  Parse("int x; long x;");
  EXPECT_TRUE(HasDiag(diag::err_redefinition_different_type));
}

TEST_F(SemaTest, DifferentKindRedefinition) {
  Parse("int x; void f(void) { } int f;");
  EXPECT_TRUE(HasDiag(diag::err_redefinition_different_kind));
}

TEST_F(SemaTest, StaticAfterNonStatic) {
  Parse("int f(void); static int f(void);");
  EXPECT_TRUE(HasDiag(diag::err_static_non_static));
}

TEST_F(SemaTest, FunctionRedefinition) {
  Parse("int f(void) { return 0; } int f(void) { return 1; }");
  EXPECT_TRUE(HasDiag(diag::err_redefinition));
}

TEST_F(SemaTest, TagAndOrdinaryNamespacesAreSeparate) {
  Parse("struct s { int x; }; int s; struct s v;");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, EnumConstantConflict) {
  Parse("enum e { A }; int A;");
  EXPECT_TRUE(HasDiag(diag::err_redefinition_different_kind));
}

//===----------------------------------------------------------------------===//
// Conversions.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, UsualArithmeticConversions) {
  Parse("short s; int i; long l; unsigned u; float f; double d;"
        "void g(void) {"
        "  int r1 = s + s;"       // promoted to int
        "  long r2 = i + l;"      // -> long
        "  unsigned r3 = i + u;"  // -> unsigned
        "  double r4 = f + d;"    // -> double
        "  float r5 = i + f;"     // -> float
        "}");
  EXPECT_EQ(NumErrors(), 0u);
  std::string dump = DumpAST();
  // s + s promotes both shorts to int.
  EXPECT_NE(dump.find("ImplicitCastExpr 'int' <IntegralCast>"),
            std::string::npos);
  EXPECT_NE(dump.find("ImplicitCastExpr 'double' <FloatingCast>"),
            std::string::npos);
  EXPECT_NE(dump.find("<IntegralToFloating>"), std::string::npos);
}

TEST_F(SemaTest, ArrayDecayAndFunctionDecay) {
  Parse("int a[3]; int *p = a; int f(void); int (*fp)(void) = f;");
  EXPECT_EQ(NumErrors(), 0u);
  std::string dump = DumpAST();
  EXPECT_NE(dump.find("<ArrayToPointerDecay>"), std::string::npos);
  EXPECT_NE(dump.find("<FunctionToPointerDecay>"), std::string::npos);
}

TEST_F(SemaTest, NullPointerConstant) {
  Parse("int *p = 0; void *q = (void *)0;"
        "void f(void) { int *r = q; }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("<NullToPointer>"), std::string::npos);
}

TEST_F(SemaTest, IntPointerConversionWarning) {
  Parse("int *p; void f(void) { p = 5; }");
  EXPECT_TRUE(HasDiag(diag::warn_typecheck_convert_int_pointer));
}

TEST_F(SemaTest, IncompatiblePointerWarning) {
  Parse("int *p; long *q; void f(void) { p = q; }");
  EXPECT_TRUE(HasDiag(diag::warn_typecheck_convert_incompatible_pointer));
}

TEST_F(SemaTest, DiscardsQualifiersWarning) {
  Parse("const int *cp; int *p; void f(void) { p = cp; }");
  EXPECT_TRUE(HasDiag(diag::warn_typecheck_convert_discards_qualifiers));
}

TEST_F(SemaTest, VoidPointerImplicitConversion) {
  Parse("void *v; int *p; void f(void) { p = v; v = p; }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(NumWarnings(), 0u);
}

//===----------------------------------------------------------------------===//
// Operators.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, InvalidBinaryOperands) {
  Parse("struct s { int x; } v; int r = v + 1;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_invalid_operands));
}

TEST_F(SemaTest, PointerArithmetic) {
  Parse("int a[10]; int *p = a + 2;"
        "void f(void) { long d = (a + 5) - p; int x = *(p + 1); }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, PointerSubtractionIncompatible) {
  Parse("int *p; long *q; long d = p - q;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_sub_ptr_compatible));
}

TEST_F(SemaTest, IndirectionRequiresPointer) {
  Parse("int x; int y = *x;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_indirection_requires_pointer));
}

TEST_F(SemaTest, AddressOfRvalue) {
  Parse("int f(void); int *p = &(1 + 2);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_invalid_lvalue_addrof));
}

TEST_F(SemaTest, AddressOfFunctionAndArray) {
  Parse("int f(void); int (*fp)(void) = &f; int a[3]; int (*ap)[3] = &a;");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, IncrementRequiresModifiableLValue) {
  Parse("void f(void) { int x = 1; x++; ++x; 5++; }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_expression_not_modifiable_lvalue));
}

TEST_F(SemaTest, AssignToConst) {
  Parse("void f(void) { const int c = 1; c = 2; }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_assign_const));
}

TEST_F(SemaTest, ArrayIsNotAssignable) {
  Parse("void f(void) { int a[3], b[3]; a = b; }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_expression_not_modifiable_lvalue));
}

TEST_F(SemaTest, ComparisonResultIsInt) {
  Parse("void f(void) { int r = 1.5 < 2.5; }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("BinaryOperator 'int' '<'"), std::string::npos);
}

TEST_F(SemaTest, LogicalNotResultIsInt) {
  Parse("double d; void f(void) { int r = !d; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, ShiftResultHasPromotedLHSType) {
  Parse("char c; long l; void f(void) { int r_ok = c << l; }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("BinaryOperator 'int' '<<'"), std::string::npos);
}

TEST_F(SemaTest, CompoundAssignPointer) {
  Parse("void f(void) { int a[4]; int *p = a; p += 2; p -= 1; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, ConditionalPointerMismatch) {
  Parse("int i; int *p; double *q; void f(void) { (void)(i ? p : q); }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_cond_incompatible_operands));
}

TEST_F(SemaTest, ConditionalWithVoidPointer) {
  Parse("int i; int *p; void *v; void f(void) { (void)(i ? p : v); }");
  EXPECT_EQ(NumErrors(), 0u);
}

//===----------------------------------------------------------------------===//
// Calls.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, CallArgumentCount) {
  Parse("int f(int, int); int a = f(1);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_call_too_few_args));
  Parse("int f(int); int a = f(1, 2);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_call_too_many_args));
}

TEST_F(SemaTest, CallArgumentConversion) {
  Parse("int f(double); void g(void) { int a = f(1); }");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_NE(DumpAST().find("<IntegralToFloating>"), std::string::npos);
}

TEST_F(SemaTest, VariadicArgumentPromotion) {
  Parse("int printf(const char *, ...); float g;"
        "void f(void) { printf(\"%f\", g); }");
  EXPECT_EQ(NumErrors(), 0u);
  // float passed through ... promotes to double.
  EXPECT_NE(DumpAST().find("ImplicitCastExpr 'double' <FloatingCast>"),
            std::string::npos);
}

TEST_F(SemaTest, CallThroughFunctionPointer) {
  Parse("int f(int); int (*fp)(int) = f;"
        "void g(void) { int a = fp(3); int b = (*fp)(4); }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, CallNonFunction) {
  Parse("int x; int y = x(1);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_call_not_function));
}

TEST_F(SemaTest, ImplicitFunctionDeclIsError) {
  Parse("void f(void) { g(); }");
  EXPECT_TRUE(HasDiag(diag::err_implicit_function_decl));
}

//===----------------------------------------------------------------------===//
// Members.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, MemberAccess) {
  Parse("struct p { int x; }; struct p v; struct p *pp = &v;"
        "void f(void) { int a = v.x; int b = pp->x; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, NoSuchMember) {
  Parse("struct p { int x; }; struct p v; int a = v.z;");
  EXPECT_TRUE(HasDiag(diag::err_no_member));
}

TEST_F(SemaTest, MemberOfConstStructIsConst) {
  Parse("struct p { int x; }; const struct p v; void f(void) { v.x = 1; }");
  EXPECT_TRUE(NumErrors() > 0);
}

TEST_F(SemaTest, ArrowOnNonPointerAndDotOnPointer) {
  Parse("struct p { int x; }; struct p v; int a = v->x;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_member_reference_arrow));
  Parse("struct p { int x; }; struct p *v; int a = v.x;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_member_reference_suggestion));
}

TEST_F(SemaTest, IncompleteStructMemberAccess) {
  Parse("struct s *p; int use(void) { return p->x; }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_incomplete_tag));
}

//===----------------------------------------------------------------------===//
// sizeof / layout.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, SizeofValues) {
  Parse("_Static_assert(sizeof(char) == 1, \"\");"
        "_Static_assert(sizeof(short) == 2, \"\");"
        "_Static_assert(sizeof(int) == 4, \"\");"
        "_Static_assert(sizeof(long) == 8, \"\");"
        "_Static_assert(sizeof(void *) == 8, \"\");"
        "_Static_assert(sizeof(double) == 8, \"\");"
        "_Static_assert(sizeof(int[10]) == 40, \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, StructLayout) {
  Parse("struct a { char c; int i; };"
        "_Static_assert(sizeof(struct a) == 8, \"\");"
        "struct b { char c; double d; char e; };"
        "_Static_assert(sizeof(struct b) == 24, \"\");"
        "union u { char c; int i; };"
        "_Static_assert(sizeof(union u) == 4, \"\");"
        "struct bf { unsigned a : 3; unsigned b : 29; unsigned c : 1; };"
        "_Static_assert(sizeof(struct bf) == 8, \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, AlignofValues) {
  Parse("_Static_assert(_Alignof(char) == 1, \"\");"
        "_Static_assert(_Alignof(int) == 4, \"\");"
        "_Static_assert(_Alignof(double) == 8, \"\");"
        "struct s { char c; long l; };"
        "_Static_assert(_Alignof(struct s) == 8, \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, SizeofErrors) {
  Parse("int f(void); unsigned long a = sizeof(f);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_sizeof_function));
  Parse("struct s; unsigned long a = sizeof(struct s);");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_sizeof_incomplete));
}

//===----------------------------------------------------------------------===//
// Tags and fields.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, StructRedefinition) {
  Parse("struct s { int a; }; struct s { int b; };");
  EXPECT_TRUE(HasDiag(diag::err_redefinition));
}

TEST_F(SemaTest, ForwardDeclarationThenDefinition) {
  Parse("struct s; struct s *p; struct s { int x; }; struct s v;"
        "int use(void) { return p == &v; }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, WrongTagKind) {
  Parse("struct s { int x; }; union s u;");
  EXPECT_TRUE(HasDiag(diag::err_use_with_wrong_tag));
}

TEST_F(SemaTest, FlexibleArrayMember) {
  Parse("struct ok { int n; int data[]; };");
  EXPECT_EQ(NumErrors(), 0u);
  Parse("struct bad { int data[]; };");
  EXPECT_TRUE(HasDiag(diag::err_flexible_array_empty_struct));
  Parse("struct bad2 { int data[]; int n; };");
  EXPECT_TRUE(HasDiag(diag::err_flexible_array_not_at_end));
}

TEST_F(SemaTest, BitFieldChecks) {
  Parse("struct s { double d : 3; };");
  EXPECT_TRUE(HasDiag(diag::err_bitfield_not_integer));
  Parse("struct s { int b : 33; };");
  EXPECT_TRUE(HasDiag(diag::err_bitfield_width_exceeds_type_width));
  Parse("struct s { int b : -1; };");
  EXPECT_TRUE(HasDiag(diag::err_bitfield_negative_width));
}

TEST_F(SemaTest, IncompleteFieldType) {
  Parse("struct s { struct t member; };");
  EXPECT_TRUE(HasDiag(diag::err_field_incomplete));
}

//===----------------------------------------------------------------------===//
// Statements.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, BreakContinueOutsideLoop) {
  Parse("void f(void) { break; }");
  EXPECT_TRUE(HasDiag(diag::err_break_not_in_loop_or_switch));
  Parse("void f(void) { continue; }");
  EXPECT_TRUE(HasDiag(diag::err_continue_not_in_loop));
}

TEST_F(SemaTest, BreakInSwitchContinueNot) {
  Parse("void f(int n) { switch (n) { case 1: break; } }");
  EXPECT_EQ(NumErrors(), 0u);
  Parse("void f(int n) { switch (n) { case 1: continue; } }");
  EXPECT_TRUE(HasDiag(diag::err_continue_not_in_loop));
}

TEST_F(SemaTest, DuplicateCase) {
  Parse("void f(int n) { switch (n) { case 1: case 1: break; } }");
  EXPECT_TRUE(HasDiag(diag::err_duplicate_case));
}

TEST_F(SemaTest, DuplicateDefault) {
  Parse("void f(int n) { switch (n) { default: default: break; } }");
  EXPECT_TRUE(HasDiag(diag::err_multiple_default_labels_defined));
}

TEST_F(SemaTest, CaseOutsideSwitch) {
  Parse("void f(void) { case 1: ; }");
  EXPECT_TRUE(HasDiag(diag::err_case_not_in_switch));
}

TEST_F(SemaTest, NonScalarCondition) {
  Parse("struct s { int x; } v; void f(void) { if (v) {} }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_statement_requires_scalar));
}

TEST_F(SemaTest, SwitchRequiresInteger) {
  Parse("void f(double d) { switch (d) {} }");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_statement_requires_integer));
}

TEST_F(SemaTest, ReturnChecks) {
  Parse("void f(void) { return 1; }");
  EXPECT_TRUE(HasDiag(diag::err_return_void_function));
  Parse("int f(void) { return; }");
  EXPECT_TRUE(HasDiag(diag::warn_return_missing_expr));
  Parse("int f(void) { return 1.5; }");
  EXPECT_EQ(NumErrors(), 0u);  // converted, no diagnostic
}

TEST_F(SemaTest, LabelRedefinition) {
  Parse("void f(void) { x: ; x: ; }");
  EXPECT_TRUE(HasDiag(diag::err_label_redefinition));
}

//===----------------------------------------------------------------------===//
// Initialization.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, StaticInitMustBeConstant) {
  Parse("int x; int y = x;");
  EXPECT_TRUE(HasDiag(diag::err_init_element_not_constant));
  Parse("int x; int *p = &x; int a[3]; int *q = a;");
  EXPECT_EQ(NumErrors(), 0u);  // address constants are fine
}

TEST_F(SemaTest, LocalInitNeedNotBeConstant) {
  Parse("int g(void); void f(void) { int x = g(); }");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, StringInitTooLong) {
  Parse("char s[2] = \"abc\";");
  EXPECT_TRUE(HasDiag(diag::err_array_init_string_too_long));
}

TEST_F(SemaTest, UnionInit) {
  Parse("union u { int i; double d; };"
        "union u a = {1}; union u b = {.d = 2.5};");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, ScalarBracedInit) {
  Parse("int x = {3};");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, TentativeDefinitionCompletedAtEnd) {
  Parse("int a[]; int a[4];");
  EXPECT_EQ(NumErrors(), 0u);
  EXPECT_EQ(TypeOf("a"), "int [4]");
}

//===----------------------------------------------------------------------===//
// Constant expressions.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, ICEArithmetic) {
  Parse("_Static_assert((3 + 4) * 2 == 14, \"\");"
        "_Static_assert(1 << 4 == 16, \"\");"
        "_Static_assert(-7 / 2 == -3, \"\");"
        "_Static_assert(1 ? 2 : 3, \"\");"
        "_Static_assert('A' == 65, \"\");"
        "_Static_assert((char)257 == 1, \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, ICEUsesEnumConstants) {
  Parse("enum e { A = 3, B }; int arr[B]; "
        "_Static_assert(sizeof(arr) == 16, \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, NonICEArrayBoundAtFileScope) {
  Parse("int n; int arr[n];");
  // A VLA at file scope: represented, and rejected when sized.
  EXPECT_EQ(TypeOf("arr"), "int [*]");
}

TEST_F(SemaTest, CaseLabelMustBeICE) {
  Parse("void f(int n, int v) { switch (n) { case v: break; } }");
  EXPECT_TRUE(HasDiag(diag::err_case_label_not_ice));
}

//===----------------------------------------------------------------------===//
// _Generic.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, GenericSelectsByType) {
  Parse("double d;"
        "_Static_assert(_Generic(1, int: 1, double: 2, default: 3) == 1, \"\");"
        "_Static_assert(_Generic(d, int: 1, double: 2, default: 3) == 2, \"\");"
        "_Static_assert(_Generic((float)1, int: 1, double: 2, default: 3) == 3,"
        " \"\");");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, GenericErrors) {
  Parse("int x = _Generic(1, default: 1, default: 2);");
  EXPECT_TRUE(HasDiag(diag::err_generic_multiple_default));
  Parse("int x = _Generic(1.5f, int: 1);");
  EXPECT_TRUE(HasDiag(diag::err_generic_no_match));
}

//===----------------------------------------------------------------------===//
// Casts.
//===----------------------------------------------------------------------===//

TEST_F(SemaTest, ValidCasts) {
  Parse("int i; double d; int *p;"
        "void f(void) {"
        "  (void)i; (double)i; (int)d; (long)p; (int *)0x1000; (char *)p;"
        "}");
  EXPECT_EQ(NumErrors(), 0u);
}

TEST_F(SemaTest, InvalidCasts) {
  Parse("double d; int *p = (int *)d;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_cast_illegal));
  Parse("struct s { int x; } v; int i = (int)v;");
  EXPECT_TRUE(HasDiag(diag::err_typecheck_cond_expect_scalar));
}

}  // namespace
}  // namespace bcc
