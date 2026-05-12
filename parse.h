#ifndef __PARSE_H__
#define __PARSE_H__

#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "macros.h"
#include "tokenize.h"

struct Head {};

struct Body {};

struct Weight {};

struct Statement {
  std::unique_ptr<Head> head;
  std::unique_ptr<Body> body;
  std::unique_ptr<Weight> weight;
};

using Statements = std::unique_ptr<std::vector<std::unique_ptr<Statement>>>;

struct Term {
  virtual ~Term() = default;
};

using Terms = std::unique_ptr<std::vector<std::unique_ptr<Term>>>;

struct ClassicalLiteral {
  bool negated;
  std::string id;
  Terms args;
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

  String(std::string_view value) : value(value) {}
};

struct Variable : Term {
  std::string name;

  Variable(std::string_view name) : name(name) {}
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

 private:
  // <statements> ::= [<statements>] <statement>
  absl::StatusOr<Statements> parse_statements() {
    Statements statements;
    ASSIGN_OR_RETURN(auto statement, parse_statement());
    statements->push_back(std::move(statement));

    while (true) {
      LexerCheckpoint try_next_statement(lexer_);
      auto next_statement = parse_statement();
      if (next_statement.ok()) {
        try_next_statement.commit();
        statements->push_back(std::move(*next_statement));
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

    // Otherwise, we start with a <head>.
    ASSIGN_OR_RETURN(statement->head, parse_head());
    // TODO [CONS [<body>]]
    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kDOT);

    return statement;
  }

  // <body> ::= [<body> COMMA] (<naf_literal> | [NAF] <aggregate>)
  absl::StatusOr<std::unique_ptr<Body>> parse_body() {
    // TODO
    return std::make_unique<Body>();
  }

  // <head> ::= <disjunction> | <choice>
  absl::StatusOr<std::unique_ptr<Head>> parse_head() {
    // TODO
    return std::make_unique<Head>();
  }

  // <weight_at_level> ::= <term> [AT <term>] [COMMA <terms>]
  absl::StatusOr<std::unique_ptr<Weight>> parse_weight() {
    // TODO
    return std::make_unique<Weight>();
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
    // We handle the left-recursive "<term> <arithop> <term>" production
    // iteratively here, falling back to the simpler single term
    // productions only when we stop seeing a connecting +,-,*, or /.
    LexerCheckpoint checkpoint(lexer_);

    ASSIGN_OR_RETURN(auto lhs, parse_single_term());

    LexerCheckpoint try_arith_op(lexer_);
    Token token = lexer_.next();
    OperationType op;
    bool found_op = true;
    if (token.type == TokenType::kPLUS) {
      op = OperationType::kPLUS;
    } else if (token.type == TokenType::kMINUS) {
      op = OperationType::kMINUS;
    } else if (token.type == TokenType::kTIMES) {
      op = OperationType::kTIMES;
    } else if (token.type == TokenType::kDIV) {
      op = OperationType::kDIV;
    } else {
      found_op = false;
    }

    if (!found_op) {
      checkpoint.commit();
      return lhs;  // Not a term operation, just a term.
    }
    try_arith_op.commit();

    ASSIGN_OR_RETURN(auto rhs, parse_term());

    checkpoint.commit();
    return std::make_unique<TermOperation>(op, std::move(lhs), std::move(rhs));
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
      ASSIGN_OR_RETURN(auto term, parse_single_term());
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
    Terms terms;
    ASSIGN_OR_RETURN(auto first_term, parse_single_term());
    terms->push_back(std::move(first_term));

    while (true) {
      LexerCheckpoint try_comma(lexer_);
      Token token = lexer_.next();
      if (token.type != TokenType::kCOMMA) {
        return terms;
      }
      try_comma.commit();

      ASSIGN_OR_RETURN(auto next_term, parse_single_term());
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
