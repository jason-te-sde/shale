#include "shale/env.h"

namespace shale {

// The destructors are defined here, in one translation unit, so that the vtable of each interface
// is emitted once and not in every file that includes the header.
SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
FileLock::~FileLock() = default;
Env::~Env() = default;

}  // namespace shale
