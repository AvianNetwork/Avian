#pragma once

// Minimal stub of Bitcoin's policy/settings.h for PSBT backport
namespace policy
{

static const bool DEFAULT_PERMIT_BAREMULTISIG = true;
static const bool DEFAULT_ACCEPT_NONSTD_TXN = true;
static const bool DEFAULT_DATACARRIER = true;

// Maximum size (bytes) of OP_RETURN data that nodes relay.
static const unsigned int MAX_OP_RETURN_RELAY = 83;

} // namespace policy
