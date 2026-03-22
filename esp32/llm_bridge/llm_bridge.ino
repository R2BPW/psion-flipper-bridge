/*
 * llm_bridge.ino - ESP32 LLM Bridge with RPC Opcode Table (Steps 32, 42)
 *
 * Receives RPC commands from Flipper Zero over UART, dispatches to
 * opcode handlers with per-opcode system prompts, calls LLM via
 * OpenRouter API over WiFi, returns response.
 *
 * UART protocol (Flipper <-> ESP32):
 *   Flipper->ESP32:  OP:HH:params\n   (opcode HH hex + params)
 *                    Q:text\n          (legacy generic query)
 *                    PING\n            (heartbeat)
 *                    STATUS\n          (status request)
 *   ESP32->Flipper:  R:response\n      (LLM response)
 *                    S:status\n        (status message)
 *                    S:TOKENS:nnnn\n   (token count after LLM call)
 *                    E:error\n         (error message)
 *
 * ESP32 opcodes (0x80+):
 *   0x80  GENERATE_LISP   - Generate Lisp code from description
 *   0x81  EXPLAIN_ERROR   - Explain a Lisp error, suggest fix
 *   0x82  COMPLETE_CODE   - Complete partial s-expression
 *   0x83  SUMMARIZE_TEXT  - Summarize text briefly
 *   0x84  TRANSLATE       - Translate text to English
 *   0x85  REWRITE_REGION  - Rewrite text per instruction
 *   0x86  ORG_DECOMPOSE   - Decompose task into org-mode subtasks
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>

/* ---- Configuration ---- */
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASS     "YOUR_PASS"
#define API_HOST      "openrouter.ai"
#define API_PATH      "/api/v1/chat/completions"
#define API_MODEL     "meta-llama/llama-3-8b-instruct"
#define API_PORT      443

/* UART to Flipper: Serial1 on GPIO 43(TX)/44(RX) */
#define FLIPPER_TX_PIN  43
#define FLIPPER_RX_PIN  44
#define FLIPPER_BAUD    115200

#define UART_BUF_SIZE   512
#define RESPONSE_MAX    256
#define MAX_OPCODES     16
#define CH_TIMEOUT_MS   30000  /* 30-second channel timeout (Step 33) */

/* ---- RPC Opcode Definitions ---- */
#define OP_GENERATE_LISP   0x80
#define OP_EXPLAIN_ERROR   0x81
#define OP_COMPLETE_CODE   0x82
#define OP_SUMMARIZE_TEXT  0x83
#define OP_TRANSLATE       0x84
#define OP_REWRITE_REGION  0x85
#define OP_ORG_DECOMPOSE   0x86

/* ---- RPC Opcode Table ---- */

/* System prompt per opcode - kept short to minimize tokens */
typedef struct {
    uint8_t     opcode;
    const char* name;
    const char* system_prompt;
} EspRpcEntry;

/* Opcode table - static, initialized at startup */
static EspRpcEntry rpc_table[MAX_OPCODES];
static uint8_t rpc_count = 0;

/* Register an opcode with its system prompt */
static bool rpc_register(uint8_t opcode, const char* name,
                          const char* system_prompt) {
    if (rpc_count >= MAX_OPCODES) return false;
    /* Check for duplicate */
    for (uint8_t i = 0; i < rpc_count; i++) {
        if (rpc_table[i].opcode == opcode) {
            rpc_table[i].name = name;
            rpc_table[i].system_prompt = system_prompt;
            return true;
        }
    }
    rpc_table[rpc_count].opcode = opcode;
    rpc_table[rpc_count].name = name;
    rpc_table[rpc_count].system_prompt = system_prompt;
    rpc_count++;
    return true;
}

/* Look up system prompt for an opcode. Returns NULL if not found. */
static const char* rpc_lookup_prompt(uint8_t opcode) {
    for (uint8_t i = 0; i < rpc_count; i++) {
        if (rpc_table[i].opcode == opcode)
            return rpc_table[i].system_prompt;
    }
    return NULL;
}

