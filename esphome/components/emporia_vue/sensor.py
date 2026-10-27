from esphome.components import sensor, i2c
from esphome import automation
import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.const import (
    CONF_CALIBRATION,
    CONF_CURRENT,
    CONF_ID,
    CONF_INPUT,
    CONF_POWER,
    CONF_TRIGGER_ID,
    CONF_VOLTAGE,
    CONF_FREQUENCY,
    CONF_PHASE_ANGLE,
    CONF_APPARENT_POWER,
    CONF_POWER_FACTOR,
    CONF_REACTIVE_POWER,
    DEVICE_CLASS_APPARENT_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_VOLT_AMPS,
    UNIT_VOLT_AMPS_REACTIVE,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_HERTZ,
    UNIT_DEGREES,
)

CONF_CT_CLAMPS = "ct_clamps"
CONF_PHASES = "phases"
CONF_VIRTUAL_PHASES = "virtual_phases"
CONF_COMBINE = "combine"
CONF_ANGLE = "angle"
CONF_PHASE_ID = "phase_id"
CONF_VARIANT = "variant"
CONF_FREQUENCY_SAMPLES = "frequency_samples"
CONF_I2C_ERRORS = "i2c_errors"
CONF_MALFORMED_READINGS = "malformed_readings"
CONF_MISSED_READINGS = "missed_readings"
CONF_CHECKSUM = "checksum"

CONF_ON_UPDATE = "on_update"

DIAGNOSTIC_COUNTER_SCHEMA = sensor.sensor_schema(
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    accuracy_decimals=0,
)

CODEOWNERS = ["@flaviut", "@Maelstrom96", "@krconv"]
ESP_PLATFORMS = ["esp-idf"]
DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]

emporia_vue_ns = cg.esphome_ns.namespace("emporia_vue")
EmporiaVueComponent = emporia_vue_ns.class_(
    "EmporiaVueComponent", cg.PollingComponent, i2c.I2CDevice
)
PhaseBase = emporia_vue_ns.class_("PhaseBase")
PhaseConfig = emporia_vue_ns.class_("PhaseConfig", PhaseBase)
VirtualPhaseConfig = emporia_vue_ns.class_("VirtualPhaseConfig", PhaseBase)
CTClampConfig = emporia_vue_ns.class_("CTClampConfig")

# Trigger for after statistics sensors are updated
EmporiaVueUpdateTrigger = emporia_vue_ns.class_(
    "EmporiaVueUpdateTrigger", automation.Trigger.template()
)


ChecksumMode = emporia_vue_ns.enum("ChecksumMode", is_class=True)
CHECKSUM_MODES = {
    "none": ChecksumMode.CHECKSUM_MODE_NONE,
    # log candidate checksums for the first packets so the algorithm can be found
    "discover": ChecksumMode.CHECKSUM_MODE_DISCOVER,
    "sum8_body": ChecksumMode.CHECKSUM_MODE_SUM8_BODY,
    "xor8_body": ChecksumMode.CHECKSUM_MODE_XOR8_BODY,
    "negsum8_body": ChecksumMode.CHECKSUM_MODE_NEGSUM8_BODY,
    "sum8_all": ChecksumMode.CHECKSUM_MODE_SUM8_ALL,
    "xor8_all": ChecksumMode.CHECKSUM_MODE_XOR8_ALL,
    "negsum8_all": ChecksumMode.CHECKSUM_MODE_NEGSUM8_ALL,
}

PhaseInputWire = emporia_vue_ns.enum("PhaseInputWire")
PHASE_INPUT = {
    "BLACK": PhaseInputWire.BLACK,
    "RED": PhaseInputWire.RED,
    "BLUE": PhaseInputWire.BLUE,
}

CTInputPort = emporia_vue_ns.enum("CTInputPort")
CT_INPUT = {
    "A": CTInputPort.A,
    "B": CTInputPort.B,
    "C": CTInputPort.C,
    "1": CTInputPort.ONE,
    "2": CTInputPort.TWO,
    "3": CTInputPort.THREE,
    "4": CTInputPort.FOUR,
    "5": CTInputPort.FIVE,
    "6": CTInputPort.SIX,
    "7": CTInputPort.SEVEN,
    "8": CTInputPort.EIGHT,
    "9": CTInputPort.NINE,
    "10": CTInputPort.TEN,
    "11": CTInputPort.ELEVEN,
    "12": CTInputPort.TWELVE,
    "13": CTInputPort.THIRTEEN,
    "14": CTInputPort.FOURTEEN,
    "15": CTInputPort.FIFTEEN,
    "16": CTInputPort.SIXTEEN,
}

