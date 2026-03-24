#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <infrared_worker.h>
#include <infrared_transmit.h>
#include <storage/storage.h>

#define TAG "IRBridge"
#define MAX_LINES 6
#define LINE_LEN 40
#define TX_FREQ 36000
#define TX_DUTY 0.33f

/* Pulse-width TX (Flipper -> Psion), unchanged from v6 */
#define MARK_BIT0  10000
#define MARK_BIT1  26000
#define MARK_START 50000
#define MARK_END   75000
#define SPACE_US    5000

/* Ternary RX decoder (Psion OPX -> Flipper)
 *
 * Protocol: Psion sends SIR bursts via OPX (RBusDevComm).
 * Gap timing between bursts encodes ternary digits (base-3).
 *
 * Per character: 4 bursts (start + digit2 + digit1 + digit0).
 * Char code = d2*9 + d1*3 + d0: A=0, B=1, ... Z=25, space=26.
 *
 * Gap thresholds (calibrated from OPX gaps_001 data):
 *   gap < 150ms                    -> noise (ignored)
 *   150ms <= gap < 300ms           -> digit 0
 *   300ms <= gap < 430ms           -> digit 1
 *   430ms <= gap < 550ms           -> digit 2
 *   gap >= 550ms                   -> start (new character)
 *   silence >= 2000ms              -> message complete, dispatch
 */
#define TER_NOISE_MS       150   /* ignore gaps shorter than this         */
#define TER_D01_MS         300   /* boundary: digit0 / digit1             */
#define TER_D12_MS         430   /* boundary: digit1 / digit2             */
#define TER_START_MIN_MS   550   /* gap >= this = start (new char)        */
#define TER_MSG_TIMEOUT_MS 2000  /* silence -> message complete           */

#define RX_MSG_MAX 64

/* Frame protocol (Step 30: packetized Flipper->Psion) */
#define FRAME_STX       0x02
#define FRAME_ETX       0x03
#define FRAME_MAX_PAY   56
#define FRAME_MAX_RETRY 3
#define FRAME_ACK_TIMEOUT_MS 5000   /* wait for ACK/NAK after frame TX */

/* RPC opcode table (Step 31) */
#define RPC_MAX_OPCODES 32
#define RPC_SYSTEM_MAX  0x0F  /* opcodes 0x00-0x0F reserved for system */

/* Flow control (Step 33: bidirectional half-duplex) */
#define CH_TIMEOUT_MS      30000  /* 30-second channel timeout */

/* AI response cache on SD card (Step 41) */
#define CACHE_MAX_ENTRIES  100
#define CACHE_MAX_RESP     255
#define CACHE_DIR          "/ext/psmacs"
#define CACHE_PATH         "/ext/psmacs/cache.bin"
#define CACHE_HEADER_SIZE  4       /* count(1) + next_slot(1) + reserved(2) */
#define CACHE_ENTRY_SIZE   262     /* hash(4) + opcode(1) + resp_len(1) + response(256) */

typedef enum {
    ChIdle,        /* channel free, ready for new request */
    ChPsionTx,     /* Psion is transmitting (receiving bits) */
    ChProcessing,  /* request dispatched, waiting for handler/ESP32 */
    ChFlipperTx,   /* Flipper is transmitting response to Psion */
} ChannelState;

/* UART to ESP32 WiFi devboard */
#define ESP_UART_BAUD 115200
#define UART_BUF_SIZE 512

/* Ternary RX decoder state machine */
typedef enum {
    TerIdle,        /* waiting for first burst */
    TerWaitStart,   /* got burst, waiting for start gap (>= TER_START_MIN) */
    TerDigit,       /* receiving ternary digits (d2, d1, d0) */
} TerState;

typedef struct {
    TerState state;
    uint8_t  trits[3];   /* d2, d1, d0 */
    uint8_t  trit_idx;   /* 0=d2, 1=d1, 2=d0 */
} TerRxState;

/* Forward declaration for RPC handler type */
typedef struct IrBridgeApp IrBridgeApp;

/* RPC handler: receives app, opcode, params string, params length.
 * Handler should respond via send_to_psion or other mechanism. */
typedef void (*RpcHandler)(IrBridgeApp* app, uint8_t opcode,
                           const char* params, size_t params_len);

/* RPC opcode table entry */
typedef struct {
    uint8_t    opcode;     /* 0x00-0x3F (6-bit range) */
    const char* name;      /* human-readable name for debug display */
    RpcHandler handler;    /* NULL = unregistered */
} RpcEntry;

struct IrBridgeApp {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* mq;
    InfraredWorker* ir_worker;

    char lines[MAX_LINES][LINE_LEN];
    int line_idx;

    uint32_t tx_timings[1024];

    /* Ternary RX state */
    TerRxState ter_rx;
    volatile uint32_t last_signal_tick;
    volatile uint32_t last_gap_ms;     /* last measured gap (debug display) */
    volatile uint32_t rx_cb_count;     /* total IR callbacks received */
    char rx_msg[RX_MSG_MAX];
    size_t rx_msg_len;

    /* Adaptive calibration: thresholds derived from first start gap */
    bool     cal_done;       /* true after first start gap measured */
    uint32_t cal_d01;        /* adaptive d0/d1 boundary */
    uint32_t cal_d12;        /* adaptive d1/d2 boundary */
    uint32_t cal_start;      /* adaptive start threshold */

    /* ACK/NAK state for framed TX */
    volatile uint8_t ack_bursts;        /* burst count after TX (1=ACK, 2=NAK) */
    volatile bool    awaiting_ack;      /* true while waiting for ACK/NAK */
    volatile uint32_t ack_last_tick;    /* timestamp of last burst during ACK wait */

