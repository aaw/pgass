#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "format.h"
#include "normalize.h"
#include "parse.h"
#include "safety.h"

ABSL_FLAG(bool, format, false, "Format the program and print to stdout");

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Parse and ground an ASP program.\n"
      "Usage: pgass [--format] [source_file ...]\n"
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
      source =
          absl::StrCat(source, std::string(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>()));
    }
  }

  Parser parser(source);
  auto program = parser.parse_program();
  if (!program.ok()) {
    std::cerr << "pgass: parse error: " << program.status().message() << "\n";
    return 1;
  }

  if (absl::GetFlag(FLAGS_format)) {
    std::cout << format(**program);
    return 0;
  }

  auto safety = verify_safe(**program);
  if (!safety.ok()) {
    std::cerr << "pgass: safety error: " << safety.message() << "\n";
    return 1;
  }
  return 0;

  auto normal = normalize(**program);
  if (!normal.ok()) {
    std::cerr << "pgass: normalization error: " << normal.message() << "\n";
    return 1;
  }
  return 0;
}
