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
  absl::string_view input = "   ";
  Lexer lexer(input);

  EXPECT_EQ(lexer.next().type, TokenType::Eof);
}

} // namespace
