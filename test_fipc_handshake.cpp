#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// FIPC Protocol constants (matching input_ipc_protocol.hpp)
constexpr uint32_t FIPC_MAGIC = 0x46495043; // "FIPC"
constexpr uint16_t FIPC_VERSION = 1;
constexpr size_t WIRE_HEADER_SIZE = 32;

enum class InputMessageType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    TouchFrame = 3,
    KeyEvent = 4,
    CancelAll = 5,
    Ping = 6,
    Pong = 7,
    Disconnect = 8,
    Error = 9
};

enum class InputProtocolError : uint8_t {
    None = 0,
    InvalidMagic = 1,
    UnsupportedVersion = 2,
    UnknownMessageType = 3,
    OversizedPayload = 4,
    InvalidCoordinates = 5,
    InvalidContactId = 6,
    ExceededContactLimit = 7,
    StaleGeneration = 8,
    SessionMismatch = 9,
    QueueOverflow = 10,
    ConnectionClosed = 11
};

struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint64_t sequence_number;
    uint64_t timestamp_ms;
    uint64_t reserved;
};

struct HelloPayload {
    uint16_t client_version;
    uint32_t display_width;
    uint32_t display_height;
    std::string session_id;
    std::string generation_id;
};

struct HelloAckPayload {
    uint16_t guest_version;
    uint8_t status;
    uint8_t max_contacts;
    uint32_t supported_caps;
    std::string session_id;
    std::string generation_id;
};

// Simple string encoding for FIPC
std::vector<uint8_t> encode_string(const std::string& str) {
    std::vector<uint8_t> result;
    uint32_t len = static_cast<uint32_t>(str.size());
    result.push_back((len >> 24) & 0xFF);
    result.push_back((len >> 16) & 0xFF);
    result.push_back((len >> 8) & 0xFF);
    result.push_back(len & 0xFF);
    result.insert(result.end(), str.begin(), str.end());
    return result;
}

std::string decode_string(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 4 > data.size()) return "";
    uint32_t len = (data[offset] << 24) | (data[offset+1] << 16) | (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    if (offset + len > data.size()) return "";
    std::string result(reinterpret_cast<const char*>(data.data() + offset), len);
    offset += len;
    return result;
}

std::vector<uint8_t> encode_wire_header(const WireHeader& header) {
    std::vector<uint8_t> result(WIRE_HEADER_SIZE);
    result[0] = (header.magic >> 24) & 0xFF;
    result[1] = (header.magic >> 16) & 0xFF;
    result[2] = (header.magic >> 8) & 0xFF;
    result[3] = header.magic & 0xFF;
    result[4] = (header.version >> 8) & 0xFF;
    result[5] = header.version & 0xFF;
    result[6] = (header.type >> 8) & 0xFF;
    result[7] = header.type & 0xFF;
    result[8] = (header.payload_size >> 24) & 0xFF;
    result[9] = (header.payload_size >> 16) & 0xFF;
    result[10] = (header.payload_size >> 8) & 0xFF;
    result[11] = header.payload_size & 0xFF;
    for (int i = 0; i < 8; ++i) {
        result[12 + i] = (header.sequence_number >> (56 - i * 8)) & 0xFF;
    }
    for (int i = 0; i < 8; ++i) {
        result[20 + i] = (header.timestamp_ms >> (56 - i * 8)) & 0xFF;
    }
    for (int i = 0; i < 8; ++i) {
        result[28 + i] = (header.reserved >> (56 - i * 8)) & 0xFF;
    }
    return result;
}

