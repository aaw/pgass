#include "format.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

namespace {

void format_terms(std::string* fmt, const Terms& terms);

void format_term(std::string* fmt, const Term& term) {
  switch (term.kind) {
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      absl::StrAppend(fmt, atom.name);
      if (atom.args != nullptr) {
        absl::StrAppend(fmt, "(");
        format_terms(fmt, atom.args);
        absl::StrAppend(fmt, ")");
      }
      break;
    }
    case Term::NumberKind: {
      const Number& number = static_cast<const Number&>(term);
      absl::StrAppend(fmt, number.value);
      break;
    }
    case Term::StringKind: {
      const String& str = static_cast<const String&>(term);
      absl::StrAppend(fmt, "\"", str.value, "\"");
      break;
    }
    case Term::VariableKind: {
      const Variable& var = static_cast<const Variable&>(term);
      absl::StrAppend(fmt, var.name);
      break;
    }
    case Term::AnonymousVariableKind: {
      absl::StrAppend(fmt, "_");
      break;
    }
    case Term::NegatedTermKind: {
      const NegatedTerm& nterm = static_cast<const NegatedTerm&>(term);
      absl::StrAppend(fmt, "-");
      format_term(fmt, *nterm.term);
      break;
    }
    case Term::TermOperationKind: {
      const TermOperation& op = static_cast<const TermOperation&>(term);
      format_term(fmt, *op.left);
      switch (op.op) {
        case OperationType::kPLUS: {
          absl::StrAppend(fmt, " + ");
          break;
        }
        case OperationType::kMINUS: {
          absl::StrAppend(fmt, " - ");
          break;
        }
        case OperationType::kTIMES: {
          absl::StrAppend(fmt, " * ");
          break;
        }
        case OperationType::kDIV: {
          absl::StrAppend(fmt, " / ");
          break;
        }
      }
      format_term(fmt, *op.right);
      break;
    }
  }
}

void format_terms(std::string* fmt, const Terms& terms) {
  if (!terms) return;

  absl::StrAppend(
      fmt, absl::StrJoin(*terms, ", ", [](std::string* out, const auto& term) {
        format_term(out, *term);
      }));
}

void format_classical_literal(std::string* fmt, const ClassicalLiteral& lit) {
  if (lit.negated) absl::StrAppend(fmt, "-");
  absl::StrAppend(fmt, lit.id);
  if (lit.args != nullptr) {
    absl::StrAppend(fmt, "(");
    format_terms(fmt, lit.args);
    absl::StrAppend(fmt, ")");
  }
}

void format_binop(std::string* fmt, BinopType binop) {
  switch (binop) {
    case BinopType::kEQUAL: {
      absl::StrAppend(fmt, "=");
      break;
    }
    case BinopType::kUNEQUAL: {
      absl::StrAppend(fmt, "!=");
      break;
    }
    case BinopType::kLESS: {
      absl::StrAppend(fmt, "<");
      break;
    }
    case BinopType::kGREATER: {
      absl::StrAppend(fmt, ">");
      break;
    }
    case BinopType::kLESS_OR_EQ: {
      absl::StrAppend(fmt, "<=");
      break;
    }
    case BinopType::kGREATER_OR_EQ: {
      absl::StrAppend(fmt, ">=");
      break;
    }
  }
}

void format_literal(std::string* fmt, const Literal& lit) {
  switch (lit.kind) {
    case Literal::BuiltinAtomKind: {
      const BuiltinAtom& atom = static_cast<const BuiltinAtom&>(lit);
      format_term(fmt, *atom.left);
      absl::StrAppend(fmt, " ");
      format_binop(fmt, atom.op);
      absl::StrAppend(fmt, " ");
      format_term(fmt, *atom.right);
      break;
    }
    case Literal::ClassicalLiteralKind: {
      const ClassicalLiteral& cl = static_cast<const ClassicalLiteral&>(lit);
      if (cl.negated) absl::StrAppend(fmt, "-");
      absl::StrAppend(fmt, cl.id);
      if (cl.args) {
        absl::StrAppend(fmt, "(");
        format_terms(fmt, cl.args);
        absl::StrAppend(fmt, ")");
      }
      break;
    }
  }
}

void format_naf_literal(std::string* fmt, const NafLiteral& lit) {
  if (lit.naf) absl::StrAppend(fmt, "not ");
  format_literal(fmt, *lit.literal);
}

void format_naf_literals(std::string* fmt, const NafLiterals& lits) {
  absl::StrAppend(fmt, absl::StrJoin(*lits, ", ",
                                     [](std::string* out, const auto& naf_lit) {
                                       format_naf_literal(out, *naf_lit);
                                     }));
}

void format_choice_element(std::string* fmt, const ChoiceElement& element) {
  format_classical_literal(fmt, *element.literal);
  if (element.conditions) {
    absl::StrAppend(fmt, " : ");
    format_naf_literals(fmt, element.conditions);
  }
}

void format_choice_elements(std::string* fmt, const ChoiceElements& elements) {
  if (!elements) return;

  absl::StrAppend(fmt, absl::StrJoin(*elements, "; ",
                                     [](std::string* out, const auto& elem) {
                                       format_choice_element(out, *elem);
                                     }));
}

