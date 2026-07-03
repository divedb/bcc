#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bcc/as/mcvalue.hh"

namespace bcc::as {

struct Symbol;

/// \brief A deferred relocation/patch recorded while emitting a section.
///
/// A fixup names a field of `size` bytes at `offset` within its owning section
/// that depends on \ref value. At finalization it is either resolved to a
/// constant and patched in place, or lowered to an ELF `.rela` entry.
///
/// For PC-relative fixups, `trailing` is the number of instruction bytes that
/// follow the field (e.g. an immediate after a rip-relative displacement); the
/// emitted relocation addend is `value.addend - (size + trailing)`, matching
/// the x86-64 `S + A - P` convention where P points at the field start.
struct Fixup {
  uint64_t offset = 0;
  MCValue value;
  uint8_t size = 4;         ///< 1, 2, 4, or 8.
  bool pcrel = false;       ///< PC-relative (branches, rip-relative memory).
  bool sign_extend = false; ///< Prefer R_X86_64_32S over R_X86_64_32.
  uint8_t trailing = 0;     ///< Bytes after the field to end of instruction.
  bool is_branch = false;   ///< A branch target: unresolved -> R_X86_64_PLT32.
};

/// \brief A recorded `.align`/`.p2align` point, so branch relaxation can
///        recompute its padding after instruction sizes change.
struct AlignPoint {
  uint64_t offset = 0;   ///< Location of the padding (pre-relaxation).
  uint32_t boundary = 1; ///< Required power-of-two byte alignment.
  uint8_t fill = 0;      ///< Fill byte (ignored when `use_nop`).
  bool use_nop = false;  ///< Pad executable sections with multi-byte NOPs.
};

/// \brief One output section: a growable byte buffer (or a size, for `.bss`)
///        plus the fixups recorded against it.
class Section {
 public:
  Section(std::string name, uint32_t sh_type, uint64_t sh_flags)
      : name_(std::move(name)), sh_type_(sh_type), sh_flags_(sh_flags) {}

  const std::string& name() const noexcept { return name_; }
  uint32_t sh_type() const noexcept { return sh_type_; }
  uint64_t sh_flags() const noexcept { return sh_flags_; }

  uint32_t align() const noexcept { return align_; }
  void BumpAlign(uint32_t a) noexcept {
    if (a > align_) align_ = a;
  }

  bool is_bss() const noexcept { return is_bss_; }
  void set_is_bss(bool v) noexcept { is_bss_ = v; }

  std::vector<uint8_t>& data() noexcept { return data_; }
  const std::vector<uint8_t>& data() const noexcept { return data_; }
  std::vector<Fixup>& fixups() noexcept { return fixups_; }
  const std::vector<Fixup>& fixups() const noexcept { return fixups_; }

  /// Current location counter: number of bytes emitted so far.
  uint64_t Offset() const noexcept {
    return is_bss_ ? bss_size_ : data_.size();
  }

  void AppendByte(uint8_t b) {
    if (is_bss_)
      ++bss_size_;
    else
      data_.push_back(b);
  }

  void Append(const uint8_t* p, size_t n) {
    if (is_bss_)
      bss_size_ += n;
    else
      data_.insert(data_.end(), p, p + n);
  }

  /// Emit \p n low-order bytes of \p v in little-endian order.
  void AppendLE(uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) AppendByte(static_cast<uint8_t>(v >> (8 * i)));
  }

  /// Emit \p n bytes of fill value \p b.
  void Fill(size_t n, uint8_t b) {
    if (is_bss_)
      bss_size_ += n;
    else
      data_.insert(data_.end(), n, b);
  }

  void AddFixup(const Fixup& f) { fixups_.push_back(f); }

  void AddAlignment(const AlignPoint& a) { aligns_.push_back(a); }
  std::vector<AlignPoint>& aligns() noexcept { return aligns_; }
  const std::vector<AlignPoint>& aligns() const noexcept { return aligns_; }

  /// Patch \p n little-endian bytes at \p off (used to resolve fixups in place).
  void PatchLE(uint64_t off, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
      data_[off + i] = static_cast<uint8_t>(v >> (8 * i));
  }

  // Fields filled in during ELF emission.
  Symbol* section_symbol = nullptr;  ///< STT_SECTION symbol for relocations.
  uint32_t elf_index = 0;            ///< Section-header index of this section.
  uint32_t rela_elf_index = 0;       ///< Section-header index of `.rela.NAME`.
  uint64_t file_offset = 0;          ///< sh_offset assigned at layout time.

 private:
  std::string name_;
  uint32_t sh_type_;
  uint64_t sh_flags_;
  uint32_t align_ = 1;
  bool is_bss_ = false;
  std::vector<uint8_t> data_;
  uint64_t bss_size_ = 0;
  std::vector<Fixup> fixups_;
  std::vector<AlignPoint> aligns_;
};

}  // namespace bcc::as