    /* ESP32 UART */
    FuriHalSerialHandle* esp_serial;
    char uart_buf[UART_BUF_SIZE];
    volatile size_t uart_buf_len;
    volatile bool uart_line_ready;
    bool esp_connected;
    bool waiting_llm;

    /* RPC opcode table */
    RpcEntry rpc_table[RPC_MAX_OPCODES];
    uint8_t  rpc_count;  /* number of registered opcodes */

    /* Channel flow control (Step 33) */
    ChannelState ch_state;       /* current half-duplex channel state */
    uint32_t     ch_state_tick;  /* timestamp when state last changed */

    /* AI response cache (Step 41) */
    uint32_t cache_hashes[CACHE_MAX_ENTRIES]; /* in-memory hash index */
    uint8_t  cache_count;      /* entries in file */
    uint8_t  cache_next_slot;  /* circular write position */
    uint32_t cache_pending_hash;   /* hash of request being forwarded */
    uint8_t  cache_pending_opcode; /* opcode of pending request */
    bool     cache_pending;    /* true = cache the next ESP32 response */
    bool     cache_loaded;     /* true after cache index loaded from SD */

    /* Token usage tracking (Step 42) */
    uint32_t session_tokens;   /* cumulative tokens from ESP32 S:TOKENS */
    uint32_t session_calls;    /* number of LLM calls */

    bool running;
};

typedef enum { EvtKey, EvtIrRx, EvtUart } EvtType;
typedef struct { EvtType type; InputEvent input; } AppEvent;

/* Forward declaration for cache_store (defined after process_esp_response) */
static void cache_store(IrBridgeApp* app, uint32_t hash, uint8_t opcode,
                        const char* response, size_t resp_len);

static void add_line(IrBridgeApp* app, const char* text) {
    if(app->line_idx >= MAX_LINES) {
        for(int i = 0; i < MAX_LINES - 1; i++)
            memcpy(app->lines[i], app->lines[i + 1], LINE_LEN);
        app->line_idx = MAX_LINES - 1;
    }
    strncpy(app->lines[app->line_idx], text, LINE_LEN - 1);
    app->lines[app->line_idx][LINE_LEN - 1] = 0;
    app->line_idx++;
}

/* Forward declarations */
static void send_to_psion(IrBridgeApp* app, const char* text);

/* ---- Channel flow control (Step 33) ---- */

static void ch_set_state(IrBridgeApp* app, ChannelState state) {
    app->ch_state = state;
    app->ch_state_tick = furi_get_tick();
}

static const char* ch_state_name(ChannelState state) {
    switch(state) {
        case ChIdle:       return "IDLE";
        case ChPsionTx:    return "RX";
        case ChProcessing: return "PROC";
        case ChFlipperTx:  return "TX";
        default:           return "?";
    }
}

/* Check if channel has timed out. Returns true if timed out and reset. */
static bool ch_check_timeout(IrBridgeApp* app) {
    if(app->ch_state == ChIdle) return false;
    uint32_t elapsed = furi_get_tick() - app->ch_state_tick;
    if(elapsed >= CH_TIMEOUT_MS) {
        add_line(app, "CH: timeout->idle");
        app->waiting_llm = false;
        app->rx_msg_len = 0;
        app->rx_msg[0] = 0;
        app->ter_rx.state    = TerIdle;
        app->ter_rx.trit_idx = 0;
        ch_set_state(app, ChIdle);
        return true;
    }
    return false;
}

/* ---- Classify ternary gap into digit value ---- */
static int ter_classify_gap(IrBridgeApp* app, uint32_t gap) {
    uint32_t d01 = app->cal_done ? app->cal_d01 : TER_D01_MS;
    uint32_t d12 = app->cal_done ? app->cal_d12 : TER_D12_MS;
    uint32_t start = app->cal_done ? app->cal_start : TER_START_MIN_MS;
    if(gap < d01)        return 0;  /* digit 0 */
    else if(gap < d12)   return 1;  /* digit 1 */
    else if(gap < start) return 2;  /* digit 2 */
    else                 return -1; /* start gap */
}

/* ---- 38kHz fast decoder: parse mark/space from raw signal ---- */
/* At 38400 baud: mark=0x00 (~260us/byte), space=0xFF (~260us/byte)
 * Header: ~8ms mark + ~4ms space
 * Digit: ~1ms mark + space (1ms=d0, 2ms=d1, 3ms=d2)
 * 3 digits per char, ternary encoding */
#define FAST38_HEADER_MIN_US  4000   /* header mark >= 4ms */
#define FAST38_D1_MIN_US      1500   /* space >= 1.5ms = digit 1 */
#define FAST38_D2_MIN_US      2500   /* space >= 2.5ms = digit 2 */
#define FAST38_MIN_TIMINGS    8      /* minimum entries for valid signal */

