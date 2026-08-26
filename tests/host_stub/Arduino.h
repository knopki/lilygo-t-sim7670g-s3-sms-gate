#pragma once
// Minimal Arduino stub for host tests — provides String, F(), Serial, millis/delay.
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cstring>

#ifndef F
#define F(x) (x)
#endif

class String {
 public:
  String() = default;
  String(const char* s) : data_(s ? s : "") {}
  String(const char* s, size_t len) : data_(s ? std::string(s, len) : "") {}
  String(const String& o) = default;
  String(String&& o) = default;
  String& operator=(const String& o) = default;
  String& operator=(const char* s) { data_ = s ? s : ""; return *this; }

  // numeric ctors
  String(int v) : data_(std::to_string(v)) {}
  String(unsigned int v) : data_(std::to_string(v)) {}
  String(long v) : data_(std::to_string(v)) {}
  String(unsigned long v) : data_(std::to_string(v)) {}
  String(uint16_t v) : data_(std::to_string(v)) {}

  size_t length() const { return data_.size(); }
  bool isEmpty() const { return data_.empty(); }
  const char* c_str() const { return data_.c_str(); }
  char* begin() { return &data_[0]; }
  operator std::string() const { return data_; }

  void reserve(size_t n) { data_.reserve(n); }

  void trim() {
    size_t s = 0;
    while (s < data_.size() && isspace((unsigned char)data_[s])) ++s;
    size_t e = data_.size();
    while (e > s && isspace((unsigned char)data_[e - 1])) --e;
    data_ = data_.substr(s, e - s);
  }

  String substring(size_t from, size_t len = std::string::npos) const {
    if (from >= data_.size()) return String("");
    return String(data_.substr(from, len).c_str());
  }

  int indexOf(const String& needle) const {
    auto pos = data_.find(needle.data_);
    return pos == std::string::npos ? -1 : (int)pos;
  }
  int indexOf(const char* needle) const {
    auto pos = data_.find(needle);
    return pos == std::string::npos ? -1 : (int)pos;
  }
  int indexOf(char c) const {
    auto pos = data_.find(c);
    return pos == std::string::npos ? -1 : (int)pos;
  }

  void replace(const String& from, const String& to) {
    if (from.data_.empty()) return;
    std::string r;
    size_t pos = 0, fpos;
    while ((fpos = data_.find(from.data_, pos)) != std::string::npos) {
      r.append(data_, pos, fpos - pos);
      r += to.data_;
      pos = fpos + from.data_.size();
    }
    r.append(data_, pos, std::string::npos);
    data_ = std::move(r);
  }
  void replace(const char* from, const char* to) { replace(String(from), String(to)); }

  String& operator+=(const String& o) { data_ += o.data_; return *this; }
  String& operator+=(const char* s) { if (s) data_ += s; return *this; }
  String& operator+=(char c) { data_ += c; return *this; }

  friend String operator+(const String& a, const String& b) { return String((a.data_ + b.data_).c_str()); }
  friend String operator+(const String& a, const char* b) { return String((a.data_ + (b?b:"")).c_str()); }
  friend String operator+(const char* a, const String& b) { return String(((a?a:"") + b.data_).c_str()); }

  bool operator==(const String& o) const { return data_ == o.data_; }
  bool operator!=(const String& o) const { return data_ != o.data_; }
  bool operator==(const char* s) const { return data_ == (s?s:""); }

  char operator[](size_t i) const { return data_[i]; }

  friend bool constantTimeEquals(const String& a, const String& b);

 private:
  std::string data_;
};

// Minimal Serial stub
struct SerialStub {
  void println(const char* s) { (void)s; }
  void println(const String& s) { (void)s; }
  void printf(const char* fmt, ...) { (void)fmt; }
  void print(const char* s) { (void)s; }
  void print(const String& s) { (void)s; }
};
inline SerialStub Serial;

inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
inline void yield() {}

// ESP stub
struct EspStub { uint32_t getFreeHeap(){return 0;} uint32_t getMaxAllocHeap(){return 0;} };
inline EspStub ESP;
