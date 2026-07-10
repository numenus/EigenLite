#include <eigenapi.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>

namespace {

constexpr uint8_t kBreathCc = 2;
constexpr uint8_t kRibbonCc = 21;
constexpr uint8_t kRollCc = 74;
constexpr uint8_t kModeButtonBaseNote = 44;
constexpr float kBreathMidiGain = 6.0f;
constexpr float kParityBreathDeadband = 0.015f;
constexpr float kParityBreathGain = 4.0f;
constexpr int kParityBreathHoldTicks = 100;
constexpr unsigned long long kParityKeyDebounceUs = 20000ULL;
constexpr unsigned kParityEstimationFrames = 14;
constexpr float kParityKeyGateThreshold = 300.0f / 3192.0f;
constexpr float kParityKeyHardThreshold = 900.0f / 3192.0f;
constexpr float kParityKeyRiseThreshold = 220.0f / 3192.0f;
constexpr float kParityKeyVelocityGain = 8.0f;
constexpr float kParityKeyVelocityCurve = 0.6f;

volatile sig_atomic_t keep_running = 1;

void int_handler(int) {
    keep_running = 0;
}

static unsigned to_u7(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<unsigned>(std::lround(v * 127.0f));
}

static float scale_breath(float v) {
    v *= kBreathMidiGain;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static float scale_parity_key_velocity(float v) {
    v *= kParityKeyVelocityGain;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    v = std::pow(v, kParityKeyVelocityCurve);
    return v;
}

enum class BridgeModeKind {
    Stable,
    Parity,
};

enum class DebugScope {
    All,
    Gates,
    Controls,
};

static BridgeModeKind parse_mode(const char* raw) {
    if (raw == nullptr) {
        return BridgeModeKind::Stable;
    }

    const std::string mode(raw);
    if (mode == "stable" || mode == "simple" || mode == "current") {
        return BridgeModeKind::Stable;
    }
    if (mode == "parity") {
        return BridgeModeKind::Parity;
    }

    throw std::runtime_error("unknown bridge mode; expected stable or parity");
}

static DebugScope parse_debug_scope(const char* raw) {
    if (raw == nullptr) {
        return DebugScope::All;
    }

    const std::string scope(raw);
    if (scope == "all" || scope == "raw") {
        return DebugScope::All;
    }
    if (scope == "gates" || scope == "keys") {
        return DebugScope::Gates;
    }
    if (scope == "controls" || scope == "cc") {
        return DebugScope::Controls;
    }

    throw std::runtime_error("unknown debug scope; expected all, gates, or controls");
}

class UdpMidiOut {
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

    ~UdpMidiOut() {
        if (sock_ >= 0) {
            close(sock_);
        }
    }

    void send3(uint8_t status, uint8_t d1, uint8_t d2) {
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

class MidiBridgeImplementation {
   public:
    virtual ~MidiBridgeImplementation() = default;
    virtual const char* name() const = 0;
    virtual void on_key(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float y) = 0;
    virtual void on_button(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned key, bool active) = 0;
    virtual void on_breath(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long t, float val) = 0;
    virtual void on_strip(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned strip, float val, bool active) = 0;
};

class StableMidiBridgeImplementation : public MidiBridgeImplementation {
   public:
    StableMidiBridgeImplementation() {
        for (bool& v : key_down_) v = false;
        for (uint8_t& v : last_pressure_) v = 0xFF;
        for (uint8_t& v : last_cc_) v = 0xFF;
    }

    const char* name() const override {
        return "stable";
    }

    void on_key(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, unsigned course, unsigned key, bool active, float p, float /*r*/, float /*y*/) override {
        if (course != 0 || key >= 128) {
            return;
        }

        if (debug && debug_scope == DebugScope::All) {
            std::cout << "key course=" << course << " key=" << key << " active=" << active << " pressure=" << p << std::endl;
        }

        const uint8_t note = static_cast<uint8_t>(key + 48);
        const uint8_t vel = static_cast<uint8_t>(to_u7(p));

        if (active) {
            if (!key_down_[key]) {
                out.send3(0x90, note, vel == 0 ? 1 : vel);
                key_down_[key] = true;
            }
            send_poly_pressure(out, note, p);
        } else {
            if (key_down_[key]) {
                out.send3(0x80, note, 0);
                key_down_[key] = false;
            }
            last_pressure_[key] = 0xFF;
        }
    }

    void on_button(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, unsigned key, bool active) override {
        if (key >= 4) {
            return;
        }

        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Gates)) {
            std::cout << "button key=" << key << " active=" << active << std::endl;
        }

        const uint8_t note = static_cast<uint8_t>(kModeButtonBaseNote + key);
        if (active) {
            out.send3(0x90, note, 127);
        } else {
            out.send3(0x80, note, 0);
        }
    }

    void on_breath(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, float val) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "breath " << val << std::endl;
        }
        send_cc(out, kBreathCc, scale_breath(val), 0);
    }

