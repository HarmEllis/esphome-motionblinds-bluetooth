import esphome.codegen as cg
from esphome.components import (
    esp32_ble,
    esp32_ble_client,
    esp32_ble_tracker,
)
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components import time as time_
from esphome.components.esp32_ble import BTLoggers
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_TIME_ID,
    CONF_TYPE,
)
import esphome.final_validate as fv

CODEOWNERS = ["@HarmEllis"]
DEPENDENCIES = ["esp32_ble_tracker", "time"]
AUTO_LOAD = ["esp32_ble_client"]
MULTI_CONF = True

# Bluedroid exposes GATT_MAX_APPS registrations and keeps two for its own GATT
# and GAP use, so six client applications is the hard ceiling. Each motor needs
# one. ESPHome's own slot accounting only warns below the generic IDF limit of
# nine, which is why this component enforces the real limit itself.
MAX_MOTORS = 6

motionblinds_ble_ns = cg.esphome_ns.namespace("motionblinds_ble")
MotionblindsBLEMotor = motionblinds_ble_ns.class_("MotionblindsBLEMotor", cg.Component)
# One connection per motor, owned by the component rather than configured by
# the user. Subclassing BLEClientBase (as bluetooth_proxy does) instead of
# reusing the ble_client component keeps a motor to a single YAML block.
MotionblindsBLEClient = motionblinds_ble_ns.class_(
    "MotionblindsBLEClient", esp32_ble_client.BLEClientBase
)

BlindType = motionblinds_ble_ns.enum("BlindType", is_class=True)
BLIND_TYPES = {
    "ROLLER": BlindType.ROLLER,
    "HONEYCOMB": BlindType.HONEYCOMB,
    "ROMAN": BlindType.ROMAN,
    "VENETIAN": BlindType.VENETIAN,
    "DOUBLE_ROLLER": BlindType.DOUBLE_ROLLER,
    "CURTAIN": BlindType.CURTAIN,
    "VERTICAL": BlindType.VERTICAL,
}

SpeedLevel = motionblinds_ble_ns.enum("SpeedLevel", is_class=True)
SPEED_LEVELS = {
    "LOW": SpeedLevel.LOW,
    "MEDIUM": SpeedLevel.MEDIUM,
    "HIGH": SpeedLevel.HIGH,
}

CONF_MAC_CODE = "mac_code"
CONF_MOTIONBLINDS_BLE_ID = "motionblinds_ble_id"
CONF_BLE_CLIENT_ID = "ble_client_id"
CONF_BLIND_TYPE = "blind_type"
CONF_INVERT = "invert"
CONF_WINDOW_MIN = "window_min"
CONF_WINDOW_MAX = "window_max"
CONF_DISCONNECT_DELAY = "disconnect_delay"
CONF_DISCOVERY_TIMEOUT = "discovery_timeout"
CONF_CONNECT_TIMEOUT = "connect_timeout"
CONF_HANDSHAKE_TIMEOUT = "handshake_timeout"
CONF_OPERATION_TIMEOUT = "operation_timeout"
CONF_STUCK_CONNECT_TIMEOUT = "stuck_connect_timeout"
CONF_RECOVER_BY_REBOOT = "recover_by_reboot"
CONF_RECOVER_AFTER = "recover_after"
CONF_DISCOVERY_ROUNDS = "discovery_rounds"
CONF_FAST_CONNECT = "fast_connect"

# Bumped when the stored layout changes, so an old blob is never found rather
# than found and misread.
PREFERENCE_VERSION = 1


def _preference_key(config) -> int:
    """A namespaced, versioned preference key for one motor.

    Derived from whichever identifier was given rather than from an entity id,
    because a motor is a plain component and has no entity name to hash.

    Hashed rather than used raw. The earlier version folded the address into an
    integer, which for a four-character ``mac_code`` is a small number like
    2650 -- and preference keys share one flat 32-bit namespace with every other
    component on the node, where small numbers are exactly what a collision
    looks like. A collision reads back either nothing or another component's
    bytes at this component's length.

    Switching a motor between ``mac_code`` and ``mac_address`` changes its
    identity and therefore its key, so its stored position is lost once. The
    full address is not knowable from the code at build time, so the two cannot
    be reconciled; refreshing the blind restores the position.
    """
    if mac_address := config.get(CONF_MAC_ADDRESS):
        identity = "mac:" + bytes(mac_address.parts).hex()
    else:
        identity = "code:" + config[CONF_MAC_CODE].upper()

    # FNV-1a, for no reason beyond being short and well spread.
    digest = 0x811C9DC5
    for byte in f"motionblinds_ble:{PREFERENCE_VERSION}:{identity}".encode():
        digest ^= byte
        digest = (digest * 0x01000193) & 0xFFFFFFFF
    return digest


