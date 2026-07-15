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

// gate a parity note on key 0 with a rising attack; returns t after the last frame
unsigned long long gate_parity_note(ParityMidiBridgeImplementation& impl, RecordingSink& sink,
                                    unsigned long long t0 = 1) {
    for (unsigned long long i = 0; i < kParityEstimationFrames; ++i) {
        const float p = 0.5f * static_cast<float>(i + 1) / kParityEstimationFrames;
        impl.on_key(sink, false, DebugScope::All, t0 + i, 0, 0, true, p, 0.f, 0.f);
    }
    return t0 + kParityEstimationFrames;
}

bool saw_msg(const RecordingSink& sink, uint8_t status, uint8_t d1) {
    for (const auto& m : sink.sent) {
        if (m.status == status && m.d1 == d1) return true;
    }
    return false;
}

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

TEST(StableBridge, MappableModeButtonsSendNotes46And47) {
    // buttons 0/1 are the octave switches and must send no notes
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    for (unsigned button = 0; button < 4; ++button) {
        impl.on_button(sink, false, DebugScope::All, 0, button, true);
    }
    ASSERT_EQ(sink.sent.size(), 2u);
    EXPECT_EQ(sink.sent[0], (Msg{0x90, 46, 127}));
    EXPECT_EQ(sink.sent[1], (Msg{0x90, 47, 127}));
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

// --- Octave switching -------------------------------------------------------

namespace {
void press_button(StableMidiBridgeImplementation& impl, RecordingSink& sink, unsigned button) {
    impl.on_button(sink, false, DebugScope::All, 0, button, true);
    impl.on_button(sink, false, DebugScope::All, 0, button, false);
}
}  // namespace

TEST(StableBridge, OctaveUpShiftsNewNotesByTwelve) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    press_button(impl, sink, kOctaveUpButton);
    EXPECT_EQ(impl.octave_shift(), 1);
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    EXPECT_EQ(sink.sent[0], (Msg{0x90, 60, 64}));
}

TEST(StableBridge, HeldNoteKeepsPitchAcrossOctaveChange) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    press_button(impl, sink, kOctaveUpButton);
    sink.sent.clear();
    // aftertouch and the release both stay on the original pitch
    impl.on_key(sink, false, DebugScope::All, 1, 0, 0, true, 0.7f, 0.f, 0.f);
    impl.on_key(sink, false, DebugScope::All, 2, 0, 0, false, 0.f, 0.f, 0.f);
    ASSERT_EQ(sink.sent.size(), 2u);
    EXPECT_EQ(sink.sent[0].d1, 48);
    EXPECT_EQ(sink.sent[1], (Msg{0x80, 48, 0}));
}

TEST(StableBridge, OctaveShiftClampsToConfiguredRange) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    for (int i = 0; i < 10; ++i) press_button(impl, sink, kOctaveUpButton);
    EXPECT_EQ(impl.octave_shift(), kOctaveShiftMax);
    for (int i = 0; i < 20; ++i) press_button(impl, sink, kOctaveDownButton);
    EXPECT_EQ(impl.octave_shift(), kOctaveShiftMin);
}

TEST(ParityBridge, GatedNoteUsesOctaveShiftFromNoteStart) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    press_button(impl, sink, kOctaveUpButton);
    press_button(impl, sink, kOctaveUpButton);
    const unsigned long long t = gate_parity_note(impl, sink);
    (void)t;
    EXPECT_TRUE(saw_msg(sink, 0x90, 72)) << "note gates two octaves up";
    sink.sent.clear();
    press_button(impl, sink, kOctaveDownButton);  // shift while sounding
    impl.on_key(sink, false, DebugScope::All, 500, 0, 0, false, 0.f, 0.f, 0.f);
    EXPECT_TRUE(saw_msg(sink, 0x80, 72)) << "release uses the captured pitch";
}

TEST(OctaveLedTest, ColoursTrackShiftDirectionAndMagnitude) {
    EXPECT_EQ(octave_up_led(0), EigenApi::Eigenharp::LED_OFF);
    EXPECT_EQ(octave_down_led(0), EigenApi::Eigenharp::LED_OFF);
    EXPECT_EQ(octave_up_led(1), EigenApi::Eigenharp::LED_GREEN);
    EXPECT_EQ(octave_up_led(2), EigenApi::Eigenharp::LED_ORANGE);
    EXPECT_EQ(octave_up_led(4), EigenApi::Eigenharp::LED_ORANGE);
    EXPECT_EQ(octave_down_led(-1), EigenApi::Eigenharp::LED_RED);
    EXPECT_EQ(octave_down_led(-2), EigenApi::Eigenharp::LED_ORANGE);
    EXPECT_EQ(octave_up_led(-1), EigenApi::Eigenharp::LED_OFF);
    EXPECT_EQ(octave_down_led(1), EigenApi::Eigenharp::LED_OFF);
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
            // pressure 0.5 sits ~62% into the gate..full-scale velocity range
            EXPECT_EQ(m.d2, 98);
        }
    }
    EXPECT_TRUE(sawNoteOn) << "expected a gated note-on by the final estimation frame";
}

