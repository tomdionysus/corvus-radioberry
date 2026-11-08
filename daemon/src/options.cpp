// SPDX-License-Identifier: GPL-2.0-or-later
#include "options.hpp"

#include <arpa/inet.h>

#include <charconv>
#include <cstdio>
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

}  // namespace

Options parse_options(const std::vector<std::string>& args) {
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
            o.daemon.server.client_timeout =
                std::chrono::milliseconds(number(a, value(), 3'600'000));
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
           "Serves the Radioberry to SDR clients over openHPSDR Protocol 1 (Hermes-Lite 2\n"
           "dialect), UDP and TCP. This version receives only: it never sets PTT or the\n"
           "TX permission bit, and it claims no PA interlock.\n"
           "\n"
           "  --device PATH            radioberry.ko device (default /dev/radioberry)\n"
           "  --address IPV4           address to listen on (default all)\n"
           "  --port N                 Protocol 1 port (default 1024)\n"
           "  --mac XX:XX:XX:XX:XX:XX  MAC reported by discovery (default: the interface's)\n"
           "  --client-timeout-ms N    silence that ends a client's stream (default 3000)\n"
           "  --log-level LEVEL        trace, debug, info, warn or error (default info)\n"
           "  --version                print the version and exit\n"
           "  -h, --help               print this and exit\n";
}

}  // namespace radioberry::daemon
