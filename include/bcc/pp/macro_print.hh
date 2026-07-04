#pragma once

#include <string>

namespace bcc {

class IdentifierInfo;
class MacroInfo;

/// \brief Reconstructs the `#define` source text for \p macro (named \p name).
///
/// Produces a single line such as `#define MAX(a, b) ((a) > (b) ? (a) : (b))`,
/// suitable for `-dM` / `-dD` output. Parameter lists, the C99 `...` and the
/// GNU `name...` variadic forms are reproduced, and the replacement list is
/// rejoined using each token's recorded leading-space flag.
std::string FormatMacroDefine(const IdentifierInfo& name, const MacroInfo& macro);

}  // namespace bcc
