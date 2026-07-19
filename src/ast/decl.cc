#include "bcc/ast/decl.hh"

#include "bcc/pp/identifier_table.hh"

namespace bcc {

std::string_view NamedDecl::GetName() const noexcept {
  return name_ ? name_->GetName() : std::string_view{};
}

const FieldDecl* RecordDecl::FindField(
    const IdentifierInfo* name, std::vector<const FieldDecl*>* path) const {
  for (const FieldDecl* field : fields_) {
    if (field->GetIdentifier() == name) {
      if (path) path->push_back(field);
      return field;
    }
    // An unnamed field of struct/union type is an anonymous member whose
    // fields are looked up as if they were members of this record.
    if (field->GetIdentifier() == nullptr) {
      if (const auto* rt =
              field->GetType().GetCanonical().GetTypePtr()->As<RecordType>()) {
        std::size_t mark = path ? path->size() : 0;
        if (path) path->push_back(field);
        if (const FieldDecl* found = rt->GetDecl()->FindField(name, path)) {
          return found;
        }
        if (path) path->resize(mark);
      }
    }
  }
  return nullptr;
}

}  // namespace bcc