SCHEMA_CT_CLAMP = {
    cv.GenerateID(): cv.declare_id(CTClampConfig),
    # a physical leg from `phases` or a line-to-line entry from `virtual_phases`
    cv.Required(CONF_PHASE_ID): cv.use_id(PhaseBase),
    cv.Required(CONF_INPUT): cv.enum(CT_INPUT),
    # Per-clamp gain applied to both power and current. Use it to trim an
    # individual CT against a reference meter; 1.0 leaves the reading unchanged.
    cv.Optional(CONF_CALIBRATION, default=1.0): cv.positive_float,
    cv.Optional(CONF_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_CURRENT): sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    ),
    # Derived from real power, RMS current and the phase's RMS voltage.
    cv.Optional(CONF_APPARENT_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT_AMPS,
        device_class=DEVICE_CLASS_APPARENT_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_REACTIVE_POWER): sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT_AMPS_REACTIVE,
        device_class=DEVICE_CLASS_REACTIVE_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
    cv.Optional(CONF_POWER_FACTOR): sensor.sensor_schema(
        device_class=DEVICE_CLASS_POWER_FACTOR,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=2,
    ),
}


def validate_phases(val):
    base_validated = cv.Schema(
        cv.ensure_list(
            {
                cv.Required(CONF_ID): cv.declare_id(PhaseConfig),
                cv.Required(CONF_INPUT): cv.enum(PHASE_INPUT),
                cv.Optional(CONF_CALIBRATION, default=0.022): cv.zero_to_one_float,
                cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_VOLT,
                    device_class=DEVICE_CLASS_VOLTAGE,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_FREQUENCY): sensor.sensor_schema(
                    unit_of_measurement=UNIT_HERTZ,
                    device_class=DEVICE_CLASS_FREQUENCY,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=1,
                ),
                cv.Optional(CONF_PHASE_ANGLE): sensor.sensor_schema(
                    unit_of_measurement=UNIT_DEGREES,
                    state_class=STATE_CLASS_MEASUREMENT,
                    accuracy_decimals=0,
                ),
            }
        )
    )(val)

    if len(base_validated) > 3:
        raise cv.Invalid("No more than 3 phases are supported")

    inputs = [phase[CONF_INPUT] for phase in base_validated]
    if len(inputs) != len(set(inputs)):
        raise cv.Invalid("Only one entry per input color is allowed")

    for i, phase in enumerate(base_validated):
        input_wire = phase[CONF_INPUT]
        if input_wire == "BLACK":
            if CONF_PHASE_ANGLE in phase:
                raise cv.Invalid(
                    "Phase angle is not supported for the black wire, only for the "
                    "red and blue wires",
                    path=[i, CONF_PHASE_ANGLE],
                )
        elif input_wire in {"RED", "BLUE"}:
            if CONF_FREQUENCY in phase:
                raise cv.Invalid(
                    "Frequency is not supported for the red and blue wires, only for "
                    "the black wire",
                    path=[i, CONF_FREQUENCY],
                )

    return base_validated


def validate_combine(val: list) -> list:
    val = cv.ensure_list(cv.use_id(PhaseConfig))(val)
    if len(val) != 2:
        raise cv.Invalid("combine must list exactly two phase ids")
    if val[0].id == val[1].id:
        raise cv.Invalid("combine must reference two different phases")
    return val


