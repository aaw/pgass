#ifndef AST_H_
#define AST_H_

#include <memory>
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
    TermOperationKind
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

struct Statement {
  std::unique_ptr<Head> head;
  std::unique_ptr<Body> body;
  std::unique_ptr<Weight> weight;
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

#endif  // AST_H_
