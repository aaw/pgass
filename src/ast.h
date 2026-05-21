#ifndef AST_H_
#define AST_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct Head {
  virtual ~Head() = default;
};

struct Term {
  enum Kind {
    PredicateKind,
    NumberKind,
    StringKind,
    VariableKind,
    AnonymousVariableKind,
    NegatedTermKind,
    TermOperationKind
  };
  Kind kind;
  virtual ~Term() = default;

 protected:
  Term(Kind k) : kind(k) {}
};

using Terms = std::unique_ptr<std::vector<std::unique_ptr<Term>>>;

struct BodyItem {
  enum Kind { NafLiteralKind, AggregateKind };
  Kind kind;
  virtual ~BodyItem() = default;

 protected:
  BodyItem(Kind k) : kind(k) {}
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

 protected:
  Literal(Kind k) : kind(k) {}
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
};

using NafLiterals = std::unique_ptr<std::vector<std::unique_ptr<NafLiteral>>>;

struct AggregateElement {
  Terms terms;
  NafLiterals literals;

  AggregateElement(Terms t, NafLiterals l)
      : terms(std::move(t)), literals(std::move(l)) {}
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
};

struct BuiltinAtom : Literal {
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;
  BinopType op;

  BuiltinAtom() : Literal(BuiltinAtomKind), op(BinopType::kEQUAL) {}
};

struct ClassicalLiteral : Literal {
  bool negated;
  std::string id;
  Terms args;

  ClassicalLiteral() : Literal(ClassicalLiteralKind), negated(false) {}
};

struct Disjunction : Head {
  std::vector<std::unique_ptr<ClassicalLiteral>> literals;
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
};

struct Query {
  std::unique_ptr<ClassicalLiteral> lit;
};

struct Program {
  Statements statements;
  std::unique_ptr<Query> query;
};

struct Predicate : Term {
  std::string name;
  Terms args;

  Predicate(std::string name, Terms args)
      : Term(PredicateKind), name(std::move(name)), args(std::move(args)) {}
};

struct Number : Term {
  std::uint64_t value;  // TODO: use an unlimited precision bignum

  Number(std::uint64_t value) : Term(NumberKind), value(value) {}
};

struct String : Term {
  std::string value;

  String(std::string_view value)
      : Term(StringKind), value(std::string(value)) {}
};

struct Variable : Term {
  std::string name;

  Variable(std::string_view name)
      : Term(VariableKind), name(std::string(name)) {}
};

struct AnonymousVariable : Term {
  AnonymousVariable() : Term(AnonymousVariableKind) {}
};

struct NegatedTerm : Term {
  std::unique_ptr<Term> term;

  NegatedTerm(std::unique_ptr<Term> term)
      : Term(NegatedTermKind), term(std::move(term)) {}
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
};

#endif  // AST_H_
