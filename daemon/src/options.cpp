// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <arpa/inet.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace radioberry::daemon {
namespace {

unsigned long number(const std::string& name, const std::string& text, unsigned long max) {
    unsigned long value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value > max) {
        throw std::invalid_argument(name + ": not a number from 0 to " + std::to_string(max) +
                                    ": " + text);
    }
    return value;
}

std::array<std::uint8_t, 6> mac(const std::string& text) {
    std::array<std::uint8_t, 6> m{};
    unsigned v[6];
    char tail;
    if (std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4],
                    &v[5], &tail) != 6) {
        throw std::invalid_argument("--mac: not of the form 02:00:00:00:00:01: " + text);
    }
    for (std::size_t i = 0; i < 6; ++i) {
        m[i] = static_cast<std::uint8_t>(v[i]);
    }
    return m;
}

// Config keys that take a value, as their long options do.
const std::set<std::string> kValueKeys = {"device",        "address",           "port",
                                          "mac",           "client_timeout_ms", "frequency_ppm",
                                          "iq_convention", "p2_rx_gain",        "drive_ceiling",
                                          "tx_watchdog_s", "log_level"};

// Config keys that are true or false.
const std::set<std::string> kSwitchKeys = {"allow_tx", "allow_pa", "realtime"};

}  // namespace

std::vector<std::string> config_args(const std::string& path) {
    const auto fail = [&](const std::string& why) {
        return std::invalid_argument("config " + path + ": " + why);
    };
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw fail(e.what());
    }
    std::vector<std::string> out;
    if (root.IsNull()) {
        return out;
    }
    if (!root.IsMap()) {
        throw fail("not a map of options");
    }
    for (const auto& entry : root) {
        const std::string key = entry.first.as<std::string>();
        const YAML::Node& value = entry.second;
        if (!kValueKeys.contains(key) && !kSwitchKeys.contains(key)) {
            throw fail("unknown key: " + key);
        }
        if (!value.IsScalar()) {
            throw fail(key + ": not a single value");
        }
        std::string option = "--" + key;
        std::replace(option.begin(), option.end(), '_', '-');
        if (kValueKeys.contains(key)) {
            out.push_back(option);
            out.push_back(value.Scalar());
            continue;
        }
        bool on = false;
        if (!YAML::convert<bool>::decode(value, on)) {
            throw fail(key + ": not true or false: " + value.Scalar());
        }
        if (key == "realtime") {
            if (!on) {
                out.emplace_back("--no-realtime");
            }
        } else if (on) {
            out.push_back(option);
        }
    }
    return out;
}

