// ESP32 CLKIN generator for ADS131M02 (rack-manager board bring-up test)
// ------------------------------------------------------------------
// Outputs a continuous clock on GPIO14 so it can be measured with a
// logic analyzer / scope. Two generation methods are provided:
//
//   APLL  (default) : I2S peripheral MCLK driven from the Audio PLL.
//                     Produces a true fractional frequency with NO
//                     divider dithering -> low jitter. This is the
//                     method you should use in production for the ADC
//                     master clock. 8.192 MHz = 256 * 32 kHz is a
//                     "clean" audio-PLL frequency.
//
//   LEDC            : LED-PWM peripheral, APB(80 MHz)/fractional-divider.
//                     Simple, but 80 MHz / 8.192 MHz = 9.7656.. is not
//                     integer, so the divider dithers -> period jitter.
//                     Included for comparison only.
//
// Serial console (115200): send a line to reconfigure at runtime
//   <number>   set target frequency in Hz          e.g.  8192000
//   A          switch to APLL/I2S method
//   L          switch to LEDC method
//   ?          print current status
// A 1 Hz "ALIVE" heartbeat is printed so you can confirm the board is
// running even if the logic analyzer shows nothing.

#include <Arduino.h>
#include "driver/i2s.h"

static const int      CLK_PIN   = 14;
static const int      LEDC_CH   = 0;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

static uint32_t clkHz  = 4000000;   // chosen fCLKIN: 80MHz/20 -> dither-free LEDC,
                                    // ADS131M02 Low-Power mode (see README)
enum Method { M_APLL, M_LEDC };
// NOTE: on the *classic* ESP32, I2S MCLK (APLL method) can ONLY be routed to
// GPIO0/1/3 (CLK_OUT mux, not the GPIO matrix). On GPIO14 it produces NO output.
// Since the board wires CLKIN to GPIO14, LEDC is the only on-pin option here.
static Method method   = M_LEDC;
static bool   i2sOn    = false;
static bool   ledcOn   = false;

static void stopAll() {
  if (i2sOn)  { i2s_driver_uninstall(I2S_PORT); i2sOn = false; }
  if (ledcOn) { ledcDetachPin(CLK_PIN); ledcOn = false; }
}

static void startLEDC(uint32_t hz) {
  stopAll();
  // 1-bit resolution -> highest attainable frequency; duty=1 of 2^1 = 50%.
  double actual = ledcSetup(LEDC_CH, hz, 1);
  ledcAttachPin(CLK_PIN, LEDC_CH);
  ledcWrite(LEDC_CH, 1);
  ledcOn = true;
  Serial.printf("[LEDC] target=%u Hz  actual=%.3f Hz  (APB/dithered)\n", hz, actual);
}

static void startAPLL(uint32_t hz) {
  stopAll();
  // MCLK = fixed_mclk. sample_rate is only used to derive BCLK/WS (unused).
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = hz / 256;                 // 8.192M -> 32000
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 64;
  cfg.use_apll = true;                         // <-- clean fractional clock
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = (int)hz;                    // force exact MCLK

  esp_err_t e = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (e != ESP_OK) { Serial.printf("[APLL] i2s_driver_install failed: %d\n", e); return; }

  i2s_pin_config_t pins = {};
  pins.mck_io_num  = CLK_PIN;                  // route MCLK to GPIO14
  pins.bck_io_num  = I2S_PIN_NO_CHANGE;
  pins.ws_io_num   = I2S_PIN_NO_CHANGE;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  e = i2s_set_pin(I2S_PORT, &pins);
  if (e != ESP_OK) { Serial.printf("[APLL] i2s_set_pin failed: %d\n", e); return; }

  i2sOn = true;
  Serial.printf("[APLL] MCLK=%u Hz on GPIO%d (fractional-N, low jitter)\n", hz, CLK_PIN);
}

static void apply() {
  if (method == M_APLL) startAPLL(clkHz);
  else                  startLEDC(clkHz);
}

static void status() {
  Serial.printf("STATUS method=%s freq=%u Hz pin=GPIO%d\n",
                method == M_APLL ? "APLL" : "LEDC", clkHz, CLK_PIN);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32 CLKIN test (ADS131M02) — clock on GPIO14 ===");
  Serial.println("cmds: <Hz> | A(pll) | L(edc) | ?(status)");
  apply();
  status();
}

static unsigned long lastBeat = 0;
static String buf;

void loop() {
  if (millis() - lastBeat >= 1000) {
    lastBeat = millis();
    Serial.printf("ALIVE %s %u Hz\n", method == M_APLL ? "APLL" : "LEDC", clkHz);
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf.trim();
      if (buf.length()) {
        if (buf[0] == 'A' || buf[0] == 'a')      { method = M_APLL; apply(); status(); }
        else if (buf[0] == 'L' || buf[0] == 'l') { method = M_LEDC; apply(); status(); }
        else if (buf[0] == '?')                  { status(); }
        else {
          uint32_t v = strtoul(buf.c_str(), nullptr, 10);
          if (v > 0) { clkHz = v; apply(); status(); }
        }
      }
      buf = "";
    } else if (buf.length() < 32) {
      buf += c;
    }
  }
}
