#include "encode.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
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
// A weight body adds up the weights of many literals at once, which no
// difference logic atom can say, and costs the whole program the more general
// QF_LIA. Every QF_IDL formula is also a QF_LIA formula, so answering QF_LIA is
// never wrong, only slower.
//
// Weak constraints never reach here. Their costs come with the quantified
// optimality assertion, which takes ALL whatever the rest of the program says.

// Whether a weight body only counts its literals. #count writes every weight as
// one, and so does the counting constraint a choice rule normalizes into, which
// makes this the common weight body by far. at_least() says it without
// arithmetic, so it leaves the logic alone.
bool is_cardinality(const aspif::Rule& rule) {
  if (rule.body_type != aspif::Rule::BodyType::kWeight) return false;
  for (const aspif::WeightedLit& weighted : rule.weighted_body) {
    if (weighted.weight != BigInt(1)) return false;
  }
  return true;
}

const char* logic_for(const aspif::Program& prog) {
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.body_type == aspif::Rule::BodyType::kWeight &&
        !is_cardinality(rule)) {
      return "QF_LIA";
    }
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
  // Whether a smaller model of the reduct could drop each atom, which is true
  // exactly of the atoms on a positive cycle. A model described by minimality
  // rather than by a ranking quantifies over these atoms and no others.
  std::vector<bool> on_cycle;
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
  ranking.on_cycle.assign(prog.next_atom, false);
  ranking.head_cyclic.assign(ranking.component.size(), false);

  std::vector<int> component_size(ranking.component.size(), 0);
  for (int component : ranking.component) ++component_size[component];

  // Two head atoms of one rule in one component are a head cycle. A choice
  // head is exempt: it leaves each of its atoms free on its own, so no two of
  // them ever have to hold together.
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.head_type == aspif::Rule::HeadType::kChoice) continue;
    absl::flat_hash_set<int> seen;
    for (aspif::Atom head : rule.head) {
      if (seen.insert(ranking.component[head]).second) continue;
      ranking.head_cyclic[ranking.component[head]] = true;
      ranking.any_head_cycle = true;
    }
  }

  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    bool cyclic = component_size[ranking.component[atom]] > 1;
    // A component of one atom is still a cycle when that atom depends on
    // itself, as in 'a :- a, p.', so a self-edge counts too.
    for (int successor : succ[atom]) {
      if (successor == atom) cyclic = true;
    }
    const bool head_cyclic = ranking.head_cyclic[ranking.component[atom]];
    ranking.on_cycle[atom] = cyclic || head_cyclic;
    ranking.needs_level[atom] = cyclic && !head_cyclic;
  }
  return ranking;
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
  // A body that only counts is a question about the literals themselves, and
  // at_least() answers it without reaching for arithmetic.
  if (is_cardinality(rule)) {
    // A bound past the number of literals is out of reach however they fall,
    // and checking that first is what leaves the bound small enough to count
    // to.
    if (rule.lower_bound >
        BigInt(static_cast<int64_t>(rule.weighted_body.size()))) {
      return tm.mkFalse();
    }
    std::vector<cvc5::Term> lits;
    lits.reserve(rule.weighted_body.size());
    for (const aspif::WeightedLit& weighted : rule.weighted_body) {
      lits.push_back(literal_term(tm, atom_var, weighted.lit));
    }
    return at_least(tm, lits, rule.lower_bound.to_int64().value_or(0));
  }
  // A weight body holds when the weights of its true literals reach
  // lower_bound.
  return tm.mkTerm(cvc5::Kind::GEQ,
                   {weighted_sum(tm, atom_var, rule.weighted_body),
                    tm.mkInteger(rule.lower_bound.to_string())});
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
    const bool choice = rule.head_type == aspif::Rule::HeadType::kChoice;
    for (aspif::Atom head : rule.head) {
      std::vector<cvc5::Term> conjuncts = {body};
      // A choice head supports each of its atoms on the body alone.
      if (!choice) {
        for (aspif::Atom other : rule.head) {
          if (other == head) continue;
          conjuncts.push_back(tm.mkTerm(cvc5::Kind::NOT, {atom_var[other]}));
        }
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
//
// A choice rule forces nothing, so it has no formula here. Its head atoms are
// free, and support alone decides them.
void rule_formulas(cvc5::TermManager& tm, const aspif::Program& prog,
                   const std::vector<cvc5::Term>& atom_var,
                   std::vector<cvc5::Term>& out) {
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.head_type == aspif::Rule::HeadType::kChoice) continue;
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

// One paragraph of comment, wrapped to fit in 80 columns.
std::string comment_lines(std::string_view text) {
  constexpr size_t kWidth = 74;
  std::string out;
  std::string line = ";";
  for (std::string_view word : absl::StrSplit(text, ' ', absl::SkipEmpty())) {
    if (line.size() + 1 + word.size() > kWidth) {
      absl::StrAppend(&out, line, "\n");
      line = ";";
    }
    absl::StrAppend(&line, " ", word);
  }
  if (line != ";") absl::StrAppend(&out, line, "\n");
  return out;
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

// The literals of each priority level, the most important level first, which is
// the order the levels have to be settled in. Two minimize statements sharing a
// priority are one level, the cost of a level being the total over its
// literals.
std::vector<std::pair<BigInt, std::vector<aspif::WeightedLit>>> level_costs(
    const aspif::Program& prog) {
  absl::btree_map<BigInt, std::vector<aspif::WeightedLit>> by_priority;
  for (const aspif::Minimize& minimize : prog.minimize) {
    std::vector<aspif::WeightedLit>& lits = by_priority[minimize.priority];
    lits.insert(lits.end(), minimize.lits.begin(), minimize.lits.end());
  }
  return {by_priority.rbegin(), by_priority.rend()};
}

// The block naming the cost of each priority level and asserting that no answer
// set costs less, so that one check-sat lands on an optimal answer set.
std::string weak_constraint_block(const aspif::Program& prog,
                                  const Encoding& encoding) {
  // Only the atom names can collide with a cost name. Every level name holds
  // parentheses, which no cost name does.
  absl::flat_hash_set<std::string> taken(encoding.atom_name.begin(),
                                         encoding.atom_name.end());
  std::vector<std::string> names;
  std::string definitions;
  size_t level = 0;
  for (const auto& [priority, lits] : level_costs(prog)) {
    names.push_back(
        fresh_name(taken, absl::StrCat("cost@", priority.to_string())));
    absl::StrAppend(&definitions, "(define-fun ", names.back(), " () Int ",
                    encoding.level_cost[level++].toString(), ")\n");
  }

  return absl::StrCat(
      comment_lines(absl::StrCat(
          "What a model costs at each priority level, most important level "
          "first: ",
          absl::StrJoin(names, ", "),
          ". The assertion below says no answer set costs less, so the "
          "check-sat at the end lands on an optimal one. cvc5 answers it far "
          "faster under --sygus-inst.")),
      definitions, "(assert ", encoding.optimality->toString(), ")\n");
}

// One rule of the reduct, as a formula the subset has to satisfy.
//
// A rule whose body holds a 'not q' where the model holds q is not in the
// reduct at all, so the 'not' literals read the model. The positive body atoms
// and the head read the subset, which is what has to model the reduct.
//
// A choice head is '{a} :- B' for each of its atoms, which is 'a :- B, not not
// a'. The reduct keeps that rule for the atoms the model holds and drops it for
// the rest, so each atom is forced only where the model holds it.
cvc5::Term reduct_rule(cvc5::TermManager& tm, const aspif::Rule& rule,
                       const std::vector<cvc5::Term>& atom_var,
                       const std::vector<cvc5::Term>& subset_var) {
  cvc5::Term body;
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    std::vector<cvc5::Term> conjuncts;
    conjuncts.reserve(rule.body.size());
    for (aspif::Lit lit : rule.body) {
      conjuncts.push_back(lit > 0
                              ? subset_var[lit]
                              : tm.mkTerm(cvc5::Kind::NOT, {atom_var[-lit]}));
    }
    body = conjunction(tm, conjuncts);
  } else {
    const cvc5::Term zero = tm.mkInteger(0);
    std::vector<cvc5::Term> addends;
    addends.reserve(rule.weighted_body.size());
    for (const aspif::WeightedLit& weighted : rule.weighted_body) {
      const cvc5::Term weight = tm.mkInteger(weighted.weight.to_string());
      const cvc5::Term holds =
          weighted.lit > 0
              ? subset_var[weighted.lit]
              : tm.mkTerm(cvc5::Kind::NOT, {atom_var[-weighted.lit]});
      addends.push_back(tm.mkTerm(cvc5::Kind::ITE, {holds, weight, zero}));
    }
    body = tm.mkTerm(
        cvc5::Kind::GEQ,
        {sum(tm, addends), tm.mkInteger(rule.lower_bound.to_string())});
  }

  if (rule.head_type == aspif::Rule::HeadType::kChoice) {
    std::vector<cvc5::Term> kept;
    kept.reserve(rule.head.size());
    for (aspif::Atom head : rule.head) {
      kept.push_back(tm.mkTerm(
          cvc5::Kind::IMPLIES,
          {tm.mkTerm(cvc5::Kind::AND, {body, atom_var[head]}),
           subset_var[head]}));
    }
    return conjunction(tm, kept);
  }

  std::vector<cvc5::Term> heads;
  heads.reserve(rule.head.size());
  for (aspif::Atom head : rule.head) heads.push_back(subset_var[head]);
  return tm.mkTerm(cvc5::Kind::IMPLIES, {body, disjunction(tm, heads)});
}

// The minimality check as one formula: no proper subset of the atoms the model
// `model_var` holds is itself a model of the reduct under it. That is the
// second half of the ASP-Core-2 definition of an answer set, and the subset
// being a bound variable is what lets one formula stand for every candidate at
// once.
//
// `quantified` picks the atoms the subset may drop. Elsewhere the subset is the
// model itself, which costs it nothing, every rule of the reduct being an
// implication. Restricting the quantifier this way is what makes the formula
// usable: quantifying over every atom sent one 2-QBF instance of 55 atoms, 23
// of them head-cyclic, from 0.03s to over 45s.
cvc5::Term minimality_term(cvc5::TermManager& tm, const aspif::Program& prog,
                           const std::vector<cvc5::Term>& model_var,
                           const std::vector<std::string>& model_name,
                           const std::vector<bool>& quantified,
                           absl::flat_hash_set<std::string>& taken) {
  const cvc5::Sort bool_sort = tm.getBooleanSort();
  std::vector<cvc5::Term> subset_var(prog.next_atom);
  std::vector<cvc5::Term> bound_vars;
  std::vector<cvc5::Term> conjuncts;
  // What the quantified atoms say about the subset: it holds nothing the model
  // leaves out, and it leaves out something the model holds, which together
  // make it a proper subset. An atom standing for itself says neither.
  std::vector<cvc5::Term> dropped;
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (!quantified[atom]) {
      subset_var[atom] = model_var[atom];
      continue;
    }
    const cvc5::Term subset = tm.mkVar(
        bool_sort,
        fresh_name(taken, absl::StrCat("sub(", model_name[atom], ")")));
    subset_var[atom] = subset;
    bound_vars.push_back(subset);
    conjuncts.push_back(
        tm.mkTerm(cvc5::Kind::IMPLIES, {subset, model_var[atom]}));
    dropped.push_back(
        tm.mkTerm(cvc5::Kind::AND,
                  {model_var[atom], tm.mkTerm(cvc5::Kind::NOT, {subset})}));
  }
  // With nothing to drop there is no proper subset to rule out, and the check
  // holds of every model.
  if (bound_vars.empty()) return tm.mkTrue();

  conjuncts.push_back(disjunction(tm, dropped));
  for (const aspif::Rule& rule : prog.rules) {
    conjuncts.push_back(reduct_rule(tm, rule, model_var, subset_var));
  }

  return tm.mkTerm(cvc5::Kind::FORALL,
                   {tm.mkTerm(cvc5::Kind::VARIABLE_LIST, bound_vars),
                    tm.mkTerm(cvc5::Kind::NOT, {conjunction(tm, conjuncts)})});
}

// Whether the model `alt_var` costs lexicographically less than the model
// `atom_var`. The levels are settled in turn, the most important first, so this
// is one disjunct per level: every level above it costs the same, and that
// level costs strictly less.
cvc5::Term lower_cost_term(cvc5::TermManager& tm, const aspif::Program& prog,
                           const std::vector<cvc5::Term>& atom_var,
                           const std::vector<cvc5::Term>& alt_var) {
  std::vector<cvc5::Term> disjuncts;
  std::vector<cvc5::Term> equal_above;
  for (const auto& [priority, lits] : level_costs(prog)) {
    const cvc5::Term cost = weighted_sum(tm, atom_var, lits);
    const cvc5::Term alt_cost = weighted_sum(tm, alt_var, lits);
    std::vector<cvc5::Term> conjuncts = equal_above;
    conjuncts.push_back(tm.mkTerm(cvc5::Kind::LT, {alt_cost, cost}));
    disjuncts.push_back(conjunction(tm, conjuncts));
    equal_above.push_back(tm.mkTerm(cvc5::Kind::EQUAL, {alt_cost, cost}));
  }
  return disjunction(tm, disjuncts);
}

// Optimality as one formula: no answer set costs less than this one. The other
// answer set is a second Bool per atom, bound by a quantifier, so the formula
// stands for every candidate at once the way the minimality check does.
//
// The rules, the support, and the minimality check are what say the bound atoms
// are an answer set. The level ranking is left out on purpose: it would take an
// Int per atom, and describing the other answer set by its reduct instead keeps
// every bound variable Boolean. Only the costs stay arithmetic.
cvc5::Term optimality_term(cvc5::TermManager& tm, const aspif::Program& prog,
                           const Encoding& encoding, const Ranking& ranking) {
  absl::flat_hash_set<std::string> taken(encoding.atom_name.begin(),
                                         encoding.atom_name.end());
  const cvc5::Sort bool_sort = tm.getBooleanSort();
  std::vector<cvc5::Term> alt_var(prog.next_atom);
  std::vector<std::string> alt_name(prog.next_atom);
  std::vector<cvc5::Term> bound_vars;
  bound_vars.reserve(prog.next_atom - 1);
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    alt_name[atom] =
        fresh_name(taken, absl::StrCat("alt(", encoding.atom_name[atom], ")"));
    alt_var[atom] = tm.mkVar(bool_sort, alt_name[atom]);
    bound_vars.push_back(alt_var[atom]);
  }
  // A program with no atoms has one answer set, so nothing can cost less.
  if (bound_vars.empty()) return tm.mkTrue();

  std::vector<cvc5::Term> is_answer_set;
  rule_formulas(tm, prog, alt_var, is_answer_set);
  support_formulas(tm, prog, alt_var,
                   collect_supports(tm, prog, alt_var, ranking), is_answer_set);
  const cvc5::Term minimal =
      minimality_term(tm, prog, alt_var, alt_name, ranking.on_cycle, taken);
  if (minimal != tm.mkTrue()) is_answer_set.push_back(minimal);

  return tm.mkTerm(
      cvc5::Kind::FORALL,
      {tm.mkTerm(cvc5::Kind::VARIABLE_LIST, bound_vars),
       tm.mkTerm(cvc5::Kind::IMPLIES,
                 {conjunction(tm, is_answer_set),
                  tm.mkTerm(cvc5::Kind::NOT,
                            {lower_cost_term(tm, prog, encoding.atom_var,
                                             alt_var)})})});
}

