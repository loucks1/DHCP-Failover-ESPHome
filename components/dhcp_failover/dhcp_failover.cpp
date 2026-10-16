#include "dhcp_failover.h"

#include <cctype>
#include <cstring>
#include <fcntl.h>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/util.h"

#include <ArduinoJson.h>
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace esphome::dhcp_failover {

static const char *const TAG = "dhcp_failover";
static const uint8_t PROBE_MAC[6] = {0x02, 0x00, 0x5E, 0xD1, 0x0C, 0xF0};  // locally administered, fake
static const uint8_t COOKIE[4] = {99, 130, 83, 99};
static const uint32_t PROBE_TIMEOUT_MS = 5000;  // dnsmasq may ping-check before OFFER
static const uint32_t SYNC_RETRY_MS = 15UL * 60 * 1000;
static const char *const OPTS[] = {"auto", "force_active", "force_passive"};
enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE, INFORM };

// ------------------------------------------------------------ small helpers
static uint32_t uptime_s() { return (uint32_t) (esp_timer_get_time() / 1000000ULL); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static std::string ip_str(uint32_t u) {
  char b[16];
  snprintf(b, sizeof b, "%u.%u.%u.%u", (unsigned) (u >> 24), (unsigned) ((u >> 16) & 255), (unsigned) ((u >> 8) & 255),
           (unsigned) (u & 255));
  return b;
}
static std::string mac_str(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}
static bool parse_mac(const char *s, uint8_t *o) {
  unsigned v[6];
  char c;
  if (!s || strlen(s) != 17 ||
      sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &c) != 6)
    return false;
  for (int i = 0; i < 6; i++) o[i] = v[i];
  return true;
}
static bool parse_ip(const char *s, uint32_t *o) {
  unsigned a, b, c, d;
  char x;
  if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &x) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return false;
  *o = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}
static int udp_sock(uint16_t port, bool bcast) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr *) &a, sizeof a) < 0) { close(s); return -1; }
  fcntl(s, F_SETFL, O_NONBLOCK);
  return s;
}
static void send_to(int s, const uint8_t *b, int n, uint32_t ip, uint16_t port) {
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(ip);
  sendto(s, b, n, 0, (sockaddr *) &a, sizeof a);
}
// find option in a DHCP packet
static const uint8_t *opt(const uint8_t *b, int n, uint8_t code, int *len) {
  int i = 240;
  while (i < n) {
    uint8_t c = b[i];
    if (c == 255) break;
    if (c == 0) { i++; continue; }
    if (i + 1 >= n) break;
    int l = b[i + 1];
    if (i + 2 + l > n) break;
    if (c == code) { *len = l; return b + i + 2; }
    i += 2 + l;
  }
  return nullptr;
}

// ------------------------------------------------------------ entities
void OverrideSelect::control(size_t index) { this->parent_->set_override(index); }
void SyncButton::press_action() { this->parent_->request_sync(); }

// ------------------------------------------------------------ lifecycle
void DhcpFailover::setup() {
  this->tab_ = new Entry[MAX_ENTRIES]();  // NOLINT
  this->dns_ = new DnsRec[MAX_DNS]();     // NOLINT
  memset(this->pend_, 0, sizeof this->pend_);
  this->load_snapshot_();

  this->override_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("dhcp_failover_override"));
  uint8_t ov = OV_AUTO;
  if (!this->override_pref_.load(&ov) || ov > OV_FORCE_PASSIVE) ov = OV_AUTO;
  this->override_ = ov;
  if (this->override_sel_ != nullptr) this->override_sel_->publish_state((size_t) ov);
  this->publish_status_();
}

void DhcpFailover::dump_config() {
  ESP_LOGCONFIG(TAG, "DHCP failover:");
  ESP_LOGCONFIG(TAG, "  Pi-hole: %s:%u", ip_str(this->pihole_ip_).c_str(), this->pihole_port_);
  ESP_LOGCONFIG(TAG, "  This device: %s  gateway %s  mask %s", ip_str(this->self_ip_).c_str(),
                ip_str(this->gateway_).c_str(), ip_str(this->netmask_).c_str());
  ESP_LOGCONFIG(TAG, "  Pool: %s - %s, lease %us, domain '%s'", ip_str(this->pool_start_).c_str(),
                ip_str(this->pool_end_).c_str(), (unsigned) this->lease_secs_, this->domain_.c_str());
  ESP_LOGCONFIG(TAG, "  Upstream DNS: %s, %s", ip_str(this->up1_).c_str(), ip_str(this->up2_).c_str());
  ESP_LOGCONFIG(TAG, "  Probe every %ums; %u fails -> active, %u ok -> passive", (unsigned) this->probe_interval_ms_,
                this->fail_threshold_, this->recover_threshold_);
  ESP_LOGCONFIG(TAG, "  Nightly sync %02u:%02u; override %s", this->sync_hour_, this->sync_minute_,
                OPTS[this->override_]);
}

void DhcpFailover::on_shutdown() {
  this->set_active_(false, "shutdown/OTA");
}

void DhcpFailover::set_override(uint8_t o) {
  if (o > OV_FORCE_PASSIVE) return;
  this->override_ = o;
  this->override_pref_.save(&o);
  if (this->override_sel_ != nullptr) this->override_sel_->publish_state((size_t) o);
  ESP_LOGI(TAG, "Override -> %s", OPTS[o]);
}

