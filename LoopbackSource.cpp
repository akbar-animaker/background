#include <LoopbackSource.h>

LoopbackSource::LoopbackSource() {
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(hr)) {
		ERR(L"CoInitializeEx: hr = 0x%08x", hr);
		throw std::runtime_error("Failed to initialize COM");
	}

	GetDefaultDevice();
	GetAudioClient();
	GetDefaultDeviceFormat();
	GetAudioCaptureClient();

	// call IAudioClient::Start
	hr = pAudioClient->Start();
	if (FAILED(hr)) {
		ERR(L"IAudioClient::Start failed: hr = 0x%08x", hr);
		throw std::runtime_error("Failed to start IAudioClient");
	}
}

LoopbackSource::~LoopbackSource() {
	if (!AvRevertMmThreadCharacteristics(hTask)) {
		ERR(L"AvRevertMmThreadCharacteristics failed: last error is %d", GetLastError());
	}
	CoTaskMemFree(pwfx);
	pAudioClient->Stop();

	pMMDevice->Release();
	pAudioClient->Release();
	pAudioCaptureClient->Release();
}

HRESULT LoopbackSource::GetAudioCaptureClient() {
	HRESULT hr = pAudioClient->Initialize(
		AUDCLNT_SHAREMODE_SHARED,
		AUDCLNT_STREAMFLAGS_LOOPBACK,
		0, 0, pwfx, 0
	);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::Initialize failed: hr = 0x%08x", hr);
		return hr;
	}

	hr = pAudioClient->GetBufferSize(&bufferFrameCount);
	if (FAILED(hr)) {
		ERR(L"Failed to get buffer size: hr = 0x%08x", hr);
		return hr;
	}

	// activate an IAudioCaptureClient
	hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)& pAudioCaptureClient);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::GetService(IAudioCaptureClient) failed: hr = 0x%08x", hr);
		return hr;
	}

	DWORD nTaskIndex = 0;
	hTask = AvSetMmThreadCharacteristics("Audio", &nTaskIndex);
	if (NULL == hTask) {
		DWORD dwErr = GetLastError();
		ERR(L"AvSetMmThreadCharacteristics failed: last error = %u", dwErr);
		return HRESULT_FROM_WIN32(dwErr);
	}

	return hr;
}

HRESULT LoopbackSource::GetDefaultDevice() {
	HRESULT hr = S_OK;
	IMMDeviceEnumerator* pMMDeviceEnumerator;

	// activate a device enumerator
	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), (void**)& pMMDeviceEnumerator
	);

	if (FAILED(hr)) {
		ERR(L"CoCreateInstance(IMMDeviceEnumerator) failed: hr = 0%08x", hr);
		return hr;
	}

	// get the default endpoint
	hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pMMDevice);
	pMMDeviceEnumerator->Release();

	if (FAILED(hr)) {
		ERR(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint failed: hr = 0x%08x", hr);
		return hr;
	}

	return hr;
}

HRESULT LoopbackSource::GetAudioClient() {
	// activate an IAudioClient
	HRESULT hr;

	hr = pMMDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)& pAudioClient);
	if (FAILED(hr)) {
		ERR(L"IMMDevice::Activate(IAudioClient) failed: hr = 0x%08x", hr);
		return hr;
	}
}

HRESULT LoopbackSource::GetDefaultDeviceFormat() {
	HRESULT hr = pAudioClient->GetMixFormat(&pwfx);
	if (FAILED(hr)) {
		ERR(L"IAudioClient::GetMixFormat failed: hr = 0x%08x", hr);
		return hr;
	}

	switch (pwfx->wFormatTag) {
	case WAVE_FORMAT_IEEE_FLOAT:
		pwfx->wFormatTag = WAVE_FORMAT_PCM;
		pwfx->wBitsPerSample = 16;
		pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
		pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
		break;

	case WAVE_FORMAT_EXTENSIBLE:
	{
		// naked scope for case-local variable
		PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
		if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat)) {
			pEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
			pEx->Samples.wValidBitsPerSample = 16;
			pwfx->wBitsPerSample = 16;
			pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
			pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
		}
		else {
			ERR(L"%s", L"Don't know how to coerce mix format to int-16");
			return E_UNEXPECTED;
		}
	}
	break;
	default:
		ERR(L"Don't know how to coerce WAVEFORMATEX with wFormatTag = 0x%08x to int-16", pwfx->wFormatTag);
		return E_UNEXPECTED;
	}

	return hr;
}

HRESULT LoopbackSource::NextFrame(BYTE **ppData) {
	HRESULT hr;
	BYTE* audioData = nullptr;

	hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize);

	if (FAILED(hr)) {
		ERR(L"Failed to get packet size: hr = 0x%08x", hr);
		return hr;
	}

	if (nNextPacketSize == 0) {
		ppData = nullptr;
		return S_OK;
	}

	// get the captured data
	DWORD dwFlags;
	UINT64 lastPos = 0;

	hr = pAudioCaptureClient->GetBuffer(
		ppData,
		&numFramesRead,
		&dwFlags,
		&lastPos,
		NULL
	);
	if (FAILED(hr)) {
		ERR(L"IAudioCaptureClient::GetBuffer failed: hr = 0x%08x", hr);
		return hr;
	}

	unsigned dataSize = numFramesRead * pwfx->nBlockAlign;

	/*
	Copy buffer content. This lets us load more packets into the buffer
	without having to wait for the current data to be processed
	*/
	audioData = new BYTE[dataSize];
	memcpy(audioData, *ppData, dataSize);
	ppData = &audioData;
	
	hr = pAudioCaptureClient->ReleaseBuffer(numFramesRead);
	if (FAILED(hr)) {
		ERR(L"IAudioCaptureClient::ReleaseBuffer failed: hr = 0x%08x", hr);
		return hr;
	}

	if (dwFlags == AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
		LOG(L"IAudioCaptureClient::GetBuffer discontinuity %d %lld", numFramesRead, lastPos);
		//return E_UNEXPECTED;
	}
#if _DEBUG
	else if (dwFlags == AUDCLNT_BUFFERFLAGS_SILENT) {
		LOG(L"IAudioCaptureClient::GetBuffer SILENCE");
	}
#endif
	else if (dwFlags == AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
		LOG(L"IAudioCaptureClient::GetBuffer timestamp error");
	}

	if (0 == numFramesRead) {
		ERR(L"IAudioCaptureClient::GetBuffer said to read 0 frames");
		return E_UNEXPECTED;
	}

	return hr;
}
