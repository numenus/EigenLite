#include "pico_midi_bridge_core.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <signal.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <chrono>
#include <iostream>
#include <string>

namespace {

// Minimal portability layer over BSD sockets vs winsock. Behaviour is
// identical; on Windows non-blocking recv needs FIONBIO on the socket since
// there is no MSG_DONTWAIT flag.
#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kInvalidSocket = INVALID_SOCKET;

void close_socket(socket_t s) {
    closesocket(s);
}

bool set_nonblocking(socket_t s) {
    u_long enabled = 1;
    return ioctlsocket(s, FIONBIO, &enabled) == 0;
}

int recv_nonblocking(socket_t s, unsigned char* buf, int len) {
    return recv(s, reinterpret_cast<char*>(buf), len, 0);
}

int send_udp(socket_t s, const unsigned char* buf, int len, const sockaddr_in& addr) {
    return sendto(s, reinterpret_cast<const char*>(buf), len, 0,
                  reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~WinsockInit() { WSACleanup(); }
};
#else
using socket_t = int;
const socket_t kInvalidSocket = -1;

void close_socket(socket_t s) {
    close(s);
}

bool set_nonblocking(socket_t) {
    return true;  // POSIX path uses MSG_DONTWAIT per-call instead
}

int recv_nonblocking(socket_t s, unsigned char* buf, int len) {
    return static_cast<int>(recv(s, buf, len, MSG_DONTWAIT));
}

int send_udp(socket_t s, const unsigned char* buf, int len, const sockaddr_in& addr) {
    return static_cast<int>(sendto(s, buf, len, 0,
                                   reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
}

struct WinsockInit {};
#endif

volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t clean_exit_done = 0;

void int_handler(int) {
    keep_running = 0;
}

#ifdef _WIN32
// window close / logoff / taskkill (non-force) arrive here, not as SIGINT.
// A hard kill mid-stream resets the Pico (firmware drops, next start pays
// the ~30s reload), so shut down cleanly and hold the close until the main
// loop has stopped the harp (Windows allows ~5s after the handler returns).
BOOL WINAPI console_ctrl_handler(DWORD /*type*/) {
    keep_running = 0;
    for (int i = 0; i < 40 && !clean_exit_done; ++i) {
        Sleep(100);
    }
    return TRUE;
}
#endif

// EigenLite's internal (picross) log stream, filtered. Without --debug the
// known-noise lines are dropped: per-second enumerator polling, and the
// non-fatal isochronous "frame out of order" diagnostics (present on both
// usbipd and native Windows; data is still processed).
bool g_verbose_internal_logs = false;

// seconds since program start, for locating startup stalls
double uptime_seconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void bridge_log_filter(const char* msg) {
    if (!g_verbose_internal_logs) {
        if (std::strstr(msg, "frame out of order") != nullptr ||
            std::strstr(msg, "enumerate : searching") != nullptr ||
            std::strstr(msg, "enumerate found") != nullptr ||
            std::strstr(msg, "availableDevices found") != nullptr) {
            return;
        }
        // an unplugged device floods failed-transfer callbacks until it is
        // torn down; print the first few then sample
        if (std::strstr(msg, "completed unsuccessful") != nullptr) {
            static unsigned long transfer_errors = 0;
            ++transfer_errors;
            if (transfer_errors > 3 && transfer_errors % 250 != 0) {
                return;
            }
            std::cerr << "[+" << uptime_seconds() << "s] log:" << msg << " [x" << transfer_errors << "]" << std::endl;
            return;
        }
    }
    std::cerr << "[+" << uptime_seconds() << "s] log:" << msg << std::endl;
}

class UdpMidiOut : public PicoBridge::MidiSink {
   public:
    UdpMidiOut(const std::string& host, int port) : sock_(kInvalidSocket) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kInvalidSocket) {
            throw std::runtime_error("unable to create UDP socket");
        }

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
            close_socket(sock_);
            throw std::runtime_error("invalid IPv4 host");
        }
    }

    ~UdpMidiOut() override {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
        }
    }

