#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

#include "secrets.h"

static const char *HOSTNAME_PREFIX = "logix-tea-light";
// SSIDs max out at 32 bytes; a name that long isn't null-terminated in the
// AP config and the beacon goes out corrupted, so keep this well under.
static const char *AP_NAME = "Logix Tea Light Setup";
static const char *FW_VERSION = "1.1.0";
static const char *PREFS_NAMESPACE = "tealight";

// Pin assignments match the Tea Light Mini v2 board (ESP32-C3 Super Mini):
// GPIO2 drives the RGB1-OB net (LED14..LED21 chain, also broken out on the
// U8 "IN" header); the chain's last DOUT is broken out on the U2 "OUT"
// header so more boards can be daisy-chained. GPIO4 is the TSOP38238.
static const uint8_t PIN_LED = 2;
static const uint8_t PIN_IR = 4;
static const uint16_t DEFAULT_NUM_LEDS = 8;
static const uint16_t MAX_LEDS = 64;

// The C3 Super Mini's RF is marginal: at full power (19.5 dBm) the AP is
// invisible. Measured on a board with a 31 mm wire antenna mod: 15-17 dBm is
// reliably visible, 19.5 and <=11 dBm are not.
static const wifi_power_t WIFI_TX_POWER = WIFI_POWER_17dBm;
static const uint16_t WIFI_PORTAL_PORT = 8080;
static const unsigned long WIFI_PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;
static const unsigned long SETUP_PORTAL_RETRY_MS = 5UL * 60UL * 1000UL;
static const unsigned long HOSTNAME_REBOOT_DELAY_MS = 2000;
static const unsigned long SAVE_DELAY_MS = 2000;
static const unsigned long FRAME_INTERVAL_MS = 16;
static const unsigned long POWER_FADE_MS = 400;
static const unsigned long IR_LEARN_TIMEOUT_MS = 15000;
static const unsigned long IR_REPEAT_WINDOW_MS = 250;

struct Rgb {
  uint8_t r, g, b;
};

enum Effect : uint8_t {
  EFFECT_SOLID,
  EFFECT_CANDLE,
  EFFECT_BREATHE,
  EFFECT_RAINBOW,
  EFFECT_COLOR_CYCLE,
  EFFECT_CHASE,
  EFFECT_TWINKLE,
  EFFECT_FIRE,
  EFFECT_CUSTOM,
  EFFECT_COUNT
};

static const char *EFFECT_NAMES[EFFECT_COUNT] = {
  "Solid", "Candle", "Breathe", "Rainbow", "Color Cycle", "Chase", "Twinkle", "Fire", "Custom"};

// Colors stepped through by the IR "color_next" action.
static const Rgb COLOR_PRESETS[] = {
  {255, 120, 20}, {255, 180, 90}, {255, 255, 255}, {255, 0, 0}, {255, 80, 0},
  {255, 220, 0}, {0, 255, 0}, {0, 255, 200}, {0, 80, 255}, {140, 0, 255}, {255, 0, 140}};
static const uint8_t NUM_COLOR_PRESETS = sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]);

enum IrAction : uint8_t {
  IR_POWER,
  IR_BRIGHT_UP,
  IR_BRIGHT_DOWN,
  IR_EFFECT_NEXT,
  IR_EFFECT_PREV,
  IR_COLOR_NEXT,
  IR_SPEED_UP,
  IR_SPEED_DOWN,
  IR_ACTION_COUNT
};

static const char *IR_ACTION_KEYS[IR_ACTION_COUNT] = {
  "power", "bright_up", "bright_down", "effect_next", "effect_prev", "color_next", "speed_up", "speed_down"};

WebServer server(80);
Adafruit_NeoPixel pixels(DEFAULT_NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
WiFiManager wm;
WiFiManagerParameter *hostnameParam = nullptr;
Preferences prefs;

// Persistent light state
bool powerOn = true;
Rgb baseColor = {255, 120, 20};
uint8_t brightnessPercent = 60;
uint8_t effect = EFFECT_CANDLE;
uint8_t speedPercent = 50;
uint16_t numLeds = DEFAULT_NUM_LEDS;
Rgb customColors[MAX_LEDS];
uint32_t irCodes[IR_ACTION_COUNT];
// Most boards ship without the TSOP38238 fitted, so IR is opt-in.
bool irEnabled = false;
// Extra clockwise steps applied to the WebUI ring so it matches how the board
// is wired/mounted. UI-only; doesn't change the LED output.
uint8_t ringRotation = 0;

// Rendering
Rgb frame[MAX_LEDS];
float powerLevel = 0.0f;
float effectTime = 0.0f;
float flickerLevel[MAX_LEDS];
float flickerTarget[MAX_LEDS];
unsigned long lastFrameMillis = 0;
uint8_t colorPresetIndex = 0;

bool stateDirty = false;
unsigned long stateDirtyMillis = 0;

// Network
bool networkStarted = false;
bool setupPortalActive = false;
unsigned long setupPortalStartMillis = 0;
bool wifiPortalActive = false;
unsigned long wifiPortalStartMillis = 0;
String currentHostname;
bool restartPending = false;
unsigned long restartPendingMillis = 0;

// IR (NEC protocol) receiver, decoded from edge timings in the ISR
volatile uint32_t irLastEdgeUs = 0;
volatile uint8_t irDecodeState = 0;
volatile uint32_t irShiftData = 0;
volatile uint8_t irBitCount = 0;
volatile uint32_t irReceivedCode = 0;
volatile bool irCodeReady = false;
volatile bool irRepeatReady = false;

uint32_t irLastCode = 0;
unsigned long irLastCodeMillis = 0;
int8_t irLastAction = -1;
unsigned long irLastActionMillis = 0;
int8_t irLearningAction = -1;
unsigned long irLearnStartMillis = 0;

String macSuffix() {
  String mac = WiFi.macAddress();
  String suffix = mac.substring(12, 14) + mac.substring(15, 17);
  suffix.toLowerCase();
  return suffix;
}

String colorToHex(const Rgb &c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
  return String(buf);
}

bool parseHexColor(String s, Rgb &out) {
  s.trim();
  if (s.startsWith("#")) {
    s = s.substring(1);
  }
  if (s.length() != 6) {
    return false;
  }
  for (size_t i = 0; i < 6; i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return false;
    }
  }
  uint32_t v = strtoul(s.c_str(), nullptr, 16);
  out = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  return true;
}

void markDirty() {
  stateDirty = true;
  stateDirtyMillis = millis();
}

void loadState() {
  prefs.begin(PREFS_NAMESPACE, true);
  powerOn = prefs.getBool("power", true);
  uint32_t c = prefs.getUInt("color", 0xFF7814);
  baseColor = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
  brightnessPercent = constrain(prefs.getUChar("bright", 60), 1, 100);
  effect = prefs.getUChar("effect", EFFECT_CANDLE);
  if (effect >= EFFECT_COUNT) {
    effect = EFFECT_CANDLE;
  }
  speedPercent = constrain(prefs.getUChar("speed", 50), 1, 100);
  numLeds = constrain(prefs.getUShort("leds", DEFAULT_NUM_LEDS), 1, MAX_LEDS);
  for (uint16_t i = 0; i < MAX_LEDS; i++) {
    customColors[i] = baseColor;
  }
  prefs.getBytes("custom", customColors, sizeof(customColors));
  memset(irCodes, 0, sizeof(irCodes));
  prefs.getBytes("ircodes", irCodes, sizeof(irCodes));
  irEnabled = prefs.getBool("irenabled", false);
  ringRotation = prefs.getUChar("ringrot", 0) % 8;
  prefs.end();
}

