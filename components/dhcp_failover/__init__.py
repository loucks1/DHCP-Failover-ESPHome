"""DHCP/DNS failover for Pi-hole (ESPHome external component)."""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, select, sensor, text_sensor
from esphome.components import esp32, time as time_
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_TIME_ID,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@loucks1"]
DEPENDENCIES = ["network", "time"]
AUTO_LOAD = ["json", "sensor", "binary_sensor", "text_sensor", "select", "button"]
MULTI_CONF = False

ns = cg.esphome_ns.namespace("dhcp_failover")
DhcpFailover = ns.class_("DhcpFailover", cg.Component)
OverrideSelect = ns.class_("OverrideSelect", select.Select)
SyncButton = ns.class_("SyncButton", button.Button)

CONF_PIHOLE_IP = "pihole_ip"
CONF_PIHOLE_PORT = "pihole_port"
CONF_PIHOLE_PASSWORD = "pihole_password"
CONF_ADDRESS = "address"
CONF_GATEWAY = "gateway"
CONF_NETMASK = "netmask"
CONF_POOL_START = "pool_start"
CONF_POOL_END = "pool_end"
CONF_LEASE_TIME = "lease_time"
CONF_DOMAIN = "domain"
CONF_UPSTREAM_DNS = "upstream_dns"
CONF_PROBE_INTERVAL = "probe_interval"  # deprecated alias: fixed interval (min = max)
CONF_PROBE_INTERVAL_MIN = "probe_interval_min"
CONF_PROBE_INTERVAL_MAX = "probe_interval_max"
CONF_FAIL_THRESHOLD = "fail_threshold"
CONF_RECOVER_THRESHOLD = "recover_threshold"
CONF_SYNC_TIME = "sync_time"
CONF_DNS_TTL = "dns_ttl"

CONF_MODE = "mode"
CONF_OVERRIDE = "override"
CONF_PIHOLE_DHCP_HEALTHY = "pihole_dhcp_healthy"
CONF_RESERVATION_COUNT = "reservation_count"
CONF_ACTIVE_LEASE_COUNT = "active_lease_count"
CONF_LAST_SYNC = "last_sync"
CONF_SYNC_STATUS = "sync_status"
CONF_SYNC_NOW = "sync_now"

OVERRIDE_OPTIONS = ["auto", "force_active", "force_passive"]


def _probe_interval(config):
    if CONF_PROBE_INTERVAL in config:
        if CONF_PROBE_INTERVAL_MIN in config or CONF_PROBE_INTERVAL_MAX in config:
            raise cv.Invalid("use either probe_interval (deprecated) or probe_interval_min/max, not both")
        _LOGGER.warning("dhcp_failover: 'probe_interval' is deprecated; use probe_interval_min/probe_interval_max")
        config[CONF_PROBE_INTERVAL_MIN] = config[CONF_PROBE_INTERVAL_MAX] = config.pop(CONF_PROBE_INTERVAL)
    config.setdefault(CONF_PROBE_INTERVAL_MIN, cv.positive_time_period_milliseconds("60s"))
    config.setdefault(CONF_PROBE_INTERVAL_MAX, cv.positive_time_period_milliseconds("120s"))
    if config[CONF_PROBE_INTERVAL_MAX].total_milliseconds < config[CONF_PROBE_INTERVAL_MIN].total_milliseconds:
        raise cv.Invalid("probe_interval_max must be >= probe_interval_min")
    return config


def _consume_sockets(config):
    from esphome.components import socket

    # DHCP server :67 (also used for probes), DNS server :53, probe/upstream DNS relay
    socket.consume_sockets(3, "dhcp_failover", socket.SocketType.UDP)(config)
    return config


