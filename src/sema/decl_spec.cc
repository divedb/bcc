#include "bcc/sema/decl_spec.hh"

namespace bcc {

std::string_view DeclSpec::GetSpecifierName(SCS sc) noexcept {
  switch (sc) {
    case SCS::kUnspecified: return "";
    case SCS::kTypedef: return "typedef";
    case SCS::kExtern: return "extern";
    case SCS::kStatic: return "static";
    case SCS::kAuto: return "auto";
    case SCS::kRegister: return "register";
  }
  return "";
}

std::string_view DeclSpec::GetSpecifierName(TSW w) noexcept {
  switch (w) {
    case TSW::kUnspecified: return "";
    case TSW::kShort: return "short";
    case TSW::kLong: return "long";
    case TSW::kLongLong: return "long long";
  }
  return "";
}

std::string_view DeclSpec::GetSpecifierName(TSS s) noexcept {
  switch (s) {
    case TSS::kUnspecified: return "";
    case TSS::kSigned: return "signed";
    case TSS::kUnsigned: return "unsigned";
  }
  return "";
}

std::string_view DeclSpec::GetSpecifierName(TST t) noexcept {
  switch (t) {
    case TST::kUnspecified: return "";
    case TST::kVoid: return "void";
    case TST::kChar: return "char";
    case TST::kInt: return "int";
    case TST::kFloat: return "float";
    case TST::kDouble: return "double";
    case TST::kBool: return "_Bool";
    case TST::kStruct: return "struct";
    case TST::kUnion: return "union";
    case TST::kEnum: return "enum";
    case TST::kTypedefName: return "type-name";
  }
  return "";
}

bool DeclSpec::SetStorageClass(SCS sc, SourceLocation loc,
                               std::string_view& prev_spec) {
  NoteLoc(loc);
  if (scs_ != SCS::kUnspecified) {
    prev_spec = GetSpecifierName(scs_);
    return false;
  }
  scs_ = sc;
  scs_loc_ = loc;
  return true;
}

bool DeclSpec::SetTypeSpecWidth(TSW w, SourceLocation loc,
                                std::string_view& prev_spec) {
  NoteLoc(loc);
  // `long long` is formed from two `long`s; everything else conflicts.
  if (tsw_ == TSW::kUnspecified) {
    tsw_ = w;
    return true;
  }
  if (tsw_ == TSW::kLong && w == TSW::kLong) {
    tsw_ = TSW::kLongLong;
    return true;
  }
  prev_spec = GetSpecifierName(tsw_);
  return false;
}

bool DeclSpec::SetTypeSpecSign(TSS s, SourceLocation loc,
                               std::string_view& prev_spec) {
  NoteLoc(loc);
  if (tss_ != TSS::kUnspecified) {
    prev_spec = GetSpecifierName(tss_);
    return false;
  }
  tss_ = s;
  return true;
}

bool DeclSpec::SetTypeSpecType(TST t, SourceLocation loc,
                               std::string_view& prev_spec) {
  NoteLoc(loc);
  if (tst_ != TST::kUnspecified) {
    prev_spec = GetSpecifierName(tst_);
    return false;
  }
  tst_ = t;
  tst_loc_ = loc;
  return true;
}

void DeclSpec::SetTypeQual(uint8_t qual_bit, SourceLocation loc) {
  NoteLoc(loc);
  type_quals_ |= qual_bit;
}

}  // namespace bcc