    void send3(uint8_t status, uint8_t d1, uint8_t d2) override {
        unsigned char msg[3] = {status, d1, d2};
        int rc = send_udp(sock_, msg, sizeof(msg), addr_);
        if (rc != 3) {
            std::cerr << "warning: UDP send failed" << std::endl;
        }
    }

   private:
    socket_t sock_;
    sockaddr_in addr_;
};

// Non-blocking UDP listener for the reverse (Bitwig -> Pico) LED control
// channel. Windows forwards Bitwig's outbound MIDI here instead of
// discarding it (see tools/udp_midi_sink.py).
class UdpMidiIn {
   public:
    explicit UdpMidiIn(int port) : sock_(kInvalidSocket) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kInvalidSocket) {
            throw std::runtime_error("unable to create UDP listen socket");
        }

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(sock_);
            throw std::runtime_error("unable to bind UDP listen socket");
        }
        if (!set_nonblocking(sock_)) {
            close_socket(sock_);
            throw std::runtime_error("unable to make UDP listen socket non-blocking");
        }
    }

    ~UdpMidiIn() {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
        }
    }

    // Drains at most one pending 3-byte MIDI message per call. Returns false
    // if nothing was available.
    bool poll(uint8_t msg[3]) {
        unsigned char buf[3];
        int rc = recv_nonblocking(sock_, buf, sizeof(buf));
        if (rc != 3) {
            return false;
        }
        std::memcpy(msg, buf, 3);
        return true;
    }

   private:
    socket_t sock_;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace PicoBridge;

    signal(SIGINT, int_handler);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    std::string host = "127.0.0.1";
    int port = 5005;
    bool debug = false;
    BridgeModeKind mode = BridgeModeKind::Stable;
    DebugScope debug_scope = DebugScope::All;
    int led_port = 0;  // 0 = LED control channel disabled
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
    }
    if (argc >= 4) {
        debug = std::atoi(argv[3]) != 0;
    }
    if (argc >= 5) {
        mode = parse_mode(argv[4]);
    }
    if (argc >= 6) {
        debug_scope = parse_debug_scope(argv[5]);
    }
    if (argc >= 7) {
        led_port = std::atoi(argv[6]);
    }

    g_verbose_internal_logs = debug;
    EigenApi::Logger::setLogFunc(bridge_log_filter);

    try {
        WinsockInit winsock;
        (void)winsock;
        UdpMidiOut out(host, port);
        EigenApi::FWR_Embedded fwr;
        EigenApi::Eigenharp harp(&fwr);
        harp.setPollTime(100);
        // pico only: skips basestation scanning entirely (4 of the 6
        // enumerate calls per discovery pass, ~2s each on Windows)
        harp.setDeviceFilter(2, 0);

        const char* debug_scope_name = "all";
        switch (debug_scope) {
            case DebugScope::All:
                debug_scope_name = "all";
                break;
            case DebugScope::Gates:
                debug_scope_name = "gates";
                break;
            case DebugScope::Controls:
                debug_scope_name = "controls";
                break;
        }

        std::cout << "bridge start host=" << host
                  << " port=" << port
                  << " debug=" << (debug ? 1 : 0)
                  << " mode=" << (mode == BridgeModeKind::Parity ? "parity" : "stable")
                  << " scope=" << debug_scope_name
                  << " led_port=" << led_port
                  << std::endl;

        auto* cb = new MidiBridgeCallback(out, debug, debug_scope, make_bridge_implementation(mode), &harp);
        harp.addLifecycleCallback(cb);
        harp.addCallback(cb);

        if (!harp.start()) {
            std::cerr << "unable to start EigenLite bridge" << std::endl;
            return 1;
        }

        std::unique_ptr<UdpMidiIn> led_in;
        if (led_port > 0) {
            led_in.reset(new UdpMidiIn(led_port));
            std::cout << "listening for LED control on UDP " << led_port << std::endl;
        }

        std::cout << "sending UDP MIDI to " << host << ":" << port
                  << " using " << cb->implementation_name() << " bridge mode" << std::endl;
        while (keep_running) {
            harp.process();
            if (led_in) {
                uint8_t msg[3];
                while (led_in->poll(msg)) {
                    cb->led_control(msg);
                }
            }
        }

        harp.stop();
        clean_exit_done = 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
