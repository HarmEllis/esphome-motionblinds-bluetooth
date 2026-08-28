import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
)

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

CONF_SIGNAL_STRENGTH = "signal_strength"

MotionblindsBLESensor = motionblinds_ble_ns.class_(
    "MotionblindsBLESensor", cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MotionblindsBLESensor),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SIGNAL_STRENGTH): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(MOTIONBLINDS_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    motor = await cg.get_variable(config[CONF_MOTIONBLINDS_BLE_ID])
    cg.add(var.set_motor(motor))

    if battery := config.get(CONF_BATTERY_LEVEL):
        cg.add(var.set_battery_sensor(await sensor.new_sensor(battery)))
    if signal := config.get(CONF_SIGNAL_STRENGTH):
        cg.add(var.set_signal_sensor(await sensor.new_sensor(signal)))
