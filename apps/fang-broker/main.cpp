#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "broker_pipe.hpp"

#include <atomic>
#include <iostream>
#include <string>

namespace {

std::atomic<fang::broker::BrokerPipeServer*> active_server{};

BOOL WINAPI control_handler(DWORD control) {
    if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT &&
        control != CTRL_CLOSE_EVENT && control != CTRL_LOGOFF_EVENT &&
        control != CTRL_SHUTDOWN_EVENT) {
        return FALSE;
    }
    auto* server = active_server.load(std::memory_order_acquire);
    if (server != nullptr) {
        server->request_stop();
    }
    return TRUE;
}

int report_error(const fang::broker::BrokerError& error) {
    std::cerr << "broker_error code=" << static_cast<unsigned int>(error.code)
              << " win32=" << error.native_error
              << " message=" << error.message << '\n';
    return 1;
}

}

int wmain() {
    using namespace fang::broker;

    auto identity = current_user_identity();
    if (!identity.succeeded) {
        return report_error(identity.error);
    }
    const auto pipe_name = pipe_name_for_user(identity.identity.sid);
    if (pipe_name.empty()) {
        return report_error({BrokerErrorCode::InternalFailure, ERROR_INVALID_SID,
            "Could not construct the user-scoped broker pipe name."});
    }

    BrokerLifecycle lifecycle;
    BrokerProtocolService protocol(lifecycle);
    BrokerPipeServer server(pipe_name, identity.identity.sid, lifecycle, protocol);
    if (SetConsoleCtrlHandler(control_handler, TRUE) == 0) {
        return report_error({BrokerErrorCode::InternalFailure, GetLastError(),
            "Could not register the broker console shutdown handler."});
    }

    active_server.store(&server, std::memory_order_release);
    std::cout << "broker_starting\n";
    const auto result = server.run();
    active_server.store(nullptr, std::memory_order_release);
    SetConsoleCtrlHandler(control_handler, FALSE);

    if (result) {
        return report_error(result);
    }
    std::cout << "broker_stopped\n";
    return 0;
}
