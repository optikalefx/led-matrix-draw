#include <FastLED.h>

#define LED_PIN     13
#define NUM_LEDS    256
#define MIC_PIN     34
#define SAMPLE_RATE 16000
#define REC_SECONDS 3
#define REC_SAMPLES (SAMPLE_RATE * REC_SECONDS)

CRGB leds[NUM_LEDS];
int16_t recording[REC_SAMPLES];

void setup() {
  Serial.begin(921600);
  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);  // full 0-3.3V range

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(32);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 400);
  FastLED.clear(true);
}

// Samples for ~100ms and reports min/max/mean and peak-to-peak
void reportLevel() {
  const int n = SAMPLE_RATE / 10;
  int lo = 4095, hi = 0;
  long sum = 0;
  uint32_t next = micros();
  for (int i = 0; i < n; i++) {
    while ((int32_t)(micros() - next) < 0) {}
    next += 1000000 / SAMPLE_RATE;
    int v = analogRead(MIC_PIN);
    lo = min(lo, v);
    hi = max(hi, v);
    sum += v;
  }
  int p2p = hi - lo;
  Serial.printf("LEVEL mean=%4ld min=%4d max=%4d p2p=%4d\n", sum / n, lo, hi, p2p);

  int bar = constrain(map(p2p, 0, 2000, 0, 16), 0, 16);
  for (int i = 0; i < 16; i++) {
    leds[i] = i < bar ? (i < 10 ? CRGB::Green : i < 13 ? CRGB::Yellow : CRGB::Red) : CRGB::Black;
  }
  FastLED.show();
}

void record() {
  Serial.println("REC start");
  uint32_t next = micros();
  for (int i = 0; i < REC_SAMPLES; i++) {
    while ((int32_t)(micros() - next) < 0) {}
    next += 1000000 / SAMPLE_RATE;
    recording[i] = analogRead(MIC_PIN);
  }

  // Remove DC offset and scale 12-bit ADC to 16-bit PCM
  long sum = 0;
  for (int i = 0; i < REC_SAMPLES; i++) sum += recording[i];
  int mean = sum / REC_SAMPLES;
  for (int i = 0; i < REC_SAMPLES; i++) {
    recording[i] = constrain((recording[i] - mean) * 16, -32768, 32767);
  }

  Serial.printf("DATA %d\n", REC_SAMPLES * 2);
  Serial.write((uint8_t *)recording, REC_SAMPLES * 2);
  Serial.println();
  Serial.println("REC done");
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') {
    record();
  }
  reportLevel();
}
