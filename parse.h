#ifndef __PARSE_H__
#define __PARSE_H__

#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "macros.h"
#include "tokenize.h"

struct Statement {};

using Statements = std::vector<std::unique_ptr<Statement>>;

struct Query {};

struct Program {
  Statements statements;
  std::unique_ptr<Query> query;
};

struct Term {
  virtual ~Term() = default;
};

using Terms = std::vector<std::unique_ptr<Term>>;

struct Predicate : Term {
  std::string name;
  Terms args;
};

struct Number : Term {
  std::uint64_t value;  // TODO: use an unlimited precision bignum
};

struct String : Term {
  std::string value;
};

struct Variable : Term {
  std::string name;
};

struct AnonymousVariable : Term {};

struct NegatedTerm : Term {
  std::unique_ptr<Term> term;
};

enum class OperationType { kPLUS, kMINUS, kTIMES, kDIV };

struct TermOperation : Term {
  OperationType op;
  std::unique_ptr<Term> left;
  std::unique_ptr<Term> right;
};

struct ClassicalLiteral {
  bool negated;
  std::string id;
  Terms args;
};

class Parser {
 public:
  explicit Parser(std::string_view source) : lexer_(source) {}

  // <program> ::= [<statements>] [<query>]
  absl::StatusOr<std::unique_ptr<Program>> parse_program() {
    auto checkpoint = lexer_.checkpoint();
    ASSIGN_OR_RETURN(Statements statements, parse_statements());
    if (statements.empty()) lexer_.rewind(checkpoint);

    checkpoint = lexer_.checkpoint();
    ASSIGN_OR_RETURN(std::unique_ptr<Query> query, parse_query());
    if (query == nullptr) lexer_.rewind(checkpoint);

    if (lexer_.next().type != TokenType::kEOF) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Unexpected content at end of program: ", lexer_.report_pos()));
    }

    return absl::make_unique<Program>(
        Program{std::move(statements), std::move(query)});
  }

 private:
  // <statements> ::= [<statements>] <statement>
  absl::StatusOr<Statements> parse_statements() { return absl::OkStatus(); }

  // <query> ::= <classical_literal> QUERY_MARK
  absl::StatusOr<std::unique_ptr<Query>> parse_query() {
    LexerCheckpoint checkpoint(lexer_);
    auto query = std::make_unique<Query>();

    checkpoint.commit();
    return query;
  }

  // <terms> ::= [<terms> COMMA] <term>
  absl::StatusOr<Terms> parse_terms() { return absl::OkStatus(); }

  // <classical_literal> ::= [MINUS] ID [PAREN_OPEN [<terms>] PAREN_CLOSE]
  absl::StatusOr<std::unique_ptr<ClassicalLiteral>> parse_classical_literal() {
    LexerCheckpoint checkpoint(lexer_);
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

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kPAREN_OPEN);

    ASSIGN_OR_RETURN(Terms terms, parse_terms());
    lit->args = std::move(terms);

    CONSUME_TOKEN_TYPE_OR_RETURN(lexer_, TokenType::kPAREN_CLOSE);

    checkpoint.commit();
    return lit;
  }

  Lexer lexer_;
};

#endif  // __PARSE_H__
