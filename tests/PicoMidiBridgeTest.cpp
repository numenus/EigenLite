// Unit tests for tools/pico_midi_bridge_core.h -- the Pico-to-Bitwig MIDI
// mapping and LED state logic, driven directly with synthetic events instead
// of live hardware/UDP. See docs/reference/pico-bitwig-midi.md for the
// mapping this is meant to lock in.

#include <gtest/gtest.h>

#include "pico_midi_bridge_core.h"

using namespace PicoBridge;

namespace {

struct Msg {
    uint8_t status, d1, d2;
    bool operator==(const Msg& o) const {
        return status == o.status && d1 == o.d1 && d2 == o.d2;
    }
};

class RecordingSink : public MidiSink {
   public:
    std::vector<Msg> sent;
    void send3(uint8_t status, uint8_t d1, uint8_t d2) override {
        sent.push_back({status, d1, d2});
    }
};

struct LedCall {
    unsigned course, key;
    EigenApi::Eigenharp::LedColour colour;
};

class RecordingLedState : public LedState {
   public:
    std::vector<LedCall> calls;

   protected:
    void applyToHardware(unsigned course, unsigned key, EigenApi::Eigenharp::LedColour colour) override {
        calls.push_back({course, key, colour});
    }
};

}  // namespace

// --- Stable: keys ------------------------------------------------------

TEST(StableBridge, KeyPressSendsNoteOnThenPolyAftertouch) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    ASSERT_EQ(sink.sent.size(), 2u);
    EXPECT_EQ(sink.sent[0], (Msg{0x90, 48, 64}));
    EXPECT_EQ(sink.sent[1], (Msg{0xA0, 48, 64}));
}

TEST(StableBridge, HeldKeyDoesNotRetriggerOrSpam) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    sink.sent.clear();
    // Same pressure repeated, as EigenLite streams while a key is held.
    impl.on_key(sink, false, DebugScope::All, 1, 0, 0, true, 0.5f, 0.f, 0.f);
    impl.on_key(sink, false, DebugScope::All, 2, 0, 0, true, 0.5f, 0.f, 0.f);
    EXPECT_TRUE(sink.sent.empty()) << "no retrigger or controller spam while held";
}

TEST(StableBridge, KeyReleaseSendsNoteOff) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    sink.sent.clear();
    impl.on_key(sink, false, DebugScope::All, 1, 0, 0, false, 0.f, 0.f, 0.f);
    ASSERT_EQ(sink.sent.size(), 1u);
    EXPECT_EQ(sink.sent[0], (Msg{0x80, 48, 0}));
}

TEST(StableBridge, ZeroVelocityNoteOnIsBumpedToOne) {
    // MIDI note-on with velocity 0 is conventionally a note-off; avoid it.
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.0f, 0.f, 0.f);
    ASSERT_FALSE(sink.sent.empty());
    EXPECT_EQ(sink.sent[0], (Msg{0x90, 48, 1}));
}

TEST(StableBridge, IgnoresPercussionCourseAndOutOfRangeKey) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 1, 0, true, 0.5f, 0.f, 0.f);  // course != 0
    EXPECT_TRUE(sink.sent.empty());
}

// --- Stable: mode buttons -----------------------------------------------

TEST(StableBridge, ModeButtonsMapToNotes44Through47) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    for (unsigned button = 0; button < 4; ++button) {
        impl.on_button(sink, false, DebugScope::All, 0, button, true);
    }
    ASSERT_EQ(sink.sent.size(), 4u);
    EXPECT_EQ(sink.sent[0], (Msg{0x90, 44, 127}));
    EXPECT_EQ(sink.sent[1], (Msg{0x90, 45, 127}));
    EXPECT_EQ(sink.sent[2], (Msg{0x90, 46, 127}));
    EXPECT_EQ(sink.sent[3], (Msg{0x90, 47, 127}));
}

TEST(StableBridge, ModeButtonReleaseSendsNoteOff) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_button(sink, false, DebugScope::All, 0, 2, false);
    ASSERT_EQ(sink.sent.size(), 1u);
    EXPECT_EQ(sink.sent[0], (Msg{0x80, 46, 0}));
}

TEST(StableBridge, IgnoresOutOfRangeButtonIndex) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_button(sink, false, DebugScope::All, 0, 4, true);
    EXPECT_TRUE(sink.sent.empty());
}

// --- Stable: breath and ribbon -------------------------------------------

