// gesture_detect.ino
// Nesso N1 — Gesture Detection Demo (Edge Impulse version)
//
// Gestures: circle / shake / swipe_h / swipe_v / tap
//
// SETUP:
//   1. Train your model on Edge Impulse
//   2. Export as Arduino Library (Deployment → Arduino Library → Build)
//   3. In Arduino IDE: Sketch → Include Library → Add .ZIP Library
//   4. Replace "your-project-name_inferencing.h" below with your actual library name
//      (open the zip, look at the .h file in src/ folder)
//
// Runs on battery — no USB needed after flashing
// KEY1 single press = pause
// KEY1 double press = resume
//
// Features:
//   - Adaptive threshold (rolling baseline, updates continuously)
//   - Per-gesture thresholds
//   - 500ms anti-spam cooldown
//   - Buzzer at 25% volume
//   - 2s result hold on display
//   - Battery percentage and voltage display
//   - Stays on when unplugged (battery latch)
//
// Libraries: Arduino_Nesso_N1, M5GFX, Arduino_BMI270_BMM150
//            + your Edge Impulse exported library

#include <Arduino_Nesso_N1.h>
#include <Wire.h>
#include "Arduino_BMI270_BMM150.h"

// ─── REPLACE THIS with your actual Edge Impulse library header name ───────────
// Example: #include <my_gesture_project_inferencing.h>
#include <nthan267-project-1_inferencing.h>

NessoDisplay display;
NessoBattery battery;

#define BUZZ_FREQ  4000
#define BUZZ_RES   8
#define BUZZ_VOL   17   // 25% volume (0=silent, 255=max)

const uint16_t COLOR_BLACK  = 0x0000;
const uint16_t COLOR_WHITE  = 0xFFFF;
const uint16_t COLOR_GREEN  = 0x07E0;
const uint16_t COLOR_GREY   = 0x7BEF;
const uint16_t COLOR_RED    = 0xF800;
const uint16_t COLOR_ORANGE = 0xFD20;
const uint16_t COLOR_YELLOW = 0xFFE0;
const uint16_t COLOR_TEAL   = 0x0410;

// ─── Gesture labels — must match Edge Impulse label names exactly ─────────────
// Edge Impulse sorts labels alphabetically
// 0=circle  1=shake  2=swipe_h  3=swipe_v  4=tap
const int NUM_GESTURES = 5;

// ─── Per-gesture detection thresholds (added to rolling baseline) ─────────────
// Index matches alphabetical order above
const float DETECT_THRESH[] = {1.5f, 1.5f, 1.5f, 1.5f, 2.5f};

// ─── Adaptive threshold ───────────────────────────────────────────────────────
float rollingBaseline = 1.0f;
const float ALPHA = 0.05f;

// ─── Timing ───────────────────────────────────────────────────────────────────
const unsigned long DISPLAY_HOLD_MS    = 2000;
const unsigned long DETECTION_COOLDOWN = 500;
const unsigned long DOUBLE_PRESS_MS    = 400;
const unsigned long BATTERY_UPDATE_MS  = 10000;  // update battery every 10 seconds

// ─── Edge Impulse inference buffer ───────────────────────────────────────────
// EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE = axes * window samples (set by Edge Impulse)
float ei_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
int   ei_buf_idx = 0;

// ─── State ────────────────────────────────────────────────────────────────────
int           last_gesture      = -1;
unsigned long last_detection_ms = 0;
unsigned long last_detect_time  = 0;
unsigned long last_battery_ms   = 0;
bool          paused            = false;
bool          collecting        = false;

bool          btn_last       = HIGH;
int           press_count    = 0;
unsigned long first_press_ms = 0;

