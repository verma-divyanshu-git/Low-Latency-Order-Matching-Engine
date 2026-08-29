#include "matching_engine/benchmark_comparison.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  std::string error;
  const auto config = matching_engine::benchmark_comparison::parse_cli(arguments, &error);
  if (!config.has_value()) {
    std::cerr << "benchmark_comparison: " << error << '\n';
    return 2;
  }
  const auto report = matching_engine::benchmark_comparison::run(*config, error);
  if (!report.has_value()) {
    std::cerr << "benchmark_comparison: " << error << '\n';
    return 1;
  }
  const auto json = matching_engine::benchmark_comparison::report_json(*report);
  if (!json.has_value()) {
    std::cerr << "benchmark_comparison: invalid report\n";
    return 1;
  }
  std::cout << *json << '\n';
  if (config->output.has_value()) {
    std::ofstream output{*config->output, std::ios::out | std::ios::trunc};
    output << *json << '\n';
    output.close();
    if (output.fail()) {
      std::cerr << "benchmark_comparison: output failure\n";
      return 1;
    }
  }
  return 0;
}
