#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "aspif.h"
#include "ground.h"
#include "normalize.h"
#include "parse.h"
#include "safety.h"
#include "solve.h"

namespace nb = nanobind;

namespace {

// One exception type per pipeline stage, so the Python layer can tell a parse
// error from a grounding error without inspecting a message string. Each is
// translated into the matching registered Python exception below.
struct ParseException : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct SafetyException : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct GroundingException : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct GroundingResourceExhaustedException : GroundingException {
  using GroundingException::GroundingException;
};
struct SolveException : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// register_exception_translator only accepts a capture-less function, so the
// five registered exception types live here instead of as locals closed over.
PyObject* g_parse_error = nullptr;
PyObject* g_safety_error = nullptr;
PyObject* g_grounding_error = nullptr;
PyObject* g_grounding_resource_exhausted = nullptr;
PyObject* g_solve_error = nullptr;

void translate_exception(const std::exception_ptr& p, void*) {
  try {
    if (p) std::rethrow_exception(p);
  } catch (const GroundingResourceExhaustedException& e) {
    PyErr_SetString(g_grounding_resource_exhausted, e.what());
  } catch (const GroundingException& e) {
    PyErr_SetString(g_grounding_error, e.what());
  } catch (const ParseException& e) {
    PyErr_SetString(g_parse_error, e.what());
  } catch (const SafetyException& e) {
    PyErr_SetString(g_safety_error, e.what());
  } catch (const SolveException& e) {
    PyErr_SetString(g_solve_error, e.what());
  }
}

// The names an answer set's atoms print as, the same match print_answer_set
// in pgass.cc makes against a program's Output statements.
std::vector<std::string> output_names(const aspif::Program& prog,
                                       const AnswerSet& answer_set) {
  absl::flat_hash_set<aspif::Atom> is_true(answer_set.atoms.begin(),
                                            answer_set.atoms.end());
  std::vector<std::string> names;
  for (const aspif::Output& output : prog.outputs) {
    bool holds = true;
    for (aspif::Lit lit : output.condition) {
      if (is_true.contains(std::abs(lit)) != (lit > 0)) {
        holds = false;
        break;
      }
    }
    if (holds) names.push_back(output.name);
  }
  return names;
}

std::vector<std::string> cost_strings(const AnswerSet& answer_set) {
  std::vector<std::string> costs;
  costs.reserve(answer_set.costs.size());
  for (const BigInt& cost : answer_set.costs) costs.push_back(cost.to_string());
  return costs;
}

// The names a query's substitutions print as, the same match print_substitutions
// in pgass.cc makes: a query atom's name is the Output whose condition is that
// atom on its own.
std::vector<std::string> query_holds_names(const aspif::Program& prog,
                                            const QueryResult& result) {
  const absl::flat_hash_set<aspif::Lit> answers(result.holds.begin(),
                                                 result.holds.end());
  std::vector<std::string> names;
  for (const aspif::Output& output : prog.outputs) {
    if (output.condition.size() != 1) continue;
    if (answers.contains(output.condition.front())) names.push_back(output.name);
  }
  return names;
}

// Runs the same parse -> safety -> normalize -> ground -> simplify pipeline
// src/pgass.cc runs for source text, throwing the exception naming the stage
// that failed instead of returning absl::Status.
aspif::Program compile_program(const std::string& text,
                                std::uint64_t max_ground_atoms) {
  Parser parser(text);
  auto program = parser.parse_program();
  if (!program.ok()) {
    throw ParseException(std::string(program.status().message()));
  }

  auto safety = verify_safe(**program);
  if (!safety.ok()) throw SafetyException(std::string(safety.message()));

  auto normal = normalize(**program);
  if (!normal.ok()) throw SafetyException(std::string(normal.message()));

  auto grounded = ground(**program, /*warnings=*/nullptr, max_ground_atoms);
  if (!grounded.ok()) {
    if (absl::IsResourceExhausted(grounded.status())) {
      throw GroundingResourceExhaustedException(
          std::string(grounded.status().message()));
    }
    throw GroundingException(std::string(grounded.status().message()));
  }
  aspif::simplify(*grounded);
  return std::move(*grounded);
}

}  // namespace

