#ifndef SYMBOLS_H_
#define SYMBOLS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/node_hash_map.h"
#include "absl/types/span.h"

// A ground value, interned down to four bytes.
//
// Grounding hashes, compares, and copies tuples of values far more often than
// it builds new ones, so what a value costs to carry around matters more than
// what it costs to make. Interning gives every distinct value one handle, so a
// tuple is a few words that compare with a memcmp instead of a walk through
// strings and nested arguments.
//
// The top bit of a handle says how to read the rest. Set means the handle is an
// integer itself, held in the remaining 31 bits and biased so it can be
// negative: integers in [-2^30, 2^30) never reach the table at all, which
// matters because arithmetic invents them by the million and each one would
// otherwise cost a hash lookup. Clear means the handle is a position in the
// table, which is where constants, strings, function terms, and the integers
// too big to inline live.
using Sym = std::uint32_t;

// Set on a handle that carries its own integer.
inline constexpr Sym kInlineNumberTag = 0x80000000u;

// Added to an inlined integer so the negative ones encode too: the range
// [-2^30, 2^30) becomes [0, 2^31), which is what the 31 bits below the tag
// hold.
inline constexpr std::int64_t kInlineNumberBias = std::int64_t{1} << 30;
inline constexpr std::int64_t kMinInlineNumber = -kInlineNumberBias;
inline constexpr std::int64_t kMaxInlineNumber = kInlineNumberBias - 1;

// A slot holding no value yet, e.g. a rule variable the body has not bound. It
// reads as the last table position, which no program reaches: a table that full
// would hold two billion entries of fifty-odd bytes each.
inline constexpr Sym kNoSym = 0x7FFFFFFFu;

// What one interned value holds. A number in the inline range has no entry;
// every other value has exactly one, which is what makes two equal values come
// out as the same handle.
//
// The kind order matches the ASP-Core-2 spec, which orders integers < symbolic
// constants < string constants < functional terms.
struct SymEntry {
  enum Kind { kNumber, kConstant, kString, kFunction };

  Kind kind;
  std::int64_t number = 0;  // set only when kind == kNumber
  std::string text;         // the constant's, string's, or function's name
  std::vector<Sym> args;    // set only when kind == kFunction, never empty

  bool operator==(const SymEntry&) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const SymEntry& entry) {
    return H::combine(std::move(h), entry.kind, entry.number, entry.text,
                      entry.args);
  }
};

// Whether `sym` carries its own integer rather than pointing into the table.
inline bool is_inline_number(Sym sym) { return (sym & kInlineNumberTag) != 0; }

// The integer an inlined handle carries.
inline std::int64_t inline_number(Sym sym) {
  return static_cast<std::int64_t>(sym & ~kInlineNumberTag) - kInlineNumberBias;
}

// The table every ground value of one grounding run is interned into. It only
// ever grows: a value stays reachable for as long as any tuple holds its
// handle, which is until grounding is done.
class Symbols {
 public:
  // The handle for a value, interning it if this is the first time it is seen.
  Sym number(std::int64_t value);
  Sym constant(std::string_view name);
  Sym string(std::string_view contents);
  Sym function(std::string_view name, std::vector<Sym> args);

  SymEntry::Kind kind_of(Sym sym) const {
    return is_inline_number(sym) ? SymEntry::kNumber : entry(sym).kind;
  }
  bool is_number(Sym sym) const { return kind_of(sym) == SymEntry::kNumber; }

  // The integer `sym` stands for. Only ask this of a handle is_number() said
  // yes to.
  std::int64_t number_of(Sym sym) const {
    return is_inline_number(sym) ? inline_number(sym) : entry(sym).number;
  }

  // What `sym` was interned from. Only ask this of a handle that has a table
  // entry, i.e. one is_inline_number() said no to.
  const SymEntry& entry(Sym sym) const { return *entries_[sym]; }

  // The value as it prints in an answer set: 42, abc, "abc", or f(1,abc).
  std::string printed(Sym sym) const;

  // One name applied to argument values, e.g. p(1,abc), which is how both a
  // function term and a ground atom of that name and those arguments print.
  std::string printed_call(std::string_view name,
                           absl::Span<const Sym> args) const;

  // Orders two values per the ASP-Core-2 spec (see SymEntry): numbers
  // numerically, constants and strings lexicographically, and function terms by
  // arity first, then name, then argument by argument.
  int compare(Sym a, Sym b) const;

 private:
  Sym intern(SymEntry entry);

  // Interning looks a value up here and, on a miss, adds it and appends the
  // key's address to `entries_`. The map holds its entries in nodes so those
  // addresses survive it growing.
  absl::node_hash_map<SymEntry, Sym> index_;
  std::vector<const SymEntry*> entries_;  // handle -> its entry
};

#endif  // SYMBOLS_H_
