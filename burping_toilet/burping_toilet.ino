#include <ArduinoOTA.h>
#include <ESP_I2S.h>
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include "burp.h"
#include "cert.h"
#include "goat.h"
#include "httpd_stream.h"
#include "page.h"
#include "secrets.h"
#include "sound.h"
#include "constants.h"

// The committed secrets.h has no keys; these keep a fresh checkout compiling
#ifndef LOCAL_LLM_API_KEY
#define LOCAL_LLM_API_KEY ""
#endif
#ifndef ELEVENLABS_API_KEY
#define ELEVENLABS_API_KEY ""
#endif


I2SClass i2s;
Servo lid;
WebServer server(80);
httpd_handle_t httpsServer = NULL;  // HTTPS on port 443, needed for the phone's microphone

// Commands from the web pages, run one at a time in loop(). A queue because the
// HTTPS server runs its handlers on its own task.
const int COMMAND_LEN = 32;
QueueHandle_t commandQueue;

void queueCommand(const char *cmd) {
  char buf[COMMAND_LEN];
  strlcpy(buf, cmd, sizeof(buf));
  xQueueSend(commandQueue, buf, 0);
}

// Speech from /say or /speak, handed to loop() to speak. Holds at most one at a time.
QueueHandle_t speechQueue;
volatile bool talking = false;   // from taking a request until the lid closes again
volatile bool speaking = false;  // audio is actually playing (the voice may still be generating before this)
char voiceError[160] = "";       // why the last voice request failed, shown on the Talk page
bool mouthOn = MOUTH_ENABLED;    // 'm' toggles it without re-uploading
bool compressorOn = true;        // 'k' toggles the speech compressor, for testing

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

// All sounds must be mono, 16-bit, SAMPLE_RATE WAV files:
//   ffmpeg -i in.mp3 -ac 1 -ar 16000 -c:a pcm_s16le -map_metadata -1 name.wav
//   xxd -i name.wav > name.h   (then make the array const)
const uint32_t SAMPLE_RATE = 16000;

// Played in this order, one per trigger
Sound sounds[] = {
  {"burp", burp_wav, burp_wav_len, 0, 0},
  {"screaming goats", goat_wav, goat_wav_len, 0, 0},
};
const int SOUND_COUNT = sizeof(sounds) / sizeof(sounds[0]);
int nextSound = 0;

uint16_t read16(const unsigned char *b, size_t p) {
  return b[p] | (b[p + 1] << 8);
}

uint32_t read32(const unsigned char *b, size_t p) {
  return b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24);
}

// Find the format info and the audio samples inside a WAV file
bool parseWav(Sound &snd) {
  const unsigned char *b = snd.wav;
  if (snd.len < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
    Serial.printf("%s: not a WAV file\n", snd.name);
    return false;
  }
  size_t pos = 12;
  while (pos + 8 <= snd.len) {
    uint32_t size = read32(b, pos + 4);
    if (!memcmp(b + pos, "fmt ", 4)) {
      uint16_t channels = read16(b, pos + 10);
      uint32_t rate = read32(b, pos + 12);
      uint16_t bits = read16(b, pos + 22);
      Serial.printf("%s: %u Hz, %u channel(s), %u-bit\n", snd.name, rate, channels, bits);
      if (channels != 1 || bits != 16 || rate != SAMPLE_RATE) {
        Serial.printf("%s: re-export as mono, 16-bit PCM, %u Hz\n", snd.name, SAMPLE_RATE);
        return false;
      }
    } else if (!memcmp(b + pos, "data", 4)) {
      snd.dataStart = pos + 8;
      snd.dataEnd = min((size_t)(snd.dataStart + size), (size_t)snd.len);
      return true;
    }
    pos += 8 + size + (size & 1);
  }
  Serial.printf("%s: no audio data found\n", snd.name);
  return false;
}

// Play the next sound in the rotation
void playNextSound() {
  Sound &snd = sounds[nextSound];
  nextSound = (nextSound + 1) % SOUND_COUNT;
  Serial.printf("Playing %s\n", snd.name);
  playSound(snd);
}

