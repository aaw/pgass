#include "bigint.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/hash/hash.h"
#include "absl/strings/str_cat.h"

using namespace ::testing;

namespace {

class BigIntTest : public ::testing::Test {};

// The value `text` spells. Only for input the tests know is well formed.
BigInt Big(std::string_view text) { return *BigInt::from_decimal(text); }

TEST_F(BigIntTest, RoundTripsDecimal) {
  for (std::string_view text :
       {"0", "1", "-1", "9", "999999999", "1000000000", "1000000001",
        "-1000000000", "123456789012345678901234567890",
        "-99999999999999999999999999999999999999"}) {
    EXPECT_EQ(Big(text).to_string(), text);
  }
}

TEST_F(BigIntTest, ParsesLeadingZerosAndNegativeZero) {
  EXPECT_EQ(Big("007").to_string(), "7");
  EXPECT_EQ(Big("-0").to_string(), "0");
  EXPECT_EQ(Big("-0"), BigInt(0));
  EXPECT_EQ(Big("0000000000000000000").to_string(), "0");
}

TEST_F(BigIntTest, RejectsWhatIsNotADecimal) {
  EXPECT_FALSE(BigInt::from_decimal("").has_value());
  EXPECT_FALSE(BigInt::from_decimal("-").has_value());
  EXPECT_FALSE(BigInt::from_decimal("12a").has_value());
  EXPECT_FALSE(BigInt::from_decimal(" 12").has_value());
  EXPECT_FALSE(BigInt::from_decimal("1-2").has_value());
  EXPECT_FALSE(BigInt::from_decimal("+12").has_value());
}

TEST_F(BigIntTest, DefaultsToZero) {
  EXPECT_TRUE(BigInt().is_zero());
  EXPECT_EQ(BigInt(), BigInt(0));
  EXPECT_EQ(BigInt().to_string(), "0");
  EXPECT_FALSE(BigInt(1).is_zero());
  EXPECT_FALSE(BigInt(-1).is_zero());
}

TEST_F(BigIntTest, ConvertsToAndFromInt64) {
  for (std::int64_t value : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1},
                             std::int64_t{1000000000}, INT64_MAX, INT64_MIN}) {
    EXPECT_EQ(BigInt(value).to_int64(), value);
    EXPECT_EQ(BigInt(value).to_string(), absl::StrCat(value));
  }
}

TEST_F(BigIntTest, RefusesAnInt64ItWouldNotFit) {
  EXPECT_FALSE(Big("9223372036854775808").to_int64().has_value());
  EXPECT_FALSE(Big("-9223372036854775809").to_int64().has_value());
  EXPECT_EQ(Big("9223372036854775807").to_int64(), INT64_MAX);
  EXPECT_EQ(Big("-9223372036854775808").to_int64(), INT64_MIN);
  EXPECT_FALSE(Big("99999999999999999999999999999999").to_int64().has_value());
}

TEST_F(BigIntTest, Adds) {
  EXPECT_EQ(Big("999999999") + Big("1"), Big("1000000000"));
  EXPECT_EQ(Big("-999999999") + Big("-1"), Big("-1000000000"));
  EXPECT_EQ(Big("5") + Big("-8"), Big("-3"));
  EXPECT_EQ(Big("-5") + Big("8"), Big("3"));
  EXPECT_EQ(Big("5") + Big("-5"), BigInt(0));
  EXPECT_EQ(Big("123456789012345678901234567890") + Big("1"),
            Big("123456789012345678901234567891"));
}

TEST_F(BigIntTest, Subtracts) {
  EXPECT_EQ(Big("1000000000") - Big("1"), Big("999999999"));
  EXPECT_EQ(Big("1") - Big("1000000000"), Big("-999999999"));
  EXPECT_EQ(Big("-5") - Big("-8"), Big("3"));
  EXPECT_EQ(Big("7") - Big("7"), BigInt(0));
}

TEST_F(BigIntTest, Multiplies) {
  EXPECT_EQ(Big("999999999") * Big("999999999"), Big("999999998000000001"));
  EXPECT_EQ(Big("-3") * Big("4"), Big("-12"));
  EXPECT_EQ(Big("-3") * Big("-4"), Big("12"));
  EXPECT_EQ(Big("123456789") * BigInt(0), BigInt(0));
  // 2^64, which is where the machine integers this replaces gave up.
  EXPECT_EQ(Big("4294967296") * Big("4294967296"), Big("18446744073709551616"));
}

