import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

CONF_CONNECTION_STATUS = "connection_status"

MotionblindsBLETextSensor = motionblinds_ble_ns.class_(
    "MotionblindsBLETextSensor", cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MotionblindsBLETextSensor),
            cv.Optional(CONF_CONNECTION_STATUS): text_sensor.text_sensor_schema(
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

    if status := config.get(CONF_CONNECTION_STATUS):
        cg.add(var.set_status_sensor(await text_sensor.new_text_sensor(status)))
