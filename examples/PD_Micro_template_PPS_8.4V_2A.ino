#include <Wire.h>
#include "PD_UFP.h"

#define PIN_FUSB302_INT 10

class PD_UFP_c PD_UFP;

void setup()
{
  Serial.begin(115200);
  Wire.begin();
  PD_UFP.set_fusb302_int_pin(PIN_FUSB302_INT);
  PD_UFP.init_PPS(PPS_V(8.4), PPS_A(2.0));
}

void loop()
{
  PD_UFP.run();
  if (PD_UFP.is_PPS_ready())
  {
    Serial.println("PPS trigger success");

  }
  else if (PD_UFP.is_power_ready())
  {
    Serial.println("Fail to trigger PPS, fall back");
  }
}
