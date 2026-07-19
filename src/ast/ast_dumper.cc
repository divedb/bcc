#include "bcc/ast/ast_dumper.hh"

#include "bcc/ast/decl.hh"
#include "bcc/ast/expr.hh"
#include "bcc/ast/stmt.hh"
#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/source_manager.hh"

namespace bcc {

void ASTDumper::Indent(unsigned depth) {
  for (unsigned i = 0; i < depth; ++i) os_ << (i + 1 == depth ? "|-" : "| ");
}

void ASTDumper::PrintLoc(const Stmt* s) {
  if (!sm_) return;
  PresumedLoc p = sm_->GetPresumedLoc(s->GetBeginLoc());
  if (p.IsValid()) os_ << " <line:" << p.line << ":" << p.col << ">";
}

void ASTDumper::DumpDecl(const Decl* d, unsigned depth) {
  Indent(depth);
  switch (d->GetKind()) {
    case DeclKind::kTranslationUnit: {
      os_ << "TranslationUnitDecl\n";
      for (const Decl* child :
           static_cast<const TranslationUnitDecl*>(d)->GetDecls()) {
        DumpDecl(child, depth + 1);
      }
      return;
    }
    case DeclKind::kTypedef: {
      const auto* td = static_cast<const TypedefDecl*>(d);
      os_ << "TypedefDecl " << td->GetName() << " '"
          << td->GetType().GetAsString() << "'\n";
      return;
    }
    case DeclKind::kVar:
    case DeclKind::kParmVar: {
      const auto* vd = static_cast<const VarDecl*>(d);
      os_ << (d->GetKind() == DeclKind::kParmVar ? "ParmVarDecl "
                                                 : "VarDecl ");
      os_ << (vd->GetIdentifier() ? vd->GetName() : "<unnamed>") << " '"
          << vd->GetType().GetAsString() << "'";
      switch (vd->GetStorageClass()) {
        case StorageClass::kExtern: os_ << " extern"; break;
        case StorageClass::kStatic: os_ << " static"; break;
        case StorageClass::kRegister: os_ << " register"; break;
        default: break;
      }
      os_ << "\n";
      if (vd->HasInit()) DumpStmt(vd->GetInit(), depth + 1);
      return;
    }
    case DeclKind::kFunction: {
      const auto* fd = static_cast<const FunctionDecl*>(d);
      os_ << "FunctionDecl " << fd->GetName() << " '"
          << fd->GetType().GetAsString() << "'";
      if (fd->GetStorageClass() == StorageClass::kStatic) os_ << " static";
      if (fd->GetStorageClass() == StorageClass::kExtern) os_ << " extern";
      if (fd->IsInline()) os_ << " inline";
      os_ << "\n";
      for (const ParmVarDecl* p : fd->GetParams()) DumpDecl(p, depth + 1);
      if (fd->GetBody()) DumpStmt(fd->GetBody(), depth + 1);
      return;
    }
    case DeclKind::kField: {
      const auto* fd = static_cast<const FieldDecl*>(d);
      os_ << "FieldDecl "
          << (fd->GetIdentifier() ? fd->GetName() : "<unnamed>") << " '"
          << fd->GetType().GetAsString() << "'";
      if (fd->IsBitField()) os_ << " bitfield:" << fd->GetBitWidth();
      os_ << "\n";
      return;
    }
    case DeclKind::kRecord: {
      const auto* rd = static_cast<const RecordDecl*>(d);
      os_ << "RecordDecl " << (rd->IsUnion() ? "union" : "struct");
      if (rd->GetIdentifier()) os_ << " " << rd->GetName();
      if (rd->IsCompleteDefinition()) os_ << " definition";
      os_ << "\n";
      for (const FieldDecl* f : rd->GetFields()) DumpDecl(f, depth + 1);
      return;
    }
    case DeclKind::kEnum: {
      const auto* ed = static_cast<const EnumDecl*>(d);
      os_ << "EnumDecl";
      if (ed->GetIdentifier()) os_ << " " << ed->GetName();
      if (ed->IsCompleteDefinition()) os_ << " definition";
      os_ << "\n";
      for (const EnumConstantDecl* e : ed->GetEnumerators()) {
        DumpDecl(e, depth + 1);
      }
      return;
    }
    case DeclKind::kEnumConstant: {
      const auto* ec = static_cast<const EnumConstantDecl*>(d);
      os_ << "EnumConstantDecl " << ec->GetName() << " '"
          << ec->GetType().GetAsString() << "' " << ec->GetValue() << "\n";
      return;
    }
    case DeclKind::kLabel:
      os_ << "LabelDecl " << static_cast<const LabelDecl*>(d)->GetName()
          << "\n";
      return;
    case DeclKind::kStaticAssert: {
      const auto* sa = static_cast<const StaticAssertDecl*>(d);
      os_ << "StaticAssertDecl\n";
      DumpStmt(sa->GetCond(), depth + 1);
      return;
    }
  }
}

void ASTDumper::DumpStmt(const Stmt* s, unsigned depth) {
  Indent(depth);
  if (!s) {
    os_ << "<<<NULL>>>\n";
    return;
  }

  auto print_expr_header = [&](const char* name) {
    const auto* e = static_cast<const Expr*>(s);
    os_ << name << " '" << e->GetType().GetAsString() << "'"
        << (e->IsLValue() ? " lvalue" : "");
  };

  switch (s->GetStmtClass()) {
    case StmtClass::kNullStmt:
      os_ << "NullStmt\n";
      return;
    case StmtClass::kCompoundStmt: {
      os_ << "CompoundStmt\n";
      for (const Stmt* child : static_cast<const CompoundStmt*>(s)->GetBody()) {
        DumpStmt(child, depth + 1);
      }
      return;
    }
    case StmtClass::kDeclStmt: {
      os_ << "DeclStmt\n";
      for (const Decl* d : static_cast<const DeclStmt*>(s)->GetDecls()) {
        DumpDecl(d, depth + 1);
      }
      return;
    }
    case StmtClass::kIfStmt: {
      const auto* is = static_cast<const IfStmt*>(s);
      os_ << "IfStmt" << (is->GetElse() ? " has_else" : "") << "\n";
      DumpStmt(is->GetCond(), depth + 1);
      DumpStmt(is->GetThen(), depth + 1);
      if (is->GetElse()) DumpStmt(is->GetElse(), depth + 1);
      return;
    }
    case StmtClass::kWhileStmt: {
      const auto* ws = static_cast<const WhileStmt*>(s);
      os_ << "WhileStmt\n";
      DumpStmt(ws->GetCond(), depth + 1);
      DumpStmt(ws->GetBody(), depth + 1);
      return;
    }
    case StmtClass::kDoStmt: {
      const auto* ds = static_cast<const DoStmt*>(s);
      os_ << "DoStmt\n";
      DumpStmt(ds->GetBody(), depth + 1);
      DumpStmt(ds->GetCond(), depth + 1);
      return;
    }
    case StmtClass::kForStmt: {
      const auto* fs = static_cast<const ForStmt*>(s);
      os_ << "ForStmt\n";
      DumpStmt(fs->GetInit(), depth + 1);
      DumpStmt(fs->GetCond(), depth + 1);
      DumpStmt(fs->GetInc(), depth + 1);
      DumpStmt(fs->GetBody(), depth + 1);
      return;
    }
    case StmtClass::kSwitchStmt: {
      const auto* ss = static_cast<const SwitchStmt*>(s);
      os_ << "SwitchStmt\n";
      DumpStmt(ss->GetCond(), depth + 1);
      DumpStmt(ss->GetBody(), depth + 1);
      return;
    }
    case StmtClass::kCaseStmt: {
      const auto* cs = static_cast<const CaseStmt*>(s);
      os_ << "CaseStmt " << cs->GetValue() << "\n";
      DumpStmt(cs->GetSubStmt(), depth + 1);
      return;
    }
    case StmtClass::kDefaultStmt:
      os_ << "DefaultStmt\n";
      DumpStmt(static_cast<const DefaultStmt*>(s)->GetSubStmt(), depth + 1);
      return;
    case StmtClass::kBreakStmt:
      os_ << "BreakStmt\n";
      return;
    case StmtClass::kContinueStmt:
      os_ << "ContinueStmt\n";
      return;
    case StmtClass::kReturnStmt: {
      const auto* rs = static_cast<const ReturnStmt*>(s);
      os_ << "ReturnStmt\n";
      if (rs->GetValue()) DumpStmt(rs->GetValue(), depth + 1);
      return;
    }
    case StmtClass::kGotoStmt:
      os_ << "GotoStmt '"
          << static_cast<const GotoStmt*>(s)->GetLabel()->GetName() << "'\n";
      return;
    case StmtClass::kLabelStmt: {
      const auto* ls = static_cast<const LabelStmt*>(s);
      os_ << "LabelStmt '" << ls->GetLabel()->GetName() << "'\n";
      DumpStmt(ls->GetSubStmt(), depth + 1);
      return;
    }
    case StmtClass::kIntegerLiteral: {
      print_expr_header("IntegerLiteral");
      os_ << " " << static_cast<const IntegerLiteral*>(s)->GetValue() << "\n";
      return;
    }
    case StmtClass::kFloatingLiteral: {
      print_expr_header("FloatingLiteral");
      os_ << " " << static_cast<const FloatingLiteral*>(s)->GetValue() << "\n";
      return;
    }
    case StmtClass::kCharacterLiteral: {
      print_expr_header("CharacterLiteral");
      os_ << " " << static_cast<const CharacterLiteral*>(s)->GetValue()
          << "\n";
      return;
    }
    case StmtClass::kStringLiteral: {
      print_expr_header("StringLiteral");
      const auto* sl = static_cast<const StringLiteral*>(s);
      if (sl->GetCharByteWidth() == 1) {
        os_ << " \"";
        for (char c : sl->GetBytes()) {
          if (c == '\n') {
            os_ << "\\n";
          } else if (c == '"') {
            os_ << "\\\"";
          } else if (c == '\\') {
            os_ << "\\\\";
          } else {
            os_ << c;
          }
        }
        os_ << "\"";
      }
      os_ << "\n";
      return;
    }
    case StmtClass::kDeclRefExpr: {
      print_expr_header("DeclRefExpr");
      os_ << " '" << static_cast<const DeclRefExpr*>(s)->GetDecl()->GetName()
          << "'\n";
      return;
    }
    case StmtClass::kParenExpr:
      print_expr_header("ParenExpr");
      os_ << "\n";
      DumpStmt(static_cast<const ParenExpr*>(s)->GetSubExpr(), depth + 1);
      return;
    case StmtClass::kUnaryOperator: {
      const auto* uo = static_cast<const UnaryOperator*>(s);
      print_expr_header("UnaryOperator");
      os_ << " '" << GetUnaryOperatorSpelling(uo->GetOpcode()) << "'"
          << (uo->GetOpcode() == UnaryOperatorKind::kPostInc ||
                      uo->GetOpcode() == UnaryOperatorKind::kPostDec
                  ? " postfix"
                  : "")
          << "\n";
      DumpStmt(uo->GetSubExpr(), depth + 1);
      return;
    }
    case StmtClass::kBinaryOperator:
    case StmtClass::kCompoundAssignOperator: {
      const auto* bo = static_cast<const BinaryOperator*>(s);
      print_expr_header(s->GetStmtClass() == StmtClass::kCompoundAssignOperator
                            ? "CompoundAssignOperator"
                            : "BinaryOperator");
      os_ << " '" << GetBinaryOperatorSpelling(bo->GetOpcode()) << "'\n";
      DumpStmt(bo->GetLHS(), depth + 1);
      DumpStmt(bo->GetRHS(), depth + 1);
      return;
    }
    case StmtClass::kConditionalOperator: {
      const auto* co = static_cast<const ConditionalOperator*>(s);
      print_expr_header("ConditionalOperator");
      os_ << "\n";
      DumpStmt(co->GetCond(), depth + 1);
      DumpStmt(co->GetTrueExpr(), depth + 1);
      DumpStmt(co->GetFalseExpr(), depth + 1);
      return;
    }
    case StmtClass::kArraySubscriptExpr: {
      const auto* as = static_cast<const ArraySubscriptExpr*>(s);
      print_expr_header("ArraySubscriptExpr");
      os_ << "\n";
      DumpStmt(as->GetBase(), depth + 1);
      DumpStmt(as->GetIdx(), depth + 1);
      return;
    }
    case StmtClass::kCallExpr: {
      const auto* ce = static_cast<const CallExpr*>(s);
      print_expr_header("CallExpr");
      os_ << "\n";
      DumpStmt(ce->GetCallee(), depth + 1);
      for (const Expr* arg : ce->GetArgs()) DumpStmt(arg, depth + 1);
      return;
    }
    case StmtClass::kMemberExpr: {
      const auto* me = static_cast<const MemberExpr*>(s);
      print_expr_header("MemberExpr");
      os_ << " " << (me->IsArrow() ? "->" : ".") << me->GetMember()->GetName()
          << "\n";
      DumpStmt(me->GetBase(), depth + 1);
      return;
    }
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* ce = static_cast<const CastExpr*>(s);
      print_expr_header(s->GetStmtClass() == StmtClass::kImplicitCastExpr
                            ? "ImplicitCastExpr"
                            : "CStyleCastExpr");
      os_ << " <" << GetCastKindName(ce->GetCastKind()) << ">\n";
      DumpStmt(ce->GetSubExpr(), depth + 1);
      return;
    }
    case StmtClass::kSizeOfAlignOfExpr: {
      const auto* se = static_cast<const SizeOfAlignOfExpr*>(s);
      print_expr_header(se->IsSizeOf() ? "SizeOfExpr" : "AlignOfExpr");
      os_ << " " << se->GetValue();
      if (se->IsArgumentType()) {
        os_ << " '" << se->GetArgumentType().GetAsString() << "'";
      }
      os_ << "\n";
      if (!se->IsArgumentType()) DumpStmt(se->GetArgumentExpr(), depth + 1);
      return;
    }
    case StmtClass::kCompoundLiteralExpr: {
      print_expr_header("CompoundLiteralExpr");
      os_ << "\n";
      DumpStmt(static_cast<const CompoundLiteralExpr*>(s)->GetInitializer(),
               depth + 1);
      return;
    }
    case StmtClass::kInitListExpr: {
      print_expr_header("InitListExpr");
      os_ << "\n";
      for (const Expr* init : static_cast<const InitListExpr*>(s)->GetInits()) {
        DumpStmt(init, depth + 1);
      }
      return;
    }
    case StmtClass::kDesignatedInitExpr: {
      const auto* de = static_cast<const DesignatedInitExpr*>(s);
      os_ << "DesignatedInitExpr\n";
      DumpStmt(de->GetInit(), depth + 1);
      return;
    }
    case StmtClass::kGenericSelectionExpr: {
      const auto* ge = static_cast<const GenericSelectionExpr*>(s);
      print_expr_header("GenericSelectionExpr");
      os_ << "\n";
      DumpStmt(ge->GetControllingExpr(), depth + 1);
      DumpStmt(ge->GetChosenExpr(), depth + 1);
      return;
    }
  }
}

}  // namespace bcc
