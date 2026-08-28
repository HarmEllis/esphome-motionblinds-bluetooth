import esphome.codegen as cg
from esphome.components import cover
import esphome.config_validation as cv

from .. import (
    CONF_TDBU_ID,
    TDBU_DEVICE_SCHEMA,
    motionblinds_ble_tdbu_ns,
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble_tdbu"]

CONF_RAIL = "rail"

MotionblindsBLETdbuCover = motionblinds_ble_tdbu_ns.class_(
    "MotionblindsBLETdbuCover", cover.Cover, cg.Component
)

CoverRail = motionblinds_ble_tdbu_ns.enum("CoverRail", is_class=True)
COVER_RAILS = {
    "TOP": CoverRail.TOP,
    "BOTTOM": CoverRail.BOTTOM,
    "COMBINED": CoverRail.COMBINED,
}

CONFIG_SCHEMA = (
    cover.cover_schema(MotionblindsBLETdbuCover, device_class="blind")
    .extend(
        {
            cv.Required(CONF_RAIL): cv.enum(COVER_RAILS, upper=True),
        }
    )
    .extend(TDBU_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    cg.add(var.set_tdbu(await cg.get_variable(config[CONF_TDBU_ID])))
    cg.add(var.set_rail(config[CONF_RAIL]))
