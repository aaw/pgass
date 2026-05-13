#ifndef __TOKENIZE_H__
#define __TOKENIZE_H__

#include <string_view>

#include "absl/strings/str_cat.h"

enum class TokenType {
  kAGGREGATE_COUNT,
  kAGGREGATE_MAX,
  kAGGREGATE_MIN,
  kAGGREGATE_SUM,
  kANONYMOUS_VARIABLE,
  kAT,
  kCOLON,
  kCOMMA,
  kCONS,
  kCURLY_CLOSE,
  kCURLY_OPEN,
  kDIV,
  kDOT,
  kEOF,
  kEQUAL,
  kERROR,
  kGREATER,
  kGREATER_OR_EQ,
  kID,
  kLESS,
  kLESS_OR_EQ,
  kMINUS,
  kNAF,
  kNUMBER,
  kOR,
  kPAREN_CLOSE,
  kPAREN_OPEN,
  kPLUS,
  kQUERY_MARK,
  kSEMICOLON,
  kSQUARE_CLOSE,
  kSQUARE_OPEN,
  kSTRING,
  kTIMES,
  kUNEQUAL,
  kVARIABLE,
  kWCONS,
};

struct Token {
  TokenType type;
  std::string_view val;
};

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  std::string report_pos() {
    int line = 1;
    size_t last_newline_pos = 0;

    for (size_t i = 0; i < pos_; ++i) {
      if (source_[i] == '\n') {
        line++;
        last_newline_pos = i + 1;  // Start of the next line
      }
    }
    return absl::StrCat("line ", line, ", column ",
                        pos_ - last_newline_pos + 1);
  }

  Token next() {
    // Skip any whitespace or comments.
    bool did_work = true;
    while (did_work) {
      did_work = false;
      while (pos_ < source_.size() &&
             (source_[pos_] == ' ' || source_[pos_] == '\t' ||
              source_[pos_] == '\n')) {
        did_work = true;
        ++pos_;
      }
      if (pos_ < source_.size() && source_[pos_] == '%') {
        did_work = true;
        if (++pos_ >= source_.size()) break;
        if (source_[pos_] == '*') {
          while (pos_ < source_.size() - 1 && source_.substr(pos_, 2) != "*%")
            ++pos_;
          if (pos_ >= source_.size() - 1)
            return Token{
                .type = TokenType::kERROR,
                .val = "Unterminated multi-line comment at end-of-file"};
          pos_ += 2;
        } else {
          while (pos_ < source_.size() && source_[pos_] != '\n') ++pos_;
          ++pos_;
        }
      }
    }

    if (pos_ >= source_.size()) return Token{.type = TokenType::kEOF};

    // Consume/return any recognized tokens. First handle (possibly)
    // multi-character tokens.
    if (source_.substr(pos_, 3) == "not" && !is_id_char(pos_ + 3))
      return consume(TokenType::kNAF, 3);
    if (source_[pos_] == '#') {
      if (source_.substr(pos_, 6) == "#count" && !is_id_char(pos_ + 6))
        return consume(TokenType::kAGGREGATE_COUNT, 6);
      if (source_.substr(pos_, 4) == "#max" && !is_id_char(pos_ + 4))
        return consume(TokenType::kAGGREGATE_MAX, 4);
      if (source_.substr(pos_, 4) == "#min" && !is_id_char(pos_ + 4))
        return consume(TokenType::kAGGREGATE_MIN, 4);
      if (source_.substr(pos_, 4) == "#sum" && !is_id_char(pos_ + 4))
        return consume(TokenType::kAGGREGATE_SUM, 4);
    }
    if (source_.substr(pos_, 2) == "<>" || source_.substr(pos_, 2) == "!=") {
      return consume(TokenType::kUNEQUAL, 2);
    }
    if (source_.substr(pos_, 2) == "<=")
      return consume(TokenType::kLESS_OR_EQ, 2);
    if (source_.substr(pos_, 2) == ">=")
      return consume(TokenType::kGREATER_OR_EQ, 2);
    if (source_.substr(pos_, 2) == ":-") return consume(TokenType::kCONS, 2);
    if (source_.substr(pos_, 2) == ":~") return consume(TokenType::kWCONS, 2);
    if (source_[pos_] >= 'a' && source_[pos_] <= 'z') {
      std::size_t end_pos = pos_ + 1;
      while (end_pos < source_.size() &&
             ((source_[end_pos] >= 'a' && source_[end_pos] <= 'z') ||
              (source_[end_pos] >= 'A' && source_[end_pos] <= 'Z') ||
              (source_[end_pos] >= '0' && source_[end_pos] <= '9') ||
              source_[end_pos] == '_')) {
        ++end_pos;
      }
      return consume(TokenType::kID, end_pos - pos_);
    }
    if (source_[pos_] >= 'A' && source_[pos_] <= 'Z') {
      std::size_t end_pos = pos_ + 1;
      while (end_pos < source_.size() &&
             ((source_[end_pos] >= 'a' && source_[end_pos] <= 'z') ||
              (source_[end_pos] >= 'A' && source_[end_pos] <= 'Z') ||
              (source_[end_pos] >= '0' && source_[end_pos] <= '9') ||
              source_[end_pos] == '_')) {
        ++end_pos;
      }
      return consume(TokenType::kVARIABLE, end_pos - pos_);
    }
    if (source_[pos_] == '\"') {
      std::size_t end_pos = pos_ + 1;
      bool found_quote = false;
      while (end_pos < source_.size()) {
        if (source_[end_pos] == '\"') {
          ++end_pos;
          found_quote = true;
          break;
        }
        if (source_[end_pos] == '\\') {
          ++end_pos;
          if (end_pos >= source_.size()) break;
        }
        ++end_pos;
      }
      if (!found_quote)
        return Token{.type = TokenType::kERROR,
                     .val = "Unterminated string at end-of-file"};
      return consume(TokenType::kSTRING, end_pos - pos_);
    }
    if (source_[pos_] >= '1' && source_[pos_] <= '9') {
      std::size_t end_pos = pos_ + 1;
      while (end_pos < source_.size() && source_[end_pos] >= '0' &&
             source_[end_pos] <= '9') {
        ++end_pos;
      }
      return consume(TokenType::kNUMBER, end_pos - pos_);
    }
    if (source_[pos_] == '0') return consume(TokenType::kNUMBER, 1);

    // Now handle all single-char tokens.
    if (source_[pos_] == '_') return consume(TokenType::kANONYMOUS_VARIABLE, 1);
    if (source_[pos_] == '.') return consume(TokenType::kDOT, 1);
    if (source_[pos_] == ',') return consume(TokenType::kCOMMA, 1);
    if (source_[pos_] == '?') return consume(TokenType::kQUERY_MARK, 1);
    if (source_[pos_] == ':') return consume(TokenType::kCOLON, 1);
    if (source_[pos_] == ';') return consume(TokenType::kSEMICOLON, 1);
    if (source_[pos_] == '|') return consume(TokenType::kOR, 1);
    if (source_[pos_] == '+') return consume(TokenType::kPLUS, 1);
    if (source_[pos_] == '-') return consume(TokenType::kMINUS, 1);
    if (source_[pos_] == '*') return consume(TokenType::kTIMES, 1);
    if (source_[pos_] == '/') return consume(TokenType::kDIV, 1);
    if (source_[pos_] == '@') return consume(TokenType::kAT, 1);
    if (source_[pos_] == '(') return consume(TokenType::kPAREN_OPEN, 1);
    if (source_[pos_] == ')') return consume(TokenType::kPAREN_CLOSE, 1);
    if (source_[pos_] == '[') return consume(TokenType::kSQUARE_OPEN, 1);
    if (source_[pos_] == ']') return consume(TokenType::kSQUARE_CLOSE, 1);
    if (source_[pos_] == '{') return consume(TokenType::kCURLY_OPEN, 1);
    if (source_[pos_] == '}') return consume(TokenType::kCURLY_CLOSE, 1);
    if (source_[pos_] == '=') return consume(TokenType::kEQUAL, 1);
    if (source_[pos_] == '<') return consume(TokenType::kLESS, 1);
    if (source_[pos_] == '>') return consume(TokenType::kGREATER, 1);

    return Token{.type = TokenType::kERROR};
  }

 private:
  friend class LexerCheckpoint;

  std::size_t checkpoint() { return pos_; }
  void rewind(std::size_t checkpoint_id) { pos_ = checkpoint_id; }

  inline bool is_id_char(size_t p) {
    if (p >= source_.size()) return false;
    char c = source_[p];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  }

  inline Token consume(TokenType ttype, size_t len) {
    Token retval = Token{.type = ttype, .val = source_.substr(pos_, len)};
    pos_ += len;
    return retval;
  }

  std::size_t pos_ = 0;
  std::string_view source_;
};

class LexerCheckpoint {
 public:
  LexerCheckpoint(Lexer& lexer)
      : lexer_(lexer), checkpoint_(lexer.checkpoint()) {}

  LexerCheckpoint(const LexerCheckpoint&) = delete;
  LexerCheckpoint& operator=(const LexerCheckpoint&) = delete;

  ~LexerCheckpoint() {
    if (checkpoint_ == std::numeric_limits<std::size_t>::max()) return;
    lexer_.rewind(checkpoint_);
  }

  void commit() { checkpoint_ = std::numeric_limits<std::size_t>::max(); }

 private:
  Lexer& lexer_;
  std::size_t checkpoint_;
};

#endif  // __TOKENIZE_H__
