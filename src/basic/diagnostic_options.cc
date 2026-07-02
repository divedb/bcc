#include "bcc/basic/diagnostic_options.hh"

namespace bcc {

void DiagnosticOptions::SetSeverity(diag::DiagKind kind, DiagSeverity sev) {
  kind_overrides_[kind] = {false, sev};
}

void DiagnosticOptions::Ignore(diag::DiagKind kind) {
  kind_overrides_[kind] = {true, {}};
}

void DiagnosticOptions::SetGroupSeverity(std::string_view group,
                                         DiagSeverity sev) {
  group_overrides_[std::string(group)] = {false, sev};
}

void DiagnosticOptions::IgnoreGroup(std::string_view group) {
  group_overrides_[std::string(group)] = {true, {}};
}

std::optional<DiagSeverity> DiagnosticOptions::GetEffectiveSeverity(
    diag::DiagKind kind) const noexcept {
  // 1. Per-kind override takes highest priority.
  auto kit = kind_overrides_.find(kind);

  if (kit != kind_overrides_.end()) {
    if (kit->second.suppressed) return std::nullopt;

    return kit->second.severity;
  }

  // 2. Group override.
  std::string_view group = diag::GetDiagGroup(kind);

  if (!group.empty()) {
    auto git = group_overrides_.find(std::string(group));

    if (git != group_overrides_.end()) {
      if (git->second.suppressed) return std::nullopt;

      return git->second.severity;
    }
  }

  // 3. Fall back to the default severity from the diagnostic definition.
  return diag::GetDefaultSeverity(kind);
}

}  // namespace bcc
