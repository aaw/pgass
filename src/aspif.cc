#include "aspif.h"

#include <string>

#include "absl/strings/str_cat.h"

namespace aspif {

namespace {

void append_rule(const Rule& rule, std::string& out) {
  absl::StrAppend(&out, "1 ", static_cast<int>(rule.head_type), " ",
                  rule.head.size());
  for (Atom atom : rule.head) absl::StrAppend(&out, " ", atom);

  absl::StrAppend(&out, " ", static_cast<int>(rule.body_type));
  if (rule.body_type == Rule::BodyType::kNormal) {
    absl::StrAppend(&out, " ", rule.body.size());
    for (Lit lit : rule.body) absl::StrAppend(&out, " ", lit);
  } else {
    absl::StrAppend(&out, " ", rule.lower_bound, " ",
                    rule.weighted_body.size());
    for (const WeightedLit& wl : rule.weighted_body) {
      absl::StrAppend(&out, " ", wl.lit, " ", wl.weight);
    }
  }
  absl::StrAppend(&out, "\n");
}

}  // namespace

std::string to_aspif(const Program& prog) {
  std::string out = "asp 1 0 0\n";
  for (const Rule& rule : prog.rules) append_rule(rule, out);
  for (const Minimize& minimize : prog.minimize) {
    absl::StrAppend(&out, "2 ", minimize.priority, " ", minimize.lits.size());
    for (const WeightedLit& wl : minimize.lits) {
      absl::StrAppend(&out, " ", wl.lit, " ", wl.weight);
    }
    absl::StrAppend(&out, "\n");
  }
  for (const Output& output : prog.outputs) {
    absl::StrAppend(&out, "4 ", output.name.size(), " ", output.name, " ",
                    output.condition.size());
    for (Lit lit : output.condition) absl::StrAppend(&out, " ", lit);
    absl::StrAppend(&out, "\n");
  }
  if (!prog.assumptions.empty()) {
    absl::StrAppend(&out, "6 ", prog.assumptions.size());
    for (Lit lit : prog.assumptions) absl::StrAppend(&out, " ", lit);
    absl::StrAppend(&out, "\n");
  }
  absl::StrAppend(&out, "0\n");
  return out;
}

}  // namespace aspif
