#include "format.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "parse.h"
#include "test_macros.h"

using namespace ::testing;

namespace {

class FormatTest : public ::testing::Test {};

// Formats `src`, then formats that. The second pass parses what the first one
// printed, which catches output the parser rejects. The two passes have to
// agree, which catches output that parses as a different program.
void ExpectRoundTrips(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  std::string once = format(**prog);

  Parser reparser(once);
  auto reparsed = reparser.parse_program();
  ASSERT_TRUE(reparsed.ok()) << "formatter emitted source the parser rejects:\n"
                             << once << reparsed.status();
  EXPECT_EQ(format(**reparsed), once);
}

TEST_F(FormatTest, WeakConstraintPutsTheDotBeforeTheWeight) {
  // The grammar is ':~' body '.' '[' weight_at_level ']'.
  std::string_view src = ":~ t(X). [X@2,X]";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_EQ(format(**prog), ":~ t(X). [X@2, X]\n");
}

TEST_F(FormatTest, WeakConstraintRoundTrips) {
  ExpectRoundTrips(":~ t(X), u(X). [X@2,X]");
}

TEST_F(FormatTest, WeakConstraintWithoutLevelOrTermsRoundTrips) {
  ExpectRoundTrips("a. :~ a. [3]");
}

TEST_F(FormatTest, RuleRoundTrips) {
  ExpectRoundTrips(
      "a | b :- c, not d, 1 <= #count{X, Y : p(X, Y), not q(X)} <= 3.");
}

TEST_F(FormatTest, ChoiceRoundTrips) {
  ExpectRoundTrips("1 <= { r(X) : s(X) } <= 2 :- t(X).");
}

TEST_F(FormatTest, TermsRoundTrip) {
  ExpectRoundTrips(
      R"(p(f(g(1, a), "s"), -3, X + 1 * (2 - Y) / 4) :- q(X, Y, _).)");
}

TEST_F(FormatTest, QueryRoundTrips) { ExpectRoundTrips("p(1). -p(X)?"); }

}  // namespace
