#include "normalize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "format.h"
#include "parse.h"

using namespace ::testing;

namespace {

class NormalizeTest : public ::testing::Test {};

// Trims leading/trailing whitespace and collapses whitespace runs containing
// newlines into a single space.
std::string Strip(const std::string& str) {
  size_t start = str.find_first_not_of(" \n");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \n") + 1;

  std::string result;
  size_t i = start;
  while (i < end) {
    if (!std::isspace(str[i])) {
      result += str[i++];
      continue;
    }
    size_t ws_start = i;
    bool has_newline = false;
    while (i < end && std::isspace(str[i])) {
      if (str[i] == '\n') has_newline = true;
      i++;
    }
    if (has_newline) {
      result += ' ';
    } else {
      result.append(str, ws_start, i - ws_start);
    }
  }
  return result;
}

MATCHER_P(EquivalentToSource, expected, "") {
  std::string actual = Strip(format(arg));
  std::string want = Strip(expected);
  if (actual == want) return true;
  *result_listener << "formatted source did not match.\n  actual:   " << actual
                   << "\n  expected: " << want;
  return false;
}

TEST_F(NormalizeTest, TestNoNormalization) {
  std::string program = R"(
    edge(a, b).
    reachable(X, Y) :- edge(X, Y).
    reachable(a, b)?
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(program));
}

TEST_F(NormalizeTest, TestNormalizeIntegrityConstraints) {
  std::string program = R"(
    :- good(a), not bad(b).
    :- edge(X, Y).
    reachable(a, b)?
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _ic0 :- good(a), not bad(b), not _ic0.
    _ic1 :- edge(X, Y), not _ic1.
    reachable(a, b)?
  )"));
}

}  // namespace
