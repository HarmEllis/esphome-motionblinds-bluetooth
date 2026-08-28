import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

from esphome.components.motionblinds_ble import (
    MotionblindsBLEMotor,
    motionblinds_ble_ns,  # noqa: F401
)

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["motionblinds_ble"]
MULTI_CONF = True

motionblinds_ble_tdbu_ns = cg.esphome_ns.namespace("motionblinds_ble_tdbu")
MotionblindsBLETdbu = motionblinds_ble_tdbu_ns.class_("MotionblindsBLETdbu", cg.Component)

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
CONF_TOP_MOTOR = "top_motor"
CONF_BOTTOM_MOTOR = "bottom_motor"
CONF_FABRIC = "fabric"
CONF_MIN_GAP = "min_gap"
CONF_SAFETY_MARGIN = "safety_margin"
CONF_CLEARANCE_TIMEOUT = "clearance_timeout"
CONF_LEASE_TIMEOUT = "lease_timeout"


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


TDBU_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TDBU_ID): cv.use_id(MotionblindsBLETdbu),
    }
)
