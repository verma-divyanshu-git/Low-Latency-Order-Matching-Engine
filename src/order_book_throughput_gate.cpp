#include "matching_engine/throughput_gate.hpp"

#include <exception>
#include <fstream>
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
    const auto config = matching_engine::throughput_gate::parse_cli(arguments, &error);
    if (!config.has_value()) {
      std::cerr << "order_book_throughput_gate: " << error << '\n';
      return 2;
    }
    const auto result = matching_engine::throughput_gate::run(*config, error);
    if (!result.has_value()) {
      std::cerr << "order_book_throughput_gate: " << error << '\n';
      return 1;
    }
    const auto json = matching_engine::throughput_gate::statistics_json(
        *result, config->minimum_ops_per_second, config->maximum_relative_mad);
    if (!json.has_value()) {
      std::cerr << "order_book_throughput_gate: JSON contains invalid numeric values\n";
      return 1;
    }
    std::cout << *json << '\n';
    if (config->output.has_value()) {
      std::ofstream output{*config->output, std::ios::out | std::ios::trunc};
      if (!output) {
        std::cerr << "order_book_throughput_gate: cannot open output file\n";
        return 1;
      }
      output << *json << '\n';
      output.flush();
      output.close();
      if (output.fail()) {
        std::cerr << "order_book_throughput_gate: cannot write output file\n";
        return 1;
      }
    }
    return result->gate_passed ? 0 : 1;
  } catch (const std::exception& exception) {
    std::cerr << "order_book_throughput_gate: " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "order_book_throughput_gate: unknown failure\n";
    return 1;
  }
}
