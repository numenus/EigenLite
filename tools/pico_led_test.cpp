#include <eigenapi.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> keep_running(true);

void int_handler(int) {
    keep_running = false;
}

class PicoLifecycle : public EigenApi::LifecycleCallback {
   public:
    void connected(const char* dev, EigenApi::DeviceType dt) override {
        if (dt != EigenApi::PICO) {
            return;
        }
        if (!dev_name_.empty()) {
            return;
        }
        dev_name_ = dev;
        std::cout << "connected pico " << dev_name_ << std::endl;
    }

    void disconnected(const char* dev) override {
        if (dev_name_ == dev) {
            std::cout << "disconnected pico " << dev_name_ << std::endl;
            dev_name_.clear();
        }
    }

    void dead(const char* dev, unsigned reason) override {
        if (dev_name_ == dev) {
            std::cout << "dead pico " << dev_name_ << " reason=" << reason << std::endl;
            dev_name_.clear();
        }
    }

    const std::string& dev_name() const {
        return dev_name_;
    }

   private:
    std::string dev_name_;
};

void clear_pico_leds(EigenApi::Eigenharp& harp, const std::string& dev_name) {
    if (dev_name.empty()) {
        return;
    }
    for (unsigned key = 0; key < 18; ++key) {
        harp.setLED(dev_name.c_str(), 0, key, EigenApi::Eigenharp::LED_OFF);
    }
    for (unsigned button = 0; button < 4; ++button) {
        harp.setLED(dev_name.c_str(), 1, button, EigenApi::Eigenharp::LED_OFF);
    }
}

const char* colour_name(EigenApi::Eigenharp::LedColour colour) {
    switch (colour) {
        case EigenApi::Eigenharp::LED_OFF:
            return "off";
        case EigenApi::Eigenharp::LED_GREEN:
            return "green";
        case EigenApi::Eigenharp::LED_RED:
            return "red";
        case EigenApi::Eigenharp::LED_ORANGE:
            return "orange";
    }
    return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, int_handler);

    unsigned period_ms = 1000;
    bool walk_mode = false;
    if (argc >= 2) {
        if (std::strcmp(argv[1], "walk") == 0) {
            walk_mode = true;
        } else {
            period_ms = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
            if (period_ms == 0) {
                period_ms = 1000;
            }
        }
    }
    if (argc >= 3) {
        period_ms = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
        if (period_ms == 0) {
            period_ms = walk_mode ? 250 : 1000;
        }
    }

    try {
        EigenApi::FWR_Embedded fwr;
        EigenApi::Eigenharp harp(&fwr);
        PicoLifecycle lifecycle;
        harp.addLifecycleCallback(&lifecycle);
        harp.setPollTime(50);

        if (!harp.start()) {
            std::cerr << "failed to start Eigenharp" << std::endl;
            return 1;
        }

        unsigned phase = 0;
        bool had_device = false;
        const EigenApi::Eigenharp::LedColour colours[] = {
            EigenApi::Eigenharp::LED_GREEN,
            EigenApi::Eigenharp::LED_RED,
            EigenApi::Eigenharp::LED_ORANGE,
            EigenApi::Eigenharp::LED_OFF,
        };

        std::cout << "pico-led-test mode=" << (walk_mode ? "walk" : "focus")
                  << " period_ms=" << period_ms << std::endl;

        while (keep_running.load()) {
            harp.process();

            const std::string& dev_name = lifecycle.dev_name();
            if (dev_name.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            if (!had_device) {
                clear_pico_leds(harp, dev_name);
                had_device = true;
            }

            clear_pico_leds(harp, dev_name);

            if (walk_mode) {
                const unsigned main_key = phase % 18;
                const unsigned button = phase % 4;
                const auto colour = colours[phase % 3];

                harp.setLED(dev_name.c_str(), 0, main_key, colour);
                harp.setLED(dev_name.c_str(), 1, button, EigenApi::Eigenharp::LED_ORANGE);

                std::cout << "walk phase=" << phase
                          << " main_key=" << main_key
                          << " button=" << button
                          << " colour=" << colour_name(colour)
                          << std::endl;
            } else {
                const auto colour = colours[phase % 4];
                const unsigned main_key = 0;
                const unsigned button = 0;

                harp.setLED(dev_name.c_str(), 0, main_key, colour);
                harp.setLED(dev_name.c_str(), 1, button, colour);

                std::cout << "focus phase=" << phase
                          << " main_key=" << main_key
                          << " button=" << button
                          << " colour=" << colour_name(colour)
                          << std::endl;
            }

            ++phase;

            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }

        clear_pico_leds(harp, lifecycle.dev_name());
        harp.stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "pico-led-test error: " << e.what() << std::endl;
        return 1;
    }
}
