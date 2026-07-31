#include "encode.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "aspif.h"
#include "graph.h"
#include "macros.h"

namespace {

// The SMT logic to solve `prog` in. cvc5 takes the logic once, before the first
// assertion, and offers no way to widen it later, so this scans the whole
// program up front instead of deciding while translating.
//
// Every level ranking constraint looks like 'lvl(a) - lvl(b) >= 1': two
// variables, no coefficients, a constant on the right. That is exactly what a
// difference logic atom can say, and QF_IDL lets cvc5 decide such constraints
// by looking for a negative cycle in a graph rather than running simplex.
//
// A weight body or a minimize statement adds up the weights of many literals at
// once, which no difference logic atom can say, and costs the whole program the
// more general QF_LIA. Every QF_IDL formula is also a QF_LIA formula, so
// answering QF_LIA is never wrong, only slower.
const char* logic_for(const aspif::Program& prog) {
  // A weighted sum is a weighted sum whether it comes from an aggregate or a
  // weak constraint.
  if (!prog.minimize.empty()) return "QF_LIA";
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.body_type == aspif::Rule::BodyType::kWeight) return "QF_LIA";
  }
  return "QF_IDL";
}

// The un-negated body atoms of `rule`, whichever body form it uses. These are
// the atoms the rule depends on positively, which is what both the dependency
// graph and the level ranking care about.
std::vector<aspif::Atom> positive_body_atoms(const aspif::Rule& rule) {
  std::vector<aspif::Atom> atoms;
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    for (aspif::Lit lit : rule.body) {
      if (lit > 0) atoms.push_back(lit);
    }
  } else {
    for (const aspif::WeightedLit& weighted : rule.weighted_body) {
      if (weighted.lit > 0) atoms.push_back(weighted.lit);
    }
  }
  return atoms;
}

// The positive dependency graph of the ground program: an edge from each
// un-negated body atom to the head atom of the rule it appears in. Atom ids
// index the rows, so row 0 is present but always empty, 0 being no atom.
std::vector<std::vector<int>> positive_dependency_graph(
    const aspif::Program& prog) {
  std::vector<std::vector<int>> succ(prog.next_atom);
  for (const aspif::Rule& rule : prog.rules) {
    const std::vector<aspif::Atom> body_atoms = positive_body_atoms(rule);
    for (aspif::Atom head : rule.head) {
      DCHECK_LT(head, prog.next_atom);
      for (aspif::Atom body_atom : body_atoms) {
        DCHECK_LT(body_atom, prog.next_atom);
        succ[body_atom].push_back(head);
      }
    }
  }
  return succ;
}

// What the level ranking needs to know about the positive dependency graph.
struct Ranking {
  // The strongly connected component of each atom, indexed by atom id. Two
  // atoms can lie on a common positive cycle exactly when their components are
  // equal.
  std::vector<int> component;
  // Whether each atom can lie on a positive cycle at all, and so needs a level
  // variable. False for every atom of a stratified program, which leaves the
  // translation as plain completion.
  std::vector<bool> needs_level;
  // Whether each component, indexed by component id, holds two head atoms of
  // one rule. Nothing in such a component is ranked. A ranking would throw away
  // answer sets like the {a, b} of 'a | b. a :- b. b :- a.', where a and b
  // support each other around the cycle and neither is above the other. The
  // minimality check decides those atoms instead.
  std::vector<bool> head_cyclic;
  // Whether any component is head-cyclic, which is what turns that check on.
  bool any_head_cycle = false;
};

