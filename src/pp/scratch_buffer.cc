#include "bcc/pp/scratch_buffer.hh"

#include <string>

#include "bcc/basic/source_manager.hh"

namespace bcc {

SourceLocation ScratchBuffer::GetToken(std::string_view text,
                                       const char*& out_data) {
  FileID fid = sm_.CreateFileID("<scratch space>", std::string(text));
  out_data = sm_.GetBufferData(fid).data();
  return sm_.GetLocForStartOfFile(fid);
}

}  // namespace bcc