static void decode_fast38(IrBridgeApp* app, const uint32_t* t, size_t cnt) {
    if(cnt < FAST38_MIN_TIMINGS) return;

    /* First mark must be header (>= 4ms) */
    if(t[0] < FAST38_HEADER_MIN_US) return;

    /* Parse digit spaces: t[1]=header space, then pairs (mark, space) */
    size_t spaces = cnt / 2;
    char msg[RX_MSG_MAX];
    size_t msg_len = 0;
    uint8_t trits[3];
    uint8_t trit_idx = 0;

    /* Skip header space (index 0), start from first digit space */
    for(size_t s = 1; s < spaces && msg_len < RX_MSG_MAX - 1; s++) {
        uint32_t space_us = t[s * 2 + 1];  /* space at odd indices */
        uint32_t mark_us = t[s * 2];        /* mark at even indices */
        (void)mark_us;  /* mark duration ignored for now */

        uint8_t digit;
        if(space_us >= FAST38_D2_MIN_US)      digit = 2;
        else if(space_us >= FAST38_D1_MIN_US) digit = 1;
        else                                  digit = 0;

        trits[trit_idx++] = digit;

        if(trit_idx >= 3) {
            uint8_t code = trits[0] * 9 + trits[1] * 3 + trits[2];
            char ch = 0;
            if(code <= 25)      ch = (char)('A' + code);
            else if(code == 26) ch = ' ';
            if(ch) msg[msg_len++] = ch;
            trit_idx = 0;
        }
    }

    if(msg_len > 0) {
        msg[msg_len] = 0;
        strlcpy(app->rx_msg, msg, RX_MSG_MAX);
        app->rx_msg_len = msg_len;

        char line[LINE_LEN];
        snprintf(line, LINE_LEN, "F38>%.30s", msg);
        add_line(app, line);

        ch_set_state(app, ChPsionTx);
        AppEvent evt = {.type = EvtIrRx};
        furi_message_queue_put(app->mq, &evt, 0);
    }
}

/* ---- IR RX callback: ternary decoder + ACK/NAK detection ---- */
static void ir_rx_callback(void* ctx, InfraredWorkerSignal* signal) {
    IrBridgeApp* app = ctx;

    /* Try 38kHz fast protocol first (single raw signal = whole message) */
    if(!infrared_worker_signal_is_decoded(signal)) {
        const uint32_t* timings;
        size_t timings_cnt;
        infrared_worker_get_raw_signal(signal, &timings, &timings_cnt);
        if(timings_cnt >= FAST38_MIN_TIMINGS && timings[0] >= FAST38_HEADER_MIN_US) {
            decode_fast38(app, timings, timings_cnt);
            app->last_signal_tick = furi_get_tick();
            app->last_gap_ms = timings[0] / 1000;
            return;
        }
    }

    /* Legacy gap-timing decoder */
    uint32_t now  = furi_get_tick();
    uint32_t gap  = now - app->last_signal_tick;
    app->last_signal_tick = now;
    app->last_gap_ms = gap;
    app->rx_cb_count++;

    /* ACK/NAK counting during framed TX */
    if(app->awaiting_ack) {
        app->ack_bursts++;
        app->ack_last_tick = now;
        AppEvent evt = {.type = EvtIrRx};
        furi_message_queue_put(app->mq, &evt, 0);
        return;
    }

    /* Drop bursts while Flipper is transmitting back to Psion */
    if(app->ch_state == ChFlipperTx) return;

    /* Ignore noise */
    if(gap < TER_NOISE_MS) return;

    /* Mark channel active; refresh timeout on every burst */
    if(app->ch_state == ChIdle) {
        ch_set_state(app, ChPsionTx);
    } else if(app->ch_state == ChPsionTx) {
        app->ch_state_tick = furi_get_tick();
    }

    /* --- Ternary state machine --- */
    switch(app->ter_rx.state) {

    case TerIdle:
        /* Any burst -> wait for start gap confirmation */
        app->ter_rx.state    = TerWaitStart;
        app->ter_rx.trit_idx = 0;
        break;

    case TerWaitStart: {
        uint32_t start_thr = app->cal_done ? app->cal_start : TER_START_MIN_MS;
        if(gap >= start_thr) {
            /* Start gap confirmed. Calibrate ONLY from reasonable gaps
             * (< 2000ms = actual PAUSE, not idle silence). */
            if(gap < 2000) {
                app->cal_d01   = gap * 42 / 100;
                app->cal_d12   = gap * 62 / 100;
                app->cal_start = gap * 85 / 100;
                app->cal_done  = true;
            }
            app->ter_rx.state    = TerDigit;
            app->ter_rx.trit_idx = 0;
        } else {
            /* Gap too short for start -> noise, stay waiting */
            app->ter_rx.state = TerIdle;
        }
        break;
    }
        break;

    case TerDigit: {
        uint32_t start_thr = app->cal_done ? app->cal_start : TER_START_MIN_MS;
        if(gap >= start_thr) {
            /* Unexpected start gap mid-character -> new start.
             * Recalibrate only from reasonable gaps. */
            if(gap < 2000) {
                app->cal_d01   = gap * 42 / 100;
                app->cal_d12   = gap * 62 / 100;
                app->cal_start = gap * 85 / 100;
                app->cal_done  = true;
            }
            app->ter_rx.trit_idx = 0;
            break;
        }

        int digit = ter_classify_gap(app, gap);
        if(digit < 0) {
            /* Should not happen (gap >= START but caught above) */
            app->ter_rx.state = TerIdle;
            break;
        }

        app->ter_rx.trits[app->ter_rx.trit_idx] = (uint8_t)digit;
        app->ter_rx.trit_idx++;

        if(app->ter_rx.trit_idx >= 3) {
            /* All 3 trits received -> reconstruct character */
            uint8_t code = app->ter_rx.trits[0] * 9
                         + app->ter_rx.trits[1] * 3
                         + app->ter_rx.trits[2];
            char ch = 0;
            if(code <= 25)      ch = (char)('A' + code);
            else if(code == 26) ch = ' ';
            if(ch && app->rx_msg_len < RX_MSG_MAX - 1) {
                app->rx_msg[app->rx_msg_len++] = ch;
                app->rx_msg[app->rx_msg_len]   = 0;
            }
            /* This burst is also start of next char -> wait for start gap */
            app->ter_rx.state    = TerWaitStart;
            app->ter_rx.trit_idx = 0;
        }
        break;
    }

    } /* switch */

    AppEvent evt = {.type = EvtIrRx};
    furi_message_queue_put(app->mq, &evt, 0);
}