void format_head(std::string* fmt, const Head& head) {
  switch (head.kind) {
    case Head::ChoiceKind: {
      const Choice& choice = static_cast<const Choice&>(head);
      if (choice.lb_term) {
        format_term(fmt, *choice.lb_term);
        absl::StrAppend(fmt, " ");
        format_binop(fmt, choice.lb_op);
        absl::StrAppend(fmt, " ");
      }
      absl::StrAppend(fmt, "{ ");
      format_choice_elements(fmt, choice.elements);
      absl::StrAppend(fmt, " }");
      if (choice.ub_term) {
        absl::StrAppend(fmt, " ");
        format_binop(fmt, choice.ub_op);
        absl::StrAppend(fmt, " ");
        format_term(fmt, *choice.ub_term);
      }
      break;
    }
    case Head::DisjunctionKind: {
      const Disjunction& disjunction = static_cast<const Disjunction&>(head);
      absl::StrAppend(fmt, absl::StrJoin(disjunction.literals, " | ",
                                         [](std::string* out, const auto& lit) {
                                           format_classical_literal(out, *lit);
                                         }));
      break;
    }
  }
}

void format_aggregate_function(std::string* fmt,
                               const AggregateFunctionType func_type) {
  switch (func_type) {
    case AggregateFunctionType::kAGGREGATE_COUNT: {
      absl::StrAppend(fmt, "#count");
      break;
    }
    case AggregateFunctionType::kAGGREGATE_MAX: {
      absl::StrAppend(fmt, "#max");
      break;
    }
    case AggregateFunctionType::kAGGREGATE_MIN: {
      absl::StrAppend(fmt, "#min");
      break;
    }
    case AggregateFunctionType::kAGGREGATE_SUM: {
      absl::StrAppend(fmt, "#sum");
      break;
    }
  }
}

void format_aggregate_element(std::string* fmt, const AggregateElement& elem) {
  if (elem.terms) format_terms(fmt, elem.terms);
  if (elem.literals) {
    absl::StrAppend(fmt, ": ");
    format_naf_literals(fmt, elem.literals);
  }
}

void format_aggregate_elements(std::string* fmt,
                               const AggregateElements& elems) {
  absl::StrAppend(
      fmt, absl::StrJoin(*elems, ", ", [](std::string* out, const auto& elem) {
        format_aggregate_element(out, *elem);
      }));
}

void format_aggregate(std::string* fmt, const Aggregate& aggregate) {
  if (aggregate.naf) absl::StrAppend(fmt, "not ");
  if (aggregate.lb_term) {
    format_term(fmt, *aggregate.lb_term);
    absl::StrAppend(fmt, " ");
    format_binop(fmt, aggregate.lb_op);
    absl::StrAppend(fmt, " ");
  }
  format_aggregate_function(fmt, aggregate.function);
  absl::StrAppend(fmt, "{ ");
  format_aggregate_elements(fmt, aggregate.elements);
  absl::StrAppend(fmt, " }");
  if (aggregate.ub_term) {
    absl::StrAppend(fmt, " ");
    format_binop(fmt, aggregate.ub_op);
    absl::StrAppend(fmt, " ");
    format_term(fmt, *aggregate.ub_term);
  }
}

void format_body_item(std::string* fmt, const BodyItem& item) {
  switch (item.kind) {
    case BodyItem::NafLiteralKind: {
      const NafLiteral& naf_lit = static_cast<const NafLiteral&>(item);
      format_naf_literal(fmt, naf_lit);
      break;
    }
    case BodyItem::AggregateKind: {
      const Aggregate& agg = static_cast<const Aggregate&>(item);
      format_aggregate(fmt, agg);
      break;
    }
  }
}

void format_body(std::string* fmt, const Body& body) {
  absl::StrAppend(fmt, absl::StrJoin(*body.items, ", ",
                                     [](std::string* out, const auto& item) {
                                       format_body_item(out, *item);
                                     }));
}

void format_weight(std::string* fmt, const Weight& weight) {
  format_term(fmt, *weight.weight);
  if (weight.level) {
    absl::StrAppend(fmt, " AT ");
    format_term(fmt, *weight.level);
  }
  if (weight.terms) {
    absl::StrAppend(fmt, ", ");
    format_terms(fmt, weight.terms);
  }
}

void format_statement(std::string* fmt, const Statement& statement) {
  if (statement.weight) {
    absl::StrAppend(fmt, ":~ ");
    format_body(fmt, *statement.body);
    absl::StrAppend(fmt, "[ ");
    format_weight(fmt, *statement.weight);
    absl::StrAppend(fmt, " ]");
  } else if (statement.head) {
    format_head(fmt, *statement.head);
    if (statement.body) {
      absl::StrAppend(fmt, " :- ");
      format_body(fmt, *statement.body);
    }
  } else {
    absl::StrAppend(fmt, ":- ");
    format_body(fmt, *statement.body);
  }
  absl::StrAppend(fmt, ".");
}

void format_query(std::string* fmt, const Query& query) {
  format_classical_literal(fmt, *query.lit);
  absl::StrAppend(fmt, "?");
}

}  // namespace

std::string format(const Program& prog) {
  std::string fmt;
  for (const auto& statement : *(prog.statements)) {
    format_statement(&fmt, *statement);
    absl::StrAppend(&fmt, "\n");
  }
  if (prog.query) {
    format_query(&fmt, *prog.query);
    absl::StrAppend(&fmt, "\n");
  }
  return fmt;
}
