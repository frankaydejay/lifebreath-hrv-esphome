#pragma once
#include "esphome.h"
#include "driver/uart.h"
#include "soc/uart_reg.h"

// ═══════════════════════════════════════════════════════════════════════════
//  LifeBreath HRV RNC6-ES  <->  ESPHome (M5Stack Atom S3 Lite)
//
//  Protocol-aware proxy sitting in the middle of the proprietary 2000-baud
//  open-collector bus between the HRV and its DXPL-03 wall panel.
//
//   * GPIO5 (RX, 6N137 #1) : listen to the HRV, decode mode + fan speed
//   * GPIO6 (TX, PC817 #1) : send the active command to the HRV, anchored to
//                            its 0xC7 byte + 5.5 ms, repeated every cycle
//   * GPIO8 (TX, PC817 #2) : REGENERATE the 12-byte cycle toward the panel
//   * GPIO7 (RX, 6N137 #2) : listen to the panel AND the bathroom timers
//
//  Protocol:
//    HRV cycle : C7 67 DF 7B [TAIL] 40 30 2B [B1] [B2] F7 EF  (21 ms/byte)
//    Command   : Fresh = 0x7F - 8*speed | Recirc = 0x3F - 8*speed | Off = 0xBF
//
//  SELF-ECHO WARNING: the downstream bus is a SINGLE wire, so everything GPIO8
//  transmits is heard back on GPIO7. Note that the 2nd byte of the cycle (0x67)
//  is ALSO a valid "Fresh 3" command byte.
//    -> panel commands : filtered by TIME WINDOW relative to our own 0xC7
//    -> timer bytes    : filtered by ECHO EXCLUSION (their position drifts;
//       their own period measures 246-258 ms against our 252 ms cycle)
//
//  BATHROOM TIMER BOOST: cutting the data wire disconnects the timers from the
//  HRV, so their boost function is lost at the hardware level. It is restored
//  here in software: while the timer signal persists, the ESP32 forces a boost
//  setpoint that outranks both Home Assistant and the wall panel. The timer
//  handles its own countdown (verified: a 20 min timer releases at T+20 min).
// ═══════════════════════════════════════════════════════════════════════════

// ── Upstream UART: HRV bus ────────────────────────────────────────────────
#define HRV_UART_NUM    UART_NUM_1
#define HRV_RX_PIN      GPIO_NUM_5
#define HRV_TX_PIN      GPIO_NUM_6

// ── Downstream UART: DXPL-03 panel + bathroom timers ──────────────────────
#define LCD_UART_NUM    UART_NUM_2
#define LCD_TX_PIN      GPIO_NUM_8
#define LCD_RX_PIN      GPIO_NUM_7

#define HRV_BAUD              2000
#define LCD_BYTE_INTERVAL_US  21000   // measured on the real unit
#define HRV_CYCLE_LEN         12
#define HRV_ANCHOR_LEN        8

// Acceptance window for a panel command, measured from the START of OUR 0xC7
// write on GPIO8. At 2000 baud one byte takes 5 ms.
#define LCD_REPLY_WIN_MIN_US  6000
#define LCD_REPLY_WIN_MAX_US  19000

#define HRV_RESPONSE_DELAY_US 5500    // reply delay after the HRV's 0xC7
#define PANEL_TIMEOUT_MS      10000   // panel command considered stale after
#define TIMER_TIMEOUT_MS      5000    // timer byte considered stale after
#define HA_ARM_DELAY_MS       15000   // ignore HA setpoints for this long after boot
#define UI_GRACE_MS           15000   // don't overwrite the UI after a manual change

// Boost debounce (one timer sample per ~250 ms bus cycle)
#define BOOST_ENGAGE_SAMPLES  4       // ~1.0 s of active signal to engage
#define BOOST_RELEASE_SAMPLES 8       // ~2.0 s of idle signal to release

enum HrvMode { MODE_UNKNOWN = 0, MODE_FRESH, MODE_RECIRC };

// ── State lookup table, established empirically across all 12 states ───────
struct FrameLookup { uint8_t b1, b2, b3; HrvMode mode; uint8_t fan; };
static const FrameLookup FRAME_LOOKUP[] = {
  {0xAF, 0x9F, 0x5E, MODE_FRESH,  0},
  {0xAF, 0x99, 0x5E, MODE_FRESH,  1},
  {0xAF, 0x95, 0x5E, MODE_FRESH,  2},
  {0xAF, 0x91, 0x5E, MODE_FRESH,  3},
  {0xAE, 0x9D, 0x5E, MODE_FRESH,  4},
  {0xAE, 0x99, 0x5E, MODE_FRESH,  5},
  {0xAF, 0x9F, 0x5D, MODE_RECIRC, 0},
  {0xAF, 0x9B, 0x5D, MODE_RECIRC, 1},
  {0xAF, 0x97, 0x5D, MODE_RECIRC, 2},
  {0xAF, 0x93, 0x5D, MODE_RECIRC, 3},
  {0xAE, 0x9F, 0x5D, MODE_RECIRC, 4},
  {0xAE, 0x9B, 0x5D, MODE_RECIRC, 5},
};

