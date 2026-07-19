#include <cassert>
#include <string>

#include "bcc/codegen/codegen_function.hh"

namespace bcc::codegen {

namespace {

bool IsAggregateEvalKind(QualType t) {
  const Type* canon = t.GetCanonical().GetTypePtr();
  return canon->IsRecordType() || canon->IsArrayType();
}

/// Signedness of an integer-ish type for cast/compare selection.
bool IsSigned(const ASTContext& ast, QualType t) {
  return ast.IsSignedIntegerType(t);
}

}  // namespace

//===----------------------------------------------------------------------===//
// LValues
//===----------------------------------------------------------------------===//

LValue CodeGenFunction::EmitLValue(const Expr* e) {
  switch (e->GetStmtClass()) {
    case StmtClass::kDeclRefExpr:
      return EmitDeclRefLValue(e->As<DeclRefExpr>());
    case StmtClass::kParenExpr:
      return EmitLValue(e->As<ParenExpr>()->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return EmitLValue(e->As<GenericSelectionExpr>()->GetChosenExpr());
    case StmtClass::kUnaryOperator: {
      const auto* uo = e->As<UnaryOperator>();
      if (uo->GetOpcode() == UnaryOperatorKind::kDeref) {
        const ir::Value* ptr = EmitScalarExpr(uo->GetSubExpr());
        QualType pointee = e->GetType();
        uint64_t align =
            pointee->IsCompleteType() ? Ast().GetTypeAlign(pointee) : 1;
        return LValue::MakeAddr(Address(ptr, align), pointee);
      }
      break;
    }
    case StmtClass::kArraySubscriptExpr: {
      const auto* ase = e->As<ArraySubscriptExpr>();
      const ir::Value* base = EmitScalarExpr(ase->GetBase());
      const ir::Value* idx =
          EmitIndexAsI64(EmitScalarExpr(ase->GetIdx()),
                         ase->GetIdx()->GetType());
      QualType elem = e->GetType();
      const ir::Value* addr = builder_.CreateGEP(
          ConvertType(elem), base, {idx}, "arrayidx");
      uint64_t align =
          elem->IsCompleteType() ? Ast().GetTypeAlign(elem) : 1;
      return LValue::MakeAddr(Address(addr, align), elem);
    }
    case StmtClass::kMemberExpr:
      return EmitMemberExprLValue(e->As<MemberExpr>());
    case StmtClass::kStringLiteral: {
      const auto* sl = e->As<StringLiteral>();
      ir::GlobalVariable* gv = cgm_.GetStringLiteral(sl);
      return LValue::MakeAddr(Address(gv, sl->GetCharByteWidth()),
                              e->GetType());
    }
    case StmtClass::kCompoundLiteralExpr: {
      const auto* cle = e->As<CompoundLiteralExpr>();
      QualType t = e->GetType();
      Address slot = CreateTempAlloca(t, ".compoundliteral");
      if (IsAggregateEvalKind(t) || t->IsUnionType()) {
        EmitAggExpr(cle->GetInitializer(), slot);
      } else {
        const ir::Value* v = EmitScalarExpr(cle->GetInitializer());
        if (v) builder_.CreateStore(v, slot.ptr, slot.align);
      }
      return LValue::MakeAddr(slot, t);
    }
    default:
      break;
  }

  cgm_.ErrorUnsupported(e->GetBeginLoc(), "lvalue expression");
  Address dummy(builder_.CreateAlloca(Ir().GetInt8Type(), 1, "lvalue.err"),
                1);
  return LValue::MakeAddr(dummy, e->GetType());
}

LValue CodeGenFunction::EmitDeclRefLValue(const DeclRefExpr* e) {
  const ValueDecl* decl = e->GetDecl();

  if (const auto* fd = decl->As<FunctionDecl>()) {
    ir::Function* fn = cgm_.GetOrCreateFunction(fd);
    fn->SetUsed();
    return LValue::MakeAddr(Address(fn, 8), e->GetType());
  }

  const auto* vd = decl->As<VarDecl>();
  assert(vd && "DeclRef lvalue is neither function nor variable");

  auto it = local_addrs_.find(vd);
  if (it != local_addrs_.end()) {
    return LValue::MakeAddr(it->second, e->GetType());
  }
  if (ir::GlobalVariable* gv = cgm_.GetStaticLocal(vd)) {
    return LValue::MakeAddr(Address(gv, gv->GetAlign()), e->GetType());
  }
  if (vd->HasStaticStorage()) {
    ir::GlobalVariable* gv = cgm_.GetOrCreateGlobal(vd);
    return LValue::MakeAddr(Address(gv, gv->GetAlign()), e->GetType());
  }

  // Error recovery (e.g. a VLA local that was diagnosed and skipped).
  Address dummy(builder_.CreateAlloca(Ir().GetInt8Type(), 1, "declref.err"),
                1);
  return LValue::MakeAddr(dummy, e->GetType());
}

LValue CodeGenFunction::EmitMemberExprLValue(const MemberExpr* e) {
  Address base;
  QualType record_type;
  if (e->IsArrow()) {
    const Expr* base_e = e->GetBase();
    base.ptr = EmitScalarExpr(base_e);
    record_type = base_e->GetType()->GetPointeeType();
    base.align = record_type->IsCompleteType()
                     ? Ast().GetTypeAlign(record_type)
                     : 1;
  } else {
    LValue lv = EmitLValue(e->GetBase());
    base = lv.GetAddress();
    record_type = e->GetBase()->GetType();
  }

  const auto* rt = record_type->AsCanonical<RecordType>();
  assert(rt && "member access on non-record");
  const RecordDecl* rd = rt->GetDecl();
  const FieldDecl* field = e->GetMember();
  const CodeGenTypes::RecordInfo& info = cgm_.GetTypes().GetRecordInfo(rd);

  if (field->IsBitField()) {
    auto bit = info.bit_fields.find(field);
    assert(bit != info.bit_fields.end());
    const BitFieldInfo& bf = bit->second;
    const ir::Value* storage = base.ptr;
    if (bf.storage_offset != 0) {
      storage = builder_.CreateGEP(
          Ir().GetInt8Type(), base.ptr,
          {Ir().GetInt64(bf.storage_offset)}, "bf.addr");
    }
    return LValue::MakeBitField(Address(storage, bf.storage_size / 8),
                                e->GetType(), &bit->second);
  }

  if (rd->IsUnion()) {
    // Union members all live at the base address (opaque ptr: no cast).
    return LValue::MakeAddr(base, e->GetType());
  }

  auto idx = info.field_index.find(field);
  assert(idx != info.field_index.end());
  const ir::Value* addr = builder_.CreateGEP(
      info.type, base.ptr,
      {Ir().GetInt32(0), Ir().GetInt32(idx->second)},
      std::string(field->GetName()));
  QualType ft = e->GetType();
  uint64_t align = ft->IsCompleteType() ? Ast().GetTypeAlign(ft) : 1;
  return LValue::MakeAddr(Address(addr, align), ft);
}

//===----------------------------------------------------------------------===//
// Scalar loads/stores/conversions
//===----------------------------------------------------------------------===//

const ir::Value* CodeGenFunction::EmitLoadOfLValue(LValue lv) {
  if (lv.IsBitField()) {
    const BitFieldInfo& bf = lv.GetBitFieldInfo();
    const ir::IntegerType* storage_ty = Ir().GetIntType(bf.storage_size);
    const ir::Value* v =
        builder_.CreateLoad(storage_ty, lv.GetPointer(), lv.GetAlign(),
                            "bf.load");
    if (bf.is_signed) {
      unsigned up = bf.storage_size - bf.offset - bf.width;
      if (up != 0) {
        v = builder_.CreateBinOp(ir::Opcode::kShl, v,
                                 Ir().GetInt(storage_ty, up), "bf.shl");
      }
      v = builder_.CreateBinOp(ir::Opcode::kAShr, v,
                               Ir().GetInt(storage_ty, bf.storage_size -
                                                            bf.width),
                               "bf.ashr");
    } else {
      if (bf.offset != 0) {
        v = builder_.CreateBinOp(ir::Opcode::kLShr, v,
                                 Ir().GetInt(storage_ty, bf.offset),
                                 "bf.lshr");
      }
      if (bf.width < bf.storage_size) {
        uint64_t mask = (uint64_t{1} << bf.width) - 1;
        v = builder_.CreateBinOp(ir::Opcode::kAnd, v,
                                 Ir().GetInt(storage_ty, mask), "bf.clear");
      }
    }
    return v;
  }

  return builder_.CreateLoad(ConvertType(lv.GetType()), lv.GetPointer(),
                             lv.GetAlign());
}

const ir::Value* CodeGenFunction::EmitStoreOfScalar(const ir::Value* value,
                                                    LValue lv) {
  if (!lv.IsBitField()) {
    builder_.CreateStore(value, lv.GetPointer(), lv.GetAlign());
    return value;
  }

  const BitFieldInfo& bf = lv.GetBitFieldInfo();
  const ir::IntegerType* storage_ty = Ir().GetIntType(bf.storage_size);
  uint64_t mask = bf.width >= 64 ? ~uint64_t{0}
                                 : (uint64_t{1} << bf.width) - 1;

  const ir::Value* masked = builder_.CreateBinOp(
      ir::Opcode::kAnd, value, Ir().GetInt(storage_ty, mask), "bf.value");
  const ir::Value* shifted = masked;
  if (bf.offset != 0) {
    shifted = builder_.CreateBinOp(ir::Opcode::kShl, masked,
                                   Ir().GetInt(storage_ty, bf.offset),
                                   "bf.shifted");
  }
  const ir::Value* old = builder_.CreateLoad(storage_ty, lv.GetPointer(),
                                             lv.GetAlign(), "bf.load");
  const ir::Value* cleared = builder_.CreateBinOp(
      ir::Opcode::kAnd, old, Ir().GetInt(storage_ty, ~(mask << bf.offset)),
      "bf.cleared");
  const ir::Value* merged = builder_.CreateBinOp(ir::Opcode::kOr, cleared,
                                                 shifted, "bf.set");
  builder_.CreateStore(merged, lv.GetPointer(), lv.GetAlign());

  // The value of the assignment is the field's value after truncation.
  if (bf.is_signed) {
    unsigned amt = bf.storage_size - bf.width;
    const ir::Value* r = builder_.CreateBinOp(
        ir::Opcode::kShl, masked, Ir().GetInt(storage_ty, amt), "bf.sext.l");
    return builder_.CreateBinOp(ir::Opcode::kAShr, r,
                                Ir().GetInt(storage_ty, amt), "bf.sext");
  }
  return masked;
}

const ir::Value* CodeGenFunction::EmitScalarToBool(const ir::Value* v,
                                                   QualType type) {
  const Type* canon = type.GetCanonical().GetTypePtr();
  if (canon->IsPointerType()) {
    return builder_.CreateICmp(ir::CmpPredicate::kNE, v, Ir().GetNullPtr(),
                               "tobool");
  }
  if (canon->IsFloatingType()) {
    return builder_.CreateFCmp(ir::CmpPredicate::kUNE, v,
                               Ir().GetFP(v->GetType(), 0.0), "tobool");
  }
  const auto* it = v->GetType()->As<ir::IntegerType>();
  assert(it && "scalar-to-bool on non-scalar");
  if (it->GetBits() == 1) return v;
  return builder_.CreateICmp(ir::CmpPredicate::kNE, v, Ir().GetInt(it, 0),
                             "tobool");
}

const ir::Value* CodeGenFunction::EmitScalarConversion(const ir::Value* v,
                                                       QualType from,
                                                       QualType to) {
  const Type* from_c = from.GetCanonical().GetTypePtr();
  const Type* to_c = to.GetCanonical().GetTypePtr();
  if (from_c == to_c) return v;

  const ir::Type* dst = ConvertType(to);
  if (v->GetType() == dst) return v;

  if (to_c->IsBoolType()) {
    const ir::Value* b = EmitScalarToBool(v, from);
    return builder_.CreateCast(ir::Opcode::kZExt, b, Ir().GetInt8Type(),
                               "frombool");
  }

  if (from_c->IsPointerType() && to_c->IsPointerType()) return v;
  if (from_c->IsPointerType() && to_c->IsIntegerType()) {
    return builder_.CreateCast(ir::Opcode::kPtrToInt, v, dst, "conv");
  }
  if (from_c->IsIntegerType() && to_c->IsPointerType()) {
    return builder_.CreateCast(ir::Opcode::kIntToPtr, v, dst, "conv");
  }

  bool from_fp = from_c->IsFloatingType();
  bool to_fp = to_c->IsFloatingType();
  if (from_fp && to_fp) {
    uint64_t fs = Ast().GetTypeSize(from), ts = Ast().GetTypeSize(to);
    // long double is lowered as double; sizes compare at the IR level.
    if (v->GetType() == dst) return v;
    return builder_.CreateCast(
        ts < fs ? ir::Opcode::kFPTrunc : ir::Opcode::kFPExt, v, dst, "conv");
  }
  if (from_fp) {
    return builder_.CreateCast(IsSigned(Ast(), to) ? ir::Opcode::kFPToSI
                                                   : ir::Opcode::kFPToUI,
                               v, dst, "conv");
  }
  if (to_fp) {
    return builder_.CreateCast(IsSigned(Ast(), from) ? ir::Opcode::kSIToFP
                                                     : ir::Opcode::kUIToFP,
                               v, dst, "conv");
  }

  // Integer to integer.
  unsigned fw = v->GetType()->As<ir::IntegerType>()->GetBits();
  unsigned tw = dst->As<ir::IntegerType>()->GetBits();
  if (tw < fw) return builder_.CreateCast(ir::Opcode::kTrunc, v, dst, "conv");
  return builder_.CreateCast(IsSigned(Ast(), from) ? ir::Opcode::kSExt
                                                   : ir::Opcode::kZExt,
                             v, dst, "conv");
}

//===----------------------------------------------------------------------===//
// Scalar expressions
//===----------------------------------------------------------------------===//

const ir::Value* CodeGenFunction::EmitScalarExpr(const Expr* e) {
  switch (e->GetStmtClass()) {
    case StmtClass::kIntegerLiteral:
      return Ir().GetInt(ConvertType(e->GetType())->As<ir::IntegerType>(),
                         e->As<IntegerLiteral>()->GetValue());
    case StmtClass::kCharacterLiteral:
      return Ir().GetInt(ConvertType(e->GetType())->As<ir::IntegerType>(),
                         e->As<CharacterLiteral>()->GetValue());
    case StmtClass::kFloatingLiteral: {
      double v = e->As<FloatingLiteral>()->GetValue();
      const ir::Type* t = ConvertType(e->GetType());
      if (t->IsFloat()) v = static_cast<float>(v);
      return Ir().GetFP(t, v);
    }
    case StmtClass::kSizeOfAlignOfExpr:
      return Ir().GetInt(ConvertType(e->GetType())->As<ir::IntegerType>(),
                         e->As<SizeOfAlignOfExpr>()->GetValue());
    case StmtClass::kDeclRefExpr: {
      const auto* dre = e->As<DeclRefExpr>();
      if (const auto* ec = dre->GetDecl()->As<EnumConstantDecl>()) {
        return Ir().GetInt(ConvertType(e->GetType())->As<ir::IntegerType>(),
                           static_cast<uint64_t>(ec->GetValue()));
      }
      return EmitLoadOfLValue(EmitLValue(e));
    }
    case StmtClass::kParenExpr:
      return EmitScalarExpr(e->As<ParenExpr>()->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return EmitScalarExpr(e->As<GenericSelectionExpr>()->GetChosenExpr());
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr:
      return EmitCastExpr(e->As<CastExpr>());
    case StmtClass::kUnaryOperator:
      return EmitUnaryOperator(e->As<UnaryOperator>());
    case StmtClass::kBinaryOperator:
    case StmtClass::kCompoundAssignOperator:
      return EmitBinaryOperator(e->As<BinaryOperator>());
    case StmtClass::kConditionalOperator:
      return EmitScalarConditional(e->As<ConditionalOperator>());
    case StmtClass::kCallExpr: {
      const ir::Value* v = EmitCallRaw(e->As<CallExpr>());
      return e->GetType()->IsVoidType() ? nullptr : v;
    }
    case StmtClass::kInitListExpr: {
      // Scalar `int x = {3};` semantic form.
      const auto* ile = e->As<InitListExpr>();
      if (!ile->GetInits().empty() && ile->GetInits()[0]) {
        return EmitScalarExpr(ile->GetInits()[0]);
      }
      return Ir().GetInt(ConvertType(e->GetType())->As<ir::IntegerType>(), 0);
    }
    case StmtClass::kArraySubscriptExpr:
    case StmtClass::kMemberExpr:
    case StmtClass::kCompoundLiteralExpr:
    case StmtClass::kStringLiteral:
      // Fallback for unwrapped lvalues.
      return EmitLoadOfLValue(EmitLValue(e));
    default:
      cgm_.ErrorUnsupported(e->GetBeginLoc(), "expression");
      return Ir().GetUndef(ConvertType(e->GetType()));
  }
}

const ir::Value* CodeGenFunction::EmitCastExpr(const CastExpr* e) {
  const Expr* sub = e->GetSubExpr();
  switch (e->GetCastKind()) {
    case CastKind::kLValueToRValue:
      return EmitLoadOfLValue(EmitLValue(sub));
    case CastKind::kArrayToPointerDecay:
    case CastKind::kFunctionToPointerDecay:
      return EmitLValue(sub).GetPointer();
    case CastKind::kIntegralCast: {
      const ir::Value* v = EmitScalarExpr(sub);
      const ir::Type* dst = ConvertType(e->GetType());
      if (v->GetType() == dst) return v;
      unsigned fw = v->GetType()->As<ir::IntegerType>()->GetBits();
      unsigned tw = dst->As<ir::IntegerType>()->GetBits();
      if (tw < fw) {
        return builder_.CreateCast(ir::Opcode::kTrunc, v, dst, "conv");
      }
      return builder_.CreateCast(IsSigned(Ast(), sub->GetType())
                                     ? ir::Opcode::kSExt
                                     : ir::Opcode::kZExt,
                                 v, dst, "conv");
    }
    case CastKind::kIntegralToFloating:
      return builder_.CreateCast(IsSigned(Ast(), sub->GetType())
                                     ? ir::Opcode::kSIToFP
                                     : ir::Opcode::kUIToFP,
                                 EmitScalarExpr(sub),
                                 ConvertType(e->GetType()), "conv");
    case CastKind::kFloatingToIntegral:
      return builder_.CreateCast(IsSigned(Ast(), e->GetType())
                                     ? ir::Opcode::kFPToSI
                                     : ir::Opcode::kFPToUI,
                                 EmitScalarExpr(sub),
                                 ConvertType(e->GetType()), "conv");
    case CastKind::kFloatingCast: {
      const ir::Value* v = EmitScalarExpr(sub);
      const ir::Type* dst = ConvertType(e->GetType());
      if (v->GetType() == dst) return v;  // long double == double here
      bool narrowing = v->GetType()->IsDouble() && dst->IsFloat();
      return builder_.CreateCast(
          narrowing ? ir::Opcode::kFPTrunc : ir::Opcode::kFPExt, v, dst,
          "conv");
    }
    case CastKind::kIntegralToBoolean:
    case CastKind::kFloatingToBoolean:
    case CastKind::kPointerToBoolean: {
      const ir::Value* b =
          EmitScalarToBool(EmitScalarExpr(sub), sub->GetType());
      return builder_.CreateCast(ir::Opcode::kZExt, b, Ir().GetInt8Type(),
                                 "frombool");
    }
    case CastKind::kNullToPointer:
      return Ir().GetNullPtr();
    case CastKind::kIntegralToPointer:
      return builder_.CreateCast(ir::Opcode::kIntToPtr, EmitScalarExpr(sub),
                                 Ir().GetPointerType(), "conv");
    case CastKind::kPointerToIntegral:
      return builder_.CreateCast(ir::Opcode::kPtrToInt, EmitScalarExpr(sub),
                                 ConvertType(e->GetType()), "conv");
    case CastKind::kBitCast:
    case CastKind::kNoOp:
      return EmitScalarExpr(sub);
    case CastKind::kToVoid:
      EmitIgnoredExpr(sub);
      return nullptr;
  }
  return nullptr;
}

const ir::Value* CodeGenFunction::EmitUnaryOperator(const UnaryOperator* e) {
  const Expr* sub = e->GetSubExpr();
  switch (e->GetOpcode()) {
    case UnaryOperatorKind::kPostInc:
    case UnaryOperatorKind::kPostDec:
    case UnaryOperatorKind::kPreInc:
    case UnaryOperatorKind::kPreDec:
      return EmitIncDec(e);
    case UnaryOperatorKind::kAddrOf:
      return EmitLValue(sub).GetPointer();
    case UnaryOperatorKind::kDeref:
      return EmitLoadOfLValue(EmitLValue(e));
    case UnaryOperatorKind::kPlus:
      return EmitScalarExpr(sub);
    case UnaryOperatorKind::kMinus: {
      const ir::Value* v = EmitScalarExpr(sub);
      if (v->GetType()->IsFloatingPoint()) {
        return builder_.CreateBinOp(ir::Opcode::kFSub,
                                    Ir().GetFP(v->GetType(), -0.0), v, "fneg");
      }
      return builder_.CreateBinOp(
          ir::Opcode::kSub,
          Ir().GetInt(v->GetType()->As<ir::IntegerType>(), 0), v, "neg");
    }
    case UnaryOperatorKind::kNot: {
      const ir::Value* v = EmitScalarExpr(sub);
      return builder_.CreateBinOp(
          ir::Opcode::kXor, v,
          Ir().GetInt(v->GetType()->As<ir::IntegerType>(), ~uint64_t{0}),
          "not");
    }
    case UnaryOperatorKind::kLNot: {
      const ir::Value* b =
          EmitScalarToBool(EmitScalarExpr(sub), sub->GetType());
      const ir::Value* inverted = builder_.CreateBinOp(
          ir::Opcode::kXor, b, Ir().GetInt1(true), "lnot");
      return builder_.CreateCast(ir::Opcode::kZExt, inverted,
                                 ConvertType(e->GetType()), "lnot.ext");
    }
  }
  return nullptr;
}

const ir::Value* CodeGenFunction::EmitIncDec(const UnaryOperator* e) {
  const Expr* sub = e->GetSubExpr();
  bool is_inc = e->GetOpcode() == UnaryOperatorKind::kPreInc ||
                e->GetOpcode() == UnaryOperatorKind::kPostInc;
  bool is_pre = e->GetOpcode() == UnaryOperatorKind::kPreInc ||
                e->GetOpcode() == UnaryOperatorKind::kPreDec;

  LValue lv = EmitLValue(sub);
  QualType t = sub->GetType();
  const ir::Value* old = EmitLoadOfLValue(lv);
  const ir::Value* next = nullptr;

  const Type* canon = t.GetCanonical().GetTypePtr();
  if (canon->IsBoolType()) {
    // ++b is b = 1; --b toggles through the != 0 conversion.
    if (is_inc) {
      next = Ir().GetInt8(1);
    } else {
      const ir::Value* is_zero = builder_.CreateICmp(
          ir::CmpPredicate::kEQ, old, Ir().GetInt8(0), "bool.dec");
      next = builder_.CreateCast(ir::Opcode::kZExt, is_zero,
                                 Ir().GetInt8Type(), "frombool");
    }
  } else if (canon->IsPointerType()) {
    next = builder_.CreateGEP(ConvertType(canon->GetPointeeType()), old,
                              {Ir().GetInt64(is_inc ? 1 : ~uint64_t{0})},
                              "incdec.ptr");
  } else if (canon->IsFloatingType()) {
    next = builder_.CreateBinOp(is_inc ? ir::Opcode::kFAdd
                                       : ir::Opcode::kFSub,
                                old, Ir().GetFP(old->GetType(), 1.0), "inc");
  } else {
    next = builder_.CreateBinOp(
        is_inc ? ir::Opcode::kAdd : ir::Opcode::kSub, old,
        Ir().GetInt(old->GetType()->As<ir::IntegerType>(), 1),
        is_inc ? "inc" : "dec");
  }

  const ir::Value* stored = EmitStoreOfScalar(next, lv);
  return is_pre ? stored : old;
}

const ir::Value* CodeGenFunction::EmitIndexAsI64(const ir::Value* idx,
                                                 QualType idx_type) {
  const auto* it = idx->GetType()->As<ir::IntegerType>();
  if (it->GetBits() == 64) return idx;
  if (const auto* ci = idx->As<ir::ConstantInt>()) {
    // Fold constant indexes instead of emitting `sext i32 K to i64`.
    uint64_t v = IsSigned(Ast(), idx_type)
                     ? static_cast<uint64_t>(ci->GetSExtValue())
                     : ci->GetValue();
    return Ir().GetInt64(v);
  }
  return builder_.CreateCast(IsSigned(Ast(), idx_type) ? ir::Opcode::kSExt
                                                       : ir::Opcode::kZExt,
                             idx, Ir().GetInt64Type(), "idxprom");
}

const ir::Value* CodeGenFunction::EmitPointerAdd(const ir::Value* ptr,
                                                 QualType ptr_type,
                                                 const ir::Value* idx,
                                                 QualType idx_type,
                                                 bool negate) {
  const ir::Value* i = EmitIndexAsI64(idx, idx_type);
  if (negate) {
    i = builder_.CreateBinOp(ir::Opcode::kSub, Ir().GetInt64(0), i,
                             "idx.neg");
  }
  QualType pointee = ptr_type.GetCanonical()->GetPointeeType();
  return builder_.CreateGEP(ConvertType(pointee), ptr, {i}, "add.ptr");
}

const ir::Value* CodeGenFunction::EmitScalarArith(BinaryOperatorKind op,
                                                  const ir::Value* lhs,
                                                  const ir::Value* rhs,
                                                  QualType type,
                                                  QualType rhs_ast_type) {
  bool fp = lhs->GetType()->IsFloatingPoint();
  bool is_signed = !fp && IsSigned(Ast(), type);

  ir::Opcode opc;
  switch (op) {
    case BinaryOperatorKind::kAdd:
      opc = fp ? ir::Opcode::kFAdd : ir::Opcode::kAdd;
      break;
    case BinaryOperatorKind::kSub:
      opc = fp ? ir::Opcode::kFSub : ir::Opcode::kSub;
      break;
    case BinaryOperatorKind::kMul:
      opc = fp ? ir::Opcode::kFMul : ir::Opcode::kMul;
      break;
    case BinaryOperatorKind::kDiv:
      opc = fp ? ir::Opcode::kFDiv
               : is_signed ? ir::Opcode::kSDiv : ir::Opcode::kUDiv;
      break;
    case BinaryOperatorKind::kRem:
      opc = is_signed ? ir::Opcode::kSRem : ir::Opcode::kURem;
      break;
    case BinaryOperatorKind::kAnd:
      opc = ir::Opcode::kAnd;
      break;
    case BinaryOperatorKind::kXor:
      opc = ir::Opcode::kXor;
      break;
    case BinaryOperatorKind::kOr:
      opc = ir::Opcode::kOr;
      break;
    case BinaryOperatorKind::kShl:
    case BinaryOperatorKind::kShr: {
      // Shift operands are promoted independently; LLVM wants same widths.
      unsigned lw = lhs->GetType()->As<ir::IntegerType>()->GetBits();
      unsigned rw = rhs->GetType()->As<ir::IntegerType>()->GetBits();
      if (lw != rw) {
        rhs = rw > lw
                  ? builder_.CreateCast(ir::Opcode::kTrunc, rhs,
                                        lhs->GetType(), "sh.prom")
                  : builder_.CreateCast(IsSigned(Ast(), rhs_ast_type)
                                            ? ir::Opcode::kSExt
                                            : ir::Opcode::kZExt,
                                        rhs, lhs->GetType(), "sh.prom");
      }
      opc = op == BinaryOperatorKind::kShl
                ? ir::Opcode::kShl
                : is_signed ? ir::Opcode::kAShr : ir::Opcode::kLShr;
      break;
    }
    default:
      assert(false && "not an arithmetic operator");
      return lhs;
  }
  return builder_.CreateBinOp(opc, lhs, rhs,
                              std::string(GetOpcodeName(opc)));
}

const ir::Value* CodeGenFunction::EmitCompareI1(const BinaryOperator* e) {
  const Expr* lhs_e = e->GetLHS();
  const Expr* rhs_e = e->GetRHS();
  const ir::Value* lhs = EmitScalarExpr(lhs_e);
  const ir::Value* rhs = EmitScalarExpr(rhs_e);
  const Type* canon = lhs_e->GetType().GetCanonical().GetTypePtr();

  if (canon->IsFloatingType()) {
    ir::CmpPredicate p;
    switch (e->GetOpcode()) {
      case BinaryOperatorKind::kLT: p = ir::CmpPredicate::kOLT; break;
      case BinaryOperatorKind::kGT: p = ir::CmpPredicate::kOGT; break;
      case BinaryOperatorKind::kLE: p = ir::CmpPredicate::kOLE; break;
      case BinaryOperatorKind::kGE: p = ir::CmpPredicate::kOGE; break;
      case BinaryOperatorKind::kEQ: p = ir::CmpPredicate::kOEQ; break;
      default: p = ir::CmpPredicate::kUNE; break;
    }
    return builder_.CreateFCmp(p, lhs, rhs, "cmp");
  }

  // Pointers compare unsigned; integers by their signedness.
  bool is_signed = canon->IsPointerType() ? false : IsSigned(Ast(), lhs_e->GetType());
  ir::CmpPredicate p;
  switch (e->GetOpcode()) {
    case BinaryOperatorKind::kLT:
      p = is_signed ? ir::CmpPredicate::kSLT : ir::CmpPredicate::kULT;
      break;
    case BinaryOperatorKind::kGT:
      p = is_signed ? ir::CmpPredicate::kSGT : ir::CmpPredicate::kUGT;
      break;
    case BinaryOperatorKind::kLE:
      p = is_signed ? ir::CmpPredicate::kSLE : ir::CmpPredicate::kULE;
      break;
    case BinaryOperatorKind::kGE:
      p = is_signed ? ir::CmpPredicate::kSGE : ir::CmpPredicate::kUGE;
      break;
    case BinaryOperatorKind::kEQ:
      p = ir::CmpPredicate::kEQ;
      break;
    default:
      p = ir::CmpPredicate::kNE;
      break;
  }
  return builder_.CreateICmp(p, lhs, rhs, "cmp");
}

const ir::Value* CodeGenFunction::EmitLogicalOp(const BinaryOperator* e) {
  bool is_and = e->GetOpcode() == BinaryOperatorKind::kLAnd;
  const char* pfx = is_and ? "land" : "lor";
  ir::BasicBlock* rhs_bb = CreateBlock(std::string(pfx) + ".rhs");
  ir::BasicBlock* end_bb = CreateBlock(std::string(pfx) + ".end");

  const ir::Value* lhs_bool =
      EmitScalarToBool(EmitScalarExpr(e->GetLHS()), e->GetLHS()->GetType());
  ir::BasicBlock* lhs_end = builder_.GetInsertBlock();
  if (is_and) {
    builder_.CreateCondBr(lhs_bool, rhs_bb, end_bb);
  } else {
    builder_.CreateCondBr(lhs_bool, end_bb, rhs_bb);
  }

  EmitBlock(rhs_bb);
  const ir::Value* rhs_bool =
      EmitScalarToBool(EmitScalarExpr(e->GetRHS()), e->GetRHS()->GetType());
  ir::BasicBlock* rhs_end = builder_.GetInsertBlock();

  EmitBlock(end_bb);
  ir::PhiNode* phi = builder_.CreatePhi(Ir().GetInt1Type(),
                                        std::string(pfx) + ".val");
  phi->AddIncoming(Ir().GetInt1(!is_and), lhs_end);
  phi->AddIncoming(rhs_bool, rhs_end);
  return builder_.CreateCast(ir::Opcode::kZExt, phi,
                             ConvertType(e->GetType()), "conv");
}

const ir::Value* CodeGenFunction::EmitBinaryOperator(
    const BinaryOperator* e) {
  BinaryOperatorKind op = e->GetOpcode();
  const Expr* lhs_e = e->GetLHS();
  const Expr* rhs_e = e->GetRHS();

  if (op == BinaryOperatorKind::kAssign) {
    const ir::Value* v = EmitScalarExpr(rhs_e);
    LValue lv = EmitLValue(lhs_e);
    return EmitStoreOfScalar(v, lv);
  }

  if (e->IsAssignmentOp()) {  // compound assignment
    const auto* cao = e->As<CompoundAssignOperator>();
    QualType comp_t = cao->GetComputationType();
    BinaryOperatorKind base;
    switch (op) {
      case BinaryOperatorKind::kMulAssign: base = BinaryOperatorKind::kMul; break;
      case BinaryOperatorKind::kDivAssign: base = BinaryOperatorKind::kDiv; break;
      case BinaryOperatorKind::kRemAssign: base = BinaryOperatorKind::kRem; break;
      case BinaryOperatorKind::kAddAssign: base = BinaryOperatorKind::kAdd; break;
      case BinaryOperatorKind::kSubAssign: base = BinaryOperatorKind::kSub; break;
      case BinaryOperatorKind::kShlAssign: base = BinaryOperatorKind::kShl; break;
      case BinaryOperatorKind::kShrAssign: base = BinaryOperatorKind::kShr; break;
      case BinaryOperatorKind::kAndAssign: base = BinaryOperatorKind::kAnd; break;
      case BinaryOperatorKind::kXorAssign: base = BinaryOperatorKind::kXor; break;
      default: base = BinaryOperatorKind::kOr; break;
    }

    LValue lv = EmitLValue(lhs_e);
    const ir::Value* old = EmitLoadOfLValue(lv);
    const ir::Value* result;
    if (comp_t->IsPointerType()) {
      // p += n / p -= n.
      const ir::Value* rhs = EmitScalarExpr(rhs_e);
      result = EmitPointerAdd(old, comp_t, rhs, rhs_e->GetType(),
                              base == BinaryOperatorKind::kSub);
    } else {
      const ir::Value* promoted =
          EmitScalarConversion(old, lhs_e->GetType(), comp_t);
      const ir::Value* rhs = EmitScalarExpr(rhs_e);
      result = EmitScalarArith(base, promoted, rhs, comp_t,
                               rhs_e->GetType());
      result = EmitScalarConversion(result, comp_t, lhs_e->GetType());
    }
    return EmitStoreOfScalar(result, lv);
  }

  switch (op) {
    case BinaryOperatorKind::kComma:
      EmitIgnoredExpr(lhs_e);
      return EmitScalarExpr(rhs_e);
    case BinaryOperatorKind::kLAnd:
    case BinaryOperatorKind::kLOr:
      return EmitLogicalOp(e);
    case BinaryOperatorKind::kLT:
    case BinaryOperatorKind::kGT:
    case BinaryOperatorKind::kLE:
    case BinaryOperatorKind::kGE:
    case BinaryOperatorKind::kEQ:
    case BinaryOperatorKind::kNE: {
      const ir::Value* i1 = EmitCompareI1(e);
      return builder_.CreateCast(ir::Opcode::kZExt, i1,
                                 ConvertType(e->GetType()), "conv");
    }
    default:
      break;
  }

  // Pointer arithmetic.
  QualType lt = lhs_e->GetType(), rt = rhs_e->GetType();
  bool l_ptr = lt->IsPointerType(), r_ptr = rt->IsPointerType();
  if (op == BinaryOperatorKind::kSub && l_ptr && r_ptr) {
    const ir::Value* l = EmitScalarExpr(lhs_e);
    const ir::Value* r = EmitScalarExpr(rhs_e);
    const ir::Value* li = builder_.CreateCast(
        ir::Opcode::kPtrToInt, l, Ir().GetInt64Type(), "sub.ptr.lhs");
    const ir::Value* ri = builder_.CreateCast(
        ir::Opcode::kPtrToInt, r, Ir().GetInt64Type(), "sub.ptr.rhs");
    const ir::Value* diff =
        builder_.CreateBinOp(ir::Opcode::kSub, li, ri, "sub.ptr.sub");
    QualType pointee = lt.GetCanonical()->GetPointeeType();
    uint64_t size = Ast().GetTypeSize(pointee);
    if (size > 1) {
      diff = builder_.CreateBinOp(ir::Opcode::kSDiv, diff,
                                  Ir().GetInt64(size), "sub.ptr.div");
    }
    return diff;
  }
  if ((op == BinaryOperatorKind::kAdd || op == BinaryOperatorKind::kSub) &&
      (l_ptr || r_ptr)) {
    const Expr* ptr_e = l_ptr ? lhs_e : rhs_e;
    const Expr* idx_e = l_ptr ? rhs_e : lhs_e;
    const ir::Value* ptr;
    const ir::Value* idx;
    if (l_ptr) {
      ptr = EmitScalarExpr(ptr_e);
      idx = EmitScalarExpr(idx_e);
    } else {
      idx = EmitScalarExpr(idx_e);
      ptr = EmitScalarExpr(ptr_e);
    }
    return EmitPointerAdd(ptr, ptr_e->GetType(), idx, idx_e->GetType(),
                          op == BinaryOperatorKind::kSub);
  }

  const ir::Value* l = EmitScalarExpr(lhs_e);
  const ir::Value* r = EmitScalarExpr(rhs_e);
  return EmitScalarArith(op, l, r, e->GetType(), rt);
}

const ir::Value* CodeGenFunction::EmitScalarConditional(
    const ConditionalOperator* e) {
  ir::BasicBlock* true_bb = CreateBlock("cond.true");
  ir::BasicBlock* false_bb = CreateBlock("cond.false");
  ir::BasicBlock* end_bb = CreateBlock("cond.end");

  EmitBranchOnBool(e->GetCond(), true_bb, false_bb);

  EmitBlock(true_bb);
  const ir::Value* v1 = EmitScalarExpr(e->GetTrueExpr());
  ir::BasicBlock* true_end = builder_.GetInsertBlock();
  builder_.CreateBr(end_bb);

  EmitBlock(false_bb);
  const ir::Value* v2 = EmitScalarExpr(e->GetFalseExpr());
  ir::BasicBlock* false_end = builder_.GetInsertBlock();

  EmitBlock(end_bb);
  if (e->GetType()->IsVoidType() || !v1 || !v2) return nullptr;
  ir::PhiNode* phi = builder_.CreatePhi(ConvertType(e->GetType()), "cond");
  phi->AddIncoming(v1, true_end);
  phi->AddIncoming(v2, false_end);
  return phi;
}

void CodeGenFunction::EmitBranchOnBool(const Expr* cond,
                                       ir::BasicBlock* true_bb,
                                       ir::BasicBlock* false_bb) {
  cond = cond->IgnoreParens();

  if (const auto* uo = cond->As<UnaryOperator>()) {
    if (uo->GetOpcode() == UnaryOperatorKind::kLNot) {
      EmitBranchOnBool(uo->GetSubExpr(), false_bb, true_bb);
      return;
    }
  }

  if (const auto* bo = cond->As<BinaryOperator>()) {
    if (bo->GetOpcode() == BinaryOperatorKind::kLAnd) {
      ir::BasicBlock* mid = CreateBlock("land.lhs.true");
      EmitBranchOnBool(bo->GetLHS(), mid, false_bb);
      EmitBlock(mid);
      EmitBranchOnBool(bo->GetRHS(), true_bb, false_bb);
      return;
    }
    if (bo->GetOpcode() == BinaryOperatorKind::kLOr) {
      ir::BasicBlock* mid = CreateBlock("lor.lhs.false");
      EmitBranchOnBool(bo->GetLHS(), true_bb, mid);
      EmitBlock(mid);
      EmitBranchOnBool(bo->GetRHS(), true_bb, false_bb);
      return;
    }
    if (bo->IsComparisonOp()) {
      builder_.CreateCondBr(EmitCompareI1(bo), true_bb, false_bb);
      return;
    }
  }

  // A Sema-inserted *ToBoolean wrapper adds nothing when branching.
  if (const auto* ce = cond->As<CastExpr>()) {
    switch (ce->GetCastKind()) {
      case CastKind::kIntegralToBoolean:
      case CastKind::kFloatingToBoolean:
      case CastKind::kPointerToBoolean:
        EmitBranchOnBool(ce->GetSubExpr(), true_bb, false_bb);
        return;
      default:
        break;
    }
  }

  const ir::Value* b =
      EmitScalarToBool(EmitScalarExpr(cond), cond->GetType());
  builder_.CreateCondBr(b, true_bb, false_bb);
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

const ir::Value* CodeGenFunction::EmitCallRaw(const CallExpr* e) {
  const Expr* callee_e = e->GetCallee();
  const ir::Value* callee = EmitScalarExpr(callee_e);

  QualType callee_t = callee_e->GetType();
  QualType fn_t = callee_t->IsPointerType()
                      ? callee_t.GetCanonical()->GetPointeeType()
                      : callee_t;
  const ir::FunctionType* ir_fn_t =
      cgm_.GetTypes().ConvertFunctionType(fn_t);

  std::vector<const ir::Value*> args;
  args.reserve(e->GetNumArgs());
  for (const Expr* arg : e->GetArgs()) {
    QualType at = arg->GetType();
    if (IsAggregateEvalKind(at) || at->IsUnionType()) {
      RValue rv = EmitAnyExpr(arg);
      Address addr = rv.GetAggregateAddress();
      args.push_back(builder_.CreateLoad(ConvertType(at), addr.ptr,
                                         addr.align));
    } else {
      args.push_back(EmitScalarExpr(arg));
    }
  }

  return builder_.CreateCall(ir_fn_t, callee, std::move(args),
                             ir_fn_t->GetReturnType()->IsVoid() ? "" : "call");
}

//===----------------------------------------------------------------------===//
// Aggregates
//===----------------------------------------------------------------------===//

void CodeGenFunction::EmitAggregateCopy(Address dest, Address src,
                                        QualType type) {
  const ir::Value* v = builder_.CreateLoad(ConvertType(type), src.ptr,
                                           src.align);
  builder_.CreateStore(v, dest.ptr, dest.align);
}

void CodeGenFunction::EmitAggExpr(const Expr* e, Address dest) {
  switch (e->GetStmtClass()) {
    case StmtClass::kParenExpr:
      EmitAggExpr(e->As<ParenExpr>()->GetSubExpr(), dest);
      return;
    case StmtClass::kGenericSelectionExpr:
      EmitAggExpr(e->As<GenericSelectionExpr>()->GetChosenExpr(), dest);
      return;
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* ce = e->As<CastExpr>();
      switch (ce->GetCastKind()) {
        case CastKind::kLValueToRValue: {
          LValue src = EmitLValue(ce->GetSubExpr());
          EmitAggregateCopy(dest, src.GetAddress(), e->GetType());
          return;
        }
        case CastKind::kNoOp:
        case CastKind::kBitCast:
          EmitAggExpr(ce->GetSubExpr(), dest);
          return;
        default:
          break;
      }
      break;
    }
    case StmtClass::kInitListExpr:
      EmitAggInitList(e->As<InitListExpr>(), dest);
      return;
    case StmtClass::kStringLiteral: {
      // char s[N] = "..." — store the padded constant.
      const ir::Constant* c = cgm_.EmitConstantInit(e, e->GetType());
      if (c) builder_.CreateStore(c, dest.ptr, dest.align);
      return;
    }
    case StmtClass::kCallExpr: {
      const ir::Value* call = EmitCallRaw(e->As<CallExpr>());
      builder_.CreateStore(call, dest.ptr, dest.align);
      return;
    }
    case StmtClass::kConditionalOperator: {
      const auto* co = e->As<ConditionalOperator>();
      ir::BasicBlock* true_bb = CreateBlock("cond.true");
      ir::BasicBlock* false_bb = CreateBlock("cond.false");
      ir::BasicBlock* end_bb = CreateBlock("cond.end");
      EmitBranchOnBool(co->GetCond(), true_bb, false_bb);
      EmitBlock(true_bb);
      EmitAggExpr(co->GetTrueExpr(), dest);
      builder_.CreateBr(end_bb);
      EmitBlock(false_bb);
      EmitAggExpr(co->GetFalseExpr(), dest);
      EmitBlock(end_bb);
      return;
    }
    case StmtClass::kBinaryOperator: {
      const auto* bo = e->As<BinaryOperator>();
      if (bo->GetOpcode() == BinaryOperatorKind::kAssign) {
        LValue lhs = EmitLValue(bo->GetLHS());
        EmitAggExpr(bo->GetRHS(), lhs.GetAddress());
        EmitAggregateCopy(dest, lhs.GetAddress(), e->GetType());
        return;
      }
      if (bo->GetOpcode() == BinaryOperatorKind::kComma) {
        EmitIgnoredExpr(bo->GetLHS());
        EmitAggExpr(bo->GetRHS(), dest);
        return;
      }
      break;
    }
    case StmtClass::kDeclRefExpr:
    case StmtClass::kMemberExpr:
    case StmtClass::kArraySubscriptExpr:
    case StmtClass::kCompoundLiteralExpr: {
      LValue src = EmitLValue(e);
      EmitAggregateCopy(dest, src.GetAddress(), e->GetType());
      return;
    }
    default:
      break;
  }
  cgm_.ErrorUnsupported(e->GetBeginLoc(), "aggregate expression");
}

void CodeGenFunction::EmitAggInitList(const InitListExpr* e, Address dest) {
  QualType t = e->GetType();

  // Fully constant lists become a single store of the folded aggregate.
  if (const ir::Constant* c = cgm_.EmitConstantInit(e, t)) {
    builder_.CreateStore(c, dest.ptr, dest.align);
    return;
  }

  // Otherwise: zero the whole object (Sema's semantic form makes zero-fill
  // explicit; nulls in the list mean zero), then store each non-null
  // element through a GEP.
  builder_.CreateStore(cgm_.EmitNullConstant(t), dest.ptr, dest.align);

  const Type* canon = t.GetCanonical().GetTypePtr();
  const auto& inits = e->GetInits();

  if (const auto* at = canon->As<ConstantArrayType>()) {
    QualType elem = at->GetElementType();
    uint64_t align = Ast().GetTypeAlign(elem);
    for (size_t i = 0; i < inits.size(); ++i) {
      if (!inits[i]) continue;
      const ir::Value* addr = builder_.CreateGEP(
          ConvertType(elem), dest.ptr, {Ir().GetInt64(i)}, "arrayinit");
      Address elem_addr(addr, align);
      if (IsAggregateEvalKind(elem) || elem->IsUnionType()) {
        EmitAggExpr(inits[i], elem_addr);
      } else {
        const ir::Value* v = EmitScalarExpr(inits[i]);
        if (v) builder_.CreateStore(v, elem_addr.ptr, elem_addr.align);
      }
    }
    return;
  }

  const auto* rt = canon->As<RecordType>();
  assert(rt && "init list for non-aggregate");
  const RecordDecl* rd = rt->GetDecl();
  const CodeGenTypes::RecordInfo& info = cgm_.GetTypes().GetRecordInfo(rd);
  const auto& fields = rd->GetFields();

  for (size_t i = 0; i < inits.size() && i < fields.size(); ++i) {
    const Expr* init = inits[i];
    if (!init) continue;
    const FieldDecl* field = rd->IsUnion() && e->GetInitializedField()
                                 ? e->GetInitializedField()
                                 : fields[i];

    LValue field_lv;
    if (field->IsBitField()) {
      auto bit = info.bit_fields.find(field);
      if (bit == info.bit_fields.end()) continue;  // zero-width
      const BitFieldInfo& bf = bit->second;
      const ir::Value* storage = dest.ptr;
      if (bf.storage_offset != 0) {
        storage = builder_.CreateGEP(Ir().GetInt8Type(), dest.ptr,
                                     {Ir().GetInt64(bf.storage_offset)},
                                     "bf.addr");
      }
      field_lv = LValue::MakeBitField(Address(storage, bf.storage_size / 8),
                                      field->GetType(), &bit->second);
    } else if (rd->IsUnion()) {
      field_lv = LValue::MakeAddr(dest, field->GetType());
    } else {
      auto idx = info.field_index.find(field);
      if (idx == info.field_index.end()) continue;
      const ir::Value* addr = builder_.CreateGEP(
          info.type, dest.ptr,
          {Ir().GetInt32(0), Ir().GetInt32(idx->second)},
          std::string(field->GetName()));
      field_lv = LValue::MakeAddr(
          Address(addr, Ast().GetTypeAlign(field->GetType())),
          field->GetType());
    }

    QualType ft = field->GetType();
    if (!field->IsBitField() &&
        (IsAggregateEvalKind(ft) || ft->IsUnionType())) {
      EmitAggExpr(init, field_lv.GetAddress());
    } else {
      const ir::Value* v = EmitScalarExpr(init);
      if (v) EmitStoreOfScalar(v, field_lv);
    }

    if (rd->IsUnion()) break;  // exactly one member initialized
  }
}

RValue CodeGenFunction::EmitAnyExpr(const Expr* e) {
  QualType t = e->GetType();
  if (IsAggregateEvalKind(t) || t->IsUnionType()) {
    Address temp = CreateTempAlloca(t, "agg.tmp");
    EmitAggExpr(e, temp);
    return RValue::GetAggregate(temp);
  }
  return RValue::Get(EmitScalarExpr(e));
}

void CodeGenFunction::EmitIgnoredExpr(const Expr* e) {
  if (!e) return;
  QualType t = e->GetType();
  if (IsAggregateEvalKind(t) || t->IsUnionType()) {
    if (const auto* bo = e->As<BinaryOperator>()) {
      if (bo->GetOpcode() == BinaryOperatorKind::kAssign) {
        LValue lhs = EmitLValue(bo->GetLHS());
        EmitAggExpr(bo->GetRHS(), lhs.GetAddress());
        return;
      }
    }
    EmitAnyExpr(e);
    return;
  }
  EmitScalarExpr(e);
}

}  // namespace bcc::codegen