// One answer set at a time, wrapping AnswerSetIterator. Holds the same
// aspif::Program the iterator's Search points into, so the program the search
// was built from stays alive for as long as the iterator does.
class NativeAnswerSetIterator {
 public:
  NativeAnswerSetIterator(std::shared_ptr<aspif::Program> prog,
                           AnswerSetIterator iter)
      : prog_(std::move(prog)), iter_(std::move(iter)) {}

  std::optional<std::pair<std::vector<std::string>, std::vector<std::string>>>
  next() {
    auto result = iter_.next();
    if (!result.ok()) throw SolveException(std::string(result.status().message()));
    if (!result->has_value()) return std::nullopt;
    return std::make_pair(output_names(*prog_, **result), cost_strings(**result));
  }

 private:
  std::shared_ptr<aspif::Program> prog_;
  AnswerSetIterator iter_;
};

// A parsed, grounded program, ready to be solved or queried any number of
// times. Compiling is the expensive, fallible step. iterator() and query()
// are not.
class NativeProgram {
 public:
  static NativeProgram compile(const std::string& text,
                                std::uint64_t max_ground_atoms) {
    return NativeProgram(
        std::make_shared<aspif::Program>(compile_program(text, max_ground_atoms)));
  }

  NativeAnswerSetIterator iterator() {
    auto it = AnswerSetIterator::start(*prog_);
    if (!it.ok()) throw SolveException(std::string(it.status().message()));
    return NativeAnswerSetIterator(prog_, std::move(*it));
  }

  std::tuple<bool, std::vector<std::string>, bool> query() {
    auto result = answer_query(*prog_);
    if (!result.ok()) throw SolveException(std::string(result.status().message()));
    return {result->answer == QueryAnswer::kYes,
            query_holds_names(*prog_, *result), result->no_answer_set};
  }

 private:
  explicit NativeProgram(std::shared_ptr<aspif::Program> prog)
      : prog_(std::move(prog)) {}

  std::shared_ptr<aspif::Program> prog_;
};

NB_MODULE(_native, m) {
  m.doc() = "pgass's compiled extension: parses, grounds and solves the "
            "ASP-Core-2 text pgass.compile.to_text() produces.";

  // PgassError has no C++ exception of its own. It exists only as the common
  // Python base the stage-specific exceptions below register under, so
  // 'except pgass.PgassError' catches any of them.
  nb::object pgass_error = nb::steal(
      PyErr_NewException("pgass._native.PgassError", PyExc_Exception, nullptr));
  m.attr("PgassError") = pgass_error;

  nb::exception<ParseException> parse_error(m, "ParseError", pgass_error.ptr());
  nb::exception<SafetyException> safety_error(m, "SafetyError", pgass_error.ptr());
  nb::exception<GroundingException> grounding_error(m, "GroundingError",
                                                      pgass_error.ptr());
  nb::exception<GroundingResourceExhaustedException> grounding_resource_exhausted(
      m, "GroundingResourceExhausted", grounding_error.ptr());
  nb::exception<SolveException> solve_error(m, "SolveError", pgass_error.ptr());

  g_parse_error = parse_error.ptr();
  g_safety_error = safety_error.ptr();
  g_grounding_error = grounding_error.ptr();
  g_grounding_resource_exhausted = grounding_resource_exhausted.ptr();
  g_solve_error = solve_error.ptr();
  nb::register_exception_translator(&translate_exception);

  nb::class_<NativeAnswerSetIterator>(m, "NativeAnswerSetIterator")
      .def("next", &NativeAnswerSetIterator::next);

  nb::class_<NativeProgram>(m, "NativeProgram")
      .def_static("compile", &NativeProgram::compile, nb::arg("text"),
                  nb::arg("max_ground_atoms") = 0)
      .def("iterator", &NativeProgram::iterator)
      .def("query", &NativeProgram::query);
}