TEST_F(BigIntTest, DividesTruncatingTowardZero) {
  EXPECT_EQ(Big("7") / Big("2"), Big("3"));
  EXPECT_EQ(Big("-7") / Big("2"), Big("-3"));
  EXPECT_EQ(Big("7") / Big("-2"), Big("-3"));
  EXPECT_EQ(Big("-7") / Big("-2"), Big("3"));
  EXPECT_EQ(Big("1") / Big("2"), BigInt(0));
  EXPECT_EQ(Big("-1") / Big("2"), BigInt(0));
  EXPECT_EQ(Big("18446744073709551616") / Big("4294967296"), Big("4294967296"));
  EXPECT_EQ(Big("123456789012345678901234567890") / Big("987654321"),
            Big("124999998873437499901"));
}

// Every quotient times its divisor, plus what long division left over, has to
// come back to where it started.
TEST_F(BigIntTest, DivisionAgreesWithMultiplication) {
  const BigInt big = Big("98765432109876543210987654321098765432");
  for (std::string_view divisor_text :
       {"1", "2", "7", "999999999", "1000000000", "1000000007",
        "123456789012345678901", "98765432109876543210987654321098765433"}) {
    const BigInt divisor = Big(divisor_text);
    const BigInt quotient = big / divisor;
    const BigInt remainder = big - quotient * divisor;
    EXPECT_GE(remainder, BigInt(0)) << divisor_text;
    EXPECT_LT(remainder, divisor) << divisor_text;
  }
}

TEST_F(BigIntTest, Negates) {
  EXPECT_EQ(-Big("5"), Big("-5"));
  EXPECT_EQ(-Big("-5"), Big("5"));
  EXPECT_EQ(-BigInt(0), BigInt(0));
  EXPECT_EQ((-BigInt(0)).to_string(), "0");
  EXPECT_EQ(-BigInt(INT64_MIN), Big("9223372036854775808"));
}

TEST_F(BigIntTest, Orders) {
  EXPECT_LT(Big("-1000000000"), Big("-999999999"));
  EXPECT_LT(Big("-1"), BigInt(0));
  EXPECT_LT(BigInt(0), Big("1"));
  EXPECT_LT(Big("999999999"), Big("1000000000"));
  EXPECT_LT(Big("999999999999999999999"), Big("1000000000000000000000"));
  EXPECT_GT(Big("5"), Big("-8"));
  EXPECT_EQ(Big("42"), BigInt(42));
  EXPECT_NE(Big("42"), Big("-42"));
  // Mixing in a machine integer, which is how the bound arithmetic reads.
  EXPECT_EQ(Big("41") + 1, BigInt(42));
  EXPECT_LT(BigInt(41), 42);
}

TEST_F(BigIntTest, AccumulatesInPlace) {
  BigInt sum;
  for (int k = 0; k < 5; ++k) sum += Big("999999999");
  EXPECT_EQ(sum, Big("4999999995"));
  sum -= Big("4999999995");
  EXPECT_TRUE(sum.is_zero());
}

TEST_F(BigIntTest, StringifiesThroughAbsl) {
  EXPECT_EQ(absl::StrCat(Big("-123456789012345678901")),
            "-123456789012345678901");
  EXPECT_EQ(absl::StrCat("n=", BigInt(0)), "n=0");
}

// Equal values have to hash equally, which is what lets a SymEntry holding one
// intern to a single handle.
TEST_F(BigIntTest, Hashes) {
  const absl::Hash<BigInt> hash;
  EXPECT_EQ(hash(BigInt()), hash(Big("-0")));
  EXPECT_EQ(hash(Big("123456789012345678901234567890")),
            hash(Big("000123456789012345678901234567890")));
  EXPECT_EQ(hash(BigInt(INT64_MIN)), hash(Big("-9223372036854775808")));
  EXPECT_NE(hash(Big("1")), hash(Big("-1")));
  EXPECT_NE(hash(Big("123456789012345678901234567890")),
            hash(Big("-123456789012345678901234567890")));
}

}  // namespace
