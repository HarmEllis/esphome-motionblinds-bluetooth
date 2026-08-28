import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import (
    CONF_MOTIONBLINDS_BLE_ID,
    MOTIONBLINDS_BLE_DEVICE_SCHEMA,
    motionblinds_ble_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]

CONF_ACTION = "action"

MotionblindsBLEButton = motionblinds_ble_ns.class_(
    "MotionblindsBLEButton", button.Button, cg.Component
)

ButtonAction = motionblinds_ble_ns.enum("ButtonAction", is_class=True)
ACTIONS = {
    # Connects, asks the motor for a status frame and refreshes position,
    # battery, speed and calibration. This is the counterpart of the
    # update_entity call that is the only thing known to wake a motor which is
    # connected but silently ignoring commands.
    "STATUS_QUERY": ButtonAction.STATUS_QUERY,
    "FAVORITE": ButtonAction.FAVORITE,
    "CONNECT": ButtonAction.CONNECT,
    "DISCONNECT": ButtonAction.DISCONNECT,
}

CONFIG_SCHEMA = (
    button.button_schema(MotionblindsBLEButton, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(
        {
            cv.Required(CONF_ACTION): cv.enum(ACTIONS, upper=True, space="_"),
        }
    )
    .extend(MOTIONBLINDS_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    motor = await cg.get_variable(config[CONF_MOTIONBLINDS_BLE_ID])
    cg.add(var.set_motor(motor))
    cg.add(var.set_action(config[CONF_ACTION]))