TEST(ParityBridge, VelocityTracksAttackPressure) {
    // soft, medium, and hard attacks must land clearly apart -- the old x8
    // gain saturated everything above pressure 0.125 into vel 107-127
    const float pressures[] = {0.15f, 0.5f, 0.7f};
    uint8_t vels[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        ParityMidiBridgeImplementation impl;
        RecordingSink sink;
        // ramp up to the target across the window: a soft press only gates
        // via the rise check (constant pressure = rest-pressure, rejected)
        for (unsigned long long t = 0; t < kParityEstimationFrames; ++t) {
            const float p = pressures[i] * static_cast<float>(t + 1) / kParityEstimationFrames;
            impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, p, 0.f, 0.f);
        }
        for (const auto& m : sink.sent) {
            if (m.status == 0x90 && m.d1 == 48) vels[i] = m.d2;
        }
        ASSERT_NE(vels[i], 0) << "no note-on for pressure " << pressures[i];
    }
    EXPECT_LT(vels[0] + 20, vels[1]) << "soft vs medium too close";
    EXPECT_LT(vels[1] + 15, vels[2]) << "medium vs hard too close";
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

// --- Parity: relative roll CC (gated on note-on) ---------------------------

TEST(ParityBridge, RollIsRelativeToNoteStartAndGated) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;

    // before any note gates, touches send no roll (absolute tilt would slam
    // the CC on every press)
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.02f, 0.5f, 0.f);
    for (const auto& m : sink.sent) {
        EXPECT_FALSE(m.status == 0xB0 && m.d1 == 74) << "no roll before note-on";
    }
    sink.sent.clear();

    // gate a note with a rising attack, finger landing at roll 0.5
    for (unsigned long long t = 1; t <= kParityEstimationFrames; ++t) {
        const float p = 0.5f * static_cast<float>(t) / kParityEstimationFrames;
        impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, p, 0.5f, 0.f);
    }
    // unchanged roll while held = centred 64
    bool sawCentre = false;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == 74) {
            sawCentre = true;
            EXPECT_EQ(m.d2, 64);
        }
    }
    EXPECT_TRUE(sawCentre);
    sink.sent.clear();

    // rocking away from where the finger landed moves the CC off centre
    impl.on_key(sink, false, DebugScope::All, 100, 0, 0, true, 0.5f, 0.9f, 0.f);
    bool sawDelta = false;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == 74) {
            sawDelta = true;
            // delta 0.4 -> (0.4+1)*0.5 = 0.7 -> round(0.7*127) = 89
            EXPECT_EQ(m.d2, 89);
        }
    }
    EXPECT_TRUE(sawDelta);
    sink.sent.clear();

    // release recentres
    impl.on_key(sink, false, DebugScope::All, 200, 0, 0, false, 0.f, 0.f, 0.f);
    bool sawRecentre = false;
    for (const auto& m : sink.sent) {
        if (m.status == 0xB0 && m.d1 == 74) {
            sawRecentre = true;
            EXPECT_EQ(m.d2, 64);
        }
    }
    EXPECT_TRUE(sawRecentre);
}

// --- Stuck-note watchdog ----------------------------------------------------
// The closed decoder can drop key tracking without a key-up when skipped iso
// frames force a resync: the key just goes silent while the note is sounding.
// Other events (breath streams constantly) keep flowing, so a gated note
// whose key has been silent past kStuckNoteTimeoutUs is force-released.

TEST(ParityBridge, WatchdogReleasesNoteWhenKeyStreamGoesSilent) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    const unsigned long long t = gate_parity_note(impl, sink);
    ASSERT_TRUE(saw_msg(sink, 0x90, 48));
    sink.sent.clear();
    // release lost; only breath still streaming
    impl.on_breath(sink, false, DebugScope::All, t + kStuckNoteTimeoutUs + 1, 0.f);
    EXPECT_TRUE(saw_msg(sink, 0x80, 48)) << "watchdog must force the note off";
}

TEST(ParityBridge, WatchdogDoesNotFireWhileKeyKeepsStreaming) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    unsigned long long t = gate_parity_note(impl, sink);
    sink.sent.clear();
    // long hold: key events keep arriving, breath interleaved, big total time
    for (int i = 0; i < 10; ++i) {
        t += kStuckNoteTimeoutUs / 2;
        impl.on_key(sink, false, DebugScope::All, t, 0, 0, true, 0.5f, 0.f, 0.f);
        impl.on_breath(sink, false, DebugScope::All, t + 1, 0.f);
    }
    EXPECT_FALSE(saw_msg(sink, 0x80, 48)) << "held key streaming events must not be reaped";
}

