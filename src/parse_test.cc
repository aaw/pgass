#include "parse.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "test_macros.h"
#include "tokenize.h"

using namespace ::testing;

namespace {

class ParserTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

TEST_F(ParserTest, LexEmpty) {
  Lexer lexer("");
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, LexerIgnoresWhitespaceAndComments) {
  Lexer lexer(
      "   \n  \t <> \n % here's a comment\n %* here's a \n multiline \n "
      "comment *%");
  EXPECT_EQ(lexer.next().type, TokenType::kUNEQUAL);
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, LexString) {
  Lexer lexer("   \"hello, \\\"world\\\"!\"  ");
  EXPECT_THAT(lexer.next(),
              AllOf(Field(&Token::type, Eq(TokenType::kSTRING)),
                    Field(&Token::val, Eq("\"hello, \\\"world\\\"!\""))));
}

TEST_F(ParserTest, LexStringUnterminated) {
  Lexer lexer("   \"hello, world!  ");
  EXPECT_EQ(lexer.next().type, TokenType::kERROR);
}

TEST_F(ParserTest, LexMultiCharIds) {
  Lexer lexer("   a hello X Name 0 100 123456  ");
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kID)),
                                  Field(&Token::val, Eq("a"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kID)),
                                  Field(&Token::val, Eq("hello"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kVARIABLE)),
                                  Field(&Token::val, Eq("X"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kVARIABLE)),
                                  Field(&Token::val, Eq("Name"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)),
                                  Field(&Token::val, Eq("0"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)),
                                  Field(&Token::val, Eq("100"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)),
                                  Field(&Token::val, Eq("123456"))));
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, ParseSingleTermId) {
  Parser parser("p");
  auto term_status = parser.parse_single_term();
  ASSERT_OK(term_status);
  auto term = std::move(*term_status);
  auto* atom = dynamic_cast<Atom*>(term.get());
  ASSERT_NE(atom, nullptr);
  EXPECT_EQ(atom->name, "p");
  EXPECT_EQ(atom->args, nullptr);
}

TEST_F(ParserTest, ParseSingleTermNumber) {
  Parser parser("123");
  auto term_status = parser.parse_single_term();
  ASSERT_OK(term_status);
  auto term = std::move(*term_status);
  auto* num = dynamic_cast<Number*>(term.get());
  ASSERT_NE(num, nullptr);
  EXPECT_EQ(num->value, 123);
}

TEST_F(ParserTest, ParseTerms) {
  Parser parser("X, 456, \"hello\"");
  auto terms_status = parser.parse_terms();
  ASSERT_OK(terms_status);
  auto terms = std::move(*terms_status);
  ASSERT_EQ(terms->size(), 3);

  EXPECT_NE(dynamic_cast<Variable*>(terms->at(0).get()), nullptr);
  EXPECT_EQ(dynamic_cast<Variable*>(terms->at(0).get())->name, "X");

  EXPECT_NE(dynamic_cast<Number*>(terms->at(1).get()), nullptr);
  EXPECT_EQ(dynamic_cast<Number*>(terms->at(1).get())->value, 456);

  EXPECT_NE(dynamic_cast<String*>(terms->at(2).get()), nullptr);
  EXPECT_EQ(dynamic_cast<String*>(terms->at(2).get())->value, "hello");
}

TEST_F(ParserTest, ParseTermArithmeticLeftAssociative) {
  // 1-2-3 must parse as (1-2)-3, not 1-(2-3).
  Parser parser("1-2-3");
  auto term_status = parser.parse_term();
  ASSERT_OK(term_status);
  auto* outer = dynamic_cast<TermOperation*>(term_status->get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, OperationType::kMINUS);
  // left child must itself be (1-2)
  auto* inner = dynamic_cast<TermOperation*>(outer->left.get());
  ASSERT_NE(inner, nullptr) << "expected left-associative grouping (1-2)-3";
  EXPECT_EQ(inner->op, OperationType::kMINUS);
  EXPECT_EQ(dynamic_cast<Number*>(inner->left.get())->value, 1);
  EXPECT_EQ(dynamic_cast<Number*>(inner->right.get())->value, 2);
  // right child is 3
  EXPECT_EQ(dynamic_cast<Number*>(outer->right.get())->value, 3);
}

