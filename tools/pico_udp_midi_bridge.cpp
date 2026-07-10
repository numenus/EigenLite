#include "pico_midi_bridge_core.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>

namespace {

volatile sig_atomic_t keep_running = 1;

void int_handler(int) {
    keep_running = 0;
}

class UdpMidiOut : public PicoBridge::MidiSink {
   public:
    UdpMidiOut(const std::string& host, int port) : sock_(-1) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            throw std::runtime_error("unable to create UDP socket");
        }

        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr_.sin_addr) != 1) {
            close(sock_);
            throw std::runtime_error("invalid IPv4 host");
        }
    }

    ~UdpMidiOut() override {
        if (sock_ >= 0) {
            close(sock_);
        }
    }

    void send3(uint8_t status, uint8_t d1, uint8_t d2) override {
        unsigned char msg[3] = {status, d1, d2};
        ssize_t rc = sendto(sock_, msg, sizeof(msg), 0, reinterpret_cast<sockaddr*>(&addr_), sizeof(addr_));
        if (rc != 3) {
            std::cerr << "warning: UDP send failed: " << std::strerror(errno) << std::endl;
        }
    }

   private:
    int sock_;
    sockaddr_in addr_;
};

// Non-blocking UDP listener for the reverse (Bitwig -> Pico) LED control
// channel. Windows forwards Bitwig's outbound MIDI here instead of
// discarding it (see tools/udp_midi_sink.py).
class UdpMidiIn {
   public:
    explicit UdpMidiIn(int port) : sock_(-1) {
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            throw std::runtime_error("unable to create UDP listen socket");
        }

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(sock_);
            throw std::runtime_error("unable to bind UDP listen socket");
        }
    }

    ~UdpMidiIn() {
        if (sock_ >= 0) {
            close(sock_);
        }
    }

    // Drains at most one pending 3-byte MIDI message per call. Returns false
    // if nothing was available.
    bool poll(uint8_t msg[3]) {
        unsigned char buf[3];
        ssize_t rc = recv(sock_, buf, sizeof(buf), MSG_DONTWAIT);
        if (rc != 3) {
            return false;
        }
        std::memcpy(msg, buf, 3);
        return true;
    }

   private:
    int sock_;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace PicoBridge;

    signal(SIGINT, int_handler);

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

    try {
        UdpMidiOut out(host, port);
        EigenApi::FWR_Embedded fwr;
        EigenApi::Eigenharp harp(&fwr);
        harp.setPollTime(100);

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
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
