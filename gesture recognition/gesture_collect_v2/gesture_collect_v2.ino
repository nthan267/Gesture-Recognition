// gesture_collect_v5.ino
// Nesso N1 — IMU Data Collection (5 gestures)
//
// Gestures: circle, shake, swipe_h, swipe_v, tap
//
// HOW TO USE:
//   1. Flash this sketch, CLOSE Arduino Serial Monitor
//   2. Run collect.py on PC: py collect.py
//   3. Hold board still for 3s boot calibration
//   4. KEY2 = cycle gesture label
//   5. Do ONE gesture → auto-triggers → records → two beeps = done
//   6. Wait for cooldown, repeat. Aim for 100 per gesture.
//
// Features:
//   - Per-gesture sample counts tuned to gesture duration
//   - Per-gesture thresholds tuned to gesture intensity
//   - Adaptive rolling baseline (updates when not recording)
//   - Pre-trigger circular buffer 5 samples (captures wind-up)
//   - Weak capture rejection (1.2g minimum peak)
//   - Buzzer feedback (start/done/reject)
//   - Battery percentage and voltage display
//   - Stays on when unplugged (battery latch)
//   - 500ms cooldown between captures
//
// Sample counts at delay(100) = 10Hz:
//   circle=15(1.5s) shake=20(2s) swipe_h=10(1s) swipe_v=10(1s) tap=8(0.8s)
//
// Thresholds above rolling baseline:
//   circle=0.55g shake=0.55g swipe_h=0.2g swipe_v=0.2g tap=0.3g
//
// Libraries: Arduino_Nesso_N1, M5GFX, Arduino_BMI270_BMM150

#include <Arduino_Nesso_N1.h>
#include <Wire.h>
#include "Arduino_BMI270_BMM150.h"

NessoDisplay display;
NessoBattery battery;

#define BUZZ_FREQ   4000
#define BUZZ_RES    8
#define BUZZ_VOL    31

const uint16_t COLOR_BLACK  = 0x0000;
const uint16_t COLOR_WHITE  = 0xFFFF;
const uint16_t COLOR_GREEN  = 0x07E0;
const uint16_t COLOR_RED    = 0xF800;
const uint16_t COLOR_GREY   = 0x7BEF;
const uint16_t COLOR_TEAL   = 0x0410;
const uint16_t COLOR_YELLOW = 0xFFE0;
const uint16_t COLOR_ORANGE = 0xFD20;

// ─── Gestures — alphabetical, must match Edge Impulse label names ─────────────
const char*  GESTURES[]      = {"circle", "shake", "swipe_h", "swipe_v", "tap"};
const int    NUM_GESTURES    = 5;
int          gesture_idx     = 0;

// ─── Per-gesture sample counts ────────────────────────────────────────────────
//                                circle  shake  swipe_h  swipe_v  tap
const int    SAMPLE_COUNTS[]  = {   15,    20,     10,      10,    8  };

// ─── Per-gesture trigger thresholds (above rolling baseline) ─────────────────
//                                circle  shake  swipe_h  swipe_v  tap
const float  THRESHOLDS[]     = { 0.55f, 0.55f,  0.2f,   0.2f,  0.3f };

// ─── Adaptive threshold ───────────────────────────────────────────────────────
float rollingBaseline = 1.0f;
const float ALPHA = 0.05f;

// ─── Pre-trigger circular buffer ─────────────────────────────────────────────
#define PRE_BUF_SIZE 5
struct Sample { float ax, ay, az, gx, gy, gz; };
Sample  pre_buf[PRE_BUF_SIZE];
int     pre_buf_idx  = 0;
bool    pre_buf_full = false;

// ─── State ────────────────────────────────────────────────────────────────────
bool          recording       = false;
int           sample_count    = 0;
int           target_samples  = 0;
float         max_accel_seen  = 0.0f;
unsigned long last_capture_ms = 0;
int           total_recorded  = 0;
const unsigned long COOLDOWN_MS = 500;

// ─── Button debounce ──────────────────────────────────────────────────────────
bool          btn2_last     = HIGH;
unsigned long btn2_debounce = 0;
const unsigned long DEBOUNCE_MS = 50;

// ─── Minimum peak accel to accept a capture ───────────────────────────────────
const float MIN_PEAK_ACCEL = 1.2f;

