#include <ArduinoOTA.h>
#include <ESP_I2S.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include "burp.h"
#include "secrets.h"
#include "constants.h"


I2SClass i2s;
Servo lid;
WebServer server(80);

String pendingCommand;  // command from the web page, run on the next loop()

bool sensorArmed = false;      // sensor starts paused; send 'p' to resume it
bool waitingForClear = false;  // after a burp (or arming), wait until nobody's in range
unsigned long lastBurpAt = 0;
float distanceCm = 80;         // latest smoothed reading
bool streamDistance = false;   // 'd' prints readings to Serial Monitor

// The lid task steps the servo toward lidTarget so it can move while the burp plays
volatile int lidAngle = CLOSED_ANGLE;
volatile int lidTarget = CLOSED_ANGLE;
volatile int lidStepMs = 0;

void moveLid(int angle, int msPerDegree) {
  lidStepMs = msPerDegree;
  lidTarget = angle;
}

void waitForLid() {
  while (lidAngle != lidTarget) {
    delay(5);
  }
}

void lidTask(void *) {
  for (;;) {
    int target = lidTarget;
    if (lidAngle == target) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int stepMs = lidStepMs;
    if (stepMs == 0) {
      lidAngle = target;
    } else {
      lidAngle += (target > lidAngle) ? 1 : -1;
    }
    lid.write(lidAngle);
    vTaskDelay(pdMS_TO_TICKS(stepMs > 0 ? stepMs : 1));
  }
}

void printHelp();

size_t dataStart = 0;  // where the samples begin inside burp_wav
size_t dataEnd = 0;    // where they end
uint32_t sampleRate = 16000;

uint16_t read16(size_t p) {
  return burp_wav[p] | (burp_wav[p + 1] << 8);
}

uint32_t read32(size_t p) {
  return burp_wav[p] | (burp_wav[p + 1] << 8) | (burp_wav[p + 2] << 16) | ((uint32_t)burp_wav[p + 3] << 24);
}

// Find the format info and the audio samples inside the WAV file
bool parseWav() {
  if (burp_wav_len < 12 || memcmp(burp_wav, "RIFF", 4) || memcmp(burp_wav + 8, "WAVE", 4)) {
    Serial.println("burp.h doesn't contain a WAV file");
    return false;
  }
  size_t pos = 12;
  while (pos + 8 <= burp_wav_len) {
    uint32_t size = read32(pos + 4);
    if (!memcmp(burp_wav + pos, "fmt ", 4)) {
      uint16_t channels = read16(pos + 10);
      sampleRate = read32(pos + 12);
      uint16_t bits = read16(pos + 22);
      Serial.printf("WAV: %u Hz, %u channel(s), %u-bit\n", sampleRate, channels, bits);
      if (channels != 1 || bits != 16) {
        Serial.println("Re-export as mono, 16-bit PCM");
        return false;
      }
    } else if (!memcmp(burp_wav + pos, "data", 4)) {
      dataStart = pos + 8;
      dataEnd = min((size_t)(dataStart + size), (size_t)burp_wav_len);
      return true;
    }
    pos += 8 + size + (size & 1);
  }
  Serial.println("No audio data found in WAV");
  return false;
}

void playBurp() {
  int16_t buf[512];  // stereo frames: left, right, left, right...
  size_t n = 0;

  for (size_t i = dataStart; i + 1 < dataEnd; i += 2) {
    int16_t s = (int16_t)(burp_wav[i] | (burp_wav[i + 1] << 8));
    buf[n++] = s;  // left
    buf[n++] = s;  // right
    if (n == 512) {
      i2s.write((uint8_t *)buf, sizeof(buf));
      n = 0;
    }
  }
  if (n > 0) {
    i2s.write((uint8_t *)buf, n * sizeof(int16_t));
  }

  // Follow with a little silence so the end of the burp doesn't repeat
  memset(buf, 0, sizeof(buf));
  for (int k = 0; k < 4; k++) {
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
}

void burpSequence() {
  Serial.println("BURP!");
  rgbLedWrite(RGB_BUILTIN, 0, 60, 0);

  // Raise the lid and burp while it's rising
  moveLid(OPEN_ANGLE, OPEN_MS_PER_DEGREE);
  delay(OPEN_BEFORE_BURP_MS);
  playBurp();
  waitForLid();
  delay(HOLD_AFTER_BURP_MS);

  // Close, then rattle like something's still in there
  moveLid(CLOSED_ANGLE, CLOSE_MS_PER_DEGREE);
  waitForLid();
  delay(250);
  for (int i = 0; i < RATTLES; i++) {
    moveLid(RATTLE_ANGLE, 0);
    delay(RATTLE_MS);
    moveLid(CLOSED_ANGLE, 0);
    delay(RATTLE_MS + random(0, 120));
  }

  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
}

void printWifiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected to \"%s\", IP address %s, signal %d dBm\n",
                  WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("Also reachable as %s.local\n", HOSTNAME);
  } else {
    Serial.printf("Wi-Fi not connected to \"%s\" (status %d)\n", WIFI_SSID, WiFi.status());
  }
}