void DhcpFailover::loop() {
  bool net = network::is_connected();
  if (net && this->self_ip_ == 0) {  // auto-detect from whichever netif (ethernet or wifi) is up
    for (auto &a : network::get_ip_addresses()) {
      if (a.is_ip4() && a.is_set()) {
        ip4_addr_t a4 = (ip4_addr_t) a;
        uint32_t v = ntohl(a4.addr);
        this->self_ip_ = v;
        ESP_LOGI(TAG, "Using device IP %s", ip_str(v).c_str());
        break;
      }
    }
  }
  if (this->self_ip_ == 0) return;
  if (net) this->probe_loop_();

  if (this->override_ == OV_FORCE_ACTIVE) this->set_active_(true, "forced");
  else if (this->override_ == OV_FORCE_PASSIVE) this->set_active_(false, "forced");
  else if (!this->active_ && this->consec_fail_ >= this->fail_threshold_) this->set_active_(true, "Pi-hole DHCP not answering");
  else if (this->active_ && this->consec_ok_ >= this->recover_threshold_) this->set_active_(false, "Pi-hole DHCP back");

  if (this->active_) {
    this->dhcp_loop_();
    this->dns_loop_();
    if (this->dirty_ && millis() - this->last_save_ms_ > 60000) this->save_snapshot_();
  }
  if (this->sync_done_) this->commit_sync_();
  if (net) this->schedule_sync_();
  if (millis() - this->last_status_ms_ > 30000) this->publish_status_();
}

void DhcpFailover::set_active_(bool on, const char *why) {
  if (on == this->active_) return;
  this->active_ = on;
  ESP_LOGW(TAG, "MODE -> %s (%s)", on ? "ACTIVE" : "PASSIVE", why);
  if (on) { this->dhcp_begin_(); this->dns_begin_(); }
  else { this->dhcp_stop_(); this->dns_stop_(); this->save_snapshot_(); }
  this->publish_status_();
}

void DhcpFailover::publish_status_() {
  this->last_status_ms_ = millis();
  if (this->mode_ts_) {
    const char *m = this->active_ ? "active" : "passive";
    if (this->mode_ts_->state != m) this->mode_ts_->publish_state(m);
  }
  if (this->healthy_bs_ && this->healthy_known_) this->healthy_bs_->publish_state(this->consec_fail_ == 0);
  if (this->res_s_) this->res_s_->publish_state(this->cnt_res_);
  if (this->lease_s_) this->lease_s_->publish_state(this->count_live_leases_());
}

// ------------------------------------------------------------ table helpers
bool DhcpFailover::in_subnet_(uint32_t ip) const {
  uint32_t m = this->netmask_, net = this->self_ip_ & m;
  return (ip & m) == net && (ip & ~m) != 0 && (ip & ~m) != ~m;
}
bool DhcpFailover::reserved_ip_(uint32_t ip) const {
  return ip == this->self_ip_ || ip == this->gateway_ || ip == this->pihole_ip_;
}
bool DhcpFailover::live_(const Entry &e) const {
  if (e.kind == K_FREE) return false;
  if ((e.kind == K_OURS || e.kind == K_DECLINED) && (int32_t) (e.expires - uptime_s()) < 0) return false;
  return true;
}
int DhcpFailover::count_live_leases_() {
  int n = 0;
  for (int i = 0; i < MAX_ENTRIES; i++) if (this->tab_[i].kind == K_OURS && this->live_(this->tab_[i])) n++;
  return n;
}
Entry *DhcpFailover::find_mac_(const uint8_t *mac, uint8_t kind) {
  for (int i = 0; i < MAX_ENTRIES; i++)
    if (this->tab_[i].kind == kind && memcmp(this->tab_[i].mac, mac, 6) == 0) return &this->tab_[i];
  return nullptr;
}
Entry *DhcpFailover::ip_owner_(uint32_t ip) {
  for (int i = 0; i < MAX_ENTRIES; i++) if (this->tab_[i].kind == K_RES && this->tab_[i].ip == ip) return &this->tab_[i];
  for (int i = 0; i < MAX_ENTRIES; i++) if (this->tab_[i].ip == ip && this->live_(this->tab_[i])) return &this->tab_[i];
  return nullptr;
}
bool DhcpFailover::usable_for_(uint32_t ip, const uint8_t *mac) {
  if (!this->in_subnet_(ip) || this->reserved_ip_(ip)) return false;
  Entry *o = this->ip_owner_(ip);
  return !o || memcmp(o->mac, mac, 6) == 0;
}
uint32_t DhcpFailover::choose_ip_(const uint8_t *mac) {
  if (Entry *r = this->find_mac_(mac, K_RES)) return r->ip;
  if (Entry *e = this->find_mac_(mac, K_OURS)) if (this->usable_for_(e->ip, mac)) return e->ip;
  if (Entry *e = this->find_mac_(mac, K_PIHOLE)) if (this->usable_for_(e->ip, mac)) return e->ip;
  for (uint32_t ip = this->pool_start_; ip <= this->pool_end_; ip++)
    if (!this->reserved_ip_(ip) && this->in_subnet_(ip) && !this->ip_owner_(ip)) return ip;
  return 0;
}
Entry *DhcpFailover::alloc_slot_() {
  for (int i = 0; i < MAX_ENTRIES; i++) if (this->tab_[i].kind == K_FREE) return &this->tab_[i];
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry &e = this->tab_[i];
    if ((e.kind == K_OURS || e.kind == K_DECLINED) && !this->live_(e)) return &e;
  }
  for (int i = 0; i < MAX_ENTRIES; i++) if (this->tab_[i].kind == K_PIHOLE) return &this->tab_[i];
  return nullptr;
}
Entry *DhcpFailover::set_ours_(const uint8_t *mac, uint32_t ip, const char *name) {
  Entry *e = this->find_mac_(mac, K_RES);
  if (e) return e;
  e = this->find_mac_(mac, K_OURS);
  if (!e) {
    e = this->alloc_slot_();
    if (!e) return nullptr;
    memset(e, 0, sizeof *e);
    memcpy(e->mac, mac, 6);
    e->kind = K_OURS;
  }
  if (e->ip != ip) this->dirty_ = true;
  e->ip = ip;
  e->expires = uptime_s() + this->lease_secs_;
  Entry *p = this->find_mac_(mac, K_PIHOLE);
  if (name && *name) strncpy(e->name, name, 32);
  else if (!e->name[0] && p) strncpy(e->name, p->name, 32);
  if (p) p->kind = K_FREE;  // superseded hint
  return e;
}
void DhcpFailover::release_ours_(const uint8_t *mac) {
  if (Entry *e = this->find_mac_(mac, K_OURS)) { e->expires = uptime_s() - 1; this->dirty_ = true; }
}
void DhcpFailover::mark_declined_(uint32_t ip) {
  Entry *e = this->alloc_slot_();
  if (!e) return;
  memset(e, 0, sizeof *e);
  e->kind = K_DECLINED;
  e->ip = ip;
  e->expires = uptime_s() + 3600;
}
bool DhcpFailover::name_eq_(const char *a, const char *lname) const {
  size_t n = strlen(lname);
  if (strncasecmp(a, lname, n) != 0) return false;
  if (a[n] == 0) return true;
  return a[n] == '.' && strcasecmp(a + n + 1, this->domain_.c_str()) == 0;
}
uint32_t DhcpFailover::lookup_name_(const char *lname) {
  for (int i = 0; i < this->dns_count_; i++) if (this->name_eq_(this->dns_[i].name, lname)) return this->dns_[i].ip;
  for (uint8_t k : {K_RES, K_OURS, K_PIHOLE})
    for (int i = 0; i < MAX_ENTRIES; i++) {
      Entry &e = this->tab_[i];
      if (e.kind == k && e.name[0] && this->name_eq_(e.name, lname)) return e.ip;
    }
  return 0;
}
const char *DhcpFailover::lookup_ip_(uint32_t ip) {
  for (int i = 0; i < this->dns_count_; i++) if (this->dns_[i].ip == ip) return this->dns_[i].name;
  for (uint8_t k : {K_RES, K_OURS, K_PIHOLE})
    for (int i = 0; i < MAX_ENTRIES; i++) {
      Entry &e = this->tab_[i];
      if (e.kind == k && e.ip == ip && e.name[0]) return e.name;
    }
  return nullptr;
}

