// ---- Wi-Fi ----
const char *HOSTNAME = "burpingtoilet";  // reachable as burpingtoilet.local
const unsigned long WIFI_TIMEOUT_MS = 15000;

// ---- Pins ----
const int I2S_BCLK = 4;
const int I2S_LRC = 5;
const int I2S_DIN = 6;
const int SERVO_PIN = 7;
const int SENSOR_PIN = 8;  // Sharp GP2Y0A21 output; must be an ADC1 pin (GPIO 1-10) to work with Wi-Fi


// ---- Lid positions (tune these once the servo is mounted) ----
const int CLOSED_ANGLE = 0;
const int OPEN_ANGLE = 90;
const int RATTLE_ANGLE = 10;  // small bump used for the rattle after the slam

// ---- Lid speed (milliseconds per degree; higher = slower, 0 = full speed) ----
const int OPEN_MS_PER_DEGREE = 10;   // 90 degrees takes about 0.9 seconds
const int CLOSE_MS_PER_DEGREE = 3;   // 90 degrees takes about 0.3 seconds; set to 0 to slam shut

// ---- Distance sensor ----
const float TRIGGER_CM = 70;                  // burp when someone comes closer than this
const float CLEAR_CM = 78;                    // re-arm once they step back past this (readings top out at 80)
const int TRIGGER_READINGS = 3;               // close readings in a row needed (filters noise)
const unsigned long COOLDOWN_MS = 20000;      // minimum time between burps
const unsigned long SENSOR_INTERVAL_MS = 40;  // the sensor updates about every 38 ms
const int SENSOR_SAMPLES = 16;                // quick samples per reading; the median is kept
const int SENSOR_HISTORY = 5;                 // readings in the running median
const unsigned long LID_SETTLE_MS = 200;      // ignore the sensor this long after the lid moves

// ---- Timing ----
const int OPEN_BEFORE_BURP_MS = 150;        // lid starts rising just before the sound
const int HOLD_AFTER_BURP_MS = 0;           // lid stays up a moment after the sound
const int RATTLES = 0;                      // quick rattles after the slam (0 = none)
const int RATTLE_MS = 90;

// ---- Talking (speech sent from the phone's Talk page) ----
const size_t MAX_SPEECH_BYTES = 16000 * 2 * 30;  // 30 seconds of 16 kHz, 16-bit mono (ElevenLabs replies)
// The lid stays well open while talking so it doesn't muffle the speaker in the bowl
// or knock against the rim. Lower MOUTH_CLOSED_ANGLE for bigger mouth movements.
const int MOUTH_CLOSED_ANGLE = OPEN_ANGLE - 25;  // lid angle between words while talking
const int MOUTH_OPEN_ANGLE = OPEN_ANGLE;         // lid angle on the loudest syllables
const float MOUTH_GAIN = 4.0;       // higher = mouth opens wider on quieter speech
const int MOUTH_MS_PER_DEGREE = 3;  // glide between mouth positions instead of jumping (gentler on power)
const bool MOUTH_ENABLED = true;    // false: lid just holds open while talking (try this if the speech crackles)
// Speech volume: a compressor lifts quiet syllables toward SPEECH_TARGET_PEAK, by at
// most SPEECH_MAX_GAIN, and rounds off anything louder. Kokoro averages about -24 dB on
// its own; these settings bring it to about -14 dB, the same loudness as the burp.
const float SPEECH_TARGET_PEAK = 24000;  // out of 32767
const float SPEECH_MAX_GAIN = 4.0;       // higher = louder quiet parts, but more background hiss
const uint32_t KOKORO_SAMPLE_RATE = 24000;

// ---- Local servers, relayed by the toilet (Talk page -> toilet -> server) ----
// llama-server must listen on the network: llama-server -m model.gguf --host 0.0.0.0 --port 8080
const char *LOCAL_LLM_URL = "http://llama.dell-server.lan/";  // chat replies (llama.cpp)
const char *LOCAL_TTS_URL = "http://kokoro.dell-server.lan/";  // the toilet's voice (Kokoro-FastAPI)
const size_t MAX_RELAY_REQUEST_BYTES = 32 * 1024;  // chat or speech request from the phone
const uint16_t RELAY_TIMEOUT_MS = 60000;           // how long to wait for a local server's reply