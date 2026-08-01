#include "bigint.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

BigInt::BigInt(std::int64_t value) : negative_(value < 0) {
  // Negating in unsigned form so that the most negative int64_t works too.
  std::uint64_t rest = negative_ ? -static_cast<std::uint64_t>(value)
                                 : static_cast<std::uint64_t>(value);
  for (; rest != 0; rest /= kBase) {
    mag_.push_back(static_cast<std::uint32_t>(rest % kBase));
  }
}

std::optional<BigInt> BigInt::from_decimal(std::string_view text) {
  BigInt result;
  if (text.starts_with('-')) {
    result.negative_ = true;
    text.remove_prefix(1);
  }
  if (text.empty()) return std::nullopt;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  // Nine digits at a time from the right, which is one limb each.
  for (std::size_t end = text.size(); end > 0;) {
    const std::size_t start = end > 9 ? end - 9 : 0;
    std::uint32_t limb = 0;
    for (std::size_t k = start; k < end; ++k) {
      limb = limb * 10 + static_cast<std::uint32_t>(text[k] - '0');
    }
    result.mag_.push_back(limb);
    end = start;
  }
  trim(result.mag_);
  if (result.mag_.empty()) result.negative_ = false;  // "-0" is 0
  return result;
}

std::string BigInt::to_string() const {
  if (mag_.empty()) return "0";
  // The top limb prints as itself. Every limb below it stands for exactly nine
  // digits, so it needs its leading zeros back.
  std::string out = absl::StrCat(negative_ ? "-" : "", mag_.back());
  for (std::size_t k = mag_.size() - 1; k > 0; --k) {
    absl::StrAppendFormat(&out, "%09u", mag_[k - 1]);
  }
  return out;
}

std::optional<std::int64_t> BigInt::to_int64() const {
  std::uint64_t value = 0;
  for (std::size_t k = mag_.size(); k > 0; --k) {
    if (value > (UINT64_MAX - mag_[k - 1]) / kBase) return std::nullopt;
    value = value * kBase + mag_[k - 1];
  }
  // A negative reaches one further than a positive does.
  const std::uint64_t limit =
      negative_ ? std::uint64_t{1} << 63 : (std::uint64_t{1} << 63) - 1;
  if (value > limit) return std::nullopt;
  return negative_ ? static_cast<std::int64_t>(0 - value)
                   : static_cast<std::int64_t>(value);
}

void BigInt::trim(Mag& mag) {
  while (!mag.empty() && mag.back() == 0) mag.pop_back();
}

int BigInt::compare_mag(const Mag& a, const Mag& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (std::size_t k = a.size(); k > 0; --k) {
    if (a[k - 1] != b[k - 1]) return a[k - 1] < b[k - 1] ? -1 : 1;
  }
  return 0;
}

BigInt::Mag BigInt::add_mag(const Mag& a, const Mag& b) {
  Mag sum;
  std::uint32_t carry = 0;
  for (std::size_t k = 0; k < a.size() || k < b.size() || carry != 0; ++k) {
    std::uint32_t digit = carry;
    if (k < a.size()) digit += a[k];
    if (k < b.size()) digit += b[k];
    carry = digit >= kBase ? 1 : 0;
    sum.push_back(carry != 0 ? digit - kBase : digit);
  }
  return sum;
}

BigInt::Mag BigInt::sub_mag(const Mag& a, const Mag& b) {
  Mag difference;
  std::int64_t borrow = 0;
  for (std::size_t k = 0; k < a.size(); ++k) {
    std::int64_t digit = std::int64_t{a[k]} - borrow;
    if (k < b.size()) digit -= b[k];
    borrow = digit < 0 ? 1 : 0;
    difference.push_back(
        static_cast<std::uint32_t>(borrow != 0 ? digit + kBase : digit));
  }
  trim(difference);
  return difference;
}

BigInt::Mag BigInt::mul_small(const Mag& a, std::uint32_t b) {
  Mag product;
  std::uint64_t carry = 0;
  for (std::size_t k = 0; k < a.size() || carry != 0; ++k) {
    const std::uint64_t digit =
        carry + (k < a.size() ? std::uint64_t{a[k]} * b : 0);
    product.push_back(static_cast<std::uint32_t>(digit % kBase));
    carry = digit / kBase;
  }
  trim(product);
  return product;
}

BigInt BigInt::operator-() const {
  BigInt result = *this;
  if (!result.mag_.empty()) result.negative_ = !result.negative_;
  return result;
}

BigInt BigInt::operator+(const BigInt& other) const {
  BigInt result;
  if (negative_ == other.negative_) {
    result.mag_ = add_mag(mag_, other.mag_);
    result.negative_ = negative_;
  } else {
    // The bigger magnitude decides both the difference and the sign.
    const int c = compare_mag(mag_, other.mag_);
    const BigInt& bigger = c < 0 ? other : *this;
    const BigInt& smaller = c < 0 ? *this : other;
    result.mag_ = sub_mag(bigger.mag_, smaller.mag_);
    result.negative_ = bigger.negative_;
  }
  if (result.mag_.empty()) result.negative_ = false;
  return result;
}

BigInt BigInt::operator-(const BigInt& other) const { return *this + -other; }

BigInt BigInt::operator*(const BigInt& other) const {
  BigInt result;
  if (mag_.empty() || other.mag_.empty()) return result;
  result.mag_.assign(mag_.size() + other.mag_.size(), 0);
  for (std::size_t i = 0; i < mag_.size(); ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < other.mag_.size() || carry != 0; ++j) {
      const std::uint64_t digit =
          result.mag_[i + j] + carry +
          (j < other.mag_.size() ? std::uint64_t{mag_[i]} * other.mag_[j] : 0);
      result.mag_[i + j] = static_cast<std::uint32_t>(digit % kBase);
      carry = digit / kBase;
    }
  }
  trim(result.mag_);
  result.negative_ = negative_ != other.negative_;
  return result;
}

BigInt BigInt::operator/(const BigInt& other) const {
  // Long division over limbs. Bring one limb of the numerator down at a time
  // and ask how many times the divisor goes into what has been brought down.
  BigInt result;
  result.mag_.assign(mag_.size(), 0);
  Mag rest;
  for (std::size_t k = mag_.size(); k > 0; --k) {
    rest.insert(rest.begin(), mag_[k - 1]);
    trim(rest);
    // The answer is the largest limb whose product with the divisor still fits
    // under what has been brought down, and 0 always does.
    std::uint32_t low = 0;
    std::uint32_t high = kBase - 1;
    while (low < high) {
      const std::uint32_t middle = low + (high - low + 1) / 2;
      if (compare_mag(mul_small(other.mag_, middle), rest) <= 0) {
        low = middle;
      } else {
        high = middle - 1;
      }
    }
    result.mag_[k - 1] = low;
    rest = sub_mag(rest, mul_small(other.mag_, low));
  }
  trim(result.mag_);
  result.negative_ = !result.mag_.empty() && negative_ != other.negative_;
  return result;
}

std::strong_ordering BigInt::operator<=>(const BigInt& other) const {
  if (negative_ != other.negative_) {
    return negative_ ? std::strong_ordering::less
                     : std::strong_ordering::greater;
  }
  const int c = compare_mag(mag_, other.mag_);
  // Among negatives the bigger magnitude is the smaller value.
  return (negative_ ? -c : c) <=> 0;
}
