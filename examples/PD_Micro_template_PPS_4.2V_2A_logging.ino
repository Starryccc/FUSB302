#include <Wire.h>
#include "PD_UFP.h"

#define PIN_FUSB302_INT 10

// To log raw packet, init PD_UFP with parameter PD_UFP(PD_UFP(PD_LOG_LEVEL_VERBOSE)
class PD_UFP_log_c PD_UFP(PD_LOG_LEVEL_VERBOSE);

void setup()
{
  Serial.begin(115200);
  Wire.begin();
  PD_UFP.set_fusb302_int_pin(PIN_FUSB302_INT);
  PD_UFP.init_PPS(PPS_V(4.2), PPS_A(2.0));
}

void loop()
{
  PD_UFP.run();
  PD_UFP.print_status(Serial);
  if (PD_UFP.is_PPS_ready())
  {
    // PPS trigger success
  }
  else if (PD_UFP.is_power_ready())
  {
    // Fail to trigger PPS, fall back
  }
}
