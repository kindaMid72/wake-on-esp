#pragma once
#include <Arduino.h>
#include <USB.h>

// Debug log goes to both the UART pins and the USB CDC port (/dev/ttyACM*), so no bridge is needed.
extern USBCDC USBSerial;
struct TeeLog : Print {
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    Serial0.write(b, n);
    // The CDC TX buffer is small and the port is non-blocking: a long reply (help, show) would be cut off.
    // Retry briefly while a host is attached and draining; give up after ~40 ms so an idle terminal can't stall us.
    size_t done = 0;
    for (int tries = 0; done < n && USBSerial && tries < 20;) {
      size_t w = USBSerial.write(b + done, n - done);
      done += w;
      if (!w) { delay(2); tries++; }
    }
    return n;
  }
};
extern TeeLog Log;
