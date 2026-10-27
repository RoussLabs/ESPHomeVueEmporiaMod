#include "emporia_vue.h"
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdio>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace emporia_vue {

static const char *const TAG = "emporia_vue";

#if defined(EMPORIA_VUE_VARIANT_VUE3)
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 19610.0f;
#else
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 25310.0f;
#endif

void EmporiaVueComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Emporia Vue");
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  ESP_LOGCONFIG(TAG, "  Variant: Vue 3");
#else
  ESP_LOGCONFIG(TAG, "  Variant: Vue 2");
#endif
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Frequency samples: %u", this->timing_samples_);
  ESP_LOGCONFIG(TAG, "  Checksum mode: %d", static_cast<int>(this->checksum_mode_));
  LOG_SENSOR("  ", "I2C Errors", this->i2c_error_sensor_);
  LOG_SENSOR("  ", "Malformed Readings", this->malformed_reading_sensor_);
  LOG_SENSOR("  ", "Missed Readings", this->missed_reading_sensor_);

  for (auto *phase : this->phases_) {
    const char *wire = "UNKNOWN";
    switch (phase->get_input_wire()) {
      case PhaseInputWire::BLACK:
        wire = "BLACK";
        break;
      case PhaseInputWire::RED:
        wire = "RED";
        break;
      case PhaseInputWire::BLUE:
        wire = "BLUE";
        break;
    }
    ESP_LOGCONFIG(TAG, "  Phase");
    ESP_LOGCONFIG(TAG, "    Wire: %s", wire);
    ESP_LOGCONFIG(TAG, "    Calibration: %f", phase->get_calibration());
    LOG_SENSOR("    ", "Voltage", phase->get_voltage_sensor());
    LOG_SENSOR("    ", "Frequency", phase->get_frequency_sensor());
    LOG_SENSOR("    ", "Phase Angle", phase->get_phase_angle_sensor());
  }

  for (auto *vphase : this->virtual_phases_) {
    ESP_LOGCONFIG(TAG, "  Virtual Phase (line-to-line)");
    ESP_LOGCONFIG(TAG, "    Angle: %.0f deg", vphase->get_angle_deg());
    LOG_SENSOR("    ", "Voltage", vphase->get_voltage_sensor());
  }

  for (auto *ct_clamp : this->ct_clamps_) {
    ESP_LOGCONFIG(TAG, "  CT Clamp");
    ESP_LOGCONFIG(TAG, "    CT Port Index: %d", ct_clamp->get_input_port());
    ESP_LOGCONFIG(TAG, "    Calibration: %f", ct_clamp->get_calibration());
    LOG_SENSOR("    ", "Power", ct_clamp->get_power_sensor());
    LOG_SENSOR("    ", "Current", ct_clamp->get_current_sensor());
    LOG_SENSOR("    ", "Apparent Power", ct_clamp->get_apparent_power_sensor());
    LOG_SENSOR("    ", "Reactive Power", ct_clamp->get_reactive_power_sensor());
    LOG_SENSOR("    ", "Power Factor", ct_clamp->get_power_factor_sensor());
  }
}