constexpr char kMinimalityComment[] =
    R"(; A rule of this program has two head atoms on a common positive cycle,
; so the assertions above admit models that are not answer sets. The
; constraint below rules those out.
)";

// How a comment names the query: as the program wrote it, or as 'the query'
// where the text is gone, which is what a program read as aspif leaves.
std::string query_name(const aspif::Program& prog) {
  if (prog.query_text.empty()) return "The query";
  return absl::StrCat("The query ", prog.query_text);
}

// The block asking about a query of several atoms. Each atom is asked about on
// its own, under a negation of its own, so the block is several check-sats
// rather than one.
std::string several_queries_block(cvc5::TermManager& tm,
                                  const aspif::Program& prog,
                                  const Encoding& encoding) {
  constexpr char kAnswers[] =
      R"(;
; The answers come back in the order the atoms appear below:
;   unsat  that atom holds in every answer set, so it answers the query.
;   sat    it does not. Add (get-model) after the check-sat to see an
;          answer set where it fails.
; The query holds if at least one answer is unsat.
)";
  std::string block = absl::StrCat(
      comment_lines(absl::StrCat(
          query_name(prog), " matched ", encoding.query.size(),
          " atoms. Each is a separate question, so each gets a check-sat of "
          "its own, under a negation of its own.")),
      kAnswers);
  for (const cvc5::Term& formula : encoding.query) {
    absl::StrAppend(&block, "(push 1)\n(assert ",
                    tm.mkTerm(cvc5::Kind::NOT, {formula}).toString(),
                    ")\n(check-sat)\n(pop 1)\n");
  }
  return block;
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

