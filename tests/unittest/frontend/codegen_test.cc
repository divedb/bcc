#include <sstream>
#include <string>

#include "bcc/codegen/codegen_module.hh"
#include "bcc/ir/ir_printer.hh"
#include "bcc/ir/module.hh"
#include "frontend_fixture.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

/// Lowers a source string through PP -> Parser -> Sema -> CodeGen and
/// returns the printed IR.
class CodeGenTest : public FrontendTest {
 protected:
  std::string EmitIR(std::string_view code) {
    Parse(code);
    EXPECT_EQ(NumErrors(), 0u) << "input does not sema-check";
    module_ = std::make_unique<ir::Module>();
    codegen::CodeGenModule cgm(*ctx_, *diags_, *module_);
    cgm.EmitTranslationUnit();
    std::ostringstream os;
    ir::IRPrinter printer(os);
    printer.Print(*module_);
    return os.str();
  }

  static bool Contains(const std::string& ir, std::string_view needle) {
    return ir.find(needle) != std::string::npos;
  }

  std::unique_ptr<ir::Module> module_;
};

TEST_F(CodeGenTest, ReturnConstant) {
  std::string ir = EmitIR("int main(void) { return 42; }");
  EXPECT_TRUE(Contains(ir, "define i32 @main()"));
  EXPECT_TRUE(Contains(ir, "store i32 42, ptr %retval"));
  EXPECT_TRUE(Contains(ir, "ret i32"));
}

TEST_F(CodeGenTest, ParamsBecomeAllocas) {
  std::string ir = EmitIR("int add(int a, int b) { return a + b; }");
  EXPECT_TRUE(Contains(ir, "define i32 @add(i32 %a, i32 %b)"));
  EXPECT_TRUE(Contains(ir, "%a.addr = alloca i32, align 4"));
  EXPECT_TRUE(Contains(ir, "store i32 %a, ptr %a.addr"));
  EXPECT_TRUE(Contains(ir, "add i32"));
}

TEST_F(CodeGenTest, LocalVarInit) {
  std::string ir = EmitIR("int f(void) { int x = 3; return x; }");
  EXPECT_TRUE(Contains(ir, "%x = alloca i32, align 4"));
  EXPECT_TRUE(Contains(ir, "store i32 3, ptr %x"));
  EXPECT_TRUE(Contains(ir, "load i32, ptr %x"));
}

TEST_F(CodeGenTest, IntegralCasts) {
  std::string ir = EmitIR(
      "long f(short s, unsigned char c) { return s + c; }");
  EXPECT_TRUE(Contains(ir, "sext i16"));   // short -> int
  EXPECT_TRUE(Contains(ir, "zext i8"));    // unsigned char -> int
  EXPECT_TRUE(Contains(ir, "sext i32"));   // int result -> long
}

TEST_F(CodeGenTest, FloatConversions) {
  std::string ir = EmitIR(
      "double f(int i, float g) { return i + (double)g; }");
  EXPECT_TRUE(Contains(ir, "sitofp i32"));
  EXPECT_TRUE(Contains(ir, "fpext float"));
  EXPECT_TRUE(Contains(ir, "fadd double"));
}

TEST_F(CodeGenTest, UnsignedDivision) {
  std::string ir = EmitIR(
      "unsigned f(unsigned a, unsigned b) { return a / b % 3u; }");
  EXPECT_TRUE(Contains(ir, "udiv i32"));
  EXPECT_TRUE(Contains(ir, "urem i32"));
}

TEST_F(CodeGenTest, IfElseCFG) {
  std::string ir = EmitIR(
      "int f(int x) { if (x > 0) return 1; else return 2; }");
  EXPECT_TRUE(Contains(ir, "icmp sgt i32"));
  EXPECT_TRUE(Contains(ir, "if.then:"));
  EXPECT_TRUE(Contains(ir, "if.else:"));
  EXPECT_TRUE(Contains(ir, "br i1 %cmp, label %if.then, label %if.else"));
}

TEST_F(CodeGenTest, WhileLoopCFG) {
  std::string ir = EmitIR(
      "int f(int n) { int s = 0; while (n) { s += n; n--; } return s; }");
  EXPECT_TRUE(Contains(ir, "while.cond:"));
  EXPECT_TRUE(Contains(ir, "while.body:"));
  EXPECT_TRUE(Contains(ir, "while.end:"));
}

TEST_F(CodeGenTest, ForLoopBreakContinue) {
  std::string ir = EmitIR(
      "int f(void) {"
      "  int s = 0;"
      "  for (int i = 0; i < 10; i++) {"
      "    if (i == 3) continue;"
      "    if (i == 7) break;"
      "    s += i;"
      "  }"
      "  return s;"
      "}");
  EXPECT_TRUE(Contains(ir, "for.cond:"));
  EXPECT_TRUE(Contains(ir, "for.inc:"));
  EXPECT_TRUE(Contains(ir, "for.end:"));
  EXPECT_TRUE(Contains(ir, "br label %for.inc"));  // continue
  EXPECT_TRUE(Contains(ir, "br label %for.end"));  // break
}

