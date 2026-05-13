#ifndef __PARSE_H__
#define __PARSE_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "macros.h"
#include "tokenize.h"

// Forward declarations for recursive structures if needed.
struct Head {
  virtual ~Head() = default;
};

struct Term {
  virtual ~Term() = default;
};

using Terms = std::unique_ptr<std::vector<std::unique_ptr<Term>>>;

struct BodyItem {
  virtual ~BodyItem() = default;
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
  virtual ~Literal() = default;
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
};

struct BuiltinAtom : Literal {
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;
  BinopType op;
};

struct ClassicalLiteral : Literal {
  bool negated;
  std::string id;
  Terms args;
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

using ChoiceElements = std::unique_ptr<std::vector<std::unique_ptr<ChoiceElement>>>;

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
      : name(std::move(name)), args(std::move(args)) {}
};

struct Number : Term {
  std::uint64_t value;  // TODO: use an unlimited precision bignum

  Number(std::uint64_t value) : value(value) {}
};

struct String : Term {
  std::string value;

  String(std::string_view value) : value(std::string(value)) {}
};

struct Variable : Term {
  std::string name;

  Variable(std::string_view name) : name(std::string(name)) {}
};

struct AnonymousVariable : Term {};

struct NegatedTerm : Term {
  std::unique_ptr<Term> term;

  NegatedTerm(std::unique_ptr<Term> term) : term(std::move(term)) {}
};

enum class OperationType { kPLUS, kMINUS, kTIMES, kDIV };

struct TermOperation : Term {
  OperationType op;
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;

  TermOperation(OperationType o, std::unique_ptr<Term> l,
                std::unique_ptr<Term> r)
      : op(o), left(std::move(l)), right(std::move(r)) {}
};

class Parser {
 public:
  explicit Parser(std::string_view source) : lexer_(source) {}

  // <program> ::= [<statements>] [<query>]
  absl::StatusOr<std::unique_ptr<Program>> parse_program() {
    auto program = std::make_unique<Program>();

    {
      LexerCheckpoint try_statements(lexer_);
      auto statements = parse_statements();
      if (statements.ok()) {
        try_statements.commit();
        program->statements = std::move(*statements);
      }
    }

    {
      LexerCheckpoint try_query(lexer_);
      auto query = parse_query();
      if (query.ok()) {
        try_query.commit();
        program->query = std::move(*query);
      }
    }

    if (lexer_.next().type != TokenType::kEOF) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Unexpected content at end of program: ", lexer_.report_pos()));
    }

