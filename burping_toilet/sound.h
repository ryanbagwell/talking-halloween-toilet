#pragma once

#include <stddef.h>
#include <stdint.h>

// A WAV file compiled into the sketch (see burp.h / goat.h)
struct Sound {
  const char *name;
  const unsigned char *wav;
  unsigned int len;
  size_t dataStart;  // where the samples begin inside wav (filled in by parseWav)
  size_t dataEnd;    // where they end
};

// Something for the toilet to say, from the phone's Talk page
struct Speech {
  uint8_t *data;  // in PSRAM; freed after speaking
  size_t bytes;
  bool kokoro;    // true: data is a Kokoro request (JSON) to fetch and speak
                  // false: data is 16 kHz, 16-bit mono audio to play (/speak)
};