/* Look up name for an opcode. Returns NULL if not found. */
static const char* rpc_lookup_name(uint8_t opcode) {
    for (uint8_t i = 0; i < rpc_count; i++) {
        if (rpc_table[i].opcode == opcode)
            return rpc_table[i].name;
    }
    return NULL;
}

/* Initialize opcode table with all ESP32 handlers */
static void rpc_init() {
    rpc_count = 0;

    rpc_register(OP_GENERATE_LISP, "GENERATE_LISP",
        "Generate one Lisp function for a minimal Scheme. "
        "Only code, no explanation. Max 200 chars.");

    rpc_register(OP_EXPLAIN_ERROR, "EXPLAIN_ERROR",
        "Explain this Lisp error in 1 sentence, suggest fix. "
        "Max 100 chars.");

    rpc_register(OP_COMPLETE_CODE, "COMPLETE_CODE",
        "Complete this partial Lisp s-expression. "
        "Only code, no explanation. Max 200 chars.");

    rpc_register(OP_SUMMARIZE_TEXT, "SUMMARIZE_TEXT",
        "Summarize the following text in 1-2 sentences. "
        "Max 100 chars.");

    rpc_register(OP_TRANSLATE, "TRANSLATE",
        "Translate the following text to English. "
        "Be concise. Max 150 chars.");

    rpc_register(OP_REWRITE_REGION, "REWRITE_REGION",
        "Rewrite the following text according to the instruction before the |. "
        "Only output the rewritten text, no explanation. Max 200 chars.");

    rpc_register(OP_ORG_DECOMPOSE, "ORG_DECOMPOSE",
        "Break down the given task into 5-7 subtasks. "
        "Return each as a line starting with ** TODO followed by a short description. "
        "Only output the subtask lines, no other text. Max 500 chars.");
}

/* ---- Channel flow control (Step 33) ---- */
typedef enum {
    ESP_CH_IDLE,        /* waiting for command */
    ESP_CH_PROCESSING,  /* LLM call in progress */
} EspChannelState;

static EspChannelState esp_ch_state = ESP_CH_IDLE;
static unsigned long   esp_ch_tick  = 0;  /* millis() at last state change */

static void esp_ch_set(EspChannelState state) {
    esp_ch_state = state;
    esp_ch_tick = millis();
}

/* ---- Token tracking (Step 42) ---- */
static uint32_t session_tokens = 0;   /* cumulative tokens this session */
static uint32_t session_calls = 0;    /* number of LLM calls this session */

/* ---- WiFi & API ---- */
static String api_key = "";
static bool wifi_connected = false;

HardwareSerial FlipperSerial(1);

static void wifi_setup() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.println(" OK");
        FlipperSerial.println("S:WIFI_OK");
    } else {
        Serial.println(" FAILED");
        FlipperSerial.println("S:WIFI_FAIL");
    }
}

/* Extract integer value for a JSON key like "total_tokens":123
 * Returns -1 if not found. */
static int extract_json_int(const String& json, const char* key) {
    int idx = json.indexOf(key);
    if (idx < 0) return -1;
    idx += strlen(key);
    /* Skip optional whitespace and colon */
    while (idx < (int)json.length() && (json.charAt(idx) == ':' ||
           json.charAt(idx) == ' ')) idx++;
    /* Parse integer */
    int val = 0;
    bool found = false;
    while (idx < (int)json.length()) {
        char c = json.charAt(idx);
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            found = true;
        } else {
            break;
        }
        idx++;
    }
    return found ? val : -1;
}

/* Call LLM API with given system prompt and user message.
 * Returns response text, or empty string on error.
 * Updates session_tokens with token usage from API response. */
