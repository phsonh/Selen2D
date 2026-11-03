#pragma once
#include <windows.h>

// Initialize once before creating DXC objects. Shutdown is terminal and must
// follow destruction of every DXC COM object and completion of compiler jobs.
extern "C" HRESULT WINAPI EngineDxcInitialize();
extern "C" void WINAPI EngineDxcShutdown();
