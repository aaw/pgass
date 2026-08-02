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
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"
#include "encode.h"
#include "exit_code.h"
#include "format.h"
#include "ground.h"
#include "normalize.h"
#include "parse.h"
#include "safety.h"
#include "solve.h"

ABSL_FLAG(bool, format, false, "Format the program and print to stdout");
ABSL_FLAG(bool, ground, false,
          "Ground the program, print it as aspif text, and exit");
ABSL_FLAG(bool, solve, false,
          "Read the input as aspif text rather than as an ASP program, so "
          "that a program ground elsewhere can be solved here");
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
    for (const BigInt& cost : answer_set.costs) std::cout << ' ' << cost;
    std::cout << "\n";
  }
}

// Prints what a query holds under: the atoms naming its substitutions,
// separated by spaces. Grounding names every atom of a user-visible predicate,
// so a query atom's name is the Output whose condition is that atom on its own.
//
// A program with no answer set holds every query under every substitution, and
// there are too many of those to print, so it gets a sentence instead.
void print_substitutions(const aspif::Program& prog,
                         const QueryResult& answer) {
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

// Says what there is to say about a ground program: its encoding under
// --encode, the answer to its query, or its answer sets. Returns the exit code
// that stands for what was found.
int report(const aspif::Program& prog, std::string_view encode,
           const SolveOptions& options) {
  if (!encode.empty()) {
    auto script = encode_smtlib(prog);
    if (!script.ok()) {
      std::cerr << "pgass: encode error: " << script.status().message() << "\n";
      return kNoRun;
    }
    std::cout << *script;
    return kGrounded;
  }

  // A query asks a yes or no question about all the answer sets at once, so a
  // program with one gets an answer instead of a list of answer sets. A yes is
  // followed by the substitutions it holds under.
  if (prog.query.has_value()) {
    auto answer = answer_query(prog, options);
    if (!answer.ok()) {
      std::cerr << "pgass: solve error: " << answer.status().message() << "\n";
      return kNoRun;
    }
    // A query is decided over every answer set at once, so its two outcomes are
    // the true and false the standard asks a query to report.
    if (answer->answer == QueryAnswer::kNo) {
      std::cout << "no\n";
      return kUnsatisfiable;
    }
    std::cout << "yes\n";
    print_substitutions(prog, *answer);
    return kSatisfiable;
  }

  auto solved = solve(prog, options);
  if (!solved.ok()) {
    std::cerr << "pgass: solve error: " << solved.status().message() << "\n";
    return kNoRun;
  }
  if (solved->answer_sets.empty()) {
    std::cout << "UNSATISFIABLE\n";
    return kUnsatisfiable;
  }
  for (size_t i = 0; i < solved->answer_sets.size(); ++i) {
    std::cout << "Answer: " << i + 1 << "\n";
    print_answer_set(prog, solved->answer_sets[i]);
  }
  std::cout << "SATISFIABLE\n";

  // Weak constraints are minimized before any answer set comes back, so every
  // one of these is optimal. Having found them all is what ALLOPT reports, and
  // having found some is already more than a plain satisfiable answer says.
  const bool optimizing = !prog.minimize.empty();
  if (optimizing) return solved->exhausted ? kAllOptima : kExhausted;
  return solved->exhausted ? kExhausted : kSatisfiable;
}

}  // namespace

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Parse, ground and solve an ASP program.\n"
      "Usage: pgass [--format] [--ground] [--solve] [source_file ...]\n"
      "  Files are concatenated; reads from stdin if none are given.\n"
      "  --solve reads aspif text instead, so 'pgass prog.lp' and\n"
      "  'pgass --ground prog.lp | pgass --solve' answer alike.");
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);

  const std::string encode = absl::GetFlag(FLAGS_encode);
  if (encode == "sat") {
    std::cerr << "pgass: --encode=sat is not implemented yet\n";
    return kNoRun;
  }
  if (!encode.empty() && encode != "smtlib") {
    std::cerr << "pgass: unknown --encode '" << encode
              << "'; expected 'smtlib'\n";
    return kNoRun;
  }

  // Each of these three prints something and stops, so at most one can be
  // asked for.
  const int printers = absl::GetFlag(FLAGS_format) +
                       absl::GetFlag(FLAGS_ground) + !encode.empty();
  if (printers > 1) {
    std::cerr << "pgass: --format, --ground, and --encode cannot be combined\n";
    return kNoRun;
  }

  // --solve is handed a program that is already ground, so the two flags that
  // work on program text have nothing left to do. --encode still does: it
  // prints the encoding of the ground program.
  const bool solve_aspif = absl::GetFlag(FLAGS_solve);
  if (solve_aspif &&
      (absl::GetFlag(FLAGS_format) || absl::GetFlag(FLAGS_ground))) {
    std::cerr << "pgass: --solve reads a program that is already ground, so "
                 "--format and --ground have nothing to do\n";
    return kNoRun;
  }

  const std::optional<SolveOptions::Optimizer> optimizer =
      parse_optimizer(absl::GetFlag(FLAGS_optimizer));
  if (!optimizer.has_value()) {
    std::cerr << "pgass: unknown --optimizer '"
              << absl::GetFlag(FLAGS_optimizer)
              << "'; expected 'linear' or 'bisect'\n";
    return kNoRun;
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
        return kNoRun;
      }
      if (!source.empty()) source += '\n';
      source =
          absl::StrCat(source, std::string(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>()));
    }
  }

  SolveOptions options;
  options.max_answer_sets = absl::GetFlag(FLAGS_models);
  options.optimizer = *optimizer;

  // --solve is handed the ground program itself, so it skips the parsing and
  // grounding below and goes straight to reporting.
  if (solve_aspif) {
    auto program = aspif::from_aspif(source);
    if (!program.ok()) {
      std::cerr << "pgass: aspif error: " << program.status().message() << "\n";
      return kNoRun;
    }
    // gringo writes choice rules as choice rules. Grounding here has already
    // rewritten them into the disjunctions solving takes.
    aspif::replace_choice_rules(*program);
    return report(*program, encode, options);
  }

  Parser parser(source);
  auto program = parser.parse_program();
  if (!program.ok()) {
    std::cerr << "pgass: parse error: " << program.status().message() << "\n";
    return kNoRun;
  }

  if (absl::GetFlag(FLAGS_format)) {
    std::cout << format(**program);
    return kGrounded;
  }

  auto safety = verify_safe(**program);
  if (!safety.ok()) {
    std::cerr << "pgass: safety error: " << safety.message() << "\n";
    return kNoRun;
  }

  auto normal = normalize(**program);
  if (!normal.ok()) {
    std::cerr << "pgass: normalization error: " << normal.message() << "\n";
    return kNoRun;
  }

  std::vector<std::string> warnings;
  auto grounded =
      ground(**program, &warnings, absl::GetFlag(FLAGS_max_ground_atoms));
  if (!grounded.ok()) {
    std::cerr << "pgass: grounding error: " << grounded.status().message()
              << "\n";
    // Running out of the ground atoms allowed is running out of a resource, not
    // a complaint about the program, so it answers the way a run cut short by
    // its time limit does.
    return absl::IsResourceExhausted(grounded.status()) ? kInterrupted : kNoRun;
  }
  for (const std::string& warning : warnings) {
    std::cerr << "pgass: warning: " << warning << "\n";
  }
  if (absl::GetFlag(FLAGS_ground)) {
    std::cout << to_aspif(*grounded);
    return kGrounded;
  }

  return report(*grounded, encode, options);
}