TEST(StableBridge, BreathAppliesGainAndClampsToUnipolar) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_breath(sink, false, DebugScope::All, 0, 0.1f);  // 0.1 * 6 = 0.6
    ASSERT_EQ(sink.sent.size(), 1u);
    EXPECT_EQ(sink.sent[0], (Msg{0xB0, 2, 76}));  // round(0.6*127)
}

TEST(StableBridge, BreathDedupesUnchangedMidiValue) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_breath(sink, false, DebugScope::All, 0, 0.1f);
    sink.sent.clear();
    impl.on_breath(sink, false, DebugScope::All, 1, 0.0995f);  // rounds to same 7-bit value (76)
    EXPECT_TRUE(sink.sent.empty());
}

TEST(StableBridge, RibbonSendsZeroWhenTouchInactive) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_strip(sink, false, DebugScope::All, 0, 0, 0.3f, true);
    sink.sent.clear();
    impl.on_strip(sink, false, DebugScope::All, 1, 0, 0.3f, false);
    ASSERT_EQ(sink.sent.size(), 1u);
    EXPECT_EQ(sink.sent[0], (Msg{0xB0, 21, 0}));
}

// --- Parity: key gating ---------------------------------------------------

TEST(ParityBridge, StrongPressGatesNoteOnAfterEstimationWindow) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    // kParityKeyHardThreshold ~= 0.282; 0.5 clears it comfortably.
    for (unsigned long long t = 0; t < kParityEstimationFrames; ++t) {
        impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, 0.5f, 0.f, 0.f);
    }
    bool sawNoteOn = false;
    for (const auto& m : sink.sent) {
        if (m.status == 0x90 && m.d1 == 48) {
            sawNoteOn = true;
            EXPECT_EQ(m.d2, 127);  // scale_parity_key_velocity(0.5) saturates to 1.0
        }
    }
    EXPECT_TRUE(sawNoteOn) << "expected a gated note-on by the final estimation frame";
}

TEST(ParityBridge, NoNoteOnBeforeEstimationWindowCompletes) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    for (unsigned long long t = 0; t + 1 < kParityEstimationFrames; ++t) {
        impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, 0.5f, 0.f, 0.f);
    }
    for (const auto& m : sink.sent) {
        EXPECT_FALSE(m.status == 0x90 && m.d1 == 48) << "no premature note-on during estimation";
    }
}

TEST(ParityBridge, WeakPressIsRejectedNotGated) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    // Below kParityKeyGateThreshold (~0.094): should never gate a note.
    for (unsigned long long t = 0; t < kParityEstimationFrames + 5; ++t) {
        impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, 0.02f, 0.f, 0.f);
    }
    for (const auto& m : sink.sent) {
        EXPECT_FALSE(m.status == 0x90 && m.d1 == 48) << "weak press must not gate a note-on";
    }
}

TEST(ParityBridge, ReleaseWithoutGatedNoteSendsNoNoteOff) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.02f, 0.f, 0.f);
    sink.sent.clear();
    impl.on_key(sink, false, DebugScope::All, 1, 0, 0, false, 0.f, 0.f, 0.f);
    for (const auto& m : sink.sent) {
        EXPECT_FALSE(m.status == 0x80 && m.d1 == 48) << "never gated on, so nothing to release";
    }
}

// --- Parity: roll CC (diagnostic, not gated on note-on) -------------------

TEST(ParityBridge, RollSendsCc74WhenKeyActiveRegardlessOfGate) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    // r=0.5 -> unipolar (0.5+1)*0.5 = 0.75 -> round(0.75*127) = 95.
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.02f, 0.5f, 0.f);
    bool sawRoll = false;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == 74) {
            sawRoll = true;
            EXPECT_EQ(m.d2, 95);
        }
    }
    EXPECT_TRUE(sawRoll);
}

TEST(StableBridge, DoesNotSendRollCc) {
    // Roll (CC74) is parity-only; stable mode must never emit it.
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.9f, 0.f);
    for (const auto& m : sink.sent) {
        EXPECT_FALSE(m.status == 0xB0 && m.d1 == 74);
    }
}

// --- Parity: relative ribbon ----------------------------------------------