// ─── Battery display ──────────────────────────────────────────────────────────
void drawBatteryInfo() {
  float chargeLevel = battery.getChargeLevel();
  float voltage = battery.getVoltage();

  uint16_t batColor = COLOR_GREEN;
  if (voltage < 3.3) batColor = COLOR_RED;
  else if (voltage < 3.7) batColor = COLOR_YELLOW;

  display.setTextColor(batColor);
  display.setTextSize(1);
  display.setTextDatum(top_right);

  char batBuf[20];
  snprintf(batBuf, sizeof(batBuf), "%.1f%% %.2fV", chargeLevel, voltage);
  display.drawString(batBuf, display.width() - 5, 5);
}

// ─────────────────────────────────────────────────────────────────────────────
void buzz_once() {
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(100); ledcWrite(BEEP_PIN, 0);
}
void buzz_double() {
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(80);
  ledcWrite(BEEP_PIN, 0);        delay(80);
  ledcWrite(BEEP_PIN, BUZZ_VOL); delay(80);
  ledcWrite(BEEP_PIN, 0);
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
  display.fillRect(10, 78, bar_w, 12, COLOR_YELLOW);
  display.drawRect(10, 78, 220, 12, COLOR_GREY);
}

void show_idle() {
  display.fillScreen(COLOR_BLACK);
  drawBatteryInfo();
  display.setTextDatum(middle_center);
  display.setTextColor(COLOR_GREY);
  display.setTextSize(2);
  display.drawString("Waiting...", display.width()/2, display.height()/2 - 10);
  display.setTextColor(0x3186);
  display.setTextSize(1);
  display.drawString("KEY1 = pause", display.width()/2, display.height()/2 + 20);
}

void show_paused() {
  display.fillScreen(COLOR_BLACK);
  drawBatteryInfo();
  display.setTextDatum(middle_center);
  display.setTextColor(COLOR_ORANGE);
  display.setTextSize(3);
  display.drawString("PAUSED", display.width()/2, display.height()/2 - 16);
  display.setTextColor(COLOR_GREY);
  display.setTextSize(1);
  display.drawString("double-press KEY1 to resume", display.width()/2, display.height()/2 + 20);
}

void show_gesture(const char* name, float conf) {
  display.fillScreen(COLOR_BLACK);
  drawBatteryInfo();
  display.setTextDatum(middle_center);
  display.setTextColor(COLOR_GREEN);
  display.setTextSize(3);
  display.drawString(name, display.width()/2, display.height()/2 - 18);
  char buf[10];
  snprintf(buf, sizeof(buf), "%.0f%%", conf * 100.0f);
  display.setTextColor(COLOR_WHITE);
  display.setTextSize(2);
  display.drawString(buf, display.width()/2, display.height()/2 + 22);
}

int check_button() {
  bool btn_now = digitalRead(KEY1);
  if (btn_last == HIGH && btn_now == LOW) {
    press_count++;
    if (press_count == 1) first_press_ms = millis();
  }
  btn_last = btn_now;
  if (press_count > 0 && millis() - first_press_ms > DOUBLE_PRESS_MS) {
    int result = press_count >= 2 ? 2 : 1;
    press_count = 0;
    return result;
  }
  return 0;
}

