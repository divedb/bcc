#include "bcc/basic/diagnostic.hh"

#include "bcc/common/string_util.hh"

namespace bcc {
namespace diag {

namespace {

// Returns max(%N index) + 1 across all placeholders, i.e. the number of
// argument slots the format string requires.
constexpr uint8_t CountFormatArgs(std::string_view fmt) {
  uint8_t max_idx = 0;

  for (size_t i = 0; i < fmt.size();) {
    // Skip non-placeholder characters.
    if (fmt[i] != '%') {
      ++i;
      continue;
    }

    // Eat the '%' and look for a number.
    ++i;

    if (i >= fmt.size()) break;

    // Handle "%%" escape sequence for a literal percent sign.
    if (fmt[i] == '%') {
      ++i;
      continue;
    }

    if (!IsDigit(static_cast<unsigned char>(fmt[i]))) continue;

    unsigned idx = 0;

    while (i < fmt.size() && IsDigit(static_cast<unsigned char>(fmt[i]))) {
      idx = idx * 10u + static_cast<unsigned>(fmt[i++] - '0');
    }

    max_idx = std::max(max_idx, static_cast<uint8_t>(idx + 1));
  }

  return max_idx;
}

struct DiagInfo {
  DiagSeverity default_severity;
  std::string_view format_string;
  std::string_view group;
  uint8_t num_args;
};

constexpr DiagInfo kDiagInfo[] = {
#define DIAG(id, sev, group, msg) \
  {DiagSeverity::sev, msg, group, CountFormatArgs(msg)},
#include "bcc/basic/diag_kinds.def"
#undef DIAG
};

}  // namespace

DiagSeverity GetDefaultSeverity(DiagKind kind) noexcept {
  assert(static_cast<unsigned>(kind) < kNumDiags && "invalid diagnostic kind");

  return kDiagInfo[kind].default_severity;
}

std::string_view GetFormatString(DiagKind kind) noexcept {
  assert(static_cast<unsigned>(kind) < kNumDiags && "invalid diagnostic kind");

  return kDiagInfo[kind].format_string;
}

uint8_t GetExpectedArgCount(DiagKind kind) noexcept {
  assert(static_cast<unsigned>(kind) < kNumDiags && "invalid diagnostic kind");

  return kDiagInfo[kind].num_args;
}

std::string_view GetDiagGroup(DiagKind kind) noexcept {
  assert(static_cast<unsigned>(kind) < kNumDiags && "invalid diagnostic kind");

  return kDiagInfo[kind].group;
}

}  // namespace diag

std::string DiagnosticInfo::FormatMessage() const {
  std::string_view tmpl = diag::GetFormatString(kind_);
  std::string result;
  result.reserve(tmpl.size() + 32);

  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] != '%') {
      result += tmpl[i++];
      continue;
    }

    ++i;

    if (i >= tmpl.size()) {
      result += '%';
      break;
    }

    if (tmpl[i] == '%') {
      result += '%';
      ++i;
      continue;
    }

    if (!IsDigit(static_cast<unsigned char>(tmpl[i]))) {
      result += '%';
      continue;
    }

    unsigned idx = 0;

    while (i < tmpl.size() && IsDigit(static_cast<unsigned char>(tmpl[i]))) {
      idx = idx * 10u + static_cast<unsigned>(tmpl[i++] - '0');
    }

    assert(idx < args_.size() && "diagnostic argument index out of range");

    if (idx < args_.size()) {
      result += args_[idx];
    } else {
      result += "<missing arg ";
      result += std::to_string(idx);
      result += '>';
    }
  }

  return result;
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder&& o) noexcept
    : engine_(o.engine_),
      loc_(o.loc_),
      kind_(o.kind_),
      num_args_(o.num_args_),
      num_ranges_(o.num_ranges_),
      num_fix_its_(o.num_fix_its_),
      emitted_(o.emitted_) {
  for (unsigned i = 0; i < num_args_; ++i) args_[i] = std::move(o.args_[i]);

  for (unsigned i = 0; i < num_ranges_; ++i) ranges_[i] = o.ranges_[i];

  for (unsigned i = 0; i < num_fix_its_; ++i) {
    fix_its_[i] = std::move(o.fix_its_[i]);
  }

  o.emitted_ = true;
}

void DiagnosticBuilder::Emit() {
  if (emitted_ || !engine_) return;

  emitted_ = true;

  assert(num_args_ == diag::GetExpectedArgCount(kind_) &&
         "argument count mismatch for diagnostic");

  engine_->EmitDiagnostic(
      loc_, kind_, std::span<const std::string>(args_.data(), num_args_),
      std::span<const SourceRange>(ranges_.data(), num_ranges_),
      std::span<const FixItHint>(fix_its_.data(), num_fix_its_));
}

DiagnosticsEngine::DiagnosticsEngine(DiagnosticConsumer* consumer,
                                     const SourceManager* sm)
    : consumer_(consumer), sm_(sm) {}

DiagnosticBuilder DiagnosticsEngine::Report(SourceLocation loc,
                                            diag::DiagKind kind) {
  return DiagnosticBuilder(*this, loc, kind);
}

void DiagnosticsEngine::EmitDiagnostic(SourceLocation loc, diag::DiagKind kind,
                                       std::span<const std::string> args,
                                       std::span<const SourceRange> ranges,
                                       std::span<const FixItHint> fix_its) {
  if (suppress_all_) return;

  // Resolve effective severity through the policy. nullopt means suppressed.
  auto effective = options_.GetEffectiveSeverity(kind);

  if (!effective) return;

  DiagSeverity severity = *effective;

  // After a fatal error suppress everything except notes so the user sees
  // the root cause and its notes, not a cascade of follow-on errors.
  if (fatal_error_occurred_ && severity != DiagSeverity::kNote) return;

  switch (severity) {
    case DiagSeverity::kNote:
      ++num_notes_;
      break;
    case DiagSeverity::kWarning:
      ++num_warnings_;
      break;
    case DiagSeverity::kError:
    case DiagSeverity::kFatal:
      ++num_errors_;
      break;
  }

  if (severity == DiagSeverity::kFatal) fatal_error_occurred_ = true;

  if (consumer_) {
    DiagnosticInfo info(loc, kind, severity, args, ranges, fix_its, sm_);

    consumer_->HandleDiagnostic(info);
  }

  if (error_limit_ > 0 && num_errors_ >= error_limit_) suppress_all_ = true;
}

}  // namespace bcc