bool decode_wire_header(const std::vector<uint8_t>& data, WireHeader& header) {
    if (data.size() < WIRE_HEADER_SIZE) return false;
    header.magic = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    header.version = (data[4] << 8) | data[5];
    header.type = (data[6] << 8) | data[7];
    header.payload_size = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
    header.sequence_number = 0;
    for (int i = 0; i < 8; ++i) {
        header.sequence_number = (header.sequence_number << 8) | data[12 + i];
    }
    header.timestamp_ms = 0;
    for (int i = 0; i < 8; ++i) {
        header.timestamp_ms = (header.timestamp_ms << 8) | data[20 + i];
    }
    header.reserved = 0;
    for (int i = 0; i < 8; ++i) {
        header.reserved = (header.reserved << 8) | data[28 + i];
    }
    return true;
}

std::vector<uint8_t> encode_hello(const HelloPayload& hello) {
    std::vector<uint8_t> payload;
    payload.push_back((hello.client_version >> 8) & 0xFF);
    payload.push_back(hello.client_version & 0xFF);
    payload.push_back((hello.display_width >> 24) & 0xFF);
    payload.push_back((hello.display_width >> 16) & 0xFF);
    payload.push_back((hello.display_width >> 8) & 0xFF);
    payload.push_back(hello.display_width & 0xFF);
    payload.push_back((hello.display_height >> 24) & 0xFF);
    payload.push_back((hello.display_height >> 16) & 0xFF);
    payload.push_back((hello.display_height >> 8) & 0xFF);
    payload.push_back(hello.display_height & 0xFF);
    auto sid = encode_string(hello.session_id);
    auto gid = encode_string(hello.generation_id);
    payload.insert(payload.end(), sid.begin(), sid.end());
    payload.insert(payload.end(), gid.begin(), gid.end());
    return payload;
}

