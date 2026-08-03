#include "exit_code.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

// The only test that runs the pgass binary rather than calling into a library.
// Exit codes are what a caller sees, and nothing below main() can report them,
// so checking them means starting a process. PGASS_BINARY is the built
// executable, handed over by CMake.

namespace {

// Writes `source` to a file that lives as long as the object, so that a test
// can hand pgass a program without one of the examples happening to fit.
class TempProgram {
 public:
  explicit TempProgram(const std::string& source) {
    std::string name = "/tmp/pgass_exit_code_XXXXXX";
    const int fd = mkstemp(name.data());
    EXPECT_GE(fd, 0) << "could not make a temporary file";
    close(fd);
    path_ = name;
    std::ofstream out(path_);
    out << source;
  }
  ~TempProgram() { std::remove(path_.c_str()); }

  TempProgram(const TempProgram&) = delete;
  TempProgram& operator=(const TempProgram&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Runs pgass with `args` and returns the exit code it left behind. Output goes
// to /dev/null because these tests are about the status, and the tests that
// read pgass's output work on the libraries underneath it.
int run_pgass(const std::vector<std::string>& args) {
  const std::string command = absl::StrCat(PGASS_BINARY, " ",
                                           absl::StrJoin(args, " "),
                                           " >/dev/null 2>&1");
  const int status = std::system(command.c_str());
  // system() hands back a wait status rather than the exit code itself.
  if (!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

std::string example(const std::string& name) {
  return absl::StrCat(PGASS_SOURCE_DIR, "/examples/", name);
}

TEST(ExitCode, UnsatisfiableProgramReportsUnsatisfiable) {
  EXPECT_EQ(run_pgass({example("pigeonhole.lp")}), kUnsatisfiable);
}

TEST(ExitCode, FalseQueryReportsUnsatisfiable) {
  // 'a | b. a?' is false: b on its own is an answer set that leaves a out.
  TempProgram program("a | b. a?");
  EXPECT_EQ(run_pgass({program.path()}), kUnsatisfiable);
}

TEST(ExitCode, TrueQueryReportsSatisfiable) {
  TempProgram program("a. a?");
  EXPECT_EQ(run_pgass({program.path()}), kSatisfiable);
}

TEST(ExitCode, StoppingAtTheAnswerSetLimitReportsSatisfiable) {
  // Three choices give eight answer sets, so asking for one leaves the rest
  // unlooked for. That is short of proving there is no other.
  TempProgram program("{a; b; c}.");
  EXPECT_EQ(run_pgass({"--models=1", program.path()}), kSatisfiable);
}

TEST(ExitCode, FindingEveryAnswerSetReportsExhausted) {
  TempProgram program("{a; b; c}.");
  EXPECT_EQ(run_pgass({"--models=0", program.path()}), kExhausted);
}

TEST(ExitCode, ProvenOptimumReportsExhausted) {
  // meeting-time.lp minimizes at two priority levels, and the encoding admits
  // only optimal answer sets, so one answer set is already one optimum.
  EXPECT_EQ(run_pgass({"--models=1", example("meeting-time.lp")}), kExhausted);
}

TEST(ExitCode, EveryOptimumReportsAllOptima) {
  EXPECT_EQ(run_pgass({"--models=0", example("meeting-time.lp")}), kAllOptima);
}

TEST(ExitCode, PrintingRatherThanSolvingReportsGrounded) {
  EXPECT_EQ(run_pgass({"--format", example("knapsack.lp")}), kGrounded);
  EXPECT_EQ(run_pgass({"--ground", example("knapsack.lp")}), kGrounded);
  EXPECT_EQ(run_pgass({"--encode=smtlib", example("two-coloring.lp")}),
            kGrounded);
}

// Grounding and solving in two runs answers the way one run does.
TEST(ExitCode, SolvingGroundOutputAnswersAsSolvingTheSourceDoes) {
  const std::string pgass = PGASS_BINARY;
  const std::string program = example("knapsack.lp");
  const std::string piped = absl::StrCat(pgass, " --ground ", program, " | ",
                                         pgass, " --solve --models=0 ",
                                         ">/dev/null 2>&1");
  const int status = std::system(piped.c_str());
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), kExhausted);
  EXPECT_EQ(run_pgass({"--models=0", program}), kExhausted);
}

TEST(ExitCode, AspifThatCannotBeReadReportsNoRun) {
  TempProgram document("asp 1 0 0\n1 0 1 1 0\n0\n");
  EXPECT_EQ(run_pgass({"--solve", document.path()}), kNoRun);
}

TEST(ExitCode, ParseErrorReportsNoRun) {
  TempProgram program("a :-");
  EXPECT_EQ(run_pgass({program.path()}), kNoRun);
}

TEST(ExitCode, UnsafeProgramReportsNoRun) {
  // X is in the head without anything in the body to bind it.
  TempProgram program("p(X).");
  EXPECT_EQ(run_pgass({program.path()}), kNoRun);
}

TEST(ExitCode, BadCommandLineReportsNoRun) {
  TempProgram program("a.");
  EXPECT_EQ(run_pgass({"--encode=sat", program.path()}), kNoRun);
  EXPECT_EQ(run_pgass({"--format", "--ground", program.path()}), kNoRun);
  EXPECT_EQ(run_pgass({"--solve", "--ground", program.path()}), kNoRun);
  EXPECT_EQ(run_pgass({"--solve", "--format", program.path()}), kNoRun);
}

// --encode prints the encoding of a ground program, and --solve is handed one,
// so the two go together.
TEST(ExitCode, EncodingAspifReportsGrounded) {
  TempProgram document("asp 1 0 0\n1 0 1 1 0 0\n4 1 a 1 1\n0\n");
  EXPECT_EQ(run_pgass({"--solve", "--encode=smtlib", document.path()}),
            kGrounded);
}

TEST(ExitCode, MissingFileReportsNoRun) {
  EXPECT_EQ(run_pgass({"/nonexistent/program.lp"}), kNoRun);
}

TEST(ExitCode, RunningOutOfGroundAtomsReportsInterrupted) {
  // The cap is a resource limit, so hitting it answers the way a run stopped by
  // its time limit does rather than blaming the program.
  EXPECT_EQ(run_pgass({"--max_ground_atoms=1", example("knapsack.lp")}),
            kInterrupted);
}

}  // namespace