// ─── Edge Impulse data callback ───────────────────────────────────────────────
static int ei_get_data(size_t offset, size_t length, float *out) {
  for (size_t i = 0; i < length; i++) {
    out[i] = ei_buffer[offset + i];
  }
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(KEY1, INPUT_PULLUP);

  // ── Battery latch — keeps board on when unplugged ───────────────────────
  battery.begin();
  battery.enableCharge();

  ledcAttach(BEEP_PIN, BUZZ_FREQ, BUZZ_RES);
  ledcWrite(BEEP_PIN, 0);

  display.begin();
  display.setRotation(1);

  if (!IMU.begin()) {
    display.fillScreen(COLOR_RED);
    display.setTextDatum(middle_center);
    display.drawString("IMU FAIL", display.width()/2, display.height()/2);
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
      sum += sqrtf(ax*ax + ay*ay + az*az);
    }
    delay(100);
  }
  rollingBaseline = sum / CAL_SAMPLES;
  buzz_once();

  show_idle();
  Serial.println("Ready!");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {

  // ── Button ──────────────────────────────────────────────────────────────
  int btn = check_button();
  if (!paused && btn == 1) {
    paused = true;
    last_gesture = -1;
    collecting   = false;
    ei_buf_idx   = 0;
    show_paused();
    buzz_once();
    delay(100);
    return;
  }
  if (paused) {
    if (btn == 2) {
      paused = false;
      show_idle();
      buzz_double();
    }
    delay(20);
    return;
  }

  // ── Anti-spam cooldown ───────────────────────────────────────────────────
  unsigned long now = millis();
  if (now - last_detect_time < DETECTION_COOLDOWN) {
    delay(20); return;
  }

  // ── Periodic battery update ─────────────────────────────────────────────
  if (now - last_battery_ms > BATTERY_UPDATE_MS) {
    last_battery_ms = now;
    if (!collecting && last_gesture < 0) {
      show_idle();  // redraws with updated battery info
    }
  }

  // ── IMU ─────────────────────────────────────────────────────────────────
  float ax, ay, az, gx, gy, gz;
  if (!IMU.accelerationAvailable() || !IMU.gyroscopeAvailable()) {
    delay(100); return;
  }
  IMU.readAcceleration(ax, ay, az);
  IMU.readGyroscope(gx, gy, gz);

  float magnitude = sqrtf(ax*ax + ay*ay + az*az);

  // ── Update adaptive baseline when idle ───────────────────────────────────
  if (!collecting) {
    rollingBaseline = (rollingBaseline * (1.0f - ALPHA)) + (magnitude * ALPHA);
  }

  float trigger = rollingBaseline + 0.5f;  // lowered for faster detection

  // ── Trigger collection ───────────────────────────────────────────────────
  if (!collecting && magnitude > trigger) {
    collecting = true;
    ei_buf_idx = 0;
    Serial.println("Collecting...");
  }

  // ── Fill EI buffer ───────────────────────────────────────────────────────
  if (collecting) {
    int base = ei_buf_idx * 6;
    if (base + 5 < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      ei_buffer[base + 0] = ax;
      ei_buffer[base + 1] = ay;
      ei_buffer[base + 2] = az;
      ei_buffer[base + 3] = gx;
      ei_buffer[base + 4] = gy;
      ei_buffer[base + 5] = gz;
      ei_buf_idx++;
    }

    // Buffer full — run inference
    if (ei_buf_idx * 6 >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      collecting = false;

      signal_t signal;
      signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
      signal.get_data     = &ei_get_data;

      ei_impulse_result_t result;
      EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

      if (err == EI_IMPULSE_OK) {
        // Find best prediction
        int   best_idx  = 0;
        float best_conf = result.classification[0].value;
        for (int i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
          if (result.classification[i].value > best_conf) {
            best_conf = result.classification[i].value;
            best_idx  = i;
          }
        }

        const char* label = result.classification[best_idx].label;

        // Only show if confidence > 60%
        if (best_conf > 0.6f) {
          last_detect_time  = now;
          last_detection_ms = now;
          last_gesture      = best_idx;

          show_gesture(label, best_conf);
          buzz_once();

          Serial.print("DETECTED: ");
          Serial.print(label);
          Serial.print(" | ");
          Serial.print(best_conf * 100.0f, 1);
          Serial.println("%");
        } else {
          // Low confidence — go back to idle
          if (last_gesture >= 0 && now - last_detection_ms > DISPLAY_HOLD_MS) {
            last_gesture = -1;
            show_idle();
          }
        }
      }

      ei_buf_idx = 0;
    }
  }

  // ── Clear display after hold time ────────────────────────────────────────
  if (!collecting && last_gesture >= 0 &&
      now - last_detection_ms > DISPLAY_HOLD_MS) {
    last_gesture = -1;
    show_idle();
  }

  delay(100);  // 10Hz matches collection rate
}
