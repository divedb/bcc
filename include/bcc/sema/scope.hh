#pragma once

#include <cstdint>
#include <vector>

namespace bcc {

class NamedDecl;
class TagDecl;

/// \brief One lexical scope. The parser pushes/pops scopes; Sema hangs the
///        declarations made in each scope off it so their name bindings can
///        be removed when the scope ends (mirrors Clang's Scope).
class Scope {
 public:
  enum Flags : unsigned {
    kNone = 0,
    /// The outermost scope of a function body: labels live here, and
    /// parameters are injected into it.
    kFn = 0x01,
    /// `break` binds to an enclosing scope with this flag (loops, switch).
    kBreak = 0x02,
    /// `continue` binds to an enclosing scope with this flag (loops).
    kContinue = 0x04,
    /// Declarations may appear here (almost every scope).
    kDecl = 0x08,
    /// The scope of a control statement's condition/init (if/for/while...).
    kControl = 0x10,
    /// A `{}` block scope.
    kBlock = 0x20,
    /// The scope of a switch statement body.
    kSwitch = 0x40,
    /// A function prototype's parameter scope.
    kFnProto = 0x80,
  };

  Scope(Scope* parent, unsigned flags) : parent_(parent), flags_(flags) {
    fn_parent_ = parent ? parent->fn_parent_ : nullptr;
    if (flags & kFn) fn_parent_ = this;
    break_parent_ =
        (flags & kBreak) ? this : (parent ? parent->break_parent_ : nullptr);
    continue_parent_ = (flags & kContinue)
                           ? this
                           : (parent ? parent->continue_parent_ : nullptr);
  }

  Scope* GetParent() const noexcept { return parent_; }
  unsigned GetFlags() const noexcept { return flags_; }
  bool HasFlag(Flags f) const noexcept { return (flags_ & f) != 0; }

  Scope* GetFnParent() const noexcept { return fn_parent_; }
  Scope* GetBreakParent() const noexcept { return break_parent_; }
  Scope* GetContinueParent() const noexcept { return continue_parent_; }

  /// Declarations in the ordinary namespace made directly in this scope.
  void AddOrdinaryDecl(NamedDecl* d) { ordinary_decls_.push_back(d); }
  const std::vector<NamedDecl*>& GetOrdinaryDecls() const noexcept {
    return ordinary_decls_;
  }

  /// Declarations in the tag namespace made directly in this scope.
  void AddTagDecl(TagDecl* d) { tag_decls_.push_back(d); }
  const std::vector<TagDecl*>& GetTagDecls() const noexcept {
    return tag_decls_;
  }

 private:
  Scope* parent_;
  Scope* fn_parent_;
  Scope* break_parent_;
  Scope* continue_parent_;
  unsigned flags_;

  std::vector<NamedDecl*> ordinary_decls_;
  std::vector<TagDecl*> tag_decls_;
};

}  // namespace bcc
