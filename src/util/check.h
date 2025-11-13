#pragma once
#include <assert.h>

// Minimal stubs so PSBT builds cleanly
#define Assume(expr) assert(expr)
#define Assert(expr) assert(expr)