/* ---- UART RX callback from ESP32 ---- */
static void
    esp_uart_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    IrBridgeApp* app = ctx;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        if(byte == '\n' || byte == '\r') {
            if(app->uart_buf_len > 0) {
                app->uart_line_ready = true;
                AppEvent evt = {.type = EvtUart};
                furi_message_queue_put(app->mq, &evt, 0);
            }
        } else if(app->uart_buf_len < UART_BUF_SIZE - 1 && !app->uart_line_ready) {
            app->uart_buf[app->uart_buf_len++] = (char)byte;
            app->uart_buf[app->uart_buf_len] = 0;
        }
    }
}

/* ---- Process UART response from ESP32 ---- */
static void process_esp_response(IrBridgeApp* app) {
    if(!app->uart_line_ready) return;

    char line[UART_BUF_SIZE];
    memcpy(line, app->uart_buf, app->uart_buf_len + 1);
    app->uart_buf_len = 0;
    app->uart_buf[0] = 0;
    app->uart_line_ready = false;

    if(line[0] == 'R' && line[1] == ':') {
        /* LLM response - transition from Processing to TX */
        app->waiting_llm = false;
        app->line_idx = 0;
        add_line(app, "LLM:");
        const char* text = line + 2;
        size_t tlen = strlen(text);
        for(size_t i = 0; i < tlen; i += LINE_LEN - 1) {
            char chunk[LINE_LEN];
            size_t clen = tlen - i;
            if(clen > (size_t)(LINE_LEN - 1)) clen = LINE_LEN - 1;
            memcpy(chunk, text + i, clen);
            chunk[clen] = 0;
            add_line(app, chunk);
        }
        /* Step 41: Store response in cache before sending */
        if(app->cache_pending && tlen > 0 && tlen <= CACHE_MAX_RESP) {
            cache_store(app, app->cache_pending_hash,
                        app->cache_pending_opcode, text, tlen);
            app->cache_pending = false;
        }
        /* Send response to Psion via IR */
        view_port_update(app->vp);
        send_to_psion(app, text);
    } else if(line[0] == 'S' && line[1] == ':') {
        /* Status */
        const char* status = line + 2;
        if(strncmp(status, "WIFI_OK", 7) == 0) {
            app->esp_connected = true;
        } else if(strncmp(status, "ASKING_LLM", 10) == 0) {
            add_line(app, "ESP: asking LLM...");
        } else if(strncmp(status, "TOKENS:", 7) == 0) {
            /* Step 42: Parse token count from ESP32 */
            uint32_t tokens = 0;
            const char* p = status + 7;
            while(*p >= '0' && *p <= '9') {
                tokens = tokens * 10 + (*p - '0');
                p++;
            }
            if(tokens > 0) {
                app->session_tokens += tokens;
                app->session_calls++;
            }
            char tline[LINE_LEN];
            snprintf(tline, LINE_LEN, "Tok:+%lu =%lu",
                (unsigned long)tokens, (unsigned long)app->session_tokens);
            add_line(app, tline);
        }
        char sline[LINE_LEN];
        strlcpy(sline, "ESP:", LINE_LEN);
        strlcat(sline, status, LINE_LEN);
        add_line(app, sline);
    } else if(line[0] == 'E' && line[1] == ':') {
        /* Error - send error to Psion, channel returns to idle via send_to_psion */
        app->waiting_llm = false;
        app->cache_pending = false; /* don't cache errors */
        char eline[LINE_LEN];
        strlcpy(eline, "ERR:", LINE_LEN);
        strlcat(eline, line + 2, LINE_LEN);
        add_line(app, eline);
        /* Forward error to Psion so it knows request failed */
        char err_resp[LINE_LEN];
        strlcpy(err_resp, "E:", LINE_LEN);
        strlcat(err_resp, line + 2, LINE_LEN);
        send_to_psion(app, err_resp);
    }
}

/* ---- Send query to ESP32 ---- */
static void send_to_llm(IrBridgeApp* app) {
    char query[RX_MSG_MAX];
    strlcpy(query, app->rx_msg, sizeof(query));
    size_t len = strlen(query);
    if(len == 0) return;

    char cmd[RX_MSG_MAX + 4];
    snprintf(cmd, sizeof(cmd), "Q:%s\n", query);
    furi_hal_serial_tx(app->esp_serial, (uint8_t*)cmd, strlen(cmd));

    app->waiting_llm = true;
    app->rx_msg_len = 0;
    app->rx_msg[0] = 0;

    char line[LINE_LEN];
    strlcpy(line, "Q: ", LINE_LEN);
    strlcat(line, query, LINE_LEN);
    add_line(app, line);
}

/* ---- Pulse-width TX helpers ---- */
static size_t encode_byte_pw(uint8_t v, uint32_t* t, size_t off, size_t max_t) {
    for(int bit = 7; bit >= 0; bit--) {
        if(off + 2 > max_t) return off;
        t[off++] = (v & (1 << bit)) ? MARK_BIT1 : MARK_BIT0;
        t[off++] = SPACE_US;
    }
    return off;
}

/* (frame_checksum and encode_frame_pw removed — using raw byte TX now) */

/* Send text to Psion as raw pulse-width encoded ASCII bytes.
 * Single START mark, data in chunks (back-to-back, no gaps), single END mark.
 * Psion sees one continuous transmission and exits on END marker. */
