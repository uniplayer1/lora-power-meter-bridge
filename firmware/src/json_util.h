#pragma once
// JSON string escaping, shared by the web UI and the MQTT bridge.
//
// Both used to carry their own copy and the copies drifted: the MQTT one passed
// control characters through, which is enough to make Home Assistant reject a
// whole discovery config.

#include <Arduino.h>

#include <stdio.h>

inline String escapeJson(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += " "; break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}
