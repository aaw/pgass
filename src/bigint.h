#ifndef BIGINT_H_
#define BIGINT_H_

#include <compare>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "absl/container/inlined_vector.h"

// An integer with no bound on its size.
//
// ASP-Core-2 puts no limit on how big a number in a program can be, so neither
// the literals a program spells out nor the values arithmetic builds from them
// fit any fixed width. This holds a sign and a magnitude, and the magnitude
// grows a limb at a time as the value does.
//
// It supports what grounding asks of a number and nothing else: the four
// arithmetic operators, an ordering, printing, parsing, and a narrowing back to
// int64_t for the places that still take a machine integer.
class BigInt {
 public:
  BigInt() = default;
  BigInt(std::int64_t value);  // NOLINT: implicit, so 'k + 1' reads as it does

  // The value `text` spells in decimal, e.g. "42" or "-42". Digits and an
  // optional leading '-', nothing else. Nullopt when `text` is anything else.
  static std::optional<BigInt> from_decimal(std::string_view text);

  std::string to_string() const;

  // The same value as an int64_t, or nullopt when it is too big for one.
  // Symbols::number() asks, to decide whether a value is small enough to ride
  // inside a Sym handle instead of taking a table entry.
  std::optional<std::int64_t> to_int64() const;

  bool is_zero() const { return mag_.empty(); }

  BigInt operator-() const;
  BigInt operator+(const BigInt& other) const;
  BigInt operator-(const BigInt& other) const;
  BigInt operator*(const BigInt& other) const;
  // Truncates toward zero, so -7 / 2 is -3. Ask only when `other` is nonzero.
  BigInt operator/(const BigInt& other) const;

  BigInt& operator+=(const BigInt& other) { return *this = *this + other; }
  BigInt& operator-=(const BigInt& other) { return *this = *this - other; }

  // One value has one representation, so equality is a member-by-member test.
  bool operator==(const BigInt& other) const = default;
  std::strong_ordering operator<=>(const BigInt& other) const;

  template <typename H>
  friend H AbslHashValue(H h, const BigInt& value) {
    return H::combine(H::combine_contiguous(std::move(h), value.mag_.data(),
                                            value.mag_.size()),
                      value.negative_);
  }

  // Lets absl::StrCat and absl::StrAppend take a BigInt directly.
  template <typename Sink>
  friend void AbslStringify(Sink& sink, const BigInt& value) {
    sink.Append(value.to_string());
  }

  // Lets a failing test print the value rather than the bytes behind it.
  friend std::ostream& operator<<(std::ostream& out, const BigInt& value) {
    return out << value.to_string();
  }

 private:
  // A magnitude is a little-endian run of digits in this base, with no leading
  // zeros, so an empty one is zero. Base 10^9 is the largest power of ten whose
  // square fits a uint64_t, which is what makes printing and parsing plain
  // digit shuffling and still lets multiplication carry in one word.
  static constexpr std::uint32_t kBase = 1000000000;
  // Three limbs cover the whole int64_t range, so the numbers a program
  // actually does arithmetic on need no allocation at all. A bigger one spills
  // to the heap.
  using Mag = absl::InlinedVector<std::uint32_t, 3>;

  static void trim(Mag& mag);
  static int compare_mag(const Mag& a, const Mag& b);
  static Mag add_mag(const Mag& a, const Mag& b);
  static Mag sub_mag(const Mag& a, const Mag& b);  // needs a >= b
  static Mag mul_small(const Mag& a, std::uint32_t b);

  // The sign the magnitude carries. Zero is never negative, which is what makes
  // one value have one representation.
  bool negative_ = false;
  Mag mag_;
};

#endif  // BIGINT_H_
