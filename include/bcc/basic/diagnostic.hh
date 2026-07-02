#pragma once

#include <array>
#include <cassert>
#include <span>
#include <string>
#include <string_view>

#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/basic/diagnostic_options.hh"
#include "bcc/basic/fix_it_hint.hh"
#include "bcc/basic/source_location.hh"
#include "bcc/basic/source_range.hh"

namespace bcc {

class DiagnosticsEngine;
class SourceManager;

inline constexpr unsigned kMaxDiagArgs = 8;
inline constexpr unsigned kMaxDiagRanges = 8;
inline constexpr unsigned kMaxDiagFixIts = 6;

/// \brief Represents a single diagnostic message with all its associated
///        information.
class DiagnosticInfo {
 public:
  /// \brief Constructs a DiagnosticInfo with the given properties.
  ///
  /// \param loc      The source location of the diagnostic.
  /// \param kind     The kind of diagnostic.
  /// \param severity The severity of the diagnostic.
  /// \param args     The arguments for the diagnostic message.
  /// \param ranges   The source ranges associated with the diagnostic.
  /// \param fix_its  The fix-it hints associated with the diagnostic.
  /// \param sm       The source manager used to resolve source locations.
  DiagnosticInfo(SourceLocation loc, diag::DiagKind kind, DiagSeverity severity,
                 std::span<const std::string> args,
                 std::span<const SourceRange> ranges,
                 std::span<const FixItHint> fix_its, const SourceManager* sm)
      : loc_(loc),
        kind_(kind),
        severity_(severity),
        args_(args),
        ranges_(ranges),
        fix_its_(fix_its),
        sm_(sm) {}

  diag::DiagKind GetDiagKind() const noexcept { return kind_; }
  DiagSeverity GetSeverity() const noexcept { return severity_; }
  SourceLocation GetLocation() const noexcept { return loc_; }
  const SourceManager* GetSourceManager() const noexcept { return sm_; }
  std::span<const SourceRange> GetRanges() const noexcept { return ranges_; }
  std::span<const FixItHint> GetFixItHints() const noexcept { return fix_its_; }
  std::span<const std::string> GetArgs() const noexcept { return args_; }

  /// \brief Formats the diagnostic message by substituting %0, %1, ... with
  ///        the corresponding arguments. Use %% for a literal percent sign.
  ///
  /// \return The formatted diagnostic message.
  std::string FormatMessage() const;

 private:
  SourceLocation loc_;
  diag::DiagKind kind_;
  DiagSeverity severity_;
  std::span<const std::string> args_;
  std::span<const SourceRange> ranges_;
  std::span<const FixItHint> fix_its_;
  const SourceManager* sm_;
};

class DiagnosticConsumer {
 public:
  virtual ~DiagnosticConsumer() = default;

  /// \brief Called once per emitted diagnostic.
  virtual void HandleDiagnostic(const DiagnosticInfo& info) = 0;

  /// \brief Called after all diagnostics have been emitted. Flush here.
  virtual void Finish() {}
};

/// \brief Accumulates arguments for one in-flight diagnostic and emits it on
///        destruction.
///
/// Constructed by DiagnosticsEngine::Report(); do not instantiate directly.
class DiagnosticBuilder {
 public:
  ~DiagnosticBuilder() { Emit(); }

  DiagnosticBuilder(const DiagnosticBuilder&) = delete;
  DiagnosticBuilder& operator=(const DiagnosticBuilder&) = delete;
  DiagnosticBuilder(DiagnosticBuilder&& o) noexcept;

  /// \name Argument-streaming operators
  ///
  /// Stream arguments into the builder before it goes out of scope.
  /// @{
  DiagnosticBuilder& operator<<(std::string arg) {
    assert(num_args_ < kMaxDiagArgs && "too many diagnostic arguments");

    args_[num_args_++] = std::move(arg);

    return *this;
  }

  DiagnosticBuilder& operator<<(std::string_view arg) {
    return *this << std::string(arg);
  }

  DiagnosticBuilder& operator<<(const char* arg) {
    return *this << std::string_view(arg);
  }

  DiagnosticBuilder& operator<<(int arg) {
    return *this << std::to_string(arg);
  }

  DiagnosticBuilder& operator<<(unsigned arg) {
    return *this << std::to_string(arg);
  }

  DiagnosticBuilder& operator<<(long long arg) {
    return *this << std::to_string(arg);
  }

  DiagnosticBuilder& operator<<(unsigned long long arg) {
    return *this << std::to_string(arg);
  }

  DiagnosticBuilder& operator<<(SourceRange range) {
    if (num_ranges_ < kMaxDiagRanges) ranges_[num_ranges_++] = range;

    return *this;
  }