// Join the network in secrets.h; burping still works if this fails
void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Check the name and password in secrets.h. It will keep retrying in the background.");
  }
  printWifiStatus();
}

const char INDEX_HTML[] = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Burping Toilet</title>
<style>
:root{--bg:#f4f6f3;--card:#fff;--text:#1d2420;--muted:#5b665f;--accent:#2f8f4e;--accent-text:#fff;--line:#d9dfda}
@media (prefers-color-scheme:dark){:root{--bg:#121614;--card:#1c221f;--text:#e8eee9;--muted:#9aa69e;--accent:#5fd27f;--accent-text:#0d1a12;--line:#2c3530}}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}
main{max-width:420px;margin:0 auto;padding:20px 16px}
h1{font-size:1.4rem;margin:0 0 4px}
#info{color:var(--muted);margin:0 0 20px;min-height:1.4em}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
button{font:inherit;font-weight:600;padding:18px 12px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--text)}
button:active{transform:scale(.97)}
#burp{grid-column:span 2;background:var(--accent);color:var(--accent-text);border-color:var(--accent);font-size:1.25rem;padding:26px}
label{display:block;margin:24px 0 8px;color:var(--muted)}
input[type=range]{width:100%}
</style></head><body><main>
<h1>Burping Toilet</h1>
<p id="info">Connecting...</p>
<div class="grid">
<button id="burp" onclick="send('b')">Burp!</button>
<button onclick="send('o')">Open lid</button>
<button onclick="send('c')">Close lid</button>
<button onclick="send('s')">Sound only</button>
<button id="arm" onclick="send('p')">Sensor</button>
</div>
<label for="angle">Servo angle: <span id="angleVal">-</span>&deg;</label>
<input id="angle" type="range" min="0" max="180" step="1">
</main><script>
const $ = id => document.getElementById(id);
function send(c) {
  fetch('/cmd', {method: 'POST', body: new URLSearchParams({c})}).then(() => setTimeout(poll, 300));
}
function poll() {
  fetch('/status').then(r => r.json()).then(s => {
    const state = !s.armed ? 'Sensor paused'
      : s.waiting ? 'Sensor on - waiting for the area to clear'
      : 'Sensor on - waiting for a victim';
    $('info').textContent = state + ' (' + (s.dist >= 80 ? '80+' : s.dist) + ' cm)';
    $('arm').textContent = s.armed ? 'Pause sensor' : 'Resume sensor';
    if (document.activeElement !== $('angle')) {
      $('angle').value = s.lid;
      $('angleVal').textContent = s.lid;
    }
  }).catch(() => $('info').textContent = 'Not connected');
}
$('angle').oninput = e => $('angleVal').textContent = e.target.value;
$('angle').onchange = e => send('a' + e.target.value);
poll();
setInterval(poll, 1000);
</script></body></html>
)rawliteral";

void startWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", INDEX_HTML);
  });
  server.on("/cmd", HTTP_POST, []() {
    pendingCommand = server.arg("c");
    server.send(200, "text/plain", "ok");
  });
  server.on("/status", HTTP_GET, []() {
    String json = String("{\"armed\":") + (sensorArmed ? "true" : "false") +
                  ",\"waiting\":" + (waitingForClear ? "true" : "false") +
                  ",\"dist\":" + (int)distanceCm + ",\"lid\":" + lidTarget + "}";
    server.send(200, "application/json", json);
  });
  server.begin();
}

void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() {
    sensorArmed = false;
    moveLid(CLOSED_ANGLE, 0);
    Serial.println("Wireless upload starting");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    int pct = done * 100 / total;
    if (pct / 10 != lastPct / 10) {
      Serial.printf("Upload %d%%\n", pct);
    }
    lastPct = pct;
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("Upload done, restarting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Wireless upload failed (error %u)\n", error);
  });
  ArduinoOTA.begin();  // also starts mDNS as HOSTNAME.local
  MDNS.addService("http", "tcp", 80);
}

