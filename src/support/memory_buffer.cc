#include "bcc/support/memory_buffer.hh"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>

namespace bcc {
namespace {

class StringBuffer final : public MemoryBuffer {
 public:
  StringBuffer(std::string content, std::string identifier) {
    identifier_ = std::move(identifier);

    // C++11 guarantees a null terminator, but we don't want to include it in
    // the buffer view.
    content_ = std::move(content);
    Init(content_.data(), content_.data() + content_.size());
  }

  ~StringBuffer() override = default;

 private:
  std::string content_;
};

class MMapBuffer final : public MemoryBuffer {
 public:
  MMapBuffer(void* base, size_t file_size, std::string identifier)
      : map_size_(file_size) {
    identifier_ = std::move(identifier);
    const char* p = static_cast<const char*>(base);
    Init(p, p + file_size);
  }

  ~MMapBuffer() override {
    ::munmap(const_cast<char*>(GetBufferStart()), map_size_);
  }

 private:
  size_t map_size_;
};

struct UniqueFd {
  int fd = -1;

  explicit UniqueFd(int f) : fd(f) {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  ~UniqueFd() {
    if (fd >= 0) ::close(fd);
  }

  explicit operator bool() const { return fd >= 0; }
};

}  // namespace

std::unique_ptr<MemoryBuffer> MemoryBuffer::GetFile(std::string_view path) {
  std::string path_str(path);
  const UniqueFd ufd(::open(path_str.c_str(), O_RDONLY));

  if (!ufd) return nullptr;

  struct stat st{};

  if (::fstat(ufd.fd, &st) != 0) return nullptr;

  if (!S_ISREG(st.st_mode)) return nullptr;

  if (st.st_size <= 0) {
    return std::make_unique<StringBuffer>("", std::move(path_str));
  }

  void* base = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                      MAP_PRIVATE, ufd.fd, 0);

  if (base == MAP_FAILED) return nullptr;

  return std::make_unique<MMapBuffer>(base, static_cast<size_t>(st.st_size),
                                      std::move(path_str));
}

std::unique_ptr<MemoryBuffer> MemoryBuffer::GetMemoryBufferCopy(
    std::string content, std::string identifier) {
  return std::make_unique<StringBuffer>(std::move(content),
                                        std::move(identifier));
}

std::unique_ptr<MemoryBuffer> MemoryBuffer::GetStdin() {
  std::string content;
  std::array<char, BUFSIZ> buffer;

  while (true) {
    const ssize_t bytes_read =
        ::read(STDIN_FILENO, buffer.data(), buffer.size());

    if (bytes_read == 0) break;

    if (bytes_read < 0) {
      if (errno == EINTR) continue;

      return nullptr;
    }

    content.append(buffer.data(), static_cast<size_t>(bytes_read));
  }

  return std::make_unique<StringBuffer>(std::move(content), "<stdin>");
}

}  // namespace bcc
