#ifndef COLLECT_H_
#define COLLECT_H_

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "ast.h"

// Helpers for scanning the AST to find things. Today that means variable
// collection; grounding and translation are expected to grow more queries
// (e.g. constant/predicate collection) alongside these.
namespace collect {

// Invokes `visit` once per variable occurrence within the node, in left-to-
// right order. Repeated variables are visited repeatedly; callers that want
// distinct names should dedupe (or use a collect_variables overload).
//
// The visitor gets the Variable node rather than just its name, so a caller
// that wants to attach something to each occurrence, e.g. the grounder
// numbering a rule's variables, can key it on the node's address.
using VariableVisitor = std::function<void(const Variable&)>;
void for_each_variable(const Term& term, const VariableVisitor& visit);
void for_each_variable(const Literal& literal, const VariableVisitor& visit);
// An aggregate's variables are the ones in its bounds, in its element terms,
// and in its element conditions, e.g. N, X and Y for '#count{X : e(X, Y)} = N'.
void for_each_variable(const Aggregate& aggregate,
                       const VariableVisitor& visit);

// Inserts every variable name occurring in the node into `out`.
void collect_variables(const Term& term,
                       absl::flat_hash_set<std::string_view>& out);
void collect_variables(const Literal& literal,
                       absl::flat_hash_set<std::string_view>& out);

// Appends the distinct variable names occurring in the node to `out`, in
// first-occurrence order, skipping any already present. Useful when the order
// of variables matters (e.g. synthesizing an argument list).
void collect_variables(const Term& term, std::vector<std::string>& out);
void collect_variables(const Literal& literal, std::vector<std::string>& out);

// Invokes `visit` on the outermost term of each place the node holds one: each
// argument of a classical literal, each side of a builtin atom. The reference
// is mutable, so a visitor can put a different term in the place it is handed.
//
// A node that holds terms through elements of its own, such as an aggregate,
// is not here. Which element a term stands in is something its caller needs to
// know, so it walks the elements itself.
using TermVisitor = std::function<void(std::unique_ptr<Term>&)>;
void for_each_term(Terms& terms, const TermVisitor& visit);
void for_each_term(Literal& literal, const TermVisitor& visit);
void for_each_term(std::vector<std::unique_ptr<NafLiteral>>& nafs,
                   const TermVisitor& visit);
void for_each_term(Weight& weight, const TermVisitor& visit);

// Invokes `visit` on every term within `slot`, innermost first, ending with
// `slot` itself: the 1, the 3, the '1..3', then the 'f(1..3)' of 'f(1..3)'.
// Innermost first means a visitor that replaces a term is handed one whose
// parts it has already rewritten.
void for_each_subterm(std::unique_ptr<Term>& slot, const TermVisitor& visit);

// Invokes `visit` on every ClassicalLiteral reachable through the node, in
// left-to-right order. The reference is mutable so callers can rewrite literals
// in place (e.g. eliminating classical negation). BuiltinAtoms carry no
// predicate id and are skipped.
using ClassicalLiteralVisitor = std::function<void(ClassicalLiteral&)>;
void for_each_classical_literal(Head& head,
                                const ClassicalLiteralVisitor& visit);
void for_each_classical_literal(Body& body,
                                const ClassicalLiteralVisitor& visit);

// Invokes `visit(literal, negated)` for each classical literal in `nafs`,
// where `negated` combines the literal's own 'not' with `negated_context`
// (e.g. an enclosing default-negated aggregate).
using NegatedClassicalLiteralVisitor =
    std::function<void(ClassicalLiteral&, bool)>;
void for_each_classical_literal(std::vector<std::unique_ptr<NafLiteral>>& nafs,
                                bool negated_context,
                                const NegatedClassicalLiteralVisitor& visit);

// Invokes `visit(literal, negated)` for every classical literal in `body`,
// including ones inside aggregate elements (additionally negated by the
// aggregate's own 'not').
void for_each_classical_literal(Body& body,
                                const NegatedClassicalLiteralVisitor& visit);

}  // namespace collect

#endif  // COLLECT_H_