// ------------------------------------------------------------ snapshot (NVS blob)
// Layout: u32 magic, u32 sync_epoch, u16 nent, u16 ndns, Entry[nent], DnsRec[ndns]
static const uint32_t SNAP_MAGIC = 0x44464F31;  // "DFO1"
void DhcpFailover::save_snapshot_() {
  std::vector<uint8_t> buf(12);
  uint16_t nent = 0;
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry e = this->tab_[i];
    if (e.kind != K_RES && e.kind != K_PIHOLE && e.kind != K_OURS) continue;
    if (e.kind == K_OURS) { e.kind = K_PIHOLE; e.expires = 0; }  // our leases become IP-affinity hints
    const uint8_t *p = (const uint8_t *) &e;
    buf.insert(buf.end(), p, p + sizeof e);
    nent++;
  }
  const uint8_t *d = (const uint8_t *) this->dns_;
  buf.insert(buf.end(), d, d + sizeof(DnsRec) * this->dns_count_);
  uint32_t ep = (uint32_t) this->last_sync_epoch_;
  uint16_t nd = this->dns_count_;
  memcpy(&buf[0], &SNAP_MAGIC, 4);
  memcpy(&buf[4], &ep, 4);
  memcpy(&buf[8], &nent, 2);
  memcpy(&buf[10], &nd, 2);
  nvs_handle_t h;
  if (nvs_open("dhcp_fo", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "NVS open failed"); return; }
  esp_err_t err = nvs_set_blob(h, "snap", buf.data(), buf.size());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) ESP_LOGE(TAG, "snapshot save failed: %s", esp_err_to_name(err));
  else ESP_LOGD(TAG, "snapshot saved (%u bytes)", (unsigned) buf.size());
  this->dirty_ = false;
  this->last_save_ms_ = millis();
}
void DhcpFailover::load_snapshot_() {
  nvs_handle_t h;
  if (nvs_open("dhcp_fo", NVS_READONLY, &h) != ESP_OK) { ESP_LOGI(TAG, "no snapshot"); return; }
  size_t len = 0;
  std::vector<uint8_t> buf;
  if (nvs_get_blob(h, "snap", nullptr, &len) == ESP_OK && len >= 12) {
    buf.resize(len);
    if (nvs_get_blob(h, "snap", buf.data(), &len) != ESP_OK) buf.clear();
  }
  nvs_close(h);
  uint32_t magic = 0, ep = 0;
  uint16_t nent = 0, nd = 0;
  if (buf.size() >= 12) { memcpy(&magic, &buf[0], 4); memcpy(&ep, &buf[4], 4); memcpy(&nent, &buf[8], 2); memcpy(&nd, &buf[10], 2); }
  if (magic != SNAP_MAGIC || buf.size() != 12 + nent * sizeof(Entry) + nd * sizeof(DnsRec) || nent > MAX_ENTRIES ||
      nd > MAX_DNS) {
    ESP_LOGW(TAG, "no valid snapshot");
    return;
  }
  memcpy(this->tab_, &buf[12], nent * sizeof(Entry));
  memcpy(this->dns_, &buf[12 + nent * sizeof(Entry)], nd * sizeof(DnsRec));
  this->dns_count_ = nd;
  this->last_sync_epoch_ = ep;
  this->cnt_res_ = this->cnt_leases_ = 0;
  for (int i = 0; i < nent; i++) (this->tab_[i].kind == K_RES ? this->cnt_res_ : this->cnt_leases_)++;
  this->sync_msg_ = "loaded snapshot from flash";
  ESP_LOGI(TAG, "snapshot: %d res, %d leases, %d dns", this->cnt_res_, this->cnt_leases_, this->dns_count_);
  if (this->sync_status_ts_) this->sync_status_ts_->publish_state(this->sync_msg_);
  this->publish_last_sync_();
}

