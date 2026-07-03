#include "bcc/basic/file_manager.hh"

#include <unistd.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace bcc {
namespace {

struct TempFile {
  std::string path;

  explicit TempFile(std::string_view content = "hello") {
    std::string tmpl = "/tmp/bcc_fm_XXXXXX";

    if (int fd = ::mkstemp(tmpl.data()); fd >= 0) {
      path = std::move(tmpl);
      ::write(fd, content.data(), content.size());
      ::close(fd);
    }
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) noexcept = default;

  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      cleanup();
      path = std::move(other.path);
    }

    return *this;
  }

  ~TempFile() { cleanup(); }

  [[nodiscard]] bool Valid() const { return !path.empty(); }

 private:
  void cleanup() {
    if (!path.empty()) std::filesystem::remove(path);
  }
};

struct TempDir {
  std::string path;

  TempDir() {
    std::string tmpl = "/tmp/bcc_dir_XXXXXX";
    if (::mkdtemp(tmpl.data())) path = std::move(tmpl);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) noexcept = default;
  TempDir& operator=(TempDir&& other) noexcept {
    if (this != &other) {
      cleanup();
      path = std::move(other.path);
    }
    return *this;
  }

  ~TempDir() { cleanup(); }

  [[nodiscard]] bool Valid() const { return !path.empty(); }

 private:
  void cleanup() {
    if (!path.empty()) std::filesystem::remove(path);
  }
};

TEST(FileManagerGetFileTest, MissingFileReturnsNull) {
  FileManager fm;
  EXPECT_EQ(fm.GetFile("/no/such/file/bcc_xyz999"), nullptr);
}

TEST(FileManagerGetFileTest, ExistingFileReturnsNonNull) {
  TempFile tf;
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  EXPECT_NE(fm.GetFile(tf.path), nullptr);
}

TEST(FileManagerGetFileTest, FileEntryNameMatchesPath) {
  TempFile tf;
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  const FileEntry* entry = fm.GetFile(tf.path);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->GetName(), tf.path);
}

TEST(FileManagerGetFileTest, SamePathReturnsSamePointer) {
  TempFile tf;
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  const FileEntry* a = fm.GetFile(tf.path);
  const FileEntry* b = fm.GetFile(tf.path);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a, b);
}

TEST(FileManagerGetFileTest, HardLinkSharesFileEntry) {
  TempFile tf("data");
  ASSERT_TRUE(tf.Valid());
  std::string link_path = tf.path + ".link";
  if (::link(tf.path.c_str(), link_path.c_str()) != 0) {
    GTEST_SKIP() << "hard link creation failed";
  }

  FileManager fm;
  const FileEntry* a = fm.GetFile(tf.path);
  const FileEntry* b = fm.GetFile(link_path);
  ::unlink(link_path.c_str());

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a, b);
}

TEST(FileManagerGetFileTest, SymlinkSharesFileEntryWithTarget) {
  TempFile tf("data");
  ASSERT_TRUE(tf.Valid());
  std::string link_path = tf.path + ".sym";
  if (::symlink(tf.path.c_str(), link_path.c_str()) != 0) {
    GTEST_SKIP() << "symlink creation failed";
  }

  FileManager fm;
  const FileEntry* a = fm.GetFile(tf.path);
  const FileEntry* b = fm.GetFile(link_path);
  ::unlink(link_path.c_str());

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a, b);
}

TEST(FileManagerGetFileTest, DirectoryPathReturnsNull) {
  TempDir td;
  ASSERT_TRUE(td.Valid());
  FileManager fm;
  EXPECT_EQ(fm.GetFile(td.path), nullptr);
}

TEST(FileManagerGetFileTest, NegativeResultIsCached) {
  FileManager fm;
  const FileEntry* a = fm.GetFile("/no/such/file/bcc_xyz999");
  const FileEntry* b = fm.GetFile("/no/such/file/bcc_xyz999");
  EXPECT_EQ(a, nullptr);
  EXPECT_EQ(b, nullptr);
}

// GetNumUniqueFiles

TEST(FileManagerUniqueFilesTest, InitiallyZero) {
  FileManager fm;
  EXPECT_EQ(fm.GetNumUniqueFiles(), 0u);
}

TEST(FileManagerUniqueFilesTest, IncrementsForEachNewInode) {
  TempFile tf1("aaa");
  TempFile tf2("bbb");
  ASSERT_TRUE(tf1.Valid());
  ASSERT_TRUE(tf2.Valid());

  FileManager fm;
  fm.GetFile(tf1.path);
  EXPECT_EQ(fm.GetNumUniqueFiles(), 1u);
  fm.GetFile(tf2.path);
  EXPECT_EQ(fm.GetNumUniqueFiles(), 2u);
}

TEST(FileManagerUniqueFilesTest, CachedPathDoesNotIncrement) {
  TempFile tf;
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  fm.GetFile(tf.path);
  fm.GetFile(tf.path);
  EXPECT_EQ(fm.GetNumUniqueFiles(), 1u);
}

TEST(FileManagerUniqueFilesTest, MissingFileDoesNotIncrement) {
  FileManager fm;
  fm.GetFile("/no/such/file/bcc_xyz999");
  EXPECT_EQ(fm.GetNumUniqueFiles(), 0u);
}

TEST(FileManagerGetStdinTest, ReturnsNonNull) {
  FileManager fm;
  EXPECT_NE(fm.GetStdin(), nullptr);
}

TEST(FileManagerGetStdinTest, ReturnsStablePointer) {
  FileManager fm;
  const FileEntry* a = fm.GetStdin();
  const FileEntry* b = fm.GetStdin();
  EXPECT_EQ(a, b);
}

TEST(FileManagerGetBufferForFileTest, CorrectContent) {
  TempFile tf("hello world");
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  const FileEntry* entry = fm.GetFile(tf.path);
  ASSERT_NE(entry, nullptr);
  auto buf = fm.GetBufferForFile(*entry);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->GetBuffer(), "hello world");
}

TEST(FileManagerGetBufferForFileTest, EmptyFileGivesZeroSize) {
  TempFile tf("");
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  const FileEntry* entry = fm.GetFile(tf.path);
  ASSERT_NE(entry, nullptr);
  auto buf = fm.GetBufferForFile(*entry);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->GetBufferSize(), 0u);
}

TEST(FileManagerGetBufferForFileTest, BufferIdentifierIsFilePath) {
  TempFile tf("data");
  ASSERT_TRUE(tf.Valid());
  FileManager fm;
  const FileEntry* entry = fm.GetFile(tf.path);
  ASSERT_NE(entry, nullptr);
  auto buf = fm.GetBufferForFile(*entry);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->GetBufferIdentifier(), tf.path);
}

TEST(FileManagerWorkingDirTest, ResolvesRelativePath) {
  TempFile tf("content");
  ASSERT_TRUE(tf.Valid());
  auto sep = tf.path.rfind('/');
  std::string dir = tf.path.substr(0, sep);
  std::string name = tf.path.substr(sep + 1);

  FileManager fm({dir});
  const FileEntry* entry = fm.GetFile(name);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->GetName(), dir + "/" + name);
}

TEST(FileManagerWorkingDirTest, AbsolutePathIgnoresWorkingDir) {
  TempFile tf("data");
  ASSERT_TRUE(tf.Valid());
  FileManager fm({"nonexistent_working_dir"});
  EXPECT_NE(fm.GetFile(tf.path), nullptr);
}

}  // namespace
}  // namespace bcc
