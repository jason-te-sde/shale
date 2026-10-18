#ifndef SHALE_DB_INFO_LOG_H_
#define SHALE_DB_INFO_LOG_H_

// The informational log: the file a person reads to find out what the database did, and why.

#include <memory>
#include <string>
#include <string_view>

#include "shale/env.h"
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

/// Appends timestamped lines to the file LOG in a database directory, one line for each call.
///
/// The previous run's LOG is renamed to LOG.old when this is created, so that the last two runs are
/// there to read and no more. A line is handed to the operating system as it is logged, so that it
/// is in the file if the process dies on the next instruction.
///
/// The log is for people and never for the program: a failure to create or write the file is not
/// reported to anyone. The first failed write ends the logging, since the Env allows nothing but
/// Close() after one. Safe to use from any number of threads, with the database's mutex held or
/// not.
class InfoLog {
 public:
  /// Renames an existing LOG in `dbname` to LOG.old and creates a new one. Neither the Env, which
  /// is not owned, nor the directory is touched again after the object is gone. If either step
  /// fails, nothing is logged.
  /// @param env the Env to write through; must outlive this object.
  /// @param dbname the database directory.
  InfoLog(Env* env, const std::string& dbname);

  /// Closes the file.
  ~InfoLog();

  InfoLog(const InfoLog&) = delete;
  InfoLog& operator=(const InfoLog&) = delete;

  /// Appends `line`, preceded by the time as seconds and microseconds on the Env's clock, and
  /// followed by a newline.
  /// @param line the text of the line, which should not hold a newline.
  void Log(std::string_view line);

 private:
  Env* const env_;
  Mutex mu_;
  std::unique_ptr<WritableFile> file_ SHALE_GUARDED_BY(mu_);  // null if logging has ended
};

}  // namespace shale

#endif  // SHALE_DB_INFO_LOG_H_
