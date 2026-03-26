/*
 * LLM Bridge + TCP Relay for Flipper Zero WiFi Devboard (ESP32-S2-WROVER)
 * UART1 on GPIO43(TX)/GPIO44(RX) <-> Flipper USART (header pins 13/14)
 *
 * Features:
 * 1. LLM queries: Flipper sends Q:text, ESP32 calls OpenRouter, returns R:response
 * 2. TCP relay (port 8080): Computer sends data, ESP32 forwards as R:data to Flipper
 *    Flipper automatically IR-transmits to Psion. Bidirectional.
 * 3. Flipper UART uplink: lines from Flipper forwarded to connected TCP client
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiServer.h>

#define WIFI_SSID     ""
#define WIFI_PASS     ""
#define API_KEY       ""
#define API_HOST      "openrouter.ai"
#define API_PATH      "/api/v1/chat/completions"
#define MODEL         "google/gemini-2.0-flash-001"
#define MAX_TOKENS    400
#define TCP_PORT      8080

HardwareSerial FlipperSerial(1);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
String uart_buffer = "";

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
    if (WiFi.status() != WL_CONNECTED) { FlipperSerial.println("E:NO_WIFI"); return; }
    FlipperSerial.println("S:ASKING_LLM");
    Serial.println("Q: " + query);
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    if (!client.connect(API_HOST, 443)) { FlipperSerial.println("E:CONNECT_FAIL"); return; }
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
    unsigned long t = millis() + 15000;
    String raw = "";
    while ((client.connected() || client.available()) && millis() < t) {
        if (client.available()) raw += (char)client.read();
        else delay(10);
    }
    client.stop();
    int sep = raw.indexOf("\r\n\r\n");
    String jb = (sep >= 0) ? raw.substring(sep + 4) : raw;
    if (jb.length() > 0 && jb.charAt(0) >= '0' && jb.charAt(0) <= 'f') {
        int nl = jb.indexOf("\r\n");
        if (nl > 0 && nl < 10) jb = jb.substring(nl + 2);
        int end = jb.lastIndexOf("\r\n0\r\n");
        if (end > 0) jb = jb.substring(0, end);
    }
    if (jb.length() == 0) { FlipperSerial.println("E:EMPTY"); return; }
    String a = extract_response(jb);
    if (a.length() == 0) { FlipperSerial.println("E:PARSE"); return; }
    if (a.length() > 500) a = a.substring(0, 500);
    FlipperSerial.println("R:" + a);
    Serial.println("A: " + a);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Bridge v4 (TCP+LLM)");

    FlipperSerial.begin(115200, SERIAL_8N1, 44, 43);
    FlipperSerial.println("S:BOOTING");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    int att = 0;
    while (WiFi.status() != WL_CONNECTED && att < 20) { delay(500); Serial.print("."); att++; }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" OK " + WiFi.localIP().toString());
        FlipperSerial.println("S:WIFI_OK");
        tcpServer.begin();
        Serial.println("TCP :8080 ready");
    } else {
        Serial.println(" FAIL");
        FlipperSerial.println("S:WIFI_FAIL");
    }
}

void loop() {
    /* --- Accept TCP connections --- */
    if (tcpServer.hasClient()) {
        if (tcpClient && tcpClient.connected()) tcpClient.stop();
        tcpClient = tcpServer.available();
        Serial.println("TCP client connected");
    }

    /* --- TCP -> Flipper UART (downlink to Psion) --- */
    static String tcp_buf = "";
    if (tcpClient && tcpClient.connected()) {
        while (tcpClient.available()) {
            char c = tcpClient.read();
            if (c == '\n') {
                tcp_buf.trim();
                if (tcp_buf.length() > 0) {
                    /* Forward as R: to Flipper — triggers IR TX to Psion */
                    FlipperSerial.println("R:" + tcp_buf);
                    Serial.println("TCP>IR: " + tcp_buf.substring(0, 40));
                    /* Echo confirmation to TCP client */
                    tcpClient.println("OK:" + String(tcp_buf.length()));
                }
                tcp_buf = "";
            } else if (c != '\r') {
                tcp_buf += c;
            }
        }
    }

    /* --- Flipper UART -> process --- */
    while (FlipperSerial.available()) {
        char c = FlipperSerial.read();
        if (c == '\n') {
            uart_buffer.trim();
            if (uart_buffer.startsWith("Q:")) {
                /* Dev mode: forward ALL Psion messages to TCP, no LLM */
                String q = uart_buffer.substring(2);
                Serial.println("UP: " + q);
                if (tcpClient && tcpClient.connected()) {
                    tcpClient.println(q);
                }
            } else if (uart_buffer == "PING") {
                FlipperSerial.println("S:PONG");
            } else if (uart_buffer == "STATUS") {
                String s = "S:WIFI=";
                s += (WiFi.status() == WL_CONNECTED) ? "OK" : "NO";
                s += ",IP=" + WiFi.localIP().toString();
                s += ",TCP=";
                s += (tcpClient && tcpClient.connected()) ? "ON" : "OFF";
                FlipperSerial.println(s);
            } else if (uart_buffer.length() > 0) {
                /* Uplink: forward Flipper messages to TCP client */
                Serial.println("UP: " + uart_buffer);
                if (tcpClient && tcpClient.connected()) {
                    tcpClient.println(uart_buffer);
                }
            }
            uart_buffer = "";
        } else if (c != '\r') uart_buffer += c;
    }

    /* --- USB Serial debug commands --- */
    static String usb_buf = "";
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            usb_buf.trim();
            if (usb_buf.startsWith("Q:")) Serial.println("LLM disabled");
            else if (usb_buf.startsWith("R:")) FlipperSerial.println(usb_buf);
            else if (usb_buf == "PING") Serial.println("S:PONG");
            usb_buf = "";
        } else if (c != '\r') usb_buf += c;
    }

    /* --- WiFi keepalive --- */
    static unsigned long last = 0;
    static bool wifi_reported = false;
    if (!wifi_reported && WiFi.status() == WL_CONNECTED) {
        wifi_reported = true;
        FlipperSerial.println("S:WIFI_OK");
        Serial.println("WiFi OK: " + WiFi.localIP().toString());
    }
    if (millis() - last > 10000) {
        last = millis();
        if (WiFi.status() != WL_CONNECTED) { wifi_reported = false; WiFi.reconnect(); }
    }
}