TEST_F(ParserTest, ParseTermMultiplicationBindsTighterThanAddition) {
  // 1+2*3 must parse as 1+(2*3).
  Parser parser("1+2*3");
  auto term_status = parser.parse_term();
  ASSERT_OK(term_status);
  auto* outer = dynamic_cast<TermOperation*>(term_status->get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, OperationType::kPLUS);
  EXPECT_EQ(dynamic_cast<Number*>(outer->left.get())->value, 1);

  auto* product = dynamic_cast<TermOperation*>(outer->right.get());
  ASSERT_NE(product, nullptr) << "expected 2*3 to group under the +";
  EXPECT_EQ(product->op, OperationType::kTIMES);
  EXPECT_EQ(dynamic_cast<Number*>(product->left.get())->value, 2);
  EXPECT_EQ(dynamic_cast<Number*>(product->right.get())->value, 3);
}

TEST_F(ParserTest, ParseTermDivisionBindsTighterThanSubtraction) {
  // 6/3-1 must parse as (6/3)-1.
  Parser parser("6/3-1");
  auto term_status = parser.parse_term();
  ASSERT_OK(term_status);
  auto* outer = dynamic_cast<TermOperation*>(term_status->get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, OperationType::kMINUS);
  EXPECT_EQ(dynamic_cast<Number*>(outer->right.get())->value, 1);

  auto* quotient = dynamic_cast<TermOperation*>(outer->left.get());
  ASSERT_NE(quotient, nullptr) << "expected 6/3 to group under the -";
  EXPECT_EQ(quotient->op, OperationType::kDIV);
  EXPECT_EQ(dynamic_cast<Number*>(quotient->left.get())->value, 6);
  EXPECT_EQ(dynamic_cast<Number*>(quotient->right.get())->value, 3);
}

TEST_F(ParserTest, ParseTermMultiplicationLeftAssociative) {
  // 8/4*2 must parse as (8/4)*2, not 8/(4*2).
  Parser parser("8/4*2");
  auto term_status = parser.parse_term();
  ASSERT_OK(term_status);
  auto* outer = dynamic_cast<TermOperation*>(term_status->get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, OperationType::kTIMES);
  auto* inner = dynamic_cast<TermOperation*>(outer->left.get());
  ASSERT_NE(inner, nullptr) << "expected left-associative grouping (8/4)*2";
  EXPECT_EQ(inner->op, OperationType::kDIV);
  EXPECT_EQ(dynamic_cast<Number*>(outer->right.get())->value, 2);
}

TEST_F(ParserTest, ParseTermParenthesesOverridePrecedence) {
  // (1+2)*3 must parse as (1+2)*3.
  Parser parser("(1+2)*3");
  auto term_status = parser.parse_term();
  ASSERT_OK(term_status);
  auto* outer = dynamic_cast<TermOperation*>(term_status->get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->op, OperationType::kTIMES);
  auto* sum = dynamic_cast<TermOperation*>(outer->left.get());
  ASSERT_NE(sum, nullptr);
  EXPECT_EQ(sum->op, OperationType::kPLUS);
  EXPECT_EQ(dynamic_cast<Number*>(outer->right.get())->value, 3);
}

TEST_F(ParserTest, ParseTermsWithArithmetic) {
  // parse_terms() must use parse_term() so arithmetic expressions are accepted.
  Parser parser("1+2, X*3");
  auto terms_status = parser.parse_terms();
  ASSERT_OK(terms_status);
  auto terms = std::move(*terms_status);
  ASSERT_EQ(terms->size(), 2);

  auto* add = dynamic_cast<TermOperation*>(terms->at(0).get());
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->op, OperationType::kPLUS);
  EXPECT_NE(dynamic_cast<Number*>(add->left.get()), nullptr);
  EXPECT_NE(dynamic_cast<Number*>(add->right.get()), nullptr);

  auto* mul = dynamic_cast<TermOperation*>(terms->at(1).get());
  ASSERT_NE(mul, nullptr);
  EXPECT_EQ(mul->op, OperationType::kTIMES);
}