// ─── Battery display ──────────────────────────────────────────────────────────
void drawBatteryInfo() {
  float chargeLevel = battery.getChargeLevel();
  float voltage     = battery.getVoltage();

  uint16_t batColor = COLOR_GREEN;
  if (voltage < 3.3f)      batColor = COLOR_RED;
  else if (voltage < 3.7f) batColor = COLOR_YELLOW;

  display.setTextColor(batColor);
  display.setTextSize(1);
  display.setTextDatum(top_right);

  char batBuf[20];
  snprintf(batBuf, sizeof(batBuf), "%.1f%% (%.2fV)", chargeLevel, voltage);
  display.drawString(batBuf, display.width() - 5, 5);
}

// ─── Buzzer functions ─────────────────────────────────────────────────────────
void buzz_short() {
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(60); ledcWrite(BEEP_PIN, 0);
}
void buzz_done() {
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(200); ledcWrite(BEEP_PIN, 0);
  delay(80);
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(200); ledcWrite(BEEP_PIN, 0);
}
void buzz_reject() {
  for (int i = 0; i < 3; i++) {
    ledcWrite(BEEP_PIN, 32); delay(60); ledcWrite(BEEP_PIN, 0); delay(60);
  }
}

// ─── Display functions ────────────────────────────────────────────────────────
void update_display() {
  display.fillScreen(COLOR_BLACK);
  drawBatteryInfo();
  display.setTextDatum(middle_center);
  int cx = display.width() / 2;

  display.setTextColor(COLOR_GREEN);
  display.setTextSize(3);
  display.drawString(GESTURES[gesture_idx], cx, 30);

  char cfg[40];
  snprintf(cfg, sizeof(cfg), "%ds | thr:%.2fg | #%d done",
           SAMPLE_COUNTS[gesture_idx] / 10,
           rollingBaseline + THRESHOLDS[gesture_idx],
           total_recorded);
  display.setTextColor(COLOR_TEAL);
  display.setTextSize(1);
  display.drawString(cfg, cx, 60);

  if (recording) {
    char buf[24];
    snprintf(buf, sizeof(buf), "REC  %d / %d", sample_count, target_samples);
    display.setTextColor(COLOR_RED);
    display.setTextSize(2);
    display.drawString(buf, cx, 95);
  } else {
    display.setTextColor(COLOR_GREY);
    display.setTextSize(1);
    display.drawString("KEY2=next  |  do motion", cx, 95);
  }
}

void show_status(const char* msg, uint16_t color) {
  display.fillScreen(COLOR_BLACK);
  display.setTextDatum(middle_center);
  display.setTextColor(color);
  display.setTextSize(2);
  display.drawString(msg, display.width()/2, display.height()/2);
}

void show_calibrating(int i, int total) {
  display.fillScreen(COLOR_BLACK);
  display.setTextDatum(middle_center);
  int cx = display.width() / 2;
  display.setTextColor(COLOR_YELLOW);
  display.setTextSize(2);
  display.drawString("Calibrating...", cx, 35);
  display.setTextColor(COLOR_GREY);
  display.setTextSize(1);
  display.drawString("Hold the board still!", cx, 58);
  int bar_w = (int)(220.0f * i / total);
  display.fillRect(10, 80, bar_w, 12, COLOR_YELLOW);
  display.drawRect(10, 80, 220, 12, COLOR_GREY);
}

