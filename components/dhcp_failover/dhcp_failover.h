#pragma once
// DHCP/DNS failover for Pi-hole. Passive (silent on UDP 67/53) while Pi-hole passes its
// health probe (a DNS query sent straight to Pi-hole by default, or a DHCPDISCOVER in
// probe_mode: dhcp); active (serves DHCP + DNS) after `fail_threshold` failed probes.
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome::dhcp_failover {

static const int MAX_ENTRIES = 300;  // reservations + lease hints + our leases
static const int MAX_DNS = 128;      // Pi-hole local DNS records

enum Kind : uint8_t { K_FREE = 0, K_RES, K_PIHOLE, K_OURS, K_DECLINED };
struct Entry {
  uint8_t mac[6];
  uint8_t kind;
  uint8_t pad;
  uint32_t ip;
  uint32_t expires;  // uptime seconds (K_OURS / K_DECLINED)
  char name[33];
};
struct DnsRec {
  uint32_t ip;
  char name[64];
};
struct Pending {
  bool used;
  uint8_t tries;
  uint16_t new_id, orig_id, len;
  uint32_t sent_ms;
  uint8_t client[16];  // sockaddr_in
  uint8_t q[512];
};

enum Override : uint8_t { OV_AUTO = 0, OV_FORCE_ACTIVE, OV_FORCE_PASSIVE };
enum ProbeMode : uint8_t { PROBE_DNS = 0, PROBE_DHCP };

class DhcpFailover;

class OverrideSelect : public select::Select, public Parented<DhcpFailover> {
 protected:
  void control(size_t index) override;
};
class SyncButton : public button::Button, public Parented<DhcpFailover> {
 protected:
  void press_action() override;
};

class DhcpFailover : public Component {
 public:
  // config
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_pihole(uint32_t ip, uint16_t port, const std::string &pw) {
    this->pihole_ip_ = ip; this->pihole_port_ = port; this->pihole_password_ = pw;
  }
  void set_network(uint32_t self, uint32_t gw, uint32_t mask) { this->self_ip_ = self; this->gateway_ = gw; this->netmask_ = mask; }
  void set_pool(uint32_t a, uint32_t b) { this->pool_start_ = a; this->pool_end_ = b; }
  void set_lease_secs(uint32_t s) { this->lease_secs_ = s; }
  void set_domain(const std::string &d) { this->domain_ = d; }
  void set_upstream(uint32_t a, uint32_t b) { this->up1_ = a; this->up2_ = b; }
  void set_probe(uint32_t min_ms, uint32_t max_ms, uint8_t fails, uint8_t oks) {
    this->probe_min_ms_ = min_ms; this->probe_max_ms_ = max_ms < min_ms ? min_ms : max_ms; this->fail_threshold_ = fails; this->recover_threshold_ = oks;
  }
  void set_probe_retry_ms(uint32_t ms) { this->probe_retry_ms_ = ms; }
  void set_probe_mode(ProbeMode m) { this->probe_mode_ = m; }
  void set_probe_dns(uint32_t server, const std::string &name, uint32_t timeout_ms) {
    this->probe_dns_server_ = server; this->probe_dns_name_ = name; this->probe_dns_timeout_ms_ = timeout_ms;
  }
  void set_sync_time(uint8_t h, uint8_t m) { this->sync_hour_ = h; this->sync_minute_ = m; }
  void set_dns_ttl(uint32_t t) { this->dns_ttl_ = t; }
  // entities
  void set_mode_sensor(text_sensor::TextSensor *s) { this->mode_ts_ = s; }
  void set_sync_status_sensor(text_sensor::TextSensor *s) { this->sync_status_ts_ = s; }
  void set_last_sync_sensor(text_sensor::TextSensor *s) { this->last_sync_ts_ = s; }
  void set_healthy_sensor(binary_sensor::BinarySensor *s) { this->healthy_bs_ = s; }
  void set_reservation_sensor(sensor::Sensor *s) { this->res_s_ = s; }
  void set_lease_sensor(sensor::Sensor *s) { this->lease_s_ = s; }
  void set_override_select(OverrideSelect *s) { this->override_sel_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }
  void on_shutdown() override;

  void set_override(uint8_t o);
  void request_sync() { this->sync_requested_ = true; }

 protected:
  // mode
  void set_active_(bool on, const char *why);
  void publish_status_();
  void publish_last_sync_();
  // helpers
  bool in_subnet_(uint32_t ip) const;
  bool reserved_ip_(uint32_t ip) const;
  bool live_(const Entry &e) const;
  Entry *find_mac_(const uint8_t *mac, uint8_t kind);
  Entry *ip_owner_(uint32_t ip);
  bool usable_for_(uint32_t ip, const uint8_t *mac);
  uint32_t choose_ip_(const uint8_t *mac);
  Entry *alloc_slot_();
  Entry *set_ours_(const uint8_t *mac, uint32_t ip, const char *name);
  void release_ours_(const uint8_t *mac);
  void mark_declined_(uint32_t ip);
  bool name_eq_(const char *a, const char *lname) const;
  uint32_t lookup_name_(const char *lname);
  const char *lookup_ip_(uint32_t ip);
  int count_live_leases_();
  // snapshot (NVS)
  void save_snapshot_();
  void load_snapshot_();
  // dhcp
  void dhcp_begin_();
  void dhcp_stop_();
  void dhcp_loop_();
  void dhcp_handle_(const uint8_t *b, int n);
  void dhcp_reply_(const uint8_t *q, uint8_t type, uint32_t yiaddr, bool with_lease);
  void send_probe_();         // DHCP mode
  void send_dns_probe_();     // DNS mode
  void probe_recv_dhcp_();
  void probe_recv_dns_();
  void probe_done_(bool ok, const char *detail);
  void probe_loop_();
  // dns
  void dns_begin_();
  void dns_stop_();
  void dns_loop_();
  void dns_handle_(uint8_t *b, int n, const void *client);
  void dns_answer_(const uint8_t *q, int qend, const void *cl, uint8_t rcode, uint16_t type, const uint8_t *rdata, int rdlen);
  void dns_send_up_(Pending &p, uint32_t ip);
  // pi-hole
  void schedule_sync_();
  static void sync_task_(void *arg);
  void sync_worker_();   // runs in its own FreeRTOS task (blocking HTTP)
  void commit_sync_();   // runs in loop()
  bool http_(const char *method, const std::string &path, const std::string &sid, const std::string &body,
             std::string &out, int &status);

  time::RealTimeClock *time_{nullptr};
  uint32_t pihole_ip_{0}, self_ip_{0}, gateway_{0}, netmask_{0}, pool_start_{0}, pool_end_{0}, up1_{0}, up2_{0};
  uint16_t pihole_port_{80};
  std::string pihole_password_, domain_{"lan"};
  uint32_t lease_secs_{600}, probe_min_ms_{60000}, probe_max_ms_{120000}, next_probe_ms_{0}, dns_ttl_{60};
  uint32_t probe_retry_ms_{5000}, probe_dns_server_{0}, probe_dns_timeout_ms_{2000};
  ProbeMode probe_mode_{PROBE_DNS};
  std::string probe_dns_name_{"proxmox1.lan"};
  uint8_t fail_threshold_{4}, recover_threshold_{3}, sync_hour_{3}, sync_minute_{30};

  text_sensor::TextSensor *mode_ts_{nullptr}, *sync_status_ts_{nullptr}, *last_sync_ts_{nullptr};
  binary_sensor::BinarySensor *healthy_bs_{nullptr};
  sensor::Sensor *res_s_{nullptr}, *lease_s_{nullptr};
  OverrideSelect *override_sel_{nullptr};
  ESPPreferenceObject override_pref_;

  Entry *tab_{nullptr};   // heap: MAX_ENTRIES
  DnsRec *dns_{nullptr};  // heap: MAX_DNS
  int dns_count_{0}, cnt_res_{0}, cnt_leases_{0};
  bool dirty_{false};
  uint32_t last_save_ms_{0};

  bool active_{false};
  uint8_t override_{OV_AUTO};
  uint32_t consec_fail_{0}, consec_ok_{0};
  bool healthy_known_{false};

  int srv_{-1}, prb_{-1}, s53_{-1}, up_{-1};
  Pending pend_[8];
  uint32_t probe_xid_{0}, probe_sent_ms_{0}, last_probe_ms_{0}, last_probe_latency_ms_{0};
  bool probe_waiting_{false}, got_offer_{false}, probe_ever_{false};
  uint8_t probe_q_[300];      // last DNS probe query (to match the echoed question)
  uint16_t probe_qlen_{0};

  bool sync_requested_{false}, boot_synced_{false}, last_sync_ok_{false};
  uint32_t last_sync_try_ms_{0}, last_status_ms_{0};
  int last_nightly_yday_{-1};
  time_t last_sync_epoch_{0};
  // sync worker hand-off
  struct Res { uint8_t mac[6]; uint32_t ip; std::string name; };
  volatile bool sync_running_{false}, sync_done_{false};
  bool sync_result_ok_{false};
  std::string sync_msg_{"never"};
  std::vector<Res> new_res_, new_leases_;
  std::vector<DnsRec> new_dns_;
};

}  // namespace esphome::dhcp_failover