namespace {

// The counts of one run of `lits`: entry i says at least i + 1 of
// lits[begin, end) hold. The list stops at `cap` entries, so a run longer than
// the bound still only carries the counts the bound can tell apart.
std::vector<cvc5::Term> at_least_counts(cvc5::TermManager& tm,
                                        const std::vector<cvc5::Term>& lits,
                                        size_t begin, size_t end, size_t cap) {
  const size_t n = end - begin;
  if (n == 1) return {lits[begin]};

  const size_t mid = begin + n / 2;
  const std::vector<cvc5::Term> left =
      at_least_counts(tm, lits, begin, mid, cap);
  const std::vector<cvc5::Term> right =
      at_least_counts(tm, lits, mid, end, cap);

  // At least k hold across both halves when, for some split of k, that many
  // hold in each. The halves are counted already, so this only picks the splits
  // and takes them together.
  std::vector<cvc5::Term> counts;
  counts.reserve(std::min(n, cap));
  for (size_t k = 1; k <= std::min(n, cap); ++k) {
    // The right half cannot supply more than it has, which is what puts a floor
    // under the left half's share.
    const size_t lowest = k > right.size() ? k - right.size() : 0;
    std::vector<cvc5::Term> splits;
    for (size_t i = lowest; i <= std::min(k, left.size()); ++i) {
      const size_t j = k - i;
      if (i == 0) {
        splits.push_back(right[j - 1]);
      } else if (j == 0) {
        splits.push_back(left[i - 1]);
      } else {
        splits.push_back(
            tm.mkTerm(cvc5::Kind::AND, {left[i - 1], right[j - 1]}));
      }
    }
    counts.push_back(disjunction(tm, splits));
  }
  return counts;
}

}  // namespace