SCHEMA_VIRTUAL_PHASE = {
    cv.GenerateID(): cv.declare_id(VirtualPhaseConfig),
    # two ids from `phases`; a CT referencing this entry is treated as a
    # line-to-line load between them (NA 240 V split-phase, or 208 V)
    cv.Required(CONF_COMBINE): validate_combine,
    # electrical angle between the two legs: 180 for NA split-phase (240 V),
    # 120 for two legs of a three-phase wye (208 V). The MCU's measured angle
    # is not used here; it reads ~45 deg for in-phase legs on tested hardware.
    cv.Optional(CONF_ANGLE, default=180.0): cv.float_range(min=0.0, max=360.0),
    cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
    ),
}


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmporiaVueComponent),
            cv.Required(CONF_PHASES): validate_phases,
            cv.Optional(CONF_VIRTUAL_PHASES, default=[]): cv.ensure_list(SCHEMA_VIRTUAL_PHASE),
            cv.Required(CONF_CT_CLAMPS): cv.ensure_list(SCHEMA_CT_CLAMP),
            cv.Optional(CONF_VARIANT, default="vue2"): cv.one_of("vue2", "vue3", lower=True),
            # Average the raw period / phase-angle counts over this many readings
            # (each ~240 ms) before converting to Hz / degrees. 1 = no averaging.
            cv.Optional(CONF_FREQUENCY_SAMPLES, default=1): cv.int_range(min=1, max=250),
            # Packet checksum handling; the MCU's algorithm is not yet confirmed.
            cv.Optional(CONF_CHECKSUM, default="none"): cv.enum(CHECKSUM_MODES, lower=True),
            # Diagnostic counters (since boot)
            cv.Optional(CONF_I2C_ERRORS): DIAGNOSTIC_COUNTER_SCHEMA,
            cv.Optional(CONF_MALFORMED_READINGS): DIAGNOSTIC_COUNTER_SCHEMA,
            cv.Optional(CONF_MISSED_READINGS): DIAGNOSTIC_COUNTER_SCHEMA,
            cv.Optional(CONF_ON_UPDATE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        EmporiaVueUpdateTrigger
                    ),
                }
            ),
        },
    )
    .extend(cv.polling_component_schema("200ms"))
    .extend(i2c.i2c_device_schema(0x64)),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if config.get(CONF_VARIANT, "vue2") == "vue3":
        cg.add_define("EMPORIA_VUE_VARIANT_VUE3")
    else:
        cg.add_define("EMPORIA_VUE_VARIANT_VUE2")

    cg.add(var.set_timing_samples(config[CONF_FREQUENCY_SAMPLES]))
    cg.add(var.set_checksum_mode(config[CONF_CHECKSUM]))

    if CONF_I2C_ERRORS in config:
        s = await sensor.new_sensor(config[CONF_I2C_ERRORS])
        cg.add(var.set_i2c_error_sensor(s))
    if CONF_MALFORMED_READINGS in config:
        s = await sensor.new_sensor(config[CONF_MALFORMED_READINGS])
        cg.add(var.set_malformed_reading_sensor(s))
    if CONF_MISSED_READINGS in config:
        s = await sensor.new_sensor(config[CONF_MISSED_READINGS])
        cg.add(var.set_missed_reading_sensor(s))

    phases = []
    for phase_config in config[CONF_PHASES]:
        phase_var = cg.new_Pvariable(phase_config[CONF_ID], PhaseConfig())
        cg.add(phase_var.set_input_wire(phase_config[CONF_INPUT]))
        cg.add(phase_var.set_calibration(phase_config[CONF_CALIBRATION]))

        if CONF_VOLTAGE in phase_config:
            voltage_sensor = await sensor.new_sensor(phase_config[CONF_VOLTAGE])
            cg.add(phase_var.set_voltage_sensor(voltage_sensor))

        if CONF_FREQUENCY in phase_config:
            frequency_sensor = await sensor.new_sensor(phase_config[CONF_FREQUENCY])
            cg.add(phase_var.set_frequency_sensor(frequency_sensor))

        if CONF_PHASE_ANGLE in phase_config:
            phase_angle_sensor = await sensor.new_sensor(phase_config[CONF_PHASE_ANGLE])
            cg.add(phase_var.set_phase_angle_sensor(phase_angle_sensor))

        phases.append(phase_var)
    cg.add(var.set_phases(phases))

    virtual_phases = []
    for vp_config in config[CONF_VIRTUAL_PHASES]:
        vp_var = cg.new_Pvariable(vp_config[CONF_ID], VirtualPhaseConfig())
        phase_a = await cg.get_variable(vp_config[CONF_COMBINE][0])
        phase_b = await cg.get_variable(vp_config[CONF_COMBINE][1])
        cg.add(vp_var.set_phases(phase_a, phase_b))
        cg.add(vp_var.set_angle_deg(vp_config[CONF_ANGLE]))
        if CONF_VOLTAGE in vp_config:
            voltage_sensor = await sensor.new_sensor(vp_config[CONF_VOLTAGE])
            cg.add(vp_var.set_voltage_sensor(voltage_sensor))
        virtual_phases.append(vp_var)
    cg.add(var.set_virtual_phases(virtual_phases))

    ct_clamps = []
    for ct_config in config[CONF_CT_CLAMPS]:
        ct_clamp_var = cg.new_Pvariable(ct_config[CONF_ID], CTClampConfig())
        phase_var = await cg.get_variable(ct_config[CONF_PHASE_ID])
        cg.add(ct_clamp_var.set_phase(phase_var))
        cg.add(ct_clamp_var.set_input_port(ct_config[CONF_INPUT]))
        cg.add(ct_clamp_var.set_calibration(ct_config[CONF_CALIBRATION]))

        if CONF_POWER in ct_config:
            power_sensor = await sensor.new_sensor(ct_config[CONF_POWER])
            cg.add(ct_clamp_var.set_power_sensor(power_sensor))

        if CONF_CURRENT in ct_config:
            current_sensor = await sensor.new_sensor(ct_config[CONF_CURRENT])
            cg.add(ct_clamp_var.set_current_sensor(current_sensor))

        if CONF_APPARENT_POWER in ct_config:
            s = await sensor.new_sensor(ct_config[CONF_APPARENT_POWER])
            cg.add(ct_clamp_var.set_apparent_power_sensor(s))

        if CONF_REACTIVE_POWER in ct_config:
            s = await sensor.new_sensor(ct_config[CONF_REACTIVE_POWER])
            cg.add(ct_clamp_var.set_reactive_power_sensor(s))

        if CONF_POWER_FACTOR in ct_config:
            s = await sensor.new_sensor(ct_config[CONF_POWER_FACTOR])
            cg.add(ct_clamp_var.set_power_factor_sensor(s))

        ct_clamps.append(ct_clamp_var)
    cg.add(var.set_ct_clamps(ct_clamps))

    for trigger_conf in config.get(CONF_ON_UPDATE, []):
        trigger = cg.new_Pvariable(trigger_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], trigger_conf )
