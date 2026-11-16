// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SoapySDR/Device.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "convert.hpp"
#include "device/service.hpp"
#include "radioberry/device.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "tx_policy.hpp"

namespace radioberry::soapy {

// The RadioBerry as a SoapySDR device, over /dev/radioberry. The device
// layer, the session and the transmit policy are radioberryd's own
// (daemon/src), so the radio is driven the same way: commands queued in
// Control, an RX and a TX device thread, MOX only through TxPolicy.
//
// Receive: the gateware's receivers are the channels, one rate for all.
// Transmit: one channel at 48 ksps. SoapySDR has no PTT call; activating
// and deactivating the TX stream is key down and key up (as Quisk and the
// HackRF module use it). MOX is requested while a burst is open: from the
// first write after activation or after the last burst, until a write with
// SOAPY_SDR_END_BURST or the stream's deactivation; then, once the
// gateware has taken the last sample, MOX is released. Transmit needs allow_tx
// in the config; the drive ceiling, PA permission and watchdog apply as in
// radioberryd.
class Radio final : public SoapySDR::Device {
public:
    // The gateware's count of TX samples taken, if it can be read.
    using TxCounter = std::function<std::optional<std::uint32_t>()>;

    // `tx_counter` defaults to the driver's sysfs counters.
    Radio(std::unique_ptr<radioberry::Device> device, const Settings& settings,
          const Timing& timing = {}, TxCounter tx_counter = {});
    ~Radio() override;
    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;

    // Identification
    std::string getDriverKey() const override;
    std::string getHardwareKey() const override;
    SoapySDR::Kwargs getHardwareInfo() const override;

    // Channels
    std::size_t getNumChannels(int direction) const override;
    bool getFullDuplex(int direction, std::size_t channel) const override;

    // Streams
    std::vector<std::string> getStreamFormats(int direction, std::size_t channel) const override;
    std::string getNativeStreamFormat(int direction, std::size_t channel,
                                      double& full_scale) const override;
    SoapySDR::Stream* setupStream(int direction, const std::string& format,
                                  const std::vector<std::size_t>& channels,
                                  const SoapySDR::Kwargs& args) override;
    void closeStream(SoapySDR::Stream* stream) override;
    std::size_t getStreamMTU(SoapySDR::Stream* stream) const override;
    int activateStream(SoapySDR::Stream* stream, int flags, long long time_ns,
                       std::size_t elements) override;
    int deactivateStream(SoapySDR::Stream* stream, int flags, long long time_ns) override;
    int readStream(SoapySDR::Stream* stream, void* const* buffs, std::size_t elements, int& flags,
                   long long& time_ns, long timeout_us) override;
    int writeStream(SoapySDR::Stream* stream, const void* const* buffs, std::size_t elements,
                    int& flags, long long time_ns, long timeout_us) override;

    // Antennas
    std::vector<std::string> listAntennas(int direction, std::size_t channel) const override;
    void setAntenna(int direction, std::size_t channel, const std::string& name) override;
    std::string getAntenna(int direction, std::size_t channel) const override;

    // Frequency correction, in ppm, shared by every channel
    bool hasFrequencyCorrection(int direction, std::size_t channel) const override;
    void setFrequencyCorrection(int direction, std::size_t channel, double ppm) override;
    double getFrequencyCorrection(int direction, std::size_t channel) const override;

    // Gain: LNA on receive (shared by the channels: one ADC), DRIVE on
    // transmit.
    std::vector<std::string> listGains(int direction, std::size_t channel) const override;
    void setGain(int direction, std::size_t channel, double value) override;
    void setGain(int direction, std::size_t channel, const std::string& name,
                 double value) override;
    double getGain(int direction, std::size_t channel) const override;
    double getGain(int direction, std::size_t channel, const std::string& name) const override;
    SoapySDR::Range getGainRange(int direction, std::size_t channel) const override;
    SoapySDR::Range getGainRange(int direction, std::size_t channel,
                                 const std::string& name) const override;

