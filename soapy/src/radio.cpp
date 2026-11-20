// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>

#include <SoapySDR/Logger.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "device/capacity.hpp"
#include "find.hpp"
#include "frequency.hpp"
#include "log.hpp"
#include "radioberry/command.hpp"
#include "radioberry/version.hpp"
#include "realtime.hpp"

namespace radioberry::soapy {
namespace {

// The one client of the session: the program that opened the device.
const daemon::Owner kOwner{};

constexpr double kMaxFrequency = 38.4e6;  // half the AD9866's 76.8 MHz clock
constexpr double kDefaultFrequency = 7.074e6;
constexpr std::uint32_t kTxRateHz = 48000;
constexpr std::size_t kTxMtu = 1024;
// Samples per TX block handed to the device layer.
constexpr std::size_t kTxBlockSamples = daemon::TxBlock::kCapacity / kTxSampleBytes;
// RX waiting for readStream() at most, in seconds of samples; beyond it
// blocks are dropped and readStream() reports an overflow.
constexpr unsigned kQueueSeconds = 1;
constexpr std::uint8_t kTxFrequencyAddress = 0x01;
// Longest wait for queued commands to reach the device at activation.
constexpr std::chrono::milliseconds kCommandWait{250};
constexpr std::uint32_t kPaEnable = 1u << 19;

daemon::DeviceOptions device_options() {
    daemon::DeviceOptions o;
    // writeStream() keeps the queue short itself; nothing is dropped.
    o.tx_ring_limit_words = 0;
    o.on_thread_start = [](const char* role) {
        daemon::name_thread(std::string(role) == "rx" ? "rb-soapy-rx" : "rb-soapy-tx");
    };
    return o;
}

daemon::TxPolicyOptions tx_options(const Settings& settings, const radioberry_info& info) {
    daemon::TxPolicyOptions o = settings.tx;
    o.tx_available = info.transmitters != 0;
    return o;
}

std::string transport_name(unsigned transport) {
    switch (transport) {
        case RADIOBERRY_TRANSPORT_PIO:
            return "rp1-pio";
        case RADIOBERRY_TRANSPORT_I2S:
            return "i2s";
        case RADIOBERRY_TRANSPORT_CPU:
            return "cpu";
        default:
            return "unknown";
    }
}

std::string boolean(bool b) { return b ? "true" : "false"; }

void check_direction(int direction) {
    if (direction != SOAPY_SDR_RX && direction != SOAPY_SDR_TX) {
        throw std::invalid_argument("radioberry: direction " + std::to_string(direction) +
                                    " is neither RX nor TX");
    }
}

}  // namespace

std::string Radio::owner_prefix() { return daemon::describe(kOwner) + " "; }

Radio::Radio(std::unique_ptr<radioberry::Device> device, const Settings& settings,
             const Timing& timing, TxCounter tx_counter)
    : device_(std::move(device)),
      info_(device_->info()),
      settings_(settings),
      timing_(timing),
      tx_counter_(tx_counter ? std::move(tx_counter)
                             : [dir = settings.sysfs_dir] { return image_tx_samples(dir); }),
      clock_(daemon::steady_clock()),
      session_([this] { service_.control().safe(); }, std::chrono::nanoseconds::max()),
      service_(
          *device_, [this](const daemon::Fault& fault) { on_fault(fault); }, device_options(),
          daemon::steady_clock()),
      policy_(service_.control(), session_, tx_options(settings, info_)),
      ppm_(settings.frequency_ppm),
      lna_gain_db_(std::clamp(settings.lna_gain_db, kLnaGainMinDb, kLnaGainMaxDb)) {
    install_logger(owner_prefix());
    rx_frequency_.fill(kDefaultFrequency);
    tx_frequency_ = kDefaultFrequency;
    session_.claim(kOwner, clock_());
    if (!service_.start()) {
        throw std::runtime_error("radioberry: the device failed at start");
    }
    // As radioberryd for a client that owns the radio: with jack_keying the
    // key jack may key from now until the program closes the device.
    policy_.update(kOwner, false, clock_());
    {
        const std::lock_guard lock(mutex_);
        send_general_locked();
        for (std::size_t c = 0; c < info_.receivers; ++c) {
            send_frequency_locked(SOAPY_SDR_RX, c);
        }
        if (info_.transmitters != 0) {
            send_frequency_locked(SOAPY_SDR_TX, 0);
        }
        send(hl2::lna_gain(lna_gain_db_));
    }
    SoapySDR::logf(SOAPY_SDR_INFO, "radioberry: gateware %s, %u receivers, transmit %s",
                   version_text(info_.gateware_major, info_.gateware_minor).c_str(),
                   unsigned{info_.receivers},
                   info_.transmitters == 0 ? "absent (receive-only gateware)"
                   : settings_.tx.allow_tx ? "allowed"
                                           : "not allowed");
}

Radio::~Radio() {
    {
        const std::lock_guard lock(mutex_);
        if (tx_stream_) {
            deactivate_locked(*tx_stream_);
        }
        if (rx_stream_) {
            deactivate_locked(*rx_stream_);
        }
    }
    stop_ticker();
    stop_delivery();
    service_.stop();
}

// Identification

std::string Radio::getDriverKey() const { return kDriverKey; }

std::string Radio::getHardwareKey() const { return "RadioBerry 2.x"; }

SoapySDR::Kwargs Radio::getHardwareInfo() const {
    SoapySDR::Kwargs k;
    k["driver_version"] = driver_version_text(info_.driver_version);
    k["gateware_version"] = version_text(info_.gateware_major, info_.gateware_minor);
    k["fpga"] = fpga_text(info_.fpga_type);
    k["receivers"] = std::to_string(info_.receivers);
    k["transmitters"] = std::to_string(info_.transmitters);
    k["transport"] = transport_name(info_.transport);
    k["module_version"] = kVersion;
    k["transmit"] = settings_.tx.allow_tx ? "allowed" : "not allowed";
    k["config"] = settings_.from_file ? settings_.path : "none (receive only)";
    return k;
}

// Channels

std::size_t Radio::getNumChannels(int direction) const {
    check_direction(direction);
    return direction == SOAPY_SDR_RX ? info_.receivers : info_.transmitters;
}

bool Radio::getFullDuplex(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    return true;
}

void Radio::check_channel(int direction, std::size_t channel) const {
    if (channel >= getNumChannels(direction)) {
        throw std::invalid_argument("radioberry: no " +
                                    std::string(direction == SOAPY_SDR_RX ? "RX" : "TX") +
                                    " channel " + std::to_string(channel));
    }
}

// Streams

std::vector<std::string> Radio::getStreamFormats(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    if (direction == SOAPY_SDR_RX) {
        return {"CF32", "CS16", "CS32"};
    }
    return {"CF32", "CS16"};
}

std::string Radio::getNativeStreamFormat(int direction, std::size_t channel,
                                         double& full_scale) const {
    check_channel(direction, channel);
    if (direction == SOAPY_SDR_RX) {
        full_scale = kRxFullScale;
        return "CS32";
    }
    full_scale = kTxFullScale;
    return "CS16";
}

SoapySDR::Stream* Radio::setupStream(int direction, const std::string& format,
                                     const std::vector<std::size_t>& channels,
                                     const SoapySDR::Kwargs& args) {
    (void)args;
    check_direction(direction);
    const auto f = parse_format(format);
    if (!f || (direction == SOAPY_SDR_TX && *f == Format::CS32)) {
        throw std::invalid_argument("radioberry: stream format " + format +
                                    " not offered; use one of getStreamFormats()");
    }
    const std::vector<std::size_t> list = channels.empty() ? std::vector<std::size_t>{0} : channels;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] != i) {
            throw std::invalid_argument(
                "radioberry: a stream's channels are 0 to n-1 in order (the radio sends the "
                "first n receivers)");
        }
    }
    if (list.size() > getNumChannels(direction)) {
        throw std::invalid_argument("radioberry: " + std::to_string(list.size()) +
                                    " channels asked for, the gateware has " +
                                    std::to_string(getNumChannels(direction)));
    }
    const std::lock_guard lock(mutex_);
    std::unique_ptr<Stream>& slot = direction == SOAPY_SDR_RX ? rx_stream_ : tx_stream_;
    if (slot) {
        throw std::runtime_error("radioberry: a stream in that direction is already open");
    }
    slot = std::make_unique<Stream>();
    slot->direction = direction;
    slot->format = *f;
    slot->channels = static_cast<unsigned>(list.size());
    return reinterpret_cast<SoapySDR::Stream*>(slot.get());
}