void DhcpFailover::publish_last_sync_() {
  if (!this->last_sync_ts_ || this->last_sync_epoch_ < 1700000000) return;
  char b[32];
  struct tm tm;
  gmtime_r(&this->last_sync_epoch_, &tm);
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  this->last_sync_ts_->publish_state(b);
}

// ------------------------------------------------------------ DHCP server
void DhcpFailover::dhcp_begin_() {
  if (this->srv_ < 0) {
    this->srv_ = udp_sock(67, true);
    if (this->srv_ >= 0) ESP_LOGI(TAG, "DHCP server listening :67");
    else ESP_LOGE(TAG, "DHCP server BIND FAILED");
  }
}
void DhcpFailover::dhcp_stop_() {
  if (this->srv_ >= 0) { close(this->srv_); this->srv_ = -1; ESP_LOGI(TAG, "DHCP server stopped"); }
}

void DhcpFailover::dhcp_reply_(const uint8_t *q, uint8_t type, uint32_t yiaddr, bool with_lease) {
  uint8_t r[400] = {};
  r[0] = 2; r[1] = 1; r[2] = 6;
  memcpy(r + 4, q + 4, 4);    // xid
  memcpy(r + 10, q + 10, 2);  // flags
  uint32_t ciaddr = rd32(q + 12), giaddr = rd32(q + 24);
  if (type == ACK && !with_lease) wr32(r + 12, ciaddr);  // INFORM
  wr32(r + 16, yiaddr);
  memcpy(r + 24, q + 24, 4);    // giaddr
  memcpy(r + 28, q + 28, 16);   // chaddr
  memcpy(r + 236, COOKIE, 4);
  int i = 240;
  auto o8 = [&](uint8_t c, uint8_t v) { r[i++] = c; r[i++] = 1; r[i++] = v; };
  auto o32 = [&](uint8_t c, uint32_t v) { r[i++] = c; r[i++] = 4; wr32(r + i, v); i += 4; };
  o8(53, type);
  o32(54, this->self_ip_);
  if (type != NAK) {
    if (with_lease) {
      o32(51, this->lease_secs_);
      o32(58, this->lease_secs_ / 2);
      o32(59, this->lease_secs_ * 7 / 8);
    }
    uint32_t m = this->netmask_;
    o32(1, m);
    o32(3, this->gateway_);
    o32(6, this->self_ip_);  // DNS = this device
    o32(28, (this->self_ip_ & m) | ~m);
    size_t dl = std::min<size_t>(this->domain_.size(), 64);
    r[i++] = 15; r[i++] = dl; memcpy(r + i, this->domain_.data(), dl); i += dl;
  }
  r[i++] = 255;
  int n = i < 300 ? 300 : i;
  if (giaddr) send_to(this->srv_, r, n, giaddr, 67);
  else if (type != NAK && ciaddr && !(q[10] & 0x80)) send_to(this->srv_, r, n, ciaddr, 68);
  else send_to(this->srv_, r, n, 0xFFFFFFFF, 68);
}

void DhcpFailover::dhcp_handle_(const uint8_t *b, int n) {
  if (n < 244 || b[0] != 1 || b[1] != 1 || b[2] != 6 || memcmp(b + 236, COOKIE, 4)) return;
  const uint8_t *mac = b + 28;
  if (!memcmp(mac, PROBE_MAC, 6)) return;  // our own probe
  int l = 0;
  const uint8_t *p = opt(b, n, 53, &l);
  if (!p || l != 1) return;
  uint8_t type = p[0];
  char host[33] = "";
  if ((p = opt(b, n, 12, &l))) {
    int k = l < 32 ? l : 32;
    memcpy(host, p, k);
    host[k] = 0;
    for (char *c = host; *c; c++) if (!isalnum((unsigned char) *c) && *c != '-') *c = '-';
  }
  uint32_t sid = 0, req = 0, ciaddr = rd32(b + 12);
  if ((p = opt(b, n, 54, &l)) && l == 4) sid = rd32(p);
  if ((p = opt(b, n, 50, &l)) && l == 4) req = rd32(p);

  switch (type) {
    case DISCOVER: {
      uint32_t ip = this->choose_ip_(mac);
      if (!ip) { ESP_LOGW(TAG, "DHCP: pool exhausted"); return; }
      this->dhcp_reply_(b, OFFER, ip, true);
      ESP_LOGI(TAG, "DHCP OFFER %s -> %s", mac_str(mac).c_str(), ip_str(ip).c_str());
      break;
    }
    case REQUEST: {
      if (sid && sid != this->self_ip_) return;  // client picked another server
      uint32_t want = req ? req : ciaddr;
      uint32_t ip = this->choose_ip_(mac);
      if (want && want == ip && this->in_subnet_(want)) {
        this->set_ours_(mac, ip, host);
        this->dhcp_reply_(b, ACK, ip, true);
        ESP_LOGI(TAG, "DHCP ACK %s -> %s", mac_str(mac).c_str(), ip_str(ip).c_str());
      } else {
        this->dhcp_reply_(b, NAK, 0, false);
        ESP_LOGI(TAG, "DHCP NAK %s wanted %s", mac_str(mac).c_str(), ip_str(want).c_str());
      }
      break;
    }
    case RELEASE:
      if (sid == this->self_ip_) this->release_ours_(mac);
      break;
    case DECLINE:
      if (sid == this->self_ip_ && req) { this->mark_declined_(req); this->release_ours_(mac); }
      break;
    case INFORM:
      if (ciaddr) this->dhcp_reply_(b, ACK, 0, false);
      break;
  }
}

