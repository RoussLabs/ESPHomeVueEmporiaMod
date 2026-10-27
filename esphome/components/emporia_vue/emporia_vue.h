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

// Hz = constant / raw period count; empirically derived upstream, see PR #88.
#if defined(EMPORIA_VUE_VARIANT_VUE3)
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 19610.0f;
#else
static constexpr float EMPORIA_VUE_FREQUENCY_CONSTANT = 25310.0f;
#endif

// Period / phase-angle counts, optionally averaged over several readings.
struct AcTiming {
  float period;
  float degrees[2];
};

struct RawTiming {
  uint16_t period;
  uint16_t degrees[2];
};

enum class ChecksumMode : uint8_t {
  CHECKSUM_MODE_NONE = 0,
  CHECKSUM_MODE_DISCOVER,
  CHECKSUM_MODE_SUM8_BODY,
  CHECKSUM_MODE_XOR8_BODY,
  CHECKSUM_MODE_NEGSUM8_BODY,
  CHECKSUM_MODE_SUM8_ALL,
  CHECKSUM_MODE_XOR8_ALL,
  CHECKSUM_MODE_NEGSUM8_ALL,
};

class PhaseConfig;
class VirtualPhaseConfig;
class CTClampConfig;

class EmporiaVueComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void dump_config() override;

  float get_setup_priority() const override { return esphome::setup_priority::HARDWARE; }

  void set_phases(std::vector<PhaseConfig *> phases) { this->phases_ = std::move(phases); }
  void set_virtual_phases(std::vector<VirtualPhaseConfig *> v) { this->virtual_phases_ = std::move(v); }
  void set_ct_clamps(std::vector<CTClampConfig *> ct_clamps) { this->ct_clamps_ = std::move(ct_clamps); }
  void set_timing_samples(uint8_t samples) { this->timing_samples_ = samples; }
  void set_i2c_error_sensor(sensor::Sensor *s) { this->i2c_error_sensor_ = s; }
  void set_malformed_reading_sensor(sensor::Sensor *s) { this->malformed_reading_sensor_ = s; }
  void set_missed_reading_sensor(sensor::Sensor *s) { this->missed_reading_sensor_ = s; }
  void set_checksum_mode(ChecksumMode mode) { this->checksum_mode_ = mode; }

  void setup() override;
  void update() override;

  void add_on_update_callback(std::function<void()> &&callback);

 protected:
  AcTiming average_timing_(const SensorReading &sensor_reading);
  void bump_counter_(uint32_t &counter, sensor::Sensor *sensor);
  bool check_checksum_(const SensorReading &reading);
  void log_raw_ports_(const SensorReading &reading);

  ChecksumMode checksum_mode_{ChecksumMode::CHECKSUM_MODE_NONE};
  uint32_t discover_counter_{0};

  uint32_t i2c_errors_{0};
  uint32_t malformed_readings_{0};
  uint32_t missed_readings_{0};
  sensor::Sensor *i2c_error_sensor_{nullptr};
  sensor::Sensor *malformed_reading_sensor_{nullptr};
  sensor::Sensor *missed_reading_sensor_{nullptr};

  uint8_t last_sequence_num_ = 0;
  bool have_last_sequence_num_ = false;
  std::vector<PhaseConfig *> phases_;
  std::vector<VirtualPhaseConfig *> virtual_phases_;
  std::vector<CTClampConfig *> ct_clamps_;
  CallbackManager<void()> callback_;

  uint8_t timing_samples_{1};
  std::vector<RawTiming> timing_history_;  // sized once in setup(), never resized
  size_t timing_index_{0};
  size_t timing_filled_{0};
};

enum PhaseInputWire : uint8_t {
  BLACK = 0,
  RED = 1,
  BLUE = 2,
};

// Something a CT clamp can be referenced against: a physical leg (PhaseConfig)
// or a line-to-line combination of two legs (VirtualPhaseConfig).
class PhaseBase {
 public:
  virtual ~PhaseBase() = default;

  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { this->voltage_sensor_ = voltage_sensor; }
  sensor::Sensor *get_voltage_sensor() const { return this->voltage_sensor_; }