cvc5::Term at_least(cvc5::TermManager& tm, const std::vector<cvc5::Term>& lits,
                    std::int64_t bound) {
  // Nothing to reach, and nothing that could reach it.
  if (bound <= 0) return tm.mkTrue();
  if (static_cast<size_t>(bound) > lits.size()) return tm.mkFalse();

  const std::vector<cvc5::Term> counts =
      at_least_counts(tm, lits, 0, lits.size(), static_cast<size_t>(bound));
  return counts[bound - 1];
}

cvc5::Term weighted_sum(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        const std::vector<aspif::WeightedLit>& lits) {
  const cvc5::Term zero = tm.mkInteger(0);
  std::vector<cvc5::Term> addends;
  addends.reserve(lits.size());
  for (const aspif::WeightedLit& weighted : lits) {
    addends.push_back(tm.mkTerm(
        cvc5::Kind::ITE, {literal_term(tm, atom_var, weighted.lit),
                          tm.mkInteger(weighted.weight.to_string()), zero}));
  }
  return sum(tm, addends);
}

absl::StatusOr<Encoding> build_encoding(cvc5::TermManager& tm,
                                        const aspif::Program& prog) {
  const Ranking ranking = build_ranking(prog);
  Names names = choose_names(prog, ranking);

  Encoding encoding;
  // The minimality and optimality assertions below are quantified, which no
  // quantifier free logic can hold.
  const bool quantified = ranking.any_head_cycle || !prog.minimize.empty();
  encoding.logic = quantified ? "ALL" : logic_for(prog);
  encoding.atom_var = declare_constants(tm, tm.getBooleanSort(), names.atom);
  encoding.level_var = declare_constants(tm, tm.getIntegerSort(), names.level);
  encoding.atom_name = std::move(names.atom);

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
  if (ranking.any_head_cycle) {
    absl::flat_hash_set<std::string> taken(encoding.atom_name.begin(),
                                           encoding.atom_name.end());
    // Only the atoms of a head-cyclic component are quantified. The rest are
    // ranked, and a ranked atom cannot be unfounded, so no smaller model of the
    // reduct differs there.
    std::vector<bool> quantified(prog.next_atom, false);
    for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
      quantified[atom] = ranking.head_cyclic[ranking.component[atom]];
    }
    const cvc5::Term minimality = minimality_term(
        tm, prog, encoding.atom_var, encoding.atom_name, quantified, taken);
    encoding.sections.push_back(Section{.title = "Minimality",
                                        .comment = kMinimalityComment,
                                        .assertions = {minimality}});
  }
  if (!prog.minimize.empty()) {
    for (const auto& [priority, lits] : level_costs(prog)) {
      encoding.level_cost.push_back(weighted_sum(tm, encoding.atom_var, lits));
    }
    encoding.optimality = optimality_term(tm, prog, encoding, ranking);
  }
  return encoding;
}