void DhcpFailover::dhcp_loop_() {
  if (this->srv_ < 0) return;
  uint8_t b[600];
  for (int k = 0; k < 8; k++) {
    int n = recv(this->srv_, b, sizeof b, 0);
    if (n <= 0) break;
    this->dhcp_handle_(b, n);
  }
}

// ------------------------------------------------------------ Pi-hole DHCP probe
void DhcpFailover::send_probe_() {
  uint8_t b[300] = {};
  b[0] = 1; b[1] = 1; b[2] = 6;
  this->probe_xid_ = esp_random();
  wr32(b + 4, this->probe_xid_);
  b[10] = 0x80;  // broadcast flag
  memcpy(b + 28, PROBE_MAC, 6);
  memcpy(b + 236, COOKIE, 4);
  int i = 240;
  b[i++] = 53; b[i++] = 1; b[i++] = DISCOVER;
  b[i++] = 55; b[i++] = 3; b[i++] = 1; b[i++] = 3; b[i++] = 6;
  b[i++] = 255;
  send_to(this->prb_, b, 300, 0xFFFFFFFF, 67);
  this->probe_sent_ms_ = millis();
  this->probe_waiting_ = true;
  this->got_offer_ = false;
}

void DhcpFailover::probe_loop_() {
  if (this->prb_ < 0) { this->prb_ = udp_sock(68, true); if (this->prb_ < 0) return; }
  uint8_t b[600];
  int n;
  while ((n = recv(this->prb_, b, sizeof b, 0)) > 0) {
    if (!this->probe_waiting_ || n < 244 || b[0] != 2 || rd32(b + 4) != this->probe_xid_ || memcmp(b + 28, PROBE_MAC, 6))
      continue;
    int l = 0, l2 = 0;
    const uint8_t *p = opt(b, n, 53, &l);
    const uint8_t *s = opt(b, n, 54, &l2);
    if (p && l == 1 && p[0] == OFFER && s && l2 == 4 && rd32(s) == this->pihole_ip_) this->got_offer_ = true;
  }
  if (this->probe_waiting_) {
    if (this->got_offer_ || millis() - this->probe_sent_ms_ > PROBE_TIMEOUT_MS) {
      this->probe_waiting_ = false;
      bool was = this->consec_fail_ == 0 && this->healthy_known_;
      if (this->got_offer_) { this->consec_ok_++; this->consec_fail_ = 0; }
      else { this->consec_fail_++; this->consec_ok_ = 0; }
      this->healthy_known_ = true;
      ESP_LOGD(TAG, "probe: offer=%d -> fail=%u ok=%u", this->got_offer_, (unsigned) this->consec_fail_,
               (unsigned) this->consec_ok_);
      if (was != (this->consec_fail_ == 0) || this->consec_ok_ == 1 || this->consec_fail_ == 1) this->publish_status_();
    }
  } else if (this->last_probe_ms_ == 0 || millis() - this->last_probe_ms_ > this->probe_interval_ms_) {
    this->last_probe_ms_ = millis();
    this->send_probe_();
  }
}