    void on_strip(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, unsigned strip, float val, bool active) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "strip " << strip << " value=" << val << " active=" << active << std::endl;
        }
        send_cc(out, kRibbonCc, active ? val : 0.0f, 1);
    }

   private:
    void send_poly_pressure(UdpMidiOut& out, uint8_t note, float p) {
        const unsigned key = static_cast<unsigned>(note - 48);
        if (key >= 128) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(p));
        if (last_pressure_[key] == midi) return;
        last_pressure_[key] = midi;
        out.send3(0xA0, note, midi);
    }

    void send_cc(UdpMidiOut& out, uint8_t cc, float val, unsigned idx) {
        if (idx >= 8) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(val));
        if (last_cc_[idx] == midi) return;
        last_cc_[idx] = midi;
        out.send3(0xB0, cc, midi);
    }

    bool key_down_[128];
    uint8_t last_pressure_[128];
    uint8_t last_cc_[8];
};

class ParityMidiBridgeImplementation : public StableMidiBridgeImplementation {
   public:
    ParityMidiBridgeImplementation() {
        for (uint8_t& v : last_pressure_) v = 0xFF;
        for (uint8_t& v : last_cc_) v = 0xFF;
    }

    const char* name() const override {
        return "parity";
    }

    void on_key(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float /*y*/) override {
        if (course != 0 || key >= 128) {
            return;
        }

        if (debug && debug_scope == DebugScope::Gates) {
            std::cout << "gate event key=" << key
                      << " note=" << static_cast<unsigned>(key + 48)
                      << " active=" << active
                      << " pressure=" << p
                      << " [parity]" << std::endl;
        }

        if (debug && debug_scope == DebugScope::All) {
            std::cout << "key course=" << course << " key=" << key << " active=" << active
                      << " pressure=" << p << " [parity]" << std::endl;
        }

        // Diagnostic per-key roll, parity-mode only: last-touched key's tilt as a
        // single CC. Not gated on note-on state -- it's an auxiliary expressive
        // signal, not part of note triggering.
        if (active) {
            send_cc(out, kRollCc, (r + 1.0f) * 0.5f, 2);
        }

        auto& state = keys_[key];
        const uint8_t note = static_cast<uint8_t>(key + 48);

        if (!active) {
            if (state.note_on) {
                if (debug && debug_scope == DebugScope::Gates) {
                    std::cout << "gate note_off key=" << key << " note=" << static_cast<unsigned>(note)
                              << " max=" << state.max_pressure << " [parity]" << std::endl;
                }
                out.send3(0x80, note, 0);
            } else if (debug && debug_scope == DebugScope::Gates && state.tracking) {
                std::cout << "gate release_without_note key=" << key << " note=" << static_cast<unsigned>(note)
                          << " first=" << state.first_pressure
                          << " max=" << state.max_pressure
                          << " frames=" << state.frames
                          << " [parity]" << std::endl;
            }
            const unsigned long long release_ts = t;
            state = {};
            state.last_release_ts = release_ts;
            last_pressure_[key] = 0xFF;
            return;
        }

        if (!state.tracking) {
            if (state.last_release_ts != 0 && t < state.last_release_ts + kParityKeyDebounceUs) {
                if (debug && debug_scope == DebugScope::Gates) {
                    std::cout << "gate debounce key=" << key << " note=" << static_cast<unsigned>(note)
                              << " pressure=" << p
                              << " [parity]" << std::endl;
                }
                return;
            }
            state.tracking = true;
            state.frames = 0;
            state.first_pressure = p;
            state.max_pressure = 0.0f;
            if (debug && debug_scope == DebugScope::Gates) {
                std::cout << "gate track_start key=" << key << " note=" << static_cast<unsigned>(note)
                          << " first=" << state.first_pressure
                          << " [parity]" << std::endl;
            }
        }

        state.max_pressure = std::max(state.max_pressure, p);

        if (!state.note_on) {
            if (state.frames < kParityEstimationFrames) {
                state.frames++;
                if (state.frames < kParityEstimationFrames) {
                    if (debug && debug_scope == DebugScope::Gates && state.frames == 1) {
                        std::cout << "gate estimating key=" << key << " note=" << static_cast<unsigned>(note)
                                  << " first=" << state.first_pressure
                                  << " max=" << state.max_pressure
                                  << " [parity]" << std::endl;
                    }
                    return;
                }
            }

            const float pressure_rise = state.max_pressure - state.first_pressure;
            const float current_rise = p - state.first_pressure;
            const bool hard_gate = state.max_pressure >= kParityKeyHardThreshold;
            const bool soft_gate = state.max_pressure >= kParityKeyGateThreshold &&
                                   pressure_rise >= kParityKeyRiseThreshold &&
                                   current_rise >= (kParityKeyRiseThreshold * 0.75f);
            if (hard_gate || soft_gate) {
                const uint8_t vel = static_cast<uint8_t>(to_u7(scale_parity_key_velocity(state.max_pressure)));
                if (debug && debug_scope == DebugScope::Gates) {
                    std::cout << "gate note_on key=" << key << " note=" << static_cast<unsigned>(note)
                              << " vel=" << static_cast<unsigned>(vel == 0 ? 1 : vel)
                              << " first=" << state.first_pressure
                              << " max=" << state.max_pressure
                              << " rise=" << pressure_rise
                              << " current_rise=" << current_rise
                              << " hard=" << hard_gate
                              << " soft=" << soft_gate
                              << " [parity]" << std::endl;
                }
                out.send3(0x90, note, vel == 0 ? 1 : vel);
                state.note_on = true;
            } else {
                if (debug && debug_scope == DebugScope::Gates) {
                    std::cout << "gate reject key=" << key << " note=" << static_cast<unsigned>(note)
                              << " first=" << state.first_pressure
                              << " max=" << state.max_pressure
                              << " rise=" << pressure_rise
                              << " current_rise=" << current_rise
                              << " [parity]" << std::endl;
                }
                return;
            }
        }

        send_poly_pressure(out, key, note, p);
    }

