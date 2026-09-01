#pragma once
#include "Arduino.h"
class WebServer {
 public:
  WebServer(int) {}
  void send(int, const char*, const String&) {}
  void sendHeader(const char*, const char*) {}
  void send_P(int, const char*, const char*, size_t) {}
  String arg(const char*) { return String(""); }
  bool authenticate(const char*, const char*) { return true; }
  void requestAuthentication(int, const char*) {}
  void on(const char*, int, void (*)()) {}
  void onNotFound(void (*)()) {}
  void begin() {}
  void handleClient() {}
};
#define HTTP_GET 1
#define HTTP_POST 2
#define DIGEST_AUTH 1
