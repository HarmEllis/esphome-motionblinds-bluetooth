import esphome.codegen as cg
from esphome.components import cover
import esphome.config_validation as cv

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

MotionblindsBLECover = motionblinds_ble_ns.class_(
    "MotionblindsBLECover", cover.Cover, cg.Component
)

CONFIG_SCHEMA = (
    cover.cover_schema(MotionblindsBLECover, device_class="blind")
    .extend(MOTIONBLINDS_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    motor = await cg.get_variable(config[CONF_MOTIONBLINDS_BLE_ID])
    cg.add(var.set_motor(motor))