void EmporiaVueComponent::update() {
  SensorReading sensor_reading;

  i2c::ErrorCode err = read(reinterpret_cast<uint8_t *>(&sensor_reading), sizeof(sensor_reading));

  if (err != i2c::ErrorCode::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read from sensor due to I2C error %d", err);
    this->bump_counter_(this->i2c_errors_, this->i2c_error_sensor_);
    return;
  }

  if (sensor_reading.end != 0) {
    ESP_LOGE(TAG, "Failed to read from sensor due to a malformed reading, should end in null bytes but is %d",
             sensor_reading.end);
    this->bump_counter_(this->malformed_readings_, this->malformed_reading_sensor_);
    return;
  }

  if (!sensor_reading.is_unread) {
    ESP_LOGV(TAG, "Ignoring sensor reading that is marked as read");
    return;
  }

  if (!this->check_checksum_(sensor_reading)) {
    this->bump_counter_(this->malformed_readings_, this->malformed_reading_sensor_);
    return;
  }

  ESP_LOGV(TAG, "Received sensor reading with sequence #%d", sensor_reading.sequence_num);

  if (this->have_last_sequence_num_) {
    // uint8_t arithmetic wraps correctly across 255 -> 0
    uint8_t gap = static_cast<uint8_t>(sensor_reading.sequence_num - this->last_sequence_num_ - 1);
    if (gap != 0) {
      ESP_LOGW(TAG, "Detected %u missing reading(s), data may not be accurate!", gap);
      this->missed_readings_ += gap;
      if (this->missed_reading_sensor_)
        this->missed_reading_sensor_->publish_state(this->missed_readings_);
    }
  }
  this->have_last_sequence_num_ = true;

  AcTiming timing = this->average_timing_(sensor_reading);

  for (auto *phase : this->phases_) {
    phase->update_from_reading(sensor_reading, timing);
  }
  // after the physical legs, so their angles are current
  for (auto *vphase : this->virtual_phases_) {
    vphase->update_from_reading(sensor_reading, timing);
  }
  for (auto *ct_clamp : this->ct_clamps_) {
    ct_clamp->update_from_reading(sensor_reading);
  }

  this->last_sequence_num_ = sensor_reading.sequence_num;

  this->callback_.call();
}

void EmporiaVueComponent::add_on_update_callback(std::function<void()> &&callback) {
  this->callback_.add(std::move(callback));
}

void EmporiaVueComponent::setup() {
  // Allocate the averaging buffer here; no heap allocation once the loop is running.
  if (this->timing_samples_ > 1) {
    this->timing_history_.assign(this->timing_samples_, RawTiming{0, {0, 0}});
  }

  // Publish zero so the diagnostic entities show a value before the first fault.
  if (this->i2c_error_sensor_)
    this->i2c_error_sensor_->publish_state(0);
  if (this->malformed_reading_sensor_)
    this->malformed_reading_sensor_->publish_state(0);
  if (this->missed_reading_sensor_)
    this->missed_reading_sensor_->publish_state(0);
}

// Candidate checksums over the body (after the 4-byte header) and over the
// whole packet minus the checksum byte; the real algorithm is not yet known.
struct ChecksumCandidates {
  uint8_t sum_body, xor_body, negsum_body;
  uint8_t sum_all, xor_all, negsum_all;
};

static ChecksumCandidates compute_checksum_candidates(const SensorReading &reading) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&reading);
  const size_t len = sizeof(SensorReading);
  const size_t checksum_offset = offsetof(SensorReading, checksum);
  const size_t body_start = offsetof(SensorReading, power);

  uint8_t sum_body = 0, xor_body = 0, sum_all = 0, xor_all = 0;
  for (size_t i = 0; i < len; i++) {
    if (i == checksum_offset)
      continue;
    sum_all += bytes[i];
    xor_all ^= bytes[i];
    if (i >= body_start) {
      sum_body += bytes[i];
      xor_body ^= bytes[i];
    }
  }
  return ChecksumCandidates{sum_body, xor_body, static_cast<uint8_t>(-sum_body),
                            sum_all,  xor_all,  static_cast<uint8_t>(-sum_all)};
}