Radio::Stream* Radio::stream_of(SoapySDR::Stream* stream) const {
    auto* s = reinterpret_cast<Stream*>(stream);
    if (s == nullptr || (s != rx_stream_.get() && s != tx_stream_.get())) {
        throw std::invalid_argument("radioberry: not a stream of this device");
    }
    return s;
}

void Radio::closeStream(SoapySDR::Stream* stream) {
    const std::lock_guard lock(mutex_);
    Stream* s = stream_of(stream);
    deactivate_locked(*s);
    (s->direction == SOAPY_SDR_RX ? rx_stream_ : tx_stream_).reset();
}

std::size_t Radio::getStreamMTU(SoapySDR::Stream* stream) const {
    const std::lock_guard lock(mutex_);
    const Stream* s = stream_of(stream);
    if (s->direction == SOAPY_SDR_RX) {
        return daemon::RxBlock::kCapacity / rx_frame_bytes(s->channels);
    }
    return kTxMtu;
}

int Radio::activateStream(SoapySDR::Stream* stream, int flags, long long time_ns,
                          std::size_t elements) {
    (void)time_ns;
    (void)elements;
    if ((flags & SOAPY_SDR_HAS_TIME) != 0) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const std::lock_guard lock(mutex_);
    Stream* s = stream_of(stream);
    if (s->active) {
        return 0;
    }
    if (s->direction == SOAPY_SDR_RX) {
        const unsigned most =
            daemon::max_receivers(rate_code_, info_.receivers, info_.rx_values_max);
        if (s->channels > most) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                           "radioberry: %u channels at %u ksps asked for; the link carries %u",
                           s->channels, daemon::rx_rate_hz(rate_code_) / 1000, most);
            return SOAPY_SDR_NOT_SUPPORTED;
        }
        s->active = true;
        {
            const std::lock_guard rx(rx_mutex_);
            rx_active_ = true;
            rx_overflow_ = false;
        }
        send_general_locked();
        // Run goes out on the next command sent. With commands still queued
        // ahead of the receiver count, RX would start in the old layout and
        // the driver would resynchronise; so they go first.
        wait_commands_sent();
    } else {
        // setupStream() has refused a TX stream on a receive-only image.
        if (!settings_.tx.allow_tx) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                           "radioberry: transmit is not allowed: set allow_tx: true in %s",
                           settings_.path.c_str());
            return SOAPY_SDR_NOT_SUPPORTED;
        }
        s->active = true;
        {
            const std::lock_guard tx(tx_mutex_);
            tx_active_ = true;
        }
        start_ticker();
    }
    update_run_locked();
    return 0;
}