TEST_F(CodeGenTest, SwitchLowering) {
  std::string ir = EmitIR(
      "int f(int x) {"
      "  switch (x) {"
      "  case 1: return 10;"
      "  case 2: return 20;"
      "  default: return -1;"
      "  }"
      "}");
  EXPECT_TRUE(Contains(ir, "switch i32"));
  EXPECT_TRUE(Contains(ir, "i32 1, label %sw.bb"));
  EXPECT_TRUE(Contains(ir, "i32 2, label %sw.bb1"));
  EXPECT_TRUE(Contains(ir, "label %sw.default"));
}

TEST_F(CodeGenTest, LogicalAndProducesPhi) {
  std::string ir = EmitIR("int f(int a, int b) { return a && b; }");
  EXPECT_TRUE(Contains(ir, "land.rhs:"));
  EXPECT_TRUE(Contains(ir, "phi i1"));
  EXPECT_TRUE(Contains(ir, "[ false,"));
}

TEST_F(CodeGenTest, ConditionalProducesPhi) {
  std::string ir = EmitIR("int f(int c) { return c ? 1 : 2; }");
  EXPECT_TRUE(Contains(ir, "cond.true:"));
  EXPECT_TRUE(Contains(ir, "cond.false:"));
  EXPECT_TRUE(Contains(ir, "phi i32 [ 1,"));
}

TEST_F(CodeGenTest, PointerArithmeticIsGEP) {
  std::string ir = EmitIR(
      "int f(int *p, long n) { return *(p + n) + p[2]; }");
  EXPECT_TRUE(Contains(ir, "getelementptr inbounds i32, ptr"));
  EXPECT_TRUE(Contains(ir, "arrayidx"));
}

TEST_F(CodeGenTest, ArrayDecayAndSubscript) {
  std::string ir = EmitIR(
      "int f(void) { int a[4]; a[1] = 5; return a[1]; }");
  EXPECT_TRUE(Contains(ir, "alloca [4 x i32]"));
  EXPECT_TRUE(Contains(ir, "getelementptr inbounds i32, ptr %a, i64 1"));
}

TEST_F(CodeGenTest, StructFieldAccess) {
  std::string ir = EmitIR(
      "struct P { char c; int x; };"
      "int f(struct P *p) { p->x = 3; return p->x; }");
  EXPECT_TRUE(Contains(ir, "%struct.P = type { i8, [3 x i8], i32 }"));
  EXPECT_TRUE(
      Contains(ir, "getelementptr inbounds %struct.P, ptr %0, i32 0, i32 2"));
}

TEST_F(CodeGenTest, UnionIsByteBag) {
  std::string ir = EmitIR(
      "union U { int i; float f; };"
      "float f(union U *u) { u->i = 1; return u->f; }");
  EXPECT_TRUE(Contains(ir, "%union.U = type { [4 x i8] }"));
  EXPECT_TRUE(Contains(ir, "store i32 1, ptr"));
  EXPECT_TRUE(Contains(ir, "load float, ptr"));
}

TEST_F(CodeGenTest, BitFieldLoadStore) {
  std::string ir = EmitIR(
      "struct B { unsigned a : 3; int b : 5; };"
      "int f(struct B *p) { p->a = 6; return p->b; }");
  // Store: mask to 3 bits, merge into the unit.
  EXPECT_TRUE(Contains(ir, "and i32"));
  EXPECT_TRUE(Contains(ir, "or i32"));
  // Signed read: shl + ashr.
  EXPECT_TRUE(Contains(ir, "shl i32"));
  EXPECT_TRUE(Contains(ir, "ashr i32"));
}

TEST_F(CodeGenTest, StructAssignmentIsAggregateCopy) {
  std::string ir = EmitIR(
      "struct S { int a, b; };"
      "void f(struct S *d, struct S *s) { *d = *s; }");
  EXPECT_TRUE(Contains(ir, "load %struct.S, ptr"));
  EXPECT_TRUE(Contains(ir, "store %struct.S"));
}

TEST_F(CodeGenTest, GlobalsAndLinkage) {
  std::string ir = EmitIR(
      "int g = 5;"
      "static int s = 7;"
      "int t;"
      "extern int e;"
      "int use(void) { return g + s + t + e; }");
  EXPECT_TRUE(Contains(ir, "@g = global i32 5, align 4"));
  EXPECT_TRUE(Contains(ir, "@s = internal global i32 7, align 4"));
  EXPECT_TRUE(Contains(ir, "@t = global i32 0, align 4"));
  EXPECT_TRUE(Contains(ir, "@e = external global i32, align 4"));
}

TEST_F(CodeGenTest, GlobalForwardReference) {
  std::string ir = EmitIR("int x; int *p = &x; int x;");
  EXPECT_TRUE(Contains(ir, "@p = global ptr @x"));
}

TEST_F(CodeGenTest, StaticLocalNamedAfterFunction) {
  std::string ir = EmitIR(
      "int counter(void) { static int n = 3; return n++; }");
  EXPECT_TRUE(Contains(ir, "@counter.n = internal global i32 3"));
}