static String call_llm(const char* system_prompt, const char* user_msg) {
    if (!wifi_connected || api_key.length() == 0) {
        return "";
    }

    FlipperSerial.println("S:ASKING_LLM");

    WiFiClientSecure client;
    client.setInsecure();  /* Skip cert validation */

    if (!client.connect(API_HOST, API_PORT)) {
        Serial.println("API connect failed");
        return "";
    }

    /* Build JSON body - escape user message */
    String escaped_msg = user_msg;
    escaped_msg.replace("\\", "\\\\");
    escaped_msg.replace("\"", "\\\"");
    escaped_msg.replace("\n", "\\n");
    escaped_msg.replace("\r", "");

    String escaped_sys = system_prompt;
    escaped_sys.replace("\\", "\\\\");
    escaped_sys.replace("\"", "\\\"");

    String body = "{\"model\":\"" + String(API_MODEL) + "\","
        "\"max_tokens\":200,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + escaped_sys + "\"},"
        "{\"role\":\"user\",\"content\":\"" + escaped_msg + "\"}"
        "]}";

    /* Use HTTP/1.0 to avoid chunked transfer encoding */
    String request = String("POST ") + API_PATH + " HTTP/1.0\r\n"
        "Host: " + API_HOST + "\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer " + api_key + "\r\n"
        "Content-Length: " + String(body.length()) + "\r\n"
        "\r\n" + body;

    client.print(request);

    /* Read response - skip headers */
    bool headers_done = false;
    String response_body = "";
    unsigned long start = millis();

    while (client.connected() && millis() - start < 30000) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headers_done) {
                if (line == "\r" || line.length() == 0) {
                    headers_done = true;
                }
            } else {
                response_body += line;
            }
        }
    }
    client.stop();

    /* Extract token usage from JSON (Step 42) */
    int total_tok = extract_json_int(response_body, "\"total_tokens\"");
    if (total_tok > 0) {
        session_tokens += (uint32_t)total_tok;
        session_calls++;
    }

    /* Extract content from JSON: find "content":" and extract value */
    int idx = response_body.indexOf("\"content\":\"");
    if (idx < 0) {
        idx = response_body.indexOf("\"content\": \"");
        if (idx < 0) return "";
        idx += 12;
    } else {
        idx += 11;
    }

    String content = "";
    bool escaped = false;
    for (int i = idx; i < (int)response_body.length(); i++) {
        char c = response_body.charAt(i);
        if (escaped) {
            if (c == 'n') content += '\n';
            else if (c == '"') content += '"';
            else if (c == '\\') content += '\\';
            else content += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            content += c;
        }
    }

    return content;
}

/* ---- UART Command Processing ---- */
static char uart_buf[UART_BUF_SIZE];
static size_t uart_buf_len = 0;

/* Parse hex byte from two chars. Returns -1 on invalid. */
static int parse_hex_byte(const char* s) {
    int val = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else return -1;
    }
    return val;
}

