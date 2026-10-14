#include "host_timing.hpp"

#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto parsed = fang::host_timing::parse_arguments(arguments);
    if (parsed.show_help) {
        fang::host_timing::print_usage(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "error: " << parsed.error << '\n';
        fang::host_timing::print_usage(std::cerr);
        return 2;
    }

    if (parsed.options.output_file.empty()) {
        return fang::host_timing::run_benchmark(parsed.options, std::cout, std::cerr);
    }

    std::ofstream output(parsed.options.output_file, std::ios::binary);
    if (!output) {
        std::cerr << "error: cannot open output file: " << parsed.options.output_file.string() << '\n';
        return 2;
    }
    return fang::host_timing::run_benchmark(parsed.options, output, std::cerr);
}