#ifndef __MACROS_H__
#define __MACROS_H__

#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)

#define RETURN_IF_ERROR(expr)    \
  do {                           \
    auto _s = (expr);            \
    if (!_s.ok()) return _s;     \
  } while (0)

#define ASSIGN_OR_RETURN(lhs, rexpr)              \
  auto CONCAT(status_or_, __LINE__) = (rexpr);    \
  if (!CONCAT(status_or_, __LINE__).ok()) {       \
    return CONCAT(status_or_, __LINE__).status(); \
  }                                               \
  lhs = std::move(CONCAT(status_or_, __LINE__)).value()

#define CONSUME_TOKEN_OR_RETURN(lexer, lhs, token_type)                \
  Token lhs = lexer.next();                                            \
  if (lhs.type != token_type) {                                        \
    return absl::InvalidArgumentError(absl::StrCat(                    \
        "Unexpected token '", lhs.val, "'\n",                          \
        lexer.report_last_token_pos()));                                \
  }

#define CONSUME_TOKEN_TYPE_OR_RETURN(lexer, token_type) \
  CONSUME_TOKEN_OR_RETURN(lexer, CONCAT(token_, __LINE__), token_type)

#endif  // __MACROS_H__
