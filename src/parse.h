#ifndef PARSE_H_
#define PARSE_H_

#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "ast.h"
#include "tokenize.h"

class Parser {
 public:
  explicit Parser(std::string_view source);

  absl::StatusOr<std::unique_ptr<Program>> parse_program();
  absl::StatusOr<Statements> parse_statements();
  absl::StatusOr<std::unique_ptr<Statement>> parse_statement();
  absl::StatusOr<std::unique_ptr<Term>> parse_basic_term();
  absl::StatusOr<Terms> parse_basic_terms();
  absl::StatusOr<NafLiterals> parse_naf_literals();
  absl::StatusOr<std::unique_ptr<AggregateElement>> parse_aggregate_element();
  absl::StatusOr<AggregateElements> parse_aggregate_elements();
  absl::StatusOr<AggregateFunctionType> parse_aggregate_function();
  absl::StatusOr<std::unique_ptr<Aggregate>> parse_aggregate();
  absl::StatusOr<std::unique_ptr<Body>> parse_body();
  absl::StatusOr<std::unique_ptr<ChoiceElement>> parse_choice_element();
  absl::StatusOr<ChoiceElements> parse_choice_elements();
  absl::StatusOr<std::unique_ptr<Choice>> parse_choice();
  absl::StatusOr<std::unique_ptr<Disjunction>> parse_disjunction();
  absl::StatusOr<std::unique_ptr<Head>> parse_head();
  absl::StatusOr<std::unique_ptr<Weight>> parse_weight();
  absl::StatusOr<std::unique_ptr<NafLiteral>> parse_naf_literal();
  absl::StatusOr<BinopType> parse_binop();
  absl::StatusOr<std::unique_ptr<BuiltinAtom>> parse_builtin_atom();
  absl::StatusOr<std::unique_ptr<Query>> parse_query();
  absl::StatusOr<std::unique_ptr<Term>> parse_term();
  absl::StatusOr<std::unique_ptr<Term>> parse_single_term();
  absl::StatusOr<Terms> parse_terms();
  absl::StatusOr<std::unique_ptr<ClassicalLiteral>> parse_classical_literal();

 private:
  void update_furthest(const absl::Status& s);

  Lexer lexer_;
  size_t furthest_error_pos_ = 0;
  std::string furthest_error_msg_;
};

#endif  // PARSE_H_