// ------------------------------------------------------------ DNS (active only)
void DhcpFailover::dns_begin_() {
  if (this->s53_ < 0) {
    this->s53_ = udp_sock(53, false);
    if (this->s53_ >= 0) ESP_LOGI(TAG, "DNS listening :53");
    else ESP_LOGE(TAG, "DNS BIND FAILED");
  }
  if (this->up_ < 0) this->up_ = udp_sock(0, false);
}
void DhcpFailover::dns_stop_() {
  if (this->s53_ >= 0) { close(this->s53_); this->s53_ = -1; ESP_LOGI(TAG, "DNS stopped"); }
  if (this->up_ >= 0) { close(this->up_); this->up_ = -1; }
  memset(this->pend_, 0, sizeof this->pend_);
}
void DhcpFailover::dns_send_up_(Pending &p, uint32_t ip) {
  send_to(this->up_, p.q, p.len, ip, 53);
  p.sent_ms = millis();
  p.tries++;
}
static int parse_q(const uint8_t *b, int n, char *name, int cap, uint16_t *qtype) {
  int i = 12, o = 0;
  while (i < n && b[i]) {
    int l = b[i];
    if ((l & 0xC0) || i + 1 + l > n || o + l + 2 > cap) return -1;
    if (o) name[o++] = '.';
    for (int k = 0; k < l; k++) name[o++] = tolower(b[i + 1 + k]);
    i += 1 + l;
  }
  name[o] = 0;
  if (i + 5 > n) return -1;
  *qtype = (b[i + 1] << 8) | b[i + 2];
  return i + 5;
}
void DhcpFailover::dns_answer_(const uint8_t *q, int qend, const void *cl, uint8_t rcode, uint16_t type,
                               const uint8_t *rdata, int rdlen) {
  uint8_t r[600];
  if (qend > 512) return;
  memcpy(r, q, qend);
  r[2] = 0x84 | (q[2] & 0x01);  // QR, AA, keep RD
  r[3] = 0x80 | rcode;          // RA
  r[4] = 0; r[5] = 1; r[6] = 0; r[7] = rdata ? 1 : 0; r[8] = r[9] = r[10] = r[11] = 0;
  int i = qend;
  if (rdata) {
    r[i++] = 0xC0; r[i++] = 12;
    r[i++] = type >> 8; r[i++] = type; r[i++] = 0; r[i++] = 1;
    uint32_t ttl = this->dns_ttl_;
    r[i++] = ttl >> 24; r[i++] = ttl >> 16; r[i++] = ttl >> 8; r[i++] = ttl;
    r[i++] = rdlen >> 8; r[i++] = rdlen;
    memcpy(r + i, rdata, rdlen);
    i += rdlen;
  }
  sendto(this->s53_, r, i, 0, (const sockaddr *) cl, sizeof(sockaddr_in));
}
void DhcpFailover::dns_handle_(uint8_t *b, int n, const void *cl) {
  if (n < 17 || (b[2] & 0x80) || ((b[4] << 8) | b[5]) != 1) return;
  char name[256];
  uint16_t qt;
  int qend = parse_q(b, n, name, sizeof name, &qt);
  if (qend < 0) return;
  const char *dom = this->domain_.c_str();
  size_t nl = strlen(name), dl = strlen(dom);
  bool local = false;
  if (nl > dl + 1 && name[nl - dl - 1] == '.' && !strcmp(name + nl - dl, dom)) { name[nl - dl - 1] = 0; local = true; }
  else if (nl && !strchr(name, '.')) local = true;
  if (local) {
    uint32_t ip = this->lookup_name_(name);
    if (!ip) { this->dns_answer_(b, qend, cl, 3, 0, nullptr, 0); return; }  // NXDOMAIN, never leak upstream
    if (qt != 1 && qt != 255) { this->dns_answer_(b, qend, cl, 0, 0, nullptr, 0); return; }  // NODATA
    uint8_t a[4] = {(uint8_t) (ip >> 24), (uint8_t) (ip >> 16), (uint8_t) (ip >> 8), (uint8_t) ip};
    this->dns_answer_(b, qend, cl, 0, 1, a, 4);
    return;
  }
  unsigned d, c, bb, a;
  char tail[64];
  if (sscanf(name, "%u.%u.%u.%u.%63s", &d, &c, &bb, &a, tail) == 5 && !strcmp(tail, "in-addr.arpa")) {
    uint32_t ip = (a << 24) | (bb << 16) | (c << 8) | d;
    if (this->in_subnet_(ip)) {
      const char *h = this->lookup_ip_(ip);
      if (!h || qt != 12) { this->dns_answer_(b, qend, cl, h ? 0 : 3, 0, nullptr, 0); return; }
      uint8_t rd[300];
      int j = 0;
      char full[200];
      if (strchr(h, '.')) snprintf(full, sizeof full, "%s", h);
      else snprintf(full, sizeof full, "%s.%s", h, dom);
      for (char *p = full, *e; *p; p = *e ? e + 1 : e) {
        e = strchr(p, '.');
        if (!e) e = p + strlen(p);
        int l = e - p;
        if (l > 63 || j + l + 2 > 290) break;
        rd[j++] = l;
        memcpy(rd + j, p, l);
        j += l;
      }
      rd[j++] = 0;
      this->dns_answer_(b, qend, cl, 0, 12, rd, j);
      return;
    }
  }
  if (n > 512 || this->up_ < 0) return;
  Pending *p = nullptr;
  for (auto &x : this->pend_) if (!x.used) { p = &x; break; }
  if (!p) { p = &this->pend_[0]; for (auto &x : this->pend_) if (x.sent_ms < p->sent_ms) p = &x; }
  p->used = true;
  p->tries = 0;
  memcpy(p->client, cl, sizeof(sockaddr_in));
  p->len = n;
  p->orig_id = (b[0] << 8) | b[1];
  p->new_id = esp_random();
  memcpy(p->q, b, n);
  p->q[0] = p->new_id >> 8;
  p->q[1] = p->new_id;
  this->dns_send_up_(*p, this->up1_);
}
void DhcpFailover::dns_loop_() {
  if (this->s53_ < 0) return;
  uint8_t b[1500];
  sockaddr_in from;
  socklen_t fl;
  for (int k = 0; k < 8; k++) {
    fl = sizeof from;
    int n = recvfrom(this->s53_, b, sizeof b, 0, (sockaddr *) &from, &fl);
    if (n <= 0) break;
    this->dns_handle_(b, n, &from);
  }
  for (int k = 0; k < 8 && this->up_ >= 0; k++) {
    fl = sizeof from;
    int n = recvfrom(this->up_, b, sizeof b, 0, (sockaddr *) &from, &fl);
    if (n <= 0) break;
    uint32_t src = ntohl(from.sin_addr.s_addr);
    if (n < 12 || (src != this->up1_ && src != this->up2_)) continue;
    uint16_t id = (b[0] << 8) | b[1];
    for (auto &p : this->pend_)
      if (p.used && p.new_id == id) {
        b[0] = p.orig_id >> 8;
        b[1] = p.orig_id;
        sendto(this->s53_, b, n, 0, (sockaddr *) p.client, sizeof(sockaddr_in));
        p.used = false;
        break;
      }
  }
  for (auto &p : this->pend_)
    if (p.used && millis() - p.sent_ms > 1500) {
      if (p.tries == 1) this->dns_send_up_(p, this->up2_);
      else p.used = false;
    }
}