int Radio::deactivateStream(SoapySDR::Stream* stream, int flags, long long time_ns) {
    (void)time_ns;
    if ((flags & SOAPY_SDR_HAS_TIME) != 0) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const std::lock_guard lock(mutex_);
    return deactivate_locked(*stream_of(stream));
}

int Radio::deactivate_locked(Stream& s) {
    if (!s.active) {
        return 0;
    }
    s.active = false;
    if (s.direction == SOAPY_SDR_RX) {
        {
            const std::lock_guard rx(rx_mutex_);
            rx_active_ = false;
            rx_overflow_ = false;
        }
        rx_cv_.notify_all();
        send_general_locked();
    } else {
        {
            std::unique_lock tx(tx_mutex_);
            if (burst_) {
                // The queued samples go out first, as at a burst's end.
                ending_ = true;
                tx_cv_.notify_all();
                tx_cv_.wait_for(tx, timing_.tx_drain + timing_.tx_tail_max,
                                [&] { return !burst_; });
                if (burst_) {
                    end_burst_locked(clock_());
                }
            }
            tx_active_ = false;
        }
        tx_cv_.notify_all();
        stop_ticker();
    }
    update_run_locked();
    return 0;
}

void Radio::update_run_locked() {
    const bool run = rx_streaming_locked() || tx_streaming_locked();
    service_.control().set_run(run);
    if (run) {
        start_delivery();
    } else {
        stop_delivery();
    }
}

void Radio::send(const Command& command) { service_.control().set(command); }

