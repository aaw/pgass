#include <cctype>
#include <string_view>

enum class TokenType {
  Id,
  Var,
  Eof,
  Error
};

struct Token {
  TokenType type;
  std::string_view val;
};

class Lexer {
public:
  explicit Lexer(std::string_view source) : source_(source) {}

  Token next() {
    consume_whitespace();
    if (pos_ >= source_.size()) return Token{.type=TokenType::Eof};

    return Token{.type=TokenType::Error};
  }

private:
  void inline consume_whitespace() {
    while (pos_ < source_.size() && std::isspace(source_[pos_])) ++pos_;
  }

  std::size_t pos_ = 0;
  std::string_view source_;
};