// Raw per-port values, index order is the MCU's (Vue 3: 0=C 1=B 2=A, then 1..16).
void EmporiaVueComponent::log_raw_ports_(const SensorReading &reading) {
  char buf[19 * 7 + 1];
  size_t pos = 0;
  for (int i = 0; i < 19; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%04X", i ? " " : "", reading.current[i]);
  ESP_LOGI(TAG, "raw current[0..18] hex: %s", buf);

  pos = 0;
  char pbuf[19 * 12 + 1];
  for (int i = 0; i < 19; i++)
    pos += snprintf(pbuf + pos, sizeof(pbuf) - pos, "%s%ld", i ? " " : "", (long) reading.power[i].phase_black);
  ESP_LOGI(TAG, "raw power vs BLACK[0..18]: %s", pbuf);

  pos = 0;
  for (int i = 0; i < 19; i++)
    pos += snprintf(pbuf + pos, sizeof(pbuf) - pos, "%s%ld", i ? " " : "", (long) reading.power[i].phase_red);
  ESP_LOGI(TAG, "raw power vs RED[0..18]:   %s", pbuf);

  ESP_LOGI(TAG, "raw voltage: %u %u %u  period: %u  degrees: %u %u", reading.voltage[0], reading.voltage[1],
           reading.voltage[2], reading.frequency, reading.degrees[0], reading.degrees[1]);

  // Whole packet as hex, 71 bytes per line, for offline checksum analysis.
  const auto *bytes = reinterpret_cast<const uint8_t *>(&reading);
  char hbuf[71 * 2 + 1];
  for (size_t off = 0; off < sizeof(SensorReading); off += 71) {
    size_t n = std::min<size_t>(71, sizeof(SensorReading) - off);
    for (size_t i = 0; i < n; i++)
      snprintf(hbuf + i * 2, 3, "%02X", bytes[off + i]);
    ESP_LOGI(TAG, "pkt[%03u]: %s", (unsigned) off, hbuf);
  }
}

bool EmporiaVueComponent::check_checksum_(const SensorReading &reading) {
  if (this->checksum_mode_ == ChecksumMode::CHECKSUM_MODE_NONE)
    return true;

  ChecksumCandidates c = compute_checksum_candidates(reading);

  if (this->checksum_mode_ == ChecksumMode::CHECKSUM_MODE_DISCOVER) {
    // Log the first 10 packets, then one every ~60 s, so a log attached at any time sees data.
    uint32_t n = this->discover_counter_++;
    if (n < 10 || n % 250 == 0) {
      ESP_LOGI(TAG,
               "checksum=0x%02X unknown=0x%02X seq=%u | body: sum8=0x%02X xor8=0x%02X negsum8=0x%02X | "
               "all: sum8=0x%02X xor8=0x%02X negsum8=0x%02X",
               reading.checksum, reading.unknown, reading.sequence_num, c.sum_body, c.xor_body, c.negsum_body,
               c.sum_all, c.xor_all, c.negsum_all);
      this->log_raw_ports_(reading);
    }
    return true;
  }

  uint8_t expected;
  switch (this->checksum_mode_) {
    case ChecksumMode::CHECKSUM_MODE_SUM8_BODY:
      expected = c.sum_body;
      break;
    case ChecksumMode::CHECKSUM_MODE_XOR8_BODY:
      expected = c.xor_body;
      break;
    case ChecksumMode::CHECKSUM_MODE_NEGSUM8_BODY:
      expected = c.negsum_body;
      break;
    case ChecksumMode::CHECKSUM_MODE_SUM8_ALL:
      expected = c.sum_all;
      break;
    case ChecksumMode::CHECKSUM_MODE_XOR8_ALL:
      expected = c.xor_all;
      break;
    case ChecksumMode::CHECKSUM_MODE_NEGSUM8_ALL:
      expected = c.negsum_all;
      break;
    default:
      return true;
  }
  if (expected != reading.checksum) {
    ESP_LOGW(TAG, "Checksum mismatch: packet 0x%02X, computed 0x%02X; discarding reading", reading.checksum, expected);
    return false;
  }
  return true;
}

void EmporiaVueComponent::bump_counter_(uint32_t &counter, sensor::Sensor *sensor) {
  counter++;
  if (sensor)
    sensor->publish_state(counter);
}

AcTiming EmporiaVueComponent::average_timing_(const SensorReading &sensor_reading) {
  // Raw counts resolve to ~0.18 Hz / ~1 degree per reading; averaging the counts
  // before converting recovers resolution. With frequency_samples: 1 this is a pass-through.
  size_t n = this->timing_history_.size();
  if (n <= 1) {
    return AcTiming{static_cast<float>(sensor_reading.frequency),
                    {static_cast<float>(sensor_reading.degrees[0]), static_cast<float>(sensor_reading.degrees[1])}};
  }

  this->timing_history_[this->timing_index_] =
      RawTiming{sensor_reading.frequency, {sensor_reading.degrees[0], sensor_reading.degrees[1]}};
  this->timing_index_ = (this->timing_index_ + 1) % n;
  if (this->timing_filled_ < n)
    this->timing_filled_++;

  uint32_t period_sum = 0, deg0_sum = 0, deg1_sum = 0;
  for (size_t i = 0; i < this->timing_filled_; i++) {
    period_sum += this->timing_history_[i].period;
    deg0_sum += this->timing_history_[i].degrees[0];
    deg1_sum += this->timing_history_[i].degrees[1];
  }
  float count = static_cast<float>(this->timing_filled_);
  return AcTiming{period_sum / count, {deg0_sum / count, deg1_sum / count}};
}

static inline bool is_mains_port(CTInputPort port) {
  return port == CTInputPort::A || port == CTInputPort::B || port == CTInputPort::C;
}

float PhaseConfig::get_calibrated_voltage(const SensorReading &sensor_reading) const {
  return sensor_reading.voltage[this->input_wire_] * this->calibration_;
}

void PhaseConfig::update_from_reading(const SensorReading &sensor_reading, const AcTiming &timing) {
  if (this->voltage_sensor_) {
    this->voltage_sensor_->publish_state(this->get_calibrated_voltage(sensor_reading));
  }

  // validation that these sensors are allowed on this phase is done in the codegen stage
  if (this->frequency_sensor_) {
    // see https://github.com/emporia-vue-local/esphome/pull/88 for constant explanation
    float frequency = timing.period > 0.0f ? EMPORIA_VUE_FREQUENCY_CONSTANT / timing.period : NAN;
    this->frequency_sensor_->publish_state(frequency);
  }
  if (this->input_wire_ != PhaseInputWire::BLACK) {
    // angle relative to the black wire; the raw count is in the same units as the period
    float raw_phase_angle = timing.degrees[((uint8_t) this->input_wire_) - 1];
    this->phase_angle_deg_ = timing.period > 0.0f ? raw_phase_angle * 360.0f / timing.period : NAN;
    if (this->phase_angle_sensor_) {
      this->phase_angle_sensor_->publish_state(this->phase_angle_deg_);
    }
  }
}

int32_t PhaseConfig::extract_power_for_phase(const ReadingPowerEntry &power_entry) const {
  switch (this->input_wire_) {
    case PhaseInputWire::BLACK:
      return power_entry.phase_black;
    case PhaseInputWire::RED:
      return power_entry.phase_red;
    case PhaseInputWire::BLUE:
      return power_entry.phase_blue;
    default:
      ESP_LOGE(TAG, "Unsupported phase input wire, this should never happen");
      return -1;
  }
}

float PhaseConfig::extract_calibrated_power(const ReadingPowerEntry &power_entry) const {
  return this->extract_power_for_phase(power_entry) * this->calibration_;
}

float VirtualPhaseConfig::get_calibrated_voltage(const SensorReading &sensor_reading) const {
  float va = this->phase_a_->get_calibrated_voltage(sensor_reading);
  float vb = this->phase_b_->get_calibrated_voltage(sensor_reading);
  float theta = this->get_angle_deg() * static_cast<float>(M_PI) / 180.0f;
  // |V_a - V_b| for two phasors separated by theta: 180° gives Va+Vb (240 V
  // split-phase), 120° gives sqrt(3)*V (208 V from a wye).
  float v2 = va * va + vb * vb - 2.0f * va * vb * std::cos(theta);
  return v2 > 0.0f ? std::sqrt(v2) : 0.0f;
}

float VirtualPhaseConfig::extract_calibrated_power(const ReadingPowerEntry &power_entry) const {
  // Line-to-line voltage is V_a - V_b, so P = Re((V_a - V_b) I*) = P_a - P_b for any
  // angle between the legs; at exactly 180 degrees this equals the old 'multiply: 2'.
  return this->phase_a_->extract_calibrated_power(power_entry) - this->phase_b_->extract_calibrated_power(power_entry);
}

void VirtualPhaseConfig::update_from_reading(const SensorReading &sensor_reading, const AcTiming & /*timing*/) {
  if (this->voltage_sensor_) {
    this->voltage_sensor_->publish_state(this->get_calibrated_voltage(sensor_reading));
  }
}

void CTClampConfig::update_from_reading(const SensorReading &sensor_reading) {
  uint16_t raw_current = sensor_reading.current[this->input_port_];
  if (raw_current == CT_DISCONNECTED_SENTINEL) {
    // The metering MCU reports 0xFFFF current on a port with no CT plugged in
    // (observed on the Vue 3). Publish NaN rather than a nonsense value.
    if (!this->warned_disconnected_) {
      ESP_LOGW(TAG, "CT port index %d reports no clamp connected (0xFFFF); publishing NaN", this->input_port_);
      this->warned_disconnected_ = true;
    }
    if (this->power_sensor_)
      this->power_sensor_->publish_state(NAN);
    if (this->current_sensor_)
      this->current_sensor_->publish_state(NAN);
    if (this->apparent_power_sensor_)
      this->apparent_power_sensor_->publish_state(NAN);
    if (this->reactive_power_sensor_)
      this->reactive_power_sensor_->publish_state(NAN);
    if (this->power_factor_sensor_)
      this->power_factor_sensor_->publish_state(NAN);
    return;
  }
  this->warned_disconnected_ = false;

  bool want_derived = this->apparent_power_sensor_ || this->reactive_power_sensor_ || this->power_factor_sensor_;

  float power = NAN;
  if (this->power_sensor_ || want_derived) {
    power = this->get_calibrated_power(sensor_reading.power[this->input_port_]);
    if (this->power_sensor_)
      this->power_sensor_->publish_state(power);
  }

  float current = NAN;
  if (this->current_sensor_ || want_derived) {
    current = this->get_calibrated_current(raw_current);
    if (this->current_sensor_)
      this->current_sensor_->publish_state(current);
  }

  if (want_derived) {
    // From real power P, RMS current I and RMS voltage V: S = V*I, Q = sqrt(S^2 - P^2), PF = P/S.
    float voltage = this->phase_->get_calibrated_voltage(sensor_reading);
    float apparent = voltage * current;
    if (this->apparent_power_sensor_)
      this->apparent_power_sensor_->publish_state(apparent);
    if (this->reactive_power_sensor_) {
      float q2 = apparent * apparent - power * power;
      this->reactive_power_sensor_->publish_state(q2 > 0.0f ? std::sqrt(q2) : 0.0f);
    }
    if (this->power_factor_sensor_) {
      // Below ~1 VA the ratio is noise; report NaN so filters/graphs ignore it.
      this->power_factor_sensor_->publish_state(apparent > 1.0f ? power / apparent : NAN);
    }
  }
}

float CTClampConfig::get_calibrated_power(const ReadingPowerEntry &power_entry) const {
  float correction_factor = is_mains_port(this->input_port_) ? 5.5f : 22.0f;
  return this->phase_->extract_calibrated_power(power_entry) * this->calibration_ / correction_factor;
}

float CTClampConfig::get_calibrated_current(uint16_t raw_current) const {
  float scalar = is_mains_port(this->input_port_) ? (775.0f / 42624.0f) : (775.0f / 170496.0f);
  return raw_current * scalar * this->calibration_;
}

}  // namespace emporia_vue
}  // namespace esphome
