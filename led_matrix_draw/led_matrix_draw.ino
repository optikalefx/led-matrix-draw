// Push-to-talk pixel art: hold the button, speak, release.
// Audio goes to the relay add-on (speech to text + Claude), which returns 16x16 RGB pixels.

#include <WiFi.h>
#include <HTTPClient.h>
#include <FastLED.h>
#include "secrets.h"

#define LED_PIN     13
#define MIC_PIN     34
#define BUTTON_PIN  27

#define GRID        16
#define NUM_LEDS    (GRID * GRID)
#define BRIGHTNESS  40
#define MAX_MILLIAMPS 500   // powered from USB; raise with a real 5V supply

#define SAMPLE_RATE 16000
#define MAX_SECONDS 5
#define MIN_SAMPLES (SAMPLE_RATE * 3 / 10)   // ignore presses shorter than 0.3s
#define MIC_GAIN    16                       // 12-bit ADC -> 16-bit PCM
#define DOUBLE_TAP_MS 500                    // two quick taps within this clear the panel

#define MAX_FRAMES  64                       // upper limit; the real limit depends on free memory
#define FRAME_BYTES (NUM_LEDS * 3)           // one frame: 16x16 RGB, row-major, top-left first

enum State { CONNECTING, IDLE, SHOWING, RECORDING, THINKING, ERROR_SHOWN };

CRGB leds[NUM_LEDS];
uint8_t *frameData = nullptr;   // maxFrames * FRAME_BYTES, allocated at boot
int maxFrames = 0;
volatile State state = CONNECTING;
volatile bool hasPicture = false;
volatile int frameCount = 0;
volatile uint16_t frameMs = 250;
volatile uint32_t animStart = 0;
volatile int micLevel = 0;
volatile uint32_t errorUntil = 0;

uint8_t *audioBuf = nullptr;
size_t audioCap = 0;
uint32_t lastTapAt = 0;

// Clockwise path around each ring, outermost first, ending at the center
uint8_t spiralX[NUM_LEDS], spiralY[NUM_LEDS];

// ---------- display (runs on core 0) ----------

// Panel is column-serpentine: LED 0 top-left, first 16 run down the left column,
// the next 16 run back up the second column, and so on.
uint16_t xyToIndex(int x, int y) {
  return x * GRID + ((x & 1) ? (GRID - 1 - y) : y);
}

void setXY(int x, int y, CRGB c) {
  if (x >= 0 && x < GRID && y >= 0 && y < GRID) leds[xyToIndex(x, y)] = c;
}

// Position i (0..59) clockwise around the outer edge
void borderXY(int i, int &x, int &y) {
  i %= 60;
  if (i < 15)      { x = i;           y = 0; }
  else if (i < 30) { x = 15;          y = i - 15; }
  else if (i < 45) { x = 45 - i;      y = 15; }
  else             { x = 0;           y = 60 - i; }
}

void buildSpiral() {
  int i = 0;
  for (int r = 0; r < GRID / 2; r++) {
    int lo = r, hi = GRID - 1 - r;
    for (int x = lo; x < hi; x++, i++) { spiralX[i] = x;  spiralY[i] = lo; }
    for (int y = lo; y < hi; y++, i++) { spiralX[i] = hi; spiralY[i] = y; }
    for (int x = hi; x > lo; x--, i++) { spiralX[i] = x;  spiralY[i] = hi; }
    for (int y = hi; y > lo; y--, i++) { spiralX[i] = lo; spiralY[i] = y; }
  }
}

void drawPicture() {
  int f = frameCount > 1 ? ((millis() - animStart) / frameMs) % frameCount : 0;
  const uint8_t *px = frameData + f * FRAME_BYTES;
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      setXY(x, y, CRGB(px[(y * GRID + x) * 3], px[(y * GRID + x) * 3 + 1], px[(y * GRID + x) * 3 + 2]));
}

