#include "guest_input_backend.hpp"
#include "guest_input_daemon.hpp"
#include "input_transport.hpp"
#include "socket_input_transport.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --port <p>              Listen on TCP port for host bridge (default: 0 = offline loopback)\n"
              << "  --session-id <id>       Expected session ID (default: default_session)\n"
              << "  --generation-id <id>    Expected generation ID (default: gen-1)\n"
              << "  --display-width <w>     Display width in pixels (default: 1080)\n"
              << "  --display-height <h>    Display height in pixels (default: 1920)\n"
              << "  --max-contacts <n>      Max simultaneous contacts (default: 10)\n"
              << "  --backend <name>        Backend: recording, uinput, unavailable (default: recording)\n"
              << "  --help                  Show this help message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    fang::guest::GuestInputDaemon::Config config;
    std::string backend_name = "recording";
    uint16_t listen_port = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            listen_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--session-id" && i + 1 < argc) {
            config.expected_session_id = argv[++i];
        } else if (arg == "--generation-id" && i + 1 < argc) {
            config.expected_generation_id = argv[++i];
        } else if (arg == "--display-width" && i + 1 < argc) {
            config.display_width = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--display-height" && i + 1 < argc) {
            config.display_height = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-contacts" && i + 1 < argc) {
            config.max_contacts = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--backend" && i + 1 < argc) {
            backend_name = argv[++i];
        }
    }

    std::shared_ptr<fang::guest::IGuestInputBackend> backend;
    if (backend_name == "uinput") {
        backend = std::make_shared<fang::guest::LinuxUinputGuestInputBackend>();
    } else if (backend_name == "unavailable") {
        backend = std::make_shared<fang::guest::UnavailableGuestInputBackend>();
    } else {
        backend = std::make_shared<fang::guest::RecordingGuestInputBackend>();
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (listen_port > 0) {
        fang::studio::SocketInputServer server;
        if (!server.start("127.0.0.1", listen_port)) {
            std::cerr << "Failed to start socket server: " << server.error_diagnostic() << "\n";
            return 1;
        }
        std::cout << "Fang guest input daemon listening on " << server.bound_address() << ":" << server.bound_port() << "...\n";

        std::shared_ptr<fang::studio::SocketInputTransport> client_transport;
        while (g_running && !client_transport) {
            client_transport = server.accept_client(200);
        }

        if (!client_transport) {
            std::cout << "Server stopped before client connected.\n";
            return 0;
        }

        std::cout << "Host client connected from port " << client_transport->remote_port() << ".\n";
        fang::guest::GuestInputDaemon daemon(client_transport, backend, config);
        if (!daemon.start()) {
            std::cerr << "Failed to start guest input daemon: " << daemon.status_diagnostic() << "\n";
            return 1;
        }

        std::cout << "Status: " << daemon.status_diagnostic() << "\n";
        while (g_running && daemon.is_active()) {
            daemon.poll_and_process();
        }

        daemon.shutdown();
        server.stop();
        std::cout << "Fang guest input daemon stopped cleanly.\n";
        return 0;
    }

    auto transport = std::make_shared<fang::studio::LoopbackInputTransport>();
    fang::guest::GuestInputDaemon daemon(transport, backend, config);

    if (!daemon.start()) {
        std::cerr << "Failed to start guest input daemon: " << daemon.status_diagnostic() << "\n";
        return 1;
    }

    std::cout << "Fang guest input daemon initialized. Backend: " << backend->backend_name()
              << ", Session: " << config.expected_session_id
              << ", Generation: " << config.expected_generation_id << "\n";
    std::cout << "Status: " << daemon.status_diagnostic() << "\n";

    daemon.shutdown();
    std::cout << "Fang guest input daemon stopped cleanly.\n";
    return 0;
}
