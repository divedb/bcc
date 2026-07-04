#pragma once

#include <string>
#include <string_view>

namespace bcc {

/// \brief Checks if the given path is an absolute path.
///
/// \param path The path to check.
/// \return     True if the path is absolute; otherwise false.
constexpr bool IsAbsolutePath(std::string_view path) {
  return !path.empty() && path.front() == '/';
}

/// \brief Joins a directory and a filename into a single path.
///
/// \param dir      The directory path.
/// \param filename The filename to append to the directory.
/// \return         The combined path.
constexpr std::string JoinPath(std::string_view dir,
                               std::string_view filename) {
  if (IsAbsolutePath(filename)) return std::string(filename);

  if (dir.empty()) return std::string(filename);

  std::string out(dir);

  if (out.back() != '/') out.push_back('/');

  out.append(filename);

  return out;
}

}  // namespace bcc