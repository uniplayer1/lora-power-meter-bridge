#pragma once
// Serial command shell over USB. Used for field diagnosis and by the host test
// harness (tools/e2e.py), which drives a board through these commands.

#include <Arduino.h>

#include "meter_reader.h"
#include "radio_link.h"

void cliBegin();
void cliPoll(RadioLink& link, MeterReader& meter);
