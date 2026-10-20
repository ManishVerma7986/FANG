#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

void write_parallel_streams(const std::string& stdout_prefix) {
    const std::string stdout_payload = stdout_prefix + std::string(1024 * 1024, 'o');
    const std::string stderr_payload = "FAKE_STDERR_MARKER\n" + std::string(1024 * 1024, 'e');
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
    std::size_t stdout_offset = 0;
    std::size_t stderr_offset = 0;
    while (stdout_offset < stdout_payload.size() || stderr_offset < stderr_payload.size()) {
        if (stdout_offset < stdout_payload.size()) {
            DWORD written{};
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(4096, stdout_payload.size() - stdout_offset));
            if (!WriteFile(stdout_handle, stdout_payload.data() + stdout_offset, chunk, &written, nullptr) || written == 0) {
                return;
            }
            stdout_offset += written;
        }
        if (stderr_offset < stderr_payload.size()) {
            DWORD written{};
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(4096, stderr_payload.size() - stderr_offset));
            if (!WriteFile(stderr_handle, stderr_payload.data() + stderr_offset, chunk, &written, nullptr) || written == 0) {
                return;
            }
            stderr_offset += written;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--sentinel") {
        Sleep(INFINITE);
        return 0;
    }

    const std::string mode = env_value("FANG_FAKE_ADB_MODE");
    const std::string operation = argc > 3 ? argv[3] : "";
    const std::string subcommand = argc > 4 ? argv[4] : "";
    const std::string log_path = env_value("FANG_FAKE_ADB_LOG");
    if (!log_path.empty()) {
        std::ofstream log(log_path, std::ios::binary | std::ios::app);
        log << "PID " << GetCurrentProcessId() << " MODE " << mode << '\n';
        for (int index = 1; index < argc; ++index) {
            log << "ARG " << index << " " << std::string(argv[index]).size() << " " << argv[index] << '\n';
        }
    }

    if (mode == "timeout" && operation == "devices") {
        Sleep(1200);
    }
    if (mode == "delay" && operation == "devices") {
        Sleep(1200);
    }
    if (operation == "devices") {
        if (mode == "nonzero") {
            std::cerr << "FAKE_NONZERO_STDERR" << std::endl;
            return 23;
        }
        if (mode == "empty") {
            return 0;
        }
        if (mode == "malformed") {
            std::cout << "emulator-5554 unauthorized\n";
            return 0;
        }
        std::cout << "List of devices attached\nemulator-5554\tdevice\n";
        return 0;
    }
    if (operation == "shell" && subcommand == "getprop") {
        if (mode == "streams") {
            write_parallel_streams("x86_64\n");
        } else {
            std::cout << "x86_64\n";
        }
        return 0;
    }
    if (operation == "push") {
        std::cout << "fake push accepted\n";
        return 0;
    }
    if (operation == "shell" && subcommand == "stat") {
        std::cout << "8\n";
        return 0;
    }
    if (operation == "shell" && subcommand == "sha256sum") {
        if (mode == "streams") {
            write_parallel_streams(std::string(64, 'a') + "  /fake/path\n");
        } else {
            std::cout << std::string(64, 'a') << "  " << (argc > 5 ? argv[5] : "") << '\n';
        }
        return 0;
    }
    return 31;
}