void saveState() {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putBool("power", powerOn);
  prefs.putUInt("color", ((uint32_t)baseColor.r << 16) | ((uint32_t)baseColor.g << 8) | baseColor.b);
  prefs.putUChar("bright", brightnessPercent);
  prefs.putUChar("effect", effect);
  prefs.putUChar("speed", speedPercent);
  prefs.putUShort("leds", numLeds);
  prefs.putBytes("custom", customColors, sizeof(customColors));
  prefs.putBytes("ircodes", irCodes, sizeof(irCodes));
  prefs.putBool("irenabled", irEnabled);
  prefs.putUChar("ringrot", ringRotation);
  prefs.end();
  stateDirty = false;
}

String loadHostname(const String &fallback) {
  prefs.begin(PREFS_NAMESPACE, true);
  String s = prefs.getString("hostname", "");
  prefs.end();
  s.trim();
  return s.length() > 0 ? s : fallback;
}

void saveHostname(const String &name) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString("hostname", name);
  prefs.end();
}

String sanitizeHostname(const String &name) {
  String out;
  for (size_t i = 0; i < name.length() && out.length() < 32; i++) {
    char c = name[i];
    if (isalnum((unsigned char)c)) {
      out += (char)tolower(c);
    } else if (c == '-' || c == ' ' || c == '_') {
      out += '-';
    }
  }
  return out;
}

void scheduleRestart() {
  restartPending = true;
  restartPendingMillis = millis();
}

void requestHostnameChange(const String &requested) {
  String newName = sanitizeHostname(requested);
  if (newName.length() > 0 && newName != currentHostname) {
    saveHostname(newName);
    scheduleRestart();
  }
}

void onSaveParams() {
  if (hostnameParam) {
    requestHostnameChange(String(hostnameParam->getValue()));
  }
}

// ---------------------------------------------------------------------------
// Light control
// ---------------------------------------------------------------------------

void setPower(bool on) {
  powerOn = on;
  markDirty();
}

void setBrightness(int percent) {
  brightnessPercent = (uint8_t)constrain(percent, 1, 100);
  markDirty();
}

void setSpeed(int percent) {
  speedPercent = (uint8_t)constrain(percent, 1, 100);
  markDirty();
}

void setEffect(int e) {
  if (e < 0 || e >= EFFECT_COUNT) {
    return;
  }
  if (e == EFFECT_CUSTOM && effect != EFFECT_CUSTOM) {
    bool allBlack = true;
    for (uint16_t i = 0; i < numLeds; i++) {
      if (customColors[i].r || customColors[i].g || customColors[i].b) {
        allBlack = false;
        break;
      }
    }
    if (allBlack) {
      for (uint16_t i = 0; i < numLeds; i++) {
        customColors[i] = baseColor;
      }
    }
  }
  effect = (uint8_t)e;
  effectTime = 0.0f;
  markDirty();
}

void setBaseColor(const Rgb &c) {
  baseColor = c;
  markDirty();
}

// Setting a single pixel switches to Custom, seeding the other pixels with
// whatever they currently show as the base color so nothing jumps.
void setCustomPixel(uint16_t index, const Rgb &c) {
  if (index >= numLeds) {
    return;
  }
  if (effect != EFFECT_CUSTOM) {
    for (uint16_t i = 0; i < numLeds; i++) {
      customColors[i] = baseColor;
    }
    effect = EFFECT_CUSTOM;
  }
  customColors[index] = c;
  markDirty();
}

