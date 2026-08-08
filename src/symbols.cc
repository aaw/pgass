#include "symbols.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

Sym Symbols::intern(SymEntry entry) {
  auto [it, inserted] =
      index_.try_emplace(std::move(entry), static_cast<Sym>(entries_.size()));
  if (inserted) entries_.push_back(&it->first);
  return it->second;
}

namespace {

// Whether `value` is small enough to ride inside a handle.
bool fits_inline_number(std::int64_t value) {
  return value >= kMinInlineNumber && value <= kMaxInlineNumber;
}

// The handle carrying `value`, which fits_inline_number() has to accept.
Sym inline_number_sym(std::int64_t value) {
  return kInlineNumberTag | static_cast<Sym>(value + kInlineNumberBias);
}

}  // namespace

Sym Symbols::number(std::int64_t value) {
  // Only a number too big to ride inside a handle is copied into the table.
  if (fits_inline_number(value)) return inline_number_sym(value);
  return intern(SymEntry{.kind = SymEntry::kNumber, .number = BigInt(value)});
}

Sym Symbols::number(const BigInt& value) {
  const std::optional<std::int64_t> small = value.to_int64();
  if (small.has_value() && fits_inline_number(*small)) {
    return inline_number_sym(*small);
  }
  return intern(SymEntry{.kind = SymEntry::kNumber, .number = value});
}

Sym Symbols::constant(std::string_view name) {
  return intern(
      SymEntry{.kind = SymEntry::kConstant, .text = std::string(name)});
}

Sym Symbols::string(std::string_view contents) {
  return intern(
      SymEntry{.kind = SymEntry::kString, .text = std::string(contents)});
}

Sym Symbols::function(std::string_view name, std::vector<Sym> args) {
  return intern(SymEntry{.kind = SymEntry::kFunction,
                         .text = std::string(name),
                         .args = std::move(args)});
}

std::string Symbols::printed_call(std::string_view name,
                                  absl::Span<const Sym> args) const {
  if (args.empty()) return std::string(name);
  auto print_arg = [this](std::string* out, Sym arg) {
    out->append(printed(arg));
  };
  return absl::StrCat(name, "(", absl::StrJoin(args, ",", print_arg), ")");
}

std::string Symbols::printed(Sym sym) const {
  if (is_inline_number(sym)) return absl::StrCat(inline_number(sym));
  const SymEntry& value = entry(sym);
  switch (value.kind) {
    case SymEntry::kNumber:
      return absl::StrCat(value.number);
    case SymEntry::kString:
      return absl::StrCat("\"", value.text, "\"");
    case SymEntry::kConstant:
      return value.text;
    case SymEntry::kFunction:
      return printed_call(value.text, value.args);
  }
  return value.text;  // unreachable
}

int Symbols::compare(Sym a, Sym b) const {
  // Interning gives equal values equal handles, so this settles most calls.
  if (a == b) return 0;
  const SymEntry::Kind kind = kind_of(a);
  const SymEntry::Kind other_kind = kind_of(b);
  if (kind != other_kind) return kind < other_kind ? -1 : 1;
  if (kind == SymEntry::kNumber) {
    // Almost every number a program compares rides inside its handle, and
    // reading those two straight off costs nothing.
    if (is_inline_number(a) && is_inline_number(b)) {
      const std::int64_t left = inline_number(a);
      const std::int64_t right = inline_number(b);
      return left < right ? -1 : left > right ? 1 : 0;
    }
    const BigInt left = number_of(a);
    const BigInt right = number_of(b);
    return left < right ? -1 : left > right ? 1 : 0;
  }
  const SymEntry& left = entry(a);
  const SymEntry& right = entry(b);
  if (kind == SymEntry::kFunction && left.args.size() != right.args.size()) {
    return left.args.size() < right.args.size() ? -1 : 1;
  }
  if (left.text != right.text) return left.text < right.text ? -1 : 1;
  for (std::size_t k = 0; k < left.args.size(); ++k) {
    const int c = compare(left.args[k], right.args[k]);
    if (c != 0) return c;
  }
  return 0;
}