TEST_F(ParserTest, ParseClassicalLiteral) {
  Parser parser("-p(X, 1)");
  auto lit_status = parser.parse_classical_literal();
  ASSERT_OK(lit_status);
  auto lit = std::move(*lit_status);
  EXPECT_TRUE(lit->negated);
  EXPECT_EQ(lit->id, "p");
  ASSERT_NE(lit->args, nullptr);
  ASSERT_EQ(lit->args->size(), 2);
  EXPECT_NE(dynamic_cast<Variable*>(lit->args->at(0).get()), nullptr);
  EXPECT_NE(dynamic_cast<Number*>(lit->args->at(1).get()), nullptr);
}

TEST_F(ParserTest, ParseAggregateElement) {
  {
    Parser parser("X, 1 : p(X), not q");
    auto element_status = parser.parse_aggregate_element();
    ASSERT_OK(element_status);
    auto element = std::move(*element_status);
    ASSERT_NE(element->terms, nullptr);
    ASSERT_EQ(element->terms->size(), 2);
    ASSERT_NE(element->literals, nullptr);
    ASSERT_EQ(element->literals->size(), 2);
    EXPECT_FALSE(element->literals->at(0)->naf);
    EXPECT_TRUE(element->literals->at(1)->naf);
  }

  {
    Parser parser("X");
    auto element_status = parser.parse_aggregate_element();
    ASSERT_OK(element_status);
    auto element = std::move(*element_status);
    ASSERT_NE(element->terms, nullptr);
    ASSERT_EQ(element->terms->size(), 1);
    EXPECT_EQ(element->literals, nullptr);
  }

  {
    Parser parser(": p");
    auto element_status = parser.parse_aggregate_element();
    ASSERT_OK(element_status);
    auto element = std::move(*element_status);
    EXPECT_EQ(element->terms, nullptr);
    ASSERT_NE(element->literals, nullptr);
    ASSERT_EQ(element->literals->size(), 1);
  }
}

TEST_F(ParserTest, ParseHeadDisjunction) {
  Parser parser("p(X) | -q(Y)");
  auto head_status = parser.parse_head();
  ASSERT_OK(head_status);
  auto head = std::move(*head_status);
  auto* disj = dynamic_cast<Disjunction*>(head.get());
  ASSERT_NE(disj, nullptr);
  ASSERT_EQ(disj->literals.size(), 2);
  EXPECT_EQ(disj->literals[0]->id, "p");
  EXPECT_EQ(disj->literals[1]->id, "q");
  EXPECT_TRUE(disj->literals[1]->negated);
}

TEST_F(ParserTest, ParseHeadChoice) {
  {
    Parser parser("{ p(X) : q(X); r } <= 1");
    auto head_status = parser.parse_head();
    ASSERT_OK(head_status);
    auto head = std::move(*head_status);
    auto* choice = dynamic_cast<Choice*>(head.get());
    ASSERT_NE(choice, nullptr);
    ASSERT_EQ(choice->elements->size(), 2);
    EXPECT_EQ(choice->elements->at(0)->literal->id, "p");
    ASSERT_NE(choice->elements->at(0)->conditions, nullptr);
    EXPECT_EQ(choice->elements->at(1)->literal->id, "r");
    EXPECT_EQ(choice->elements->at(1)->conditions, nullptr);
    EXPECT_EQ(choice->ub_op, BinopType::kLESS_OR_EQ);
    ASSERT_NE(choice->ub_term, nullptr);
  }

  {
    Parser parser("1 < { p }");
    auto head_status = parser.parse_head();
    ASSERT_OK(head_status);
    auto head = std::move(*head_status);
    auto* choice = dynamic_cast<Choice*>(head.get());
    ASSERT_NE(choice, nullptr);
    ASSERT_EQ(choice->elements->size(), 1);
    EXPECT_EQ(choice->lb_op, BinopType::kLESS);
    ASSERT_NE(choice->lb_term, nullptr);
  }
}

TEST_F(ParserTest, ParseBodyNafLiteral) {
  Parser parser(":- not p, q.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto& body = (*stmt_status)->body;
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->items->size(), 2);

  auto* naf_lit0 = dynamic_cast<NafLiteral*>(body->items->at(0).get());
  ASSERT_NE(naf_lit0, nullptr);
  EXPECT_TRUE(naf_lit0->naf);
  EXPECT_EQ(dynamic_cast<ClassicalLiteral*>(naf_lit0->literal.get())->id, "p");

  auto* naf_lit1 = dynamic_cast<NafLiteral*>(body->items->at(1).get());
  ASSERT_NE(naf_lit1, nullptr);
  EXPECT_FALSE(naf_lit1->naf);
  EXPECT_EQ(dynamic_cast<ClassicalLiteral*>(naf_lit1->literal.get())->id, "q");
}

