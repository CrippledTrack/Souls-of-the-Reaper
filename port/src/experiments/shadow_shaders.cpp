// Experiment (gpu-layer-experiment branch): automatic capture and conversion of the game's
// shaders for the Vulkan renderer. See shadow_shaders.h.
#include "experiments/shadow_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
extern char **environ;
#endif

REXCVAR_DECLARE(std::string, shadow_shader_dir);

REXCVAR_DEFINE_INT32(shadow_shader_workers, 0, "Experiments",
                     "Parallel XenosRecomp processes converting shaders for the Vulkan renderer "
                     "(0: half the CPU threads, at most 6)");

namespace d3::shadow::shaders {
namespace {

namespace fs = std::filesystem;

constexpr size_t kBatchSize = 32;  // shaders per XenosRecomp process
constexpr int kMaxAttempts = 2;    // a shader that kills the converter twice is given up

struct Pending {
  bool vertex;
  uint64_t hash;
  std::vector<uint8_t> container;
};

std::mutex mutex;
std::condition_variable wake;
bool started = false;
bool stopping = false;
fs::path xenos_dir, spirv_dir, work_dir, tool, header;
std::vector<Pending> buffered;           // created before Start()
std::deque<fs::path> queue;              // containers waiting for conversion
std::set<fs::path> queued;               // in the queue or being converted
std::unordered_map<std::string, int> attempts;
size_t in_flight = 0;
size_t converted = 0, failed = 0;
bool scanned = false;
std::chrono::steady_clock::time_point last_new_shader{};
std::atomic<uint32_t> generation{0};
std::vector<std::thread> workers;

std::string Name(bool vertex, uint64_t hash) {
  char name[32];
  std::snprintf(name, sizeof(name), "%s_%016llx", vertex ? "vs" : "ps",
                static_cast<unsigned long long>(hash));
  return name;
}

bool HasSpirv(const fs::path &container) {
  std::error_code ec;
  return fs::exists(spirv_dir / (container.stem().string() + ".spv"), ec);
}

// Caller holds the mutex.
void Enqueue(const fs::path &container) {
  if (queued.count(container) || HasSpirv(container))
    return;
  if (attempts[container.stem().string()] >= kMaxAttempts)
    return;
  queued.insert(container);
  queue.push_back(container);
  wake.notify_one();
}

// Caller holds the mutex.
void Save(const Pending &shader, bool convert) {
  const fs::path path = xenos_dir / (Name(shader.vertex, shader.hash) + ".bin");
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    const fs::path tmp = path.string() + ".tmp";
    if (FILE *file = std::fopen(tmp.string().c_str(), "wb")) {
      std::fwrite(shader.container.data(), 1, shader.container.size(), file);
      std::fclose(file);
      fs::rename(tmp, path, ec);
    }
  }
  last_new_shader = std::chrono::steady_clock::now();
  if (convert)
    Enqueue(path);
}

// A fingerprint of the converter build: its size and time, and the header's. A different
// converter means different (hopefully fixed) output, so the SPIR-V is rebuilt.
std::string Fingerprint() {
  std::string text;
  for (const fs::path &p : {tool, header}) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
    text += p.filename().string() + " " + std::to_string(size) + " " + std::to_string(time) + "\n";
  }
  return text;
}

void CheckStamp() {
  const fs::path stamp = spirv_dir / "converter.txt";
  const std::string want = Fingerprint();
  std::string have;
  if (std::ifstream in(stamp, std::ios::binary); in)
    have.assign(std::istreambuf_iterator<char>(in), {});
  if (have == want)
    return;
  size_t removed = 0;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(spirv_dir, ec))
    if (entry.path().extension() == ".spv" && fs::remove(entry.path(), ec))
      ++removed;
  if (removed)
    REXLOG_INFO("shadow_shaders: converter changed, discarded {} converted shaders", removed);
  std::ofstream(stamp, std::ios::binary) << want;
}