static void send_to_psion(IrBridgeApp* app, const char* text) {
    ch_set_state(app, ChFlipperTx);
    add_line(app, "TX...");
    view_port_update(app->vp);

    size_t tlen = strlen(text);
    if(tlen > 200) tlen = 200;

    infrared_worker_rx_stop(app->ir_worker);

    /* 1. Send START mark */
    app->tx_timings[0] = MARK_START;
    app->tx_timings[1] = SPACE_US;
    infrared_send_raw_ext(app->tx_timings, 2, true, TX_FREQ, TX_DUTY);

    /* 2. Send data bytes in chunks of 63 (63*16=1008 < 1024), no delay */
    size_t offset = 0;
    while(offset < tlen) {
        size_t chunk = tlen - offset;
        if(chunk > 63) chunk = 63;

        size_t n = 0;
        for(size_t i = 0; i < chunk; i++) {
            n = encode_byte_pw((uint8_t)text[offset + i], app->tx_timings, n, 1020);
        }
        infrared_send_raw_ext(app->tx_timings, n, true, TX_FREQ, TX_DUTY);
        offset += chunk;
    }

    /* 3. Send EOT byte (0x04) as end-of-message signal */
    {
        size_t n = 0;
        n = encode_byte_pw(0x04, app->tx_timings, n, 1020);
        infrared_send_raw_ext(app->tx_timings, n, true, TX_FREQ, TX_DUTY);
    }

    /* 4. Send END mark */
    app->tx_timings[0] = MARK_END;
    app->tx_timings[1] = SPACE_US;
    infrared_send_raw_ext(app->tx_timings, 2, true, TX_FREQ, TX_DUTY);

    /* Restore normal RX mode */
    app->ter_rx.state = TerIdle;
    infrared_worker_rx_start(app->ir_worker);

    char line[LINE_LEN];
    strlcpy(line, "TX: ", LINE_LEN);
    strlcat(line, text, LINE_LEN);
    add_line(app, line);

    /* TX complete - channel returns to idle */
    ch_set_state(app, ChIdle);
}

/* ---- RPC opcode table (Step 31) ---- */

/* Register an opcode handler in the table */
static bool rpc_register(IrBridgeApp* app, uint8_t opcode,
                          const char* name, RpcHandler handler) {
    if(app->rpc_count >= RPC_MAX_OPCODES) return false;
    /* Check for duplicate */
    for(uint8_t i = 0; i < app->rpc_count; i++) {
        if(app->rpc_table[i].opcode == opcode) {
            app->rpc_table[i].handler = handler;
            app->rpc_table[i].name = name;
            return true;
        }
    }
    app->rpc_table[app->rpc_count].opcode = opcode;
    app->rpc_table[app->rpc_count].name = name;
    app->rpc_table[app->rpc_count].handler = handler;
    app->rpc_count++;
    return true;
}

/* Look up handler for an opcode. Returns NULL if not registered. */
static RpcHandler rpc_lookup(IrBridgeApp* app, uint8_t opcode) {
    for(uint8_t i = 0; i < app->rpc_count; i++) {
        if(app->rpc_table[i].opcode == opcode)
            return app->rpc_table[i].handler;
    }
    return NULL;
}

/* Look up name for an opcode. Returns NULL if not registered. */
static const char* rpc_name(IrBridgeApp* app, uint8_t opcode) {
    for(uint8_t i = 0; i < app->rpc_count; i++) {
        if(app->rpc_table[i].opcode == opcode)
            return app->rpc_table[i].name;
    }
    return NULL;
}

/* Dispatch a received message as RPC.
 * First char (raw 6-bit value = char - 32) is the opcode.
 * Returns true if dispatched, false if no handler found. */
static bool rpc_dispatch(IrBridgeApp* app, const char* msg, size_t msg_len) {
    if(msg_len == 0) return false;
    uint8_t opcode = (uint8_t)(msg[0] - 32);  /* raw 6-bit value */
    RpcHandler handler = rpc_lookup(app, opcode);
    if(!handler) return false;

    const char* name = rpc_name(app, opcode);
    char line[LINE_LEN];
    snprintf(line, LINE_LEN, "RPC:%02X %s", opcode, name ? name : "?");
    add_line(app, line);

    const char* params = (msg_len > 1) ? msg + 1 : "";
    size_t params_len = (msg_len > 1) ? msg_len - 1 : 0;
    handler(app, opcode, params, params_len);
    return true;
}

/* --- System opcode handlers (0x00-0x0F) --- */

/* 0x00 PING: respond with PONG */
static void rpc_handle_ping(IrBridgeApp* app, uint8_t opcode,
                             const char* params, size_t params_len) {
    UNUSED(opcode); UNUSED(params); UNUSED(params_len);
    send_to_psion(app, "PONG");
}

/* 0x01 STATUS: respond with device status summary (Step 42: includes tokens/cost) */
static void rpc_handle_status(IrBridgeApp* app, uint8_t opcode,
                               const char* params, size_t params_len) {
    UNUSED(opcode); UNUSED(params); UNUSED(params_len);
    /* Cost estimate: ~$0.15 per 1M tokens for Llama 3 8B on OpenRouter.
     * cost_cents = tokens * 15 / 1000000
     * Display as $0.XX where XX is cents. */
    uint32_t cost_cents = app->session_tokens * 15 / 1000000;
    char resp[96];
    snprintf(resp, sizeof(resp), "S:ESP=%s CH=%s TOK=%lu CALLS=%lu COST=$0.%02lu",
        app->esp_connected ? "OK" : "NO",
        ch_state_name(app->ch_state),
        (unsigned long)app->session_tokens,
        (unsigned long)app->session_calls,
        (unsigned long)cost_cents);
    send_to_psion(app, resp);
}

