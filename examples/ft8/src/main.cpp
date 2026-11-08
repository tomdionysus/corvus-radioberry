// SPDX-License-Identifier: GPL-2.0-or-later
//
// ft8rx: an example Protocol 1 client. It receives one channel from
// radioberryd (or any Hermes-Lite 2 style radio), decodes FT8 in each
// UTC 15 s slot and prints what it hears, one line per message as WSJT-X
// does. Receive only: it never sets MOX.
//
//   ft8rx [--host 127.0.0.1] [--port 1024] [--dial 14074000] [--gain 20]
//         [--slots N] [--iq-order qi|iq]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decimator.hpp"
#include "ft8/decoder.h"
#include "message_text.hpp"
#include "p1_receiver.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1024;
    std::uint32_t dial_hz = 14074000;
    int gain_db = 20;
    int slots = 0;  // 0: until interrupted
    // Each sample's second value taken as I: measured on the development
    // Radioberry through radioberryd, FT8 decodes this way and not the
    // other (docs/examples.md).
    bool q_first = true;
};

const char* const kUsage =
    "usage: ft8rx [options]\n"
    "\n"
    "Receives one channel over openHPSDR Protocol 1 and prints the FT8 it decodes in\n"
    "each 15 s slot. Receive only. The clock must be on UTC within a second.\n"
    "\n"
    "  --host IPV4    the radio (default 127.0.0.1, radioberryd on this machine)\n"
    "  --port N       its Protocol 1 port (default 1024)\n"
    "  --dial HZ      the FT8 dial frequency (default 14074000)\n"
    "  --gain DB      LNA gain, -12 to 48 (default 20)\n"
    "  --slots N      stop after N decoded slots (default: until interrupted)\n"
    "  --iq-order O   qi (default): each sample's second value is I; iq: the first\n";

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(a + " needs a value");
            }
            return argv[++i];
        };
        if (a == "--host") {
            o.host = value();
        } else if (a == "--port") {
            o.port = static_cast<std::uint16_t>(std::stoul(value()));
        } else if (a == "--dial") {
            o.dial_hz = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (a == "--gain") {
            o.gain_db = std::stoi(value());
        } else if (a == "--iq-order") {
            const std::string order = value();
            if (order != "qi" && order != "iq") {
                throw std::invalid_argument("--iq-order is qi or iq");
            }
            o.q_first = order == "qi";
        } else if (a == "--slots") {
            o.slots = std::stoi(value());
        } else {
            throw std::invalid_argument(a == "-h" || a == "--help" ? "" : "unknown option " + a);
        }
    }
    return o;
}

struct Slot {
    std::time_t start = 0;
    std::vector<std::complex<float>> samples;
};

void print(const Slot& slot, const std::vector<ft8::Decoder::Frame>& frames) {
    std::tm utc{};
    gmtime_r(&slot.start, &utc);
    char when[16];
    std::strftime(when, sizeof when, "%H%M%S", &utc);
    for (const auto& frame : frames) {
        const auto text = ft8::message_text(frame.payload);
        std::printf("%s %4.0f %5.1f %5.0f ~  %s\n", when, frame.snr_db,
                    frame.time_s - ft8::kStartOffset, frame.freq_hz + 1500.0f,
                    text ? text->c_str() : "(not a standard message)");
    }
    std::printf("-- %s: %zu decoded from %zu samples\n", when, frames.size(), slot.samples.size());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception& e) {
        if (*e.what() != '\0') {
            std::cerr << "ft8rx: " << e.what() << "\n\n";
        }
        std::cerr << kUsage;
        return 2;
    }

    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    try {
        ft8::P1Receiver receiver(options.host, options.port);
        std::optional<ft8::P1Receiver::Radio> radio;
        for (int attempt = 0; attempt < 3 && !radio; ++attempt) {
            radio = receiver.discover(std::chrono::milliseconds(700));
        }
        if (!radio) {
            std::cerr << "ft8rx: no reply to discovery from " << options.host << "\n";
            return 1;
        }
        std::printf("radio: board %u, gateware %u.%u, %u receivers\n", radio->board,
                    radio->version_major, radio->version_minor, radio->receivers);
        if (radio->busy) {
            std::cerr << "ft8rx: the radio is streaming to another client\n";
            return 1;
        }

        // Tuned 1500 Hz above the dial, so FT8's 0 to 3000 Hz is centred.
        receiver.set_swap_iq(options.q_first);
        receiver.start(options.dial_hz + 1500, options.gain_db);
        std::printf("receiving %u Hz dial; the first whole slot starts at the next boundary\n",
                    options.dial_hz);
        std::fflush(stdout);

        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Slot> queue;
        bool done = false;
        int decoded_slots = 0;
        std::size_t total = 0;

        std::thread decoder_thread([&] {
            ft8::Decoder::Options d;
            d.sample_rate = ft8::Decimator::kOutputRate;
            for (;;) {
                Slot slot;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return done || !queue.empty(); });
                    if (queue.empty()) {
                        return;
                    }
                    slot = std::move(queue.front());
                    queue.pop_front();
                }
                ft8::Decoder decoder(d);
                decoder.process(slot.samples);
                const auto frames = decoder.decode();
                print(slot, frames);
                const std::lock_guard lock(mutex);
                total += frames.size();
                ++decoded_slots;
            }
        });

        ft8::Decimator decimator;
        std::vector<std::complex<float>> wide;
        Slot current;
        int silent = 0;
        for (;;) {
            {
                const std::lock_guard lock(mutex);
                if (g_stop || (options.slots > 0 &&
                               decoded_slots + static_cast<int>(queue.size()) >= options.slots)) {
                    break;
                }
            }
            wide.clear();
            if (!receiver.receive(wide, std::chrono::seconds(1))) {
                if (++silent == 5) {
                    std::cerr << "ft8rx: no data for 5 s\n";
                    break;
                }
                continue;
            }
            silent = 0;
            const std::time_t now = std::time(nullptr);
            const std::time_t slot = now - now % 15;
            if (slot != current.start) {
                // A slot counts only if it was heard from (nearly) its start.
                if (current.start != 0 &&
                    current.samples.size() >
                        static_cast<std::size_t>(14 * ft8::Decimator::kOutputRate)) {
                    const std::lock_guard lock(mutex);
                    queue.push_back(std::move(current));
                    ready.notify_one();
                }
                current = Slot{slot, {}};
            }
            decimator.process(wide, current.samples);
        }

        receiver.stop();
        {
            const std::lock_guard lock(mutex);
            done = true;
        }
        ready.notify_one();
        decoder_thread.join();
        std::printf("%d slots, %zu decoded; %llu packets, %llu lost\n", decoded_slots, total,
                    static_cast<unsigned long long>(receiver.packets()),
                    static_cast<unsigned long long>(receiver.lost()));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ft8rx: " << e.what() << "\n";
        return 1;
    }
}
