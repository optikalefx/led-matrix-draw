// Push-to-talk pixel art: hold the button, speak, release.
// Audio goes to the relay add-on (speech to text + Claude), which returns 16x16 RGB pixels.
// While a picture is showing the button belongs to it: presses go to its Lua script, and
// holding for HOLD_CLEAR_MS clears it so the next hold records again.
// The relay page's Send button POSTs a saved entry's key to /show here; the device then fetches
// that entry from the relay and shows it like a spoken one.

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_http_server.h>
#include <FastLED.h>
#include "secrets.h"
#include "panel.h"
#include "lua_anim.h"

#define LED_PIN     13
#define MIC_PIN     34
#define BUTTON_PIN  27

#define BRIGHTNESS  40
#define MAX_MILLIAMPS 500   // powered from USB; raise with a real 5V supply

#define SAMPLE_RATE 8000                     // phone quality; lets 10s fit in RAM (no PSRAM)
#define MAX_SECONDS 10
#define MIN_SAMPLES (SAMPLE_RATE * 3 / 10)   // ignore presses shorter than 0.3s
#define MIC_GAIN    16                       // 12-bit ADC -> 16-bit PCM
#define HOLD_CLEAR_MS 800                    // holding this long while a picture shows clears it
#define DEBOUNCE_MS   30                     // the button counts as released after this long up
#define REC_FRAME_MS  100                    // panel refresh while recording; slower = less mic noise
#define REC_ORBIT_MS  2000                   // one lap of the recording dot
#define REC_WAIT_MS   50                     // red dot shown while the switch-over click dies down
#define REC_SETTLE_MS 30                     // audio kept from this long after the green ring is up

#define MAX_FRAMES  4                        // hand-drawn animations; code animations run as Lua
#define FRAME_BYTES (NUM_LEDS * 3)           // one frame: 16x16 RGB, row-major, top-left first
#define MAX_SCRIPT_BYTES 16384               // largest Lua animation script we accept
#define KEY_LENGTH  40                       // a relay cache key: 40 hex characters
#define HELLO_RETRY_MS 30000                 // how often to retry telling the relay our IP

enum State { CONNECTING, IDLE, SHOWING, RECORDING, THINKING, ERROR_SHOWN };

CRGB leds[NUM_LEDS];
uint8_t *frameData = nullptr;   // MAX_FRAMES * FRAME_BYTES, allocated at boot
volatile State state = CONNECTING;
volatile bool hasPicture = false;
volatile int frameCount = 0;
volatile uint16_t frameMs = 250;
volatile uint32_t animStart = 0;
volatile bool luaRunning = false;   // SHOWING a Lua script instead of stored frames
volatile uint32_t errorUntil = 0;
volatile uint32_t talkAt = 0;   // millis() when the green ring went up; 0 while still red

// Button events while a picture shows, counted by loop() and delivered to the Lua script by the
// display task. Presses and releases alternate, so the counts say what order they came in.
volatile uint32_t presses = 0, releases = 0;
volatile bool pressed = false;
uint32_t pressesSeen = 0, releasesSeen = 0;

uint8_t *audioBuf = nullptr;
size_t audioCap = 0;

// Saved entries sent from the relay page: the web server task puts the key here, loop() shows it
QueueHandle_t sentKeys;
bool helloNeeded = true;   // the relay doesn't know our current IP yet
uint32_t helloAt = 0;

// Clockwise path around each ring, outermost first, ending at the center
uint8_t spiralX[NUM_LEDS], spiralY[NUM_LEDS];

// ---------- display (runs on core 0) ----------

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

void showError();