// ── Bathroom timers ───────────────────────────────────────────────────────
//  The timers do NOT transmit a byte. They pull the bus low for a variable
//  amount of time, and the UART frames that pulse into a byte whose trailing
//  zero count reflects the width:
//     width ≈ (zero_bits + 1) × 0.5 ms   (start bit included, 2000 baud)
//
//  Measured: 0xFE = 1.0 ms (idle) / 0x00 = 4.5 ms (a timer is running)
//  The 20, 40 and 60 min timers ALL produce 0x00 — the byte saturates, so the
//  durations are indistinguishable. Two timers at once also give 0x00
//  (wired-OR). The signal is therefore purely binary, and duration is derived
//  from how long the active state PERSISTS.
struct TimerLookup { uint8_t val; const char *label; };
static const TimerLookup TIMER_LOOKUP[] = {
  {0xFE, "None"},
  {0x00, "Active"},   // 20, 40 or 60 min — indistinguishable
};
#define TIMER_IDLE_BYTE 0xFE

// ── Command byte encoding / decoding ──────────────────────────────────────
static inline uint8_t hrv_tx_command(HrvMode mode, uint8_t fan) {
  if (mode == MODE_UNKNOWN || fan == 0) return 0xBF;
  if (mode == MODE_FRESH)  return (uint8_t)(0x7F - 8 * fan);
  if (mode == MODE_RECIRC) return (uint8_t)(0x3F - 8 * fan);
  return 0xBF;
}

static inline bool hrv_decode_command(uint8_t cmd, HrvMode *mode, uint8_t *fan) {
  if (cmd == 0xBF) { *mode = MODE_FRESH; *fan = 0; return true; }
  for (uint8_t f = 1; f <= 5; f++) {
    if (cmd == (uint8_t)(0x7F - 8 * f)) { *mode = MODE_FRESH;  *fan = f; return true; }
    if (cmd == (uint8_t)(0x3F - 8 * f)) { *mode = MODE_RECIRC; *fan = f; return true; }
  }
  return false;
}

// Count contiguous zero bits from the LSB -> timer pulse width
static inline uint8_t timer_low_bits(uint8_t v) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < 8; i++) { if (v & (1 << i)) break; n++; }
  return n;
}

// ── Build the 12-byte cycle sent to the wall panel ────────────────────────
static inline bool hrv_build_cycle(HrvMode mode, uint8_t fan, uint8_t *out12) {
  for (auto &f : FRAME_LOOKUP) {
    if (f.mode == mode && f.fan == fan) {
      out12[0]  = 0xC7; out12[1] = 0x67; out12[2] = 0xDF; out12[3] = 0x7B;
      out12[4]  = f.b3;                                   // TAIL
      out12[5]  = 0x40; out12[6] = 0x30; out12[7] = 0x2B;
      out12[8]  = f.b1; out12[9] = f.b2;
      out12[10] = 0xF7; out12[11] = 0xEF;
      return true;
    }
  }
  return false;
}

// ── Panel transmitter: hardware timer, one byte every 21 ms ───────────────
static uint8_t  lcd_tx_frame_[HRV_CYCLE_LEN] = {0};
static uint8_t  lcd_tx_next_[HRV_CYCLE_LEN]  = {0};
static volatile bool     lcd_tx_next_ready_  = false;
static volatile bool     lcd_tx_enabled_     = false;
static volatile bool     lcd_tx_allowed_     = true;
static volatile uint8_t  lcd_tx_pos_         = 0;
static volatile uint32_t lcd_c7_sent_us_     = 0;
static volatile uint32_t lcd_tx_cycle_count_ = 0;
static hw_timer_t *lcd_tx_timer = nullptr;

// Is a byte received on GPIO7 an echo of one of OUR cycle bytes?
static inline bool lcd_is_echo_byte(uint8_t v) {
  for (int i = 0; i < HRV_CYCLE_LEN; i++)
    if (lcd_tx_frame_[i] == v) return true;
  return false;
}

