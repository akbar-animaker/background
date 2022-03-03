#include <Common.h>
#include <crtdbg.h>

HRESULT LCStartup() {
#if _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		ERR("CoInitialzieEx error: hr = 0x%08x", hr);
		return hr;
	}

	return hr;
}

void LCShutdown() {
	CoUninitialize();
}