    void on_breath(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, float val) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "breath " << val << " [parity]" << std::endl;
        }

        float shaped = 0.0f;
        if (val > kParityBreathDeadband) {
            shaped = (val - kParityBreathDeadband) * kParityBreathGain;
            if (shaped > 1.0f) shaped = 1.0f;
            breath_hold_ticks_ = kParityBreathHoldTicks;
        } else if (val < -kParityBreathDeadband) {
            breath_hold_ticks_ = kParityBreathHoldTicks;
            shaped = 0.0f;
        } else if (breath_hold_ticks_ > 0) {
            breath_hold_ticks_--;
            shaped = 0.0f;
        }

        send_cc(out, kBreathCc, shaped, 0);
    }

    void on_strip(UdpMidiOut& out, bool debug, DebugScope debug_scope, unsigned long long /*t*/, unsigned strip, float val, bool active) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "strip " << strip << " value=" << val << " active=" << active << " [parity]" << std::endl;
        }
        send_cc(out, kRibbonCc, active ? val : 0.0f, 1);
    }

   private:
    struct KeyState {
        bool tracking = false;
        bool note_on = false;
        unsigned frames = 0;
        float first_pressure = 0.0f;
        float max_pressure = 0.0f;
        unsigned long long last_release_ts = 0ULL;
    };

    void send_poly_pressure(UdpMidiOut& out, unsigned key, uint8_t note, float p) {
        if (key >= 128) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(p));
        if (last_pressure_[key] == midi) return;
        last_pressure_[key] = midi;
        out.send3(0xA0, note, midi);
    }

    void send_cc(UdpMidiOut& out, uint8_t cc, float val, unsigned idx) {
        if (idx >= 8) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(val));
        if (last_cc_[idx] == midi) return;
        last_cc_[idx] = midi;
        out.send3(0xB0, cc, midi);
    }

    KeyState keys_[128];
    uint8_t last_pressure_[128] = {0xFF};
    uint8_t last_cc_[8] = {0xFF};
    int breath_hold_ticks_ = 0;
};

