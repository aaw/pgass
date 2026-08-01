#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "encode.h"
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
ABSL_FLAG(std::string, encode, "",
          "Print the encoding of the grounded program and exit, in the given "
          "format. Only 'smtlib' is supported");
ABSL_FLAG(uint64_t, max_ground_atoms, 10000000,
          "Give up grounding after deriving this many ground atoms; 0 means "
          "no limit");
ABSL_FLAG(
    std::string, optimizer, "linear",
    "How to minimize weak constraints: 'linear' asks for a cheaper answer "
    "set until there is none, 'bisect' halves the range the cheapest cost "
    "lies in. Both find the same cost");

namespace {

std::optional<SolveOptions::Optimizer> parse_optimizer(std::string_view name) {
  if (name == "linear") return SolveOptions::Optimizer::kLinear;
  if (name == "bisect") return SolveOptions::Optimizer::kBisect;
  return std::nullopt;
}

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

// Prints what a query holds under: the atoms naming its substitutions,
// separated by spaces. Grounding names every atom of a user-visible predicate,
// so a query atom's name is the Output whose condition is that atom on its own.
//
// A program with no answer set holds every query under every substitution, and
// there are too many of those to print, so it gets a sentence instead.
void print_substitutions(const aspif::Program& prog, const QueryResult& answer) {
  if (answer.no_answer_set) {
    std::cout << "the program has no answer set. every substitution answers "
                 "the query\n";
    return;
  }

  const absl::flat_hash_set<aspif::Lit> answers(answer.holds.begin(),
                                                answer.holds.end());
  bool first = true;
  for (const aspif::Output& output : prog.outputs) {
    if (output.condition.size() != 1) continue;
    if (!answers.contains(output.condition.front())) continue;
    if (!first) std::cout << ' ';
    std::cout << output.name;
    first = false;
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Parse and ground an ASP program.\n"
      "Usage: pgass [--format] [source_file ...]\n"
      "  Files are concatenated; reads from stdin if none are given.");
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);

  const std::string encode = absl::GetFlag(FLAGS_encode);
  if (encode == "sat") {
    std::cerr << "pgass: --encode=sat is not implemented yet\n";
    return 1;
  }
  if (!encode.empty() && encode != "smtlib") {
    std::cerr << "pgass: unknown --encode '" << encode
              << "'; expected 'smtlib'\n";
    return 1;
  }

  // Each of these three prints something and stops, so at most one can be
  // asked for.
  const int printers = absl::GetFlag(FLAGS_format) +
                       absl::GetFlag(FLAGS_ground) + !encode.empty();
  if (printers > 1) {
    std::cerr << "pgass: --format, --ground, and --encode cannot be combined\n";
    return 1;
  }

  const std::optional<SolveOptions::Optimizer> optimizer =
      parse_optimizer(absl::GetFlag(FLAGS_optimizer));
  if (!optimizer.has_value()) {
    std::cerr << "pgass: unknown --optimizer '"
              << absl::GetFlag(FLAGS_optimizer)
              << "'; expected 'linear' or 'bisect'\n";
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

  std::vector<std::string> warnings;
  auto grounded = ground(**program, &warnings,
                         absl::GetFlag(FLAGS_max_ground_atoms));
  if (!grounded.ok()) {
    std::cerr << "pgass: grounding error: " << grounded.status().message()
              << "\n";
    return 1;
  }
  for (const std::string& warning : warnings) {
    std::cerr << "pgass: warning: " << warning << "\n";
  }
  if (absl::GetFlag(FLAGS_ground)) {
    std::cout << to_aspif(*grounded);
    return 0;
  }

  if (!encode.empty()) {
    auto script = encode_smtlib(*grounded);
    if (!script.ok()) {
      std::cerr << "pgass: encode error: " << script.status().message() << "\n";
      return 1;
    }
    std::cout << *script;
    return 0;
  }

  SolveOptions options;
  options.max_answer_sets = absl::GetFlag(FLAGS_models);
  options.optimizer = *optimizer;

  // A query asks a yes or no question about all the answer sets at once, so a
  // program with one gets an answer instead of a list of answer sets. A yes is
  // followed by the substitutions it holds under.
  if (grounded->query.has_value()) {
    auto answer = answer_query(*grounded, options);
    if (!answer.ok()) {
      std::cerr << "pgass: solve error: " << answer.status().message() << "\n";
      return 1;
    }
    if (answer->answer == QueryAnswer::kNo) {
      std::cout << "no\n";
      return 0;
    }
    std::cout << "yes\n";
    print_substitutions(*grounded, *answer);
    return 0;
  }

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