// ------------------------------------------------------------ Pi-hole v6 sync
void DhcpFailover::schedule_sync_() {
  if (this->sync_running_) return;
  bool want = this->sync_requested_;
  if (!want && this->active_) return;  // Pi-hole is down anyway
  if (!this->boot_synced_ && millis() > 10000) want = true;
  if (!this->last_sync_ok_ && this->boot_synced_ && millis() - this->last_sync_try_ms_ > SYNC_RETRY_MS) want = true;
  if (this->time_ != nullptr) {
    ESPTime t = this->time_->now();
    if (t.is_valid() && t.hour == this->sync_hour_ && t.minute == this->sync_minute_ &&
        t.day_of_year != this->last_nightly_yday_) {
      this->last_nightly_yday_ = t.day_of_year;
      want = true;
    }
  }
  if (!want) return;
  this->sync_requested_ = false;
  this->boot_synced_ = true;
  this->last_sync_try_ms_ = millis();
  this->sync_running_ = true;
  this->sync_done_ = false;
  if (this->sync_status_ts_) this->sync_status_ts_->publish_state("syncing");
  if (xTaskCreate(&DhcpFailover::sync_task_, "dhcpfo_sync", 8192, this, 1, nullptr) != pdPASS) {
    this->sync_running_ = false;
    this->sync_msg_ = "could not start sync task";
    this->last_sync_ok_ = false;
    if (this->sync_status_ts_) this->sync_status_ts_->publish_state(this->sync_msg_);
  }
}

void DhcpFailover::sync_task_(void *arg) {
  static_cast<DhcpFailover *>(arg)->sync_worker_();
  vTaskDelete(nullptr);
}

