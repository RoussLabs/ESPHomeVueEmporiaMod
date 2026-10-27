#pragma once

#ifdef USE_ESP32

#include <cmath>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace emporia_vue {

struct __attribute__((__packed__)) ReadingPowerEntry {
  int32_t phase_black;
  int32_t phase_red;
  int32_t phase_blue;
};

struct __attribute__((__packed__)) SensorReading {
  bool is_unread;
  uint8_t checksum;
  uint8_t unknown;
  uint8_t sequence_num;

  ReadingPowerEntry power[19];

  uint16_t voltage[3];
  uint16_t frequency;
  uint16_t degrees[2];

  uint16_t current[19];

  uint16_t end;
};

// Period / phase-angle counts, optionally averaged over several readings.
struct AcTiming {
  float period;
  float degrees[2];
};

struct RawTiming {
  uint16_t period;
  uint16_t degrees[2];
};

class PhaseConfig;
class CTClampConfig;

class EmporiaVueComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void dump_config() override;

  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE; }

  void set_phases(std::vector<PhaseConfig *> phases) { this->phases_ = std::move(phases); }
  void set_ct_clamps(std::vector<CTClampConfig *> ct_clamps) { this->ct_clamps_ = std::move(ct_clamps); }
  void set_timing_samples(uint8_t samples) { this->timing_samples_ = samples; }

  void update() override;

  void add_on_update_callback(std::function<void()> &&callback);

 protected:
  AcTiming average_timing_(const SensorReading &sensor_reading);

  uint8_t last_sequence_num_ = 0;
  bool have_last_sequence_num_ = false;
  std::vector<PhaseConfig *> phases_;
  std::vector<CTClampConfig *> ct_clamps_;
  CallbackManager<void()> callback_;

  uint8_t timing_samples_{1};
  std::vector<RawTiming> timing_history_;
  size_t timing_index_{0};
  size_t timing_filled_{0};
};

enum PhaseInputWire : uint8_t {
  BLACK = 0,
  RED = 1,
  BLUE = 2,
};

class PhaseConfig {
 public:
  void set_input_wire(PhaseInputWire input_wire) { this->input_wire_ = input_wire; }
  PhaseInputWire get_input_wire() const { return this->input_wire_; }
  void set_calibration(float calibration) { this->calibration_ = calibration; }
  float get_calibration() const { return this->calibration_; }
  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { this->voltage_sensor_ = voltage_sensor; }
  sensor::Sensor *get_voltage_sensor() const { return this->voltage_sensor_; }
  void set_frequency_sensor(sensor::Sensor *frequency_sensor) { this->frequency_sensor_ = frequency_sensor; }
  sensor::Sensor *get_frequency_sensor() const { return this->frequency_sensor_; }
  void set_phase_angle_sensor(sensor::Sensor *phase_angle_sensor) { this->phase_angle_sensor_ = phase_angle_sensor; }
  sensor::Sensor *get_phase_angle_sensor() const { return this->phase_angle_sensor_; }

  void update_from_reading(const SensorReading &sensor_reading, const AcTiming &timing);

  float get_calibrated_voltage(const SensorReading &sensor_reading) const;
  int32_t extract_power_for_phase(const ReadingPowerEntry &power_entry);
  // Last computed angle relative to the BLACK wire (NaN for BLACK itself).
  float get_phase_angle_deg() const { return this->phase_angle_deg_; }

 protected:
  PhaseInputWire input_wire_;
  float calibration_;
  float phase_angle_deg_{NAN};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *frequency_sensor_{nullptr};
  sensor::Sensor *phase_angle_sensor_{nullptr};
};

enum CTInputPort : uint8_t {
#if defined(EMPORIA_VUE_VARIANT_VUE3)
  A = 2,
  B = 1,
  C = 0,
#else
  A = 0,
  B = 1,
  C = 2,
#endif
  ONE = 3,
  TWO = 4,
  THREE = 5,
  FOUR = 6,
  FIVE = 7,
  SIX = 8,
  SEVEN = 9,
  EIGHT = 10,
  NINE = 11,
  TEN = 12,
  ELEVEN = 13,
  TWELVE = 14,
  THIRTEEN = 15,
  FOURTEEN = 16,
  FIFTEEN = 17,
  SIXTEEN = 18,
};

// Reported in the current field for a port with no CT connected.
static constexpr uint16_t CT_DISCONNECTED_SENTINEL = 0xFFFF;

class CTClampConfig : public sensor::Sensor {
 public:
  void set_phase(PhaseConfig *phase) { this->phase_ = phase; };
  const PhaseConfig *get_phase() const { return this->phase_; }
  void set_input_port(CTInputPort input_port) { this->input_port_ = input_port; };
  CTInputPort get_input_port() const { return this->input_port_; }
  // Per-clamp gain applied to both power and current (CT tolerance / turns ratio).
  void set_calibration(float calibration) { this->calibration_ = calibration; }
  float get_calibration() const { return this->calibration_; }
  void set_power_sensor(sensor::Sensor *power_sensor) { this->power_sensor_ = power_sensor; }
  sensor::Sensor *get_power_sensor() const { return this->power_sensor_; }
  void set_current_sensor(sensor::Sensor *current_sensor) { this->current_sensor_ = current_sensor; }
  sensor::Sensor *get_current_sensor() const { return this->current_sensor_; }
  void set_apparent_power_sensor(sensor::Sensor *s) { this->apparent_power_sensor_ = s; }
  sensor::Sensor *get_apparent_power_sensor() const { return this->apparent_power_sensor_; }
  void set_reactive_power_sensor(sensor::Sensor *s) { this->reactive_power_sensor_ = s; }
  sensor::Sensor *get_reactive_power_sensor() const { return this->reactive_power_sensor_; }
  void set_power_factor_sensor(sensor::Sensor *s) { this->power_factor_sensor_ = s; }
  sensor::Sensor *get_power_factor_sensor() const { return this->power_factor_sensor_; }

  void update_from_reading(const SensorReading &sensor_reading);
  float get_calibrated_power(int32_t raw_power) const;
  float get_calibrated_current(uint16_t raw_current) const;

 protected:
  PhaseConfig *phase_;
  CTInputPort input_port_;
  float calibration_{1.0f};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *apparent_power_sensor_{nullptr};
  sensor::Sensor *reactive_power_sensor_{nullptr};
  sensor::Sensor *power_factor_sensor_{nullptr};
  bool warned_disconnected_{false};
};

}  // namespace emporia_vue
}  // namespace esphome

#endif  // ifdef USE_ESP32