Ranking build_ranking(const aspif::Program& prog) {
  const std::vector<std::vector<int>> succ = positive_dependency_graph(prog);

  Ranking ranking;
  ranking.component = strongly_connected_components(succ);
  ranking.needs_level.assign(prog.next_atom, false);
  ranking.head_cyclic.assign(ranking.component.size(), false);

  std::vector<int> component_size(ranking.component.size(), 0);
  for (int component : ranking.component) ++component_size[component];

  // Two head atoms of one rule in one component are a head cycle.
  for (const aspif::Rule& rule : prog.rules) {
    absl::flat_hash_set<int> seen;
    for (aspif::Atom head : rule.head) {
      if (seen.insert(ranking.component[head]).second) continue;
      ranking.head_cyclic[ranking.component[head]] = true;
      ranking.any_head_cycle = true;
    }
  }

  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (ranking.head_cyclic[ranking.component[atom]]) continue;
    if (component_size[ranking.component[atom]] > 1) {
      ranking.needs_level[atom] = true;
      continue;
    }
    // A component of one atom is still a cycle when that atom depends on
    // itself, as in 'a :- a, p.', so a self-edge counts too.
    for (int successor : succ[atom]) {
      if (successor == atom) {
        ranking.needs_level[atom] = true;
        break;
      }
    }
  }
  return ranking;
}

// Rejects the parts of aspif this translation does not cover, so that such a
// program gets an error rather than a wrong answer.
absl::Status check_supported(const aspif::Program& prog) {
  for (const aspif::Rule& rule : prog.rules) {
    // A choice head leaves its atoms free rather than deriving them, so the
    // completion below would have to stop forcing them. Nothing produces one:
    // normalization rewrites choice rules away.
    if (rule.head_type == aspif::Rule::HeadType::kChoice) {
      return absl::UnimplementedError("choice rule heads are not supported");
    }
  }
  return absl::OkStatus();
}

// What to call each atom and each level variable, indexed by atom id. Slot 0 is
// empty, 0 being no atom, as is the level of an atom that needs no ranking.
struct Names {
  std::vector<std::string> atom;
  std::vector<std::string> level;
};

// Whether `name` can be an SMT-LIB symbol. cvc5 prints a symbol holding
// anything unusual between vertical bars, so 'edge(a,b)' comes out as
// |edge(a,b)| and reads back the same. Not even that can hold a '|' or a '\',
// which an ASP program reaches only through a string, as in 'p("a|b")'.
bool is_smtlib_symbol(const std::string& name) {
  return !name.empty() && name.find_first_of("|\\") == std::string::npos;
}

// `wanted`, or the first name like it that nothing has taken. A program can
// print a symbol called 'a7' of its own, which is what the fallback name of
// atom 7 would otherwise be.
std::string fresh_name(absl::flat_hash_set<std::string>& taken,
                       std::string wanted) {
  while (!taken.insert(wanted).second) wanted.insert(wanted.begin(), '_');
  return wanted;
}

// Names each atom after the symbol the program prints for it, so that a model
// reads as a set of ASP atoms rather than as numbered booleans.
//
// An atom falls back to 'a<id>' where no printed symbol will serve. That covers
// the predicates normalization invents, which are kept out of the output, and
// symbols two atoms share, symbols holding only under several literals, and
// symbols an SMT-LIB symbol cannot spell.
Names choose_names(const aspif::Program& prog, const Ranking& ranking) {
  Names names;
  names.atom.resize(prog.next_atom);
  names.level.resize(prog.next_atom);

  absl::flat_hash_set<std::string> taken;
  for (const aspif::Output& output : prog.outputs) {
    if (output.condition.size() != 1 || output.condition.front() <= 0) continue;
    const aspif::Atom atom = output.condition.front();
    if (!names.atom[atom].empty() || !is_smtlib_symbol(output.name)) continue;
    if (!taken.insert(output.name).second) continue;
    names.atom[atom] = output.name;
  }

  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (names.atom[atom].empty()) {
      names.atom[atom] = fresh_name(taken, absl::StrCat("a", atom));
    }
    if (ranking.needs_level[atom]) {
      names.level[atom] =
          fresh_name(taken, absl::StrCat("lvl(", names.atom[atom], ")"));
    }
  }
  return names;
}

// One constant per name, indexed the way `names` is. An empty name leaves a
// null Term, which is what slot 0 and an atom needing no level variable get, so
// a null Term is how the rest of this file asks whether an atom is ranked.
std::vector<cvc5::Term> declare_constants(
    cvc5::TermManager& tm, const cvc5::Sort& sort,
    const std::vector<std::string>& names) {
  std::vector<cvc5::Term> vars(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i].empty()) continue;
    vars[i] = tm.mkConst(sort, names[i]);
  }
  return vars;
}

