#pragma once

#include <stddef.h>

// A WAV file compiled into the sketch (see burp.h / goat.h)
struct Sound {
  const char *name;
  const unsigned char *wav;
  unsigned int len;
  size_t dataStart;  // where the samples begin inside wav (filled in by parseWav)
  size_t dataEnd;    // where they end
};