Options parse_options(const std::vector<std::string>& command_line) {
    // The config file's options come first, so the command line's win.
    std::vector<std::string> args;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < command_line.size(); ++i) {
        if (command_line[i] != "--config") {
            rest.push_back(command_line[i]);
        } else if (i + 1 >= command_line.size()) {
            throw std::invalid_argument("--config: needs a value");
        } else {
            const auto from_file = config_args(command_line[++i]);
            args.insert(args.end(), from_file.begin(), from_file.end());
        }
    }
    args.insert(args.end(), rest.begin(), rest.end());

    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(a + ": needs a value");
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            o.help = true;
        } else if (a == "--version") {
            o.version = true;
        } else if (a == "--device") {
            o.device = value();
        } else if (a == "--address") {
            const std::string& text = value();
            in_addr addr{};
            if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) {
                throw std::invalid_argument("--address: not an IPv4 address: " + text);
            }
            o.daemon.address = ntohl(addr.s_addr);
        } else if (a == "--port") {
            o.daemon.port = static_cast<std::uint16_t>(number(a, value(), 65535));
        } else if (a == "--mac") {
            o.daemon.mac = mac(value());
        } else if (a == "--client-timeout-ms") {
            o.daemon.client_timeout = std::chrono::milliseconds(number(a, value(), 3'600'000));
        } else if (a == "--frequency-ppm") {
            const std::string& text = value();
            std::size_t used = 0;
            double ppm = 0;
            try {
                ppm = std::stod(text, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != text.size() || !(ppm >= -200 && ppm <= 200)) {
                throw std::invalid_argument("--frequency-ppm: a number from -200 to 200: " + text);
            }
            o.daemon.frequency_ppm = ppm;
        } else if (a == "--iq-convention") {
            const std::string& text = value();
            if (text == "hl2") {
                o.daemon.iq_convention = p1::IqConvention::Hl2;
            } else if (text == "standard") {
                o.daemon.iq_convention = p1::IqConvention::Standard;
            } else {
                throw std::invalid_argument("--iq-convention: hl2 or standard: " + text);
            }
        } else if (a == "--p2-rx-gain") {
            const std::string& text = value();
            int db = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), db);
            if (ec != std::errc{} || end != text.data() + text.size() || db < -12 || db > 48) {
                throw std::invalid_argument("--p2-rx-gain: a whole number from -12 to 48: " + text);
            }
            o.daemon.p2_rx_gain_db = db;
        } else if (a == "--no-realtime") {
            o.daemon.realtime = false;
        } else if (a == "--allow-tx") {
            o.daemon.tx.allow_tx = true;
        } else if (a == "--allow-pa") {
            o.daemon.tx.allow_pa = true;
        } else if (a == "--drive-ceiling") {
            o.daemon.tx.drive_ceiling = static_cast<std::uint8_t>(number(a, value(), 255));
        } else if (a == "--tx-watchdog-s") {
            const unsigned long s = number(a, value(), 3600);
            if (s == 0) {
                throw std::invalid_argument("--tx-watchdog-s: at least 1");
            }
            o.daemon.tx.watchdog = std::chrono::seconds(s);
        } else if (a == "--log-level") {
            o.log_level = value();
            if (o.log_level != "trace" && o.log_level != "debug" && o.log_level != "info" &&
                o.log_level != "warn" && o.log_level != "error") {
                throw std::invalid_argument("--log-level: one of trace, debug, info, warn, error");
            }
        } else {
            throw std::invalid_argument("unknown option: " + a);
        }
    }
    return o;
}

std::string usage() {
    return "usage: radioberryd [options]\n"
           "\n"
           "Serves the RadioBerry to SDR clients over openHPSDR Protocol 1 (Hermes-Lite 2\n"
           "dialect, UDP and TCP) and Protocol 2 (UDP, on the Protocol 1 port and the\n"
           "ports after it). It transmits only with --allow-tx, only for the client that\n"
           "owns the radio, and only while that client holds MOX. It claims no PA\n"
           "interlock.\n"
           "\n"
           "  --config PATH            read options from this YAML file first (keys are the\n"
           "                           long options with _ for -; the command line wins)\n"
           "  --device PATH            radioberry.ko device (default /dev/radioberry)\n"
           "  --address IPV4           address to listen on (default all)\n"
           "  --port N                 Protocol 1 port and Protocol 2 base port (default\n"
           "                           1024; Protocol 2 also takes N+1 to N+5 and N+11 to\n"
           "                           N+16)\n"
           "  --mac XX:XX:XX:XX:XX:XX  MAC reported by discovery (default: the interface's)\n"
           "  --client-timeout-ms N    silence that ends a client's stream (default 3000)\n"
           "  --frequency-ppm P        the radio's frequency error, positive if it tunes high;\n"
           "                           every RX and TX frequency is corrected (default 0)\n"
           "  --iq-convention C        hl2 (default): I/Q as the Hermes-Lite 2 sends it, for\n"
           "                           HL2 software such as piHPSDR; standard: I and Q\n"
           "                           swapped both ways, for plain complex-baseband clients\n"
           "  --p2-rx-gain DB          Protocol 2 LNA gain, -12 to 48 dB, less the client's\n"
           "                           step attenuation (default 14)\n"
           "  --no-realtime            no SCHED_FIFO threads or locked memory (default: both,\n"
           "                           which need CAP_SYS_NICE and CAP_IPC_LOCK)\n"
           "  --allow-tx               let the owning client transmit (default: receive only)\n"
           "  --allow-pa               let the client enable the PA while transmitting\n"
           "  --drive-ceiling N        highest drive level sent, 0 to 255 (default 0)\n"
           "  --tx-watchdog-s N        longest continuous MOX, seconds (default 120)\n"
           "  --log-level LEVEL        trace, debug, info, warn or error (default info)\n"
           "  --version                print the version and exit\n"
           "  -h, --help               print this and exit\n";
}

}  // namespace radioberry::daemon