// The formula for one rule body.
cvc5::Term body_term(cvc5::TermManager& tm,
                     const std::vector<cvc5::Term>& atom_var,
                     const aspif::Rule& rule) {
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    std::vector<cvc5::Term> conjuncts;
    conjuncts.reserve(rule.body.size());
    for (aspif::Lit lit : rule.body) {
      conjuncts.push_back(literal_term(tm, atom_var, lit));
    }
    return conjunction(tm, conjuncts);
  }
  // A weight body holds when the weights of its true literals reach
  // lower_bound.
  return tm.mkTerm(cvc5::Kind::GEQ,
                   {weighted_sum(tm, atom_var, rule.weighted_body),
                    tm.mkInteger(rule.lower_bound)});
}

// One rule deriving one atom.
struct Support {
  // The formula for the rule's body, and for a disjunctive head every other
  // head atom being false. One rule supports one head atom at a time. 'a | b.'
  // supports a where a holds alone, and b where b holds alone, and neither
  // where both hold, holding both being more than the rule asks for. This is
  // the shift of Ben-Eliyahu & Dechter, taken per ground atom.
  cvc5::Term body;
  // The un-negated body atoms sharing the head's component, which the level
  // ranking has to order strictly below the head. A positive dependency that
  // crosses components cannot lie on a cycle, so it is left out and costs no
  // constraint.
  std::vector<aspif::Atom> ranked_below;
};

// The un-negated body atoms of `rule` that need a rank relative to `head`.
//
// A weight body contributes none of these in practice: ASP-Core-2 forbids
// recursive aggregates, so a weight body's atoms always sit in a lower
// component than the head. Were that to change, asking every literal of a
// weight body to rank below the head would be too strong, since reaching the
// bound only needs some of them to hold, and this would reject real answer
// sets.
std::vector<aspif::Atom> ranked_body_atoms(const aspif::Rule& rule,
                                           aspif::Atom head,
                                           const Ranking& ranking) {
  std::vector<aspif::Atom> ranked_below;
  if (!ranking.needs_level[head]) return ranked_below;
  for (aspif::Atom body_atom : positive_body_atoms(rule)) {
    if (ranking.component[body_atom] == ranking.component[head]) {
      ranked_below.push_back(body_atom);
    }
  }
  return ranked_below;
}

// Groups the rules by the atoms they derive. Rules with an empty head derive
// nothing and are left out. rule_formulas handles those.
std::vector<std::vector<Support>> collect_supports(
    cvc5::TermManager& tm, const aspif::Program& prog,
    const std::vector<cvc5::Term>& atom_var, const Ranking& ranking) {
  std::vector<std::vector<Support>> supports(prog.next_atom);
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.head.empty()) continue;
    const cvc5::Term body = body_term(tm, atom_var, rule);
    for (aspif::Atom head : rule.head) {
      std::vector<cvc5::Term> conjuncts = {body};
      for (aspif::Atom other : rule.head) {
        if (other == head) continue;
        conjuncts.push_back(tm.mkTerm(cvc5::Kind::NOT, {atom_var[other]}));
      }
      supports[head].push_back(
          Support{.body = conjunction(tm, conjuncts),
                  .ranked_below = ranked_body_atoms(rule, head, ranking)});
    }
  }
  return supports;
}

// Every rule is satisfied: where its body holds, one of its head atoms holds.
// An integrity constraint is the same statement about a rule with no head,
// whose empty disjunction is false, so it says the body cannot hold.
void rule_formulas(cvc5::TermManager& tm, const aspif::Program& prog,
                   const std::vector<cvc5::Term>& atom_var,
                   std::vector<cvc5::Term>& out) {
  for (const aspif::Rule& rule : prog.rules) {
    std::vector<cvc5::Term> heads;
    heads.reserve(rule.head.size());
    for (aspif::Atom head : rule.head) heads.push_back(atom_var[head]);
    out.push_back(tm.mkTerm(cvc5::Kind::IMPLIES, {body_term(tm, atom_var, rule),
                                                  disjunction(tm, heads)}));
  }
}

