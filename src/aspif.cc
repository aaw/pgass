#include "aspif.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "macros.h"

namespace aspif {

namespace {

void append_rule(const Rule& rule, std::string& out) {
  absl::StrAppend(&out, "1 ", static_cast<int>(rule.head_type), " ",
                  rule.head.size());
  for (Atom atom : rule.head) absl::StrAppend(&out, " ", atom);

  absl::StrAppend(&out, " ", static_cast<int>(rule.body_type));
  if (rule.body_type == Rule::BodyType::kNormal) {
    absl::StrAppend(&out, " ", rule.body.size());
    for (Lit lit : rule.body) absl::StrAppend(&out, " ", lit);
  } else {
    absl::StrAppend(&out, " ", rule.lower_bound, " ",
                    rule.weighted_body.size());
    for (const WeightedLit& wl : rule.weighted_body) {
      absl::StrAppend(&out, " ", wl.lit, " ", wl.weight);
    }
  }
  absl::StrAppend(&out, "\n");
}

}  // namespace

std::string to_aspif(const Program& prog) {
  std::string out = "asp 1 0 0\n";
  for (const Rule& rule : prog.rules) append_rule(rule, out);
  for (const Minimize& minimize : prog.minimize) {
    absl::StrAppend(&out, "2 ", minimize.priority, " ", minimize.lits.size());
    for (const WeightedLit& wl : minimize.lits) {
      absl::StrAppend(&out, " ", wl.lit, " ", wl.weight);
    }
    absl::StrAppend(&out, "\n");
  }
  for (const Output& output : prog.outputs) {
    absl::StrAppend(&out, "4 ", output.name.size(), " ", output.name, " ",
                    output.condition.size());
    for (Lit lit : output.condition) absl::StrAppend(&out, " ", lit);
    absl::StrAppend(&out, "\n");
  }
  // Statement 6, assumptions. A query that matched one atom is that
  // assumption. A query that matched several is left out. Its atoms are
  // alternatives, and an assumption would ask for all of them at once.
  if (prog.query.has_value() && prog.query->size() == 1) {
    absl::StrAppend(&out, "6 1 ", prog.query->front(), "\n");
  }
  absl::StrAppend(&out, "0\n");
  return out;
}

namespace {

// Where reading a document has got to, and the check each kind of field
// carries with it: a count cannot be negative, 0 is not a literal, a head atom
// is positive.
//
// One statement is one line, and its fields are separated by single spaces, so
// a read stops at the end of the current line rather than running on into the
// next statement. The name in an output statement is the one field that can
// hold spaces, which is why it is length-prefixed and why bytes() exists.
class Reader {
 public:
  explicit Reader(std::string_view text) : rest_(text) {}

  bool has_line() const { return !rest_.empty(); }

  // Moves to the next line, which the caller has checked is there.
  void next_line() {
    const size_t end = rest_.find('\n');
    line_ = rest_.substr(0, end);
    rest_ = end == std::string_view::npos ? std::string_view()
                                          : rest_.substr(end + 1);
    // A document written on Windows ends its lines with a carriage return.
    if (!line_.empty() && line_.back() == '\r') line_.remove_suffix(1);
    pos_ = 0;
    ++line_number_;
  }

  // What is left of the current line, spaces and all.
  std::string_view rest_of_line() const { return line_.substr(pos_); }

  absl::Status error(std::string_view message) const {
    return absl::InvalidArgumentError(
        absl::StrCat("line ", line_number_, ": ", message));
  }

  absl::StatusOr<std::string_view> field() {
    while (pos_ < line_.size() && line_[pos_] == ' ') ++pos_;
    if (pos_ == line_.size()) return error("a field is missing");
    const size_t start = pos_;
    while (pos_ < line_.size() && line_[pos_] != ' ') ++pos_;
    return line_.substr(start, pos_ - start);
  }

  absl::StatusOr<std::int32_t> integer() {
    ASSIGN_OR_RETURN(const std::string_view text, field());
    std::int32_t value = 0;
    if (!absl::SimpleAtoi(text, &value)) {
      return error(absl::StrCat("'", text, "' is not a 32 bit integer"));
    }
    return value;
  }

  absl::StatusOr<std::int32_t> count() {
    ASSIGN_OR_RETURN(const std::int32_t value, integer());
    if (value < 0) return error(absl::StrCat("a count cannot be ", value));
    return value;
  }

