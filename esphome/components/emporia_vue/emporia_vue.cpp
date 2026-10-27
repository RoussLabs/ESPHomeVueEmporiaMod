#include "emporia_vue.h"
#include <cmath>
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

  for (auto *phase : this->phases_) {
    std::string wire;
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
    ESP_LOGCONFIG(TAG, "    Wire: %s", wire.c_str());
    ESP_LOGCONFIG(TAG, "    Calibration: %f", phase->get_calibration());
    LOG_SENSOR("    ", "Voltage", phase->get_voltage_sensor());
  }

  for (auto *ct_clamp : this->ct_clamps_) {
    ESP_LOGCONFIG(TAG, "  CT Clamp");
    ESP_LOGCONFIG(TAG, "    Phase Calibration: %f", ct_clamp->get_phase()->get_calibration());
    ESP_LOGCONFIG(TAG, "    CT Port Index: %d", ct_clamp->get_input_port());
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
    return;
  }

  if (sensor_reading.end != 0) {
    ESP_LOGE(TAG, "Failed to read from sensor due to a malformed reading, should end in null bytes but is %d",
             sensor_reading.end);
    return;
  }

  if (!sensor_reading.is_unread) {
    ESP_LOGV(TAG, "Ignoring sensor reading that is marked as read");
    return;
  }

  ESP_LOGV(TAG, "Received sensor reading with sequence #%d", sensor_reading.sequence_num);

  if (this->have_last_sequence_num_) {
    // uint8_t arithmetic wraps correctly across 255 -> 0
    uint8_t gap = static_cast<uint8_t>(sensor_reading.sequence_num - this->last_sequence_num_ - 1);
    if (gap != 0) {
      ESP_LOGW(TAG, "Detected %u missing reading(s), data may not be accurate!", gap);
    }
  }
  this->have_last_sequence_num_ = true;

  AcTiming timing = this->average_timing_(sensor_reading);

  for (auto *phase : this->phases_) {
    phase->update_from_reading(sensor_reading, timing);
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

AcTiming EmporiaVueComponent::average_timing_(const SensorReading &sensor_reading) {
  // The period and phase-angle fields are integer counts with ~0.18 Hz / ~1 degree
  // resolution per reading. Averaging the raw counts over several readings before
  // converting recovers useful resolution; with frequency_samples: 1 this is a
  // pass-through of the current reading.
  size_t n = this->timing_samples_;
  if (n <= 1) {
    return AcTiming{static_cast<float>(sensor_reading.frequency),
                    {static_cast<float>(sensor_reading.degrees[0]), static_cast<float>(sensor_reading.degrees[1])}};
  }

  if (this->timing_history_.size() != n) {
    this->timing_history_.assign(n, RawTiming{0, {0, 0}});
    this->timing_index_ = 0;
    this->timing_filled_ = 0;
  }

  this->timing_history_[this->timing_index_] = RawTiming{sensor_reading.frequency,
                                                         {sensor_reading.degrees[0], sensor_reading.degrees[1]}};
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

int32_t PhaseConfig::extract_power_for_phase(const ReadingPowerEntry &power_entry) {
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
    ReadingPowerEntry power_entry = sensor_reading.power[this->input_port_];
    int32_t raw_power = this->phase_->extract_power_for_phase(power_entry);
    power = this->get_calibrated_power(raw_power);
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
    // The metering MCU gives real power (P) and true-RMS current (I); with the
    // phase's RMS voltage (V) we derive apparent power S = V*I, reactive power
    // Q = sqrt(S^2 - P^2) and true power factor PF = P/S.
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

float CTClampConfig::get_calibrated_power(int32_t raw_power) const {
  float calibration = this->phase_->get_calibration();

  float correction_factor = is_mains_port(this->input_port_) ? 5.5f : 22.0f;

  return (raw_power * calibration * this->calibration_) / correction_factor;
}

float CTClampConfig::get_calibrated_current(uint16_t raw_current) const {
  float scalar = is_mains_port(this->input_port_) ? (775.0f / 42624.0f) : (775.0f / 170496.0f);
  return raw_current * scalar * this->calibration_;
}

}  // namespace emporia_vue
}  // namespace esphome