def _hhmm(value):
    value = cv.string_strict(value)
    parts = value.split(":")
    if len(parts) != 2:
        raise cv.Invalid("sync_time must be HH:MM")
    h, m = int(parts[0]), int(parts[1])
    if not (0 <= h < 24 and 0 <= m < 60):
        raise cv.Invalid("sync_time out of range")
    return [h, m]


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DhcpFailover),
        cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_PIHOLE_IP, default="192.168.86.10"): cv.ipv4address,
        cv.Optional(CONF_PIHOLE_PORT, default=80): cv.port,
        cv.Required(CONF_PIHOLE_PASSWORD): cv.string,
        cv.Optional(CONF_ADDRESS): cv.ipv4address,  # this device's static IP; auto-detected if omitted
        cv.Optional(CONF_GATEWAY, default="192.168.86.1"): cv.ipv4address,
        cv.Optional(CONF_NETMASK, default="255.255.255.0"): cv.ipv4address,
        cv.Optional(CONF_POOL_START, default="192.168.86.210"): cv.ipv4address,
        cv.Optional(CONF_POOL_END, default="192.168.86.249"): cv.ipv4address,
        cv.Optional(CONF_LEASE_TIME, default="10min"): cv.positive_time_period_seconds,
        cv.Optional(CONF_DOMAIN, default="lan"): cv.All(cv.string_strict, cv.Length(min=1, max=63), lambda v: v.strip(".").lower()),
        cv.Optional(CONF_UPSTREAM_DNS, default=["1.1.1.1", "9.9.9.9"]): cv.All(
            cv.ensure_list(cv.ipv4address), cv.Length(min=1, max=2)
        ),
        cv.Optional(CONF_PROBE_INTERVAL): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PROBE_INTERVAL_MIN): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PROBE_INTERVAL_MAX): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FAIL_THRESHOLD, default=4): cv.int_range(min=1, max=100),
        cv.Optional(CONF_RECOVER_THRESHOLD, default=3): cv.int_range(min=1, max=100),
        cv.Optional(CONF_SYNC_TIME, default="03:30"): _hhmm,
        cv.Optional(CONF_DNS_TTL, default=60): cv.int_range(min=0, max=86400),
        # entities
        cv.Optional(CONF_MODE, default={"name": "Mode"}): text_sensor.text_sensor_schema(
            icon="mdi:swap-horizontal"
        ),
        cv.Optional(CONF_SYNC_STATUS, default={"name": "Sync Status"}): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:sync"
        ),
        cv.Optional(CONF_LAST_SYNC, default={"name": "Last Sync"}): text_sensor.text_sensor_schema(
            device_class="timestamp", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        cv.Optional(
            CONF_PIHOLE_DHCP_HEALTHY, default={"name": "Pi-hole DHCP Healthy"}
        ): binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_CONNECTIVITY),
        cv.Optional(CONF_RESERVATION_COUNT, default={"name": "Reservations"}): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:bookmark-multiple",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ACTIVE_LEASE_COUNT, default={"name": "Active Leases"}): sensor.sensor_schema(
            accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT, icon="mdi:lan-connect"
        ),
        cv.Optional(CONF_OVERRIDE, default={"name": "Override"}): select.select_schema(
            OverrideSelect, entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:hand-back-right"
        ),
        cv.Optional(CONF_SYNC_NOW, default={"name": "Sync Now"}): button.button_schema(
            SyncButton, entity_category=ENTITY_CATEGORY_CONFIG, icon="mdi:cloud-download"
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _probe_interval, _consume_sockets)


def _ip(v):
    a, b, c, d = (int(x) for x in str(v).split("."))
    return cg.RawExpression(f"0x{(a << 24) | (b << 16) | (c << 8) | d:08X}UL")


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))

    cg.add(var.set_pihole(_ip(config[CONF_PIHOLE_IP]), config[CONF_PIHOLE_PORT], config[CONF_PIHOLE_PASSWORD]))
    cg.add(var.set_network(_ip(config[CONF_ADDRESS]) if CONF_ADDRESS in config else 0, _ip(config[CONF_GATEWAY]), _ip(config[CONF_NETMASK])))
    cg.add(var.set_pool(_ip(config[CONF_POOL_START]), _ip(config[CONF_POOL_END])))
    cg.add(var.set_lease_secs(config[CONF_LEASE_TIME].total_seconds))
    cg.add(var.set_domain(config[CONF_DOMAIN]))
    ups = config[CONF_UPSTREAM_DNS]
    cg.add(var.set_upstream(_ip(ups[0]), _ip(ups[1] if len(ups) > 1 else ups[0])))
    cg.add(var.set_probe(config[CONF_PROBE_INTERVAL_MIN].total_milliseconds,
                         config[CONF_PROBE_INTERVAL_MAX].total_milliseconds, config[CONF_FAIL_THRESHOLD],
                         config[CONF_RECOVER_THRESHOLD]))
    cg.add(var.set_sync_time(config[CONF_SYNC_TIME][0], config[CONF_SYNC_TIME][1]))
    cg.add(var.set_dns_ttl(config[CONF_DNS_TTL]))

    cg.add(var.set_mode_sensor(await text_sensor.new_text_sensor(config[CONF_MODE])))
    cg.add(var.set_sync_status_sensor(await text_sensor.new_text_sensor(config[CONF_SYNC_STATUS])))
    cg.add(var.set_last_sync_sensor(await text_sensor.new_text_sensor(config[CONF_LAST_SYNC])))
    cg.add(var.set_healthy_sensor(await binary_sensor.new_binary_sensor(config[CONF_PIHOLE_DHCP_HEALTHY])))
    cg.add(var.set_reservation_sensor(await sensor.new_sensor(config[CONF_RESERVATION_COUNT])))
    cg.add(var.set_lease_sensor(await sensor.new_sensor(config[CONF_ACTIVE_LEASE_COUNT])))
    sel = await select.new_select(config[CONF_OVERRIDE], options=OVERRIDE_OPTIONS)
    cg.add(sel.set_parent(var))
    cg.add(var.set_override_select(sel))
    btn = await button.new_button(config[CONF_SYNC_NOW])
    cg.add(btn.set_parent(var))

    if CORE.is_esp32:
        esp32.include_builtin_idf_component("esp_http_client")
        esp32.include_builtin_idf_component("tcp_transport")
