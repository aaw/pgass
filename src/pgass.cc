#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "format.h"
#include "ground.h"
#include "normalize.h"
#include "parse.h"
#include "safety.h"
#include "solve.h"

ABSL_FLAG(bool, format, false, "Format the program and print to stdout");
ABSL_FLAG(bool, ground, false,
          "Ground the program, print it as aspif text, and exit");
ABSL_FLAG(int, models, 1,
          "How many answer sets to look for; 0 means all of them");

namespace {

// Prints one answer set the way clingo does: the names of the Output
// statements whose condition holds, separated by spaces.
void print_answer_set(const aspif::Program& prog, const AnswerSet& answer_set) {
  absl::flat_hash_set<aspif::Atom> is_true(answer_set.atoms.begin(),
                                           answer_set.atoms.end());
  bool first = true;
  for (const aspif::Output& output : prog.outputs) {
    bool holds = true;
    for (aspif::Lit lit : output.condition) {
      if (is_true.contains(std::abs(lit)) != (lit > 0)) {
        holds = false;
        break;
      }
    }
    if (!holds) continue;
    if (!first) std::cout << ' ';
    std::cout << output.name;
    first = false;
  }
  std::cout << "\n";
  if (!answer_set.costs.empty()) {
    std::cout << "Cost:";
    for (std::int64_t cost : answer_set.costs) std::cout << ' ' << cost;
    std::cout << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Parse and ground an ASP program.\n"
      "Usage: pgass [--format] [source_file ...]\n"
      "  Files are concatenated; reads from stdin if none are given.");
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);

  if (absl::GetFlag(FLAGS_format) && absl::GetFlag(FLAGS_ground)) {
    std::cerr << "pgass: --format and --ground cannot be combined\n";
    return 1;
  }

  std::string source;
  if (positional.size() <= 1) {
    source = std::string(std::istreambuf_iterator<char>(std::cin),
                         std::istreambuf_iterator<char>());
  } else {
    for (size_t i = 1; i < positional.size(); ++i) {
      std::ifstream f(positional[i]);
      if (!f) {
        std::cerr << "pgass: cannot open '" << positional[i] << "'\n";
        return 1;
      }
      if (!source.empty()) source += '\n';
      source =
          absl::StrCat(source, std::string(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>()));
    }
  }

  Parser parser(source);
  auto program = parser.parse_program();
  if (!program.ok()) {
    std::cerr << "pgass: parse error: " << program.status().message() << "\n";
    return 1;
  }

  if (absl::GetFlag(FLAGS_format)) {
    std::cout << format(**program);
    return 0;
  }

  auto safety = verify_safe(**program);
  if (!safety.ok()) {
    std::cerr << "pgass: safety error: " << safety.message() << "\n";
    return 1;
  }

  auto normal = normalize(**program);
  if (!normal.ok()) {
    std::cerr << "pgass: normalization error: " << normal.message() << "\n";
    return 1;
  }

  auto grounded = ground(**program);
  if (!grounded.ok()) {
    std::cerr << "pgass: grounding error: " << grounded.status().message()
              << "\n";
    return 1;
  }
  if (absl::GetFlag(FLAGS_ground)) {
    std::cout << to_aspif(*grounded);
    return 0;
  }

  SolveOptions options;
  options.max_answer_sets = absl::GetFlag(FLAGS_models);
  auto answer_sets = solve(*grounded, options);
  if (!answer_sets.ok()) {
    std::cerr << "pgass: solve error: " << answer_sets.status().message()
              << "\n";
    return 1;
  }
  if (answer_sets->empty()) {
    std::cout << "UNSATISFIABLE\n";
    return 0;
  }
  for (size_t i = 0; i < answer_sets->size(); ++i) {
    std::cout << "Answer: " << i + 1 << "\n";
    print_answer_set(*grounded, (*answer_sets)[i]);
  }
  std::cout << "SATISFIABLE\n";
  return 0;
}
