#pragma once

#include <string_view>

namespace bcc {

class SourceBuffer {
 public:
  explicit SourceBuffer(std::string_view data_view) : data_view_(data_view) {}
  std::string_view Data() const noexcept { return data_view_; }

 private:
  std::string_view data_view_;
};

}  // namespace bcc