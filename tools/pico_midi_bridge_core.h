#pragma once

// Pico-to-Bitwig bridge core: MIDI mapping (stable/parity) and LED state,
// decoupled from live hardware and UDP sockets so it can be driven directly
// by unit tests (tests/PicoMidiBridgeTest.cpp). tools/pico_udp_midi_bridge.cpp
// wires this up to a real EigenApi::Eigenharp and UDP sockets.

#include <eigenapi.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <stdexcept>

namespace PicoBridge {

constexpr uint8_t kBreathCc = 2;
constexpr uint8_t kRibbonCc = 21;
constexpr uint8_t kRibbonRelativeCc = 22;
constexpr uint8_t kRollCc = 74;
constexpr uint8_t kModeButtonBaseNote = 44;
// LED control protocol (Bitwig -> Pico), channel 16:
// Note On (0x9F): note = key/button index (0-17 main, 18-21 mode buttons),
// velocity picks the colour by thirds (0=off, 1-42 green, 43-84 red,
// 85-127 orange). Note Off (0x8F) clears the key, so LEDs simply follow
// held/sequenced DAW notes. See docs/reference/pico-bitwig-midi.md.
constexpr uint8_t kLedControlStatus = 0x9F;
constexpr uint8_t kLedControlOffStatus = 0x8F;
constexpr unsigned kLedIndexCount = 22;
constexpr float kBreathMidiGain = 6.0f;
constexpr float kParityBreathDeadband = 0.015f;
constexpr float kParityBreathGain = 4.0f;
constexpr int kParityBreathHoldTicks = 100;
constexpr unsigned long long kParityKeyDebounceUs = 20000ULL;
constexpr unsigned kParityEstimationFrames = 14;
constexpr float kParityKeyGateThreshold = 300.0f / 3192.0f;
constexpr float kParityKeyHardThreshold = 900.0f / 3192.0f;
constexpr float kParityKeyRiseThreshold = 220.0f / 3192.0f;
// velocity = where max attack pressure lands between the gate threshold
// (-> floor velocity) and full-scale pressure (-> 127); the old x8 gain
// saturated at pressure 0.125, compressing all play into vel 107-127
constexpr float kParityKeyVelocityFullPressure = 0.75f;
constexpr float kParityKeyVelocityFloor = 0.08f;  // gated notes never inaudible
constexpr float kParityKeyVelocityCurve = 0.6f;
// relative roll: CC74 = 64 + (roll - roll at note start) * gain; raise the
// gain if rocking barely moves the CC, lower if it pegs too easily
constexpr float kParityRollGain = 1.0f;
// stuck-note watchdog: the device streams events for a held key every frame,
// so a sounding note whose key has gone silent this long -- while other
// events (breath streams constantly) still flow -- lost its release; the
// closed decoder drops key tracking without a key-up when skipped iso frames
// force a resync. Force the note off and re-arm the key so the next press
// retriggers cleanly.
constexpr unsigned long long kStuckNoteTimeoutUs = 250000ULL;

inline unsigned to_u7(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<unsigned>(std::lround(v * 127.0f));
}

inline float scale_breath(float v) {
    v *= kBreathMidiGain;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

inline float scale_parity_key_velocity(float v) {
    v = (v - kParityKeyGateThreshold) /
        (kParityKeyVelocityFullPressure - kParityKeyGateThreshold);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    v = std::pow(v, kParityKeyVelocityCurve);
    return kParityKeyVelocityFloor + v * (1.0f - kParityKeyVelocityFloor);
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

inline BridgeModeKind parse_mode(const char* raw) {
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

// Maps to EigenApi::Eigenharp::setDeviceFilter's first argument:
// 0 = scan everything, 1 = basestations (Alpha/Tau) only, 2 = pico only.
// The bridge defaults to pico-only because each USB enumerate pass costs
// ~2s on Windows and basestation scanning doubles-plus the startup time;
// Alpha/Tau owners pass "all".
enum class DeviceFilterKind : unsigned {
    All = 0,
    BaseStation = 1,
    Pico = 2,
};

inline DeviceFilterKind parse_device_filter(const char* raw) {
    if (raw == nullptr) {
        return DeviceFilterKind::Pico;
    }

    const std::string filter(raw);
    if (filter == "pico") {
        return DeviceFilterKind::Pico;
    }
    if (filter == "base" || filter == "basestation" || filter == "alpha" || filter == "tau") {
        return DeviceFilterKind::BaseStation;
    }
    if (filter == "all") {
        return DeviceFilterKind::All;
    }

    throw std::runtime_error("unknown device filter; expected pico, base, or all");
}

inline DebugScope parse_debug_scope(const char* raw) {
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

// Abstract MIDI byte-triple sink. UdpMidiOut (real UDP) implements this in
// tools/pico_udp_midi_bridge.cpp; tests use a recording implementation.
class MidiSink {
   public:
    virtual ~MidiSink() = default;
    virtual void send3(uint8_t status, uint8_t d1, uint8_t d2) = 0;
};

// Tracks per-key/button LED colour and layers a transient press overlay
// (orange while held) over a DAW-settable base colour, mirroring EigenD's own
// pico module (light_input + status_mixer combining multiple LED sources).
//
// applyToHardware is a seam for testing: the default implementation calls
// through to a real EigenApi::Eigenharp; tests override it to record calls
// instead.
class LedState {
   public:
    virtual ~LedState() = default;

    void attach(EigenApi::Eigenharp* harp) {
        harp_ = harp;
    }

    void setDevice(const std::string& dev) {
        dev_ = dev;
        for (unsigned i = 0; i < kLedIndexCount; ++i) {
            applyIndex(i);
        }
    }

    void clearDevice(const std::string& dev) {
        if (dev_ == dev) {
            dev_.clear();
        }
    }

    void setBaseColour(unsigned index, EigenApi::Eigenharp::LedColour colour) {
        if (index >= kLedIndexCount) return;
        base_[index] = colour;
        if (!pressed_[index]) {
            applyIndex(index);
        }
    }

    void onActive(unsigned index, bool active) {
        if (index >= kLedIndexCount) return;
        pressed_[index] = active;
        if (active) {
            setLed(index, EigenApi::Eigenharp::LED_ORANGE);
        } else {
            applyIndex(index);
        }
    }

    EigenApi::Eigenharp::LedColour baseColour(unsigned index) const {
        return index < kLedIndexCount ? base_[index] : EigenApi::Eigenharp::LED_OFF;
    }

   protected:
    virtual void applyToHardware(unsigned course, unsigned key, EigenApi::Eigenharp::LedColour colour) {
        if (harp_ == nullptr || dev_.empty()) return;
        harp_->setLED(dev_.c_str(), course, key, colour);
    }

   private:
    void applyIndex(unsigned index) {
        setLed(index, base_[index]);
    }

    void setLed(unsigned index, EigenApi::Eigenharp::LedColour colour) {
        if (index < 18) {
            applyToHardware(0, index, colour);
        } else {
            applyToHardware(1, index - 18, colour);
        }
    }

    EigenApi::Eigenharp* harp_ = nullptr;
    std::string dev_;
    EigenApi::Eigenharp::LedColour base_[kLedIndexCount] = {};
    bool pressed_[kLedIndexCount] = {false};
};

inline void handle_led_control(const uint8_t msg[3], LedState& led) {
    const unsigned index = msg[1];
    if (msg[0] == kLedControlOffStatus) {
        led.setBaseColour(index, EigenApi::Eigenharp::LED_OFF);
        return;
    }
    if (msg[0] != kLedControlStatus) {
        return;
    }
    EigenApi::Eigenharp::LedColour colour;
    if (msg[2] == 0) {
        colour = EigenApi::Eigenharp::LED_OFF;  // Note On vel 0 == note off
    } else if (msg[2] <= 42) {
        colour = EigenApi::Eigenharp::LED_GREEN;
    } else if (msg[2] <= 84) {
        colour = EigenApi::Eigenharp::LED_RED;
    } else {
        colour = EigenApi::Eigenharp::LED_ORANGE;
    }
    led.setBaseColour(index, colour);
}

class MidiBridgeImplementation {
   public:
    virtual ~MidiBridgeImplementation() = default;
    virtual const char* name() const = 0;
    virtual void on_key(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float y) = 0;
    virtual void on_button(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned key, bool active) = 0;
    virtual void on_breath(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, float val) = 0;
    virtual void on_strip(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned strip, float val, bool active) = 0;
    // force note-off for everything sounding (device disconnect / shutdown)
    virtual void release_all(MidiSink& out) = 0;
};

class StableMidiBridgeImplementation : public MidiBridgeImplementation {
   public:
    StableMidiBridgeImplementation() {
        for (bool& v : key_down_) v = false;
        for (uint8_t& v : last_pressure_) v = 0xFF;
        for (uint8_t& v : last_cc_) v = 0xFF;
        for (unsigned long long& v : key_last_event_t_) v = 0ULL;
    }

    const char* name() const override {
        return "stable";
    }

    void on_key(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned course, unsigned key, bool active, float p, float /*r*/, float /*y*/) override {
        if (course != 0 || key >= 128) {
            return;
        }

        if (debug && debug_scope == DebugScope::All) {
            std::cout << "key course=" << course << " key=" << key << " active=" << active << " pressure=" << p << std::endl;
        }

        reap_stuck_notes(out, t);

        const uint8_t note = static_cast<uint8_t>(key + 48);
        const uint8_t vel = static_cast<uint8_t>(to_u7(p));

        if (active) {
            key_last_event_t_[key] = t;
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

    void on_button(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned key, bool active) override {
        if (key >= 4) {
            return;
        }

        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Gates)) {
            std::cout << "button key=" << key << " active=" << active << std::endl;
        }

        reap_stuck_notes(out, t);

        const uint8_t note = static_cast<uint8_t>(kModeButtonBaseNote + key);
        if (active) {
            out.send3(0x90, note, 127);
        } else {
            out.send3(0x80, note, 0);
        }
    }

    void on_breath(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, float val) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "breath " << val << std::endl;
        }
        reap_stuck_notes(out, t);
        send_cc(out, kBreathCc, scale_breath(val), 0);
    }

    void on_strip(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned strip, float val, bool active) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "strip " << strip << " value=" << val << " active=" << active << std::endl;
        }
        reap_stuck_notes(out, t);
        send_cc(out, kRibbonCc, active ? val : 0.0f, 1);
    }

    void release_all(MidiSink& out) override {
        for (unsigned key = 0; key < 128; ++key) {
            if (!key_down_[key]) continue;
            out.send3(0x80, static_cast<uint8_t>(key + 48), 0);
            key_down_[key] = false;
            last_pressure_[key] = 0xFF;
        }
    }

   protected:
    // see kStuckNoteTimeoutUs; called from every event handler so a stuck
    // key is reaped by whatever traffic is still flowing (breath streams
    // constantly), and a re-press of a stuck key reaps itself first and
    // then retriggers as a fresh note
    virtual void reap_stuck_notes(MidiSink& out, unsigned long long t) {
        if (t > latest_event_t_) latest_event_t_ = t;
        for (unsigned key = 0; key < 128; ++key) {
            if (!key_down_[key]) continue;
            if (latest_event_t_ - key_last_event_t_[key] <= kStuckNoteTimeoutUs) continue;
            std::cout << "watchdog: releasing stuck note key=" << key
                      << " note=" << (key + 48)
                      << " silent_ms=" << (latest_event_t_ - key_last_event_t_[key]) / 1000
                      << std::endl;
            out.send3(0x80, static_cast<uint8_t>(key + 48), 0);
            key_down_[key] = false;
            last_pressure_[key] = 0xFF;
        }
    }
    void send_poly_pressure(MidiSink& out, uint8_t note, float p) {
        const unsigned key = static_cast<unsigned>(note - 48);
        if (key >= 128) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(p));
        if (last_pressure_[key] == midi) return;
        last_pressure_[key] = midi;
        out.send3(0xA0, note, midi);
    }

