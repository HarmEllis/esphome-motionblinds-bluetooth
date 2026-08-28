import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_ID, UNIT_PERCENT

from .. import (
    CONF_FABRIC,
    CONF_TDBU_ID,
    TDBU_DEVICE_SCHEMA,
    motionblinds_ble_tdbu_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble_tdbu"]

CONF_FABRIC_POSITION = "fabric_position"

MotionblindsBLETdbuNumber = motionblinds_ble_tdbu_ns.class_(
    "MotionblindsBLETdbuNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(cg.Component),
            cv.Required(CONF_FABRIC_POSITION): number.number_schema(
                MotionblindsBLETdbuNumber,
                unit_of_measurement=UNIT_PERCENT,
                icon="mdi:arrow-up-down",
            ),
        }
    )
    .extend(TDBU_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    """Only 'between_rails' has a fabric block that can be slid."""
    import esphome.final_validate as fv

    full = fv.full_config.get()
    target = str(config[CONF_TDBU_ID])
    for blind in full.get("motionblinds_ble_tdbu", []):
        if str(blind[CONF_ID]) == target and str(blind[CONF_FABRIC]) == "OUTSIDE_IN":
            raise cv.Invalid(
                "'fabric_position' slides a single sheet of fabric between the "
                "rails, which an 'outside_in' blind does not have. Use the "
                "combined cover to change how far it is open instead."
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await number.new_number(
        config[CONF_FABRIC_POSITION], min_value=0.0, max_value=100.0, step=1.0
    )
    await cg.register_component(var, config[CONF_FABRIC_POSITION])
    cg.add(var.set_tdbu(await cg.get_variable(config[CONF_TDBU_ID])))