TEST_F(ParserTest, ParseBodyNafAggregate) {
  Parser parser(":- not #count{} = 0, #sum{} > 1.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto& body = (*stmt_status)->body;
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body->items->size(), 2);

  auto* agg0 = dynamic_cast<Aggregate*>(body->items->at(0).get());
  ASSERT_NE(agg0, nullptr);
  EXPECT_TRUE(agg0->naf);

  auto* agg1 = dynamic_cast<Aggregate*>(body->items->at(1).get());
  ASSERT_NE(agg1, nullptr);
  EXPECT_FALSE(agg1->naf);
}

// A left guard that starts with an identifier looks like a literal to the
// body item parser, so it has to back up when a binop follows.
TEST_F(ParserTest, ParseAggregateWithConstantLeftGuard) {
  Parser parser("q :- a > #count{ X : p(X) } >= 1.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto& body = (*stmt_status)->body;
  ASSERT_EQ(body->items->size(), 1);

  auto* agg = dynamic_cast<Aggregate*>(body->items->at(0).get());
  ASSERT_NE(agg, nullptr);
  auto* lb = dynamic_cast<Atom*>(agg->lb_term.get());
  ASSERT_NE(lb, nullptr);
  EXPECT_EQ(lb->name, "a");
  EXPECT_EQ(agg->lb_op, BinopType::kGREATER);
  EXPECT_EQ(agg->ub_op, BinopType::kGREATER_OR_EQ);
}

TEST_F(ParserTest, ParseAggregateWithFunctionLeftGuard) {
  Parser parser("q :- f(1) < #count{ X : p(X) }.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto& body = (*stmt_status)->body;
  ASSERT_EQ(body->items->size(), 1);

  auto* agg = dynamic_cast<Aggregate*>(body->items->at(0).get());
  ASSERT_NE(agg, nullptr);
  auto* lb = dynamic_cast<Atom*>(agg->lb_term.get());
  ASSERT_NE(lb, nullptr);
  EXPECT_EQ(lb->name, "f");
  ASSERT_NE(lb->args, nullptr);
  EXPECT_EQ(lb->args->size(), 1);
  EXPECT_EQ(agg->lb_op, BinopType::kLESS);
}

// A comparison of two terms is still a builtin atom, not an aggregate with a
// guard and a missing function.
TEST_F(ParserTest, ParseBodyBuiltinAtomOverConstants) {
  Parser parser("q :- a > b.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto& body = (*stmt_status)->body;
  ASSERT_EQ(body->items->size(), 1);

  auto* naf_lit = dynamic_cast<NafLiteral*>(body->items->at(0).get());
  ASSERT_NE(naf_lit, nullptr);
  EXPECT_NE(dynamic_cast<BuiltinAtom*>(naf_lit->literal.get()), nullptr);
}

// '#count{ }' is the empty set. The grammar lets an aggregate element be
// empty, which would read '{ }' as a set holding one empty tuple and count it.
TEST_F(ParserTest, ParseAggregateWithNoElements) {
  Parser parser("q :- #count{ } >= 0.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto* agg =
      dynamic_cast<Aggregate*>((*stmt_status)->body->items->at(0).get());
  ASSERT_NE(agg, nullptr);
  EXPECT_EQ(agg->elements, nullptr);
}

// An element needs a term or a condition, but not both: '#count{ 1 }' puts the
// tuple 1 in the set whatever else holds.
TEST_F(ParserTest, ParseAggregateElementWithNoCondition) {
  Parser parser("q :- #count{ 1 } >= 1.");
  auto stmt_status = parser.parse_statement();
  ASSERT_OK(stmt_status);
  auto* agg =
      dynamic_cast<Aggregate*>((*stmt_status)->body->items->at(0).get());
  ASSERT_NE(agg, nullptr);
  ASSERT_NE(agg->elements, nullptr);
  ASSERT_EQ(agg->elements->size(), 1);
  EXPECT_NE(agg->elements->at(0)->terms, nullptr);
  EXPECT_EQ(agg->elements->at(0)->literals, nullptr);
}

TEST_F(ParserTest, ParseProgramGraphColoring) {
  Parser parser(R"(
    node(a). node(b). node(c). node(d).
    edge(a,b). edge(b,c). edge(c,d). edge(d,a). edge(a,c).
    color(red). color(green). color(blue).
    1 <= { col(N,C) : color(C) } <= 1 :- node(N).
    :- edge(X,Y), col(X,C), col(Y,C).
  )");
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
}

TEST_F(ParserTest, ParseProgramNQueens) {
  Parser parser(R"(
    n(1). n(2). n(3). n(4).
    1 <= { queen(R,C) : n(C) } <= 1 :- n(R).
    :- queen(R1,C), queen(R2,C), R1 <> R2.
    :- queen(R1,C1), queen(R2,C2), R1 <> R2, R1-C1 = R2-C2.
    :- queen(R1,C1), queen(R2,C2), R1 <> R2, R1+C1 = R2+C2.
  )");
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
}

TEST_F(ParserTest, ParseProgramReachability) {
  Parser parser(R"(
    edge(a,b). edge(b,c). edge(c,d). edge(a,d).
    reachable(X,Y) :- edge(X,Y).
    reachable(X,Z) :- reachable(X,Y), edge(Y,Z).
    connected :- reachable(a,d).
    :- not connected.
  )");
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
}

TEST_F(ParserTest, ParseProgramWithNoStatementsGivesAnEmptyList) {
  for (std::string_view source : {"", "   \n\t ", "% just a comment\n"}) {
    Parser parser(source);
    auto prog = parser.parse_program();
    ASSERT_OK(prog);
    ASSERT_NE((*prog)->statements, nullptr) << "source: " << source;
    EXPECT_TRUE((*prog)->statements->empty());
  }
}

// Error message format tests — each pins the exact string so they double as
// examples of what users will see.

TEST_F(ParserTest, ErrorMessageUnexpectedTermToken) {
  // Simplest case: bad token as the only input.
  Parser parser(".");
  auto result = parser.parse_single_term();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Unexpected token '.' while parsing term\n"
            "line 1, column 1:\n"
            ".\n"
            "^");
}

TEST_F(ParserTest, ErrorMessageCaretPointsMidLine) {
  // After committing to the arithmetic op, the rhs fails — caret must land
  // under the '.' on column 5, not at the start of the expression.
  Parser parser("1 + .");
  auto result = parser.parse_term();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Unexpected token '.' while parsing term\n"
            "line 1, column 5:\n"
            "1 + .\n"
            "    ^");
}

TEST_F(ParserTest, ErrorMessageExpectedIdentifier) {
  Parser parser("123");
  auto result = parser.parse_classical_literal();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Expected identifier but got '123'\n"
            "line 1, column 1:\n"
            "123\n"
            "^");
}

TEST_F(ParserTest, ErrorMessageUnexpectedContentMidLine) {
  // First statement parses fine; caret should sit at the end of 'garbage'
  // (where the '.' was expected), not at the beginning of 'garbage'.
  Parser parser("p(X). garbage");
  auto result = parser.parse_program();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Expected '.' to end rule, got ''\n"
            "line 1, column 14:\n"
            "p(X). garbage\n"
            "             ^");
}

TEST_F(ParserTest, ErrorMessageMultilineShowsCorrectLine) {
  // Error is on the third source line — message must show that line and
  // report line 3, not line 1.
  Parser parser("p(a).\nq(Y).\n123");
  auto result = parser.parse_program();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Unexpected token '123'\n"
            "line 3, column 1:\n"
            "123\n"
            "^");
}

TEST_F(ParserTest, ErrorMessageMissingDotBetweenRules) {
  Parser parser("p(1).\np(2)\np(3).");
  auto result = parser.parse_program();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Expected '.' to end rule, got 'p'\n"
            "line 3, column 1:\n"
            "p(3).\n"
            "^");
}

TEST_F(ParserTest, ErrorMessageMissingDotAtEndOfProgram) {
  Parser parser("p(1). p(X) :- not r(X)");
  auto result = parser.parse_program();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message(),
            "Expected '.' to end rule, got ''\n"
            "line 1, column 23:\n"
            "p(1). p(X) :- not r(X)\n"
            "                      ^");
}

}  // namespace
