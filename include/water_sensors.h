#ifndef WATER_SENSORS_H
#define WATER_SENSORS_H

#include "struct.h"

namespace WaterSensors
{
    RetResult on();
    RetResult off();

    void log();

    bool is_measure_int_value_valid(int interval);

    RetResult init();
}

#endif