import esphome.codegen as cg
from esphome.components import binary_sensor, button, sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
)

from esphome.components.motionblinds_ble import (
    MotionblindsBLEMotor,
    motionblinds_ble_ns,  # noqa: F401
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]
# Loaded so that an optional `diagnostics:` block can build its entities. They
# cost a little flash even when unused, which is a fair trade for not having to
# hand-write twenty near-identical entity declarations per window.
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "button"]
MULTI_CONF = True

motionblinds_ble_tdbu_ns = cg.esphome_ns.namespace("motionblinds_ble_tdbu")
MotionblindsBLETdbu = motionblinds_ble_tdbu_ns.class_("MotionblindsBLETdbu", cg.Component)
MotionblindsBLETdbuDiagnostics = motionblinds_ble_tdbu_ns.class_(
    "MotionblindsBLETdbuDiagnostics", cg.Component
)
MotionblindsBLETdbuRefreshButton = motionblinds_ble_tdbu_ns.class_(
    "MotionblindsBLETdbuRefreshButton", button.Button, cg.Component
)

Fabric = motionblinds_ble_tdbu_ns.enum("Fabric", is_class=True)
FABRICS = {
    # One sheet strung between the rails: the distance between them is the
    # covered part of the window.
    "BETWEEN_RAILS": Fabric.BETWEEN_RAILS,
    # Two sheets closing in from the top and bottom: the distance between the
    # rails is the see-through gap.
    "OUTSIDE_IN": Fabric.OUTSIDE_IN,
}

Rail = motionblinds_ble_tdbu_ns.enum("Rail", is_class=True)
RAILS = {
    "TOP": Rail.TOP,
    "BOTTOM": Rail.BOTTOM,
}

CONF_TDBU_ID = "tdbu_id"
CONF_DIAGNOSTICS = "diagnostics"
CONF_SIGNAL_STRENGTH = "signal_strength"
CONF_CONNECTION_STATUS = "connection_status"
CONF_POSITION_FRESH = "position_fresh"
CONF_REFRESH = "refresh"
CONF_TOP_MOTOR = "top_motor"
CONF_BOTTOM_MOTOR = "bottom_motor"
CONF_FABRIC = "fabric"
CONF_MIN_GAP = "min_gap"
CONF_SAFETY_MARGIN = "safety_margin"
CONF_CLEARANCE_TIMEOUT = "clearance_timeout"
CONF_LEASE_TIMEOUT = "lease_timeout"


# One diagnostics set per rail. Written out per entity rather than generated
# in a loop so that every declared id is visible to ESPHome's id pass.
_RAILS = ("top", "bottom")


def _diagnostics_schema():
    schema = {
        cv.GenerateID(): cv.declare_id(MotionblindsBLETdbuDiagnostics),
        cv.Required(CONF_NAME): cv.string,
    }
    for rail in _RAILS:
        schema[cv.GenerateID(f"{rail}_{CONF_BATTERY_LEVEL}")] = cv.declare_id(sensor.Sensor)
        schema[cv.GenerateID(f"{rail}_{CONF_SIGNAL_STRENGTH}")] = cv.declare_id(sensor.Sensor)
        schema[cv.GenerateID(f"{rail}_{CONF_CONNECTION_STATUS}")] = cv.declare_id(
            text_sensor.TextSensor
        )
        schema[cv.GenerateID(f"{rail}_{CONF_POSITION_FRESH}")] = cv.declare_id(
            binary_sensor.BinarySensor
        )
        schema[cv.GenerateID(f"{rail}_{CONF_REFRESH}")] = cv.declare_id(
            MotionblindsBLETdbuRefreshButton
        )
    return cv.Schema(schema)