TEST_F(CodeGenTest, StringLiteralUniqued) {
  std::string ir = EmitIR(
      "char *f(void) { return \"hi\"; }"
      "char *g(void) { return \"hi\"; }");
  EXPECT_TRUE(Contains(
      ir, "@.str = private unnamed_addr constant [3 x i8] c\"hi\\00\""));
  // Same literal, one global.
  EXPECT_FALSE(Contains(ir, "@.str.1"));
}

TEST_F(CodeGenTest, CallsAndDeclare) {
  std::string ir = EmitIR(
      "int ext(int, double);"
      "int f(void) { return ext(1, 2.5); }");
  EXPECT_TRUE(Contains(ir, "call i32 @ext(i32 1, double"));
  EXPECT_TRUE(Contains(ir, "declare i32 @ext(i32, double)"));
}

TEST_F(CodeGenTest, VariadicCall) {
  std::string ir = EmitIR(
      "int printf(const char *, ...);"
      "int f(int x) { return printf(\"%d\", x); }");
  EXPECT_TRUE(Contains(ir, "call i32 (ptr, ...) @printf(ptr @.str"));
  EXPECT_TRUE(Contains(ir, "declare i32 @printf(ptr, ...)"));
}

TEST_F(CodeGenTest, UnusedDeclarationNotPrinted) {
  std::string ir = EmitIR(
      "int unused_fn(void);"
      "int f(void) { return 1; }");
  EXPECT_FALSE(Contains(ir, "unused_fn"));
}

TEST_F(CodeGenTest, BoolIsI8InMemory) {
  std::string ir = EmitIR(
      "_Bool f(int x) { _Bool b = x; return !b; }");
  EXPECT_TRUE(Contains(ir, "alloca i8, align 1"));
  EXPECT_TRUE(Contains(ir, "icmp ne i32"));
  EXPECT_TRUE(Contains(ir, "zext i1"));
  EXPECT_TRUE(Contains(ir, "store i8"));
}

TEST_F(CodeGenTest, AggregateInitList) {
  std::string ir = EmitIR(
      "struct S { int a, b; };"
      "int f(int x) { struct S s = {x, 2}; int a[3] = {1}; "
      "  return s.a + a[0]; }");
  // Non-constant element: zero skeleton + element store.
  EXPECT_TRUE(Contains(ir, "store %struct.S zeroinitializer"));
  // Fully constant array list: single constant store.
  EXPECT_TRUE(Contains(ir, "[ i32 1, i32 0, i32 0 ]"));
}

TEST_F(CodeGenTest, GlobalAggregateInit) {
  std::string ir = EmitIR(
      "struct S { char c; int i; };"
      "struct S gs = { 'a', 400 };"
      "int ga[4] = {1, 2};"
      "char msg[8] = \"hi\";");
  EXPECT_TRUE(Contains(
      ir, "@gs = global %struct.S { i8 97, [3 x i8] zeroinitializer, "
          "i32 400 }"));
  EXPECT_TRUE(
      Contains(ir, "@ga = global [4 x i32] [ i32 1, i32 2, i32 0, i32 0 ]"));
  EXPECT_TRUE(Contains(ir, "c\"hi\\00\\00\\00\\00\\00\\00\""));
}

TEST_F(CodeGenTest, ReturnBlockShared) {
  std::string ir = EmitIR(
      "int f(int x) { if (x) return 1; return 0; }");
  EXPECT_TRUE(Contains(ir, "return:"));
  // Both returns branch to the shared epilogue.
  size_t first = ir.find("br label %return");
  size_t last = ir.rfind("br label %return");
  EXPECT_NE(first, std::string::npos);
  EXPECT_NE(first, last);
}

TEST_F(CodeGenTest, GotoAndLabels) {
  std::string ir = EmitIR(
      "int f(int x) { if (x) goto out; x = 1; out: return x; }");
  EXPECT_TRUE(Contains(ir, "br label %out"));
  EXPECT_TRUE(Contains(ir, "out:"));
}

TEST_F(CodeGenTest, CompoundAssignPointer) {
  std::string ir = EmitIR("int *f(int *p, int n) { p += n; return p; }");
  EXPECT_TRUE(Contains(ir, "getelementptr inbounds i32, ptr"));
}

TEST_F(CodeGenTest, PointerDifference) {
  std::string ir = EmitIR("long f(int *a, int *b) { return a - b; }");
  EXPECT_TRUE(Contains(ir, "ptrtoint ptr"));
  EXPECT_TRUE(Contains(ir, "sdiv i64 %sub.ptr.sub, 4"));
}

TEST_F(CodeGenTest, VLADiagnosed) {
  Parse("void f(int n) { int a[n]; a[0] = 1; }");
  ASSERT_EQ(NumErrors(), 0u);
  ir::Module module;
  codegen::CodeGenModule cgm(*ctx_, *diags_, module);
  cgm.EmitTranslationUnit();
  EXPECT_TRUE(HasDiag(diag::err_codegen_cannot_compile));
}

}  // namespace
}  // namespace bcc
