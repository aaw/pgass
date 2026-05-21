#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/check.h"
#include "safety.h"
#include "parse.h"

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Parse and ground an ASP program.\n"
      "Usage: pgass [source_file ...]\n"
      "  Files are concatenated; reads from stdin if none are given.");
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);

  std::string source;
  if (positional.size() <= 1) {
    source = std::string(std::istreambuf_iterator<char>(std::cin),
                         std::istreambuf_iterator<char>());
  } else {
    for (size_t i = 1; i < positional.size(); ++i) {
      std::ifstream f(positional[i]);
      if (!f) {
        std::cerr << "pgass: cannot open '" << positional[i] << "'\n";
        return 1;
      }
      if (!source.empty()) source += '\n';
      source += std::string(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>());
    }
  }

  Parser parser(source);
  auto program = parser.parse_program();
  if (!program.ok()) {
    std::cerr << "pgass: parse error: " << program.status().message() << "\n";
    return 1;
  }

  CHECK(verify_safe(**program));
  return 0;
}