void IRAM_ATTR lcd_tx_isr() {
  if (!lcd_tx_enabled_ || !lcd_tx_allowed_) return;

  if (lcd_tx_pos_ == 0) {
    // Swap buffers at cycle start ONLY, so a frame is never half-old/half-new
    if (lcd_tx_next_ready_) {
      for (int i = 0; i < HRV_CYCLE_LEN; i++) lcd_tx_frame_[i] = lcd_tx_next_[i];
      lcd_tx_next_ready_ = false;
    }
    lcd_c7_sent_us_ = micros();   // anchor for echo filtering on GPIO7
    lcd_tx_cycle_count_++;
  }

  WRITE_PERI_REG(UART_FIFO_REG(LCD_UART_NUM), lcd_tx_frame_[lcd_tx_pos_]);
  lcd_tx_pos_ = (lcd_tx_pos_ + 1) % HRV_CYCLE_LEN;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main component
// ═══════════════════════════════════════════════════════════════════════════
class LifeBreathHrv {
 public:
  // SRC_BOOST outranks the other two sources
  enum ControlSource { SRC_PANEL = 0, SRC_HA = 1, SRC_BOOST = 2 };

  // ── Actual HRV state (source of truth) ──
  HrvMode  mode_ = MODE_UNKNOWN;
  uint8_t  fan_speed_ = 0;
  bool     valid_ = false;
  uint32_t last_frame_ms_ = 0;
  uint32_t frame_count_ = 0;
  bool     special_flag_ = false;
  uint8_t  special_b1_ = 0, special_b2_ = 0, special_b3_ = 0;

  // ── Control arbitration ──
  volatile ControlSource control_source_ = SRC_PANEL;
  ControlSource source_before_boost_ = SRC_PANEL;   // where to return after boost
  bool     panel_override_enabled_ = false;
  bool     ha_armed_    = false;
  bool     select_sync_ = false;
  bool     tx_enabled_  = true;
  bool     lcd_rx_enabled_ = true;

  // ── Home Assistant UI mirrors ──
  // Recent ESPHome versions removed the public Select::state member, so the
  // component tracks what the UI is showing. These must match the
  // `initial_option` / `initial_value` values in the YAML.
  std::string ui_mode_  = "Fresh";
  uint8_t     ui_speed_ = 0;
  uint32_t    ui_touched_ms_ = 0;

  // ── Home Assistant setpoint ──
  HrvMode  ha_mode_ = MODE_FRESH;
  uint8_t  ha_fan_  = 0;
  bool     ha_cmd_valid_ = false;

  // ── Command captured from the wall panel ──
  HrvMode  panel_mode_ = MODE_FRESH;
  uint8_t  panel_fan_  = 0;
  uint8_t  panel_cmd_byte_ = 0xBF;
  bool     panel_cmd_valid_ = false;
  uint32_t panel_last_seen_ms_ = 0;
  uint32_t panel_rx_count_ = 0;
  uint8_t  panel_prev_cmd_ = 0x00;
  volatile bool panel_changed_ = false;

  // ── Bathroom timers ──
  volatile uint8_t  timer_byte_       = TIMER_IDLE_BYTE;
  volatile uint16_t timer_active_run_ = 0;   // consecutive active samples
  volatile uint16_t timer_idle_run_   = 0;   // consecutive idle samples
  uint8_t  timer_prev_byte_    = TIMER_IDLE_BYTE;
  uint32_t timer_rx_count_     = 0;
  uint32_t timer_change_count_ = 0;
  uint32_t timer_last_seen_ms_ = 0;
  uint32_t timer_last_dt_us_   = 0;
  uint8_t  timer_vals_[8]      = {0};
  uint32_t timer_counts_[8]    = {0};
  uint8_t  timer_vals_n_       = 0;

  // ── Bathroom timer boost ──
  // These three values are the FIRST-BOOT defaults. The matching YAML entities
  // use restore_value: true (and therefore cannot declare initial_value /
  // initial_option, which ESPHome forbids combining). On later boots ESPHome
  // republishes the saved values, firing on_value and resyncing these members.
  bool     boost_enabled_  = true;
  HrvMode  boost_mode_     = MODE_FRESH;
  uint8_t  boost_fan_      = 5;         // clamped to 1-5 in YAML: never 0
  uint16_t boost_max_min_  = 90;        // safety cap; 0 = unlimited
  bool     boost_active_   = false;
  bool     boost_lockout_  = false;     // set after the safety cap trips
  uint32_t boost_started_ms_ = 0;
  uint32_t boost_count_      = 0;
  float    boost_last_min_   = NAN;     // previous boost length -> which timer?

  // ── Raw diagnostics ──
  static const int RAW_DUMP_SIZE = 128;

  volatile uint32_t raw_rx_count_ = 0;
  volatile uint8_t  last_raw_byte_ = 0;
  uint8_t  raw_dump_buf_[RAW_DUMP_SIZE] = {0};
  uint8_t  raw_dump_pos_ = 0;
  uint32_t raw_dump_dt_us_[RAW_DUMP_SIZE] = {0};
  uint32_t last_raw_byte_us_ = 0;

  volatile uint32_t raw_rx_count_lcd_ = 0;
  volatile uint8_t  last_raw_byte_lcd_ = 0;
  uint8_t  raw_dump_buf_lcd_[RAW_DUMP_SIZE] = {0};
  uint8_t  raw_dump_pos_lcd_ = 0;
  uint32_t raw_dump_dt_us_lcd_[RAW_DUMP_SIZE] = {0};
  uint32_t last_raw_byte_us_lcd_ = 0;

  // ── TX toward the HRV ──
  volatile uint8_t  tx_command_byte_ = 0x00;
  volatile bool     tx_pending_ = false;
  volatile uint32_t tx_anchor_us_ = 0;
  volatile uint32_t tx_send_count_ = 0;

  uint32_t boot_ms_ = 0;

  // ═════════════════════════════════════════════════════════════════════
  void setup() {
    boot_ms_ = millis();

    uart_config_t cfg = {
      .baud_rate  = HRV_BAUD,
      .data_bits  = UART_DATA_8_BITS,
      .parity     = UART_PARITY_DISABLE,
      .stop_bits  = UART_STOP_BITS_1,
      .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
    };

    // Upstream UART (HRV)
    uart_param_config(HRV_UART_NUM, &cfg);
    uart_set_pin(HRV_UART_NUM, HRV_TX_PIN, HRV_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(HRV_UART_NUM, 256, 0, 32, &hrv_queue_, 0);
    uart_set_line_inverse(HRV_UART_NUM, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV);
    uart_set_rx_full_threshold(HRV_UART_NUM, 1);
    uart_set_rx_timeout(HRV_UART_NUM, 2);

    // Downstream UART (panel + timers). tx_buffer_size = 0 -> direct FIFO writes.
    uart_param_config(LCD_UART_NUM, &cfg);
    uart_set_pin(LCD_UART_NUM, LCD_TX_PIN, LCD_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(LCD_UART_NUM, 256, 0, 32, &lcd_queue_, 0);
    uart_set_line_inverse(LCD_UART_NUM, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV);
    uart_set_rx_full_threshold(LCD_UART_NUM, 1);
    uart_set_rx_timeout(LCD_UART_NUM, 2);

    lcd_tx_timer = timerBegin(1000000);
    timerAttachInterrupt(lcd_tx_timer, &lcd_tx_isr);
    timerAlarm(lcd_tx_timer, LCD_BYTE_INTERVAL_US, true, 0);

    xTaskCreatePinnedToCore(hrv_task_trampoline, "hrv_task", 4096, this, 19, nullptr, 1);
  }

  void loop() {
    if (!ha_armed_ && (millis() - boot_ms_) > HA_ARM_DELAY_MS) ha_armed_ = true;

    if (panel_cmd_valid_ && (millis() - panel_last_seen_ms_) > PANEL_TIMEOUT_MS)
      panel_cmd_valid_ = false;

    if (panel_changed_) {
      panel_changed_ = false;
      if (panel_override_enabled_) {
        // During a boost we don't surrender priority; we just remember that the
        // panel should regain control once the boost ends.
        if (boost_active_)                      source_before_boost_ = SRC_PANEL;
        else if (control_source_ == SRC_HA)     control_source_      = SRC_PANEL;
      }
    }

    update_boost_();
  }

  // ═════════════ Boost state machine ═════════════
  void update_boost_() {
    uint32_t now = millis();

    // 1) No timer byte for 5 s: the downstream bus is gone, we can't assert
    //    anything. Fail-safe release so we never get stuck boosting.
    if (!timer_seen()) {
      if (boost_active_) end_boost_();
      boost_lockout_ = false;
      return;
    }

    // 2) Feature disabled by the user
    if (!boost_enabled_) {
      if (boost_active_) end_boost_();
      return;
    }

    if (!boost_active_) {
      // Engage: active signal stable for ~1 s, and no lockout in effect
      if (!boost_lockout_ && timer_active_run_ >= BOOST_ENGAGE_SAMPLES)
        start_boost_();
      // The lockout only clears once the signal has firmly gone idle
      else if (timer_idle_run_ >= BOOST_RELEASE_SAMPLES)
        boost_lockout_ = false;
    } else {
      // Normal release: idle signal stable for ~2 s
      if (timer_idle_run_ >= BOOST_RELEASE_SAMPLES) {
        end_boost_();
      }
      // Safety cap: abnormally long signal (stuck timer, shorted wire, ...)
      else if (boost_max_min_ > 0 &&
               (now - boost_started_ms_) > (uint32_t) boost_max_min_ * 60000UL) {
        end_boost_();
        boost_lockout_ = true;   // don't re-engage while the signal persists
      }
    }
  }

  void start_boost_() {
    source_before_boost_ = (control_source_ == SRC_HA) ? SRC_HA : SRC_PANEL;
    control_source_   = SRC_BOOST;
    boost_active_     = true;
    boost_started_ms_ = millis();
    boost_count_++;
  }

  void end_boost_() {
    boost_last_min_ = (millis() - boost_started_ms_) / 60000.0f;
    control_source_ = source_before_boost_;
    boost_active_   = false;
  }

  // ═════════════ API called from the YAML ═════════════
  // "HA is in control" in the background-arbitration sense; a boost is
  // temporary and outranks it without changing this answer.
  bool ha_has_control() const {
    return (boost_active_ ? source_before_boost_ : control_source_) == SRC_HA;
  }

  void set_source(bool ha) {
    ControlSource target;
    if (ha) {
      if (!ha_armed_) {
        // Boot / power-failure recovery: adopt the HRV's current state rather
        // than whatever (possibly "Off") value the UI happens to show.
        if (valid_) { ha_mode_ = mode_; ha_fan_ = fan_speed_; ha_cmd_valid_ = true; }
      } else {
        ha_mode_ = (ui_mode_ == "Recirc") ? MODE_RECIRC : MODE_FRESH;
        ha_fan_  = ui_speed_;
        ha_cmd_valid_ = true;
      }
      target = SRC_HA;
    } else {
      target = SRC_PANEL;
    }

    // An active boost keeps priority; we only record where to return.
    if (boost_active_) source_before_boost_ = target;
    else               control_source_      = target;
  }

  void set_panel_override(bool en) { panel_override_enabled_ = en; }
  void set_tx_enabled(bool en)     { tx_enabled_ = en; }
  void set_lcd_rx_enabled(bool en) { lcd_rx_enabled_ = en; }
  void set_lcd_tx_enabled(bool en) { lcd_tx_allowed_ = en; }
  void set_boost_enabled(bool en)  { boost_enabled_ = en; }

  void set_ha_command(HrvMode mode, uint8_t fan) {
    if (!ha_armed_) return;
    if (fan > 5) fan = 5;
    ha_mode_ = mode; ha_fan_ = fan; ha_cmd_valid_ = true;
  }

  // Called when the user touches either the mode select or the speed number.
  // The setpoint is ALWAYS rebuilt from both mirrors, so changing only one
  // control can never produce an inconsistent mode/speed pair.
  void apply_ui_command() {
    ui_touched_ms_ = millis();
    HrvMode m = (ui_mode_ == "Recirc") ? MODE_RECIRC : MODE_FRESH;
    set_ha_command(m, ui_speed_);
  }

  // During the grace period the automatic sync won't overwrite the UI, which
  // lets you set mode and speed BEFORE flipping the control switch to HA.
  bool ui_recently_touched() const {
    return ui_touched_ms_ != 0 && (millis() - ui_touched_ms_) < UI_GRACE_MS;
  }

  // ── Text representations ──
  static std::string option_str(HrvMode m, uint8_t f) {
    if (f == 0 || m == MODE_UNKNOWN) return std::string("Off");
    char b[16];
    snprintf(b, sizeof(b), "%s %u", (m == MODE_FRESH) ? "Fresh" : "Recirc",
             (unsigned) f);
    return std::string(b);
  }

  std::string mode_option_str() const {
    return std::string(mode_ == MODE_RECIRC ? "Recirc" : "Fresh");
  }
  std::string state_option_str() { return option_str(mode_, fan_speed_); }
  std::string ha_option_str()    { return ha_cmd_valid_    ? option_str(ha_mode_, ha_fan_)
                                                           : std::string("—"); }
  std::string panel_option_str() { return panel_cmd_valid_ ? option_str(panel_mode_, panel_fan_)
                                                           : std::string("—"); }
  std::string boost_option_str() { return option_str(boost_mode_, boost_fan_); }

  std::string source_str() const {
    if (boost_active_) return std::string("Timer boost");
    return std::string(control_source_ == SRC_HA ? "Home Assistant"
                                                 : "Wall panel");
  }

  const char* mode_str() const {
    switch (mode_) {
      case MODE_FRESH:  return "Fresh";
      case MODE_RECIRC: return "Recirc";
      default:          return "Unknown";
    }
  }

  std::string lcd_frame_hex() {
    char b[HRV_CYCLE_LEN * 3 + 1]; char *p = b;
    for (int i = 0; i < HRV_CYCLE_LEN; i++) p += sprintf(p, "%02X ", lcd_tx_frame_[i]);
    return std::string(b);
  }

  // A boost short-circuits normal arbitration
  uint8_t active_command_byte() {
    if (boost_active_) {
      uint8_t f = boost_fan_;
      if (f < 1) f = 1;                 // a boost must never switch the HRV off
      if (f > 5) f = 5;
      return hrv_tx_command(boost_mode_, f);
    }
    if (control_source_ == SRC_PANEL) {
      if (panel_cmd_valid_) return hrv_tx_command(panel_mode_, panel_fan_);
    } else {
      if (ha_cmd_valid_)    return hrv_tx_command(ha_mode_, ha_fan_);
    }
    if (valid_) return hrv_tx_command(mode_, fan_speed_);  // hold current state
    return 0x00;
  }

  uint32_t lcd_cycle_count() { return lcd_tx_cycle_count_; }
  bool     lcd_tx_running()  { return lcd_tx_enabled_ && lcd_tx_allowed_; }

  // ── Bathroom timers ──
  bool timer_seen() const {
    return timer_rx_count_ > 0 && (millis() - timer_last_seen_ms_) < TIMER_TIMEOUT_MS;
  }
  bool timer_active() const {
    return timer_seen() && ((uint8_t) timer_byte_) != TIMER_IDLE_BYTE;
  }
  std::string timer_hex_str() {
    char b[8]; snprintf(b, sizeof(b), "0x%02X", (uint8_t) timer_byte_);
    return timer_seen() ? std::string(b) : std::string("—");
  }
  std::string timer_state_str() {
    if (!timer_seen()) return std::string("—");
    uint8_t v = (uint8_t) timer_byte_;
    for (auto &t : TIMER_LOOKUP) if (t.val == v) return std::string(t.label);
    char b[40];
    snprintf(b, sizeof(b), "Unknown 0x%02X (%u bits, %.1f ms)",
             v, timer_low_bits(v), (timer_low_bits(v) + 1) * 0.5f);
    return std::string(b);
  }
  float timer_pulse_ms() {
    if (!timer_seen()) return NAN;
    return (timer_low_bits((uint8_t) timer_byte_) + 1) * 0.5f;
  }
  // Histogram of distinct values seen; handy when porting to another model.
  std::string timer_table_str() {
    if (timer_vals_n_ == 0) return std::string("—");
    std::string s; char b[24];
    for (uint8_t i = 0; i < timer_vals_n_; i++) {
      snprintf(b, sizeof(b), "%02X:%lu ", timer_vals_[i],
               (unsigned long) timer_counts_[i]);
      s += b;
    }
    return s;
  }

  // ── Boost accessors for the UI ──
  float boost_elapsed_min() const {
    if (!boost_active_) return NAN;
    return (millis() - boost_started_ms_) / 60000.0f;
  }
  std::string boost_state_str() const {
    if (!boost_enabled_) return std::string("Disabled");
    if (boost_lockout_)  return std::string("Locked out (max duration exceeded)");
    if (boost_active_)   return std::string("Active");
    return std::string("Standby");
  }

  // ── Diagnostic dumps ──
  std::string raw_dump_hex() {
    char buf[RAW_DUMP_SIZE * 3 + 1]; char *p = buf;
    for (int i = 0; i < RAW_DUMP_SIZE; i++)
      p += sprintf(p, "%02X ", raw_dump_buf_[(raw_dump_pos_ + i) % RAW_DUMP_SIZE]);
    return std::string(buf);
  }
  std::string raw_dump_timing_str() {
    std::string s; char b[12];
    for (int i = 0; i < RAW_DUMP_SIZE; i++) {
      snprintf(b, sizeof(b), "%lu,",
               (unsigned long) raw_dump_dt_us_[(raw_dump_pos_ + i) % RAW_DUMP_SIZE]);
      s += b;
    }
    return s;
  }
  std::string raw_dump_hex_lcd() {
    char buf[RAW_DUMP_SIZE * 3 + 1]; char *p = buf;
    for (int i = 0; i < RAW_DUMP_SIZE; i++)
      p += sprintf(p, "%02X ", raw_dump_buf_lcd_[(raw_dump_pos_lcd_ + i) % RAW_DUMP_SIZE]);
    return std::string(buf);
  }
  std::string raw_dump_timing_str_lcd() {
    std::string s; char b[12];
    for (int i = 0; i < RAW_DUMP_SIZE; i++) {
      snprintf(b, sizeof(b), "%lu,",
               (unsigned long) raw_dump_dt_us_lcd_[(raw_dump_pos_lcd_ + i) % RAW_DUMP_SIZE]);
      s += b;
    }
    return s;
  }

 protected:
  QueueHandle_t hrv_queue_;
  QueueHandle_t lcd_queue_;
  uint8_t frame_buf_[HRV_ANCHOR_LEN] = {0};

  void record_raw_byte_(uint8_t byte) {
    uint32_t now_us = micros();
    raw_dump_dt_us_[raw_dump_pos_] = now_us - last_raw_byte_us_;
    last_raw_byte_us_ = now_us;
    raw_rx_count_++; last_raw_byte_ = byte;
    raw_dump_buf_[raw_dump_pos_] = byte;
    raw_dump_pos_ = (raw_dump_pos_ + 1) % RAW_DUMP_SIZE;
  }
  void record_raw_byte_lcd_(uint8_t byte) {
    uint32_t now_us = micros();
    raw_dump_dt_us_lcd_[raw_dump_pos_lcd_] = now_us - last_raw_byte_us_lcd_;
    last_raw_byte_us_lcd_ = now_us;
    raw_rx_count_lcd_++; last_raw_byte_lcd_ = byte;
    raw_dump_buf_lcd_[raw_dump_pos_lcd_] = byte;
    raw_dump_pos_lcd_ = (raw_dump_pos_lcd_ + 1) % RAW_DUMP_SIZE;
  }

  void record_timer_byte_(uint8_t v, uint32_t dt_us) {
    timer_rx_count_++;
    timer_last_seen_ms_ = millis();
    timer_last_dt_us_   = dt_us;

    // Consecutive-sample counters feed the boost debounce.
    // One sample arrives per ~250 ms bus cycle.
    if (v == TIMER_IDLE_BYTE) {
      if (timer_idle_run_ < 0xFFFF) timer_idle_run_++;
      timer_active_run_ = 0;
    } else {
      if (timer_active_run_ < 0xFFFF) timer_active_run_++;
      timer_idle_run_ = 0;
    }

    if (v != (uint8_t) timer_byte_) {
      timer_prev_byte_ = (uint8_t) timer_byte_;
      timer_byte_ = v;
      timer_change_count_++;
    }
    for (uint8_t i = 0; i < timer_vals_n_; i++)
      if (timer_vals_[i] == v) { timer_counts_[i]++; return; }
    if (timer_vals_n_ < 8) {
      timer_vals_[timer_vals_n_]   = v;
      timer_counts_[timer_vals_n_] = 1;
      timer_vals_n_++;
    }
  }

  static bool lookup_speed_(uint8_t b1, uint8_t b2, uint8_t b3,
                            HrvMode *mode, uint8_t *fan) {
    for (auto &f : FRAME_LOOKUP)
      if (f.b1 == b1 && f.b2 == b2 && f.b3 == b3) {
        *mode = f.mode; *fan = f.fan; return true;
      }
    return false;
  }

  void refresh_lcd_frame_() {
    uint8_t tmp[HRV_CYCLE_LEN];
    if (!hrv_build_cycle(mode_, fan_speed_, tmp)) return;
    lcd_tx_next_ready_ = false;
    for (int i = 0; i < HRV_CYCLE_LEN; i++) lcd_tx_next_[i] = tmp[i];
    lcd_tx_next_ready_ = true;
    if (!lcd_tx_enabled_) {
      for (int i = 0; i < HRV_CYCLE_LEN; i++) lcd_tx_frame_[i] = tmp[i];
      lcd_tx_pos_ = 0;
      lcd_tx_enabled_ = true;   // only start transmitting once the state is known
    }
  }

  // ── Stream from the HRV (GPIO5) ──
  void process_byte_(uint8_t val) {
    memmove(frame_buf_, frame_buf_ + 1, HRV_ANCHOR_LEN - 1);
    frame_buf_[HRV_ANCHOR_LEN - 1] = val;

    if (val == 0xC7 && tx_enabled_) {
      uint8_t cmd = active_command_byte();
      if (cmd != 0x00) {
        tx_command_byte_ = cmd;
        tx_anchor_us_ = micros();
        tx_pending_ = true;
      }
    }

    uint8_t tail = frame_buf_[0];
    uint8_t b1   = frame_buf_[4];
    uint8_t b2   = frame_buf_[5];

    bool anchor_ok = (frame_buf_[1] == 0x40 && frame_buf_[2] == 0x30 &&
                      frame_buf_[3] == 0x2B && frame_buf_[6] == 0xF7 &&
                      frame_buf_[7] == 0xEF);
    if (!anchor_ok) return;

    HrvMode mode = MODE_UNKNOWN; uint8_t fan = 0;
    if (lookup_speed_(b1, b2, tail, &mode, &fan)) {
      bool changed = (mode != mode_ || fan != fan_speed_ || !valid_);
      if (changed) frame_count_++;
      last_frame_ms_ = millis();
      mode_ = mode; fan_speed_ = fan; valid_ = true; special_flag_ = false;
      if (changed || !lcd_tx_enabled_) refresh_lcd_frame_();
    } else {
      special_flag_ = true;
      special_b1_ = b1; special_b2_ = b2; special_b3_ = tail;
    }
  }

  // ── Stream from the downstream bus (GPIO7): panel + timers + our own echo ──
  void process_lcd_byte_(uint8_t val, uint32_t now_us) {
    if (!lcd_rx_enabled_) return;

    uint32_t dt = now_us - lcd_c7_sent_us_;
    HrvMode m; uint8_t f;
    bool is_cmd = hrv_decode_command(val, &m, &f);

    // 1) Panel command: tight time window plus a valid decode
    if (is_cmd && dt >= LCD_REPLY_WIN_MIN_US && dt <= LCD_REPLY_WIN_MAX_US) {
      panel_mode_ = m; panel_fan_ = f;
      panel_cmd_byte_ = val;
      panel_cmd_valid_ = true;
      panel_last_seen_ms_ = millis();
      panel_rx_count_++;
      if (val != panel_prev_cmd_) { panel_prev_cmd_ = val; panel_changed_ = true; }
      return;
    }

    // 2) Bathroom timers: anything that is NOT an echo of our own frame and is
    //    not a valid command inside its window. No time window here: the timers
    //    run on their own clock (246-258 ms vs our 252 ms cycle), so their
    //    position drifts across the frame.
    if (!lcd_is_echo_byte(val) && (!is_cmd || dt > LCD_REPLY_WIN_MAX_US)) {
      record_timer_byte_(val, dt);
      return;
    }

    // 3) Everything else is our own echo -> ignored
  }

  // ── Transmit to the HRV, 5.5 ms after its 0xC7 ──
  void tx_send_() {
    uint8_t cmd = tx_command_byte_;
    while ((micros() - tx_anchor_us_) < HRV_RESPONSE_DELAY_US) { /* spin */ }
    uart_write_bytes(HRV_UART_NUM, (const char *) &cmd, 1);
    tx_send_count_++;
    tx_pending_ = false;
  }

  static void hrv_task_trampoline(void *param) {
    static_cast<LifeBreathHrv *>(param)->hrv_task_();
  }

  void hrv_task_() {
    for (;;) {
      // 1) Upstream: HRV
      uart_event_t event;
      while (xQueueReceive(hrv_queue_, &event, 0) == pdTRUE) { /* unused */ }

      uint8_t byte;
      if (uart_read_bytes(HRV_UART_NUM, &byte, 1, pdMS_TO_TICKS(1)) == 1) {
        record_raw_byte_(byte);
        process_byte_(byte);
        uint8_t rx_buf[31];
        int len = uart_read_bytes(HRV_UART_NUM, rx_buf, sizeof(rx_buf), 0);
        for (int i = 0; i < len; i++) { record_raw_byte_(rx_buf[i]); process_byte_(rx_buf[i]); }
      }

      // 2) Reply to the HRV (spins up to 5.5 ms; bytes stay in the FIFO)
      if (tx_pending_) tx_send_();

      // 3) Downstream: wall panel + timers
      while (xQueueReceive(lcd_queue_, &event, 0) == pdTRUE) { /* unused */ }
      uint8_t lb;
      while (uart_read_bytes(LCD_UART_NUM, &lb, 1, 0) == 1) {
        uint32_t t = micros();
        record_raw_byte_lcd_(lb);
        process_lcd_byte_(lb, t);
      }
    }
  }
};

static LifeBreathHrv global_hrv_instance;
static LifeBreathHrv *global_hrv = &global_hrv_instance;