  virtual void update_from_reading(const SensorReading &sensor_reading, const AcTiming &timing) = 0;
  // RMS voltage of this phase in volts.
  virtual float get_calibrated_voltage(const SensorReading &sensor_reading) const = 0;
  // Real power of a CT entry referenced to this phase, scaled by the leg
  // calibration but not yet by the per-port correction factor or CT gain.
  virtual float extract_calibrated_power(const ReadingPowerEntry &power_entry) const = 0;

 protected:
  sensor::Sensor *voltage_sensor_{nullptr};
};

class PhaseConfig : public PhaseBase {
 public:
  void set_input_wire(PhaseInputWire input_wire) { this->input_wire_ = input_wire; }
  PhaseInputWire get_input_wire() const { return this->input_wire_; }
  void set_calibration(float calibration) { this->calibration_ = calibration; }
  float get_calibration() const { return this->calibration_; }
  void set_frequency_constant(float c) { this->frequency_constant_ = c; }
  float get_frequency_constant() const { return this->frequency_constant_; }
  void set_frequency_sensor(sensor::Sensor *frequency_sensor) { this->frequency_sensor_ = frequency_sensor; }
  sensor::Sensor *get_frequency_sensor() const { return this->frequency_sensor_; }
  void set_phase_angle_sensor(sensor::Sensor *phase_angle_sensor) { this->phase_angle_sensor_ = phase_angle_sensor; }
  sensor::Sensor *get_phase_angle_sensor() const { return this->phase_angle_sensor_; }

  void update_from_reading(const SensorReading &sensor_reading, const AcTiming &timing) override;
  float get_calibrated_voltage(const SensorReading &sensor_reading) const override;
  float extract_calibrated_power(const ReadingPowerEntry &power_entry) const override;

  int32_t extract_power_for_phase(const ReadingPowerEntry &power_entry) const;
  // Last computed angle relative to the BLACK wire (0 for BLACK itself).
  float get_phase_angle_deg() const { return this->phase_angle_deg_; }

 protected:
  PhaseInputWire input_wire_;
  float calibration_;
  float frequency_constant_{EMPORIA_VUE_FREQUENCY_CONSTANT};
  float phase_angle_deg_{0.0f};
  sensor::Sensor *frequency_sensor_{nullptr};
  sensor::Sensor *phase_angle_sensor_{nullptr};
};

// Line-to-line "phase" made of two physical legs, for North American 240 V
// split-phase or 208 V (two legs of a wye) circuits measured with one CT.
class VirtualPhaseConfig : public PhaseBase {
 public:
  void set_phases(PhaseConfig *a, PhaseConfig *b) {
    this->phase_a_ = a;
    this->phase_b_ = b;
  }
  const PhaseConfig *get_phase_a() const { return this->phase_a_; }
  const PhaseConfig *get_phase_b() const { return this->phase_b_; }
  // Configured angle between the legs: 180 for split-phase, 120 for two legs of a wye.
  void set_angle_deg(float angle) { this->angle_deg_ = angle; }
  float get_angle_deg() const { return this->angle_deg_; }

  void update_from_reading(const SensorReading &sensor_reading, const AcTiming &timing) override;
  // |V_a - V_b| by the law of cosines using the configured angle between the legs.
  float get_calibrated_voltage(const SensorReading &sensor_reading) const override;
  // P = Re(V_ab * I*) = P_a - P_b, exact for any angle between the legs.
  float extract_calibrated_power(const ReadingPowerEntry &power_entry) const override;

 protected:
  PhaseConfig *phase_a_{nullptr};
  PhaseConfig *phase_b_{nullptr};
  float angle_deg_{180.0f};
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
  void set_phase(PhaseBase *phase) { this->phase_ = phase; };
  const PhaseBase *get_phase() const { return this->phase_; }
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
  float get_calibrated_power(const ReadingPowerEntry &power_entry) const;
  float get_calibrated_current(uint16_t raw_current) const;

 protected:
  PhaseBase *phase_;
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
