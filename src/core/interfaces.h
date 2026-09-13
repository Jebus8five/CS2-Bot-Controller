#pragma once
#include <ISmmAPI.h>
#include <cstddef>
namespace cs2bc::interfaces {
// Resolves required interfaces and configures optional voice support.
bool Init(SourceMM::ISmmAPI* ismm, char* error, size_t maxlen);
// Returns the server interface used to locate the native module.
void* ServerInterface();
// Clears consumers of the borrowed engine interfaces.
void Reset();
} // namespace cs2bc::interfaces
