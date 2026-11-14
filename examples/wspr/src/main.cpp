// SPDX-License-Identifier: GPL-2.0-or-later
//
// wsprrx: an example openHPSDR client, Protocol 1 or 2, the sister of
// ft8rx. It receives one channel from radioberryd (or any Hermes-Lite 2
// style radio), decodes WSPR in each even UTC two-minute slot and prints
// what it hears, one line per message as wsprd does. Receive only: it
// never sets MOX. With --file it decodes wsprd's .c2 files instead.
//
//   wsprrx [--protocol 1|2] [--host 127.0.0.1] [--port 1024] [--dial 14095600]
//          [--gain 20] [--slots N] [--iq-order qi|iq] [--save DIR] [--file F.c2 ...]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "p1_receiver.hpp"
#include "p2_receiver.hpp"
#include "wspr/c2file.hpp"
#include "wspr/decimator.hpp"
#include "wspr/decoder.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

constexpr std::time_t kSlotSeconds = 120;

struct Options {
    int protocol = 1;
    std::string host = "127.0.0.1";
    std::uint16_t port = 1024;
    std::uint32_t dial_hz = 14095600;
    int gain_db = 20;
    bool gain_given = false;
    int slots = 0;  // 0: until interrupted
    // As ft8rx: each sample's second value taken as I for the default
    // daemon (docs/examples.md, Sample order).
    bool q_first = true;
    std::vector<std::string> files;
    std::string save_dir;  // empty: slots are not saved
};

const char* const kUsage =
    "usage: wsprrx [options]\n"
    "\n"
    "Receives one channel over openHPSDR Protocol 1 or 2 and prints the WSPR it\n"
    "decodes in each even two-minute slot. Receive only. The clock must be on UTC\n"
    "within a second.\n"
    "\n"
    "  --protocol P   1 (default) or 2\n"
    "  --host IPV4    the radio (default 127.0.0.1, radioberryd on this machine)\n"
    "  --port N       its Protocol 1 port, or Protocol 2 base port (default 1024)\n"
    "  --dial HZ      the WSPR dial frequency (default 14095600)\n"
    "  --gain DB      LNA gain, -12 to 48 (default 20); Protocol 1 only: under\n"
    "                 Protocol 2 radioberryd's --p2-rx-gain sets it\n"
    "  --slots N      stop after N decoded slots (default: until interrupted)\n"
    "  --iq-order O   qi (default): each sample's second value is I; iq: the first\n"
    "  --file F.c2    decode a wsprd .c2 file instead of a radio; repeatable\n"
    "  --save DIR     also write each slot received to DIR as a .c2 file\n";

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
        if (a == "--protocol") {
            const std::string p = value();
            if (p != "1" && p != "2") {
                throw std::invalid_argument("--protocol is 1 or 2");
            }
            o.protocol = p == "1" ? 1 : 2;
        } else if (a == "--host") {
            o.host = value();
        } else if (a == "--port") {
            o.port = static_cast<std::uint16_t>(std::stoul(value()));
        } else if (a == "--dial") {
            o.dial_hz = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (a == "--gain") {
            o.gain_db = std::stoi(value());
            o.gain_given = true;
        } else if (a == "--iq-order") {
            const std::string order = value();
            if (order != "qi" && order != "iq") {
                throw std::invalid_argument("--iq-order is qi or iq");
            }
            o.q_first = order == "qi";
        } else if (a == "--slots") {
            o.slots = std::stoi(value());
        } else if (a == "--file") {
            o.files.push_back(value());
        } else if (a == "--save") {
            o.save_dir = value();
        } else {
            throw std::invalid_argument(a == "-h" || a == "--help" ? "" : "unknown option " + a);
        }
    }
    if (o.protocol == 2 && o.gain_given) {
        throw std::invalid_argument("--gain is Protocol 1 only");
    }
    return o;
}

struct Slot {
    std::time_t start = 0;
    std::vector<std::complex<float>> samples;
};

