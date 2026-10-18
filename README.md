# DHCP-Failover-ESPHome

An ESPHome external component that turns an ESP32 into a **standby DHCP + DNS server for Pi-hole**.
It stays silent while Pi-hole is healthy and takes over only when Pi-hole stops answering its health check
(a DNS lookup by default), then goes quiet again when Pi-hole comes back. It's managed from Home Assistant like any other ESPHome device.

Framework: **ESP-IDF** (lwIP sockets + `esp_http_client`). It works on any ESP32 variant ESPHome supports,
over `ethernet:` (RMII or SPI such as W5500) or `wifi:`.

## How it works

* **Passive (default).** Nothing listens on UDP 67 or 53. Every `probe_interval_min`–`probe_interval_max` (a random
  60–120s, picked fresh with `esp_random()` for each probe), the device checks Pi-hole's health:
  * **`probe_mode: dns` (default).** It sends one `A` query for `probe_dns_name` (`proxmox1.lan`) **straight to
    `probe_dns_server`:53** (defaults to `pihole_ip`) from its own UDP socket. It never uses the board's resolver and
    never asks itself (it serves DNS when active). A probe is healthy only if a reply comes back from that server
    within `probe_dns_timeout` (2s) with the same transaction ID and question, RCODE `NOERROR` and at least one answer.
    A DNS check can't be refused by DHCP rate limits or cooldowns. It does mean a Pi-hole whose DNS works but whose
    DHCP is broken counts as healthy.
  * **`probe_mode: dhcp`.** It broadcasts a DHCPDISCOVER from a fake, locally administered MAC (`02:00:5E:D1:0C:F0`).
    Only an OFFER whose server-identifier is `pihole_ip` (within 5s) counts as healthy.
* **Retry.** After a failed probe, the next one goes out `probe_retry_interval` (5s) later instead of waiting the
  random interval, so an outage is confirmed quickly and a single lost packet doesn't cause a failover.
* **Failover.** After `fail_threshold` failed probes in a row (with `fail_threshold: 2`, about 7s in DNS mode once the
  first probe fails), the device goes **active**:
  * **DHCP on :67.** A client with a Pi-hole reservation gets that IP. A client with a known Pi-hole lease, or a lease from
    this device, keeps its IP. Anyone else gets an address from `pool_start`–`pool_end` (.210–.249). Leases are short
    (`lease_time`, 10 min), and the device hands itself out as DNS.
  * **DNS on :53.** It answers local names (`host`, `host.lan`) and reverse lookups for the subnet, using Pi-hole
    local DNS records, reservation names and lease hostnames. Unknown local names get NXDOMAIN and are never forwarded.
    Everything else is forwarded to `upstream_dns` (1.1.1.1, then 9.9.9.9). There is no blocklist.
* **Recovery.** While active it keeps probing Pi-hole every `probe_retry_interval`. After `recover_threshold` good
  probes in a row (with `recover_threshold: 2`, about 5–10s after Pi-hole answers again), it stops both servers and goes
  passive again.
* **Logs.** Each probe logs its mode, result (`rcode=0 answers=1`, `timeout`, …), latency and the fail/ok count.
  Failures log at WARN; state changes and the first good probe after a failure log at INFO; routine good probes at DEBUG.
* **Sync.** It pulls reservations, leases and local DNS records from the Pi-hole v6 API at boot, nightly at `sync_time`
  (03:30 local), and on demand (the **Sync Now** button). It skips sync while active, because Pi-hole is down. The
  snapshot is stored in NVS, so a device that reboots during an outage still knows the reservations. Reservations
  without both a MAC and an IP are skipped.

### Limitations

* **Clients move over gradually.** A client that already holds a Pi-hole lease only reaches this device when it
  rebinds (at about 87.5% of its Pi-hole lease time), reconnects or reboots. Shortening Pi-hole's lease time speeds this up.
