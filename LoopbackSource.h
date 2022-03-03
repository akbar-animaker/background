#pragma once
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <comdef.h>

#include <Common.h>

class LoopbackSource {
public:
	LoopbackSource();
	~LoopbackSource();
	HRESULT NextFrame(BYTE** ppData);
	WAVEFORMATEX* pwfx;
	unsigned bufferFrameCount = 0;
	UINT32 numFramesRead = 0;
	DWORD lastFrameReadTime;
	UINT32 nNextPacketSize = 0;
private:
	HRESULT GetAudioCaptureClient();
	HRESULT GetDefaultDevice();
	HRESULT GetDefaultDeviceFormat();
	HRESULT GetAudioClient();

	HANDLE hTask;
	IMMDevice* pMMDevice;
	IAudioClient* pAudioClient;
	IAudioCaptureClient* pAudioCaptureClient;
};
