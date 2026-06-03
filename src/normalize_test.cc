#include "normalize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "format.h"
#include "parse.h"

using namespace ::testing;

namespace {

class NormalizeTest : public ::testing::Test {};

std::string Strip(std::string str) {
  str.erase(0, str.find_first_not_of(" \t\n"));
  str.erase(str.find_last_not_of(" \t\n\r") + 1);
  return str;
}

TEST_F(NormalizeTest, TestNoNormalization) {
  std::string program =
      "edge(a, b).\n"
      "reachable(X, Y) :- edge(X, Y).\n"
      "reachable(a, b)?";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();

  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_EQ(Strip(format(**prog)), program);
}

}  // namespace
