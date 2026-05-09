#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "parse.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

// Using namespaces makes matchers like HasSubstr much cleaner
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

} // namespace