def _validate_motors(config):
    if config[CONF_TOP_MOTOR] == config[CONF_BOTTOM_MOTOR]:
        raise cv.Invalid(
            "A top-down bottom-up blind needs two different motors",
            path=[CONF_BOTTOM_MOTOR],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MotionblindsBLETdbu),
            cv.Required(CONF_TOP_MOTOR): cv.use_id(MotionblindsBLEMotor),
            cv.Required(CONF_BOTTOM_MOTOR): cv.use_id(MotionblindsBLEMotor),
            cv.Optional(CONF_FABRIC, default="BETWEEN_RAILS"): cv.enum(
                FABRICS, upper=True, space="_"
            ),
            # The rails cannot physically meet: stacked fabric takes up room.
            cv.Optional(CONF_MIN_GAP, default="5%"): cv.percentage,
            # Added on top of min_gap when computing targets, to absorb the
            # motors' whole-numbered feedback and their unspecified overshoot.
            cv.Optional(CONF_SAFETY_MARGIN, default="2%"): cv.percentage,
            cv.Optional(
                CONF_CLEARANCE_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_LEASE_TIMEOUT, default="180s"
            ): cv.positive_time_period_milliseconds,
            # Builds battery, signal, connection, freshness and a refresh
            # button for both rails, named after this prefix.
            cv.Optional(CONF_DIAGNOSTICS): _diagnostics_schema(),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_motors,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_top_motor(await cg.get_variable(config[CONF_TOP_MOTOR])))
    cg.add(var.set_bottom_motor(await cg.get_variable(config[CONF_BOTTOM_MOTOR])))
    cg.add(var.set_fabric(config[CONF_FABRIC]))
    cg.add(var.set_min_gap(config[CONF_MIN_GAP] * 100.0))
    cg.add(var.set_safety_margin(config[CONF_SAFETY_MARGIN] * 100.0))
    cg.add(var.set_clearance_timeout(config[CONF_CLEARANCE_TIMEOUT]))
    cg.add(var.set_lease_timeout(config[CONF_LEASE_TIMEOUT]))

    if diagnostics := config.get(CONF_DIAGNOSTICS):
        await _diagnostics_to_code(var, diagnostics)


async def _diagnostics_to_code(parent, config):
    prefix = config[CONF_NAME]
    diagnostics = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(diagnostics, {})
    cg.add(diagnostics.set_tdbu(parent))

    for rail in _RAILS:
        rail_enum = RAILS[rail.upper()]

        battery = await sensor.new_sensor(
            sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            )({CONF_NAME: f"{prefix} {rail} battery"})
            | {CONF_ID: config[f"{rail}_{CONF_BATTERY_LEVEL}"]}
        )
        cg.add(diagnostics.set_battery(rail_enum, battery))

        signal = await sensor.new_sensor(
            sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            )({CONF_NAME: f"{prefix} {rail} signal"})
            | {CONF_ID: config[f"{rail}_{CONF_SIGNAL_STRENGTH}"]}
        )
        cg.add(diagnostics.set_signal(rail_enum, signal))

        connection = await text_sensor.new_text_sensor(
            text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)(
                {CONF_NAME: f"{prefix} {rail} connection"}
            )
            | {CONF_ID: config[f"{rail}_{CONF_CONNECTION_STATUS}"]}
        )
        cg.add(diagnostics.set_connection(rail_enum, connection))

        fresh = await binary_sensor.new_binary_sensor(
            binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            )({CONF_NAME: f"{prefix} {rail} position fresh"})
            | {CONF_ID: config[f"{rail}_{CONF_POSITION_FRESH}"]}
        )
        cg.add(diagnostics.set_position_fresh(rail_enum, fresh))

        refresh_config = button.button_schema(
            MotionblindsBLETdbuRefreshButton, entity_category=ENTITY_CATEGORY_CONFIG
        )({CONF_NAME: f"{prefix} {rail} refresh"}) | {
            CONF_ID: config[f"{rail}_{CONF_REFRESH}"]
        }
        refresh = await button.new_button(refresh_config)
        await cg.register_component(refresh, refresh_config)
        cg.add(refresh.set_tdbu(parent))
        cg.add(refresh.set_rail(rail_enum))


TDBU_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TDBU_ID): cv.use_id(MotionblindsBLETdbu),
    }
)
