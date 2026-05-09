#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

// QT Py ESP32-S3 STEMMA QT I2C pins in Arduino.
static constexpr uint8_t OLED_SCL = 40;
static constexpr uint8_t OLED_SDA = 41;
static constexpr uint8_t OLED_ADDR = 0x3C;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;

// Use the QT Py A0 pad for the flow pulse input.
// The YF-B7 signal must be level-shifted to 3.3 V before this pin.
static constexpr uint8_t FLOW_PIN = A0;

static constexpr char AP_SSID[] = "Debitmetre-YFB7";
static constexpr char AP_PASSWORD[] = "12345678";

static constexpr float DEFAULT_CALIBRATION_HZ_PER_L_MIN = 11.0f;
static constexpr uint32_t SAMPLE_MS = 1000;
static constexpr uint32_t DISPLAY_MS = 300;

TwoWire oledWire(1);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &oledWire, -1);
Preferences prefs;
WebServer server(80);

volatile uint32_t pulseCount = 0;
volatile uint32_t lastPulseMicros = 0;

float calibrationHzPerLMin = DEFAULT_CALIBRATION_HZ_PER_L_MIN;
double totalLiters = 0.0;
float flowLMin = 0.0f;
float flowMlSec = 0.0f;
uint32_t lastSampleMs = 0;
uint32_t lastDisplayMs = 0;
bool displayOk = false;

void IRAM_ATTR onFlowPulse() {
  const uint32_t now = micros();
  if (now - lastPulseMicros > 300) {
    pulseCount++;
    lastPulseMicros = now;
  }
}

String htmlPage() {
  String page;
  page.reserve(5200);
  page += F("<!doctype html><html lang='fr'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<meta http-equiv='refresh' content='5'>");
  page += F("<title>Debitmetre YF-B7</title><style>");
  page += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f7f7f4;color:#171717}");
  page += F("main{max-width:560px;margin:auto;padding:24px}h1{font-size:1.45rem;margin:0 0 18px}");
  page += F(".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.card{background:white;border:1px solid #ddd;border-radius:8px;padding:14px}");
  page += F(".label{font-size:.78rem;color:#626262;text-transform:uppercase;letter-spacing:.04em}.value{font-size:1.55rem;font-weight:700;margin-top:4px}");
  page += F("form{margin-top:18px;background:white;border:1px solid #ddd;border-radius:8px;padding:16px}");
  page += F("label{display:block;font-weight:650;margin:12px 0 6px}input{width:100%;box-sizing:border-box;font-size:1rem;padding:10px;border:1px solid #bbb;border-radius:6px}");
  page += F("button{margin-top:14px;font-size:1rem;border:0;border-radius:6px;background:#0f766e;color:white;padding:10px 14px;font-weight:700}");
  page += F("button.secondary{background:#525252}.hint{font-size:.9rem;color:#555;line-height:1.45}</style></head><body><main>");
  page += F("<h1>Debitmetre YF-B7</h1><section class='grid'>");
  page += F("<div class='card'><div class='label'>Debit</div><div class='value'>");
  page += String(flowLMin, 2);
  page += F(" L/min</div></div><div class='card'><div class='label'>Volume</div><div class='value'>");
  page += String(totalLiters, 3);
  page += F(" L</div></div></section>");
  page += F("<form method='post' action='/calibrate'><label>Facteur K</label>");
  page += F("<input name='k' inputmode='decimal' value='");
  page += String(calibrationHzPerLMin, 4);
  page += F("'><p class='hint'>Pour le YF-B7, la valeur de depart est 11 Hz par L/min. Si 1,000 L mesure donne 0,950 L, multiplie K par 0,950.</p>");
  page += F("<button type='submit'>Enregistrer</button></form>");
  page += F("<form method='post' action='/set-volume'><label>Volume actuel en litres</label>");
  page += F("<input name='liters' inputmode='decimal' value='");
  page += String(totalLiters, 3);
  page += F("'><button type='submit'>Corriger le volume</button></form>");
  page += F("<form method='post' action='/reset'><button class='secondary' type='submit'>Remise a zero</button></form>");
  page += F("<p class='hint'>Point d'acces: ");
  page += AP_SSID;
  page += F(" / ");
  page += AP_PASSWORD;
  page += F("<br>Adresse: http://192.168.4.1</p></main></body></html>");
  return page;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", htmlPage());
  });

  server.on("/calibrate", HTTP_POST, []() {
    if (server.hasArg("k")) {
      const float k = server.arg("k").toFloat();
      if (k > 0.1f && k < 1000.0f) {
        calibrationHzPerLMin = k;
        prefs.putFloat("k", calibrationHzPerLMin);
      }
    }
    redirectHome();
  });

  server.on("/set-volume", HTTP_POST, []() {
    if (server.hasArg("liters")) {
      totalLiters = max(0.0f, server.arg("liters").toFloat());
      prefs.putDouble("liters", totalLiters);
    }
    redirectHome();
  });

  server.on("/reset", HTTP_POST, []() {
    noInterrupts();
    pulseCount = 0;
    interrupts();
    totalLiters = 0.0;
    prefs.putDouble("liters", totalLiters);
    redirectHome();
  });

  server.begin();
}

void updateDisplay() {
  if (!displayOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("Debitmetre YF-B7"));

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(flowLMin, 2);
  display.setTextSize(1);
  display.print(F(" L/min"));

  display.setTextSize(2);
  display.setCursor(0, 38);
  display.print(totalLiters, 2);
  display.setTextSize(1);
  display.print(F(" L"));

  display.setCursor(80, 56);
  display.print(WiFi.softAPIP());
  display.display();
}

void sampleFlow() {
  const uint32_t now = millis();
  const uint32_t elapsedMs = now - lastSampleMs;
  if (elapsedMs < SAMPLE_MS) {
    return;
  }

  noInterrupts();
  const uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();

  const float elapsedSec = elapsedMs / 1000.0f;
  const float frequencyHz = pulses / elapsedSec;
  flowLMin = frequencyHz / calibrationHzPerLMin;
  flowMlSec = flowLMin * 1000.0f / 60.0f;
  totalLiters += flowLMin * elapsedSec / 60.0f;

  static uint8_t saveDivider = 0;
  if (++saveDivider >= 10) {
    prefs.putDouble("liters", totalLiters);
    saveDivider = 0;
  }

  lastSampleMs = now;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("flowmeter", false);
  calibrationHzPerLMin = prefs.getFloat("k", DEFAULT_CALIBRATION_HZ_PER_L_MIN);
  totalLiters = prefs.getDouble("liters", 0.0);

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onFlowPulse, RISING);

  oledWire.begin(OLED_SDA, OLED_SCL);
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (displayOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("Initialisation..."));
    display.display();
  }

  setupWeb();
  lastSampleMs = millis();

  Serial.println(F("Debitmetre YF-B7 pret"));
  Serial.print(F("AP: "));
  Serial.println(AP_SSID);
  Serial.print(F("IP: "));
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  sampleFlow();

  const uint32_t now = millis();
  if (now - lastDisplayMs >= DISPLAY_MS) {
    updateDisplay();
    lastDisplayMs = now;
  }
}