  absl::StatusOr<Atom> atom() {
    ASSIGN_OR_RETURN(const std::int32_t value, integer());
    if (value <= 0) return error(absl::StrCat(value, " is not an atom"));
    max_atom_ = std::max(max_atom_, value);
    return value;
  }

  absl::StatusOr<Lit> lit() {
    ASSIGN_OR_RETURN(const std::int32_t value, integer());
    if (value == 0) return error("0 is not a literal");
    max_atom_ = std::max(max_atom_, std::abs(value));
    return value;
  }

  absl::StatusOr<BigInt> big() {
    ASSIGN_OR_RETURN(const std::string_view text, field());
    const std::optional<BigInt> value = BigInt::from_decimal(text);
    if (!value.has_value()) {
      return error(absl::StrCat("'", text, "' is not a number"));
    }
    return *value;
  }

  // Takes the next `length` bytes, skipping the space in front of them.
  absl::StatusOr<std::string_view> bytes(std::int32_t length) {
    if (pos_ == line_.size() || line_[pos_] != ' ') {
      return error("a space is missing before a length-prefixed name");
    }
    ++pos_;
    if (line_.size() - pos_ < static_cast<size_t>(length)) {
      return error(
          absl::StrCat("the line holds fewer than ", length, " bytes of name"));
    }
    const size_t start = pos_;
    pos_ += length;
    return line_.substr(start, length);
  }

  absl::Status end_of_line() {
    while (pos_ < line_.size() && line_[pos_] == ' ') ++pos_;
    if (pos_ != line_.size()) {
      return error(absl::StrCat("'", line_.substr(pos_),
                                "' is left over at the end of the statement"));
    }
    return absl::OkStatus();
  }

  Atom max_atom() const { return max_atom_; }

