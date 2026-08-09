#ifndef AST_H_
#define AST_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bigint.h"

struct Head {
  enum Kind { ChoiceKind, DisjunctionKind };
  Kind kind;
  virtual ~Head() = default;

 protected:
  Head(Kind k) : kind(k) {}
};

struct Term {
  enum Kind {
    AtomKind,
    NumberKind,
    StringKind,
    VariableKind,
    AnonymousVariableKind,
    NegatedTermKind,
    TermOperationKind,
    IntervalKind
  };
  Kind kind;
  virtual ~Term() = default;

  virtual std::unique_ptr<Term> clone() const = 0;

 protected:
  Term(Kind k) : kind(k) {}
};

using Terms = std::unique_ptr<std::vector<std::unique_ptr<Term>>>;

struct BodyItem {
  enum Kind { NafLiteralKind, AggregateKind };
  Kind kind;
  virtual ~BodyItem() = default;

  std::unique_ptr<BodyItem> clone() const {
    return std::unique_ptr<BodyItem>(clone_impl());
  }

 protected:
  BodyItem(Kind k) : kind(k) {}
  // Covariant raw-pointer clone, so derived classes can expose a clone() that
  // returns their own concrete type (see NafLiteral).
  virtual BodyItem* clone_impl() const = 0;
};

struct Body {
  std::unique_ptr<std::vector<std::unique_ptr<BodyItem>>> items;
};

struct Weight {
  std::unique_ptr<Term> weight;
  std::unique_ptr<Term> level;
  Terms terms;

  Weight(std::unique_ptr<Term> w, std::unique_ptr<Term> l, Terms t)
      : weight(std::move(w)), level(std::move(l)), terms(std::move(t)) {}
};

/* A '#show' directive, which says what an answer set prints and nothing else.

   A signature, '#show p/2.' or '#show -p/2.' or '#show.', names the predicates
   to print. A term, '#show t : body.', prints t for every way the body holds,
   whether or not the program derives an atom named t.

   Both forms are statements rather than fields of Program, unlike the query,
   so that format() prints them back where the program wrote them.
*/
struct Show {
  struct Signature {
    bool negated = false;
    std::string name;
    size_t arity = 0;
  };

  // Set in the signature form. '#show.' sets neither this nor `term`.
  std::optional<Signature> signature;
  // Set in the term form. The statement's body is the condition.
  std::unique_ptr<Term> term;
};

// Which predicates an answer set prints. No value means every one the user
// wrote does. A value holds the signatures '#show' named, so an empty one
// prints nothing.
using ShowFilter = std::optional<std::vector<Show::Signature>>;

// A '#const' directive, '#const n = 42.'. The name stands for the term
// everywhere in the program, whatever line the directive is written on.
struct Constant {
  std::string name;
  std::unique_ptr<Term> value;
};

struct Minimize;

struct Statement {
  std::unique_ptr<Head> head;
  std::unique_ptr<Body> body;
  std::unique_ptr<Weight> weight;
  // Set on a '#show' statement, which has no head and whose body, if any, is
  // the shown term's condition.
  std::unique_ptr<Show> show;
  // Set on a '#const' statement, which has neither head nor body.
  std::unique_ptr<Constant> constant;
  // Set on a '#minimize' or '#maximize' statement. Its elements carry their
  // own conditions, so it has neither head nor body.
  std::unique_ptr<Minimize> minimize;
  size_t source_pos = 0;
};

using Statements = std::unique_ptr<std::vector<std::unique_ptr<Statement>>>;

enum class BinopType {
  kEQUAL,
  kUNEQUAL,
  kLESS,
  kGREATER,
  kLESS_OR_EQ,
  kGREATER_OR_EQ
};

struct Literal {
  enum Kind { BuiltinAtomKind, ClassicalLiteralKind };
  Kind kind;
  virtual ~Literal() = default;

  std::unique_ptr<Literal> clone() const {
    return std::unique_ptr<Literal>(clone_impl());
  }

 protected:
  Literal(Kind k) : kind(k) {}
  // Covariant raw-pointer clone, so ClassicalLiteral can expose a clone() that
  // returns std::unique_ptr<ClassicalLiteral>.
  virtual Literal* clone_impl() const = 0;
};

enum class AggregateFunctionType {
  kAGGREGATE_COUNT,
  kAGGREGATE_MAX,
  kAGGREGATE_MIN,
  kAGGREGATE_SUM
};

struct NafLiteral : BodyItem {
  bool naf;
  std::unique_ptr<Literal> literal;

  NafLiteral() : BodyItem(NafLiteralKind), naf(false) {}

  std::unique_ptr<NafLiteral> clone() const {
    return std::unique_ptr<NafLiteral>(clone_impl());
  }

 protected:
  NafLiteral* clone_impl() const override;  // covariant with BodyItem*
};

using NafLiterals = std::unique_ptr<std::vector<std::unique_ptr<NafLiteral>>>;

// One element of a '#minimize', '1@2, X : p(X)': what it costs and when it
// costs it.
struct MinimizeElement {
  std::unique_ptr<Weight> weight;
  NafLiterals condition;
};

using MinimizeElements = std::vector<std::unique_ptr<MinimizeElement>>;