* **DNS is only answered while active.** Pi-hole hands out `192.168.86.11` as the *secondary* DNS server. With the
  default DNS probe, a Pi-hole DNS outage makes the device go active, so .11 starts answering. In `probe_mode: dhcp`,
  if Pi-hole's DNS fails while its DHCP keeps working, clients that try .11 get no answer.
* **DNS mode doesn't see DHCP-only failures.** If Pi-hole's DNS answers but its DHCP server is off or broken, the
  device stays passive. Use `probe_mode: dhcp` if that matters more to you than DHCP cooldowns.
* There is no IPv6, and DHCP relays are only minimally handled (replies go to giaddr).

### Pi-hole v6 API (UNVERIFIED shapes)

None of these have been tested against a live Pi-hole yet. If a field differs, **Sync Status** shows the error.

* `POST /api/auth {"password": "<app password>"}` returns `session.sid`, which is sent as the `X-FTL-SID` header
* `GET /api/config/dhcp/hosts` returns `config.dhcp.hosts[]`, each entry `"MAC,IP,name"`
* `GET /api/dhcp/leases` returns `leases[]` with `ip`, `hwaddr` and `name`
* `GET /api/config/dns/hosts` returns `config.dns.hosts[]`, each entry `"IP name [name...]"`
* `DELETE /api/auth` logs out so the session slot is freed

Pi-hole v5 is not supported.

## Supported hardware

| Board | Example | Notes |
|---|---|---|
| WT32-ETH01 (ESP32 + LAN8720) | [`examples/wt32-eth01.yaml`](examples/wt32-eth01.yaml) | **Recommended**, wired, cheap |
| ESP32-S3 + W5500 (SPI Ethernet) | [`examples/esp32s3-w5500.yaml`](examples/esp32s3-w5500.yaml) | Wired. The pins are placeholders |
| Generic ESP32 on WiFi | [`examples/esp32-wifi.yaml`](examples/esp32-wifi.yaml) | Works, see caveats |
| ESP32-S3-DevKitC-1 on WiFi | [`examples/esp32s3-wifi.yaml`](examples/esp32s3-wifi.yaml) | Works, see caveats |

**Use wired Ethernet for a failover service if you can.** On WiFi, the backup depends on the same AP and router health
you might be failing over from. It also adds latency and broadcast loss (DHCP is broadcast-heavy), and WiFi drops
look like a Pi-hole outage. If you do use WiFi, set `power_save_mode: none` and give it a static IP.

The device's own IP is taken from `address:` if you set it. Otherwise it's detected from whichever interface comes up.
**Always use a static IP**, because a DHCP server can't depend on DHCP.

## Install

```yaml
external_components:
  - source: github://loucks1/DHCP-Failover-ESPHome
    components: [dhcp_failover]

time:
  - platform: sntp
    id: sntp_time
    timezone: America/Chicago

dhcp_failover:
  time_id: sntp_time
  address: 192.168.86.11
  pihole_ip: 192.168.86.10
  pihole_password: !secret pihole_app_password
```

See `examples/` for complete configs. Copy `secrets.example.yaml` to `secrets.yaml` and fill it in. `secrets.yaml` is
gitignored.

### Options (defaults)

