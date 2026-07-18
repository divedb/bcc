#include "bcc/pp/identifier_table.hh"

#include <string>
#include <string_view>
#include <utility>

namespace bcc {

IdentifierTable::IdentifierTable() { AddKeywords(); }

IdentifierInfo& IdentifierTable::Get(std::string_view name) {
  if (auto it = map_.find(name); it != map_.end()) {
    return it->second;
  }

  auto [it, inserted] = map_.try_emplace(std::string(name));
  IdentifierInfo& info = it->second;
  // Point name_ at the stable key string owned by the node.
  info.name_ = it->first;

  return info;
}

void IdentifierTable::AddKeywords() {
  // C keywords occupy the contiguous range [kFirstKeyword, kLastKeyword], and
  // TokenKindName() yields each keyword's exact spelling.
  for (auto v = static_cast<uint16_t>(kFirstKeyword);
       v <= static_cast<uint16_t>(kLastKeyword); ++v) {
    auto kind = static_cast<TokenKind>(v);
    Get(TokenKindName(kind)).token_kind_ = kind;
  }

  // Preprocessor keywords. Some spellings (e.g. "if", "else") were already
  // interned above as C keywords; Get() returns the same IdentifierInfo, and
  // we additionally tag it with its pp-keyword id.
#define PP_KEYWORD(id, spelling) Get(spelling).pp_keyword_ = PPKeyword::id;
#include "bcc/pp/pp_keywords.def"
#undef PP_KEYWORD
}

}  // namespace bcc
