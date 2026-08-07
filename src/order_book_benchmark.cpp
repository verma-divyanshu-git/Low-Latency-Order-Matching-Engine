#include "matching_engine/benchmark.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  try {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    std::string error;
    const auto config = matching_engine::benchmark::parse_cli(arguments, &error);
    if (!config.has_value()) {
      std::cerr << "order_book_benchmark: " << error << '\n';
      return 2;
    }
    const auto result = matching_engine::benchmark::run(*config, error);
    if (!result.has_value()) {
      std::cerr << "order_book_benchmark: " << error << '\n';
      return 1;
    }
    std::cout << result->summary_path.string() << '\n';
    std::cerr << "claim_scope="
              << matching_engine::benchmark::claim_scope_name(result->summary.claim_scope)
              << " reason=" << result->summary.publication_reason << '\n';
    if (result->corrected_summary.has_value()) {
      std::cerr << "diagnostic raw and coordinated-omission-corrected histograms are separate; "
                   "neither is an engine claim\n";
    }
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "order_book_benchmark: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "order_book_benchmark: unknown failure\n";
    return 1;
  }
}