    return program;
  }

  // <statements> ::= [<statements>] <statement>
  absl::StatusOr<Statements> parse_statements() {
    auto statements = std::make_unique<std::vector<std::unique_ptr<Statement>>>();
    ASSIGN_OR_RETURN(auto statement, parse_statement());
    statements->push_back(std::move(statement));

    while (true) {
      LexerCheckpoint try_next_statement(lexer_);
      auto next_statement = parse_statement();
      if (next_statement.ok()) {
        try_next_statement.commit();
        statements->push_back(std::move(*next_statement));
      } else {
        break;
      }
    }

    return statements;
  }

  /* <statement> ::= CONS [<body>] DOT
                   | <head> [CONS [<body>]] DOT
                   | WCONS [<body>] DOT SQUARE_OPEN <weight_at_level>
     SQUARE_CLOSE
  */
  absl::StatusOr<std::unique_ptr<Statement>> parse_statement() {
    auto statement = std::make_unique<Statement>();

    // First, try to parse the head-less productions that start with CONS/WCONS.
    {
      LexerCheckpoint try_no_head(lexer_);
      Token tok = lexer_.next();
      if (tok.type == TokenType::kCONS || tok.type == TokenType::kWCONS) {
        try_no_head.commit();

        {
          LexerCheckpoint try_body(lexer_);
          auto body = parse_body();
          if (body.ok()) {
            try_body.commit();
            statement->body = std::move(*body);
          }
        }

        CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kDOT);
        if (tok.type == TokenType::kCONS) return statement;

        CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kSQUARE_OPEN);
        ASSIGN_OR_RETURN(statement->weight, parse_weight());
        CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kSQUARE_CLOSE);
        return statement;
      }
    }

    // Otherwise, we need to parse <head> [CONS [<body>]] DOT
    ASSIGN_OR_RETURN(statement->head, parse_head());

    {
      LexerCheckpoint try_cons(lexer_);
      Token tok = lexer_.next();
      if (tok.type == TokenType::kCONS) {
        try_cons.commit();
        {
          LexerCheckpoint try_body(lexer_);
          auto body = parse_body();
          if (body.ok()) {
            try_body.commit();
            statement->body = std::move(*body);
          }
        }
      }
    }

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kDOT);

    return statement;
  }

  // <basic_term> ::= <ground_term> | <variable_term>
  // <ground_term> ::= SYMBOLIC_CONSTANT | STRING | [MINUS] NUMBER
  // <variable_term> ::= VARIABLE | ANONYMOUS_VARIABLE
  absl::StatusOr<std::unique_ptr<Term>> parse_basic_term() {
    Token token = lexer_.next();
    if (token.type == TokenType::kID) {
      return std::make_unique<Predicate>(std::string(token.val), nullptr);
    } else if (token.type == TokenType::kSTRING) {
      return std::make_unique<String>(token.val);
    } else if (token.type == TokenType::kNUMBER) {
      uint64_t number;
      if (!absl::SimpleAtoi(token.val, &number)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Couldn't convert number ", token.val,
            " to 64-bit unsigned integer (", lexer_.report_pos(), ")"));
      }
      return std::make_unique<Number>(number);
    } else if (token.type == TokenType::kMINUS) {
      CONSUME_TOKEN_OR_RETURN(lexer_, num_tok, TokenType::kNUMBER);
      uint64_t number;
      if (!absl::SimpleAtoi(num_tok.val, &number)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Couldn't convert number ", num_tok.val,
            " to 64-bit unsigned integer (", lexer_.report_pos(), ")"));
      }
      return std::make_unique<NegatedTerm>(std::make_unique<Number>(number));
    } else if (token.type == TokenType::kVARIABLE) {
      return std::make_unique<Variable>(token.val);
    } else if (token.type == TokenType::kANONYMOUS_VARIABLE) {
      return std::make_unique<AnonymousVariable>();
    }

    return absl::InvalidArgumentError(absl::StrCat(
        "Unexpected token '", token.val,
        "' while attempting to parse basic term at ", lexer_.report_pos()));
  }

  // <basic_terms> ::= [<basic_terms> COMMA] <basic_term>
  absl::StatusOr<Terms> parse_basic_terms() {
    auto terms = std::make_unique<std::vector<std::unique_ptr<Term>>>();
    ASSIGN_OR_RETURN(auto first_term, parse_basic_term());
    terms->push_back(std::move(first_term));

    while (true) {
      LexerCheckpoint try_comma(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kCOMMA) {
        return terms;
      }
      try_comma.commit();

      ASSIGN_OR_RETURN(auto next_term, parse_basic_term());
      terms->push_back(std::move(next_term));
    }

    return terms;
  }

  // <naf_literals> ::= [<naf_literals> COMMA] <naf_literal>
  absl::StatusOr<NafLiterals> parse_naf_literals() {
    auto literals =
        std::make_unique<std::vector<std::unique_ptr<NafLiteral>>>();
    ASSIGN_OR_RETURN(auto first_literal, parse_naf_literal());
    literals->push_back(std::move(first_literal));

    while (true) {
      LexerCheckpoint try_comma(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kCOMMA) {
        return literals;
      }
      try_comma.commit();

      ASSIGN_OR_RETURN(auto next_literal, parse_naf_literal());
      literals->push_back(std::move(next_literal));
    }

    return literals;
  }

  // <aggregate_element> ::= [<basic_terms>] [COLON [<naf_literals>]]
  absl::StatusOr<std::unique_ptr<AggregateElement>> parse_aggregate_element() {
    Terms terms;
    {
      LexerCheckpoint try_basic_terms(lexer_);
      auto basic_terms = parse_basic_terms();
      if (basic_terms.ok()) {
        try_basic_terms.commit();
        terms = std::move(*basic_terms);
      }
    }

    NafLiterals literals;
    {
      LexerCheckpoint try_colon(lexer_);
      if (lexer_.next().type == TokenType::kCOLON) {
        try_colon.commit();
        {
          LexerCheckpoint try_naf_literals(lexer_);
          auto naf_literals = parse_naf_literals();
          if (naf_literals.ok()) {
            try_naf_literals.commit();
            literals = std::move(*naf_literals);
          }
        }
      }
    }

    return std::make_unique<AggregateElement>(std::move(terms),
                                              std::move(literals));
  }

  /* <aggregate_elements> ::= [<aggregate_elements> SEMICOLON]
                                <aggregate_element>
  */
  absl::StatusOr<AggregateElements> parse_aggregate_elements() {
    auto elements = std::make_unique<std::vector<std::unique_ptr<AggregateElement>>>();
    ASSIGN_OR_RETURN(auto element, parse_aggregate_element());
    elements->push_back(std::move(element));

    while (true) {
      LexerCheckpoint try_next_element(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kSEMICOLON) {
        break;
      }
      auto next_element = parse_aggregate_element();
      if (next_element.ok()) {
        try_next_element.commit();
        elements->push_back(std::move(*next_element));
      } else {
        break;
      }
    }

    return elements;
  }

  /* <aggregate_function> ::= AGGREGATE_COUNT
                            | AGGREGATE_MAX
                            | AGGREGATE_MIN
                            | AGGREGATE_SUM
  */
  absl::StatusOr<AggregateFunctionType> parse_aggregate_function() {
    Token token = lexer_.next();
    if (token.type == TokenType::kAGGREGATE_COUNT) {
      return AggregateFunctionType::kAGGREGATE_COUNT;
    } else if (token.type == TokenType::kAGGREGATE_MAX) {
      return AggregateFunctionType::kAGGREGATE_MAX;
    } else if (token.type == TokenType::kAGGREGATE_MIN) {
      return AggregateFunctionType::kAGGREGATE_MIN;
    } else if (token.type == TokenType::kAGGREGATE_SUM) {
      return AggregateFunctionType::kAGGREGATE_SUM;
    } else {
      return absl::InvalidArgumentError(absl::StrCat(
          "Expected aggregate function (#count, #max, #min, #sum), got '",
          token.val, "' at ", lexer_.report_pos()));
    }
  }

  /* <aggregate> ::= [<term> <binop>] <aggregate_function>
                       CURLY_OPEN [<aggregate_elements>]
                       CURLY_CLOSE [<binop> <term>]
  */
  absl::StatusOr<std::unique_ptr<Aggregate>> parse_aggregate() {
    auto aggregate = std::make_unique<Aggregate>();

    {
      // Parse the lower bound, if one exists.
      LexerCheckpoint try_term_and_binop(lexer_);
      auto term = parse_term();
      auto binop = parse_binop();
      if (term.ok() && binop.ok()) {
        aggregate->lb_term = std::move(*term);
        aggregate->lb_op = std::move(*binop);
        try_term_and_binop.commit();
      }
    }

    ASSIGN_OR_RETURN(aggregate->function, parse_aggregate_function());
    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kCURLY_OPEN);

    {
      LexerCheckpoint try_aggregate_elements(lexer_);
      auto aggregate_elements = parse_aggregate_elements();
      if (aggregate_elements.ok()) {
        aggregate->elements = std::move(*aggregate_elements);
        try_aggregate_elements.commit();
      }
    }

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kCURLY_CLOSE);

    {
      // Parse the upper bound, if one exists.
      LexerCheckpoint try_term_and_binop(lexer_);
      auto binop = parse_binop();
      auto term = parse_term();
      if (term.ok() && binop.ok()) {
        aggregate->ub_term = std::move(*term);
        aggregate->ub_op = std::move(*binop);
        try_term_and_binop.commit();
      }
    }

    return aggregate;
  }

  // <body> ::= [<body> COMMA] (<naf_literal> | [NAF] <aggregate>)
  absl::StatusOr<std::unique_ptr<Body>> parse_body() {
    auto body = std::make_unique<Body>();
    body->items =
        std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();

    while (true) {
      bool found_naf_literal = false;
      {
        LexerCheckpoint try_naf_literal(lexer_);
        auto naf_literal = parse_naf_literal();
        if (naf_literal.ok()) {
          try_naf_literal.commit();
          found_naf_literal = true;
          body->items->push_back(std::move(*naf_literal));
        }
      }

      // Otherwise, it's a [NAF] <aggregate>.
      if (!found_naf_literal) {
        bool naf = false;
        {
          LexerCheckpoint try_naf(lexer_);
          Token token = lexer_.next();
          if (token.type == TokenType::kNAF) {
            try_naf.commit();
            naf = true;
          }
        }
        ASSIGN_OR_RETURN(auto aggregate, parse_aggregate());
        aggregate->naf = naf;
        body->items->push_back(std::move(aggregate));
      }

      LexerCheckpoint try_comma(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kCOMMA) {
        return body;
      }
      try_comma.commit();
    }

    return body;
  }

  // <choice_element> ::= <classical_literal> [COLON [<naf_literals>]]
  absl::StatusOr<std::unique_ptr<ChoiceElement>> parse_choice_element() {
    ASSIGN_OR_RETURN(auto literal, parse_classical_literal());

    NafLiterals conditions;
    {
      LexerCheckpoint try_colon(lexer_);
      if (lexer_.next().type == TokenType::kCOLON) {
        try_colon.commit();
        {
          LexerCheckpoint try_naf_literals(lexer_);
          auto naf_literals = parse_naf_literals();
          if (naf_literals.ok()) {
            try_naf_literals.commit();
            conditions = std::move(*naf_literals);
          }
        }
      }
    }

    return std::make_unique<ChoiceElement>(std::move(literal),
                                           std::move(conditions));
  }

  // <choice_elements> ::= [<choice_elements> SEMICOLON] <choice_element>
  absl::StatusOr<ChoiceElements>
  parse_choice_elements() {
    auto elements = std::make_unique<std::vector<std::unique_ptr<ChoiceElement>>>();
    ASSIGN_OR_RETURN(auto element, parse_choice_element());
    elements->push_back(std::move(element));

    while (true) {
      LexerCheckpoint try_next_element(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kSEMICOLON) {
        break;
      }
      auto next_element = parse_choice_element();
      if (next_element.ok()) {
        try_next_element.commit();
        elements->push_back(std::move(*next_element));
      } else {
        break;
      }
    }

    return elements;
  }

  // <choice> ::= [<term> <binop>] CURLY_OPEN [<choice_elements>] CURLY_CLOSE [<binop> <term>]
  absl::StatusOr<std::unique_ptr<Choice>> parse_choice() {
    auto choice = std::make_unique<Choice>();

    {
      // Parse the lower bound, if one exists.
      LexerCheckpoint try_term_and_binop(lexer_);
      auto term = parse_term();
      auto binop = parse_binop();
      if (term.ok() && binop.ok()) {
        choice->lb_term = std::move(*term);
        choice->lb_op = std::move(*binop);
        try_term_and_binop.commit();
      }
    }

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kCURLY_OPEN);

    {
      LexerCheckpoint try_choice_elements(lexer_);
      auto choice_elements = parse_choice_elements();
      if (choice_elements.ok()) {
        choice->elements = std::move(*choice_elements);
        try_choice_elements.commit();
      }
    }

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kCURLY_CLOSE);

    {
      // Parse the upper bound, if one exists.
      LexerCheckpoint try_term_and_binop(lexer_);
      auto binop = parse_binop();
      auto term = parse_term();
      if (term.ok() && binop.ok()) {
        choice->ub_term = std::move(*term);
        choice->ub_op = std::move(*binop);
        try_term_and_binop.commit();
      }
    }

    return choice;
  }

  // <disjunction> ::= [<disjunction> OR] <classical_literal>
  absl::StatusOr<std::unique_ptr<Disjunction>> parse_disjunction() {
    auto disjunction = std::make_unique<Disjunction>();
    ASSIGN_OR_RETURN(auto literal, parse_classical_literal());
    disjunction->literals.push_back(std::move(literal));

    while (true) {
      LexerCheckpoint try_or(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kOR) {
        break;
      }
      try_or.commit();

      ASSIGN_OR_RETURN(auto next_literal, parse_classical_literal());
      disjunction->literals.push_back(std::move(next_literal));
    }

    return disjunction;
  }

  // <head> ::= <disjunction> | <choice>
  absl::StatusOr<std::unique_ptr<Head>> parse_head() {
    {
      LexerCheckpoint try_choice(lexer_);
      auto choice = parse_choice();
      if (choice.ok()) {
        try_choice.commit();
        return std::move(*choice);
      }
    }

    auto disjunction = parse_disjunction();
    if (!disjunction.ok()) return disjunction.status();
    return std::move(*disjunction);
  }

  // <weight_at_level> ::= <term> [AT <term>] [COMMA <terms>]
  absl::StatusOr<std::unique_ptr<Weight>> parse_weight() {
    ASSIGN_OR_RETURN(auto weight_term, parse_term());

    std::unique_ptr<Term> level;
    {
      LexerCheckpoint try_at(lexer_);
      if (lexer_.next().type == TokenType::kAT) {
        try_at.commit();
        ASSIGN_OR_RETURN(level, parse_term());
      }
    }

    Terms terms;
    {
      LexerCheckpoint try_comma(lexer_);
      if (lexer_.next().type == TokenType::kCOMMA) {
        try_comma.commit();
        ASSIGN_OR_RETURN(terms, parse_terms());
      }
    }

    return std::make_unique<Weight>(std::move(weight_term), std::move(level),
                                     std::move(terms));
  }

  // <naf_literal> ::= [NAF] <classical_literal> | <builtin_atom>
  absl::StatusOr<std::unique_ptr<NafLiteral>> parse_naf_literal() {
    // Try builtin_atom before classical_literal: both can start with
    // "ID [PAREN_OPEN <terms> PAREN_CLOSE]", and builtin_atom is the longer
    // production. NAF must not be consumed first — it is only valid before
    // classical_literal, not builtin_atom.
    {
      LexerCheckpoint try_builtin_atom(lexer_);
      auto builtin_atom = parse_builtin_atom();
      if (builtin_atom.ok()) {
        try_builtin_atom.commit();
        auto literal = std::make_unique<NafLiteral>();
        literal->naf = false;
        literal->literal = std::move(*builtin_atom);
        return literal;
      }
    }

    auto literal = std::make_unique<NafLiteral>();
    {
      LexerCheckpoint try_naf(lexer_);
      if (lexer_.next().type == TokenType::kNAF) {
        try_naf.commit();
        literal->naf = true;
      }
    }
    ASSIGN_OR_RETURN(literal->literal, parse_classical_literal());
    return literal;
  }

  /* <binop> ::= EQUAL
               | UNEQUAL
               | LESS
               | GREATER
               | LESS_OR_EQ
               | GREATER_OR_EQ
  */
  absl::StatusOr<BinopType> parse_binop() {
    Token token = lexer_.next();
    if (token.type == TokenType::kEQUAL) {
      return BinopType::kEQUAL;
    } else if (token.type == TokenType::kUNEQUAL) {
      return BinopType::kUNEQUAL;
    } else if (token.type == TokenType::kLESS) {
      return BinopType::kLESS;
    } else if (token.type == TokenType::kGREATER) {
      return BinopType::kGREATER;
    } else if (token.type == TokenType::kLESS_OR_EQ) {
      return BinopType::kLESS_OR_EQ;
    } else if (token.type == TokenType::kGREATER_OR_EQ) {
      return BinopType::kGREATER_OR_EQ;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Expected operator (=, <>, !=, <, >, <=, >=), got '",
                       token.val, "' at ", lexer_.report_pos()));
    }
  }

  // <builtin_atom> ::= <term> <binop> <term>
  absl::StatusOr<std::unique_ptr<BuiltinAtom>> parse_builtin_atom() {
    auto atom = std::make_unique<BuiltinAtom>();
    ASSIGN_OR_RETURN(atom->left, parse_term());
    ASSIGN_OR_RETURN(atom->op, parse_binop());
    ASSIGN_OR_RETURN(atom->right, parse_term());
    return atom;
  }

  // <query> ::= <classical_literal> QUERY_MARK
  absl::StatusOr<std::unique_ptr<Query>> parse_query() {
    auto query = std::make_unique<Query>();
    ASSIGN_OR_RETURN(query->lit, parse_classical_literal());
    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kQUERY_MARK);
    return query;
  }

  /* <term> ::= ID [PAREN_OPEN [<terms>] PAREN_CLOSE]
              | NUMBER
              | STRING
              | VARIABLE
              | ANONYMOUS_VARIABLE
              | PAREN_OPEN <term> PAREN_CLOSE
              | MINUS <term>
              | <term> <arithop> <term>
  */
  absl::StatusOr<std::unique_ptr<Term>> parse_term() {
    ASSIGN_OR_RETURN(auto lhs, parse_single_term());

    while (true) {
      LexerCheckpoint try_arith_op(lexer_);
      Token token = lexer_.next();
      OperationType op;
      if (token.type == TokenType::kPLUS) {
        op = OperationType::kPLUS;
      } else if (token.type == TokenType::kMINUS) {
        op = OperationType::kMINUS;
      } else if (token.type == TokenType::kTIMES) {
        op = OperationType::kTIMES;
      } else if (token.type == TokenType::kDIV) {
        op = OperationType::kDIV;
      } else {
        return lhs;
      }
      try_arith_op.commit();

      ASSIGN_OR_RETURN(auto rhs, parse_single_term());
      lhs = std::make_unique<TermOperation>(op, std::move(lhs), std::move(rhs));
    }
  }

  /* <term> ::= ID [PAREN_OPEN [<terms>] PAREN_CLOSE]
              | NUMBER
              | STRING
              | VARIABLE
              | ANONYMOUS_VARIABLE
              | PAREN_OPEN <term> PAREN_CLOSE
              | MINUS <term>
              | <term> <arithop> <term>
  */
  absl::StatusOr<std::unique_ptr<Term>> parse_single_term() {
    Token token = lexer_.next();
    if (token.type == TokenType::kID) {
      LexerCheckpoint try_paren(lexer_);
      if (lexer_.next().type == TokenType::kPAREN_OPEN) {
        try_paren.commit();

        Terms terms;
        {
          LexerCheckpoint try_args(lexer_);
          auto terms_status = parse_terms();
          if (terms_status.ok()) {
            try_args.commit();
            terms = std::move(*terms_status);
          }
        }

        CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kPAREN_CLOSE);
        return std::make_unique<Predicate>(std::string(token.val),
                                           std::move(terms));
      }
      return std::make_unique<Predicate>(std::string(token.val), nullptr);
    } else if (token.type == TokenType::kNUMBER) {
      uint64_t number;
      if (!absl::SimpleAtoi(token.val, &number)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Couldn't convert number ", token.val,
            " to 64-bit unsigned integer (", lexer_.report_pos(), ")"));
      }
      return std::make_unique<Number>(number);
    } else if (token.type == TokenType::kSTRING) {
      return std::make_unique<String>(token.val);
    } else if (token.type == TokenType::kVARIABLE) {
      return std::make_unique<Variable>(token.val);
    } else if (token.type == TokenType::kANONYMOUS_VARIABLE) {
      return std::make_unique<AnonymousVariable>();
    } else if (token.type == TokenType::kPAREN_OPEN) {
      ASSIGN_OR_RETURN(auto term, parse_term());
      CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kPAREN_CLOSE);
      return term;
    } else if (token.type == TokenType::kMINUS) {
      ASSIGN_OR_RETURN(auto term, parse_single_term());
      return std::make_unique<NegatedTerm>(std::move(term));
    }

    return absl::InvalidArgumentError(absl::StrCat(
        "Unexpected token '", token.val, "' while attempting to parse term at ",
        lexer_.report_pos()));
  }

  // <terms> ::= [<terms> COMMA] <term>
  absl::StatusOr<Terms> parse_terms() {
    auto terms = std::make_unique<std::vector<std::unique_ptr<Term>>>();
    ASSIGN_OR_RETURN(auto first_term, parse_term());
    terms->push_back(std::move(first_term));

    while (true) {
      LexerCheckpoint try_comma(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kCOMMA) {
        return terms;
      }
      try_comma.commit();

      ASSIGN_OR_RETURN(auto next_term, parse_term());
      terms->push_back(std::move(next_term));
    }

    return terms;
  }

  // <classical_literal> ::= [MINUS] ID [PAREN_OPEN [<terms>] PAREN_CLOSE]
  absl::StatusOr<std::unique_ptr<ClassicalLiteral>> parse_classical_literal() {
    auto lit = std::make_unique<ClassicalLiteral>();

    Token token = lexer_.next();
    if (token.type == TokenType::kMINUS) {
      lit->negated = true;
      token = lexer_.next();
    }

    if (token.type != TokenType::kID) {
      return absl::InvalidArgumentError(
          absl::StrCat("Expected ID at ", lexer_.report_pos(), " but got '",
                       token.val, "'"));
    }
    lit->id = token.val;

    LexerCheckpoint try_open_paren(lexer_);
    token = lexer_.next();
    if (token.type == TokenType::kPAREN_OPEN) {
      try_open_paren.commit();

      {
        LexerCheckpoint try_terms(lexer_);
        auto terms = parse_terms();
        if (terms.ok()) {
          try_terms.commit();
          lit->args = std::move(*terms);
        }
      }

      CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kPAREN_CLOSE);
    }

    return lit;
  }

  Lexer lexer_;
};

#endif  // __PARSE_H__