// ─── Pre-trigger buffer flush ─────────────────────────────────────────────────
void flush_pre_buffer(int& count) {
  int start = pre_buf_full ? pre_buf_idx : 0;
  int total = pre_buf_full ? PRE_BUF_SIZE : pre_buf_idx;
  for (int i = 0; i < total; i++) {
    int idx = (start + i) % PRE_BUF_SIZE;
    Serial.print(pre_buf[idx].ax, 4); Serial.print(",");
    Serial.print(pre_buf[idx].ay, 4); Serial.print(",");
    Serial.print(pre_buf[idx].az, 4); Serial.print(",");
    Serial.print(pre_buf[idx].gx, 4); Serial.print(",");
    Serial.print(pre_buf[idx].gy, 4); Serial.print(",");
    Serial.println(pre_buf[idx].gz, 4);
    count++;
  }
  pre_buf_idx  = 0;
  pre_buf_full = false;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  battery.begin();
  battery.enableCharge();

  pinMode(KEY2, INPUT_PULLUP);
  ledcAttach(BEEP_PIN, BUZZ_FREQ, BUZZ_RES);
  ledcWrite(BEEP_PIN, 0);

  display.begin();
  display.setRotation(1);

  if (!IMU.begin()) {
    show_status("IMU FAIL", COLOR_RED);
    while (1);
  }

  // ── Calibration (3 seconds) ──────────────────────────────────────────────
  const int CAL_SAMPLES = 30;
  float sum = 0.0f;
  for (int i = 0; i < CAL_SAMPLES; i++) {
    show_calibrating(i, CAL_SAMPLES);
    if (IMU.accelerationAvailable()) {
      float ax, ay, az;
      IMU.readAcceleration(ax, ay, az);
      float mag = sqrtf(ax*ax + ay*ay + az*az);
      sum += mag;
    }
    delay(100);
  }
  rollingBaseline = sum / CAL_SAMPLES;

  buzz_short(); buzz_short();
  Serial.print("BASELINE:");
  Serial.println(rollingBaseline, 4);
  Serial.println("READY");

  update_display();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── KEY2: next gesture ───────────────────────────────────────────────────
  bool btn2_now = digitalRead(KEY2);
  if (btn2_last == HIGH && btn2_now == LOW && !recording &&
      now - btn2_debounce > DEBOUNCE_MS) {
    btn2_debounce  = now;
    gesture_idx    = (gesture_idx + 1) % NUM_GESTURES;
    total_recorded = 0;
    buzz_short();
    update_display();
  }
  btn2_last = btn2_now;

  // ── Read IMU ─────────────────────────────────────────────────────────────
  if (!IMU.accelerationAvailable() || !IMU.gyroscopeAvailable()) {
    delay(100); return;
  }

  float ax, ay, az, gx, gy, gz;
  IMU.readAcceleration(ax, ay, az);
  IMU.readGyroscope(gx, gy, gz);

  float magnitude = sqrtf(ax*ax + ay*ay + az*az);

  // ── Update adaptive baseline (only when not recording) ───────────────────
  if (!recording) {
    rollingBaseline = (rollingBaseline * (1.0f - ALPHA)) + (magnitude * ALPHA);

    pre_buf[pre_buf_idx] = {ax, ay, az, gx, gy, gz};
    pre_buf_idx = (pre_buf_idx + 1) % PRE_BUF_SIZE;
    if (pre_buf_idx == 0) pre_buf_full = true;
  }

  // ── Auto-trigger ─────────────────────────────────────────────────────────
  if (!recording && now - last_capture_ms > COOLDOWN_MS) {
    float trigger = rollingBaseline + THRESHOLDS[gesture_idx];

    if (magnitude > trigger) {
      recording      = true;
      sample_count   = 0;
      max_accel_seen = magnitude;
      target_samples = SAMPLE_COUNTS[gesture_idx];

      Serial.print("LABEL:");
      Serial.println(GESTURES[gesture_idx]);

      flush_pre_buffer(sample_count);
      buzz_short();
      update_display();
    }
  }

  // ── Record samples ───────────────────────────────────────────────────────
  if (recording) {
    if (magnitude > max_accel_seen) max_accel_seen = magnitude;

    Serial.print(ax, 4); Serial.print(",");
    Serial.print(ay, 4); Serial.print(",");
    Serial.print(az, 4); Serial.print(",");
    Serial.print(gx, 4); Serial.print(",");
    Serial.print(gy, 4); Serial.print(",");
    Serial.println(gz, 4);

    sample_count++;
    if (sample_count % 3 == 0) update_display();

    if (sample_count >= target_samples) {
      recording       = false;
      last_capture_ms = now;

      if (max_accel_seen < MIN_PEAK_ACCEL) {
        Serial.println("DISCARD");
        buzz_reject();
        show_status("TOO WEAK", COLOR_ORANGE);
        delay(800);
      } else {
        total_recorded++;
        Serial.println("END");
        buzz_done();
      }
      update_display();
    }
  }

  delay(100);  // 10Hz
}