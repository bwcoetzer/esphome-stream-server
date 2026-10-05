import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    STATE_CLASS_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from . import ns, StreamServerComponent

CONF_CONNECTION_COUNT = "connection_count"
CONF_SOCKETS_USED = "sockets_used"
CONF_SOCKETS_MAX = "sockets_max"
CONF_DROPPED_CLIENTS = "dropped_clients"
CONF_STREAM_SERVER = "stream_server"

def _diag_sensor():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_STREAM_SERVER): cv.use_id(StreamServerComponent),
        cv.Optional(CONF_CONNECTION_COUNT): _diag_sensor(),
        cv.Optional(CONF_SOCKETS_USED): _diag_sensor(),
        cv.Optional(CONF_SOCKETS_MAX): _diag_sensor(),
        cv.Optional(CONF_DROPPED_CLIENTS): _diag_sensor(),
    }
)


async def to_code(config):
    server = await cg.get_variable(config[CONF_STREAM_SERVER])

    for key, setter in (
        (CONF_CONNECTION_COUNT, "set_connection_count_sensor"),
        (CONF_SOCKETS_USED, "set_sockets_used_sensor"),
        (CONF_SOCKETS_MAX, "set_sockets_max_sensor"),
        (CONF_DROPPED_CLIENTS, "set_dropped_clients_sensor"),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(server, setter)(sens))