// Plays 16-bit little-endian mono samples as they arrive, whether from memory or
// streamed from a server (HTTPClient::writeToStream). For speech it also evens out
// the volume (a compressor, so quiet syllables come up toward loud ones), and with
// `mouth` the lid follows the loudness between MOUTH_CLOSED_ANGLE and MOUTH_OPEN_ANGLE.
class SamplePlayer : public Stream {
 public:
  SamplePlayer(bool speech, bool mouth, uint32_t rate = SAMPLE_RATE) : speech_(speech), mouth_(mouth) {
    // Envelope follower: reacts to a loud syllable in ~2 ms, eases off over ~150 ms
    attack_ = 1 - expf(-1.0f / (0.002f * rate));
    release_ = 1 - expf(-1.0f / (0.150f * rate));
  }

  size_t write(const uint8_t *data, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      if (!haveLowByte_) {  // a sample can be split across network packets
        lowByte_ = data[i];
        haveLowByte_ = true;
        continue;
      }
      haveLowByte_ = false;
      addSample((int16_t)(lowByte_ | (data[i] << 8)));
    }
    return size;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }

  // Play what's left, then a little silence so the end of the sound doesn't repeat
  void finish() {
    if (n_ > 0) i2s.write((uint8_t *)buf_, n_ * sizeof(int16_t));
    n_ = 0;
    memset(buf_, 0, sizeof(buf_));
    for (int k = 0; k < 4; k++) i2s.write((uint8_t *)buf_, sizeof(buf_));
  }

  // Write-only stream
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  // Rounds off peaks above SPEECH_TARGET_PEAK instead of clipping them, which would crackle
  static int16_t softClip(float v) {
    const float knee = SPEECH_TARGET_PEAK, room = 32767 - SPEECH_TARGET_PEAK;
    float a = fabsf(v);
    if (a > knee) a = knee + room * tanhf((a - knee) / room);
    return (int16_t)lroundf(copysignf(a, v));
  }

  void addSample(int16_t in) {
    int16_t s = in;
    if (speech_) speaking = true;
    if (speech_ && compressorOn) {
      float mag = abs(in);
      envelope_ += (mag - envelope_) * (mag > envelope_ ? attack_ : release_);
      float gain = envelope_ > 1 ? min(SPEECH_MAX_GAIN, SPEECH_TARGET_PEAK / envelope_) : SPEECH_MAX_GAIN;
      s = softClip(in * gain);
    }
    buf_[n_++] = s;  // left
    buf_[n_++] = s;  // right
    sumSquares_ += (float)s * s;
    if (n_ < BUF_LEN) return;
    if (mouth_) {
      float level = min(1.0f, sqrtf(sumSquares_ / (BUF_LEN / 2)) / 32768 * MOUTH_GAIN);
      mouthLevel_ = max(level, mouthLevel_ * 0.7f);  // open fast, close a little slower
      moveLid(MOUTH_CLOSED_ANGLE + mouthLevel_ * (MOUTH_OPEN_ANGLE - MOUTH_CLOSED_ANGLE), MOUTH_MS_PER_DEGREE);
    }
    i2s.write((uint8_t *)buf_, sizeof(buf_));
    n_ = 0;
    sumSquares_ = 0;
  }

  static const size_t BUF_LEN = 512;  // stereo frames: left, right, left, right...
  int16_t buf_[BUF_LEN];
  size_t n_ = 0;
  float sumSquares_ = 0;
  float mouthLevel_ = 0;  // smoothed loudness, 0..1
  float envelope_ = 0;    // compressor's running estimate of the speech level
  float attack_, release_;
  bool speech_;
  bool mouth_;
  uint8_t lowByte_ = 0;
  bool haveLowByte_ = false;
};

void playSound(const Sound &snd) {
  playSamples(snd.wav + snd.dataStart, snd.dataEnd - snd.dataStart, false);
}

// `speech` evens out the volume and moves the lid like a mouth (sound files play as-is)
void playSamples(const uint8_t *pcm, size_t bytes, bool speech) {
  SamplePlayer player(speech, speech && mouthOn);
  player.write(pcm, bytes);
  player.finish();
}

