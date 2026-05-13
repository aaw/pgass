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

} // namespace