/* 0x02 RESET: clear all state, respond with OK */
static void rpc_handle_reset(IrBridgeApp* app, uint8_t opcode,
                              const char* params, size_t params_len) {
    UNUSED(opcode); UNUSED(params); UNUSED(params_len);
    app->rx_msg_len = 0;
    app->rx_msg[0] = 0;
    app->ter_rx.state    = TerIdle;
    app->ter_rx.trit_idx = 0;
    app->waiting_llm = false;
    app->line_idx = 0;
    /* Reset channel to idle before responding */
    ch_set_state(app, ChIdle);
    add_line(app, "Reset by RPC");
    send_to_psion(app, "OK");
}

/* ---- AI response cache (Step 41) ---- */

/* FNV-1a hash: combine opcode + params into a 32-bit hash */
static uint32_t cache_hash(uint8_t opcode, const char* params, size_t params_len) {
    uint32_t h = 0x811c9dc5u; /* FNV offset basis */
    h ^= opcode;
    h *= 0x01000193u; /* FNV prime */
    for(size_t i = 0; i < params_len; i++) {
        h ^= (uint8_t)params[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Load cache index from SD card into memory.
 * Creates directory and file if they don't exist. */
static void cache_init(IrBridgeApp* app) {
    app->cache_count = 0;
    app->cache_next_slot = 0;
    app->cache_loaded = false;
    app->cache_pending = false;
    memset(app->cache_hashes, 0, sizeof(app->cache_hashes));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, CACHE_DIR);

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, CACHE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* Read header */
        uint8_t header[CACHE_HEADER_SIZE];
        if(storage_file_read(file, header, CACHE_HEADER_SIZE) == CACHE_HEADER_SIZE) {
            app->cache_count = header[0];
            app->cache_next_slot = header[1];
            if(app->cache_count > CACHE_MAX_ENTRIES)
                app->cache_count = CACHE_MAX_ENTRIES;
            if(app->cache_next_slot >= CACHE_MAX_ENTRIES)
                app->cache_next_slot = 0;

            /* Read hash values from each entry */
            for(uint8_t i = 0; i < app->cache_count; i++) {
                uint64_t offset = CACHE_HEADER_SIZE + (uint64_t)i * CACHE_ENTRY_SIZE;
                if(storage_file_seek(file, offset, true)) {
                    uint32_t hash;
                    if(storage_file_read(file, &hash, 4) == 4) {
                        app->cache_hashes[i] = hash;
                    }
                }
            }
            app->cache_loaded = true;
        }
        storage_file_close(file);
    } else {
        /* File doesn't exist yet, that's fine */
        app->cache_loaded = true;
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    char line[LINE_LEN];
    snprintf(line, LINE_LEN, "Cache: %u entries", (unsigned)app->cache_count);
    add_line(app, line);
}

/* Look up a hash in the cache. Returns slot index or -1 if not found. */
static int cache_find(IrBridgeApp* app, uint32_t hash) {
    for(uint8_t i = 0; i < app->cache_count; i++) {
        if(app->cache_hashes[i] == hash) return (int)i;
    }
    return -1;
}

/* Read cached response from SD at given slot.
 * Returns response length, or 0 on failure.
 * resp must be at least CACHE_MAX_RESP+1 bytes. */
static size_t cache_read_response(IrBridgeApp* app, int slot, char* resp) {
    UNUSED(app);
    if(slot < 0 || slot >= CACHE_MAX_ENTRIES) return 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    size_t result = 0;

    if(storage_file_open(file, CACHE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        /* Seek to entry: header + slot * entry_size + hash(4) + opcode(1) */
        uint64_t offset = CACHE_HEADER_SIZE + (uint64_t)slot * CACHE_ENTRY_SIZE + 5;
        if(storage_file_seek(file, offset, true)) {
            uint8_t resp_len;
            if(storage_file_read(file, &resp_len, 1) == 1 && resp_len > 0) {
                /* resp_len is uint8_t (0-255), CACHE_MAX_RESP=255: always fits */
                if(storage_file_read(file, resp, resp_len) == resp_len) {
                    resp[resp_len] = 0;
                    result = resp_len;
                }
            }
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return result;
}

/* Store a response in the cache at the next circular slot. */
static void cache_store(IrBridgeApp* app, uint32_t hash, uint8_t opcode,
                         const char* response, size_t resp_len) {
    if(resp_len == 0 || resp_len > CACHE_MAX_RESP) return;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, CACHE_PATH, FSAM_READ_WRITE, FSOM_OPEN_ALWAYS)) {
        uint8_t slot = app->cache_next_slot;

        /* Write entry at slot position */
        uint64_t offset = CACHE_HEADER_SIZE + (uint64_t)slot * CACHE_ENTRY_SIZE;
        if(storage_file_seek(file, offset, true)) {
            /* Write: hash(4) + opcode(1) + resp_len(1) + response(256) */
            uint8_t entry[CACHE_ENTRY_SIZE];
            memset(entry, 0, sizeof(entry));
            memcpy(entry, &hash, 4);
            entry[4] = opcode;
            entry[5] = (uint8_t)resp_len;
            memcpy(entry + 6, response, resp_len);
            storage_file_write(file, entry, CACHE_ENTRY_SIZE);
        }

        /* Update in-memory index */
        app->cache_hashes[slot] = hash;
        if(slot >= app->cache_count) {
            app->cache_count = slot + 1;
        }
        app->cache_next_slot = (slot + 1) % CACHE_MAX_ENTRIES;

        /* Update header */
        if(storage_file_seek(file, 0, true)) {
            uint8_t header[CACHE_HEADER_SIZE] = {
                app->cache_count, app->cache_next_slot, 0, 0
            };
            storage_file_write(file, header, CACHE_HEADER_SIZE);
        }

        storage_file_close(file);

        char line[LINE_LEN];
        snprintf(line, LINE_LEN, "Cache+%u (%u)", (unsigned)slot, (unsigned)app->cache_count);
        add_line(app, line);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* --- ESP32 opcode forwarding handlers (Step 32) --- */

/* Map 6-bit IR opcodes (0x10-0x16) to 8-bit ESP32 opcodes (0x80-0x86).
 * The IR protocol only supports 6-bit values (0x00-0x3F), but the ESP32
 * opcode space uses 0x80+ to separate from Flipper-local opcodes. */
#define ESP_OPCODE_BASE 0x70  /* add to 6-bit opcode to get ESP32 opcode */

/* Generic handler: forward opcode + params to ESP32 via UART as OP:HH:params\n
 * Step 41: checks cache first, returns cached response if hit. */
static void rpc_forward_to_esp(IrBridgeApp* app, uint8_t opcode,
                                const char* params, size_t params_len) {
    /* Step 41: Check cache before forwarding to ESP32 */
    if(app->cache_loaded) {
        uint32_t hash = cache_hash(opcode, params, params_len);
        int slot = cache_find(app, hash);
        if(slot >= 0) {
            char resp[CACHE_MAX_RESP + 1];
            size_t rlen = cache_read_response(app, slot, resp);
            if(rlen > 0) {
                add_line(app, "Cache HIT");
                send_to_psion(app, resp);
                return;
            }
        }
        /* Cache miss - store hash for later caching */
        app->cache_pending_hash = hash;
        app->cache_pending_opcode = opcode;
        app->cache_pending = true;
    }

    if(!app->esp_serial) {
        add_line(app, "No ESP32 UART");
        send_to_psion(app, "E:NO_ESP");
        app->cache_pending = false;
        return;
    }

    /* Map 6-bit opcode to 8-bit ESP32 opcode */
    uint8_t esp_opcode = opcode + ESP_OPCODE_BASE;

    char cmd[RX_MSG_MAX + 16];
    if(params_len > 0) {
        snprintf(cmd, sizeof(cmd), "OP:%02X:%.*s\n",
                 esp_opcode, (int)params_len, params);
    } else {
        snprintf(cmd, sizeof(cmd), "OP:%02X:\n", esp_opcode);
    }
    furi_hal_serial_tx(app->esp_serial, (uint8_t*)cmd, strlen(cmd));
    app->waiting_llm = true;

    const char* name = rpc_name(app, opcode);
    char line[LINE_LEN];
    snprintf(line, LINE_LEN, "ESP:%s", name ? name : "?");
    add_line(app, line);
}

/* Register all system opcodes */
static void rpc_init(IrBridgeApp* app) {
    app->rpc_count = 0;
    memset(app->rpc_table, 0, sizeof(app->rpc_table));

    /* System opcodes (0x00-0x0F) - handled locally on Flipper */
    rpc_register(app, 0x00, "PING",   rpc_handle_ping);
    rpc_register(app, 0x01, "STATUS", rpc_handle_status);
    rpc_register(app, 0x02, "RESET",  rpc_handle_reset);

    /* ESP32 opcodes (0x10+) - forwarded to ESP32 via UART.
     * Note: 6-bit encoding limits opcodes to 0x00-0x3F.
     * Flipper maps these to full 8-bit opcodes (0x80+) when
     * forwarding to ESP32 via the OP:HH: UART protocol. */
    rpc_register(app, 0x10, "GEN_LISP",  rpc_forward_to_esp);
    rpc_register(app, 0x11, "EXPL_ERR",  rpc_forward_to_esp);
    rpc_register(app, 0x12, "COMPL_COD", rpc_forward_to_esp);
    rpc_register(app, 0x13, "SUMM_TXT",  rpc_forward_to_esp);
    rpc_register(app, 0x14, "TRANSLATE", rpc_forward_to_esp);
    rpc_register(app, 0x15, "REWRITE",   rpc_forward_to_esp);
    rpc_register(app, 0x16, "ORG_DECOMP", rpc_forward_to_esp);
}

/* ---- GUI ---- */
static void draw_callback(Canvas* canvas, void* ctx) {
    IrBridgeApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "IR Bridge v14 [TER]");

    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "g%lu", (unsigned long)app->last_gap_ms);
        canvas_draw_str(canvas, 100, 10, tmp);
    }

    if(app->waiting_llm) {
        canvas_draw_str(canvas, 90, 10, "...");
    }

    canvas_set_font(canvas, FontSecondary);

    /* Show received message */
    if(app->rx_msg_len > 0) {
        char msgline[LINE_LEN];
        strlcpy(msgline, ">", LINE_LEN);
        strlcat(msgline, app->rx_msg, LINE_LEN);
        canvas_draw_str(canvas, 2, 20, msgline);
    }

    int y_start = (app->rx_msg_len > 0) ? 29 : 20;
    for(int i = 0; i < app->line_idx && i < MAX_LINES; i++)
        canvas_draw_str(canvas, 2, y_start + i * 8, app->lines[i]);

    /* Show channel state + token count (Step 42) */
    {
        char status[LINE_LEN];
        if(app->session_tokens > 0) {
            snprintf(status, sizeof(status), "%s CH:%s T:%lu",
                app->esp_connected ? "WiFi" : "noWF",
                ch_state_name(app->ch_state),
                (unsigned long)app->session_tokens);
        } else {
            snprintf(status, sizeof(status), "%s | CH:%s | OK=LLM L:clr",
                app->esp_connected ? "WiFi" : "noWF",
                ch_state_name(app->ch_state));
        }
        canvas_draw_str(canvas, 2, 62, status);
    }
}

static void input_callback(InputEvent* evt, void* ctx) {
    IrBridgeApp* app = ctx;
    AppEvent e = {.type = EvtKey, .input = *evt};
    furi_message_queue_put(app->mq, &e, FuriWaitForever);
}

/* ---- Main ---- */
int32_t ir_bridge_app(void* p) {
    UNUSED(p);
    IrBridgeApp* app = malloc(sizeof(IrBridgeApp));
    memset(app, 0, sizeof(IrBridgeApp));
    app->ter_rx.state = TerIdle;

    app->mq = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_callback, app);
    view_port_input_callback_set(app->vp, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    /* IR worker */
    app->ir_worker = infrared_worker_alloc();
    infrared_worker_rx_enable_signal_decoding(app->ir_worker, false);
    infrared_worker_rx_enable_blink_on_receiving(app->ir_worker, true);
    infrared_worker_rx_set_received_signal_callback(app->ir_worker, ir_rx_callback, app);
    infrared_worker_rx_start(app->ir_worker);

    /* ESP32 UART */
    app->esp_serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->esp_serial) {
        furi_hal_serial_init(app->esp_serial, ESP_UART_BAUD);
        furi_hal_serial_async_rx_start(app->esp_serial, esp_uart_callback, app, false);
        add_line(app, "UART to ESP32 ready");
        furi_hal_serial_tx(app->esp_serial, (uint8_t*)"PING\n", 5);
    } else {
        add_line(app, "UART acquire failed!");
    }

    /* RPC opcode table */
    rpc_init(app);

    /* AI response cache (Step 41) */
    cache_init(app);

    /* Initialize channel state (Step 33) */
    ch_set_state(app, ChIdle);

    add_line(app, "Ternary RX active");
    app->running = true;
    AppEvent evt;

    while(app->running) {
        if(furi_message_queue_get(app->mq, &evt, 100) == FuriStatusOk) {
            if(evt.type == EvtKey && evt.input.type == InputTypePress) {
                if(evt.input.key == InputKeyBack)
                    app->running = false;
                else if(evt.input.key == InputKeyOk) {
                    /* Send current message to LLM */
                    if(app->rx_msg_len > 0 && app->esp_serial) {
                        send_to_llm(app);
                    }
                }
                else if(evt.input.key == InputKeyUp)
                    send_to_psion(app, "ABC");
                else if(evt.input.key == InputKeyDown)
                    send_to_psion(app, "Hello");
                else if(evt.input.key == InputKeyLeft) {
                    app->rx_msg_len = 0;
                    app->rx_msg[0] = 0;
                    app->ter_rx.state    = TerIdle;
                    app->ter_rx.trit_idx = 0;
                    app->waiting_llm = false;
                    app->line_idx = 0;
                    ch_set_state(app, ChIdle);
                    add_line(app, "Cleared");
                }
                else if(evt.input.key == InputKeyRight) {
                    /* Manual ping ESP32 */
                    if(app->esp_serial) {
                        furi_hal_serial_tx(
                            app->esp_serial, (uint8_t*)"STATUS\n", 7);
                        add_line(app, "Ping ESP32...");
                    }
                }
            }
            if(evt.type == EvtUart) {
                process_esp_response(app);
            }
        }

        /* Check channel timeout (30 seconds in any non-idle state) */
        ch_check_timeout(app);

        /* Check for message timeout (800 ms silence = message complete) */
        if(app->rx_msg_len > 0) {
            uint32_t elapsed = furi_get_tick() - app->last_signal_tick;
            if(elapsed >= TER_MSG_TIMEOUT_MS) {
                app->ter_rx.state = TerIdle;
                app->cal_done = false;  /* reset calibration for next message */
                char line[LINE_LEN];
                snprintf(line, LINE_LEN, "RX(%u): %.30s",
                    (unsigned)app->rx_msg_len, app->rx_msg);
                add_line(app, line);

                /* Try RPC dispatch first; fall back to LLM */
                if(app->rx_msg_len > 0) {
                    /* Psion done transmitting - now processing */
                    ch_set_state(app, ChProcessing);
                    if(rpc_dispatch(app, app->rx_msg, app->rx_msg_len)) {
                        /* RPC handled - clear message
                         * (channel state managed by handler via send_to_psion) */
                        app->rx_msg_len = 0;
                        app->rx_msg[0] = 0;
                    } else if(app->esp_serial) {
                        /* No RPC handler - forward to LLM as before
                         * Channel stays in ChProcessing until ESP32 responds */
                        send_to_llm(app);
                    } else {
                        /* No handler and no ESP32 - back to idle */
                        ch_set_state(app, ChIdle);
                    }
                }
            }
        }

        view_port_update(app->vp);
    }

    /* Cleanup */
    infrared_worker_rx_stop(app->ir_worker);
    infrared_worker_free(app->ir_worker);
    if(app->esp_serial) {
        furi_hal_serial_async_rx_stop(app->esp_serial);
        furi_hal_serial_deinit(app->esp_serial);
        furi_hal_serial_control_release(app->esp_serial);
    }
    view_port_enabled_set(app->vp, false);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->mq);
    free(app);
    return 0;
}
