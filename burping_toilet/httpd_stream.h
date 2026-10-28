#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

// Lets HTTPClient::writeToStream() pass a response straight through to an
// esp_http_server request as chunks, so large replies (speech audio) are never
// held in memory. writeToStream also undoes any chunked encoding from the server.
class HttpdChunkStream : public Stream {
 public:
  explicit HttpdChunkStream(httpd_req_t *req) : req_(req) {}

  size_t write(const uint8_t *data, size_t size) override {
    if (failed_ || size == 0) return 0;
    if (httpd_resp_send_chunk(req_, (const char *)data, size) != ESP_OK) {
      failed_ = true;  // the phone hung up
      return 0;
    }
    return size;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }

  // Write-only stream
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool failed() const { return failed_; }

 private:
  httpd_req_t *req_;
  bool failed_ = false;
};
