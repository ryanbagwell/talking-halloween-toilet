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
const int CLOSE_MS_PER_DEGREE = 10;  // set to 0 to slam shut

// ---- Distance sensor ----
const float TRIGGER_CM = 50;                  // burp when someone comes closer than this
const float CLEAR_CM = 65;                    // re-arm once they step back past this
const int TRIGGER_READINGS = 0;               // close readings in a row needed (filters noise)
const unsigned long COOLDOWN_MS = 20000;      // minimum time between burps
const unsigned long SENSOR_INTERVAL_MS = 40;  // the sensor updates about every 38 ms

// ---- Timing ----
const int OPEN_BEFORE_BURP_MS = 150;        // lid starts rising just before the sound
const int HOLD_AFTER_BURP_MS = 300;         // lid stays up a moment after the burp
const int RATTLES = 3;                      // quick rattles after the slam
const int RATTLE_MS = 90;