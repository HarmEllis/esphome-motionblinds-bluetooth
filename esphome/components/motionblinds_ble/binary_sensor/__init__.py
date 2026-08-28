import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

CONF_POSITION_FRESH = "position_fresh"
CONF_CALIBRATED = "calibrated"

MotionblindsBLEBinarySensor = motionblinds_ble_ns.class_(
    "MotionblindsBLEBinarySensor", cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MotionblindsBLEBinarySensor),
            # Whether the remembered position was actually observed during the
            # current connection. This is what the cover's assumed_state flag
            # cannot express, because the native API only sends that when
            # entities are listed.
            cv.Optional(CONF_POSITION_FRESH): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CALIBRATED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
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

    if fresh := config.get(CONF_POSITION_FRESH):
        cg.add(var.set_fresh_sensor(await binary_sensor.new_binary_sensor(fresh)))
    if calibrated := config.get(CONF_CALIBRATED):
        cg.add(
            var.set_calibration_sensor(await binary_sensor.new_binary_sensor(calibrated))
        )