// A set of weak constraints written as one statement: '#minimize{ w@l, t :
// body }.' says what ':~ body. [w@l, t]' says. A '#maximize' is the same with
// the sign of each weight flipped.
struct Minimize {
  bool maximize = false;
  MinimizeElements elements;
};

struct AggregateElement {
  Terms terms;  // These are actually "basic" terms, see grammar for details.
  NafLiterals literals;

  AggregateElement(Terms t, NafLiterals l)
      : terms(std::move(t)), literals(std::move(l)) {}

  std::unique_ptr<AggregateElement> clone() const;
};

using AggregateElements =
    std::unique_ptr<std::vector<std::unique_ptr<AggregateElement>>>;

struct Aggregate : BodyItem {
  bool naf;
  std::unique_ptr<Term> lb_term;
  BinopType lb_op;  // only valid if lb_term != nullptr;
  std::unique_ptr<Term> ub_term;
  BinopType ub_op;  // only valid if ub_term != nullptr;
  AggregateFunctionType function;
  AggregateElements elements;

  Aggregate() : BodyItem(AggregateKind) {}

 protected:
  Aggregate* clone_impl() const override;  // covariant with BodyItem*
};

struct BuiltinAtom : Literal {
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;
  BinopType op;

  BuiltinAtom() : Literal(BuiltinAtomKind), op(BinopType::kEQUAL) {}

 protected:
  BuiltinAtom* clone_impl() const override;  // covariant with Literal*
};

struct ClassicalLiteral : Literal {
  bool negated;
  std::string id;
  Terms args;

  ClassicalLiteral() : Literal(ClassicalLiteralKind), negated(false) {}

  std::unique_ptr<ClassicalLiteral> clone() const {
    return std::unique_ptr<ClassicalLiteral>(clone_impl());
  }

 protected:
  ClassicalLiteral* clone_impl() const override;  // covariant with Literal*
};

struct Disjunction : Head {
  std::vector<std::unique_ptr<ClassicalLiteral>> literals;

  Disjunction() : Head(DisjunctionKind) {}
};

struct ChoiceElement {
  std::unique_ptr<ClassicalLiteral> literal;
  NafLiterals conditions;

  ChoiceElement(std::unique_ptr<ClassicalLiteral> l, NafLiterals c)
      : literal(std::move(l)), conditions(std::move(c)) {}
};

using ChoiceElements =
    std::unique_ptr<std::vector<std::unique_ptr<ChoiceElement>>>;

struct Choice : Head {
  std::unique_ptr<Term> lb_term;
  BinopType lb_op;  // only valid if lb_term != nullptr;
  std::unique_ptr<Term> ub_term;
  BinopType ub_op;  // only valid if ub_term != nullptr;
  ChoiceElements elements;

  Choice() : Head(ChoiceKind) {}
};

struct Query {
  std::unique_ptr<ClassicalLiteral> lit;
};

struct Program {
  Statements statements;
  std::unique_ptr<Query> query;
  // normalize() fills this in from the program's '#show' statements.
  ShowFilter show_filter;
  std::string_view source;
};

struct Atom : Term {
  std::string name;
  Terms args;

  Atom(std::string name, Terms args)
      : Term(AtomKind), name(std::move(name)), args(std::move(args)) {}

  std::unique_ptr<Term> clone() const override;
};

struct Number : Term {
  // A literal is always non-negative. A leading '-' is its own token, which
  // parses as a NegatedTerm around this.
  BigInt value;

  Number(BigInt value) : Term(NumberKind), value(std::move(value)) {}

  std::unique_ptr<Term> clone() const override;
};

struct String : Term {
  std::string value;  // the contents, without the surrounding quotes

  String(std::string_view value)
      : Term(StringKind), value(std::string(value)) {}

  std::unique_ptr<Term> clone() const override;
};

struct Variable : Term {
  std::string name;

  Variable(std::string_view name)
      : Term(VariableKind), name(std::string(name)) {}

  std::unique_ptr<Term> clone() const override;
};

struct AnonymousVariable : Term {
  AnonymousVariable() : Term(AnonymousVariableKind) {}

  std::unique_ptr<Term> clone() const override;
};

struct NegatedTerm : Term {
  std::unique_ptr<Term> term;

  NegatedTerm(std::unique_ptr<Term> term)
      : Term(NegatedTermKind), term(std::move(term)) {}

  std::unique_ptr<Term> clone() const override;
};

enum class OperationType { kPLUS, kMINUS, kTIMES, kDIV };

struct TermOperation : Term {
  OperationType op;
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;

  TermOperation(OperationType o, std::unique_ptr<Term> l,
                std::unique_ptr<Term> r)
      : Term(TermOperationKind),
        op(o),
        left(std::move(l)),
        right(std::move(r)) {}

  std::unique_ptr<Term> clone() const override;
};

// An interval, '1..3', which stands for each integer from `lower` to `upper`.
// A term holding one stands for as many terms as the interval has values, so
// 'p(1..3).' is three facts.
struct Interval : Term {
  std::unique_ptr<Term> lower;
  std::unique_ptr<Term> upper;

  Interval(std::unique_ptr<Term> lower, std::unique_ptr<Term> upper)
      : Term(IntervalKind), lower(std::move(lower)), upper(std::move(upper)) {}

  std::unique_ptr<Term> clone() const override;
};

#endif  // AST_H_
