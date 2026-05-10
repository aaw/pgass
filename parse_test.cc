#include <gtest/gtest.h>
#include <gmock/gmock.h>
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

} // namespace