def _validate_mac_code(value):
    """The four-character code the motor advertises, e.g. 0A5A.

    It is also the last two bytes of the address, which is what lets the
    component recognise the motor without being told its full address.
    """
    value = cv.string_strict(value).strip().upper()
    if len(value) != 4 or any(character not in "0123456789ABCDEF" for character in value):
        raise cv.Invalid(
            f"'{value}' is not a Motionblinds MAC code. Use the four hex characters "
            f"from the motor's name, for example 0A5A from MOTION_0A5A."
        )
    return value


def _validate_identifier(config):
    has_address = CONF_MAC_ADDRESS in config
    has_code = CONF_MAC_CODE in config
    if has_address and has_code:
        raise cv.Invalid(
            f"Give either '{CONF_MAC_ADDRESS}' or '{CONF_MAC_CODE}', not both",
            path=[CONF_MAC_CODE],
        )
    if not has_address and not has_code:
        raise cv.Invalid(
            f"A motor needs either '{CONF_MAC_ADDRESS}' or '{CONF_MAC_CODE}' "
            f"(the four characters from its MOTION_XXXX name)"
        )
    return config


def _validate_window(config):
    if config[CONF_WINDOW_MIN] >= config[CONF_WINDOW_MAX]:
        raise cv.Invalid(
            f"'{CONF_WINDOW_MIN}' must be smaller than '{CONF_WINDOW_MAX}'",
            path=[CONF_WINDOW_MIN],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MotionblindsBLEMotor),
            # Generated rather than written by the user: one BLE client per
            # motor is an implementation detail, and hiding it keeps six motors
            # from needing twelve blocks of YAML.
            cv.GenerateID(CONF_BLE_CLIENT_ID): cv.declare_id(MotionblindsBLEClient),
            # Either the full address, or the short code the motor advertises.
            cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_MAC_CODE): _validate_mac_code,
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_BLIND_TYPE, default="ROLLER"): cv.enum(
                BLIND_TYPES, upper=True, space="_"
            ),
            cv.Optional(CONF_INVERT, default=False): cv.boolean,
            cv.Optional(CONF_WINDOW_MIN, default=0): cv.percentage_int,
            cv.Optional(CONF_WINDOW_MAX, default=100): cv.percentage_int,
            cv.Optional(
                CONF_DISCONNECT_DELAY, default="15s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_DISCOVERY_TIMEOUT, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_CONNECT_TIMEOUT, default="20s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_HANDSHAKE_TIMEOUT, default="15s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_OPERATION_TIMEOUT, default="180s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_STUCK_CONNECT_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RECOVER_BY_REBOOT, default=False): cv.boolean,
            cv.Optional(
                CONF_RECOVER_AFTER, default="5min"
            ): cv.positive_time_period_milliseconds,
            # A single listening window is fragile for a motor that advertises
            # weakly; several bounded rounds are not the same thing as an
            # unbounded retry.
            cv.Optional(CONF_DISCOVERY_ROUNDS, default=3): cv.int_range(min=1, max=10),
            cv.Optional(CONF_FAST_CONNECT, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    _validate_identifier,
    _validate_window,
    esp32_ble.consume_connection_slots(1, "motionblinds_ble"),
)