void burpSequence() {
  Serial.println("BURP!");
  rgbLedWrite(RGB_BUILTIN, 0, 60, 0);

  // Raise the lid and play the sound while it's rising
  moveLid(OPEN_ANGLE, OPEN_MS_PER_DEGREE);
  delay(OPEN_BEFORE_BURP_MS);
  playNextSound();
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

// Lift the lid to "mouth" height, high enough that it doesn't muffle the speaker
void startTalking() {
  talking = true;
  speaking = false;
  rgbLedWrite(RGB_BUILTIN, 0, 0, 60);
  moveLid(mouthOn ? MOUTH_CLOSED_ANGLE : MOUTH_OPEN_ANGLE, OPEN_MS_PER_DEGREE);
  waitForLid();
}

void stopTalking() {
  moveLid(CLOSED_ANGLE, CLOSE_MS_PER_DEGREE);
  waitForLid();

  // The guest is probably still standing there; don't burp at them right away
  waitingForClear = true;
  lastBurpAt = millis();

  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  speaking = false;
  talking = false;
}

// Keep a server's error readable and safe to put inside the /status JSON
void setVoiceError(const String &msg) {
  size_t n = 0;
  for (size_t i = 0; i < msg.length() && n < sizeof(voiceError) - 1; i++) {
    char c = msg[i];
    voiceError[n++] = (c == '"' || c == '\\') ? '\'' : (c < 32 ? ' ' : c);
  }
  voiceError[n] = 0;
  Serial.printf("Voice error: %s\n", voiceError);
}

// Play an audio clip sent from the phone (/speak)
void playClip(const Speech &speech) {
  Serial.printf("Talking (%.1f s)\n", speech.bytes / 2.0 / SAMPLE_RATE);
  startTalking();
  speaking = true;
  playSamples(speech.data, speech.bytes, true);
  stopTalking();
}

// Speak a reply from the Talk page (/say): ask Kokoro for the speech over plain HTTP
// on the home network and play it as it streams in, at Kokoro's own 24 kHz.
void sayWithKokoro(const Speech &request) {
  Serial.println("Asking Kokoro for the voice");
  voiceError[0] = 0;
  startTalking();

  String url = LOCAL_TTS_URL;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/v1/audio/speech";

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(RELAY_TIMEOUT_MS);
  if (strncmp(LOCAL_TTS_URL, "http://", 7) != 0 || !http.begin(url)) {
    setVoiceError("Set LOCAL_TTS_URL in constants.h to an http:// address");
  } else {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(request.data, request.bytes);
    if (code == 200) {
      i2s.configureTX(KOKORO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
      SamplePlayer player(true, mouthOn, KOKORO_SAMPLE_RATE);
      http.writeToStream(&player);
      player.finish();
      i2s.configureTX(SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    } else if (code > 0) {
      setVoiceError("Kokoro said HTTP " + String(code) + ": " + http.getString().substring(0, 120));
    } else {
      setVoiceError("Couldn't reach " + url + " (" + HTTPClient::errorToString(code) + ")");
    }
    http.end();
  }
  stopTalking();
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
  // Wi-Fi power saving wakes the radio in current bursts several times a second,
  // which the amp on the same supply turns into static. The toilet is plugged in anyway.
  WiFi.setSleep(false);
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

String statusJson() {
  return String("{\"armed\":") + (sensorArmed ? "true" : "false") +
         ",\"waiting\":" + (waitingForClear ? "true" : "false") +
         ",\"talking\":" + (talking || uxQueueMessagesWaiting(speechQueue) ? "true" : "false") +
         ",\"speaking\":" + (speaking ? "true" : "false") +
         ",\"mouth\":" + (mouthOn ? "true" : "false") +
         ",\"compressor\":" + (compressorOn ? "true" : "false") +
         ",\"voiceError\":\"" + voiceError + "\"" +
         ",\"dist\":" + (int)distanceCm + ",\"lid\":" + lidTarget + "}";
}

void startWebServer() {
  // The talk pages need the microphone, which only works over HTTPS
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", String("https://") + HOSTNAME + ".local/");
    server.send(302, "text/plain", "");
  });
  server.on("/talk", HTTP_GET, []() {
    server.sendHeader("Location", String("https://") + HOSTNAME + ".local/talk");
    server.send(302, "text/plain", "");
  });
  server.on("/debug", HTTP_GET, []() {
    server.send(200, "text/html", INDEX_HTML);
  });
  server.on("/cmd", HTTP_POST, []() {
    queueCommand(server.arg("c").c_str());
    server.send(200, "text/plain", "ok");
  });
  server.on("/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });
  server.begin();
}

esp_err_t httpsDebug(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// The same page at both: the root shows just the talk button, /talk adds the settings
esp_err_t httpsTalk(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, TALK_HTML, HTTPD_RESP_USE_STRLEN);
}

// Store a request body in PSRAM and hand it to loop() to speak
esp_err_t queueSpeech(httpd_req_t *req, size_t maxBytes, bool kokoro) {
  size_t len = req->content_len;
  if (len == 0 || len > maxBytes) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request is empty or too large");
  }
  if (talking || uxQueueSpacesAvailable(speechQueue) == 0) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "the toilet is already talking");
  }

  uint8_t *data = (uint8_t *)ps_malloc(len);
  if (!data) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
  }
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, (char *)data + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      free(data);
      return ESP_FAIL;  // connection dropped; the server closes it
    }
    got += r;
  }

  Speech speech = {data, len, kokoro};
  if (xQueueSend(speechQueue, &speech, 0) != pdTRUE) {
    free(data);
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "the toilet is already talking");
  }
  return httpd_resp_sendstr(req, "ok");
}