static std::unique_ptr<MidiBridgeImplementation> make_bridge_implementation(BridgeModeKind mode) {
    switch (mode) {
        case BridgeModeKind::Stable:
            return std::unique_ptr<MidiBridgeImplementation>(new StableMidiBridgeImplementation());
        case BridgeModeKind::Parity:
            return std::unique_ptr<MidiBridgeImplementation>(new ParityMidiBridgeImplementation());
    }

    throw std::runtime_error("invalid bridge mode");
}

class MidiBridgeCallback : public EigenApi::LifecycleCallback, public EigenApi::Callback {
   public:
    MidiBridgeCallback(UdpMidiOut& out, bool debug, DebugScope debug_scope, std::unique_ptr<MidiBridgeImplementation> impl)
        : out_(out), debug_(debug), debug_scope_(debug_scope), impl_(std::move(impl)) {
    }

    void beginDeviceInfo() override {
    }

    void deviceInfo(bool isPico, unsigned devEnum, const char* dev) override {
        std::cout << "device " << (isPico ? "pico" : "base") << "-" << devEnum << " " << dev << std::endl;
    }

    void endDeviceInfo() override {
    }

    void connected(const char* dev, EigenApi::DeviceType dt) override {
        std::cout << "connected " << dev << " type=" << static_cast<int>(dt) << std::endl;
    }

    void disconnected(const char* dev) override {
        std::cout << "disconnected " << dev << std::endl;
    }

    void key(const char* /*dev*/, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float y) override {
        impl_->on_key(out_, debug_, debug_scope_, t, course, key, active, p, r, y);
    }

    void button(const char* /*dev*/, unsigned long long t, unsigned key, bool active) override {
        impl_->on_button(out_, debug_, debug_scope_, t, key, active);
    }

    void breath(const char* /*dev*/, unsigned long long t, float val) override {
        impl_->on_breath(out_, debug_, debug_scope_, t, val);
    }

    void strip(const char* /*dev*/, unsigned long long t, unsigned strip, float val, bool active) override {
        impl_->on_strip(out_, debug_, debug_scope_, t, strip, val, active);
    }

    const char* implementation_name() const {
        return impl_->name();
    }

   private:
    UdpMidiOut& out_;
    bool debug_;
    DebugScope debug_scope_;
    std::unique_ptr<MidiBridgeImplementation> impl_;
};

}  // namespace

int main(int argc, char** argv) {
    signal(SIGINT, int_handler);

    std::string host = "127.0.0.1";
    int port = 5005;
    bool debug = false;
    BridgeModeKind mode = BridgeModeKind::Stable;
    DebugScope debug_scope = DebugScope::All;
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
                  << std::endl;

        auto* cb = new MidiBridgeCallback(out, debug, debug_scope, make_bridge_implementation(mode));
        harp.addLifecycleCallback(cb);
        harp.addCallback(cb);

        if (!harp.start()) {
            std::cerr << "unable to start EigenLite bridge" << std::endl;
            return 1;
        }

        std::cout << "sending UDP MIDI to " << host << ":" << port
                  << " using " << cb->implementation_name() << " bridge mode" << std::endl;
        while (keep_running) {
            harp.process();
        }

        harp.stop();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