def _final_validate(config):
    """Enforce the limits ESPHome itself only warns about.

    Exceeding the number of GATT client applications does not fail the build;
    it fails at runtime on the ESP, during app registration, long after anyone
    is watching. Catching it here turns a mystifying field failure into a
    config error.
    """
    full = fv.full_config.get()

    motors = full.get("motionblinds_ble", [])
    if len(motors) > MAX_MOTORS:
        raise cv.Invalid(
            f"At most {MAX_MOTORS} Motionblinds motors fit on one ESP32: Bluetooth's "
            f"GATT_MAX_APPS leaves six client registrations after its own two. "
            f"{len(motors)} are configured; move the rest to a second node."
        )

    # Any other component that opens BLE connections competes for the same
    # registrations, so a full house cannot share the node.
    if len(motors) == MAX_MOTORS:
        competitors = [
            name
            for name in ("bluetooth_proxy", "ble_client")
            if full.get(name)
        ]
        if competitors:
            raise cv.Invalid(
                f"{MAX_MOTORS} Motionblinds motors already use every available BLE "
                f"client registration, so this node cannot also run "
                f"{' and '.join(competitors)}. Use fewer motors or a separate node."
            )

    ble = full.get("esp32_ble", {}) or {}
    max_connections = ble.get("max_connections", 3)
    if max_connections < len(motors):
        raise cv.Invalid(
            f"{len(motors)} Motionblinds motors need 'max_connections: {len(motors)}' "
            f"under 'esp32_ble:'; it is currently {max_connections}. ESPHome adds the "
            f"advertising/scanning instance on top of this number itself."
        )

    # Connections are made from advertisements, so the scanner has to be
    # running for a motor to ever be reachable.
    tracker = full.get("esp32_ble_tracker", {}) or {}
    scan = tracker.get("scan_parameters", {}) or {}
    if scan.get("continuous") is False:
        raise cv.Invalid(
            "Motionblinds motors are reached by waiting for their advertisements, "
            "which requires 'continuous: true' under 'esp32_ble_tracker: "
            "scan_parameters:'."
        )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    esp32_ble.register_bt_logger(BTLoggers.GATT)

    # Keep the discovered GATT database in flash, the way bluetooth_proxy does.
    # Every connection otherwise re-walks the motor's whole attribute table
    # before a single command can be sent, and these are battery devices on a
    # slow connection interval — that walk is a large part of the delay between
    # pressing a button and the blind moving.
    add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", True)

    # The BLE client is created here, the way bluetooth_proxy creates its own
    # connections, so that the user only ever describes motors.
    client = cg.new_Pvariable(config[CONF_BLE_CLIENT_ID])
    await cg.register_component(client, config)
    await esp32_ble_tracker.register_client(client, config)
    if mac_address := config.get(CONF_MAC_ADDRESS):
        cg.add(client.set_address(mac_address.as_hex))
    else:
        # Left unset; the client adopts the address of the first advertisement
        # whose last two bytes match this code.
        cg.add(client.set_mac_code(int(config[CONF_MAC_CODE], 16)))
    # Auto-connect governs whether an advertisement may promote the client.
    # It stays on; the component gates reachability with set_enabled() instead,
    # so that connections are serialised by the tracker and the motor's address
    # type is learned from its advertisement.
    cg.add(client.set_auto_connect(True))
    # Both halves log under the motor's own id, so six motors stay legible.
    cg.add(client.set_label(str(config[CONF_ID])))

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # The pair know each other directly: the client forwards GATT events and
    # teardown to the motor, and the motor gates the client's reachability.
    cg.add(client.set_motor(var))
    cg.add(var.set_ble_client(client))

    cg.add(var.set_preference_key(_preference_key(config)))

    time_source = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(time_source))

    cg.add(var.set_label(str(config[CONF_ID])))
    cg.add(var.set_blind_type(config[CONF_BLIND_TYPE]))
    cg.add(var.set_discovery_rounds(config[CONF_DISCOVERY_ROUNDS]))
    cg.add(var.set_fast_connect(config[CONF_FAST_CONNECT]))
    cg.add(
        var.set_rail_range(
            config[CONF_WINDOW_MIN], config[CONF_WINDOW_MAX], config[CONF_INVERT]
        )
    )
    cg.add(var.set_disconnect_delay(config[CONF_DISCONNECT_DELAY]))
    cg.add(var.set_discovery_timeout(config[CONF_DISCOVERY_TIMEOUT]))
    cg.add(var.set_connect_timeout(config[CONF_CONNECT_TIMEOUT]))
    cg.add(var.set_handshake_timeout(config[CONF_HANDSHAKE_TIMEOUT]))
    cg.add(var.set_operation_timeout(config[CONF_OPERATION_TIMEOUT]))
    cg.add(var.set_stuck_connect_timeout(config[CONF_STUCK_CONNECT_TIMEOUT]))
    cg.add(
        var.set_recover_by_reboot(
            config[CONF_RECOVER_BY_REBOOT], config[CONF_RECOVER_AFTER]
        )
    )


MOTIONBLINDS_BLE_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_MOTIONBLINDS_BLE_ID): cv.use_id(MotionblindsBLEMotor),
    }
)