bool DhcpFailover::http_(const char *method, const std::string &path, const std::string &sid, const std::string &body,
                         std::string &out, int &status) {
  std::string url = "http://" + ip_str(this->pihole_ip_) + ":" + std::to_string(this->pihole_port_) + path;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 8000;
  cfg.disable_auto_redirect = true;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return false;
  esp_http_client_method_t m = HTTP_METHOD_GET;
  if (!strcmp(method, "POST")) m = HTTP_METHOD_POST;
  else if (!strcmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
  esp_http_client_set_method(c, m);
  if (!sid.empty()) esp_http_client_set_header(c, "X-FTL-SID", sid.c_str());
  if (!body.empty()) esp_http_client_set_header(c, "Content-Type", "application/json");
  bool ok = false;
  out.clear();
  status = 0;
  if (esp_http_client_open(c, body.size()) == ESP_OK) {
    if (body.empty() || esp_http_client_write(c, body.data(), body.size()) == (int) body.size()) {
      esp_http_client_fetch_headers(c);
      status = esp_http_client_get_status_code(c);
      char buf[512];
      int r;
      while ((r = esp_http_client_read(c, buf, sizeof buf)) > 0) {
        if (out.size() + r > 64 * 1024) break;  // sanity cap
        out.append(buf, r);
      }
      ok = true;
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return ok;
}

static bool is_lease_time(const std::string &t) {  // "3600", "12h", "45m"
  size_t i = 0;
  while (i < t.size() && isdigit((unsigned char) t[i])) i++;
  return i > 0 && (i == t.size() || (i + 1 == t.size() && strchr("smhdw", t[i])));
}

// UNVERIFIED shapes (Pi-hole v6):
//  POST /api/auth {"password"} -> {"session":{"sid":...}}
//  GET /api/config/dhcp/hosts -> {"config":{"dhcp":{"hosts":["MAC,IP,name", ...]}}}
//  GET /api/dhcp/leases -> {"leases":[{"ip","hwaddr","name"}, ...]}
//  GET /api/config/dns/hosts -> {"config":{"dns":{"hosts":["IP name [name...]", ...]}}}
void DhcpFailover::sync_worker_() {
  std::string out, err, sid;
  int st = 0;
  std::vector<Res> res, leases;
  std::vector<DnsRec> dns;
  bool ok = true;
  {
    JsonDocument q;
    q["password"] = this->pihole_password_;
    std::string body;
    serializeJson(q, body);
    if (this->http_("POST", "/api/auth", "", body, out, st) && st == 200) {
      JsonDocument d;
      if (!deserializeJson(d, out)) sid = d["session"]["sid"] | "";
    }
    if (sid.empty()) { ok = false; err = "auth failed (HTTP " + std::to_string(st) + ")"; }
  }
  auto get = [&](const char *path, JsonDocument &d) -> bool {
    if (!this->http_("GET", path, sid, "", out, st) || st != 200) { err = std::string(path) + " HTTP " + std::to_string(st); return false; }
    DeserializationError e = deserializeJson(d, out);
    if (e) { err = std::string(path) + " JSON " + e.c_str(); return false; }
    return true;
  };
  if (ok) {
    JsonDocument d;
    if ((ok = get("/api/config/dhcp/hosts", d)))
      for (const char *s : d["config"]["dhcp"]["hosts"].as<JsonArray>()) {
        if (!s) continue;
        // dnsmasq dhcp-host style "MAC,IP,name[,lease]" (order may vary). Skip without MAC+IP.
        Res r{};
        bool has_mac = false;
        std::string str(s);
        size_t start = 0;
        while (start <= str.size()) {
          size_t cpos = str.find(',', start);
          if (cpos == std::string::npos) cpos = str.size();
          std::string t = str.substr(start, cpos - start);
          start = cpos + 1;
          while (!t.empty() && isspace((unsigned char) t.back())) t.pop_back();
          while (!t.empty() && isspace((unsigned char) t.front())) t.erase(0, 1);
          uint32_t ip;
          uint8_t mac[6];
          if (t.empty()) continue;
          if (!has_mac && parse_mac(t.c_str(), mac)) { memcpy(r.mac, mac, 6); has_mac = true; }
          else if (!r.ip && parse_ip(t.c_str(), &ip)) r.ip = ip;
          else if (t.find(':') != std::string::npos || t == "infinite" || t == "ignore" || is_lease_time(t)) continue;
          else if (r.name.empty()) r.name = t;
        }
        if (has_mac && r.ip && this->in_subnet_(r.ip)) res.push_back(r);
      }
  }
  if (ok) {
    JsonDocument d;
    if ((ok = get("/api/dhcp/leases", d)))
      for (JsonObject l : d["leases"].as<JsonArray>()) {
        Res r{};
        const char *nm = l["name"] | "";
        if (!parse_mac(l["hwaddr"] | "", r.mac) || !parse_ip(l["ip"] | "", &r.ip) || !this->in_subnet_(r.ip)) continue;
        r.name = strcmp(nm, "*") ? nm : "";
        leases.push_back(r);
      }
  }
  if (ok) {
    JsonDocument d;
    if ((ok = get("/api/config/dns/hosts", d)))
      for (const char *s : d["config"]["dns"]["hosts"].as<JsonArray>()) {
        if (!s) continue;
        char buf[256];
        strncpy(buf, s, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        char *sp = nullptr;
        char *ipt = strtok_r(buf, " \t", &sp);
        uint32_t ip;
        if (!ipt || !parse_ip(ipt, &ip)) continue;  // IPv6 ignored
        for (char *n = strtok_r(nullptr, " \t", &sp); n; n = strtok_r(nullptr, " \t", &sp)) {
          DnsRec r{};
          r.ip = ip;
          strncpy(r.name, n, sizeof r.name - 1);
          dns.push_back(r);
        }
      }
  }
  if (!sid.empty()) this->http_("DELETE", "/api/auth", sid, "", out, st);  // free the session slot

  this->new_res_ = std::move(res);
  this->new_leases_ = std::move(leases);
  this->new_dns_ = std::move(dns);
  this->sync_result_ok_ = ok;
  this->sync_msg_ = ok ? "ok" : err;
  this->sync_done_ = true;
  this->sync_running_ = false;
}

void DhcpFailover::commit_sync_() {
  this->sync_done_ = false;
  if (!this->sync_result_ok_) {
    this->last_sync_ok_ = false;
    ESP_LOGW(TAG, "sync failed: %s", this->sync_msg_.c_str());
    if (this->sync_status_ts_) this->sync_status_ts_->publish_state("failed: " + this->sync_msg_);
    return;
  }
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry &e = this->tab_[i];
    if (e.kind == K_RES || e.kind == K_PIHOLE) e.kind = K_FREE;
  }
  auto add = [this](const Res &r, uint8_t kind) {
    if (kind == K_PIHOLE && (this->find_mac_(r.mac, K_RES) || this->find_mac_(r.mac, K_OURS))) return;
    for (int i = 0; i < MAX_ENTRIES; i++) {
      Entry &e = this->tab_[i];
      if (e.kind != K_FREE) continue;
      memset(&e, 0, sizeof e);
      memcpy(e.mac, r.mac, 6);
      e.ip = r.ip;
      strncpy(e.name, r.name.c_str(), 32);
      e.kind = kind;
      return;
    }
  };
  for (auto &r : this->new_res_) add(r, K_RES);
  for (auto &r : this->new_leases_) add(r, K_PIHOLE);
  this->dns_count_ = 0;
  for (auto &r : this->new_dns_) if (this->dns_count_ < MAX_DNS) this->dns_[this->dns_count_++] = r;
  this->cnt_res_ = this->new_res_.size();
  this->cnt_leases_ = this->new_leases_.size();
  this->new_res_.clear(); this->new_res_.shrink_to_fit();
  this->new_leases_.clear(); this->new_leases_.shrink_to_fit();
  this->new_dns_.clear(); this->new_dns_.shrink_to_fit();
  this->last_sync_ok_ = true;
  this->last_sync_epoch_ = ::time(nullptr);
  this->save_snapshot_();
  ESP_LOGI(TAG, "sync ok: %d res, %d leases, %d dns", this->cnt_res_, this->cnt_leases_, this->dns_count_);
  if (this->sync_status_ts_) {
    char b[64];
    snprintf(b, sizeof b, "ok: %d res, %d leases, %d dns", this->cnt_res_, this->cnt_leases_, this->dns_count_);
    this->sync_status_ts_->publish_state(b);
  }
  this->publish_last_sync_();
  this->publish_status_();
}

}  // namespace esphome::dhcp_failover