// Every atom that holds is supported: some rule deriving it has a support that
// holds. An atom no rule derives has no supports at all, so this makes it
// false.
//
// For single-atom heads this and rule_formulas are the two halves of the Clark
// completion, an atom holding exactly when one of the bodies deriving it holds.
// Disjunction splits them. 'a | b.' forces one of a and b without saying which,
// so the two directions stop sharing a formula.
//
// The rank conditions of the level ranking stay out of these formulas. Rule
// satisfaction and support hold without qualification. The ranking is a
// separate restriction on which supports count. Folding the two together would
// let the solver choose levels that falsify a support and drop an atom the
// rules force to hold.
void support_formulas(cvc5::TermManager& tm, const aspif::Program& prog,
                      const std::vector<cvc5::Term>& atom_var,
                      const std::vector<std::vector<Support>>& supports,
                      std::vector<cvc5::Term>& out) {
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    std::vector<cvc5::Term> bodies;
    bodies.reserve(supports[atom].size());
    for (const Support& support : supports[atom]) {
      bodies.push_back(support.body);
    }
    out.push_back(tm.mkTerm(cvc5::Kind::IMPLIES,
                            {atom_var[atom], disjunction(tm, bodies)}));
  }
}

// The level ranking: a true atom needs some rule whose body holds and whose
// same-component positive body atoms all rank strictly below it. Alongside the
// completion this rules out an atom whose only support is a positive cycle,
// because no assignment of levels can put every atom of a cycle below the next.
//
// Atoms with no level variable are skipped: they cannot lie on a cycle, so
// there is nothing about them to rank.
void ranking_formulas(cvc5::TermManager& tm, const aspif::Program& prog,
                      const std::vector<cvc5::Term>& atom_var,
                      const std::vector<cvc5::Term>& level_var,
                      const std::vector<std::vector<Support>>& supports,
                      std::vector<cvc5::Term>& out) {
  const cvc5::Term one = tm.mkInteger(1);
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (level_var[atom].isNull()) continue;
    std::vector<cvc5::Term> justified;
    justified.reserve(supports[atom].size());
    for (const Support& support : supports[atom]) {
      std::vector<cvc5::Term> conjuncts = {support.body};
      for (aspif::Atom below : support.ranked_below) {
        DCHECK(!level_var[below].isNull());
        // lvl(atom) - lvl(below) >= 1. A rule deriving an atom from itself, as
        // in 'a :- a, p.', asks for lvl(a) - lvl(a) >= 1 and so can justify
        // nothing, which is the right answer.
        conjuncts.push_back(tm.mkTerm(
            cvc5::Kind::GEQ,
            {tm.mkTerm(cvc5::Kind::SUB, {level_var[atom], level_var[below]}),
             one}));
      }
      justified.push_back(conjunction(tm, conjuncts));
    }
    out.push_back(tm.mkTerm(cvc5::Kind::IMPLIES,
                            {atom_var[atom], disjunction(tm, justified)}));
  }
}

// Whether a constant is named after `output`, which leaves nothing to say about
// it. The script's comment table holds the rest.
bool output_is_its_own_name(const aspif::Output& output,
                            const std::vector<std::string>& atom_name) {
  return output.condition.size() == 1 && output.condition.front() > 0 &&
         atom_name[output.condition.front()] == output.name;
}

// Adds a headed block to the script, and nothing at all where `body` is empty.
// A stratified program declares no level variables, and a program without a
// query asserts none, so those blocks go missing rather than stand empty.
void append_block(std::string& script, const char* title,
                  const std::string& body) {
  if (body.empty()) return;
  absl::StrAppend(&script, "\n; ", title, "\n", body);
}

