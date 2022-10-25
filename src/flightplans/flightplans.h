// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_FLIGHTPLANS_H
#define AVIAN_FLIGHTPLANS_H

#include <string>
#include <vector>

/* Avian Flightplans Result */
class FlightPlanResult
{
public:
    const char* result;
    bool is_error = false;
};

/* Avian Flightplans */
class CAvianFlightPlans
{
public:
    FlightPlanResult run_file(const char* file, const char* func, std::vector<std::string> args={});
};

#endif