    void send_cc(MidiSink& out, uint8_t cc, float val, unsigned idx) {
        if (idx >= 8) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(val));
        if (last_cc_[idx] == midi) return;
        last_cc_[idx] = midi;
        out.send3(0xB0, cc, midi);
    }

    bool key_down_[128];
    uint8_t last_pressure_[128];
    uint8_t last_cc_[8];
    unsigned long long latest_event_t_ = 0ULL;
    unsigned long long key_last_event_t_[128];
};

class ParityMidiBridgeImplementation : public StableMidiBridgeImplementation {
   public:
    const char* name() const override {
        return "parity";
    }

    void on_key(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float /*y*/) override {
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

        reap_stuck_notes(out, t);

        auto& state = keys_[key];
        const uint8_t note = static_cast<uint8_t>(key + 48);

        if (active) {
            state.last_event_t = t;
        }

        if (!active) {
            if (state.note_on) {
                if (debug && debug_scope == DebugScope::Gates) {
                    std::cout << "gate note_off key=" << key << " note=" << static_cast<unsigned>(note)
                              << " max=" << state.max_pressure << " [parity]" << std::endl;
                }
                out.send3(0x80, note, 0);
                send_cc(out, kRollCc, 0.5f, 2);  // recentre roll with the note
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
                state.roll_origin = r;
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

        // Relative roll, parity-only: rocking the key wobbles CC74 around 64,
        // measured from where the finger landed at note start -- absolute
        // tilt would slam the CC on every press since fingers rarely land
        // dead centre.
        float roll_delta = (r - state.roll_origin) * kParityRollGain;
        if (roll_delta < -1.0f) roll_delta = -1.0f;
        if (roll_delta > 1.0f) roll_delta = 1.0f;
        send_cc(out, kRollCc, (roll_delta + 1.0f) * 0.5f, 2);

        send_poly_pressure(out, key, note, p);
    }

    void on_breath(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, float val) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "breath " << val << " [parity]" << std::endl;
        }

        reap_stuck_notes(out, t);

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

    void on_strip(MidiSink& out, bool debug, DebugScope debug_scope, unsigned long long t, unsigned strip, float val, bool active) override {
        if (debug && (debug_scope == DebugScope::All || debug_scope == DebugScope::Controls)) {
            std::cout << "strip " << strip << " value=" << val << " active=" << active << " [parity]" << std::endl;
        }
        reap_stuck_notes(out, t);
        send_cc(out, kRibbonCc, active ? val : 0.0f, 1);

        // Relative ribbon: delta from touch origin, centred at CC 64 (no
        // displacement). Origin is captured on touch-start and held for the
        // duration of the touch, independent of the absolute-position CC above.
        if (strip < kMaxStrips) {
            if (active) {
                if (!strip_touching_[strip]) {
                    strip_origin_[strip] = val;
                }
                const float rel = val - strip_origin_[strip];
                const float rel_unipolar = std::max(0.0f, std::min(1.0f, (rel + 1.0f) * 0.5f));
                send_cc(out, kRibbonRelativeCc, rel_unipolar, 3);
            } else {
                send_cc(out, kRibbonRelativeCc, 0.5f, 3);
            }
            strip_touching_[strip] = active;
        }
    }

    void release_all(MidiSink& out) override {
        for (unsigned key = 0; key < 128; ++key) {
            if (!keys_[key].note_on) continue;
            out.send3(0x80, static_cast<uint8_t>(key + 48), 0);
            send_cc(out, kRollCc, 0.5f, 2);
            keys_[key] = {};
            last_pressure_[key] = 0xFF;
        }
    }

   protected:
    void reap_stuck_notes(MidiSink& out, unsigned long long t) override {
        if (t > latest_event_t_) latest_event_t_ = t;
        for (unsigned key = 0; key < 128; ++key) {
            auto& state = keys_[key];
            if (!state.note_on) continue;
            if (latest_event_t_ - state.last_event_t <= kStuckNoteTimeoutUs) continue;
            std::cout << "watchdog: releasing stuck note key=" << key
                      << " note=" << (key + 48)
                      << " silent_ms=" << (latest_event_t_ - state.last_event_t) / 1000
                      << " [parity]" << std::endl;
            out.send3(0x80, static_cast<uint8_t>(key + 48), 0);
            send_cc(out, kRollCc, 0.5f, 2);  // recentre roll with the note
            state = {};
            last_pressure_[key] = 0xFF;
        }
    }

   private:
    static constexpr unsigned kMaxStrips = 4;

    struct KeyState {
        bool tracking = false;
        bool note_on = false;
        unsigned frames = 0;
        float first_pressure = 0.0f;
        float max_pressure = 0.0f;
        float roll_origin = 0.0f;
        unsigned long long last_release_ts = 0ULL;
        unsigned long long last_event_t = 0ULL;
    };

    void send_poly_pressure(MidiSink& out, unsigned key, uint8_t note, float p) {
        if (key >= 128) return;
        const uint8_t midi = static_cast<uint8_t>(to_u7(p));
        if (last_pressure_[key] == midi) return;
        last_pressure_[key] = midi;
        out.send3(0xA0, note, midi);
    }

    KeyState keys_[128];
    float strip_origin_[kMaxStrips] = {0.0f};
    bool strip_touching_[kMaxStrips] = {false};
    int breath_hold_ticks_ = 0;
};

inline std::unique_ptr<MidiBridgeImplementation> make_bridge_implementation(BridgeModeKind mode) {
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
    MidiBridgeCallback(MidiSink& out, bool debug, DebugScope debug_scope, std::unique_ptr<MidiBridgeImplementation> impl,
                       EigenApi::Eigenharp* harp)
        : out_(out), debug_(debug), debug_scope_(debug_scope), impl_(std::move(impl)) {
        led_.attach(harp);
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
        if (dt == EigenApi::PICO) {
            led_.setDevice(dev);
        }
    }

    void disconnected(const char* dev) override {
        std::cout << "disconnected " << dev << std::endl;
        impl_->release_all(out_);  // no more key events coming; don't strand notes
        led_.clearDevice(dev);
    }

    void key(const char* /*dev*/, unsigned long long t, unsigned course, unsigned key, bool active, float p, float r, float y) override {
        impl_->on_key(out_, debug_, debug_scope_, t, course, key, active, p, r, y);
        if (course == 0 && key < 18) {
            led_.onActive(key, active);
        }
    }

    void button(const char* /*dev*/, unsigned long long t, unsigned key, bool active) override {
        impl_->on_button(out_, debug_, debug_scope_, t, key, active);
        if (key < 4) {
            led_.onActive(18 + key, active);
        }
    }

    void led_control(const uint8_t msg[3]) {
        handle_led_control(msg, led_);
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
    MidiSink& out_;
    bool debug_;
    DebugScope debug_scope_;
    std::unique_ptr<MidiBridgeImplementation> impl_;
    LedState led_;
};

}  // namespace PicoBridge