  DiagnosticBuilder& operator<<(FixItHint hint) {
    if (num_fix_its_ < kMaxDiagFixIts) {
      fix_its_[num_fix_its_++] = std::move(hint);
    }

    return *this;
  }
  /// @}

 private:
  friend class DiagnosticsEngine;

  DiagnosticBuilder(DiagnosticsEngine& engine, SourceLocation loc,
                    diag::DiagKind kind)
      : engine_(&engine), loc_(loc), kind_(kind) {}

  void Emit();

  DiagnosticsEngine* engine_ = nullptr;
  SourceLocation loc_;
  diag::DiagKind kind_;

  unsigned num_args_ = 0;
  unsigned num_ranges_ = 0;
  unsigned num_fix_its_ = 0;

  std::array<std::string, kMaxDiagArgs> args_;
  std::array<SourceRange, kMaxDiagRanges> ranges_;
  std::array<FixItHint, kMaxDiagFixIts> fix_its_;

  bool emitted_ = false;
};

/// \brief Represents the engine that manages diagnostics, their emission, and
///        their consumers.
class DiagnosticsEngine {
 public:
  /// \brief Constructs a DiagnosticsEngine with the given consumer and source
  ///        manager.
  ///
  /// \param consumer  Receives every emitted diagnostic. Must outlive this
  ///                  engine. May be nullptr to discard all diagnostics.
  /// \param sm        Optional source manager used for location rendering.
  ///                  Must outlive this engine if non-null.
  explicit DiagnosticsEngine(DiagnosticConsumer* consumer,
                             const SourceManager* sm = nullptr);

  DiagnosticsEngine(const DiagnosticsEngine&) = delete;
  DiagnosticsEngine& operator=(const DiagnosticsEngine&) = delete;

  /// \brief Begin building a diagnostic at \p loc with kind \p kind.
  ///
  /// Stream arguments into the returned builder; the diagnostic is emitted
  /// when the builder is destroyed (RAII). It is valid — and common — to
  /// discard the builder immediately when there are no arguments to add.
  ///
  /// \param loc  The source location of the diagnostic.
  /// \param kind The kind of diagnostic to emit.
  /// \return     A DiagnosticBuilder for streaming arguments and emitting the
  ///             diagnostic.
  DiagnosticBuilder Report(SourceLocation loc, diag::DiagKind kind);

  /// \brief Convenience overload with no source location.
  ///
  /// \param kind The kind of diagnostic to emit.
  /// \return     A DiagnosticBuilder for streaming arguments and emitting the
  ///             diagnostic.
  DiagnosticBuilder Report(diag::DiagKind kind) {
    return Report(SourceLocation{}, kind);
  }

  /// \name Diagnostic counters
  /// @{
  unsigned NumErrors() const noexcept { return num_errors_; }
  unsigned NumWarnings() const noexcept { return num_warnings_; }
  bool HasErrors() const noexcept { return num_errors_ > 0; }
  bool HasFatalError() const noexcept { return fatal_error_occurred_; }
  /// @}

  /// \brief After this call all subsequent non-note diagnostics are dropped.
  ///
  /// \param suppress Whether to suppress all non-note diagnostics.
  void SuppressAllDiagnostics(bool suppress = true) noexcept {
    suppress_all_ = suppress;
  }

  /// \brief Stop emitting non-note diagnostics after \p limit errors.
  ///
  /// \note Zero (the default) means unlimited.
  ///
  /// \param limit The maximum number of errors to emit before suppressing
  ///              further non-note diagnostics.
  void SetErrorLimit(unsigned limit) noexcept { error_limit_ = limit; }

  const SourceManager* GetSourceManager() const noexcept { return sm_; }
  DiagnosticConsumer* GetConsumer() const noexcept { return consumer_; }

  DiagnosticOptions& GetOptions() noexcept { return options_; }
  const DiagnosticOptions& GetOptions() const noexcept { return options_; }

 private:
  friend class DiagnosticBuilder;

  void EmitDiagnostic(SourceLocation loc, diag::DiagKind kind,
                      std::span<const std::string> args,
                      std::span<const SourceRange> ranges,
                      std::span<const FixItHint> fix_its);

  DiagnosticConsumer* consumer_;
  const SourceManager* sm_;
  DiagnosticOptions options_;

  unsigned num_errors_ = 0;
  unsigned num_warnings_ = 0;
  unsigned num_notes_ = 0;
  bool fatal_error_occurred_ = false;
  bool suppress_all_ = false;
  unsigned error_limit_ = 0;
};

/// \brief Represents a diagnostic consumer that ignores all diagnostics.
class IgnoringDiagConsumer final : public DiagnosticConsumer {
 public:
  void HandleDiagnostic(const DiagnosticInfo&) override {}
};

}  // namespace bcc
