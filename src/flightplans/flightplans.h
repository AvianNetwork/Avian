// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_FLIGHTPLANS_H
#define AVIAN_FLIGHTPLANS_H

/* Avian Flightplans Result */
class FlightPlanResult
{
public:
    const char* result;
    bool is_error = false;
};

/* Avian Flightplans */
class AvianFlightPlans
{
public:
    FlightPlanResult run_f(const char* file, const char* func);
};

#endif
