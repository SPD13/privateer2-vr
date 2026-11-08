// Only used to produce an import library for p2den.dll (export DirectDrawCreate, stdcall, 12 bytes args).
#include <windows.h>
extern "C" HRESULT WINAPI DirectDrawCreate(void*, void**, void*) { return E_FAIL; }
