#include "session/zenz_client_factory.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#define MOZC_ENABLE_POSIX_ZENZ_CLIENT 1
#endif  // TARGET_OS_OSX
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(OS_CHROMEOS)
#define MOZC_ENABLE_POSIX_ZENZ_CLIENT 1
#endif  // __APPLE__ / Linux desktop

#ifdef MOZC_ENABLE_POSIX_ZENZ_CLIENT
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <system_error>
#include <thread>

#include "absl/log/log.h"
#include "base/file_util.h"
#include "base/process.h"
#include "base/system_util.h"
#endif  // MOZC_ENABLE_POSIX_ZENZ_CLIENT

#include "session/zenz_live_corrector.h"
#include "session/zenz_named_pipe_client.h"
#include "session/zenz_unix_socket_client.h"
#include "zenz/zenz_unix_socket_path.h"

namespace mozc {
namespace session {

#ifdef MOZC_ENABLE_POSIX_ZENZ_CLIENT
// Testable implementation shared by the production scorer launcher and the
// POSIX regression test. Do not alter child ownership or SIGCHLD handling.
namespace internal {
// The converter/server is the scorer's parent and lives for the whole login
// session. The scorer can exit on its own (startup failure, losing the
// singleton lock to an already running scorer, or a crash), so it must be
// reaped here or it lingers as a zombie child.
void ReapScorerAsync(pid_t pid) {
  try {
    std::thread([pid] {
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }).detach();
  } catch (const std::system_error& e) {
    LOG(ERROR) << "Failed to start the Zenz scorer reaper: " << e.what();
  }
}

}  // namespace internal
#endif  // MOZC_ENABLE_POSIX_ZENZ_CLIENT

namespace {

#ifdef MOZC_ENABLE_POSIX_ZENZ_CLIENT

constexpr char kZenzRuntimeDirectory[] = "ZenzRuntime";
constexpr char kZenzScorerExecutable[] = "mozc_zenz_scorer";
constexpr auto kScorerLaunchThrottle = std::chrono::seconds(2);

bool LaunchPosixZenzScorer() {
  // The converter is normally a singleton, but several Zenz worker requests
  // can observe the same cold start. Serialize the short spawn operation and
  // treat a recent successful launch as one already in progress.
  static std::mutex launch_mutex;
  static std::chrono::steady_clock::time_point last_launch;
  static bool has_launched = false;

  const std::lock_guard<std::mutex> lock(launch_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (has_launched && now - last_launch < kScorerLaunchThrottle) {
    return true;
  }

  const std::string scorer_path = FileUtil::JoinPath({
      SystemUtil::GetServerDirectory(),
      kZenzRuntimeDirectory,
      kZenzScorerExecutable,
  });

  size_t pid = 0;
  if (!Process::SpawnProcess(scorer_path, "", &pid)) {
    LOG(ERROR) << "Failed to launch the bundled Zenz scorer";
    return false;
  }
  if (pid > 0) {
    internal::ReapScorerAsync(static_cast<pid_t>(pid));
  }

  has_launched = true;
  last_launch = now;
  return true;
}

#endif  // MOZC_ENABLE_POSIX_ZENZ_CLIENT

}  // namespace

std::unique_ptr<ZenzClient> CreateZenzClient() {
#ifdef MOZC_ENABLE_POSIX_ZENZ_CLIENT
  return std::make_unique<ZenzUnixSocketClient>(
      ::mozc::zenz::GetZenzUnixSocketPath(), &LaunchPosixZenzScorer);
#else
  return std::make_unique<ZenzNamedPipeClient>();
#endif  // MOZC_ENABLE_POSIX_ZENZ_CLIENT
}

}  // namespace session
}  // namespace mozc