// Runs XenosRecomp --spirv over one list file; returns when the process exits.
bool RunConverter(const fs::path &list) {
#ifdef _WIN32
  std::wstring command = L"\"" + tool.wstring() + L"\" --spirv \"" + list.wstring() + L"\" \"" +
                         spirv_dir.wstring() + L"\" \"" + header.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(tool.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                      tool.parent_path().wstring().c_str(), &startup, &process)) {
    REXLOG_ERROR("shadow_shaders: cannot start {} (error {})", tool.string(), GetLastError());
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  // posix_spawn rather than fork: the game's threads keep running. The converter finds
  // libdxcompiler.so beside itself through its $ORIGIN run path. `nice` keeps it below the
  // game, like BELOW_NORMAL_PRIORITY_CLASS above.
  const std::string tool_path = tool.string(), list_path = list.string(),
                    out_path = spirv_dir.string(), header_path = header.string();
  const char *argv[] = {"nice",          "-n",              "5",
                        tool_path.c_str(), "--spirv",        list_path.c_str(),
                        out_path.c_str(), header_path.c_str(), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // Its per-shader progress lines would flood the game's terminal.
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  int error = posix_spawnp(&pid, "nice", &actions, nullptr, const_cast<char **>(argv), environ);
  if (error)  // no `nice` on this system: run the converter directly
    error = posix_spawn(&pid, tool_path.c_str(), &actions, nullptr,
                        const_cast<char **>(argv + 3), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error) {
    REXLOG_ERROR("shadow_shaders: cannot start {} ({})", tool_path, std::strerror(error));
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  // 126/127: the converter could not be executed at all (e.g. its library is missing).
  if (WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127)) {
    REXLOG_ERROR("shadow_shaders: {} did not run (exit code {})", tool_path,
                 WEXITSTATUS(status));
    return false;
  }
  return true;
#endif
}

void Worker(int index) {
  const fs::path list = work_dir / ("list" + std::to_string(index) + ".txt");
  while (true) {
    std::vector<fs::path> batch;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [] { return stopping || !queue.empty(); });
      if (stopping)
        return;
      while (!queue.empty() && batch.size() < kBatchSize) {
        batch.push_back(queue.front());
        queue.pop_front();
      }
      in_flight += batch.size();
    }
    {
      std::ofstream out(list, std::ios::binary | std::ios::trunc);
      for (const fs::path &p : batch)
        out << p.string() << "\n";
    }
    const bool ran = RunConverter(list);
    size_t ok = 0;
    std::vector<fs::path> retry;
    {
      std::lock_guard lock(mutex);
      for (const fs::path &p : batch) {
        queued.erase(p);
        if (HasSpirv(p)) {
          ++ok;
          continue;
        }
        // A crash ends the process early: the shaders after the culprit get another go.
        const int tries = ++attempts[p.stem().string()];
        if (ran && tries < kMaxAttempts) {
          retry.push_back(p);
        } else {
          ++failed;
          REXLOG_WARN("shadow_shaders: could not convert {}", p.filename().string());
        }
      }
      converted += ok;
      in_flight -= batch.size();
      for (const fs::path &p : retry)
        Enqueue(p);
      if (queue.empty() && in_flight == 0)
        REXLOG_INFO("shadow_shaders: up to date ({} converted this run, {} failed)", converted,
                    failed);
    }
    generation.fetch_add(1, std::memory_order_release);
    if (!ran) {
      std::lock_guard lock(mutex);
      stopping = true;  // no converter: nothing more will work this run
      wake.notify_all();
      return;
    }
  }
}

}  // namespace

void Start(const fs::path &root) {
  std::lock_guard lock(mutex);
  if (started)
    return;
  started = true;
  xenos_dir = root / "xenos";
  spirv_dir = root / "spirv";
  work_dir = root / "work";
  const fs::path tools = rex::filesystem::GetExecutableFolder() / "xenosrecomp";
#ifdef _WIN32
  tool = tools / "XenosRecomp.exe";
#else
  tool = tools / "XenosRecomp";
#endif
  header = tools / "shader_common.h";
  std::error_code ec;
  for (const fs::path &dir : {xenos_dir, spirv_dir, work_dir})
    fs::create_directories(dir, ec);

  const bool manual = !REXCVAR_GET(shadow_shader_dir).empty();
  const bool have_tool = fs::exists(tool, ec) && fs::exists(header, ec);
  if (manual) {
    REXLOG_INFO("shadow_shaders: using the shaders in {}", REXCVAR_GET(shadow_shader_dir));
  } else if (!have_tool) {
    REXLOG_WARN("shadow_shaders: no converter at {}; shaders are only captured (to {})",
                tool.string(), xenos_dir.string());
  } else {
    CheckStamp();
  }
  const bool convert = !manual && have_tool;
  for (const Pending &shader : buffered)
    Save(shader, convert);
  buffered.clear();
  if (convert) {
    // Containers from earlier runs that never got converted.
    for (const auto &entry : fs::directory_iterator(xenos_dir, ec))
      if (entry.path().extension() == ".bin")
        Enqueue(entry.path());
    int count = REXCVAR_GET(shadow_shader_workers);
    if (count <= 0)
      count = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 6);
    for (int i = 0; i < count; ++i)
      workers.emplace_back(Worker, i);
    REXLOG_INFO("shadow_shaders: {} shaders to convert, {} workers, output {}", queue.size(),
                count, spirv_dir.string());
  }
  scanned = true;
}

void Stop() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  wake.notify_all();
  for (std::thread &t : workers)
    if (t.joinable())
      t.join();
  workers.clear();
}

void OnShaderCreated(bool vertex, uint64_t header_hash, std::vector<uint8_t> container) {
  std::lock_guard lock(mutex);
  Pending shader{vertex, header_hash, std::move(container)};
  if (!started) {
    buffered.push_back(std::move(shader));
    return;
  }
  // Without a converter (or with a hand-made folder) shaders are still saved, not queued.
  Save(shader, !stopping && !workers.empty());
}

fs::path SpirvDir() {
  const std::string &manual = REXCVAR_GET(shadow_shader_dir);
  if (!manual.empty())
    return manual;
  std::lock_guard lock(mutex);
  return spirv_dir;
}

uint32_t Generation() { return generation.load(std::memory_order_acquire); }

void Progress(uint32_t *done, uint32_t *remaining) {
  std::lock_guard lock(mutex);
  *done = uint32_t(converted);
  *remaining = uint32_t(queue.size() + in_flight);
}

bool Ready() {
  if (!REXCVAR_GET(shadow_shader_dir).empty())
    return true;
  std::lock_guard lock(mutex);
  if (!scanned || workers.empty() || stopping)
    return false;
  // Shaders arrive in a burst while the game loads; wait until it has been quiet briefly.
  const bool quiet = std::chrono::steady_clock::now() - last_new_shader > std::chrono::seconds(2);
  return queue.empty() && in_flight == 0 && quiet && last_new_shader.time_since_epoch().count();
}

}  // namespace d3::shadow::shaders