// Start OTA and mDNS the first time Wi-Fi is up (at boot or after a late connect)
void startNetworkServices() {
  static bool started = false;
  if (started || WiFi.status() != WL_CONNECTED) return;
  startOta();
  started = true;
  Serial.printf("Remote control: http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  lid.setPeriodHertz(50);
  lid.attach(SERVO_PIN, 500, 2500);
  lid.write(CLOSED_ANGLE);
  xTaskCreatePinnedToCore(lidTask, "lid", 4096, NULL, 2, NULL, 1);

  if (!parseWav()) {
    while (true) {}
  }

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S failed to start");
    while (true) {}
  }

  startWifi();
  startWebServer();
  startNetworkServices();

  if (sensorArmed) {
    waitingForClear = true;
    Serial.println("Ready. Sensor on");
  } else {
    Serial.println("Ready. Sensor is paused - send 'p' to resume it");
  }
  printHelp();
}

// Convert the sensor's output voltage to an approximate distance (accurate from 10 to 80 cm)
float readDistanceCm() {
  float volts = analogReadMilliVolts(SENSOR_PIN) / 1000.0;
  if (volts < 0.4) return 80;
  return constrain(27.86 * pow(volts, -1.15), 10, 80);
}

void setArmed(bool armed) {
  sensorArmed = armed;
  waitingForClear = armed;  // don't burp at whoever just resumed it
  Serial.println(armed ? "Sensor resumed" : "Sensor paused");
}

void checkSensor() {
  static unsigned long lastRead = 0;
  static int closeCount = 0;
  if (millis() - lastRead < SENSOR_INTERVAL_MS) return;
  lastRead = millis();

  // Smooth out the sensor's jitter
  distanceCm = distanceCm * 0.6 + readDistanceCm() * 0.4;
  if (streamDistance) {
    Serial.printf("Distance: %.0f cm\n", distanceCm);
  }

  if (waitingForClear) {
    if (distanceCm > CLEAR_CM) {
      waitingForClear = false;
      if (sensorArmed) Serial.println("All clear - waiting for a victim");
    }
    return;
  }

  closeCount = (distanceCm < TRIGGER_CM) ? closeCount + 1 : 0;
  if (!sensorArmed || closeCount < TRIGGER_READINGS) return;
  if (lastBurpAt != 0 && millis() - lastBurpAt < COOLDOWN_MS) return;

  Serial.printf("Someone's here (%.0f cm)\n", distanceCm);
  closeCount = 0;
  burpSequence();
  lastBurpAt = millis();
  waitingForClear = true;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  o      open lid");
  Serial.println("  c      close lid");
  Serial.println("  a<n>   move servo to angle n, e.g. a25");
  Serial.println("  s      play burp sound only");
  Serial.println("  b      run the full burp sequence");
  Serial.println("  p      pause / resume the motion sensor");
  Serial.println("  d      start / stop printing distance readings");
  Serial.println("  w      show Wi-Fi status");
  Serial.println("  ?      show this list");
}

// Act on one command, from Serial Monitor or the web page
void runCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  switch (cmd[0]) {
    case 'o':
      moveLid(OPEN_ANGLE, OPEN_MS_PER_DEGREE);
      Serial.printf("Lid open (%d)\n", OPEN_ANGLE);
      break;
    case 'c':
      moveLid(CLOSED_ANGLE, CLOSE_MS_PER_DEGREE);
      Serial.printf("Lid closed (%d)\n", CLOSED_ANGLE);
      break;
    case 'a': {
      int angle = constrain(cmd.substring(1).toInt(), 0, 180);
      moveLid(angle, 0);
      Serial.printf("Servo at %d\n", angle);
      break;
    }
    case 's':
      Serial.println("Burp sound");
      playBurp();
      break;
    case 'b':
      burpSequence();
      break;
    case 'p':
      setArmed(!sensorArmed);
      break;
    case 'd':
      streamDistance = !streamDistance;
      if (!streamDistance) Serial.println("Distance readings off");
      break;
    case 'w':
      printWifiStatus();
      break;
    default:
      printHelp();
  }
}

void loop() {
  if (Serial.available()) {
    runCommand(Serial.readStringUntil('\n'));
  }
  if (pendingCommand.length() > 0) {
    String cmd = pendingCommand;
    pendingCommand = "";
    runCommand(cmd);
  }

  startNetworkServices();
  ArduinoOTA.handle();
  server.handleClient();

  checkSensor();
}