// One line of the script's comment table: a symbol and the literals a model has
// to satisfy for it to be part of the answer set.
std::string output_line(const aspif::Output& output,
                        const std::vector<cvc5::Term>& atom_var) {
  if (output.condition.empty()) {
    return absl::StrCat(";   ", output.name, " : in every answer set\n");
  }
  std::vector<std::string> literals;
  literals.reserve(output.condition.size());
  for (aspif::Lit lit : output.condition) {
    literals.push_back(
        lit > 0 ? atom_var[lit].toString()
                : absl::StrCat("(not ", atom_var[-lit].toString(), ")"));
  }
  return absl::StrCat(";   ", output.name, " : ", absl::StrJoin(literals, " "),
                      "\n");
}

// The table naming the symbols no constant is named after. A model spells out
// the rest itself. pgass's own grounder gives every symbol an atom of its own,
// so it leaves this empty.
std::string symbol_table(const aspif::Program& prog, const Encoding& encoding) {
  std::string rows;
  for (const aspif::Output& output : prog.outputs) {
    if (output_is_its_own_name(output, encoding.atom_name)) continue;
    absl::StrAppend(&rows, output_line(output, encoding.atom_var));
  }
  if (rows.empty()) return rows;
  return absl::StrCat(
      ";\n; Symbols the program prints that no constant is\n"
      "; named after, and what each needs:\n",
      rows);
}
}  // namespace

cvc5::Term conjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& conjuncts) {
  if (conjuncts.empty()) return tm.mkTrue();
  if (conjuncts.size() == 1) return conjuncts.front();
  return tm.mkTerm(cvc5::Kind::AND, conjuncts);
}

cvc5::Term disjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& disjuncts) {
  if (disjuncts.empty()) return tm.mkFalse();
  if (disjuncts.size() == 1) return disjuncts.front();
  return tm.mkTerm(cvc5::Kind::OR, disjuncts);
}

cvc5::Term sum(cvc5::TermManager& tm, const std::vector<cvc5::Term>& addends) {
  if (addends.empty()) return tm.mkInteger(0);
  if (addends.size() == 1) return addends.front();
  return tm.mkTerm(cvc5::Kind::ADD, addends);
}

cvc5::Term literal_term(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        aspif::Lit lit) {
  if (lit > 0) return atom_var[lit];
  return tm.mkTerm(cvc5::Kind::NOT, {atom_var[-lit]});
}

cvc5::Term weighted_sum(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        const std::vector<aspif::WeightedLit>& lits) {
  const cvc5::Term zero = tm.mkInteger(0);
  std::vector<cvc5::Term> addends;
  addends.reserve(lits.size());
  for (const aspif::WeightedLit& weighted : lits) {
    addends.push_back(
        tm.mkTerm(cvc5::Kind::ITE, {literal_term(tm, atom_var, weighted.lit),
                                    tm.mkInteger(weighted.weight), zero}));
  }
  return sum(tm, addends);
}

absl::StatusOr<Encoding> build_encoding(cvc5::TermManager& tm,
                                        const aspif::Program& prog) {
  RETURN_IF_ERROR(check_supported(prog));

  const Ranking ranking = build_ranking(prog);
  Names names = choose_names(prog, ranking);

  Encoding encoding;
  encoding.logic = logic_for(prog);
  encoding.atom_var = declare_constants(tm, tm.getBooleanSort(), names.atom);
  encoding.level_var = declare_constants(tm, tm.getIntegerSort(), names.level);
  encoding.atom_name = std::move(names.atom);
  encoding.needs_reduct_check = ranking.any_head_cycle;

  const std::vector<std::vector<Support>> supports =
      collect_supports(tm, prog, encoding.atom_var, ranking);

  std::vector<cvc5::Term> rule_terms;
  rule_formulas(tm, prog, encoding.atom_var, rule_terms);
  std::vector<cvc5::Term> support_terms;
  support_formulas(tm, prog, encoding.atom_var, supports, support_terms);
  std::vector<cvc5::Term> ranking_terms;
  ranking_formulas(tm, prog, encoding.atom_var, encoding.level_var, supports,
                   ranking_terms);

  // The atoms the query matched, as formulas. None of them is asserted.
  // Asserting one would ask for an answer set that holds it, and a query asks
  // whether every answer set holds it. solve.cc asks that.
  if (prog.query.has_value()) {
    encoding.query.reserve(prog.query->size());
    for (aspif::Lit lit : *prog.query) {
      encoding.query.push_back(literal_term(tm, encoding.atom_var, lit));
    }
  }

  encoding.sections.push_back(
      Section{.title = "Rules", .assertions = std::move(rule_terms)});
  encoding.sections.push_back(
      Section{.title = "Support", .assertions = std::move(support_terms)});
  encoding.sections.push_back(Section{.title = "Level ranking",
                                      .assertions = std::move(ranking_terms)});
  return encoding;
}

