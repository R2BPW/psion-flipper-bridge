/*
 * LLM Bridge for Flipper Zero WiFi Devboard (official, ESP32-S2-WROVER)
 * UART1 on GPIO43(TX)/GPIO44(RX) <-> Flipper USART (header pins 13/14)
 *
 * Receives queries from Flipper over UART, forwards to OpenRouter LLM API
 * over WiFi, returns response.
 *
 * Set your credentials below before flashing.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>

#define WIFI_SSID     ""
#define WIFI_PASS     ""
#define API_KEY       ""
#define API_HOST      "openrouter.ai"
#define API_PATH      "/api/v1/chat/completions"
#define MODEL         "google/gemini-2.0-flash-001"
#define MAX_TOKENS    150

HardwareSerial FlipperSerial(1);
String uart_buffer = "";

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("LLM Bridge v3");

    FlipperSerial.begin(115200, SERIAL_8N1, 44, 43);
    FlipperSerial.println("S:BOOTING");
    Serial.println("UART OK");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("WiFi started (non-blocking)");
}

String extract_response(const String& json) {
    int idx = json.indexOf("\"content\":\"");
    if (idx < 0) { idx = json.indexOf("\"content\": \""); if (idx < 0) return ""; idx += 12; }
    else idx += 11;
    String r = "";
    bool esc = false;
    for (unsigned int i = idx; i < json.length() && r.length() < 500; i++) {
        char c = json.charAt(i);
        if (esc) { r += (c == 'n') ? ' ' : c; esc = false; }
        else if (c == '\\') esc = true;
        else if (c == '"') break;
        else r += c;
    }
    return r;
}

void query_llm(const String& query) {
    if (WiFi.status() != WL_CONNECTED) { FlipperSerial.println("E:NO_WIFI"); Serial.println("NO_WIFI"); return; }
    FlipperSerial.println("S:ASKING_LLM");
    Serial.println("Q: " + query);
    Serial.println("TLS connecting...");
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    if (!client.connect(API_HOST, 443)) { FlipperSerial.println("E:CONNECT_FAIL"); Serial.println("CONNECT_FAIL"); return; }
    Serial.println("TLS OK, sending...");
    String eq = "";
    for (unsigned int i = 0; i < query.length(); i++) {
        char c = query.charAt(i);
        if (c == '"') eq += "\\\""; else if (c == '\\') eq += "\\\\"; else eq += c;
    }
    String body = "{\"model\":\"" + String(MODEL) + "\",\"max_tokens\":" + String(MAX_TOKENS) +
                  ",\"messages\":[{\"role\":\"user\",\"content\":\"" + eq + "\"}]}";
    client.print("POST " + String(API_PATH) + " HTTP/1.0\r\nHost: " + String(API_HOST) +
                 "\r\nAuthorization: Bearer " + String(API_KEY) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
                 "\r\nConnection: close\r\n\r\n" + body);
    Serial.println("Request sent, waiting...");
    unsigned long t = millis() + 15000;
    String raw = "";
    while ((client.connected() || client.available()) && millis() < t) {
        if (client.available()) {
            char c = client.read();
            raw += c;
        } else {
            delay(10);
        }
    }
    client.stop();
    Serial.println("Raw len: " + String(raw.length()));
    int sep = raw.indexOf("\r\n\r\n");
    String jb = (sep >= 0) ? raw.substring(sep + 4) : raw;
    if (jb.length() > 0 && jb.charAt(0) >= '0' && jb.charAt(0) <= 'f') {
        int nl = jb.indexOf("\r\n");
        if (nl > 0 && nl < 10) jb = jb.substring(nl + 2);
        int end = jb.lastIndexOf("\r\n0\r\n");
        if (end > 0) jb = jb.substring(0, end);
    }
    if (jb.length() == 0) { FlipperSerial.println("E:EMPTY"); Serial.println("EMPTY!"); return; }
    String a = extract_response(jb);
    if (a.length() == 0) { FlipperSerial.println("E:PARSE:" + jb.substring(0, 60)); return; }
    if (a.length() > 200) a = a.substring(0, 200);
    FlipperSerial.println("R:" + a);
    Serial.println("A: " + a);
}

void loop() {
    while (FlipperSerial.available()) {
        char c = FlipperSerial.read();
        if (c == '\n') {
            uart_buffer.trim();
            if (uart_buffer.startsWith("Q:")) query_llm(uart_buffer.substring(2));
            else if (uart_buffer == "PING") { FlipperSerial.println("S:PONG"); Serial.println("PONG"); }
            else if (uart_buffer == "STATUS") {
                if (WiFi.status() == WL_CONNECTED) FlipperSerial.println("S:WIFI_OK:" + WiFi.localIP().toString());
                else FlipperSerial.println("S:WIFI_DISCONNECTED");
            }
            uart_buffer = "";
        } else if (c != '\r') uart_buffer += c;
    }
    static String usb_buf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            usb_buf.trim();
            if (usb_buf.startsWith("Q:")) query_llm(usb_buf.substring(2));
            else if (usb_buf == "PING") { Serial.println("S:PONG"); }
            usb_buf = "";
        } else if (c != '\r') usb_buf += c;
    }
    static unsigned long last = 0;
    static bool wifi_reported = false;
    if (!wifi_reported && WiFi.status() == WL_CONNECTED) {
        wifi_reported = true;
        FlipperSerial.println("S:WIFI_OK");
        Serial.println("WiFi OK: " + WiFi.localIP().toString());
    }
    if (millis() - last > 10000) { last = millis();
        Serial.println("HB wifi=" + String(WiFi.status()));
        if (WiFi.status() != WL_CONNECTED) { wifi_reported = false; WiFi.reconnect(); }
    }
}