/* Process a complete UART line from Flipper */
static void process_uart_line(const char* line) {
    Serial.print("RX: ");
    Serial.println(line);

    /* OP:HH:params - opcode dispatch */
    if (line[0] == 'O' && line[1] == 'P' && line[2] == ':') {
        if (strlen(line) < 5) {
            FlipperSerial.println("E:OP_TOO_SHORT");
            return;
        }
        int opcode = parse_hex_byte(line + 3);
        if (opcode < 0) {
            FlipperSerial.println("E:BAD_OPCODE");
            return;
        }

        const char* params = "";
        if (strlen(line) > 5 && line[5] == ':') {
            params = line + 6;
        }

        const char* prompt = rpc_lookup_prompt((uint8_t)opcode);
        if (!prompt) {
            FlipperSerial.print("E:UNKNOWN_OP:");
            FlipperSerial.println(opcode, HEX);
            return;
        }

        const char* name = rpc_lookup_name((uint8_t)opcode);
        Serial.print("Dispatch: ");
        Serial.print(name ? name : "?");
        Serial.print(" params=");
        Serial.println(params);

        /* Reject if already processing (flow control) */
        if (esp_ch_state == ESP_CH_PROCESSING) {
            FlipperSerial.println("E:BUSY");
            return;
        }

        esp_ch_set(ESP_CH_PROCESSING);
        uint32_t tokens_before = session_tokens;
        String response = call_llm(prompt, params);
        esp_ch_set(ESP_CH_IDLE);

        if (response.length() > 0) {
            /* Truncate to fit UART buffer */
            if (response.length() > RESPONSE_MAX) {
                response = response.substring(0, RESPONSE_MAX);
            }
            FlipperSerial.print("R:");
            FlipperSerial.println(response);
            /* Step 42: Send token count after LLM response */
            uint32_t call_tokens = session_tokens - tokens_before;
            if (call_tokens > 0) {
                FlipperSerial.print("S:TOKENS:");
                FlipperSerial.println(call_tokens);
            }
        } else {
            FlipperSerial.println("E:LLM_FAILED");
        }
        return;
    }

    /* Q:text - legacy generic LLM query (no system prompt) */
    if (line[0] == 'Q' && line[1] == ':') {
        const char* query = line + 2;
        FlipperSerial.println("S:ASKING_LLM");

        uint32_t tokens_before = session_tokens;
        String response = call_llm(
            "You are a helpful assistant. Be concise. Max 200 chars.",
            query);
        if (response.length() > 0) {
            if (response.length() > RESPONSE_MAX) {
                response = response.substring(0, RESPONSE_MAX);
            }
            FlipperSerial.print("R:");
            FlipperSerial.println(response);
            /* Step 42: Send token count after LLM response */
            uint32_t call_tokens = session_tokens - tokens_before;
            if (call_tokens > 0) {
                FlipperSerial.print("S:TOKENS:");
                FlipperSerial.println(call_tokens);
            }
        } else {
            FlipperSerial.println("E:LLM_FAILED");
        }
        return;
    }

    /* PING - heartbeat */
    if (strcmp(line, "PING") == 0) {
        FlipperSerial.println("S:PONG");
        return;
    }

    /* STATUS - report state (Step 42: includes token count) */
    if (strcmp(line, "STATUS") == 0) {
        FlipperSerial.print("S:WIFI=");
        FlipperSerial.print(wifi_connected ? "OK" : "NO");
        FlipperSerial.print(",OPS=");
        FlipperSerial.print(rpc_count);
        FlipperSerial.print(",CH=");
        FlipperSerial.print(esp_ch_state == ESP_CH_IDLE ? "IDLE" : "PROC");
        FlipperSerial.print(",TOK=");
        FlipperSerial.print(session_tokens);
        FlipperSerial.print(",CALLS=");
        FlipperSerial.println(session_calls);
        return;
    }
}

/* ---- Arduino Setup & Loop ---- */

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 LLM Bridge v3 [FC]");

    /* Initialize UART to Flipper */
    FlipperSerial.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX_PIN, FLIPPER_TX_PIN);
    Serial.println("UART to Flipper ready");

    /* Initialize RPC opcode table */
    rpc_init();
    Serial.print("Registered ");
    Serial.print(rpc_count);
    Serial.println(" opcodes");

    /* Read API key from environment (set via build flags or hardcode for dev) */
    /* In production, this comes from a config file on SPIFFS or build define */
#ifdef OPENROUTER_API_KEY
    api_key = OPENROUTER_API_KEY;
#endif

    /* Connect WiFi */
    wifi_setup();

    uart_buf_len = 0;
    uart_buf[0] = 0;
}

void loop() {
    /* Read UART from Flipper */
    while (FlipperSerial.available()) {
        char c = FlipperSerial.read();
        if (c == '\n' || c == '\r') {
            if (uart_buf_len > 0) {
                uart_buf[uart_buf_len] = 0;
                process_uart_line(uart_buf);
                uart_buf_len = 0;
                uart_buf[0] = 0;
            }
        } else if (uart_buf_len < UART_BUF_SIZE - 1) {
            uart_buf[uart_buf_len++] = c;
        }
    }

    /* Channel timeout check (Step 33) */
    if (esp_ch_state == ESP_CH_PROCESSING &&
        millis() - esp_ch_tick >= CH_TIMEOUT_MS) {
        esp_ch_set(ESP_CH_IDLE);
        FlipperSerial.println("E:TIMEOUT");
        Serial.println("Channel timeout -> IDLE");
    }

    /* Reconnect WiFi if dropped */
    if (wifi_connected && WiFi.status() != WL_CONNECTED) {
        wifi_connected = false;
        FlipperSerial.println("S:WIFI_LOST");
        wifi_setup();
    }

    delay(10);
}