absl::StatusOr<std::string> encode_smtlib(const aspif::Program& prog) {
  // The bound that pins an optimal answer set down is only known once several
  // queries have been answered. Writing the encoding without it would hand
  // back a script whose models are not the answer sets asked for.
  if (!prog.minimize.empty()) {
    return absl::UnimplementedError(
        "weak constraints cannot be encoded: finding the least cost of a "
        "priority level takes repeated queries under a falling bound");
  }

  // A script has one check-sat, so it can only carry a query of one atom.
  if (prog.query.has_value() && prog.query->size() != 1) {
    if (prog.query->empty()) {
      return absl::UnimplementedError(
          "a query matching no atom cannot be encoded: nothing can make it "
          "hold, so the answer is no whatever the answer sets are");
    }
    return absl::UnimplementedError(absl::StrCat(
        "a query matching ", prog.query->size(),
        " atoms cannot be encoded: each atom is asked about on its own, under "
        "a negation of its own"));
  }

  cvc5::TermManager tm;
  ASSIGN_OR_RETURN(const Encoding encoding, build_encoding(tm, prog));

  // Under a head cycle a model is only a candidate, and ruling out the ones a
  // smaller model of the reduct undercuts takes a query per candidate. solve.cc
  // runs those. A script cannot.
  if (encoding.needs_reduct_check) {
    return absl::UnimplementedError(
        "a head cycle cannot be encoded: telling an answer set from a model "
        "takes a further query per model, against the reduct");
  }

  std::string script = absl::StrCat("; Generated by pgass --encode=smtlib.\n",
                                    symbol_table(prog, encoding));

  // produce-models has to be set before the logic is, which SMT-LIB takes as
  // the end of the preamble.
  absl::StrAppend(&script, "(set-option :produce-models true)\n(set-logic ",
                  encoding.logic, ")\n");

  std::string constants;
  std::string levels;
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    absl::StrAppend(&constants, "(declare-const ",
                    encoding.atom_var[atom].toString(), " Bool)\n");
    if (encoding.level_var[atom].isNull()) continue;
    absl::StrAppend(&levels, "(declare-const ",
                    encoding.level_var[atom].toString(), " Int)\n");
  }
  append_block(script, "Constants", constants);
  append_block(script, "Level variables", levels);

  for (const Section& section : encoding.sections) {
    std::string assertions;
    for (const cvc5::Term& assertion : section.assertions) {
      absl::StrAppend(&assertions, "(assert ", assertion.toString(), ")\n");
    }
    append_block(script, section.title, assertions);
  }

  // The query goes in negated, so the script looks for an answer set the query
  // fails in. That flips what 'unsat' means, so the script says so itself.
  if (encoding.query.size() == 1) {
    append_block(
        script, "Query",
        absl::StrCat(
            "; The query holds where every answer set satisfies it, so it\n"
            "; goes in negated. A model is an answer set the query fails in,\n"
            "; and 'unsat' means the query holds.\n(assert ",
            tm.mkTerm(cvc5::Kind::NOT, {encoding.query.front()}).toString(),
            ")\n"));
  }

  absl::StrAppend(&script, "\n(check-sat)\n(get-model)\n");
  return script;
}