void Radio::wait_commands_sent() {
    const auto end = std::chrono::steady_clock::now() + kCommandWait;
    while (service_.control().pending() != 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Radio::send_general_locked() {
    const bool streaming = rx_streaming_locked();
    const unsigned receivers = streaming ? rx_stream_->channels : 1;
    const unsigned rate_code = streaming ? rate_code_ : 0;
    {
        const std::lock_guard rx(rx_mutex_);
        if (rx_receivers_ != receivers || rx_rate_code_ != rate_code) {
            clear_queue_locked();
        }
        rx_receivers_ = receivers;
        rx_rate_code_ = rate_code;
    }
    send(hl2::general(rate_code, receivers));
}

void Radio::send_frequency_locked(int direction, std::size_t channel) {
    const double hz = direction == SOAPY_SDR_RX ? rx_frequency_[channel] : tx_frequency_;
    const std::uint32_t value =
        daemon::corrected_frequency(static_cast<std::uint32_t>(std::lround(hz)), ppm_);
    if (direction == SOAPY_SDR_RX) {
        send(hl2::rx_frequency(static_cast<unsigned>(channel), value));
    } else {
        Command c;
        c.address = kTxFrequencyAddress;
        c.data = value;
        send(c);
    }
}

int Radio::readStream(SoapySDR::Stream* stream, void* const* buffs, std::size_t elements,
                      int& flags, long long& time_ns, long timeout_us) {
    Stream* s = nullptr;
    {
        const std::lock_guard lock(mutex_);
        s = stream_of(stream);
    }
    if (s->direction != SOAPY_SDR_RX) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    flags = 0;
    time_ns = 0;
    const auto timeout = std::chrono::microseconds(std::max(timeout_us, 0L));
    std::unique_lock lock(rx_mutex_);
    // Until the request can be filled, or the timeout; then whatever has
    // arrived. Programs that read once per period (Quisk) fall behind if a
    // read returns the few frames of the first block to arrive.
    rx_cv_.wait_for(lock, timeout, [&] {
        return failed_ || (rx_active_ && (rx_queued_frames_ >= elements || rx_overflow_));
    });
    if (failed_) {
        return SOAPY_SDR_STREAM_ERROR;
    }
    if (!rx_active_ || (rx_queue_.empty() && !rx_overflow_)) {
        return SOAPY_SDR_TIMEOUT;
    }
    if (rx_overflow_) {
        rx_overflow_ = false;
        return SOAPY_SDR_OVERFLOW;
    }
    const std::size_t frame = rx_frame_bytes(rx_receivers_);
    std::size_t done = 0;
    while (done < elements && !rx_queue_.empty()) {
        Chunk& chunk = rx_queue_.front();
        const std::size_t n = std::min(elements - done, chunk.frames - rx_offset_);
        rx_convert(chunk.bytes.data() + rx_offset_ * frame, n, rx_receivers_, s->channels,
                   s->format, buffs, done);
        done += n;
        rx_offset_ += n;
        rx_queued_frames_ -= n;
        if (rx_offset_ == chunk.frames) {
            rx_pool_.push_back(std::move(chunk));
            rx_queue_.pop_front();
            rx_offset_ = 0;
        }
    }
    return static_cast<int>(done);
}

void Radio::start_delivery() {
    if (delivery_.joinable()) {
        return;
    }
    delivery_stop_ = false;
    delivery_ = std::thread([this] { deliver(); });
}

void Radio::stop_delivery() {
    if (!delivery_.joinable()) {
        return;
    }
    delivery_stop_ = true;
    service_.rx().wake();
    delivery_.join();
    const std::lock_guard rx(rx_mutex_);
    clear_queue_locked();
}

void Radio::clear_queue_locked() {
    while (!rx_queue_.empty()) {
        rx_pool_.push_back(std::move(rx_queue_.front()));
        rx_queue_.pop_front();
    }
    rx_offset_ = 0;
    rx_queued_frames_ = 0;
}

void Radio::deliver() {
    daemon::name_thread("rb-soapy-deliv");
    auto& ring = service_.rx();
    while (!delivery_stop_) {
        ring.wait(delivery_stop_);
        bool delivered = false;
        while (const daemon::RxBlock* block = ring.front()) {
            {
                const std::lock_guard rx(rx_mutex_);
                // Blocks of another layout (before a change took effect)
                // and blocks with no RX stream to take them are dropped.
                if (rx_active_ && block->receivers == rx_receivers_ &&
                    block->rate_code == rx_rate_code_ && block->frames() > 0) {
                    const std::size_t limit =
                        std::size_t{daemon::rx_rate_hz(rx_rate_code_)} * kQueueSeconds;
                    // Full: the old samples go, not the new, so the read
                    // after the overflow report finds room and the
                    // newest samples. Dropping the new ones would keep
                    // the queue full, and every read an overflow.
                    if (rx_queued_frames_ + block->frames() > limit) {
                        SoapySDR::log(SOAPY_SDR_WARNING,
                                      "radioberry: RX samples dropped: the program is not "
                                      "reading the stream fast enough");
                        clear_queue_locked();
                        rx_overflow_ = true;
                    }
                    rx_queued_frames_ += block->frames();
                    Chunk chunk;
                    if (!rx_pool_.empty()) {
                        chunk = std::move(rx_pool_.back());
                        rx_pool_.pop_back();
                    }
                    chunk.bytes.assign(
                        block->bytes.begin(),
                        block->bytes.begin() + static_cast<std::ptrdiff_t>(block->size));
                    chunk.frames = block->frames();
                    rx_queue_.push_back(std::move(chunk));
                    delivered = true;
                }
            }
            ring.pop();
        }
        if (delivered) {
            rx_cv_.notify_all();
        }
    }
}

int Radio::writeStream(SoapySDR::Stream* stream, const void* const* buffs, std::size_t elements,
                       int& flags, long long time_ns, long timeout_us) {
    (void)time_ns;
    Stream* s = nullptr;
    {
        const std::lock_guard lock(mutex_);
        s = stream_of(stream);
    }
    if (s->direction != SOAPY_SDR_TX) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const bool end = (flags & SOAPY_SDR_END_BURST) != 0;
    flags = 0;
    std::unique_lock lock(tx_mutex_);
    if (!tx_active_ || failed_) {
        return SOAPY_SDR_STREAM_ERROR;
    }
    auto now = clock_();
    if (!burst_) {
        open_burst_locked(now);
    }
    ending_ = false;
    drained_since_.reset();
    if (!policy_.transmitting()) {
        // A fault or the watchdog dropped MOX; it stays off until the
        // burst ends.
        if (end) {
            ending_ = true;
            tx_cv_.notify_all();
        }
        return SOAPY_SDR_STREAM_ERROR;
    }
    const auto deadline = now + std::chrono::microseconds(std::max(timeout_us, 0L));
    std::size_t done = 0;
    while (done < elements && tx_active_ && !failed_ && policy_.transmitting()) {
        const std::uint32_t in_flight = tx_in_flight();
        const std::size_t room =
            timing_.tx_queue_samples > in_flight ? timing_.tx_queue_samples - in_flight : 0;
        daemon::TxBlock* block = room > 0 ? service_.tx().acquire() : nullptr;
        if (block == nullptr) {
            now = clock_();
            if (now >= deadline) {
                break;
            }
            // Until the samples over the limit have gone out at 48 ksps.
            const std::size_t want = std::min(elements - done, kTxBlockSamples);
            const std::size_t over = in_flight + want > timing_.tx_queue_samples
                                         ? in_flight + want - timing_.tx_queue_samples
                                         : 0;
            const std::chrono::nanoseconds left = deadline - now;
            const std::chrono::nanoseconds drain(
                static_cast<std::int64_t>(over * 1'000'000'000ull / kTxRateHz));
            tx_cv_.wait_for(lock, std::min(std::max<std::chrono::nanoseconds>(
                                               drain, std::chrono::milliseconds(1)),
                                           left));
            continue;
        }
        const std::size_t n = std::min({elements - done, room, kTxBlockSamples});
        clipped_ += tx_convert(buffs[0], done, n, s->format, block->bytes.data());
        block->size = n * kTxSampleBytes;
        service_.tx().publish();
        published_bytes_ += block->size;
        burst_samples_ += n;
        done += n;
    }
    if (end && done == elements) {
        ending_ = true;
        tx_cv_.notify_all();
    }
    if (done == 0) {
        return failed_ || !policy_.transmitting() ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;
    }
    return static_cast<int>(done);
}

std::uint32_t Radio::tx_in_flight() {
    const std::uint64_t queued = published_bytes_ - service_.snapshot().tx_bytes;
    return static_cast<std::uint32_t>(queued / kTxSampleBytes) + device_->stats().tx_ring_words;
}

bool Radio::tx_drained() { return service_.tx().empty() && device_->stats().tx_ring_words == 0; }

std::uint32_t Radio::drive_word() const {
    return static_cast<std::uint32_t>(drive_.load()) << 24 |
           (settings_.tx.allow_pa ? kPaEnable : 0u);
}

std::optional<std::uint32_t> Radio::tx_left_in_gateware_locked() {
    if (!burst_counter_base_) {
        return std::nullopt;
    }
    const auto count = tx_counter_();
    if (!count) {
        return std::nullopt;
    }
    // Modulo 2^32: the counter wraps.
    const std::uint32_t taken = *count - *burst_counter_base_;
    return burst_samples_ > taken ? static_cast<std::uint32_t>(burst_samples_ - taken) : 0u;
}

void Radio::open_burst_locked(std::chrono::nanoseconds now) {
    // Before MOX, so no sample of this burst is in the count yet.
    burst_counter_base_ = tx_counter_();
    burst_samples_ = 0;
    policy_.set_tx_drive(drive_word());
    policy_.update(kOwner, true, now);
    burst_ = true;
    clipped_ = 0;
    tx_cv_.notify_all();
}

void Radio::end_burst_locked(std::chrono::nanoseconds now) {
    policy_.update(kOwner, false, now);
    burst_ = false;
    ending_ = false;
    drained_since_.reset();
    if (clipped_ != 0) {
        SoapySDR::logf(SOAPY_SDR_WARNING, "radioberry: %zu TX samples beyond full scale, clipped",
                       clipped_);
        clipped_ = 0;
    }
    tx_cv_.notify_all();
}

bool Radio::burst_open() const {
    const std::lock_guard lock(tx_mutex_);
    return burst_;
}

// Only at the TX stream's activation: deactivation stops it.
void Radio::start_ticker() {
    {
        const std::lock_guard tx(tx_mutex_);
        ticker_stop_ = false;
    }
    ticker_ = std::thread([this] { tick(); });
}

void Radio::stop_ticker() {
    if (!ticker_.joinable()) {
        return;
    }
    {
        const std::lock_guard tx(tx_mutex_);
        ticker_stop_ = true;
    }
    tx_cv_.notify_all();
    ticker_.join();
}

void Radio::tick() {
    daemon::name_thread("rb-soapy-tick");
    std::unique_lock lock(tx_mutex_);
    std::chrono::nanoseconds wait = timing_.tx_tick;
    while (!ticker_stop_) {
        if (!burst_) {
            tx_cv_.wait(lock, [&] { return ticker_stop_ || burst_; });
            continue;
        }
        tx_cv_.wait_for(lock, wait);
        wait = timing_.tx_tick;
        if (ticker_stop_ || !burst_) {
            continue;
        }
        const auto now = clock_();
        policy_.tick(now);
        // Only the program ends a burst (END_BURST, deactivation): a pause
        // in its writes keeps the radio keyed, as a held PTT would. A hung
        // program meets the TX watchdog; a dead one, the driver's safe
        // command when the device closes.
        if (!ending_ || !tx_drained()) {
            drained_since_.reset();
            continue;
        }
        if (!drained_since_) {
            drained_since_ = now;
        }
        const auto since = now - *drained_since_;
        // The driver's ring is empty; the gateware still sends what its
        // FIFO holds. MOX goes when it has taken the last sample: earlier
        // cuts the burst short, later sends nothing while keyed.
        if (const auto left = tx_left_in_gateware_locked()) {
            if (*left == 0) {
                end_burst_locked(now);
            } else if (since >= timing_.tx_tail_max) {
                SoapySDR::logf(SOAPY_SDR_WARNING,
                               "radioberry: the gateware did not take the last %u TX samples",
                               *left);
                end_burst_locked(now);
            } else {
                wait = std::max<std::chrono::nanoseconds>(
                    std::chrono::nanoseconds(std::int64_t{*left} * 1'000'000'000 / kTxRateHz),
                    std::chrono::milliseconds(1));
            }
        } else if (since >= timing_.tx_tail) {
            end_burst_locked(now);
        }
    }
}

void Radio::on_fault(const daemon::Fault& fault) {
    const std::string text = "radioberry: " + daemon::describe(fault);
    switch (fault.kind) {
        case daemon::FaultKind::DeviceError:
            failed_ = true;
            SoapySDR::log(SOAPY_SDR_ERROR, text);
            break;
        case daemon::FaultKind::TxUnderrun:
            SoapySDR::log(SOAPY_SDR_DEBUG, text);
            break;
        default:
            SoapySDR::log(SOAPY_SDR_WARNING, text);
            break;
    }
    if (fault.kind == daemon::FaultKind::RxOverrun ||
        fault.kind == daemon::FaultKind::RxQueueFull) {
        const std::lock_guard rx(rx_mutex_);
        if (rx_active_) {
            rx_overflow_ = true;
        }
    }
    if (fault.kind == daemon::FaultKind::DeviceError ||
        fault.kind == daemon::FaultKind::TxInvalidBlock) {
        session_.fault(daemon::describe(fault));
    }
    rx_cv_.notify_all();
    tx_cv_.notify_all();
}

// Antennas

std::vector<std::string> Radio::listAntennas(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    return {direction == SOAPY_SDR_RX ? "RX" : "TX"};
}

void Radio::setAntenna(int direction, std::size_t channel, const std::string& name) {
    // An empty name is no choice (CubicSDR sends one).
    if (!name.empty() && name != getAntenna(direction, channel)) {
        throw std::invalid_argument("radioberry: no antenna " + name);
    }
}

std::string Radio::getAntenna(int direction, std::size_t channel) const {
    return listAntennas(direction, channel).front();
}

// Frequency correction

bool Radio::hasFrequencyCorrection(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    return true;
}

void Radio::setFrequencyCorrection(int direction, std::size_t channel, double ppm) {
    check_channel(direction, channel);
    if (!(ppm >= -200 && ppm <= 200)) {
        throw std::invalid_argument("radioberry: frequency correction from -200 to 200 ppm");
    }
    const std::lock_guard lock(mutex_);
    ppm_ = ppm;
    for (std::size_t c = 0; c < info_.receivers; ++c) {
        send_frequency_locked(SOAPY_SDR_RX, c);
    }
    if (info_.transmitters != 0) {
        send_frequency_locked(SOAPY_SDR_TX, 0);
    }
}

double Radio::getFrequencyCorrection(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    const std::lock_guard lock(mutex_);
    return ppm_;
}

// Gain

std::vector<std::string> Radio::listGains(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    return {direction == SOAPY_SDR_RX ? "LNA" : "DRIVE"};
}

void Radio::setGain(int direction, std::size_t channel, double value) {
    setGain(direction, channel, listGains(direction, channel).front(), value);
}

void Radio::setGain(int direction, std::size_t channel, const std::string& name, double value) {
    if (name != listGains(direction, channel).front()) {
        throw std::invalid_argument("radioberry: no gain element " + name);
    }
    const SoapySDR::Range range = getGainRange(direction, channel, name);
    const double v = std::clamp(value, range.minimum(), range.maximum());
    if (direction == SOAPY_SDR_RX) {
        const std::lock_guard lock(mutex_);
        lna_gain_db_ = static_cast<int>(std::lround(v));
        send(hl2::lna_gain(lna_gain_db_));
        return;
    }
    const auto code = static_cast<unsigned>(std::lround(v * 2));
    drive_ = drive_for_code(code, settings_.tx.drive_ceiling);
    const std::lock_guard tx(tx_mutex_);
    policy_.set_tx_drive(drive_word());
    if (burst_) {
        policy_.update(kOwner, true, clock_());
    }
}

double Radio::getGain(int direction, std::size_t channel) const {
    return getGain(direction, channel, listGains(direction, channel).front());
}

double Radio::getGain(int direction, std::size_t channel, const std::string& name) const {
    if (name != listGains(direction, channel).front()) {
        throw std::invalid_argument("radioberry: no gain element " + name);
    }
    if (direction == SOAPY_SDR_RX) {
        const std::lock_guard lock(mutex_);
        return lna_gain_db_;
    }
    return drive_code(drive_) * 0.5;
}

SoapySDR::Range Radio::getGainRange(int direction, std::size_t channel) const {
    return getGainRange(direction, channel, listGains(direction, channel).front());
}

SoapySDR::Range Radio::getGainRange(int direction, std::size_t channel,
                                    const std::string& name) const {
    if (name != listGains(direction, channel).front()) {
        throw std::invalid_argument("radioberry: no gain element " + name);
    }
    if (direction == SOAPY_SDR_RX) {
        return {kLnaGainMinDb, kLnaGainMaxDb, 1};
    }
    return {0, drive_code(settings_.tx.drive_ceiling) * 0.5, 0.5};
}

// Frequency

void Radio::setFrequency(int direction, std::size_t channel, double frequency,
                         const SoapySDR::Kwargs& args) {
    setFrequency(direction, channel, "RF", frequency, args);
}

void Radio::setFrequency(int direction, std::size_t channel, const std::string& name,
                         double frequency, const SoapySDR::Kwargs& args) {
    (void)args;
    check_channel(direction, channel);
    if (name != "RF") {
        throw std::invalid_argument("radioberry: no frequency component " + name);
    }
    if (std::isnan(frequency) ||
        (direction == SOAPY_SDR_TX && (frequency < 0 || frequency > kMaxFrequency))) {
        throw std::invalid_argument("radioberry: frequency outside 0 to 38.4 MHz");
    }
    // A receive frequency outside the range is clamped to it, as programs
    // that start at a default of their own (CubicSDR's 100 MHz) expect.
    const double hz = std::clamp(frequency, 0.0, kMaxFrequency);
    if (hz != frequency) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "radioberry: RX frequency %.0f Hz outside 0 to 38.4 MHz; %.0f Hz used",
                       frequency, hz);
    }
    const std::lock_guard lock(mutex_);
    (direction == SOAPY_SDR_RX ? rx_frequency_[channel] : tx_frequency_) = hz;
    send_frequency_locked(direction, channel);
}

double Radio::getFrequency(int direction, std::size_t channel) const {
    return getFrequency(direction, channel, "RF");
}

double Radio::getFrequency(int direction, std::size_t channel, const std::string& name) const {
    check_channel(direction, channel);
    if (name != "RF") {
        throw std::invalid_argument("radioberry: no frequency component " + name);
    }
    const std::lock_guard lock(mutex_);
    return direction == SOAPY_SDR_RX ? rx_frequency_[channel] : tx_frequency_;
}

std::vector<std::string> Radio::listFrequencies(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    return {"RF"};
}

SoapySDR::RangeList Radio::getFrequencyRange(int direction, std::size_t channel) const {
    return getFrequencyRange(direction, channel, "RF");
}

SoapySDR::RangeList Radio::getFrequencyRange(int direction, std::size_t channel,
                                             const std::string& name) const {
    check_channel(direction, channel);
    if (name != "RF") {
        throw std::invalid_argument("radioberry: no frequency component " + name);
    }
    return {SoapySDR::Range(0, kMaxFrequency)};
}

// Sample rate

void Radio::setSampleRate(int direction, std::size_t channel, double rate) {
    check_channel(direction, channel);
    const auto rates = listSampleRates(direction, channel);
    const auto it = std::find_if(rates.begin(), rates.end(),
                                 [&](double r) { return std::fabs(r - rate) < 0.5; });
    if (it == rates.end()) {
        throw std::invalid_argument("radioberry: sample rate " + std::to_string(rate) +
                                    " not offered; use one of listSampleRates()");
    }
    if (direction == SOAPY_SDR_TX) {
        return;
    }
    const auto code = static_cast<unsigned>(it - rates.begin());
    const std::lock_guard lock(mutex_);
    if (rx_streaming_locked()) {
        const unsigned most = daemon::max_receivers(code, info_.receivers, info_.rx_values_max);
        if (rx_stream_->channels > most) {
            throw std::invalid_argument("radioberry: the link carries " + std::to_string(most) +
                                        " channels at that rate; the stream has " +
                                        std::to_string(rx_stream_->channels));
        }
    }
    rate_code_ = code;
    send_general_locked();
}

double Radio::getSampleRate(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    if (direction == SOAPY_SDR_TX) {
        return kTxRateHz;
    }
    const std::lock_guard lock(mutex_);
    return daemon::rx_rate_hz(rate_code_);
}

std::vector<double> Radio::listSampleRates(int direction, std::size_t channel) const {
    check_channel(direction, channel);
    if (direction == SOAPY_SDR_TX) {
        return {kTxRateHz};
    }
    return {48000, 96000, 192000, 384000};
}

SoapySDR::RangeList Radio::getSampleRateRange(int direction, std::size_t channel) const {
    SoapySDR::RangeList ranges;
    for (const double r : listSampleRates(direction, channel)) {
        ranges.emplace_back(r, r);
    }
    return ranges;
}

// Sensors

std::vector<std::string> Radio::listSensors() const {
    return {"transmitting", "rx_overruns", "rx_resyncs", "tx_underruns",
            "rx_stalled",   "ptt_input",   "cw_key"};
}

SoapySDR::ArgInfo Radio::getSensorInfo(const std::string& key) const {
    SoapySDR::ArgInfo info;
    info.key = key;
    info.name = key;
    if (key == "transmitting") {
        info.description = "MOX is granted: PTT and the TX permission bit are set";
    } else if (key == "rx_overruns") {
        info.description = "RX frames the driver dropped since the device was opened";
    } else if (key == "rx_resyncs") {
        info.description = "times the driver lost RX frame alignment";
    } else if (key == "tx_underruns") {
        info.description = "times the driver's TX ring ran empty with PTT set";
    } else if (key == "rx_stalled") {
        info.description = "no RX frames for 250 ms while streaming";
    } else if (key == "ptt_input") {
        info.description = "the gateware's CW or external PTT input";
    } else if (key == "cw_key") {
        info.description = "the external CW key input";
    } else {
        throw std::invalid_argument("radioberry: no sensor " + key);
    }
    const bool counter = key == "rx_overruns" || key == "rx_resyncs" || key == "tx_underruns";
    info.type = counter ? SoapySDR::ArgInfo::INT : SoapySDR::ArgInfo::BOOL;
    return info;
}

std::string Radio::readSensor(const std::string& key) const {
    (void)getSensorInfo(key);  // throws for an unknown key
    const daemon::DeviceSnapshot s = service_.snapshot();
    const radioberry_stats driver = s.driver.value_or(radioberry_stats{});
    if (key == "transmitting") {
        return boolean(policy_.transmitting());
    }
    if (key == "rx_overruns") {
        return std::to_string(driver.rx_overrun_frames);
    }
    if (key == "rx_resyncs") {
        return std::to_string(driver.rx_resyncs);
    }
    if (key == "tx_underruns") {
        return std::to_string(driver.tx_underruns);
    }
    if (key == "rx_stalled") {
        return boolean(s.stalled);
    }
    if (key == "ptt_input") {
        return boolean(s.cw_or_ptt);
    }
    return boolean(s.external_cw_key);
}

}  // namespace radioberry::soapy
