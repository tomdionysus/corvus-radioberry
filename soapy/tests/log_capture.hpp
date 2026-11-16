// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SoapySDR/Logger.hpp>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace radioberry::soapy::test {

// Collects SoapySDR's log messages, debug ones too, while it exists, then
// restores the default handler and level. One at a time.
class LogCapture {
public:
    LogCapture() {
        clear();
        SoapySDR::setLogLevel(SOAPY_SDR_DEBUG);
        SoapySDR::registerLogHandler(&handle);
    }
    ~LogCapture() {
        SoapySDR::registerLogHandler(nullptr);
        SoapySDR::setLogLevel(SOAPY_SDR_INFO);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<std::pair<SoapySDRLogLevel, std::string>> messages() const {
        const std::lock_guard lock(mutex());
        return store();
    }
    // True if a message contains `text`.
    bool contains(const std::string& text) const {
        for (const auto& m : messages()) {
            if (m.second.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    static void clear() {
        const std::lock_guard lock(mutex());
        store().clear();
    }

private:
    static void handle(SoapySDRLogLevel level, const char* message) {
        const std::lock_guard lock(mutex());
        store().emplace_back(level, message);
    }
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    static std::vector<std::pair<SoapySDRLogLevel, std::string>>& store() {
        static std::vector<std::pair<SoapySDRLogLevel, std::string>> s;
        return s;
    }
};

}  // namespace radioberry::soapy::test