// Body is raw 16 kHz, 16-bit mono PCM, played as-is (handy for testing with curl)
esp_err_t httpsSpeak(httpd_req_t *req) {
  return queueSpeech(req, MAX_SPEECH_BYTES, false);
}

// Body is a Kokoro /v1/audio/speech request (JSON with the reply text and voice).
// The toilet fetches the audio itself: that's much faster than the phone fetching it
// and sending it back over this HTTPS connection, which the ESP32 can only push slowly.
esp_err_t httpsSay(httpd_req_t *req) {
  return queueSpeech(req, MAX_RELAY_REQUEST_BYTES, true);
}

// Settings the Talk page needs from the firmware. HTTPS only, since it includes the API keys.
esp_err_t httpsConfig(httpd_req_t *req) {
  String json = String("{\"localLlm\":\"") + LOCAL_LLM_URL +
                "\",\"localTts\":\"" + LOCAL_TTS_URL + "\",\"elevenlabsKey\":\"" + ELEVENLABS_API_KEY + "\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json.c_str(), json.length());
}

// Forwards a POST from the Talk page to a server on the home network over plain HTTP,
// and streams the server's response back. The Talk page is HTTPS, and browsers block
// HTTPS pages from calling http:// addresses. `setting` names the constant for errors.
esp_err_t relayPost(httpd_req_t *req, const char *base, const char *path, const char *apiKey,
                    size_t maxBytes, const char *setting) {
  if (strncmp(base, "http://", 7) != 0) {
    String msg = String("set ") + setting + " in constants.h to an http:// address";
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg.c_str());
  }
  size_t len = req->content_len;
  if (len == 0 || len > maxBytes) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request is empty or too large");
  }

  char *body = (char *)ps_malloc(len);
  if (!body) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
  }
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, body + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) {
      free(body);
      return ESP_FAIL;
    }
    got += r;
  }

  String url = base;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += path;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(RELAY_TIMEOUT_MS);
  if (!http.begin(url)) {
    free(body);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid server address");
  }
  const char *keep[] = {"Content-Type"};
  http.collectHeaders(keep, 1);
  http.addHeader("Content-Type", "application/json");
  if (strlen(apiKey) > 0) {
    http.addHeader("Authorization", String("Bearer ") + apiKey);
  }
  int code = http.POST((uint8_t *)body, len);
  free(body);

  if (code <= 0) {
    String msg = "Couldn't reach " + url + " (" + HTTPClient::errorToString(code) + ")";
    http.end();
    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_sendstr(req, msg.c_str());
  }

  char status[32];  // pass the server's status code and content type through
  snprintf(status, sizeof(status), "%d %s", code, code == 200 ? "OK" : "Upstream Error");
  String type = http.header("Content-Type");
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type.length() ? type.c_str() : "application/octet-stream");

  HttpdChunkStream out(req);
  http.writeToStream(&out);
  http.end();
  if (out.failed()) return ESP_FAIL;
  return httpd_resp_send_chunk(req, NULL, 0);  // end of response
}

// Chat replies from the local LLM (llama.cpp)
esp_err_t httpsLlm(httpd_req_t *req) {
  return relayPost(req, LOCAL_LLM_URL, "/v1/chat/completions", LOCAL_LLM_API_KEY,
                   MAX_RELAY_REQUEST_BYTES, "LOCAL_LLM_URL");
}

esp_err_t httpsStatus(httpd_req_t *req) {
  String json = statusJson();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.length());
}

