#include "matching_engine/journal.hpp"
#include "matching_engine/replay.hpp"

#include <array>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace matching_engine {
namespace {

struct ProcessResult {
  int exit_code{};
  std::string output;
  std::string error;
};

std::string read_pipe(int descriptor) {
  std::string result;
  std::array<char, 256U> buffer{};
  for (;;) {
    const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
    if (count <= 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return result;
}

ProcessResult run_cli(const std::vector<std::string>& arguments) {
  int output_pipe[2]{};
  int error_pipe[2]{};
  EXPECT_EQ(::pipe(output_pipe), 0);
  EXPECT_EQ(::pipe(error_pipe), 0);
  const pid_t child = ::fork();
  EXPECT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(error_pipe[0]));
    static_cast<void>(::dup2(output_pipe[1], STDOUT_FILENO));
    static_cast<void>(::dup2(error_pipe[1], STDERR_FILENO));
    std::vector<char*> raw;
    raw.reserve(arguments.size() + 2U);
    raw.push_back(const_cast<char*>(MATCHING_ENGINE_REPLAY_PATH));
    for (const std::string& argument : arguments) {
      raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);
    ::execv(MATCHING_ENGINE_REPLAY_PATH, raw.data());
    ::_exit(127);
  }
  static_cast<void>(::close(output_pipe[1]));
  static_cast<void>(::close(error_pipe[1]));
  ProcessResult result;
  result.output = read_pipe(output_pipe[0]);
  result.error = read_pipe(error_pipe[0]);
  static_cast<void>(::close(output_pipe[0]));
  static_cast<void>(::close(error_pipe[0]));
  int status{};
  EXPECT_EQ(::waitpid(child, &status, 0), child);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
  return result;
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    path_ =
        std::filesystem::temp_directory_path() /
        ("matching-engine-replay-cli-" + std::to_string(static_cast<std::uint64_t>(::getpid())) +
         "-" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

TEST(ReplayCliTest, RejectsMissingAndUnknownArgumentsWithoutStdout) {
  const ProcessResult missing = run_cli({});
  EXPECT_NE(missing.exit_code, 0);
  EXPECT_TRUE(missing.output.empty());
  EXPECT_EQ(missing.error, "matching_engine_replay: missing --journal\n");

  const ProcessResult unknown = run_cli({"--wat"});
  EXPECT_NE(unknown.exit_code, 0);
  EXPECT_TRUE(unknown.output.empty());
  EXPECT_EQ(unknown.error, "matching_engine_replay: unknown option --wat\n");
}

TEST(ReplayCliTest, FullReplayEmitsStableJsonAndChecksExpectations) {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "commands.journal";
  auto journal = MmapJournal::create(path, 2U);
  ASSERT_TRUE(journal.has_value());
  const SequencedCommand command{
      CommandPayload::submit_limit(OrderId{7U}, Side::buy, Price{101}, Quantity{4U}), Sequence{1U},
      9U};
  ASSERT_EQ(journal->append(command), JournalError::none);
  ASSERT_EQ(journal->close(), JournalError::none);

  const std::vector<std::string> arguments{"--journal",    path.string(), "--min",          "100",
                                           "--max",        "104",         "--tick",         "1",
                                           "--max-orders", "2",           "--max-quantity", "10"};
  const ProcessResult result = run_cli(arguments);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.output, "{\"format_version\":1,\"commands_applied\":1,\"first_sequence\":1,"
                           "\"last_sequence\":1,\"event_count\":1,\"event_bytes\":64,"
                           "\"crc32c\":\"7190bd74\",\"live_orders\":1,\"best_bid\":101,"
                           "\"best_ask\":null,\"snapshot_sequence\":0}\n");

  auto mismatch_arguments = arguments;
  mismatch_arguments.insert(mismatch_arguments.end(), {"--expect-events", "2"});
  const ProcessResult mismatch = run_cli(mismatch_arguments);
  EXPECT_NE(mismatch.exit_code, 0);
  EXPECT_TRUE(mismatch.output.empty());
  EXPECT_EQ(mismatch.error, "matching_engine_replay: expected event count mismatch\n");
}

TEST(ReplayCliTest, HelpIsSuccessfulAndConfigDuplicatesAreRejected) {
  const ProcessResult help = run_cli({"--help"});
  EXPECT_EQ(help.exit_code, 0);
  EXPECT_FALSE(help.output.empty());
  EXPECT_TRUE(help.error.empty());

  const ProcessResult duplicate =
      run_cli({"--journal", "a", "--journal", "b", "--min", "0", "--max", "0", "--tick", "1",
               "--max-orders", "0", "--max-quantity", "1"});
  EXPECT_NE(duplicate.exit_code, 0);
  EXPECT_TRUE(duplicate.output.empty());
  EXPECT_EQ(duplicate.error, "matching_engine_replay: duplicate --journal\n");
}

} // namespace
} // namespace matching_engine