void drawRecording() {
  for (int i = 0; i < 60; i++) {
    int x, y;
    borderXY(i, x, y);
    setXY(x, y, CRGB(80, 0, 0));
  }
  // Level meter rising from the bottom
  int h = constrain(map(micLevel, 40, 1500, 0, 14), 0, 14);
  for (int row = 0; row < h; row++) {
    CRGB c = row < 8 ? CRGB::Green : row < 11 ? CRGB::Yellow : CRGB::Red;
    for (int x = 4; x < 12; x++) setXY(x, 14 - row, c);
  }
}

// Comet that spirals from the outer ring to the center, then starts over
void drawSpinner(uint32_t frame, uint8_t hue) {
  const int tail = 10;
  int head = (frame * 2) % (NUM_LEDS + tail);   // extra steps let the tail drain into the center
  for (int k = 0; k < tail; k++) {
    int pos = head - (tail - 1 - k);
    if (pos < 0 || pos >= NUM_LEDS) continue;
    setXY(spiralX[pos], spiralY[pos], CHSV(hue + k * 8, 255, 25 + k * 23));
  }
}

void drawError() {
  for (int i = 2; i < 14; i++) {
    setXY(i, i, CRGB::Red);
    setXY(15 - i, i, CRGB::Red);
  }
}

// Where to go when nothing else is happening: the last picture, or a blank panel
State restingState() {
  return hasPicture ? SHOWING : IDLE;
}

void displayTask(void *) {
  uint32_t frame = 0;
  for (;;) {
    FastLED.clear();
    State s = state;
    if (s == ERROR_SHOWN && millis() > errorUntil) state = s = restingState();
    switch (s) {
      case CONNECTING:  drawSpinner(frame, 160); break;   // blue
      case IDLE:        break;   // blank
      case SHOWING:     drawPicture(); break;
      case RECORDING:   drawRecording(); break;
      case THINKING:    drawSpinner(frame, frame * 2); break;  // rainbow
      case ERROR_SHOWN: drawError(); break;
    }
    FastLED.show();
    frame++;
    vTaskDelay(pdMS_TO_TICKS(20));   // ~50fps so fast animations play smoothly
  }
}

void showError() {
  errorUntil = millis() + 2000;
  state = ERROR_SHOWN;
}

// ---------- audio ----------

uint8_t linearToMulaw(int32_t sample) {
  const int32_t BIAS = 0x84, CLIP = 32635;
  uint8_t sign = 0;
  if (sample < 0) { sign = 0x80; sample = -sample; }
  if (sample > CLIP) sample = CLIP;
  sample += BIAS;
  int exponent = 7;
  for (int32_t mask = 0x4000; (sample & mask) == 0 && exponent > 0; exponent--, mask >>= 1) {}
  int mantissa = (sample >> (exponent + 3)) & 0x0F;
  return ~(sign | (exponent << 4) | mantissa);
}

bool buttonHeld() {
  return digitalRead(BUTTON_PIN) == LOW;
}

// Records until the button has been released for 30ms or the buffer is full.
size_t recordWhileHeld() {
  size_t n = 0;
  int32_t dc = analogRead(MIC_PIN) << 8;   // running DC offset, 8 fractional bits
  int lo = 4095, hi = 0;
  uint32_t releasedAt = 0;
  uint32_t start = micros();

  while (n < audioCap) {
    if (!buttonHeld()) {
      if (releasedAt == 0) releasedAt = micros();
      else if (micros() - releasedAt > 30000) break;
    } else {
      releasedAt = 0;
    }

    uint32_t target = start + (uint32_t)((uint64_t)n * 1000000 / SAMPLE_RATE);
    while ((int32_t)(micros() - target) < 0) {}

    int raw = analogRead(MIC_PIN);
    dc += ((raw << 8) - dc) >> 10;
    int32_t sample = (((raw << 8) - dc) >> 8) * MIC_GAIN;
    audioBuf[n++] = linearToMulaw(sample);

    lo = min(lo, raw);
    hi = max(hi, raw);
    if (n % 800 == 0) {
      micLevel = hi - lo;
      lo = 4095;
      hi = 0;
    }
  }

  float elapsed = (micros() - start) / 1e6;
  Serial.printf("recorded %u samples in %.2fs (expected %.2fs)\n",
                (unsigned)n, elapsed, (float)n / SAMPLE_RATE);
  return n;
}