// Body is form-encoded, e.g. "c=a25"
esp_err_t httpsCmd(httpd_req_t *req) {
  char body[64] = {0};
  int len = httpd_req_recv(req, body, min(req->content_len, sizeof(body) - 1));
  char cmd[COMMAND_LEN];
  if (len <= 0 || httpd_query_key_value(body, "c", cmd, sizeof(cmd)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing c");
  }
  queueCommand(cmd);
  return httpd_resp_sendstr(req, "ok");
}

void startHttpsServer() {
  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert = (const uint8_t *)SERVER_CERT;
  conf.servercert_len = sizeof(SERVER_CERT);  // includes the terminating null, as mbedTLS expects
  conf.prvtkey_pem = (const uint8_t *)SERVER_KEY;
  conf.prvtkey_len = sizeof(SERVER_KEY);
  conf.httpd.max_uri_handlers = 12;  // the default of 8 is too few for the routes below
  // Keep the server (and its CPU-heavy TLS handshakes) on core 0 with Wi-Fi. Left
  // unpinned at priority 5, it can interrupt playback in loop() on core 1, and the
  // speaker gets tiny gaps that sound like crackle.
  conf.httpd.core_id = 0;

  if (httpd_ssl_start(&httpsServer, &conf) != ESP_OK) {
    Serial.println("HTTPS server failed to start");
    return;
  }

  const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = httpsTalk, .user_ctx = NULL},
    {.uri = "/talk", .method = HTTP_GET, .handler = httpsTalk, .user_ctx = NULL},
    {.uri = "/debug", .method = HTTP_GET, .handler = httpsDebug, .user_ctx = NULL},
    {.uri = "/speak", .method = HTTP_POST, .handler = httpsSpeak, .user_ctx = NULL},
    {.uri = "/llm", .method = HTTP_POST, .handler = httpsLlm, .user_ctx = NULL},
    {.uri = "/say", .method = HTTP_POST, .handler = httpsSay, .user_ctx = NULL},
    {.uri = "/config", .method = HTTP_GET, .handler = httpsConfig, .user_ctx = NULL},
    {.uri = "/status", .method = HTTP_GET, .handler = httpsStatus, .user_ctx = NULL},
    {.uri = "/cmd", .method = HTTP_POST, .handler = httpsCmd, .user_ctx = NULL},
  };
  for (const httpd_uri_t &route : routes) {
    httpd_register_uri_handler(httpsServer, &route);
  }
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
  MDNS.addService("https", "tcp", 443);
}

// Start OTA and mDNS the first time Wi-Fi is up (at boot or after a late connect)
void startNetworkServices() {
  static bool started = false;
  if (started || WiFi.status() != WL_CONNECTED) return;
  startOta();
  started = true;
  Serial.printf("Remote control: http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
  Serial.printf("Talk page (microphone): https://%s.local/talk\n", HOSTNAME);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  lid.setPeriodHertz(50);
  lid.attach(SERVO_PIN, 500, 2500);
  lid.write(CLOSED_ANGLE);
  xTaskCreatePinnedToCore(lidTask, "lid", 4096, NULL, 2, NULL, 1);

  // A sound that fails to parse is left empty and plays as silence
  for (int i = 0; i < SOUND_COUNT; i++) {
    parseWav(sounds[i]);
  }

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S failed to start");
    while (true) {}
  }

  commandQueue = xQueueCreate(8, COMMAND_LEN);
  speechQueue = xQueueCreate(1, sizeof(Speech));

  startWifi();
  startWebServer();
  startHttpsServer();
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
  Serial.println("  s      play the next sound only (no lid movement)");
  Serial.println("  b      run the full burp sequence");
  Serial.println("  p      pause / resume the motion sensor");
  Serial.println("  d      start / stop printing distance readings");
  Serial.println("  m      turn the talking mouth (lid movement) on / off");
  Serial.println("  k      turn the speech compressor (volume leveling) on / off");
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
      playNextSound();
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
    case 'k':
      compressorOn = !compressorOn;
      Serial.println(compressorOn ? "Speech compressor on" : "Speech compressor off - speech plays at the voice's own level");
      break;
    case 'm':
      mouthOn = !mouthOn;
      Serial.println(mouthOn ? "Mouth on" : "Mouth off - lid holds open while talking");
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
  char cmd[COMMAND_LEN];
  while (xQueueReceive(commandQueue, cmd, 0) == pdTRUE) {
    runCommand(String(cmd));
  }
  Speech speech;
  if (xQueueReceive(speechQueue, &speech, 0) == pdTRUE) {
    if (speech.kokoro) {
      sayWithKokoro(speech);
    } else {
      playClip(speech);
    }
    free(speech.data);
  }

  startNetworkServices();
  ArduinoOTA.handle();
  server.handleClient();

  checkSensor();
}
