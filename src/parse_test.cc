#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "parse.h"
#include "tokenize.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

using namespace ::testing;

namespace {

class ParserTest : public ::testing::Test {
protected:
  void SetUp() override { }
  void TearDown() override { }
};

TEST_F(ParserTest, LexEmpty) {
  Lexer lexer("");
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, LexerIgnoresWhitespaceAndComments) {
  Lexer lexer("   \n  \t <> \n % here's a comment\n %* here's a \n multiline \n comment *%");
  EXPECT_EQ(lexer.next().type, TokenType::kUNEQUAL);
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, LexString) {
  Lexer lexer("   \"hello, \\\"world\\\"!\"  ");
  EXPECT_THAT(lexer.next(), AllOf(
    Field(&Token::type, Eq(TokenType::kSTRING)),
    Field(&Token::val, Eq("\"hello, \\\"world\\\"!\""))));
}

TEST_F(ParserTest, LexStringUnterminated) {
  Lexer lexer("   \"hello, world!  ");
  EXPECT_EQ(lexer.next().type, TokenType::kERROR);
}

TEST_F(ParserTest, LexMultiCharIds) {
  Lexer lexer("   a hello X Name 0 100 123456  ");
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kID)), Field(&Token::val, Eq("a"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kID)), Field(&Token::val, Eq("hello"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kVARIABLE)), Field(&Token::val, Eq("X"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kVARIABLE)), Field(&Token::val, Eq("Name"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)), Field(&Token::val, Eq("0"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)), Field(&Token::val, Eq("100"))));
  EXPECT_THAT(lexer.next(), AllOf(Field(&Token::type, Eq(TokenType::kNUMBER)), Field(&Token::val, Eq("123456"))));
  EXPECT_EQ(lexer.next().type, TokenType::kEOF);
}

TEST_F(ParserTest, ParseSingleTermId) {
  Parser parser("p");
  auto term_status = parser.parse_single_term();
  ASSERT_TRUE(term_status.ok()) << term_status.status();
  auto term = std::move(*term_status);
  auto* pred = dynamic_cast<Predicate*>(term.get());
  ASSERT_NE(pred, nullptr);
  EXPECT_EQ(pred->name, "p");
  EXPECT_EQ(pred->args, nullptr);
}

TEST_F(ParserTest, ParseSingleTermNumber) {
  Parser parser("123");
  auto term_status = parser.parse_single_term();
  ASSERT_TRUE(term_status.ok()) << term_status.status();
  auto term = std::move(*term_status);
  auto* num = dynamic_cast<Number*>(term.get());
  ASSERT_NE(num, nullptr);
  EXPECT_EQ(num->value, 123);
}

TEST_F(ParserTest, ParseTerms) {
  Parser parser("X, 456, \"hello\"");
  auto terms_status = parser.parse_terms();
  ASSERT_TRUE(terms_status.ok()) << terms_status.status();
  auto terms = std::move(*terms_status);
  ASSERT_EQ(terms->size(), 3);
  
  EXPECT_NE(dynamic_cast<Variable*>(terms->at(0).get()), nullptr);
  EXPECT_EQ(dynamic_cast<Variable*>(terms->at(0).get())->name, "X");
  
  EXPECT_NE(dynamic_cast<Number*>(terms->at(1).get()), nullptr);
  EXPECT_EQ(dynamic_cast<Number*>(terms->at(1).get())->value, 456);
  
  EXPECT_NE(dynamic_cast<String*>(terms->at(2).get()), nullptr);
  EXPECT_EQ(dynamic_cast<String*>(terms->at(2).get())->value, "\"hello\"");
}

TEST_F(ParserTest, ParseTermArithmeticLeftAssociative) {
  // 1-2-3 must parse as (1-2)-3, not 1-(2-3).
  Parser parser("1-2-3");
  auto term_status = parser.parse_term();
  ASSERT_TRUE(term_status.ok()) << term_status.status();
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

TEST_F(ParserTest, ParseTermsWithArithmetic) {
  // parse_terms() must use parse_term() so arithmetic expressions are accepted.
  Parser parser("1+2, X*3");
  auto terms_status = parser.parse_terms();
  ASSERT_TRUE(terms_status.ok()) << terms_status.status();
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
  ASSERT_TRUE(lit_status.ok()) << lit_status.status();
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
    ASSERT_TRUE(element_status.ok()) << element_status.status();
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
    ASSERT_TRUE(element_status.ok()) << element_status.status();
    auto element = std::move(*element_status);
    ASSERT_NE(element->terms, nullptr);
    ASSERT_EQ(element->terms->size(), 1);
    EXPECT_EQ(element->literals, nullptr);
  }

  {
    Parser parser(": p");
    auto element_status = parser.parse_aggregate_element();
    ASSERT_TRUE(element_status.ok()) << element_status.status();
    auto element = std::move(*element_status);
    EXPECT_EQ(element->terms, nullptr);
    ASSERT_NE(element->literals, nullptr);
    ASSERT_EQ(element->literals->size(), 1);
  }
}

TEST_F(ParserTest, ParseHeadDisjunction) {
  Parser parser("p(X) | -q(Y)");
  auto head_status = parser.parse_head();
  ASSERT_TRUE(head_status.ok()) << head_status.status();
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
    ASSERT_TRUE(head_status.ok()) << head_status.status();
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
    ASSERT_TRUE(head_status.ok()) << head_status.status();
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
  ASSERT_TRUE(stmt_status.ok()) << stmt_status.status();
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
  ASSERT_TRUE(stmt_status.ok()) << stmt_status.status();
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

TEST_F(ParserTest, ParseProgramGraphColoring) {
  Parser parser(R"(
    node(a). node(b). node(c). node(d).
    edge(a,b). edge(b,c). edge(c,d). edge(d,a). edge(a,c).
    color(red). color(green). color(blue).
    1 <= { col(N,C) : color(C) } <= 1 :- node(N).
    :- edge(X,Y), col(X,C), col(Y,C).
  )");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
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
  ASSERT_TRUE(prog.ok()) << prog.status();
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
  ASSERT_TRUE(prog.ok()) << prog.status();
}

} // namespace