void print(const char* when, double dial_hz, std::size_t samples,
           const std::vector<wspr::Decoder::Decode>& decodes) {
    for (const auto& d : decodes) {
        std::printf("%s %3.0f %4.1f %10.6f %2.0f  %s\n", when, d.snr_db, d.dt_s,
                    (dial_hz + 1500.0 + d.freq_hz) / 1e6, d.drift_hz, d.text.c_str());
    }
    std::printf("-- %s: %zu decoded from %zu samples\n", when, decodes.size(), samples);
    std::fflush(stdout);
}

int decode_files(const Options& options) {
    wspr::Decoder decoder;
    for (const std::string& path : options.files) {
        const wspr::C2File f = wspr::read_c2(path);
        print(f.name.c_str(), f.dial_mhz * 1e6, f.samples.size(), decoder.decode(f.samples));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception& e) {
        if (*e.what() != '\0') {
            std::cerr << "wsprrx: " << e.what() << "\n\n";
        }
        std::cerr << kUsage;
        return 2;
    }
    if (!options.files.empty()) {
        try {
            return decode_files(options);
        } catch (const std::exception& e) {
            std::cerr << "wsprrx: " << e.what() << "\n";
            return 1;
        }
    }

    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    try {
        std::unique_ptr<ft8::Receiver> receiver_ptr;
        if (options.protocol == 1) {
            receiver_ptr = std::make_unique<ft8::P1Receiver>(options.host, options.port);
        } else {
            receiver_ptr = std::make_unique<ft8::P2Receiver>(options.host, options.port);
        }
        ft8::Receiver& receiver = *receiver_ptr;
        std::optional<ft8::Receiver::Radio> radio;
        for (int attempt = 0; attempt < 3 && !radio; ++attempt) {
            radio = receiver.discover(std::chrono::milliseconds(700));
        }
        if (!radio) {
            std::cerr << "wsprrx: no reply to discovery from " << options.host << "\n";
            return 1;
        }
        // Protocol 2 discovery carries only the major version.
        const std::string gateware =
            options.protocol == 1
                ? std::to_string(radio->version_major) + "." + std::to_string(radio->version_minor)
                : std::to_string(radio->version_major);
        std::printf("radio: Protocol %d, board %u, gateware %s, %u receivers\n", options.protocol,
                    radio->board, gateware.c_str(), radio->receivers);
        if (radio->busy) {
            std::cerr << "wsprrx: the radio is streaming to another client\n";
            return 1;
        }

        // Tuned 1500 Hz above the dial, so WSPR's 1400 to 1600 Hz is centred.
        receiver.set_swap_iq(options.q_first);
        receiver.start(options.dial_hz + 1500, options.gain_db);
        std::printf("receiving %u Hz dial; the first whole slot starts at the next even minute\n",
                    options.dial_hz);
        std::fflush(stdout);

        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Slot> queue;
        bool done = false;
        int decoded_slots = 0;
        std::size_t total = 0;

        std::thread decoder_thread([&] {
            wspr::Decoder decoder;
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
                std::tm utc{};
                gmtime_r(&slot.start, &utc);
                char when[8];
                std::strftime(when, sizeof when, "%H%M", &utc);
                if (!options.save_dir.empty()) {
                    char name[16];
                    std::strftime(name, sizeof name, "%y%m%d_%H%M.c2", &utc);
                    try {
                        wspr::write_c2(options.save_dir + "/" + name,
                                       {name, options.dial_hz / 1e6, slot.samples});
                    } catch (const std::exception& e) {
                        std::cerr << "wsprrx: " << e.what() << "\n";
                    }
                }
                const auto decodes = decoder.decode(slot.samples);
                print(when, options.dial_hz, slot.samples.size(), decodes);
                const std::lock_guard lock(mutex);
                total += decodes.size();
                ++decoded_slots;
            }
        });

        wspr::Decimator decimator;
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
                    std::cerr << "wsprrx: no data for 5 s\n";
                    break;
                }
                continue;
            }
            silent = 0;
            const std::time_t now = std::time(nullptr);
            const std::time_t slot = now - now % kSlotSeconds;
            if (slot != current.start) {
                // A slot counts only if it was heard from (nearly) its start.
                if (current.start != 0 &&
                    current.samples.size() >
                        static_cast<std::size_t>(116 * wspr::Decimator::kOutputRate)) {
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
        std::cerr << "wsprrx: " << e.what() << "\n";
        return 1;
    }
}