 private:
  std::string_view rest_;
  std::string_view line_;
  size_t pos_ = 0;
  int line_number_ = 0;
  Atom max_atom_ = 0;
};

// Each of these three reads a count and then that many items. The last serves
// both a weight body and a minimize statement, which spell their literals
// alike.
absl::Status read_atoms(Reader& reader, std::vector<Atom>& out) {
  ASSIGN_OR_RETURN(const std::int32_t n, reader.count());
  for (std::int32_t i = 0; i < n; ++i) {
    ASSIGN_OR_RETURN(const Atom atom, reader.atom());
    out.push_back(atom);
  }
  return absl::OkStatus();
}

absl::Status read_lits(Reader& reader, std::vector<Lit>& out) {
  ASSIGN_OR_RETURN(const std::int32_t n, reader.count());
  for (std::int32_t i = 0; i < n; ++i) {
    ASSIGN_OR_RETURN(const Lit lit, reader.lit());
    out.push_back(lit);
  }
  return absl::OkStatus();
}

absl::Status read_weighted_lits(Reader& reader, std::vector<WeightedLit>& out) {
  ASSIGN_OR_RETURN(const std::int32_t n, reader.count());
  for (std::int32_t i = 0; i < n; ++i) {
    ASSIGN_OR_RETURN(const Lit lit, reader.lit());
    ASSIGN_OR_RETURN(const BigInt weight, reader.big());
    out.push_back(WeightedLit{.lit = lit, .weight = weight});
  }
  return absl::OkStatus();
}

absl::Status read_rule(Reader& reader, Program& prog) {
  Rule rule;

  ASSIGN_OR_RETURN(const std::int32_t head_type, reader.integer());
  if (head_type != 0 && head_type != 1) {
    return reader.error(
        absl::StrCat(head_type, " is not a head type; expected 0 or 1"));
  }
  rule.head_type = static_cast<Rule::HeadType>(head_type);
  RETURN_IF_ERROR(read_atoms(reader, rule.head));

  ASSIGN_OR_RETURN(const std::int32_t body_type, reader.integer());
  if (body_type == 0) {
    rule.body_type = Rule::BodyType::kNormal;
    RETURN_IF_ERROR(read_lits(reader, rule.body));
  } else if (body_type == 1) {
    rule.body_type = Rule::BodyType::kWeight;
    ASSIGN_OR_RETURN(rule.lower_bound, reader.big());
    RETURN_IF_ERROR(read_weighted_lits(reader, rule.weighted_body));
  } else {
    return reader.error(
        absl::StrCat(body_type, " is not a body type; expected 0 or 1"));
  }

  prog.rules.push_back(std::move(rule));
  return absl::OkStatus();
}

absl::Status read_minimize(Reader& reader, Program& prog) {
  Minimize minimize;
  ASSIGN_OR_RETURN(minimize.priority, reader.big());
  RETURN_IF_ERROR(read_weighted_lits(reader, minimize.lits));
  prog.minimize.push_back(std::move(minimize));
  return absl::OkStatus();
}

absl::Status read_output(Reader& reader, Program& prog) {
  ASSIGN_OR_RETURN(const std::int32_t length, reader.count());
  ASSIGN_OR_RETURN(const std::string_view name, reader.bytes(length));

  Output output{.name = std::string(name)};
  RETURN_IF_ERROR(read_lits(reader, output.condition));
  prog.outputs.push_back(std::move(output));
  return absl::OkStatus();
}

absl::Status read_assumption(Reader& reader, Program& prog) {
  ASSIGN_OR_RETURN(const std::int32_t size, reader.count());
  if (size != 1) {
    return reader.error(absl::StrCat(
        "an assumption of ", size,
        " literals asks for all of them at once, which no query means"));
  }
  if (prog.query.has_value()) {
    return reader.error("the document holds a second assumption");
  }
  ASSIGN_OR_RETURN(const Lit lit, reader.lit());
  prog.query = {lit};
  return absl::OkStatus();
}

// Reads the body of a statement whose type field has already been read.
absl::Status read_statement(Reader& reader, std::int32_t type, Program& prog) {
  switch (type) {
    case 1:
      return read_rule(reader, prog);
    case 2:
      return read_minimize(reader, prog);
    case 4:
      return read_output(reader, prog);
    case 6:
      return read_assumption(reader, prog);
    case 3:
    case 5:
    case 7:
    case 8:
    case 9:
      return reader.error(absl::StrCat(
          "statement type ", type,
          " is not supported; pgass reads rules, minimize statements, output "
          "statements and assumptions"));
    default:
      return reader.error(
          absl::StrCat(type, " is not an aspif statement type"));
  }
}

}  // namespace

absl::StatusOr<Program> from_aspif(std::string_view text) {
  Reader reader(text);
  if (!reader.has_line()) {
    return absl::InvalidArgumentError("the document is empty");
  }

  reader.next_line();
  // Tags after the version are how an incremental document announces itself,
  // and pgass solves a whole program in one go, so the header has to be the
  // plain one to_aspif writes.
  if (reader.rest_of_line() != "asp 1 0 0") {
    return reader.error(absl::StrCat("'", reader.rest_of_line(),
                                     "' is not the aspif header 'asp 1 0 0'"));
  }

  Program prog;
  bool terminated = false;
  while (reader.has_line() && !terminated) {
    reader.next_line();
    // A blank line carries no statement.
    if (reader.rest_of_line().empty()) continue;

    ASSIGN_OR_RETURN(const std::int32_t type, reader.integer());
    if (type == 0) {
      terminated = true;
    } else if (type == 10) {
      // A comment says nothing about the program, and the rest of its line is
      // free text rather than fields, so nothing there is checked.
      continue;
    } else {
      RETURN_IF_ERROR(read_statement(reader, type, prog));
    }
    RETURN_IF_ERROR(reader.end_of_line());
  }

  if (!terminated) {
    return reader.error("the document has no terminating '0' line");
  }
  while (reader.has_line()) {
    reader.next_line();
    if (!reader.rest_of_line().empty()) {
      return reader.error("the document goes on past its terminating '0' line");
    }
  }

  prog.next_atom = reader.max_atom() + 1;
  return prog;
}

namespace {

// What the rules settle about each atom without any search.
struct Settled {
  // Whether some rule states the atom as a fact, so it holds in every answer
  // set.
  std::vector<bool> fact;
  // Whether any rule at all has the atom in its head. An atom with none holds
  // in no answer set.
  std::vector<bool> derivable;
};

// Whether the rule is the plainest statement of a fact, 'a.'. Such a rule is
// what makes its atom a fact, so simplifying it against that fact would delete
// the fact itself.
bool states_a_fact(const Rule& rule) {
  if (rule.head_type != Rule::HeadType::kDisjunction) return false;
  if (rule.head.size() != 1) return false;
  return rule.body_type == Rule::BodyType::kNormal ? rule.body.empty()
                                                   : rule.lower_bound <= 0;
}

Settled settle(const Program& prog) {
  Settled settled;
  settled.fact.assign(prog.next_atom, false);
  settled.derivable.assign(prog.next_atom, false);
  for (const Rule& rule : prog.rules) {
    for (Atom atom : rule.head) settled.derivable[atom] = true;
    if (states_a_fact(rule)) settled.fact[rule.head.front()] = true;
  }
  return settled;
}

// How a settled atom decides a body literal.
enum class LitFate { kKeep, kDrop, kKillsBody };

LitFate fate_of(Lit lit, const Settled& settled) {
  const Atom atom = std::abs(lit);
  const bool holds = settled.fact[atom];
  const bool impossible = !settled.derivable[atom];
  if (lit > 0) {
    if (holds) return LitFate::kDrop;
    if (impossible) return LitFate::kKillsBody;
  } else {
    if (holds) return LitFate::kKillsBody;
    if (impossible) return LitFate::kDrop;
  }
  return LitFate::kKeep;
}

// Rewrites one rule's body against `settled`. Returns false where the body can
// hold in no answer set, which leaves the rule to be dropped.
bool simplify_body(Rule& rule, const Settled& settled) {
  if (rule.body_type == Rule::BodyType::kNormal) {
    std::vector<Lit> kept;
    kept.reserve(rule.body.size());
    for (Lit lit : rule.body) {
      switch (fate_of(lit, settled)) {
        case LitFate::kKeep:
          kept.push_back(lit);
          break;
        case LitFate::kDrop:
          break;
        case LitFate::kKillsBody:
          return false;
      }
    }
    rule.body = std::move(kept);
    return true;
  }

  // A settled literal of a weight body is worth its weight either way. One that
  // holds pays into the bound, and one that cannot hold pays nothing, so both
  // leave the body counting over fewer literals.
  BigInt bound = rule.lower_bound;
  BigInt reachable = 0;
  std::vector<WeightedLit> kept;
  kept.reserve(rule.weighted_body.size());
  for (WeightedLit& weighted : rule.weighted_body) {
    switch (fate_of(weighted.lit, settled)) {
      case LitFate::kKeep:
        reachable += weighted.weight;
        kept.push_back(std::move(weighted));
        break;
      case LitFate::kDrop:
        bound -= weighted.weight;
        break;
      case LitFate::kKillsBody:
        break;
    }
  }
  // A bound the remaining literals cannot reach however they fall.
  if (reachable < bound) return false;
  // A bound already paid for is a body that always holds, which is a normal
  // body of no literals.
  if (bound <= 0) {
    rule.body_type = Rule::BodyType::kNormal;
    rule.weighted_body.clear();
    rule.lower_bound = 0;
    return true;
  }
  rule.lower_bound = std::move(bound);
  rule.weighted_body = std::move(kept);
  return true;
}

// Rewrites one rule's head against `settled`. Returns false where the rule has
// nothing left to say.
bool simplify_head(Rule& rule, const Settled& settled) {
  if (rule.head_type == Rule::HeadType::kChoice) {
    // A choice over an atom that already holds chooses nothing, so the atom
    // goes and the rest of the choice stands. An empty choice says nothing.
    std::erase_if(rule.head,
                  [&settled](Atom atom) { return settled.fact[atom]; });
    return !rule.head.empty();
  }
  // A disjunction holding a fact is satisfied whatever its body does. It cannot
  // support its other head atoms either: supporting one asks for every other to
  // be false, and a fact never is.
  for (Atom atom : rule.head) {
    if (settled.fact[atom]) return false;
  }
  return true;
}

// The size of a program. A pass that leaves it alone has nothing left to
// settle, since every decision rests on the facts and the underivable atoms,
// and those follow from the rules that are left.
size_t rule_and_literal_count(const Program& prog) {
  size_t count = prog.rules.size();
  for (const Rule& rule : prog.rules) {
    count += rule.head.size() + rule.body.size() + rule.weighted_body.size();
  }
  return count;
}

}  // namespace

void simplify(Program& prog) {
  while (true) {
    const size_t before = rule_and_literal_count(prog);
    const Settled settled = settle(prog);

    std::vector<Rule> kept;
    kept.reserve(prog.rules.size());
    for (Rule& rule : prog.rules) {
      if (states_a_fact(rule)) {
        kept.push_back(std::move(rule));
        continue;
      }
      if (!simplify_head(rule, settled)) continue;
      if (!simplify_body(rule, settled)) continue;
      kept.push_back(std::move(rule));
    }
    prog.rules = std::move(kept);

    if (rule_and_literal_count(prog) == before) return;
  }
}

}  // namespace aspif