absl::StatusOr<std::string> encode_smtlib(const aspif::Program& prog) {
  cvc5::TermManager tm;
  ASSIGN_OR_RETURN(const Encoding encoding, build_encoding(tm, prog));

  std::string script = absl::StrCat("; Generated by pgass --encode=smtlib.\n",
                                    symbol_table(prog, encoding));

  // Options are set before the logic is, which SMT-LIB takes as the end of the
  // preamble.
  absl::StrAppend(&script, "(set-option :produce-models true)\n");
  if (encoding.query.size() > 1) {
    absl::StrAppend(&script,
                    "; incremental: the query below asks several times.\n"
                    "(set-option :incremental true)\n");
  }
  absl::StrAppend(&script, "(set-logic ", encoding.logic, ")\n");

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
    std::string body = section.comment;
    for (const cvc5::Term& assertion : section.assertions) {
      absl::StrAppend(&body, "(assert ", assertion.toString(), ")\n");
    }
    append_block(script, section.title, body);
  }

  if (!prog.minimize.empty()) {
    append_block(script, "Weak constraints",
                 weak_constraint_block(prog, encoding));
  }

  if (encoding.query.size() > 1) {
    append_block(script, "Query", several_queries_block(tm, prog, encoding));
  } else if (encoding.query.size() == 1) {
    // The query goes in negated, so the script looks for an answer set the
    // query fails in. That flips what 'unsat' means, so the script says so
    // itself.
    append_block(
        script, "Query",
        absl::StrCat(
            comment_lines(absl::StrCat(
                query_name(prog),
                " holds where every answer set satisfies it, so it goes in "
                "negated. 'unsat' means it holds. 'sat' means it does not, and "
                "the model is an answer set where it fails.")),
            "(assert ",
            tm.mkTerm(cvc5::Kind::NOT, {encoding.query.front()}).toString(),
            ")\n"));
  } else if (prog.query.has_value()) {
    // A query no atom matched, as 'p(1). q(2)?' asks.
    append_block(
        script, "Query",
        comment_lines(absl::StrCat(
            query_name(prog),
            " matched no atom, so nothing is asserted for it. It holds only "
            "if the program has no answer set, which is what 'unsat' means.")));
  }

  // A query of several atoms brought a check-sat per atom. Every other script
  // asks here.
  if (encoding.query.size() <= 1) {
    // One check-sat finds one answer set. pgass goes on to the next by ruling
    // this one out, and a reader does the same by hand.
    if (!prog.query.has_value()) {
      absl::StrAppend(
          &script, "\n",
          comment_lines(
              "The model below is one answer set. For another, assert a clause "
              "asking some atom to differ from it, as in "
              "'(assert (or (not a) b))', and run again. Name only the "
              "constants above, never a level variable: one answer set admits "
              "many rankings."));
    }
    absl::StrAppend(&script, "(check-sat)\n(get-model)\n");
  }
  return script;
}
