// DLL-wide state shared by the provider and the tile.

#pragma once

#include <windows.h>

// This DLL's module handle, for loading its resources (the tile image).
extern HINSTANCE g_hinst;
#define HINST_THISDLL g_hinst

// Every live COM object holds a reference, so LogonUI doesn't unload the DLL under it.
void DllAddRef();
void DllRelease();
