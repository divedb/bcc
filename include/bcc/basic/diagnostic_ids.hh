#pragma once

#include <cstdint>
#include <string_view>

namespace bcc {

/// \brief Severity levels for diagnostics, ordered from least to most severe.
enum class DiagSeverity : uint8_t {
  kNote = 0,
  kWarning,
  kError,
  kFatal,
};

namespace diag {

/// \brief Enumeration of all known diagnostic kinds.
///
/// Generated from diag_kinds.def via X-macros.
enum DiagKind : uint16_t {
#define DIAG(id, sev, group, msg) id,
#include "bcc/basic/diag_kinds.def"
#undef DIAG
  kNumDiags,
};

/// \brief Returns the default severity for \p kind.
///
/// \note If \p kind is not a valid diagnostic kind, assertion is triggered in
///       debug builds.
///
/// \param kind The diagnostic kind to query.
/// \return     The default severity level associated with the diagnostic kind.
DiagSeverity GetDefaultSeverity(DiagKind kind) noexcept;

/// \brief Returns the format string for \p kind.
///
/// Placeholders have the form %N where N is the zero-based argument index.
/// Use %% to produce a literal percent sign.
///
/// \note If \p kind is not a valid diagnostic kind, assertion is triggered in
///       debug builds.
///
/// \param kind The diagnostic kind to query.
/// \return     The format string associated with the diagnostic kind.
std::string_view GetFormatString(DiagKind kind) noexcept;

/// \brief Returns the number of arguments the format string for \p kind
///        requires, i.e. max(%N index) + 1 across all placeholders.
uint8_t GetExpectedArgCount(DiagKind kind) noexcept;

/// \brief Returns the diagnostic group name for \p kind, or an empty
///        string_view if the diagnostic does not belong to any group.
std::string_view GetDiagGroup(DiagKind kind) noexcept;

}  // namespace diag
}  // namespace bcc
