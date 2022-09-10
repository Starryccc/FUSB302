# FUSB302 Arduino library
# Basic Usage

## Set power option
Allocate a PD_UFP object
```
class PD_UFP_c PD_UFP;
```

Initialize it with one of the power options. That sets the maximum voltage/current. 
```
PD_UFP.init(PD_POWER_OPTION_MAX_20V);
```

```
enum PD_power_option_t {
    PD_POWER_OPTION_MAX_5V      = 0,
    PD_POWER_OPTION_MAX_9V      = 1,
    PD_POWER_OPTION_MAX_12V     = 2,
    PD_POWER_OPTION_MAX_15V     = 3,
    PD_POWER_OPTION_MAX_20V     = 4,
    PD_POWER_OPTION_MAX_VOLTAGE = 5,
    PD_POWER_OPTION_MAX_CURRENT = 6,
    PD_POWER_OPTION_MAX_POWER   = 7,
};
```

## Run USB PD state machine
Before USB PD negotiation had completed, `PD_UFP.run()` must be called in a short interval, less than 10ms, to ensure state machine response to negotiation message in time. Long response time may result in a power reset cycle initiated by USB PD hosts.

## Wait for USB PD negotiation completed
`PD_UFP.is_power_ready()` is set when
- PD negotiation completed, PD host sent a power ready message, or
- PD host not exist, CC pins voltage is checked and the power level determined.

The negotiation process typically takes less than a second.

## Examine the negotiated power value
Examine the negotiated power value. Turn on the load switch only when the power requirements are met. Any excessive power usage may trigger USB PD host power protection. 

```
PD_UFP.run();
if (PD_UFP.get_voltage() == PD_V(20.0) && PD_UFP.get_current() >= PD_A(1.5)) {
  // PPS trigger success
} else {
  // Fail to trigger PPS, fall back
}
```
For Fix power option, `PD_UFP.get_voltage()` in 50 mV units and `PD_UFP.get_current()` in 10mA units. Use marco `PD_V` and `PD_A` to simplify conversion.

## Changing power option in run time
Power options can be changed any time after PD negotiation.
```
PD_UFP.set_power_option(PD_POWER_OPTION_MAX_20V);
```

`PD_UFP.is_ps_transition()` is set during power transition, clear when new power is ready. 
Power transition takes a maximum time of 550ms according to PD specifications. Depends on the power adapter, it is usually shorter.

# USB PD 3.0 PPS (Programmable Power Supply)
USB PD3.0 introduces a new PPS (Programmable Power Supply) mode. If PD source supports PPS, It allows devices to negotiate precise voltage range from 3.3V to 5.9/11/16/21 V with 20 mV step. PPS also supports a coarse current limit, with the value in 50 mA step.

The library provides another set of procedures to request PPS. If source PPS is not available or not qualified, it will fall back to use the regular PD power option.

## Set voltage and current on startup
Instead of using `PD_UFP.init()`, use `PD_UFP.init_PPS()` to setup PPS voltage in 20 mV units and PPS current in 50 mA units. Use marco `PPS_V()` and `PPS_A()` to simplify conversion.
```
PD_UFP.init_PPS(PPS_V(4.2), PPS_A(2.0));
```
The default fall-back PD power option is 5V. It can be changed by an extra parameter.
```
PD_UFP.init_PPS(PPS_V(4.2), PPS_A(2.0), PD_POWER_OPTION_MAX_9V);
```

## Wait for USB PD PPS trigger completed
Once PD negotiation is completed,
- If PPS is available and qualified, `PD_UFP.is_PPS_ready()` is set. 
- If PPS is not available or qualified, `PD_UFP.is_power_ready()` is set.

If PPS is successfully triggered, the output voltage and current will be the exact value provided in the startup. It is not required to examine negotiated voltage and current.
```
PD_UFP.run();
if (PD_UFP.is_PPS_ready()) {
  // PPS trigger success
} else if (PD_UFP.is_power_ready()) {
  // Fail to trigger PPS, fall back
}
```
For PPS, `PD_UFP.get_voltage()` in 20 mV units and `PD_UFP.get_current()` in 50 mA units. Use marco `PPS_V()` and `PPS_A()` to simplify conversion.

## Changing PPS voltage and current in run time
PPS settings can be changed any time after PPS ready. Similar to `PD_UFP.init_PPS()`, set voltage in 20 mV units and current in 50 mA units. Use Marco `PPS_V()` and `PPS_A()` to simplify conversion.
```
PD_UFP.set_PPS(PPS_V(3.3), PPS_A(2.0));
```

`PD_UFP.is_ps_transition()` is set during power transition, clear when new power is ready. 
Power transition takes a maximum time of 550 ms according to specification. It is usually less than 50ms in PPS mode. 

By calling `PD_UFP.set_PPS()`, the library re-evaluates all PPS source capabilities to find the best fit. If it fails to find one, it returns false, no power request, and power transition will happen.

To exit PPS mode, call `PD_UFP.set_power_option()` to clear PPS setting and fall back to regular power option mode.

# Debugging
The library can log library status, source capabilities, protocol packets, with timestamps in the millisecond step to the serial port.

<img src="images/pd-micro-pd-status-log.png" alt="serial" width="400">

To use it, allocate PD_UFP object with derived class `PD_UFP_log_c`
```
class PD_UFP_log_c PD_UFP;
```
initialize hardware serial1 in setup.
```
void setup() {
  Serial.begin(115200);
  ...
```
call `PD_UFP.print_status(Serial1)` inside `loop()`.
```
void loop() {
  PD_UFP.run();
  PD_UFP.print_status(Serial);
  ...
```
## Debug raw packets
The library provides two log levels. Initialize PD_UFP with extra parameter `PD_LOG_LEVEL_VERBOSE` to output raw packets
```
class PD_UFP_log_c PD_UFP(PD_LOG_LEVEL_VERBOSE);
```
```
enum pd_log_level_t {
    PD_LOG_LEVEL_INFO,
    PD_LOG_LEVEL_VERBOSE
};
```
<img src="images/pd-micro-pd-status-log-verbose.png" alt="serial" width="400">