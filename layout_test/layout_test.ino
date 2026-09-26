#include <FastLED.h>

#define LED_PIN  13
#define NUM_LEDS 256

CRGB leds[NUM_LEDS];

void setup() {
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(40);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 400);
  FastLED.clear();
  // First strip of 16: dim white, with LED 0 red and LED 15 green
  for (int i = 1; i < 15; i++) leds[i] = CRGB(20, 20, 20);
  leds[0] = CRGB::Red;
  leds[15] = CRGB::Green;
  // LED 16 (start of the second strip) blue
  leds[16] = CRGB::Blue;
  FastLED.show();
}

void loop() {}
