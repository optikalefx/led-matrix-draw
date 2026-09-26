#include <FastLED.h>

#define DATA_PIN   13
#define NUM_LEDS   256
#define BRIGHTNESS 32

CRGB leds[NUM_LEDS];

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  // Powered from USB 5V, so keep total draw well under the port limit
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 400);

  FastLED.clear();
  leds[0] = CRGB::Red;
  FastLED.show();
  Serial.println("LED 0 set to red");
}

void loop() {
}
