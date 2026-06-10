#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace bcc {

struct CodePointRange {
  uint32_t lo;
  uint32_t hi;

  constexpr bool Contains(uint32_t c) const noexcept {
    return lo <= c && c <= hi;
  }
};

/// C17 Annex D (normative)
/// D.1 Ranges of characters allowed
///     00A8,
///     00AA,
///     00AD,
///     00AF,
///     00B2–00B5,
///     00B7–00BA,
///     00BC–00BE,
///     00C0–00D6,
///     00D8–00F6,
///     00F8–00FF
///
///     0100–167F,
///     1681–180D,
///     180F–1FFF
///
///     200B–200D,
///     202A–202E,
///     203F–2040,
///     2054,
///     2060–206F
///
///     2070–218F,
///     2460–24FF,
///     2776–2793,
///     2C00–2DFF,
///     2E80–2FFF
///
///     3004–3007,
///     3021–302F,
///     3031–303F
///
///     3040–D7FF
///
///     F900–FD3D,
///     FD40–FDCF,
///     FDF0–FE44,
///     FE47–FFFD
///
///     10000–1FFFD,
///     20000–2FFFD,
///     30000–3FFFD,
///     40000–4FFFD,
///     50000–5FFFD,
///     60000–6FFFD,
///     70000–7FFFD,
///     80000–8FFFD,
///     90000–9FFFD,
///     A0000–AFFFD,
///     B0000–BFFFD,
///     C0000–CFFFD,
///     D0000–DFFFD,
///     E0000–EFFFD
///
/// D.2 Ranges of characters disallowed initially
/// 0300–036F, 1DC0–1DFF, 20D0–20FF, FE20–FE2F
///
/// GNU/Clang extension: allow '$' in identifiers.
inline constexpr CodePointRange kIdentStartRanges[] = {
    {'$', '$'},         {'A', 'Z'},         {'_', '_'},
    {'a', 'z'},         {0x00A8, 0x00A8},   {0x00AA, 0x00AA},
    {0x00AD, 0x00AD},   {0x00AF, 0x00AF},   {0x00B2, 0x00B5},
    {0x00B7, 0x00BA},   {0x00BC, 0x00BE},   {0x00C0, 0x00D6},
    {0x00D8, 0x00F6},   {0x00F8, 0x00FF},   {0x0100, 0x02FF},
    {0x0370, 0x167F},   {0x1681, 0x180D},   {0x180F, 0x1DBF},
    {0x1E00, 0x1FFF},   {0x200B, 0x200D},   {0x202A, 0x202E},
    {0x203F, 0x2040},   {0x2054, 0x2054},   {0x2060, 0x206F},
    {0x2070, 0x20CF},   {0x2100, 0x218F},   {0x2460, 0x24FF},
    {0x2776, 0x2793},   {0x2C00, 0x2DFF},   {0x2E80, 0x2FFF},
    {0x3004, 0x3007},   {0x3021, 0x302F},   {0x3031, 0x303F},
    {0x3040, 0xD7FF},   {0xF900, 0xFD3D},   {0xFD40, 0xFDCF},
    {0xFDF0, 0xFE1F},   {0xFE30, 0xFE44},   {0xFE47, 0xFFFD},
    {0x10000, 0x1FFFD}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    {0x40000, 0x4FFFD}, {0x50000, 0x5FFFD}, {0x60000, 0x6FFFD},
    {0x70000, 0x7FFFD}, {0x80000, 0x8FFFD}, {0x90000, 0x9FFFD},
    {0xA0000, 0xAFFFD}, {0xB0000, 0xBFFFD}, {0xC0000, 0xCFFFD},
    {0xD0000, 0xDFFFD}, {0xE0000, 0xEFFFD},
};

inline constexpr CodePointRange kIdentContinueRanges[] = {
    {'0', '9'},       {0x0300, 0x036F}, {0x1DC0, 0x1DFF},
    {0x20D0, 0x20FF}, {0xFE20, 0xFE2F},
};

namespace detail {

/// \brief Tests whether a Unicode code point appears in a range table.
///
/// This helper is used to implement identifier classification according to
/// C17 Annex D. The range table must be sorted and non-overlapping.
///
/// Lookup complexity is O(log N).
///
/// \param ranges Sorted code point ranges.
/// \param c      Unicode code point.
/// \return       True if \p c falls within any range in \p ranges.
constexpr bool InRange(std::span<const CodePointRange> ranges, uint32_t c) {
  auto it = std::lower_bound(
      ranges.begin(), ranges.end(), c,
      [](const CodePointRange& r, uint32_t c) { return r.hi < c; });

  return it != ranges.end() && it->Contains(c);
}

}  // namespace detail

constexpr bool IsIdentifierStart(uint32_t c) {
  return detail::InRange(kIdentStartRanges, c);
}

constexpr bool IsIdentifierContinue(uint32_t c) {
  return IsIdentifierStart(c) || detail::InRange(kIdentContinueRanges, c);
}

static_assert(IsIdentifierStart('_'));
static_assert(IsIdentifierStart(0x00A8));
static_assert(!IsIdentifierStart(0x0300));
static_assert(IsIdentifierContinue(0x0300));
static_assert(!IsIdentifierContinue('/'));

}  // namespace bcc