void setNumLeds(int count) {
  uint16_t n = (uint16_t)constrain(count, 1, MAX_LEDS);
  if (n == numLeds) {
    return;
  }
  pixels.clear();
  pixels.show();
  pixels.updateLength(n);
  numLeds = n;
  markDirty();
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

Rgb scaleColor(const Rgb &c, float level) {
  level = constrain(level, 0.0f, 1.0f);
  return {(uint8_t)(c.r * level), (uint8_t)(c.g * level), (uint8_t)(c.b * level)};
}

Rgb hsvColor(uint16_t hue) {
  uint32_t packed = Adafruit_NeoPixel::ColorHSV(hue, 255, 255);
  return {(uint8_t)(packed >> 16), (uint8_t)(packed >> 8), (uint8_t)packed};
}

float randomFloat(float lo, float hi) {
  return lo + (hi - lo) * (float)random(0, 10001) / 10000.0f;
}

// Candle/fire share a per-pixel "wander toward a random target" flicker.
void updateFlicker(float lo, float hi, float changeChance, float ease) {
  for (uint16_t i = 0; i < numLeds; i++) {
    if (randomFloat(0.0f, 1.0f) < changeChance) {
      flickerTarget[i] = randomFloat(lo, hi);
    }
    flickerLevel[i] += (flickerTarget[i] - flickerLevel[i]) * ease;
  }
}

void renderEffect(float dtMs) {
  float speedFactor = 0.15f + (speedPercent / 100.0f) * 2.85f;
  effectTime += dtMs * speedFactor;
  float t = effectTime;

  switch (effect) {
    case EFFECT_SOLID:
      for (uint16_t i = 0; i < numLeds; i++) {
        frame[i] = baseColor;
      }
      break;

    case EFFECT_CANDLE: {
      updateFlicker(0.45f, 1.0f, 0.04f * speedFactor, 0.12f);
      for (uint16_t i = 0; i < numLeds; i++) {
        float l = flickerLevel[i];
        // Dim flickers drift toward red, like a real flame.
        frame[i] = {(uint8_t)(baseColor.r * l), (uint8_t)(baseColor.g * l * l), (uint8_t)(baseColor.b * l * l * l)};
      }
      break;
    }

    case EFFECT_BREATHE: {
      float phase = (sinf(t * 0.0025f) + 1.0f) * 0.5f;
      float level = 0.08f + 0.92f * phase * phase;
      for (uint16_t i = 0; i < numLeds; i++) {
        frame[i] = scaleColor(baseColor, level);
      }
      break;
    }

    case EFFECT_RAINBOW:
      for (uint16_t i = 0; i < numLeds; i++) {
        frame[i] = hsvColor((uint16_t)(t * 12.0f) + (uint16_t)((uint32_t)i * 65536UL / numLeds));
      }
      break;

    case EFFECT_COLOR_CYCLE: {
      Rgb c = hsvColor((uint16_t)(t * 6.0f));
      for (uint16_t i = 0; i < numLeds; i++) {
        frame[i] = c;
      }
      break;
    }

    case EFFECT_CHASE: {
      float head = fmodf(t * 0.006f, (float)numLeds);
      for (uint16_t i = 0; i < numLeds; i++) {
        float behind = head - (float)i;
        if (behind < 0) {
          behind += numLeds;
        }
        float level = behind < 4.0f ? 1.0f - behind / 4.0f : 0.0f;
        frame[i] = scaleColor(baseColor, 0.04f + 0.96f * level * level);
      }
      break;
    }

    case EFFECT_TWINKLE: {
      for (uint16_t i = 0; i < numLeds; i++) {
        if (flickerLevel[i] < 0.2f && randomFloat(0.0f, 1.0f) < 0.008f * speedFactor) {
          flickerLevel[i] = 1.0f;
        }
        flickerLevel[i] = max(0.12f, flickerLevel[i] - 0.012f * speedFactor);
        frame[i] = scaleColor(baseColor, flickerLevel[i]);
      }
      break;
    }

    case EFFECT_FIRE: {
      updateFlicker(0.15f, 1.0f, 0.12f * speedFactor, 0.3f);
      for (uint16_t i = 0; i < numLeds; i++) {
        float h = flickerLevel[i];
        frame[i] = {(uint8_t)(255 * min(1.0f, h * 1.4f)), (uint8_t)(150 * h * h), (uint8_t)(20 * h * h * h)};
      }
      break;
    }

    case EFFECT_CUSTOM:
    default:
      for (uint16_t i = 0; i < numLeds; i++) {
        frame[i] = customColors[i];
      }
      break;
  }
}

void renderFrame() {
  unsigned long now = millis();
  float dtMs = (float)(now - lastFrameMillis);
  lastFrameMillis = now;

  float fadeStep = dtMs / POWER_FADE_MS;
  powerLevel = powerOn ? min(1.0f, powerLevel + fadeStep) : max(0.0f, powerLevel - fadeStep);

  renderEffect(dtMs);

  // Squared brightness curve so the low end of the slider is usable;
  // gamma-correct each channel so colors match the WebUI picker.
  float b = brightnessPercent / 100.0f;
  float scale = (0.008f + 0.992f * b * b) * powerLevel;
  for (uint16_t i = 0; i < numLeds; i++) {
    pixels.setPixelColor(i,
                         (uint8_t)(Adafruit_NeoPixel::gamma8(frame[i].r) * scale),
                         (uint8_t)(Adafruit_NeoPixel::gamma8(frame[i].g) * scale),
                         (uint8_t)(Adafruit_NeoPixel::gamma8(frame[i].b) * scale));
  }
  pixels.show();
}

// ---------------------------------------------------------------------------
// IR remote
// ---------------------------------------------------------------------------

// NEC: 9ms mark + 4.5ms space leader, 32 bits LSB-first (560us mark, then
// 560us space = 0 / 1690us space = 1). A held key sends 9ms mark + 2.25ms
// space + 560us mark roughly every 108ms. TSOP output is active-low, so a
// rising edge ends a mark and a falling edge ends a space.
void IRAM_ATTR irIsr() {
  uint32_t now = micros();
  uint32_t dt = now - irLastEdgeUs;
  irLastEdgeUs = now;
  bool markEnded = digitalRead(PIN_IR) == HIGH;

  if (dt > 15000) {
    irDecodeState = 0;
  }

  if (markEnded) {
    switch (irDecodeState) {
      case 0:
        if (dt > 8000 && dt < 10000) {
          irDecodeState = 1;
        }
        break;
      case 2:
        irDecodeState = (dt > 350 && dt < 800) ? 3 : 0;
        break;
      case 4:
        if (dt > 350 && dt < 800) {
          irRepeatReady = true;
        }
        irDecodeState = 0;
        break;
      default:
        irDecodeState = 0;
        break;
    }
  } else {
    switch (irDecodeState) {
      case 1:
        if (dt > 4000 && dt < 5000) {
          irDecodeState = 2;
          irBitCount = 0;
          irShiftData = 0;
        } else if (dt > 1900 && dt < 2700) {
          irDecodeState = 4;
        } else {
          irDecodeState = 0;
        }
        break;
      case 3:
        if (dt > 1300 && dt < 2000) {
          irShiftData |= (1UL << irBitCount);
        } else if (!(dt > 350 && dt < 800)) {
          irDecodeState = 0;
          break;
        }
        irBitCount++;
        if (irBitCount >= 32) {
          irReceivedCode = irShiftData;
          irCodeReady = true;
          irDecodeState = 0;
        } else {
          irDecodeState = 2;
        }
        break;
      default:
        irDecodeState = 0;
        break;
    }
  }
}

void runIrAction(uint8_t action) {
  switch (action) {
    case IR_POWER:
      setPower(!powerOn);
      break;
    case IR_BRIGHT_UP:
      setPower(true);
      setBrightness(brightnessPercent + 5);
      break;
    case IR_BRIGHT_DOWN:
      setBrightness(brightnessPercent - 5);
      break;
    case IR_EFFECT_NEXT:
      setPower(true);
      setEffect((effect + 1) % EFFECT_COUNT);
      break;
    case IR_EFFECT_PREV:
      setPower(true);
      setEffect((effect + EFFECT_COUNT - 1) % EFFECT_COUNT);
      break;
    case IR_COLOR_NEXT:
      setPower(true);
      colorPresetIndex = (colorPresetIndex + 1) % NUM_COLOR_PRESETS;
      setBaseColor(COLOR_PRESETS[colorPresetIndex]);
      if (effect == EFFECT_RAINBOW || effect == EFFECT_COLOR_CYCLE || effect == EFFECT_FIRE ||
          effect == EFFECT_CUSTOM) {
        setEffect(EFFECT_SOLID);
      }
      break;
    case IR_SPEED_UP:
      setSpeed(speedPercent + 5);
      break;
    case IR_SPEED_DOWN:
      setSpeed(speedPercent - 5);
      break;
  }
}

bool isRepeatableAction(int8_t action) {
  return action == IR_BRIGHT_UP || action == IR_BRIGHT_DOWN || action == IR_SPEED_UP || action == IR_SPEED_DOWN;
}

void handleIr() {
  if (!irEnabled) {
    irCodeReady = false;
    irRepeatReady = false;
    irLearningAction = -1;
    return;
  }

  if (irLearningAction >= 0 && millis() - irLearnStartMillis > IR_LEARN_TIMEOUT_MS) {
    irLearningAction = -1;
  }

  if (irCodeReady) {
    noInterrupts();
    uint32_t code = irReceivedCode;
    irCodeReady = false;
    irRepeatReady = false;
    interrupts();

    irLastCode = code;
    irLastCodeMillis = millis();

    if (irLearningAction >= 0) {
      // A code can only drive one action.
      for (uint8_t i = 0; i < IR_ACTION_COUNT; i++) {
        if (irCodes[i] == code) {
          irCodes[i] = 0;
        }
      }
      irCodes[irLearningAction] = code;
      irLearningAction = -1;
      irLastAction = -1;
      markDirty();
      return;
    }

    irLastAction = -1;
    for (uint8_t i = 0; i < IR_ACTION_COUNT; i++) {
      if (irCodes[i] != 0 && irCodes[i] == code) {
        runIrAction(i);
        irLastAction = (int8_t)i;
        irLastActionMillis = millis();
        break;
      }
    }
  }

  if (irRepeatReady) {
    irRepeatReady = false;
    if (irLearningAction < 0 && isRepeatableAction(irLastAction) &&
        millis() - irLastActionMillis < IR_REPEAT_WINDOW_MS) {
      runIrAction(irLastAction);
      irLastActionMillis = millis();
    }
  }
}

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------

extern const uint8_t logo_jpg_start[] asm("_binary_data_logo_jpg_start");
extern const uint8_t logo_jpg_end[] asm("_binary_data_logo_jpg_end");

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Logix Tea Light</title>
<style>
  * { box-sizing:border-box; }
  body { font-family: system-ui, sans-serif; background:#111; color:#eee; margin:0; padding:2rem 1rem; display:flex; flex-direction:column; align-items:center; gap:1rem; }
  .header { position:relative; width:100%; max-width:380px; text-align:center; }
  h1 { margin:0; font-size:1.4rem; }
  .version { margin-top:.25rem; font-size:.75rem; color:#888; }
  .gear { position:absolute; top:-.25rem; right:0; width:2rem; height:2rem; border:none; border-radius:50%; background:transparent; color:#ccc; font-size:1.25rem; cursor:pointer; line-height:2rem; padding:0; }
  .card { background:#1b1b1b; border-radius:12px; padding:1.25rem; width:100%; max-width:380px; box-shadow:0 2px 8px rgba(0,0,0,.4); }
  .logo { display:block; width:100%; max-width:200px; margin:0 auto 1rem; border-radius:8px; }
  .section-title { margin:0 0 .25rem; font-size:1rem; }
  .section-copy { margin:0 0 .9rem; color:#9aa; font-size:.82rem; }
  .ring-wrap { position:relative; width:240px; height:240px; margin:.25rem auto 1rem; border-radius:50%; background:radial-gradient(circle, #0f2a5a 0%, #0a1d40 70%, #08162f 100%); box-shadow:inset 0 0 0 3px #13305f; }
  .pix-slot { position:absolute; width:40px; height:40px; margin:-20px 0 0 -20px; }
  .pix-slot .led { position:absolute; inset:0; border-radius:8px; border:3px solid #eee; background:#222; pointer-events:none; display:flex; align-items:center; justify-content:center; font-size:11px; font-weight:700; color:rgba(255,255,255,.9); text-shadow:0 0 3px #000, 0 0 2px #000; }
  .pix-slot .pix { position:absolute; inset:0; width:100%; height:100%; margin:0; padding:0; border:none; opacity:0; cursor:pointer; }
  .power { position:absolute; left:50%; top:50%; width:84px; height:84px; margin:-42px 0 0 -42px; border-radius:50%; border:none; font-size:1rem; font-weight:600; cursor:pointer; color:#fff; background:#444; }
  .power.on { background:#2e7d32; box-shadow:0 0 18px rgba(46,125,50,.6); }
  .power:active { filter:brightness(.85); }
  .ctl { margin-bottom:1rem; }
  .ctl:last-child { margin-bottom:0; }
  .ctl-label { display:flex; justify-content:space-between; margin-bottom:.4rem; font-size:.9rem; color:#ccc; }
  input[type=range] { width:100%; accent-color:#2e7d32; }
  .color-row { display:flex; gap:.6rem; align-items:center; }
  .color-row input[type=color] { width:56px; height:40px; border:none; border-radius:8px; background:none; cursor:pointer; padding:0; flex-shrink:0; }
  .swatches { display:flex; flex-wrap:wrap; gap:.4rem; }
  .swatch { width:26px; height:26px; border-radius:50%; border:2px solid #333; cursor:pointer; padding:0; }
  .swatch:active { filter:brightness(.85); }
  .chips { display:grid; grid-template-columns:repeat(3, 1fr); gap:.45rem; }
  .chip { padding:.55rem .3rem; font-size:.85rem; border:1px solid #333; border-radius:8px; cursor:pointer; color:#ddd; background:#232323; }
  .chip.active { background:#2e7d32; border-color:#2e7d32; color:#fff; }
  .ir-row { display:flex; align-items:center; justify-content:space-between; gap:.5rem; padding:.5rem 0; border-bottom:1px solid #2a2a2a; font-size:.9rem; }
  .ir-row:last-child { border-bottom:none; }
  .ir-code { font-family:ui-monospace, monospace; font-size:.78rem; color:#9aa; flex:1; text-align:right; }
  .small-btn { padding:.35rem .7rem; font-size:.8rem; border:none; border-radius:6px; cursor:pointer; color:#fff; background:#555; flex-shrink:0; }
  .small-btn.learn { background:#8a4b00; }
  .small-btn.learning { background:#c77700; animation:pulse 1s infinite; }
  @keyframes pulse { 50% { opacity:.55; } }
  .ir-last { font-size:.82rem; color:#9aa; margin:0 0 .6rem; }
  .ir-last b { font-family:ui-monospace, monospace; color:#eee; }
  .settings { display:none; }
  .settings.show { display:block; }
  .field-row { display:flex; gap:.5rem; margin-bottom:.9rem; }
  .toggle-row { display:flex; align-items:center; justify-content:space-between; gap:.75rem; margin-bottom:.9rem; font-size:.9rem; color:#ccc; cursor:pointer; }
  .toggle-row small { display:block; color:#9aa; font-size:.78rem; margin-top:.15rem; }
  .switch { position:relative; width:44px; height:24px; flex-shrink:0; }
  .switch input { opacity:0; width:0; height:0; position:absolute; }
  .switch .knob { position:absolute; inset:0; border-radius:12px; background:#444; transition:background .15s; }
  .switch .knob::after { content:""; position:absolute; left:3px; top:3px; width:18px; height:18px; border-radius:50%; background:#eee; transition:transform .15s; }
  .switch input:checked + .knob { background:#2e7d32; }
  .switch input:checked + .knob::after { transform:translateX(20px); }
  .switch input:focus-visible + .knob { outline:2px solid #8ab4f8; outline-offset:2px; }
  .hidden { display:none; }
  .field-row input { flex:1; min-width:0; padding:.55rem .7rem; font-size:.95rem; border:1px solid #333; border-radius:8px; background:#232323; color:#eee; }
  .wide-btn { width:100%; padding:.7rem; font-size:.95rem; border:none; border-radius:8px; cursor:pointer; color:#fff; margin-bottom:.5rem; }
  .wifi-btn { background:#8a4b00; }
  .danger-btn { background:#7a1f1f; }
  .info { font-size:.8rem; color:#9aa; margin:.25rem 0 .9rem; line-height:1.5; }
  .status { min-height:1em; font-size:.8rem; color:#9aa; margin:.2rem 0 0; }
</style>
</head>
<body>
<div class="header">
  <h1>Logix Tea Light</h1>
  <div class="version">v{{VERSION}} &middot; <span id="hostLabel">{{HOSTNAME}}</span></div>
  <button class="gear" onclick="toggleSettings()" aria-label="Settings">&#9881;</button>
</div>

<div class="card settings" id="settings">
  <h2 class="section-title">Device Settings</h2>
  <p class="info" id="netInfo"></p>
  <div class="ctl-label"><span>Device name</span></div>
  <div class="field-row">
    <input type="text" id="hostnameInput" maxlength="32" value="{{HOSTNAME}}">
    <button class="small-btn" onclick="saveHostname()">Save</button>
  </div>
  <div class="ctl-label"><span>LED count (8 per board, up to 64 when daisy-chained)</span></div>
  <div class="field-row">
    <input type="number" id="ledCountInput" min="1" max="64">
    <button class="small-btn" onclick="saveLedCount()">Save</button>
  </div>
  <div class="toggle-row">
    <span>LED ring rotation<small>Turn the on-screen ring until LED 0 on screen sits over the first LED in the chain on the board.</small></span>
    <span class="field-row" style="margin:0">
      <button class="small-btn" onclick="rotateRing(-1)" aria-label="Rotate counter-clockwise">&#10226;</button>
      <button class="small-btn" onclick="rotateRing(1)" aria-label="Rotate clockwise">&#10227;</button>
    </span>
  </div>
  <label class="toggle-row">
    <span>IR remote receiver<small>Turn on only if this board has the TSOP38238 IR chip fitted.</small></span>
    <span class="switch"><input type="checkbox" id="irToggle"><span class="knob"></span></span>
  </label>
  <button class="wide-btn wifi-btn" onclick="configureWifi()">Configure WiFi</button>
  <button class="wide-btn danger-btn" onclick="restartDevice()">Restart</button>
  <p class="status" id="settingsStatus"></p>
</div>

<div class="card">
  <img class="logo" src="/logo.jpg" alt="LogixDIY">
  <div class="ring-wrap" id="ring">
    <button class="power" id="powerBtn" onclick="send({power:'toggle'})">OFF</button>
  </div>
  <div class="ctl">
    <div class="ctl-label"><span>Brightness</span><span id="brightnessValue"></span></div>
    <input type="range" id="brightness" min="1" max="100">
  </div>
  <div class="ctl">
    <div class="ctl-label"><span>Color</span><span id="colorValue"></span></div>
    <div class="color-row">
      <input type="color" id="color">
      <div class="swatches" id="swatches"></div>
    </div>
  </div>
</div>

<div class="card">
  <h2 class="section-title">Effects</h2>
  <p class="section-copy">The ring above previews the selected effect. Tap an LED on it to give that LED its own color (switches to Custom).</p>
  <div class="ctl"><div class="chips" id="effects"></div></div>
  <div class="ctl">
    <div class="ctl-label"><span>Speed</span><span id="speedValue"></span></div>
    <input type="range" id="speed" min="1" max="100">
  </div>
</div>

<div class="card hidden" id="irCard">
  <h2 class="section-title">IR Remote</h2>
  <p class="section-copy">Works with NEC-protocol remotes (most cheap LED remotes). Press Learn, then press the remote button within 15 seconds.</p>
  <p class="ir-last">Last code received: <b id="irLast">none</b></p>
  <div id="irList"></div>
</div>

<script>
const IR_LABELS = {power:'Power toggle', bright_up:'Brightness +', bright_down:'Brightness -', effect_next:'Next effect', effect_prev:'Previous effect', color_next:'Next color', speed_up:'Speed +', speed_down:'Speed -'};
const SWATCHES = ['#ff7814','#ffb45a','#ffffff','#ff0000','#ff5000','#ffdc00','#00ff00','#00ffc8','#0050ff','#8c00ff','#ff008c'];
let state = null;
let effects = [];
let dragging = {};
let pixelInputs = [];
let ledEls = [];

function $(id){ return document.getElementById(id); }

function send(params){
  const q = new URLSearchParams(params).toString();
  return fetch('/set?' + q).then(r => r.json()).then(apply).catch(() => {});
}

function throttle(fn, ms){
  let timer = null, pending = null;
  return function(arg){
    pending = arg;
    if (timer) return;
    fn(pending); pending = null;
    timer = setTimeout(function tick(){
      if (pending !== null) { fn(pending); pending = null; timer = setTimeout(tick, ms); }
      else timer = null;
    }, ms);
  };
}

function buildRing(count, rotation){
  const ring = $('ring');
  ring.querySelectorAll('.pix-slot').forEach(el => el.remove());
  pixelInputs = [];
  ledEls = [];
  const shown = Math.min(count, 8);
  const radius = 92;
  for (let i = 0; i < shown; i++) {
    // With rotation 0 the first LED in the chain is drawn at 9 o'clock (far left,
    // confirmed on the board by the user) and the
    // chain runs clockwise; ringRotation (Device Settings) shifts it per board.
    const angle = (180 + (i + rotation) * 360 / shown) * Math.PI / 180;
    const slot = document.createElement('div');
    slot.className = 'pix-slot';
    slot.style.left = (120 + radius * Math.cos(angle)) + 'px';
    slot.style.top = (120 + radius * Math.sin(angle)) + 'px';
    const led = document.createElement('div');
    led.className = 'led';
    led.textContent = i;
    const el = document.createElement('input');
    el.type = 'color';
    el.className = 'pix';
    el.title = 'LED ' + i;
    slot.appendChild(led);
    slot.appendChild(el);
    const sendPixel = throttle(v => fetch('/pixel/set?index=' + i + '&color=' + v.substring(1)).then(r => r.json()).then(apply).catch(() => {}), 150);
    el.addEventListener('focus', () => dragging['pix' + i] = true);
    el.addEventListener('blur', () => dragging['pix' + i] = false);
    el.addEventListener('input', () => {
      if (state && state.effect === 'custom') state.pixels[i] = el.value;
      sendPixel(el.value);
    });
    el.addEventListener('change', () => { dragging['pix' + i] = false; });
    ring.insertBefore(slot, $('powerBtn'));
    pixelInputs.push(el);
    ledEls.push(led);
  }
}

function apply(s){
  if (!s) return;
  const ledsChanged = !state || state.leds !== s.leds || state.ringRotation !== s.ringRotation;
  state = s;
  if (ledsChanged) {
    buildRing(s.leds, s.ringRotation);
    $('ledCountInput').value = s.leds;
  }
  const p = $('powerBtn');
  p.textContent = s.power ? 'ON' : 'OFF';
  p.classList.toggle('on', s.power);
  if (!dragging.brightness) $('brightness').value = s.brightness;
  $('brightnessValue').textContent = s.brightness + '%';
  if (!dragging.speed) $('speed').value = s.speed;
  $('speedValue').textContent = s.speed + '%';
  if (!dragging.color) $('color').value = s.color;
  $('colorValue').textContent = s.color;
  pixelInputs.forEach((el, i) => {
    if (!dragging['pix' + i]) el.value = s.effect === 'custom' ? s.pixels[i] : s.color;
  });
  document.querySelectorAll('.chip').forEach(c => c.classList.toggle('active', c.dataset.effect === s.effect));
  $('irToggle').checked = s.ir.enabled;
  $('irCard').classList.toggle('hidden', !s.ir.enabled);
  $('irLast').textContent = s.ir.last || 'none';
  renderIr(s.ir);
  $('netInfo').innerHTML = 'IP ' + s.ip + ' &middot; RSSI ' + s.rssi + ' dBm<br>http://' + s.hostname + '.local';
}

function renderIr(ir){
  const list = $('irList');
  if (!list.children.length) {
    Object.keys(IR_LABELS).forEach(key => {
      const row = document.createElement('div');
      row.className = 'ir-row';
      row.innerHTML = '<span>' + IR_LABELS[key] + '</span><span class="ir-code" id="irc-' + key + '"></span>' +
        '<button class="small-btn learn" id="irl-' + key + '">Learn</button>' +
        '<button class="small-btn" id="irx-' + key + '">Clear</button>';
      list.appendChild(row);
      $('irx-' + key).onclick = () => fetch('/ir/clear?action=' + key).then(r => r.json()).then(apply);
    });
  }
  Object.keys(IR_LABELS).forEach(key => {
    $('irc-' + key).textContent = ir.codes[key] || '—';
    const b = $('irl-' + key);
    const learning = ir.learning === key;
    b.textContent = learning ? 'Waiting…' : 'Learn';
    b.classList.toggle('learning', learning);
    b.onclick = () => fetch(learning ? '/ir/cancel' : '/ir/learn?action=' + key).then(r => r.json()).then(apply);
  });
}

function bindSlider(id, key){
  const el = $(id);
  const push = throttle(v => send({[key]: v}), 120);
  el.addEventListener('pointerdown', () => dragging[id] = true);
  el.addEventListener('pointerup', () => dragging[id] = false);
  el.addEventListener('input', () => {
    $(id + 'Value').textContent = el.value + '%';
    if (state) state[key] = +el.value;
    push(el.value);
  });
  el.addEventListener('change', () => { dragging[id] = false; send({[key]: el.value}); });
}

function bindColor(){
  const el = $('color');
  const push = throttle(v => send({color: v.substring(1), power: 'on'}), 150);
  el.addEventListener('focus', () => dragging.color = true);
  el.addEventListener('blur', () => dragging.color = false);
  el.addEventListener('input', () => {
    if (state) state.color = el.value;
    push(el.value);
  });
  el.addEventListener('change', () => { dragging.color = false; });
  const sw = $('swatches');
  SWATCHES.forEach(c => {
    const b = document.createElement('button');
    b.className = 'swatch';
    b.style.background = c;
    b.title = c;
    b.onclick = () => {
      const params = {color: c.substring(1), power: 'on'};
      if (state && ['rainbow','color_cycle','fire','custom'].includes(state.effect)) params.effect = 'solid';
      send(params);
    };
    sw.appendChild(b);
  });
}

function buildEffects(){
  const wrap = $('effects');
  effects.forEach(e => {
    const b = document.createElement('button');
    b.className = 'chip';
    b.dataset.effect = e.id;
    b.textContent = e.name;
    b.onclick = () => send({effect: e.id, power: 'on'});
    wrap.appendChild(b);
  });
}

function toggleSettings(){ $('settings').classList.toggle('show'); }

function settingsStatus(msg){ $('settingsStatus').textContent = msg; }

function saveHostname(){
  const name = $('hostnameInput').value.trim();
  if (!name) return;
  fetch('/config/hostname?name=' + encodeURIComponent(name)).then(r => r.text()).then(t => settingsStatus(t));
}

function rotateRing(step){
  fetch('/config/ring?step=' + step).then(r => r.json()).then(apply).catch(() => {});
}

function bindIrToggle(){
  $('irToggle').addEventListener('change', e => {
    $('irCard').classList.toggle('hidden', !e.target.checked);
    fetch('/config/ir?enabled=' + (e.target.checked ? 'on' : 'off')).then(r => r.json()).then(apply).catch(() => {});
  });
}

function saveLedCount(){
  send({leds: $('ledCountInput').value}).then(() => settingsStatus('LED count saved'));
}

function configureWifi(){
  fetch('/wifi-config').then(r => r.json()).then(d => {
    window.open('http://' + location.hostname + ':' + d.port + '/', '_blank');
  });
}

function restartDevice(){
  if (!confirm('Restart the tea light?')) return;
  fetch('/restart').then(() => settingsStatus('Restarting…'));
}

// Live preview: a port of renderEffect() in the firmware, driven by the last
// known state. Runs locally, so it approximates rather than mirrors the LEDs.
const sim = {t: 0, power: 0, last: 0, flick: [], targ: []};

function hexRgb(h){
  const v = parseInt(h.slice(1), 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}

function hueRgb(h){
  const x = ((h % 65536) + 65536) % 65536 / 65536 * 6;
  const f = x - Math.floor(x);
  const c = [[1, f, 0], [1 - f, 1, 0], [0, 1, f], [0, 1 - f, 1], [f, 0, 1], [1, 0, 1 - f]][Math.floor(x) % 6];
  return c.map(v => v * 255);
}

function scaleRgb(c, l){
  l = Math.max(0, Math.min(1, l));
  return [c[0] * l, c[1] * l, c[2] * l];
}

function updateFlicker(n, lo, hi, chance, ease, frames){
  for (let i = 0; i < n; i++) {
    if (Math.random() < chance * frames) sim.targ[i] = lo + (hi - lo) * Math.random();
    sim.flick[i] += (sim.targ[i] - sim.flick[i]) * Math.min(1, ease * frames);
  }
}

function simFrame(now){
  requestAnimationFrame(simFrame);
  const dt = Math.max(0, Math.min(100, sim.last ? now - sim.last : 16));
  sim.last = now;
  if (!state || !ledEls.length) return;
  const n = state.leds;
  while (sim.flick.length < n) { sim.flick.push(1); sim.targ.push(1); }
  const frames = dt / 16;
  const sf = 0.15 + (state.speed / 100) * 2.85;
  sim.t += dt * sf;
  const t = sim.t;
  sim.power = state.power ? Math.min(1, sim.power + dt / 400) : Math.max(0, sim.power - dt / 400);
  const base = hexRgb(state.color);
  const out = [];
  switch (state.effect) {
    case 'candle':
      updateFlicker(n, 0.45, 1, 0.04 * sf, 0.12, frames);
      for (let i = 0; i < n; i++) { const l = sim.flick[i]; out.push([base[0] * l, base[1] * l * l, base[2] * l * l * l]); }
      break;
    case 'breathe': {
      const ph = (Math.sin(t * 0.0025) + 1) / 2;
      for (let i = 0; i < n; i++) out.push(scaleRgb(base, 0.08 + 0.92 * ph * ph));
      break;
    }
    case 'rainbow':
      for (let i = 0; i < n; i++) out.push(hueRgb(t * 12 + i * 65536 / n));
      break;
    case 'color_cycle': {
      const c = hueRgb(t * 6);
      for (let i = 0; i < n; i++) out.push(c);
      break;
    }
    case 'chase': {
      const head = (t * 0.006) % n;
      for (let i = 0; i < n; i++) {
        let behind = head - i;
        if (behind < 0) behind += n;
        const l = behind < 4 ? 1 - behind / 4 : 0;
        out.push(scaleRgb(base, 0.04 + 0.96 * l * l));
      }
      break;
    }
    case 'twinkle':
      for (let i = 0; i < n; i++) {
        if (sim.flick[i] < 0.2 && Math.random() < 0.008 * sf * frames) sim.flick[i] = 1;
        sim.flick[i] = Math.max(0.12, sim.flick[i] - 0.012 * sf * frames);
        out.push(scaleRgb(base, sim.flick[i]));
      }
      break;
    case 'fire':
      updateFlicker(n, 0.15, 1, 0.12 * sf, 0.3, frames);
      for (let i = 0; i < n; i++) { const h = sim.flick[i]; out.push([255 * Math.min(1, h * 1.4), 150 * h * h, 20 * h * h * h]); }
      break;
    case 'custom':
      for (let i = 0; i < n; i++) out.push(hexRgb(state.pixels[i] || '#000000'));
      break;
    default:
      for (let i = 0; i < n; i++) out.push(base);
  }
  // Screens are already gamma-encoded, so use a gentler brightness curve than
  // the LEDs do; keep a dark floor so an off LED still reads as an LED.
  const scale = (0.15 + 0.85 * state.brightness / 100) * sim.power;
  ledEls.forEach((el, i) => {
    const c = out[i].map(v => Math.round(v * scale));
    el.style.background = 'rgb(' + c.map(v => Math.max(v, 34)).join(',') + ')';
    const glow = Math.max(c[0], c[1], c[2]) / 255;
    el.style.boxShadow = glow > 0.05 ? '0 0 ' + Math.round(4 + 16 * glow) + 'px rgba(' + c.join(',') + ',' + (0.4 + 0.5 * glow).toFixed(2) + ')' : 'none';
  });
}

function refresh(){
  fetch('/status').then(r => r.json()).then(apply).catch(() => {});
}

fetch('/effects').then(r => r.json()).then(list => {
  effects = list;
  buildEffects();
  bindSlider('brightness', 'brightness');
  bindSlider('speed', 'speed');
  bindColor();
  bindIrToggle();
  refresh();
  setInterval(refresh, 2000);
  requestAnimationFrame(simFrame);
});
</script>
</body>
</html>
)HTML";

String effectId(uint8_t e) {
  String id = EFFECT_NAMES[e];
  id.toLowerCase();
  id.replace(" ", "_");
  return id;
}

int parseEffect(const String &value) {
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    if (value.equalsIgnoreCase(effectId(i)) || value.equalsIgnoreCase(EFFECT_NAMES[i])) {
      return i;
    }
  }
  if (value.length() > 0 && isdigit((unsigned char)value[0])) {
    int n = value.toInt();
    if (n >= 0 && n < EFFECT_COUNT) {
      return n;
    }
  }
  return -1;
}

int parseIrAction(const String &value) {
  for (uint8_t i = 0; i < IR_ACTION_COUNT; i++) {
    if (value == IR_ACTION_KEYS[i]) {
      return i;
    }
  }
  return -1;
}

String irCodeHex(uint32_t code) {
  if (code == 0) {
    return "";
  }
  char buf[11];
  snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)code);
  return String(buf);
}

String jsonEscape(const String &s) {
  String out = s;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  return out;
}

String statusJson() {
  String json;
  json.reserve(1024 + numLeds * 12);
  json += "{\"power\":";
  json += powerOn ? "true" : "false";
  json += ",\"color\":\"" + colorToHex(baseColor) + "\"";
  json += ",\"brightness\":" + String(brightnessPercent);
  json += ",\"effect\":\"" + effectId(effect) + "\"";
  json += ",\"speed\":" + String(speedPercent);
  json += ",\"leds\":" + String(numLeds);
  json += ",\"pixels\":[";
  for (uint16_t i = 0; i < numLeds; i++) {
    if (i) {
      json += ",";
    }
    json += "\"" + colorToHex(customColors[i]) + "\"";
  }
  json += "],\"ir\":{\"last\":\"" + irCodeHex(irLastCode) + "\",\"learning\":";
  json += irLearningAction >= 0 ? "\"" + String(IR_ACTION_KEYS[irLearningAction]) + "\"" : "null";
  json += ",\"codes\":{";
  for (uint8_t i = 0; i < IR_ACTION_COUNT; i++) {
    if (i) {
      json += ",";
    }
    json += "\"" + String(IR_ACTION_KEYS[i]) + "\":\"" + irCodeHex(irCodes[i]) + "\"";
  }
  json += "},\"enabled\":";
  json += irEnabled ? "true" : "false";
  json += "},\"ringRotation\":" + String(ringRotation);
  json += ",\"hostname\":\"" + jsonEscape(currentHostname) + "\"";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"version\":\"" + String(FW_VERSION) + "\"}";
  return json;
}

void sendStatus() {
  server.send(200, "application/json", statusJson());
}

void handleRoot() {
  String html = FPSTR(INDEX_HTML);
  html.replace("{{VERSION}}", FW_VERSION);
  html.replace("{{HOSTNAME}}", currentHostname);
  // Always revalidate so a firmware update never leaves a stale UI in the browser.
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/html", html);
}

void handleLogo() {
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "image/jpeg", (const char *)logo_jpg_start, logo_jpg_end - logo_jpg_start);
}

void handleEffects() {
  String json = "[";
  for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
    if (i) {
      json += ",";
    }
    json += "{\"id\":\"" + effectId(i) + "\",\"name\":\"" + EFFECT_NAMES[i] + "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// One endpoint for every light setting so a single request (e.g. from Home
// Assistant) can change several things at once. Unknown/invalid values -> 400.
void handleSet() {
  if (server.hasArg("leds")) {
    setNumLeds(server.arg("leds").toInt());
  }
  if (server.hasArg("color")) {
    Rgb c;
    if (!parseHexColor(server.arg("color"), c)) {
      server.send(400, "text/plain", "Invalid color, expected rrggbb");
      return;
    }
    setBaseColor(c);
  }
  if (server.hasArg("brightness")) {
    setBrightness(server.arg("brightness").toInt());
  }
  if (server.hasArg("speed")) {
    setSpeed(server.arg("speed").toInt());
  }
  if (server.hasArg("effect")) {
    int e = parseEffect(server.arg("effect"));
    if (e < 0) {
      server.send(400, "text/plain", "Unknown effect");
      return;
    }
    setEffect(e);
  }
  if (server.hasArg("power")) {
    String p = server.arg("power");
    if (p == "toggle") {
      setPower(!powerOn);
    } else {
      setPower(p == "on" || p == "1" || p == "true");
    }
  }
  sendStatus();
}

void handleOn() {
  setPower(true);
  sendStatus();
}

void handleOff() {
  setPower(false);
  sendStatus();
}

void handleToggle() {
  setPower(!powerOn);
  sendStatus();
}

void handlePixelSet() {
  Rgb c;
  if (!server.hasArg("index") || !server.hasArg("color") || !parseHexColor(server.arg("color"), c)) {
    server.send(400, "text/plain", "Expected index and color=rrggbb");
    return;
  }
  int index = server.arg("index").toInt();
  if (index < 0 || index >= numLeds) {
    server.send(400, "text/plain", "Index out of range");
    return;
  }
  setCustomPixel((uint16_t)index, c);
  setPower(true);
  sendStatus();
}

// colors=rrggbb,rrggbb,... sets pixels starting at 0 and switches to Custom.
void handlePixelsSet() {
  if (!server.hasArg("colors")) {
    server.send(400, "text/plain", "Expected colors=rrggbb,rrggbb,...");
    return;
  }
  String list = server.arg("colors");
  uint16_t index = 0;
  int start = 0;
  while (start <= (int)list.length() && index < numLeds) {
    int comma = list.indexOf(',', start);
    String item = comma < 0 ? list.substring(start) : list.substring(start, comma);
    Rgb c;
    if (!parseHexColor(item, c)) {
      server.send(400, "text/plain", "Invalid color at index " + String(index));
      return;
    }
    setCustomPixel(index++, c);
    if (comma < 0) {
      break;
    }
    start = comma + 1;
  }
  setPower(true);
  sendStatus();
}

void handleIrLearn() {
  int action = parseIrAction(server.arg("action"));
  if (action < 0) {
    server.send(400, "text/plain", "Unknown action");
    return;
  }
  irLearningAction = (int8_t)action;
  irLearnStartMillis = millis();
  sendStatus();
}

void handleIrCancel() {
  irLearningAction = -1;
  sendStatus();
}

void handleIrClear() {
  int action = parseIrAction(server.arg("action"));
  if (action < 0) {
    server.send(400, "text/plain", "Unknown action");
    return;
  }
  irCodes[action] = 0;
  markDirty();
  sendStatus();
}

// step=1|-1 rotates the WebUI ring one LED clockwise/counter-clockwise;
// value=0..7 sets it directly.
void handleRingConfig() {
  if (server.hasArg("value")) {
    ringRotation = (uint8_t)(((server.arg("value").toInt() % 8) + 8) % 8);
  } else if (server.hasArg("step")) {
    ringRotation = (uint8_t)(((ringRotation + server.arg("step").toInt()) % 8 + 8) % 8);
  } else {
    server.send(400, "text/plain", "Expected step=1|-1 or value=0..7");
    return;
  }
  markDirty();
  sendStatus();
}

void handleIrConfig() {
  if (!server.hasArg("enabled")) {
    server.send(400, "text/plain", "Expected enabled=on|off");
    return;
  }
  String v = server.arg("enabled");
  irEnabled = v == "on" || v == "1" || v == "true";
  markDirty();
  sendStatus();
}

void handleHostname() {
  String name = sanitizeHostname(server.arg("name"));
  if (name.length() == 0) {
    server.send(400, "text/plain", "Invalid name (use letters, digits and dashes)");
    return;
  }
  if (name == currentHostname) {
    server.send(200, "text/plain", "Name unchanged");
    return;
  }
  requestHostnameChange(name);
  server.send(200, "text/plain", "Saved, rebooting as http://" + name + ".local");
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  scheduleRestart();
}

void handleForgetWifi() {
  server.send(200, "text/plain", "Wifi credentials cleared, rebooting...");
  delay(500);
  wm.resetSettings();
  ESP.restart();
}

void handleWifiConfig() {
  if (!wifiPortalActive) {
    wm.setHttpPort(WIFI_PORTAL_PORT);
    wm.startWebPortal();
    wifiPortalActive = true;
  }
  wifiPortalStartMillis = millis();
  String json = "{\"port\":";
  json += WIFI_PORTAL_PORT;
  json += "}";
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/logo.jpg", handleLogo);
  server.on("/status", sendStatus);
  server.on("/effects", handleEffects);
  server.on("/set", handleSet);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/toggle", handleToggle);
  server.on("/pixel/set", handlePixelSet);
  server.on("/pixels/set", handlePixelsSet);
  server.on("/ir/learn", handleIrLearn);
  server.on("/ir/cancel", handleIrCancel);
  server.on("/ir/clear", handleIrClear);
  server.on("/config/hostname", handleHostname);
  server.on("/config/ir", handleIrConfig);
  server.on("/config/ring", handleRingConfig);
  server.on("/restart", handleRestart);
  server.on("/forget-wifi", handleForgetWifi);
  server.on("/wifi-config", handleWifiConfig);
  server.begin();
}

void setupOTA() {
  ArduinoOTA.setHostname(currentHostname.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    pixels.clear();
    pixels.show();
  });
  ArduinoOTA.begin();
}

// Called once the station connection comes up (immediately at boot, or later
// after the setup portal gets credentials).
void startNetworkServices() {
  networkStarted = true;
  setupPortalActive = false;
  WiFi.setTxPower(WIFI_TX_POWER);
  MDNS.begin(currentHostname.c_str());
  MDNS.addService("http", "tcp", 80);
  setupOTA();
  setupWebServer();
  Serial.printf("Connected: http://%s.local (%s)\n", currentHostname.c_str(), WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);

  loadState();

  pixels.updateLength(numLeds);
  pixels.begin();
  for (uint16_t i = 0; i < MAX_LEDS; i++) {
    flickerLevel[i] = flickerTarget[i] = 1.0f;
  }
  lastFrameMillis = millis();
  renderFrame();

  pinMode(PIN_IR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_IR), irIsr, CHANGE);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);
  String defaultHostname = String(HOSTNAME_PREFIX) + "-" + macSuffix();
  currentHostname = loadHostname(defaultHostname);

  hostnameParam = new WiFiManagerParameter("hostname", "Device Name", currentHostname.c_str(), 32);
  wm.addParameter(hostnameParam);
  wm.setSaveParamsCallback(onSaveParams);
  wm.setHostname(currentHostname.c_str());
  wm.setTitle("Configure Tea Light");
  wm.setDarkMode(true);
  wm.setConnectTimeout(20);
  // Non-blocking so the light (and IR remote) keeps working while the setup
  // hotspot is up waiting for credentials.
  wm.setConfigPortalBlocking(false);
  if (wm.autoConnect(AP_NAME, AP_PASSWORD)) {
    startNetworkServices();
  } else {
    // WiFiManager switched to AP+STA mode; re-apply the TX power limit.
    WiFi.setTxPower(WIFI_TX_POWER);
    setupPortalActive = true;
    setupPortalStartMillis = millis();
    Serial.printf("Setup hotspot \"%s\" active\n", AP_NAME);
  }
}

void loop() {
  if (networkStarted) {
    ArduinoOTA.handle();
    server.handleClient();
  }

  if (setupPortalActive || wifiPortalActive) {
    wm.process();
  }

  if (setupPortalActive) {
    if (WiFi.status() == WL_CONNECTED) {
      startNetworkServices();
    } else if (wm.getWiFiIsSaved() && millis() - setupPortalStartMillis >= SETUP_PORTAL_RETRY_MS) {
      // Saved network was probably just down at boot; retry from scratch.
      if (stateDirty) {
        saveState();
      }
      ESP.restart();
    }
  }

  if (wifiPortalActive && millis() - wifiPortalStartMillis >= WIFI_PORTAL_TIMEOUT_MS) {
    wm.stopWebPortal();
    wifiPortalActive = false;
  }

  handleIr();

  if (millis() - lastFrameMillis >= FRAME_INTERVAL_MS) {
    renderFrame();
  }

  if (stateDirty && millis() - stateDirtyMillis >= SAVE_DELAY_MS) {
    saveState();
  }

  if (restartPending && millis() - restartPendingMillis >= HOSTNAME_REBOOT_DELAY_MS) {
    if (stateDirty) {
      saveState();
    }
    ESP.restart();
  }
}
