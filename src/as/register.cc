#include "bcc/as/register.hh"

#include <string>
#include <unordered_map>

namespace bcc::as {

namespace {

struct Entry {
  const char* name;
  RegClass cls;
  uint8_t num;
};

// The full x86-64 general-purpose register file plus rip.
constexpr Entry kRegs[] = {
    // 64-bit
    {"rax", RegClass::kGpr64, 0},  {"rcx", RegClass::kGpr64, 1},
    {"rdx", RegClass::kGpr64, 2},  {"rbx", RegClass::kGpr64, 3},
    {"rsp", RegClass::kGpr64, 4},  {"rbp", RegClass::kGpr64, 5},
    {"rsi", RegClass::kGpr64, 6},  {"rdi", RegClass::kGpr64, 7},
    {"r8", RegClass::kGpr64, 8},   {"r9", RegClass::kGpr64, 9},
    {"r10", RegClass::kGpr64, 10}, {"r11", RegClass::kGpr64, 11},
    {"r12", RegClass::kGpr64, 12}, {"r13", RegClass::kGpr64, 13},
    {"r14", RegClass::kGpr64, 14}, {"r15", RegClass::kGpr64, 15},
    // 32-bit
    {"eax", RegClass::kGpr32, 0},   {"ecx", RegClass::kGpr32, 1},
    {"edx", RegClass::kGpr32, 2},   {"ebx", RegClass::kGpr32, 3},
    {"esp", RegClass::kGpr32, 4},   {"ebp", RegClass::kGpr32, 5},
    {"esi", RegClass::kGpr32, 6},   {"edi", RegClass::kGpr32, 7},
    {"r8d", RegClass::kGpr32, 8},   {"r9d", RegClass::kGpr32, 9},
    {"r10d", RegClass::kGpr32, 10}, {"r11d", RegClass::kGpr32, 11},
    {"r12d", RegClass::kGpr32, 12}, {"r13d", RegClass::kGpr32, 13},
    {"r14d", RegClass::kGpr32, 14}, {"r15d", RegClass::kGpr32, 15},
    // 16-bit
    {"ax", RegClass::kGpr16, 0},    {"cx", RegClass::kGpr16, 1},
    {"dx", RegClass::kGpr16, 2},    {"bx", RegClass::kGpr16, 3},
    {"sp", RegClass::kGpr16, 4},    {"bp", RegClass::kGpr16, 5},
    {"si", RegClass::kGpr16, 6},    {"di", RegClass::kGpr16, 7},
    {"r8w", RegClass::kGpr16, 8},   {"r9w", RegClass::kGpr16, 9},
    {"r10w", RegClass::kGpr16, 10}, {"r11w", RegClass::kGpr16, 11},
    {"r12w", RegClass::kGpr16, 12}, {"r13w", RegClass::kGpr16, 13},
    {"r14w", RegClass::kGpr16, 14}, {"r15w", RegClass::kGpr16, 15},
    // 8-bit low
    {"al", RegClass::kGpr8, 0},     {"cl", RegClass::kGpr8, 1},
    {"dl", RegClass::kGpr8, 2},     {"bl", RegClass::kGpr8, 3},
    {"spl", RegClass::kGpr8, 4},    {"bpl", RegClass::kGpr8, 5},
    {"sil", RegClass::kGpr8, 6},    {"dil", RegClass::kGpr8, 7},
    {"r8b", RegClass::kGpr8, 8},    {"r9b", RegClass::kGpr8, 9},
    {"r10b", RegClass::kGpr8, 10},  {"r11b", RegClass::kGpr8, 11},
    {"r12b", RegClass::kGpr8, 12},  {"r13b", RegClass::kGpr8, 13},
    {"r14b", RegClass::kGpr8, 14},  {"r15b", RegClass::kGpr8, 15},
    // 8-bit high
    {"ah", RegClass::kGpr8h, 4},    {"ch", RegClass::kGpr8h, 5},
    {"dh", RegClass::kGpr8h, 6},    {"bh", RegClass::kGpr8h, 7},
    // instruction pointer
    {"rip", RegClass::kRip, 0},     {"eip", RegClass::kRip, 0},
};

const std::unordered_map<std::string, Reg>& RegMap() {
  static const std::unordered_map<std::string, Reg> m = [] {
    std::unordered_map<std::string, Reg> map;
    for (const Entry& e : kRegs) map.emplace(e.name, Reg{e.cls, e.num});
    return map;
  }();
  return m;
}

}  // namespace

bool LookupRegister(std::string_view name, Reg& out) {
  const auto& m = RegMap();
  auto it = m.find(std::string(name));
  if (it == m.end()) return false;
  out = it->second;
  return true;
}

}  // namespace bcc::as
