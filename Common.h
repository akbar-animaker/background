#pragma once
#pragma comment(lib, "avrt.lib")

#include <stdio.h>
#include <windows.h>
#include <avrt.h>
#include <stdexcept>

#include "Log.h"

#define REFTIMES_PER_SEC 10000000
#define REFTIMES_PER_MILLISEC 10000

template <class T> void SafeRelease(T** ppT)
{
	if (*ppT)
	{
		(*ppT)->Release();
		*ppT = NULL;
	}
}