void drawPicture() {
  if (luaRunning) {
    float t = (millis() - animStart) / 1000.0f;
    String error;
    bool ok = true;
    for (;;) {
      if (pressesSeen == releasesSeen && presses != pressesSeen) {
        pressesSeen++;
        ok = luaAnimEvent("press", t, error);
      } else if (pressesSeen != releasesSeen && releases != releasesSeen) {
        releasesSeen++;
        ok = luaAnimEvent("release", t, error);
      } else {
        break;
      }
      if (!ok) break;
    }
    if (!ok || !luaAnimDraw(t, pressed, error)) {
      Serial.printf("lua %s\n", error.c_str());
      luaRunning = false;
      hasPicture = false;
      showError();
    }
    return;
  }
  int f = frameCount > 1 ? ((millis() - animStart) / frameMs) % frameCount : 0;
  const uint8_t *px = frameData + f * FRAME_BYTES;
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      setXY(x, y, CRGB(px[(y * GRID + x) * 3], px[(y * GRID + x) * 3 + 1], px[(y * GRID + x) * 3 + 2]));
}

// Red center dot for the first REC_WAIT_MS (the LED current change clicks in the mic),
// then a dim green ring with a brighter dot orbiting it, meaning "talk now". Few LEDs
// change per frame to keep panel ripple out of the audio. Returns true once the ring is up.
bool drawRecording(uint32_t ms) {
  if (ms < REC_WAIT_MS) {
    for (int y = 7; y <= 8; y++)
      for (int x = 7; x <= 8; x++) setXY(x, y, CRGB(60, 0, 0));
    return false;
  }
  float a = (ms % REC_ORBIT_MS) * 2 * PI / REC_ORBIT_MS;
  setXY(lroundf(7.5f + 6.0f * sinf(a)), lroundf(7.5f - 6.0f * cosf(a)), CRGB(0, 60, 0));
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++) {
      float dx = x - 7.5f, dy = y - 7.5f;
      if (!leds[xyToIndex(x, y)] && fabsf(sqrtf(dx * dx + dy * dy) - 6.0f) < 0.6f) setXY(x, y, CRGB(0, 12, 0));
    }
  return true;
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
  State shown = CONNECTING;
  uint32_t recStart = 0, lastShow = 0;
  for (;;) {
    State s = state;
    if (s == RECORDING && shown == RECORDING && talkAt && millis() - lastShow < REC_FRAME_MS) {
      vTaskDelay(pdMS_TO_TICKS(5));   // keep the mic quiet: refresh slowly while recording
      continue;
    }
    if (s == RECORDING && shown != RECORDING) recStart = millis();
    bool talk = false;
    FastLED.clear();
    if (s == ERROR_SHOWN && millis() > errorUntil) state = s = restingState();
    switch (s) {
      case CONNECTING:  drawSpinner(frame, 160); break;   // blue
      case IDLE:        break;   // blank
      case SHOWING:     drawPicture(); break;
      case RECORDING:   talk = drawRecording(millis() - recStart); break;
      case THINKING:    drawSpinner(frame, frame * 2); break;  // rainbow
      case ERROR_SHOWN: drawError(); break;
    }
    FastLED.show();
    lastShow = millis();
    if (talk && !talkAt) talkAt = lastShow;
    shown = s;
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

// Records until the button has been released for 30ms or the buffer is full. Audio from
// before the green "talk now" ring (plus REC_SETTLE_MS) is sampled but not kept.
size_t recordWhileHeld() {
  size_t n = 0;
  uint32_t taken = 0;
  int32_t dc = analogRead(MIC_PIN) << 8;   // running DC offset, 8 fractional bits
  uint32_t releasedAt = 0;
  uint32_t start = micros();

  while (n < audioCap) {
    if (!buttonHeld()) {
      if (releasedAt == 0) releasedAt = micros();
      else if (micros() - releasedAt > DEBOUNCE_MS * 1000) break;
    } else {
      releasedAt = 0;
    }

    uint32_t target = start + (uint32_t)((uint64_t)taken++ * 1000000 / SAMPLE_RATE);
    while ((int32_t)(micros() - target) < 0) {}

    int raw = analogRead(MIC_PIN);
    dc += ((raw << 8) - dc) >> 4;   // tracks DC and rumble below ~80Hz (panel refresh, hum), removed below
    uint32_t talk = talkAt;
    if (!talk || millis() - talk < REC_SETTLE_MS) continue;
    int32_t sample = (((raw << 8) - dc) >> 8) * MIC_GAIN;
    audioBuf[n++] = linearToMulaw(sample);
  }

  float elapsed = (micros() - start) / 1e6;
  Serial.printf("recorded %u samples in %.2fs (expected %.2fs, %.2fs skipped)\n",
                (unsigned)n, elapsed, (float)taken / SAMPLE_RATE, (float)(taken - n) / SAMPLE_RATE);
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

// Body is a Lua script defining draw(t); the display task runs it live
bool loadLuaResponse(HTTPClient &http) {
  int length = http.getSize();
  if (length <= 0 || length > MAX_SCRIPT_BYTES) {
    Serial.printf("bad script size: %d bytes\n", length);
    return false;
  }
  char *source = (char *)malloc(length);
  if (!source) {
    Serial.println("not enough memory for the script");
    return false;
  }
  int got = 0;
  WiFiClient *stream = http.getStreamPtr();
  uint32_t readStart = millis();
  while (got < length && millis() - readStart < 10000) {
    int avail = stream->available();
    if (avail > 0) got += stream->read((uint8_t *)source + got, min(avail, length - got));
    else delay(1);
  }

  String error;
  bool ok = got == length && luaAnimLoad(source, length, error);
  free(source);
  if (!ok) {
    Serial.printf("lua %s\n", got == length ? error.c_str() : "script download was cut short");
    return false;
  }
  Serial.printf("lua script running: %d bytes, %u bytes of Lua memory\n", length,
                (unsigned)luaAnimMemoryUsed());
  pressesSeen = presses;   // the button is up, so these are equal: no stale events
  releasesSeen = releases;
  animStart = millis();
  luaRunning = true;
  hasPicture = true;
  return true;
}

const char *PICTURE_HEADERS[] = {"X-Transcript", "X-Title", "X-Mode", "X-Frames", "X-Frame-Ms"};

// Reads a relay response (a still, frames, or a Lua script) into the picture. Only call when
// the state isn't SHOWING, so the display task isn't using the old one.
bool loadPicture(HTTPClient &http, int code, uint32_t t0) {
  if (code != 200) {
    String why = code > 0 ? http.getString() : http.errorToString(code);
    Serial.printf("relay error %d: %s\n", code, why.c_str());
    http.end();
    return false;
  }

  hasPicture = false;
  luaRunning = false;
  String title = urlDecode(http.header("X-Title"));
  Serial.printf("heard: \"%s\" -> %s: %s (%.1fs)\n", urlDecode(http.header("X-Transcript")).c_str(),
                http.header("X-Mode").c_str(), title.c_str(), (millis() - t0) / 1000.0);

  if (http.header("X-Mode") == "lua") {
    bool ok = loadLuaResponse(http);
    http.end();
    return ok;
  }
  luaAnimClose();   // free the previous script's memory

  // Body is X-Frames frames of FRAME_BYTES each
  int frames = constrain(http.header("X-Frames").toInt(), 1, MAX_FRAMES);
  int ms = http.header("X-Frame-Ms").toInt();
  size_t want = (size_t)frames * FRAME_BYTES;
  size_t got = 0;
  WiFiClient *stream = http.getStreamPtr();
  uint32_t readStart = millis();
  while (got < want && millis() - readStart < 10000) {
    int avail = stream->available();
    if (avail > 0) got += stream->read(frameData + got, min((size_t)avail, want - got));
    else delay(1);
  }
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

bool sendToRelay(size_t samples) {
  HTTPClient http;
  http.begin(String(RELAY_URL) + "/draw");
  http.setConnectTimeout(5000);
  http.setTimeout(45000);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Sample-Rate", String(SAMPLE_RATE));
  http.addHeader("X-Device-IP", WiFi.localIP().toString());
  http.collectHeaders(PICTURE_HEADERS, 5);
  uint32_t t0 = millis();
  return loadPicture(http, http.POST(audioBuf, samples), t0);
}

bool fetchSaved(const char *key) {
  HTTPClient http;
  http.begin(String(RELAY_URL) + "/cache/" + key + "/device");
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  http.collectHeaders(PICTURE_HEADERS, 5);
  uint32_t t0 = millis();
  return loadPicture(http, http.GET(), t0);
}

// Tells the relay our IP so its Send button can reach us
void sayHello() {
  helloAt = millis();
  HTTPClient http;
  http.begin(String(RELAY_URL) + "/hello");
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  int code = http.POST(WiFi.localIP().toString());
  http.end();
  helloNeeded = code != 200;
  if (helloNeeded) Serial.printf("relay hello failed (%d), retrying later\n", code);
}

// ---------- web server: the relay's Send button ----------

// Runs on the web server's task: just hand the key to loop()
esp_err_t handleShow(httpd_req_t *req) {
  char key[KEY_LENGTH + 1] = {};
  if (req->content_len != KEY_LENGTH || httpd_req_recv(req, key, KEY_LENGTH) != KEY_LENGTH) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a cache key");
    return ESP_FAIL;
  }
  xQueueOverwrite(sentKeys, key);
  httpd_resp_sendstr(req, "ok");
  return ESP_OK;
}

void startServer() {
  sentKeys = xQueueCreate(1, KEY_LENGTH + 1);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 1;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    Serial.println("web server failed to start; Send from the relay page won't work");
    return;
  }
  httpd_uri_t show = {};
  show.uri = "/show";
  show.method = HTTP_POST;
  show.handler = handleShow;
  httpd_register_uri_handler(server, &show);
}

// Shows a saved entry the relay page sent, if there is one
void showSent() {
  char key[KEY_LENGTH + 1];
  if (!sentKeys || xQueueReceive(sentKeys, key, 0) != pdTRUE) return;
  Serial.printf("sent from the relay page: %s\n", key);
  state = THINKING;
  delay(50);   // let the display task finish any frame of the old picture
  if (fetchSaved(key)) state = SHOWING;
  else showError();
}

// ---------- main ----------

void clearPicture() {
  hasPicture = false;
  luaRunning = false;
  state = IDLE;
  delay(50);   // let the display task finish any frame it was drawing
  luaAnimClose();
  Serial.println("hold: cleared");
}

// The button went down while a picture is showing: report the press and release to its
// script, and clear the picture if the button is held for HOLD_CLEAR_MS.
void pictureButton() {
  uint32_t downAt = millis(), releasedAt = 0;
  pressed = true;
  presses++;
  for (;;) {
    if (buttonHeld()) releasedAt = 0;
    else if (!releasedAt) releasedAt = millis();
    else if (millis() - releasedAt > DEBOUNCE_MS) break;
    if (hasPicture && !releasedAt && millis() - downAt >= HOLD_CLEAR_MS) clearPicture();
    delay(2);
  }
  pressed = false;
  releases++;
}

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
  // Lua scripts run on this task, so it needs a bigger stack than plain drawing
  xTaskCreatePinnedToCore(displayTask, "display", 16384, nullptr, 1, nullptr, 0);

  connectWiFi();
  startServer();

  frameData = (uint8_t *)malloc(MAX_FRAMES * FRAME_BYTES);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  audioCap = min((size_t)(SAMPLE_RATE * MAX_SECONDS), largest > 20000 ? largest - 20000 : 0);
  audioBuf = (uint8_t *)malloc(audioCap);
  Serial.printf("audio buffer: %.1fs max, %u bytes heap free for Lua and Wi-Fi\n",
                (float)audioCap / SAMPLE_RATE, (unsigned)ESP.getFreeHeap());

  state = IDLE;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    state = CONNECTING;
    helloNeeded = true;   // we may come back with a different IP
    delay(500);
    return;
  }
  if (state == CONNECTING) state = restingState();
  if (helloNeeded && (helloAt == 0 || millis() - helloAt > HELLO_RETRY_MS)) sayHello();

  if (!buttonHeld()) {
    showSent();
    delay(5);
    return;
  }

  if (hasPicture) {
    pictureButton();
    return;
  }

  talkAt = 0;
  state = RECORDING;
  size_t samples = recordWhileHeld();

  // Too short to be speech: ignore it
  if (samples < MIN_SAMPLES) {
    state = restingState();
    return;
  }

  state = THINKING;
  if (sendToRelay(samples)) state = SHOWING;
  else showError();

  while (buttonHeld()) delay(10);   // don't re-trigger if the buffer filled while held
}