// ---------- relay ----------

String urlDecode(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '%' && i + 2 < s.length()) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

bool sendToRelay(size_t samples) {
  HTTPClient http;
  http.begin(String(RELAY_URL) + "/draw");
  http.setConnectTimeout(5000);
  http.setTimeout(45000);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Sample-Rate", String(SAMPLE_RATE));
  http.addHeader("X-Max-Frames", String(maxFrames));   // code animations are sized to fit
  const char *keys[] = {"X-Transcript", "X-Title", "X-Frames", "X-Frame-Ms"};
  http.collectHeaders(keys, 4);

  uint32_t t0 = millis();
  int code = http.POST(audioBuf, samples);
  if (code != 200) {
    String why = code > 0 ? http.getString() : http.errorToString(code);
    Serial.printf("relay error %d: %s\n", code, why.c_str());
    http.end();
    return false;
  }

  // Body is X-Frames frames of FRAME_BYTES each. Safe to write frameData here:
  // the display task only reads it in the SHOWING state, and we're in THINKING.
  int frames = constrain(http.header("X-Frames").toInt(), 1, maxFrames);
  int ms = http.header("X-Frame-Ms").toInt();
  size_t want = (size_t)frames * FRAME_BYTES;
  size_t got = 0;
  hasPicture = false;
  WiFiClient *stream = http.getStreamPtr();
  uint32_t readStart = millis();
  while (got < want && millis() - readStart < 10000) {
    int avail = stream->available();
    if (avail > 0) got += stream->read(frameData + got, min((size_t)avail, want - got));
    else delay(1);
  }
  Serial.printf("heard: \"%s\" -> drew: %s, %d frame%s (%.1fs)\n",
                urlDecode(http.header("X-Transcript")).c_str(),
                urlDecode(http.header("X-Title")).c_str(),
                frames, frames == 1 ? "" : "s",
                (millis() - t0) / 1000.0);
  http.end();

  if (got != want) {
    Serial.printf("short response: %u of %u bytes\n", (unsigned)got, (unsigned)want);
    return false;
  }
  frameCount = frames;
  frameMs = ms > 0 ? max(ms, 20) : 250;
  animStart = millis();
  hasPicture = true;
  return true;
}

// ---------- main ----------

void connectWiFi() {
  state = CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nconnected, IP %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  buildSpiral();
  xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);

  connectWiFi();

  // Audio gets first pick of memory; animation frames get what's left
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  audioCap = min((size_t)(SAMPLE_RATE * MAX_SECONDS), largest > 20000 ? largest - 20000 : 0);
  audioBuf = (uint8_t *)malloc(audioCap);

  largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  maxFrames = constrain((int)((largest > 24000 ? largest - 24000 : 0) / FRAME_BYTES), 1, MAX_FRAMES);
  frameData = (uint8_t *)malloc(maxFrames * FRAME_BYTES);
  Serial.printf("audio buffer: %.1fs max, %d animation frames, %u bytes heap free\n",
                (float)audioCap / SAMPLE_RATE, maxFrames, (unsigned)ESP.getFreeHeap());

  state = IDLE;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    state = CONNECTING;
    delay(500);
    return;
  }
  if (state == CONNECTING) state = restingState();

  if (!buttonHeld()) {
    delay(10);
    return;
  }

  state = RECORDING;
  size_t samples = recordWhileHeld();
  micLevel = 0;

  // Too short to be speech: treat it as a tap. Two quick taps clear the panel.
  if (samples < MIN_SAMPLES) {
    if (lastTapAt && millis() - lastTapAt < DOUBLE_TAP_MS) {
      hasPicture = false;
      lastTapAt = 0;
      Serial.println("double tap: cleared");
    } else {
      lastTapAt = millis();
    }
    state = restingState();
    return;
  }
  lastTapAt = 0;

  state = THINKING;
  if (sendToRelay(samples)) state = SHOWING;
  else showError();

  while (buttonHeld()) delay(10);   // don't re-trigger if the buffer filled while held
}