TEST(ParityBridge, KeyPressAfterWatchdogRetriggersFreshNoteOn) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    unsigned long long t = gate_parity_note(impl, sink);
    sink.sent.clear();
    // stuck note reaped by its own resumed press, then a fresh note gates
    t += kStuckNoteTimeoutUs + 1;
    const unsigned long long after = gate_parity_note(impl, sink, t);
    (void)after;
    EXPECT_TRUE(saw_msg(sink, 0x80, 48)) << "stale note released first";
    EXPECT_TRUE(saw_msg(sink, 0x90, 48)) << "new press retriggers";
}

TEST(StableBridge, WatchdogReleasesNoteWhenKeyStreamGoesSilent) {
    StableMidiBridgeImplementation impl;
    RecordingSink sink;
    impl.on_key(sink, false, DebugScope::All, 0, 0, 0, true, 0.5f, 0.f, 0.f);
    sink.sent.clear();
    impl.on_breath(sink, false, DebugScope::All, kStuckNoteTimeoutUs + 1, 0.f);
    EXPECT_TRUE(saw_msg(sink, 0x80, 48)) << "watchdog must force the note off";
}

TEST(ParityBridge, ReleaseAllFlushesSoundingNotes) {
    ParityMidiBridgeImplementation impl;
    RecordingSink sink;
    gate_parity_note(impl, sink);
    sink.sent.clear();
    impl.release_all(sink);
    EXPECT_TRUE(saw_msg(sink, 0x80, 48));
    sink.sent.clear();
    impl.release_all(sink);
    EXPECT_TRUE(sink.sent.empty()) << "already flushed";
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

TEST(DeviceFilterTest, ParsesKnownValuesAndDefaultsToPico) {
    EXPECT_EQ(parse_device_filter(nullptr), DeviceFilterKind::Pico);
    EXPECT_EQ(parse_device_filter("pico"), DeviceFilterKind::Pico);
    EXPECT_EQ(parse_device_filter("all"), DeviceFilterKind::All);
    EXPECT_EQ(parse_device_filter("base"), DeviceFilterKind::BaseStation);
    EXPECT_EQ(parse_device_filter("alpha"), DeviceFilterKind::BaseStation);
    EXPECT_EQ(parse_device_filter("tau"), DeviceFilterKind::BaseStation);
    EXPECT_THROW(parse_device_filter("bogus"), std::runtime_error);
    // values feed EigenApi::Eigenharp::setDeviceFilter directly
    EXPECT_EQ(static_cast<unsigned>(DeviceFilterKind::All), 0u);
    EXPECT_EQ(static_cast<unsigned>(DeviceFilterKind::BaseStation), 1u);
    EXPECT_EQ(static_cast<unsigned>(DeviceFilterKind::Pico), 2u);
}

// --- LED control protocol parsing (Bitwig -> Pico) -------------------------

TEST(LedControlTest, NoteOnChannel16SetsBaseColourByVelocityThird) {
    RecordingLedState led;
    led.setDevice("pico-1");

    struct Case { uint8_t vel; EigenApi::Eigenharp::LedColour colour; };
    const Case cases[] = {
        {1, EigenApi::Eigenharp::LED_GREEN},
        {42, EigenApi::Eigenharp::LED_GREEN},
        {43, EigenApi::Eigenharp::LED_RED},
        {84, EigenApi::Eigenharp::LED_RED},
        {85, EigenApi::Eigenharp::LED_ORANGE},
        {127, EigenApi::Eigenharp::LED_ORANGE},
    };
    for (const Case& c : cases) {
        led.calls.clear();
        const uint8_t msg[3] = {kLedControlStatus, 5, c.vel};
        handle_led_control(msg, led);
        ASSERT_EQ(led.calls.size(), 1u) << "velocity " << int(c.vel);
        EXPECT_EQ(led.calls[0].course, 0u);
        EXPECT_EQ(led.calls[0].key, 5u);
        EXPECT_EQ(led.calls[0].colour, c.colour) << "velocity " << int(c.vel);
    }
}

TEST(LedControlTest, WrongStatusByteIsIgnored) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t msg[3] = {0x90, 5, 3};  // note-on channel 1, not the LED channel
    handle_led_control(msg, led);

    EXPECT_TRUE(led.calls.empty());
}

TEST(LedControlTest, NoteOffChannel16ClearsKey) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t on[3] = {kLedControlStatus, 7, 100};
    handle_led_control(on, led);
    const uint8_t off[3] = {kLedControlOffStatus, 7, 64};  // DAW note end
    handle_led_control(off, led);

    ASSERT_EQ(led.calls.size(), 2u);
    EXPECT_EQ(led.calls[1].key, 7u);
    EXPECT_EQ(led.calls[1].colour, EigenApi::Eigenharp::LED_OFF);
}

TEST(LedControlTest, NoteOnVelocityZeroClears) {
    RecordingLedState led;
    led.setDevice("pico-1");
    led.calls.clear();

    const uint8_t msg[3] = {kLedControlStatus, 0, 0};  // running-status note off
    handle_led_control(msg, led);

    ASSERT_EQ(led.calls.size(), 1u);
    EXPECT_EQ(led.calls[0].colour, EigenApi::Eigenharp::LED_OFF);
}
