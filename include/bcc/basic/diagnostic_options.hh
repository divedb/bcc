#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bcc/basic/diagnostic_ids.hh"

namespace bcc {

/// \brief Runtime policy controlling diagnostic severity.
///
/// Determines whether individual diagnostics or diagnostic groups are emitted,
/// suppressed, or promoted/demoted to a different severity. Applied by
/// DiagnosticsEngine before a diagnostic reaches any consumer.
///
/// Priority (highest to lowest):
///   1. Per-kind override (SetSeverity / Ignore)
///   2. Group override (SetGroupSeverity / IgnoreGroup)
///   3. Default severity from the diagnostic definition
class DiagnosticOptions {
 public:
  /// \brief Override the effective severity for a specific diagnostic kind.
  ///
  /// \param kind The diagnostic kind to override.
  /// \param sev  The new severity for the diagnostic kind.
  void SetSeverity(diag::DiagKind kind, DiagSeverity sev);

  /// \brief Suppress a specific diagnostic kind (never emitted).
  ///
  /// \param kind The diagnostic kind to suppress.
  void Ignore(diag::DiagKind kind);

  /// \brief Override the effective severity for all diagnostics in \p group.
  ///
  /// \param group The diagnostic group to override.
  /// \param sev   The new severity for the diagnostic group.
  void SetGroupSeverity(std::string_view group, DiagSeverity sev);

  /// \brief Suppress all diagnostics in \p group.
  ///
  /// \param group The diagnostic group to suppress.
  void IgnoreGroup(std::string_view group);

  /// \brief Returns the effective severity for \p kind after applying
  ///        overrides.
  ///
  /// \return The effective severity for \p kind, or std::nullopt if the
  ///         diagnostic is suppressed.
  std::optional<DiagSeverity> GetEffectiveSeverity(
      diag::DiagKind kind) const noexcept;

 private:
  struct Override {
    bool suppressed;
    DiagSeverity severity;
  };

  std::unordered_map<diag::DiagKind, Override> kind_overrides_;
  std::unordered_map<std::string, Override> group_overrides_;
};

}  // namespace bcc
