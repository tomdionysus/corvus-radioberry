// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>

namespace radioberry::soapy {
namespace {

class SoapySink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit SoapySink(std::string drop_prefix) : drop_prefix_(std::move(drop_prefix)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const std::string text(msg.payload.data(), msg.payload.size());
        if (!drop_prefix_.empty() && text.starts_with(drop_prefix_)) {
            return;
        }
        SoapySDR::log(soapy_level(msg.level), "radioberry: " + text);
    }
    void flush_() override {}

private:
    std::string drop_prefix_;
};

}  // namespace

SoapySDRLogLevel soapy_level(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::critical:
            return SOAPY_SDR_CRITICAL;
        case spdlog::level::err:
            return SOAPY_SDR_ERROR;
        case spdlog::level::warn:
            return SOAPY_SDR_WARNING;
        case spdlog::level::info:
            return SOAPY_SDR_INFO;
        default:
            return SOAPY_SDR_DEBUG;
    }
}

void install_logger(const std::string& drop_prefix) {
    static std::once_flag once;
    std::call_once(once, [&] {
        auto logger = std::make_shared<spdlog::logger>("radioberry",
                                                       std::make_shared<SoapySink>(drop_prefix));
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
    });
}

}  // namespace radioberry::soapy