bool decode_hello_ack(const std::vector<uint8_t>& payload, HelloAckPayload& ack) {
    size_t offset = 0;
    if (payload.size() < 2 + 1 + 1 + 4) return false;
    ack.guest_version = (payload[offset] << 8) | payload[offset+1];
    offset += 2;
    ack.status = payload[offset++];
    ack.max_contacts = payload[offset++];
    ack.supported_caps = (payload[offset] << 24) | (payload[offset+1] << 16) | (payload[offset+2] << 8) | payload[offset+3];
    offset += 4;
    ack.session_id = decode_string(payload, offset);
    ack.generation_id = decode_string(payload, offset);
    return true;
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    // Connect to forwarded port
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(24800);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Set connect timeout
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    
    int connectResult = connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (connectResult == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            std::cerr << "Connect failed: " << error << std::endl;
            return 1;
        }
    }

    // Wait for connect with timeout
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval timeout = { 5, 0 };
    int selectResult = select(0, nullptr, &writeSet, nullptr, &timeout);
    if (selectResult <= 0) {
        std::cerr << "Connection timeout" << std::endl;
        return 1;
    }

    // Check for connect error
    int soError = 0;
    socklen_t len = sizeof(soError);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &len);
    if (soError != 0) {
        std::cerr << "Connection error: " << soError << std::endl;
        return 1;
    }

    // Set back to blocking
    nonBlocking = 0;
    ioctlsocket(sock, FIONBIO, &nonBlocking);

    // Set receive timeout
    DWORD recvTimeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&recvTimeout, sizeof(recvTimeout));

    std::cout << "Connected to daemon on port 24800" << std::endl;

    // Send Hello
    HelloPayload hello;
    hello.client_version = FIPC_VERSION;
    hello.display_width = 1080;
    hello.display_height = 1920;
    hello.session_id = "test-session";
    hello.generation_id = "test-gen";

    auto helloPayload = encode_hello(hello);
    WireHeader helloHeader;
    helloHeader.magic = FIPC_MAGIC;
    helloHeader.version = FIPC_VERSION;
    helloHeader.type = static_cast<uint16_t>(InputMessageType::Hello);
    helloHeader.payload_size = static_cast<uint32_t>(helloPayload.size());
    helloHeader.sequence_number = 1;
    helloHeader.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    helloHeader.reserved = 0;

    auto helloHeaderBytes = encode_wire_header(helloHeader);
    std::vector<uint8_t> helloPacket;
    helloPacket.insert(helloPacket.end(), helloHeaderBytes.begin(), helloHeaderBytes.end());
    helloPacket.insert(helloPacket.end(), helloPayload.begin(), helloPayload.end());

    int sent = send(sock, reinterpret_cast<const char*>(helloPacket.data()), helloPacket.size(), 0);
    if (sent != static_cast<int>(helloPacket.size())) {
        std::cerr << "Failed to send Hello packet" << std::endl;
        return 1;
    }
    std::cout << "Sent Hello packet (" << helloPacket.size() << " bytes)" << std::endl;

    // Receive HelloAck
    std::vector<uint8_t> buffer(4096);
    int received = recv(sock, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0);
    if (received <= 0) {
        std::cerr << "Failed to receive HelloAck" << std::endl;
        return 1;
    }
    buffer.resize(received);
    std::cout << "Received " << received << " bytes" << std::endl;

    // Decode header
    WireHeader respHeader;
    if (!decode_wire_header(buffer, respHeader)) {
        std::cerr << "Failed to decode response header" << std::endl;
        return 1;
    }

    std::cout << "Response header: magic=0x" << std::hex << respHeader.magic 
              << ", version=" << std::dec << respHeader.version
              << ", type=" << respHeader.type
              << ", payload_size=" << respHeader.payload_size
              << ", seq=" << respHeader.sequence_number << std::endl;

    if (respHeader.magic != FIPC_MAGIC) {
        std::cerr << "Invalid magic: 0x" << std::hex << respHeader.magic << std::endl;
        return 1;
    }
    if (respHeader.version != FIPC_VERSION) {
        std::cerr << "Version mismatch: " << respHeader.version << std::endl;
        return 1;
    }
    if (respHeader.type != static_cast<uint16_t>(InputMessageType::HelloAck)) {
        std::cerr << "Expected HelloAck, got type " << respHeader.type << std::endl;
        return 1;
    }

    // Decode HelloAck payload
    if (buffer.size() < WIRE_HEADER_SIZE + respHeader.payload_size) {
        std::cerr << "Incomplete payload" << std::endl;
        return 1;
    }

    std::vector<uint8_t> ackPayload(buffer.begin() + WIRE_HEADER_SIZE, 
                                     buffer.begin() + WIRE_HEADER_SIZE + respHeader.payload_size);
    HelloAckPayload ack;
    if (!decode_hello_ack(ackPayload, ack)) {
        std::cerr << "Failed to decode HelloAck payload" << std::endl;
        return 1;
    }

    std::cout << "HelloAck received:" << std::endl;
    std::cout << "  guest_version: " << ack.guest_version << std::endl;
    std::cout << "  status: " << static_cast<int>(ack.status) << std::endl;
    std::cout << "  max_contacts: " << static_cast<int>(ack.max_contacts) << std::endl;
    std::cout << "  supported_caps: 0x" << std::hex << ack.supported_caps << std::dec << std::endl;
    std::cout << "  session_id: " << ack.session_id << std::endl;
    std::cout << "  generation_id: " << ack.generation_id << std::endl;

    // Validate handshake
    if (ack.status != 0) {
        std::cerr << "Handshake rejected with status: " << static_cast<int>(ack.status) << std::endl;
        return 1;
    }
    if (ack.session_id != hello.session_id) {
        std::cerr << "Session ID mismatch" << std::endl;
        return 1;
    }
    if (ack.generation_id != hello.generation_id) {
        std::cerr << "Generation ID mismatch" << std::endl;
        return 1;
    }
    if (ack.guest_version != FIPC_VERSION) {
        std::cerr << "Protocol version mismatch" << std::endl;
        return 1;
    }

    std::cout << "FIPC handshake SUCCESSFUL - ProtocolReady state achieved!" << std::endl;

    // Clean up
    shutdown(sock, SD_BOTH);
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif

    return 0;
}