    // Frequency
    void setFrequency(int direction, std::size_t channel, double frequency,
                      const SoapySDR::Kwargs& args) override;
    void setFrequency(int direction, std::size_t channel, const std::string& name, double frequency,
                      const SoapySDR::Kwargs& args) override;
    double getFrequency(int direction, std::size_t channel) const override;
    double getFrequency(int direction, std::size_t channel, const std::string& name) const override;
    std::vector<std::string> listFrequencies(int direction, std::size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(int direction, std::size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(int direction, std::size_t channel,
                                          const std::string& name) const override;

    // Sample rate
    void setSampleRate(int direction, std::size_t channel, double rate) override;
    double getSampleRate(int direction, std::size_t channel) const override;
    std::vector<double> listSampleRates(int direction, std::size_t channel) const override;
    SoapySDR::RangeList getSampleRateRange(int direction, std::size_t channel) const override;

    // Sensors
    std::vector<std::string> listSensors() const override;
    SoapySDR::ArgInfo getSensorInfo(const std::string& key) const override;
    std::string readSensor(const std::string& key) const override;

    // For tests: MOX is granted now.
    bool transmitting() const { return policy_.transmitting(); }
    // For tests: a transmit burst is open.
    bool burst_open() const;

    // The prefix of the session's messages about the radio's owner, which
    // the module's logger leaves out.
    static std::string owner_prefix();

private:
    struct Stream {
        int direction = 0;
        Format format = Format::CF32;
        unsigned channels = 1;
        bool active = false;
    };
    // A copy of one RX block, waiting for readStream().
    struct Chunk {
        std::vector<std::uint8_t> bytes;
        std::size_t frames = 0;
    };

    Stream* stream_of(SoapySDR::Stream* stream) const;
    void check_channel(int direction, std::size_t channel) const;
    void on_fault(const daemon::Fault& fault);
    // Run is set while either stream is active, so the radio's link runs
    // for transmit too; the receive delivery runs while run is set.
    void update_run_locked();
    void send(const Command& command);
    void send_frequency_locked(int direction, std::size_t channel);
    // Address 0x00: the RX stream's rate and receivers, or one receiver at
    // 48 ksps without one. Clears blocks of another layout.
    void send_general_locked();
    bool rx_streaming_locked() const { return rx_stream_ && rx_stream_->active; }
    bool tx_streaming_locked() const { return tx_stream_ && tx_stream_->active; }
    int deactivate_locked(Stream& stream);
    // Waits, briefly, until the device thread has sent every queued command.
    void wait_commands_sent();

    // Receive delivery: a thread moves blocks from the device layer's RX
    // ring into rx_queue_, where readStream() waits with a timeout.
    void start_delivery();
    void stop_delivery();
    void deliver();
    void clear_queue_locked();  // rx_mutex_ held

    // Transmit.
    void start_ticker();
    void stop_ticker();
    void tick();
    // Samples queued between this module and the gateware.
    std::uint32_t tx_in_flight();
    bool tx_drained();
    // Samples of this burst the gateware has not yet taken, if its counter
    // can be read.
    std::optional<std::uint32_t> tx_left_in_gateware_locked();
    void open_burst_locked(std::chrono::nanoseconds now);
    void end_burst_locked(std::chrono::nanoseconds now);
    // Address 0x09 as requested: the drive byte, and PA enable when the
    // config allows the PA (TxPolicy caps both).
    std::uint32_t drive_word() const;

    std::unique_ptr<radioberry::Device> device_;
    const radioberry_info info_;
    const Settings settings_;
    const Timing timing_;
    const TxCounter tx_counter_;
    const daemon::Clock clock_;
    daemon::Session session_;
    daemon::DeviceService service_;
    daemon::TxPolicy policy_;

    // Radio settings and the streams. Lock order: mutex_, then rx_mutex_
    // or tx_mutex_, then the policy's and the session's.
    mutable std::mutex mutex_;
    double ppm_;
    std::array<double, 12> rx_frequency_{};
    double tx_frequency_ = 0;
    int lna_gain_db_;
    unsigned rate_code_ = 0;
    std::unique_ptr<Stream> rx_stream_;
    std::unique_ptr<Stream> tx_stream_;
    std::atomic<unsigned> drive_{0};  // the drive byte, 0 to the ceiling
    std::atomic<bool> failed_{false};

    // Receive delivery, while run is set.
    std::mutex rx_mutex_;
    std::condition_variable rx_cv_;
    std::deque<Chunk> rx_queue_;
    std::vector<Chunk> rx_pool_;
    std::size_t rx_offset_ = 0;         // frames of rx_queue_.front() already read
    std::size_t rx_queued_frames_ = 0;  // frames in rx_queue_ not yet read
    bool rx_active_ = false;            // a stream wants the blocks
    unsigned rx_receivers_ = 1;         // the layout it wants
    unsigned rx_rate_code_ = 0;
    bool rx_overflow_ = false;
    std::atomic<bool> delivery_stop_{false};
    std::thread delivery_;

    // Transmit.
    mutable std::mutex tx_mutex_;
    std::condition_variable tx_cv_;
    bool tx_active_ = false;
    bool burst_ = false;
    bool ending_ = false;
    std::optional<std::chrono::nanoseconds> drained_since_;
    std::uint64_t published_bytes_ = 0;
    // The gateware's sample count when the burst opened, and the samples
    // published in it.
    std::optional<std::uint32_t> burst_counter_base_;
    std::uint64_t burst_samples_ = 0;
    std::size_t clipped_ = 0;
    bool ticker_stop_ = false;
    std::thread ticker_;
};

}  // namespace radioberry::soapy
