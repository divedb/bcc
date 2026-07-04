#pragma once

namespace bcc {

/// \brief Classifies a source file as user code, system code, or system code
///        that is implicitly `extern "C"` in C++ mode.
///
/// Mirrors Clang's `SrcMgr::CharacteristicKind`.
/// The characteristic of a file is learned when it is found on the search path
/// (system directories yield system headers) and may be overridden to system by
/// `#pragma GCC system_header`. Diagnostics consult it to suppress warnings in
/// system headers.
enum class CharacteristicKind {
  kUser,    ///< User code: the main file, quoted includes, -iquote paths.
  kSystem,  ///< Found on a system search path, or marked system_header.
  kExternCSystem,  ///< A system header treated as `extern "C"` in C++ mode.
};

/// \return True for any system flavor (system or extern-C-system), false for
///         user code. This is the test used for warning suppression.
inline bool IsSystemHeader(CharacteristicKind ck) {
  return ck != CharacteristicKind::kUser;
}

}  // namespace bcc