TEST(ParityBridge, RelativeRibbonCentresAtTouchStartAndTracksDelta) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;

    impl.on_strip(sink, false, DebugScope::All, 0, 0, 0.5f, true);  // touch start: origin = 0.5
    std::vector<uint8_t> cc22;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == kRibbonRelativeCc) cc22.push_back(m.d2);
    }
    ASSERT_EQ(cc22.size(), 1u);
    EXPECT_EQ(cc22[0], 64);  // centred, no displacement yet

    sink.sent.clear();
    impl.on_strip(sink, false, DebugScope::All, 1, 0, 0.8f, true);  // moved +0.3 from origin
    cc22.clear();
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == kRibbonRelativeCc) cc22.push_back(m.d2);
    }
    ASSERT_EQ(cc22.size(), 1u);
    EXPECT_EQ(cc22[0], 83);  // round(((0.8-0.5)+1)*0.5*127)

    sink.sent.clear();
    impl.on_strip(sink, false, DebugScope::All, 2, 0, 0.8f, false);  // release
    cc22.clear();
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == kRibbonRelativeCc) cc22.push_back(m.d2);
    }
    ASSERT_EQ(cc22.size(), 1u);
    EXPECT_EQ(cc22[0], 64);  // resets to centre on release
}

TEST(ParityBridge, AbsoluteRibbonUnaffectedByRelativeTracking) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_strip(sink, false, DebugScope::All, 0, 0, 0.5f, true);
    impl.on_strip(sink, false, DebugScope::All, 1, 0, 0.8f, true);
    std::vector<uint8_t> cc21;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == kRibbonCc) cc21.push_back(m.d2);
    }
    ASSERT_EQ(cc21.size(), 2u);
    EXPECT_EQ(cc21[0], 64);   // round(0.5*127)
    EXPECT_EQ(cc21[1], 102);  // round(0.8*127)
}

// --- LED state: press overlay + base colour layering ----------------------

TEST(LedStateTest, SetDeviceAppliesStoredBaseColoursToAllIndices) {
    RecordingLedState led;
    led.setDevice("pico-1");
    ASSERT_EQ(led.calls.size(), kLedIndexCount);
    for (const auto& c : led.calls) {
        EXPECT_EQ(c.colour, EigenApi::Eigenharp::LED_OFF);
    }
    EXPECT_EQ(led.calls[0].course, 0u);
    EXPECT_EQ(led.calls[0].key, 0u);
    EXPECT_EQ(led.calls[17].course, 0u);
    EXPECT_EQ(led.calls[17].key, 17u);
    EXPECT_EQ(led.calls[18].course, 1u);
    EXPECT_EQ(led.calls[18].key, 0u);
    EXPECT_EQ(led.calls[21].course, 1u);
    EXPECT_EQ(led.calls[21].key, 3u);
}

TEST(LedStateTest, PressLightsOrangeReleaseRestoresBase) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    led.onActive(0, true);
    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_ORANGE);

    led.calls.clear();
    led.onActive(0, false);
    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_OFF);  // default base
}

TEST(LedStateTest, BaseColourChangeWhileHeldIsDeferredUntilRelease) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.onActive(0, true);
    led.calls.clear();

    led.setBaseColour(0, EigenApi::Eigenharp::LED_RED);
    EXPECT_TRUE(led.calls.empty()) << "base colour change while held must not touch hardware yet";

    led.onActive(0, false);
    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_RED);
}

TEST(LedStateTest, BaseColourChangeWhileNotHeldAppliesImmediately) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    led.setBaseColour(5, EigenApi::Eigenharp::LED_GREEN);
    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].course, 0u);
    EXPECT_EQ(led.calls[0].key, 5u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_GREEN);
}

TEST(LedStateTest, OutOfRangeIndexIsIgnoredSafely) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();
    led.setBaseColour(kLedIndexCount + 5, EigenApi::Eigenharp::LED_RED);
    led.onActive(kLedIndexCount + 5, true);
    EXPECT_TRUE(led.calls.empty());
}

// --- LED control protocol parsing (Bitwig -> Pico) -------------------------

TEST(LedControlTest, NoteOnChannel16SetsBaseColour) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t msg[3] = {kLedControlStatus, 5, 3};  // index 5, velocity 3 = orange
    handle_led_control(msg, led);

    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].course, 0u);
    EXPECT_EQ(led.calls[0].key, 5u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_ORANGE);
}

TEST(LedControlTest, WrongStatusByteIsIgnored) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t msg[3] = {0x90, 5, 3};  // note-on channel 1, not the LED channel
    handle_led_control(msg, led);

    EXPECT_TRUE(led.calls.empty());
}

TEST(LedControlTest, UnrecognisedVelocityDefaultsToOff) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t msg[3] = {kLedControlStatus, 0, 99};
    handle_led_control(msg, led);

    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_OFF);
}
