
/**
 * PD_UFP.h
 *
 *  Updated on: Jan 28, 2021
 *      Author: Ryan Ma
 *
 * Minimalist USB PD Ardunio Library for PD Micro board
 * Only support UFP(device) sink only functionality
 * Requires FUSB302_UFP.h, PD_UFP_Protocol.h and Standard Arduino Library
 *
 * Support PD3.0 PPS
 * 
 * Modified 10 September 2022 by Starryccc (Remove hardware-related functions)
 */
 
#include <stdint.h>
#include <string.h>

#include "PD_UFP.h"

#define t_PD_POLLING            100
#define t_TypeCSinkWaitCap      350
#define t_RequestToPSReady      580     // combine t_SenderResponse and t_PSTransition
#define t_PPSRequest            5000    // must less than 10000 (10s)

enum {
    STATUS_LOG_MSG_TX,
    STATUS_LOG_MSG_RX,
    STATUS_LOG_DEV,
    STATUS_LOG_CC,
    STATUS_LOG_SRC_CAP,
    STATUS_LOG_POWER_READY,
    STATUS_LOG_POWER_PPS_STARTUP,
    STATUS_LOG_POWER_REJECT,
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// PD_UFP_core_c
///////////////////////////////////////////////////////////////////////////////////////////////////
PD_UFP_core_c::PD_UFP_core_c():
    ready_voltage(0),
    ready_current(0),
    PPS_voltage_next(0),
    PPS_current_next(0),
    status_initialized(0),
    status_src_cap_received(0),
    status_power(STATUS_POWER_NA),
    time_polling(0),
    time_wait_src_cap(0),
    time_wait_ps_rdy(0),
    time_PPS_request(0),
    get_src_cap_retry_count(0),
    wait_src_cap(0),
    wait_ps_rdy(0),
    send_request(0)
{
    memset(&FUSB302, 0, sizeof(FUSB302_dev_t));
    memset(&protocol, 0, sizeof(PD_protocol_t));
}

void PD_UFP_core_c::set_fusb302_int_pin(uint8_t pin)
{
    fusb302_int_pin = pin;
}

void PD_UFP_core_c::init(enum PD_power_option_t power_option)
{
    init_PPS(0, 0, power_option);
}

void PD_UFP_core_c::init_PPS(uint16_t PPS_voltage, uint8_t PPS_current, enum PD_power_option_t power_option)
{
    // Initialize FUSB302
    pinMode(fusb302_int_pin, INPUT_PULLUP); // Set FUSB302 int pin input ant pull up
    FUSB302.i2c_address = 0x22;
    FUSB302.i2c_read = FUSB302_i2c_read;
    FUSB302.i2c_write = FUSB302_i2c_write;
    FUSB302.delay_ms = FUSB302_delay_ms;
    if (FUSB302_init(&FUSB302) == FUSB302_SUCCESS && FUSB302_get_ID(&FUSB302, 0, 0) == FUSB302_SUCCESS) {
        status_initialized = 1;
    }

    // Two stage startup for PPS Voltge < 5V
    if (PPS_voltage && PPS_voltage < PPS_V(5.0)) {
        PPS_voltage_next = PPS_voltage;
        PPS_current_next = PPS_current;
        PPS_voltage = PPS_V(5.0);
    }

    // Initialize PD protocol engine
    PD_protocol_init(&protocol);
    PD_protocol_set_power_option(&protocol, power_option);
    PD_protocol_set_PPS(&protocol, PPS_voltage, PPS_current, false);

    status_log_event(STATUS_LOG_DEV);
}

void PD_UFP_core_c::run(void)
{
    if (timer() || digitalRead(fusb302_int_pin) == 0) {
        FUSB302_event_t FUSB302_events = 0;
        for (uint8_t i = 0; i < 3 && FUSB302_alert(&FUSB302, &FUSB302_events) != FUSB302_SUCCESS; i++) {}
        if (FUSB302_events) {
            handle_FUSB302_event(FUSB302_events);
        }
    }
}

bool PD_UFP_core_c::set_PPS(uint16_t PPS_voltage, uint8_t PPS_current)
{
    if (status_power == STATUS_POWER_PPS && PD_protocol_set_PPS(&protocol, PPS_voltage, PPS_current, true)) {
        send_request = 1;
        return true;
    }
    return false;
}

void PD_UFP_core_c::set_power_option(enum PD_power_option_t power_option)
{
    if (PD_protocol_set_power_option(&protocol, power_option)) {
        send_request = 1;
    }
}

FUSB302_ret_t PD_UFP_core_c::FUSB302_i2c_read(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t count)
{
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    Wire.endTransmission();
    Wire.requestFrom(dev_addr, count);
    while (Wire.available() && count > 0) {
        *data++ = Wire.read();
        count--;
    }
    return count == 0 ? FUSB302_SUCCESS : FUSB302_ERR_READ_DEVICE;
}

FUSB302_ret_t PD_UFP_core_c::FUSB302_i2c_write(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, uint8_t count)
{
    Wire.beginTransmission(dev_addr);
    Wire.write(reg_addr);
    while (count > 0) {
        Wire.write(*data++);
        count--;
    }
    Wire.endTransmission();
    return FUSB302_SUCCESS;
}

FUSB302_ret_t PD_UFP_core_c::FUSB302_delay_ms(uint32_t t)
{
    delay(t);
    return FUSB302_SUCCESS;
}

void PD_UFP_core_c::handle_protocol_event(PD_protocol_event_t events)
{    
    if (events & PD_PROTOCOL_EVENT_SRC_CAP) {
        wait_src_cap = 0;
        get_src_cap_retry_count = 0;
        wait_ps_rdy = 1;
        time_wait_ps_rdy = millis();
        status_log_event(STATUS_LOG_SRC_CAP);
    }
    if (events & PD_PROTOCOL_EVENT_REJECT) {
        if (wait_ps_rdy) {
            wait_ps_rdy = 0;
            status_log_event(STATUS_LOG_POWER_REJECT);
        }
    }    
    if (events & PD_PROTOCOL_EVENT_PS_RDY) {
        PD_power_info_t p;
        uint8_t i, selected_power = PD_protocol_get_selected_power(&protocol);
        PD_protocol_get_power_info(&protocol, selected_power, &p);
        wait_ps_rdy = 0;
        if (p.type == PD_PDO_TYPE_AUGMENTED_PDO) {
            // PPS mode
            FUSB302_set_vbus_sense(&FUSB302, 0);
            if (PPS_voltage_next) {
                // Two stage startup for PPS voltage < 5V
                PD_protocol_set_PPS(&protocol, PPS_voltage_next, PPS_current_next, false);
                PPS_voltage_next = 0;
                send_request = 1;
                status_log_event(STATUS_LOG_POWER_PPS_STARTUP);
            } else {
                time_PPS_request = millis();
                status_power_ready(STATUS_POWER_PPS, 
                    PD_protocol_get_PPS_voltage(&protocol), PD_protocol_get_PPS_current(&protocol));
                status_log_event(STATUS_LOG_POWER_READY);
            }
        } else {
            FUSB302_set_vbus_sense(&FUSB302, 1);
            status_power_ready(STATUS_POWER_TYP, p.max_v, p.max_i);
            status_log_event(STATUS_LOG_POWER_READY);
        }
    }
}

void PD_UFP_core_c::handle_FUSB302_event(FUSB302_event_t events)
{
    if (events & FUSB302_EVENT_DETACHED) {
        PD_protocol_reset(&protocol);
        return;
    }
    if (events & FUSB302_EVENT_ATTACHED) {
        uint8_t cc1 = 0, cc2 = 0, cc = 0;
        FUSB302_get_cc(&FUSB302, &cc1, &cc2);
        PD_protocol_reset(&protocol);
        if (cc1 && cc2 == 0) {
            cc = cc1;
        } else if (cc2 && cc1 == 0) {
            cc = cc2;
        }
        /* TODO: handle no cc detected error */
        if (cc > 1) {
            wait_src_cap = 1;
        } else {
            set_default_power();
        }
        status_log_event(STATUS_LOG_CC);
    }
    if (events & FUSB302_EVENT_RX_SOP) {
        PD_protocol_event_t protocol_event = 0;
        uint16_t header;
        uint32_t obj[7];
        FUSB302_get_message(&FUSB302, &header, obj);
        PD_protocol_handle_msg(&protocol, header, obj, &protocol_event);
        status_log_event(STATUS_LOG_MSG_RX, obj);
        if (protocol_event) {
            handle_protocol_event(protocol_event);
        }
    }
    if (events & FUSB302_EVENT_GOOD_CRC_SENT) {
        uint16_t header;
        uint32_t obj[7];
        delay(2);  /* Delay respond in case there are retry messages */
        if (PD_protocol_respond(&protocol, &header, obj)) {
            status_log_event(STATUS_LOG_MSG_TX, obj);
            FUSB302_tx_sop(&FUSB302, header, obj);
        }
    }
}

bool PD_UFP_core_c::timer(void)
{
    uint16_t t = millis();
    if (wait_src_cap && t - time_wait_src_cap > t_TypeCSinkWaitCap) {
        time_wait_src_cap = t;
        if (get_src_cap_retry_count < 3) {
            uint16_t header;
            get_src_cap_retry_count += 1;
            /* Try to request soruce capabilities message (will not cause power cycle VBUS) */
            PD_protocol_create_get_src_cap(&protocol, &header);
            status_log_event(STATUS_LOG_MSG_TX);
            FUSB302_tx_sop(&FUSB302, header, 0);
        } else {
            get_src_cap_retry_count = 0;
            /* Hard reset will cause the source power cycle VBUS. */
            FUSB302_tx_hard_reset(&FUSB302);
            PD_protocol_reset(&protocol);
        }
    }
    if (wait_ps_rdy) {
        if (t - time_wait_ps_rdy > t_RequestToPSReady) {
            wait_ps_rdy = 0;
            set_default_power();
        }
    } else if (send_request || (status_power == STATUS_POWER_PPS && t - time_PPS_request > t_PPSRequest)) {
        wait_ps_rdy = 1;
        send_request = 0;
        time_PPS_request = t;
        uint16_t header;
        uint32_t obj[7];
        /* Send request if option updated or regularly in PPS mode to keep power alive */
        PD_protocol_create_request(&protocol, &header, obj);
        status_log_event(STATUS_LOG_MSG_TX, obj);
        time_wait_ps_rdy = millis();
        FUSB302_tx_sop(&FUSB302, header, obj);
    }
    if (t - time_polling > t_PD_POLLING) {
        time_polling = t;
        return true;
    }
    return false;
}

void PD_UFP_core_c::set_default_power(void)
{
    status_power_ready(STATUS_POWER_TYP, PD_V(5), PD_A(1));
    status_log_event(STATUS_LOG_POWER_READY);
}

void PD_UFP_core_c::status_power_ready(status_power_t status, uint16_t voltage, uint16_t current)
{
    ready_voltage = voltage;
    ready_current = current;
    status_power = status;
}

// ///////////////////////////////////////////////////////////////////////////////////////////////////
// // PD_UFP_c, extended from PD_UFP_core_c, Add LED and Load switch functions
// ///////////////////////////////////////////////////////////////////////////////////////////////////
PD_UFP_c::PD_UFP_c()
{
}

void PD_UFP_c::run(void)
{
    PD_UFP_core_c::run();
    // handle_led();
}

void PD_UFP_c::status_power_ready(status_power_t status, uint16_t voltage, uint16_t current)
{
    PD_UFP_core_c::status_power_ready(status, voltage, current);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Optional: PD_UFP_log_c, extended from PD_UFP_c to provide logging function.
//           Asynchronous, minimal impact on PD timing.
///////////////////////////////////////////////////////////////////////////////////////////////////
#define STATUS_LOG_MASK         (sizeof(status_log) / sizeof(status_log[0]) - 1)
#define STATUS_LOG_OBJ_MASK     (sizeof(status_log_obj) / sizeof(status_log_obj[0]) - 1)

PD_UFP_log_c::PD_UFP_log_c(pd_log_level_t log_level):
    status_log_write(0),
    status_log_read(0),
    status_log_counter(0),
    status_log_obj_read(0),
    status_log_obj_write(0),
    status_log_level(log_level)
{

}

uint8_t PD_UFP_log_c::status_log_obj_add(uint16_t header, uint32_t * obj)
{
    if (obj) {
        uint8_t i, w = status_log_obj_write, r = status_log_obj_read;
        PD_msg_info_t info;
        PD_protocol_get_msg_info(header, &info);
        for (i = 0; i < info.num_of_obj && (uint8_t)(w - r) < STATUS_LOG_OBJ_MASK; i++) {
            status_log_obj[w++ & STATUS_LOG_OBJ_MASK] = obj[i];
        }
        status_log_obj_write = w;
        return i;
    }
    return 0;
}

void PD_UFP_log_c::status_log_event(uint8_t status, uint32_t * obj)
{
    if (((status_log_write - status_log_read) & STATUS_LOG_MASK) >= STATUS_LOG_MASK) {
        return;
    }
    status_log_t * log = &status_log[status_log_write & STATUS_LOG_MASK];
    switch (status) {
    case STATUS_LOG_MSG_TX:
        log->msg_header = PD_protocol_get_tx_msg_header(&protocol);
        log->obj_count = status_log_obj_add(log->msg_header, obj);
        break;
    case STATUS_LOG_MSG_RX:
        log->msg_header = PD_protocol_get_rx_msg_header(&protocol);
        log->obj_count = status_log_obj_add(log->msg_header, obj);
        break;
    default:
        break;
    }
    log->status = status;
    log->time = millis();
    status_log_write++;
}

// Optimize RAM usage on AVR MCU by allocate format string in program memory
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define SNPRINTF snprintf_P
#else
#define SNPRINTF snprintf
#define PSTR(str) str
#endif

#define LOG(format, ...) do { n = SNPRINTF(buffer, maxlen, PSTR(format), ## __VA_ARGS__); } while (0)

int PD_UFP_log_c::status_log_readline_msg(char * buffer, int maxlen, status_log_t * log)
{
    char * t = status_log_time;
    int n = 0;
    if (status_log_counter == 0) {
        // output message header
        char type = log->status == STATUS_LOG_MSG_TX ? 'T' : 'R';
        PD_msg_info_t info;
        PD_protocol_get_msg_info(log->msg_header, &info);
        if (status_log_level >= PD_LOG_LEVEL_VERBOSE) {
            const char * ext = info.extended ? "ext, " : "";
            LOG("%s%cX %s id=%d %sraw=0x%04X\n", t, type, info.name, info.id, ext, log->msg_header);
            if (info.num_of_obj) {
                status_log_counter++;
            }
        } else {
            LOG("%s%cX %s\n", t, type, info.name);
        }
    } else {
        // output object data
        int i = status_log_counter - 1;
        uint32_t obj = status_log_obj[status_log_obj_read++ & STATUS_LOG_OBJ_MASK];
        LOG("%s obj%d=0x%08lX\n", t, i, obj);
        if (++status_log_counter > log->obj_count) {
            status_log_counter = 0;
        }
    }
    return n;
}

int PD_UFP_log_c::status_log_readline_src_cap(char * buffer, int maxlen)
{
    PD_power_info_t p;
    int n = 0;
    uint8_t i = status_log_counter;
    if (PD_protocol_get_power_info(&protocol, i, &p)) {
        const char * str_pps[] = {"", " BAT", " VAR", " PPS"};  /* PD_power_data_obj_type_t */
        char * t = status_log_time;
        uint8_t selected = PD_protocol_get_selected_power(&protocol);
        char min_v[8] = {0}, max_v[8] = {0}, power[8] = {0};
        if (p.min_v) SNPRINTF(min_v, sizeof(min_v)-1, PSTR("%d.%02dV-"), p.min_v / 20, (p.min_v * 5) % 100);
        if (p.max_v) SNPRINTF(max_v, sizeof(max_v)-1, PSTR("%d.%02dV"), p.max_v / 20, (p.max_v * 5) % 100);
        if (p.max_i) {
            SNPRINTF(power, sizeof(power)-1, PSTR("%d.%02dA"), p.max_i / 100, p.max_i % 100);
        } else {
            SNPRINTF(power, sizeof(power)-1, PSTR("%d.%02dW"), p.max_p / 4, p.max_p * 25);
        }
        LOG("%s   [%d] %s%s %s%s%s\n", t, i, min_v, max_v, power, str_pps[p.type], i == selected ? " *" : "");
        status_log_counter++;
    } else {
        status_log_counter = 0;
    }
    return n;
}

int PD_UFP_log_c::status_log_readline(char * buffer, int maxlen)
{
    if (status_log_write == status_log_read) {
        return 0;
    }
    
    status_log_t * log = &status_log[status_log_read & STATUS_LOG_MASK];
    int n = 0;
    char * t = status_log_time;
    if (t[0] == 0) {    // Convert timestamp number to string
        SNPRINTF(t, sizeof(status_log_time)-1, PSTR("%04u: "), log->time);
        return 0; 
    }

    switch (log->status) {
    case STATUS_LOG_MSG_TX:
    case STATUS_LOG_MSG_RX:
        n = status_log_readline_msg(buffer, maxlen, log);
        break;
    case STATUS_LOG_DEV:
        if (status_initialized) {
            uint8_t version_ID = 0, revision_ID = 0;
            FUSB302_get_ID(&FUSB302, &version_ID, &revision_ID);
            LOG("\n%sFUSB302 ver ID:%c_rev%c\n", t, 'A' + version_ID, 'A' + revision_ID);
        } else {
            LOG("\n%sFUSB302 init error\n", t);
        }
        break;
    case STATUS_LOG_CC: {
        const char *detection_type_str[] = {"USB", "1.5", "3.0"};
        uint8_t cc1 = 0, cc2 = 0;
        FUSB302_get_cc(&FUSB302, &cc1, &cc2);
        if (cc1 == 0 && cc2 == 0) {
            LOG("%sUSB attached vRA\n", t);
        } else if (cc1 && cc2 == 0) {
            LOG("%sUSB attached CC1 vRd-%s\n", t, detection_type_str[cc1 - 1]);
        } else if (cc2 && cc1 == 0) {
            LOG("%sUSB attached CC2 vRd-%s\n", t, detection_type_str[cc2 - 1]);
        } else {
            LOG("%sUSB attached unknown\n", t);
        }
        break; }
    case STATUS_LOG_SRC_CAP:
        n = status_log_readline_src_cap(buffer, maxlen);
        break;
    case STATUS_LOG_POWER_READY: {
        uint16_t v = ready_voltage;
        uint16_t a = ready_current;
        if (status_power == STATUS_POWER_TYP) {
            LOG("%s%d.%02dV %d.%02dA supply ready\n", t, v / 20, (v * 5) % 100, a / 100, a % 100);
        } else if (status_power == STATUS_POWER_PPS) {
            LOG("%sPPS %d.%02dV %d.%02dA supply ready\n", t, v / 50, (v * 2) % 100, a / 20, (a * 5) % 100);
        }
        break; }
    case STATUS_LOG_POWER_PPS_STARTUP:
        LOG("%sPPS 2-stage startup\n", t);
        break;
    case STATUS_LOG_POWER_REJECT:
        LOG("%sRequest Rejected\n", t);
        break;
    }
    if (status_log_counter == 0) {
        t[0] = 0;
        status_log_read++;
        status_log_counter = 0;
    }
    return n;
}

void PD_UFP_log_c::print_status(HardwareSerial & serial)
{
    // Wait for enough tx buffer in serial port to avoid blocking
    if (serial && serial.availableForWrite() >= SERIAL_BUFFER_SIZE - 1) {
        char buf[SERIAL_BUFFER_SIZE];
        if (status_log_readline(buf, sizeof(buf) - 1)) {
            serial.print(buf);
        }
    }
}