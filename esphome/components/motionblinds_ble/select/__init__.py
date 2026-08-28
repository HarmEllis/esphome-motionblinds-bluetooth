import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SPEED, ENTITY_CATEGORY_CONFIG

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

SPEED_OPTIONS = ["low", "medium", "high"]

MotionblindsBLESpeedSelect = motionblinds_ble_ns.class_(
    "MotionblindsBLESpeedSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(cg.Component),
            cv.Optional(CONF_SPEED): select.select_schema(
                MotionblindsBLESpeedSelect,
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:speedometer",
            ),
        }
    )
    .extend(MOTIONBLINDS_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    motor = await cg.get_variable(config[CONF_MOTIONBLINDS_BLE_ID])
    if speed := config.get(CONF_SPEED):
        var = await select.new_select(speed, options=SPEED_OPTIONS)
        await cg.register_component(var, speed)
        cg.add(var.set_motor(motor))