| Key | Default |
|---|---|
| `pihole_ip` | `192.168.86.10` |
| `pihole_port` | `80` (Pi-hole v6's web/API is often on `8080`; check the URL you use for the admin UI) |
| `pihole_password` | **required** (Pi-hole app password) |
| `address` | auto-detected (this device's static IP) |
| `gateway` / `netmask` | `192.168.86.1` / `255.255.255.0` |
| `pool_start` / `pool_end` | `192.168.86.210` / `192.168.86.249` |
| `lease_time` | `10min` |
| `domain` | `lan` |
| `upstream_dns` | `[1.1.1.1, 9.9.9.9]` |
| `probe_mode` | `dns` (or `dhcp`, the older DHCPDISCOVER probe) |
| `probe_dns_name` | `proxmox1.lan` (DNS mode: a name Pi-hole always answers) |
| `probe_dns_server` | same as `pihole_ip` (DNS mode; must not be this device) |
| `probe_dns_timeout` | `2s` (DNS mode; 200ms–10s) |
| `probe_interval_min` / `probe_interval_max` | `60s` / `120s` (random wait between probes while passive and healthy) |
| `probe_interval` | deprecated; sets a fixed interval (min = max) |
| `probe_retry_interval` | `5s` (after a failed probe, and continuously while active) |
| `fail_threshold` / `recover_threshold` | `4` / `3` (the examples use `2` / `2`) |
| `sync_time` | `"03:30"` (local time from `time_id`) |
| `dns_ttl` | `60` |

### Pi-hole app password

In the Pi-hole web UI, go to **Settings → Web interface / API** (switch to Expert mode if needed). Choose
**Configure app password**, copy the password, and click **Enable new app password**. Put it in `secrets.yaml` as
`pihole_app_password`.

## Home Assistant entities

| Entity | Type | Meaning |
|---|---|---|
| Mode | text sensor | `passive` / `active` |
| Override | select (restored across reboots) | `auto` / `force_active` / `force_passive` |
| Pi-hole DHCP Healthy | binary sensor (connectivity) | last probe passed: a good DNS answer (`dns` mode) or an OFFER (`dhcp` mode). The name is kept so existing entity IDs don't change |
| Reservations | sensor | reservations in the snapshot |
| Active Leases | sensor | live leases this device has handed out |
| Last Sync | text sensor (timestamp) | last successful Pi-hole sync (UTC ISO-8601) |
| Sync Status | text sensor | `ok: N res, N leases, N dns` or the error |
| Sync Now | button | sync from Pi-hole now |

OTA, API, logger and the optional `web_server` are standard ESPHome.

## First flash

**ESPHome web flasher / dashboard.** Build the firmware in the ESPHome dashboard, choose *Install → Manual download*,
and flash it at <https://web.esphome.io> over the serial wiring below. After that, every update goes over the air.

**WT32-ETH01 serial wiring** (the board has no USB). Use a **3.3 V** USB-serial adapter (CP2102/FTDI):

| Adapter | WT32-ETH01 |
|---|---|
| TX | RX0 |
| RX | TX0 |
| GND | GND |
| 5V | 5V |

1. Jumper **IO0 → GND**, then power the board on. This puts it in download mode.
2. Flash with `esphome run examples/wt32-eth01.yaml` (choose the serial port) or use the web flasher.
3. **Remove the IO0 jumper** and power-cycle the board. It boots normally, and later updates go over OTA.

## Test plan

1. After boot, check that **Sync Status** shows `ok: …` and that **Reservations** matches Pi-hole. **Mode** should be
   `passive` and **Pi-hole DHCP Healthy** should be on.
2. Set **Override → force_active**. Mode becomes `active`.
   * From a test client: `dig @192.168.86.11 <host>.lan`, `dig @192.168.86.11 example.com`, and
     `dig @192.168.86.11 -x 192.168.86.10`.
   * Renew DHCP on a test client (for example `sudo dhclient -v -r && sudo dhclient -v`, or toggle WiFi). You may get
     answers from both Pi-hole *and* this device.
3. **Set Override back to `auto`.** If you leave it on `force_active`, two DHCP servers answer on your LAN.
4. Optional real outage test (DNS mode): stop Pi-hole (for example `systemctl stop pihole-FTL`).
   Within one probe interval (up to 120s) plus about 7s, Mode goes `active`. Start it again, and about 5–10s later Mode
   goes `passive`. In `probe_mode: dhcp`, stop Pi-hole's DHCP instead.

## Credits

Ported from a standalone PlatformIO firmware by the same author. MIT licensed.
