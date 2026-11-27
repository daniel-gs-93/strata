#include <unistd.h>

#include <iostream>
#include <optional>
#include <string>

#include "strata/repl.h"

namespace {

constexpr const char* kUsage = "usage: strata_cli [<directory> [--parallel]]\n";

}  // namespace

// `strata_cli [<directory> [--parallel]]`: reads commands and statements
// from stdin (docs/DesignDocument.md section 3.6). A directory is opened
// first, as `.open` would. Wrong arguments print the usage to stderr and exit
// with 2. A first argument that starts with `-` is wrong, so a mistyped option
// such as `-h` does not create a directory of that name; a directory whose name
// starts with `-` is given as `./-name`.
int main(int argc, char** argv) {
  std::optional<strata::cli::InitialOpen> initial_open;
  if (argc > 3 || (argc >= 2 && std::string(argv[1]).starts_with("-")) ||
      (argc == 3 && std::string(argv[2]) != "--parallel")) {
    std::cerr << kUsage;
    return 2;
  }
  if (argc >= 2) {
    initial_open = strata::cli::InitialOpen{argv[1], argc == 3};
  }
  strata::cli::RunRepl(std::cin, std::cout, ::isatty(STDIN_FILENO) != 0,
                       initial_open);
  return 0;
}
