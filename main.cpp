#pragma comment (lib, "winmm.lib") //timeGetTime symbols

#include <thread>
#include <iostream>

#include <chrono>

#include <DXGISource.h>
#include <LoopbackSource.h>
#include <MediaWriter.h>

LONGLONG globalAudioDuration = 0;
LONGLONG globalVideoDuration = 0;

void audioCaptureProc(BOOL *pActive, MediaWriter* pMediaWriter, LoopbackSource* pAudioSource) {
	BYTE* pData = nullptr;
	BYTE* pSilenceData = nullptr;
	REFERENCE_TIME bufferActualDuration, fullBufferDuration;
	UINT32 fullBufferSize = pAudioSource->pwfx->wBitsPerSample * pAudioSource->bufferFrameCount * pAudioSource->pwfx->nChannels / 8;

	fullBufferDuration = (double)REFTIMES_PER_SEC * pAudioSource->bufferFrameCount / pAudioSource->pwfx->nSamplesPerSec;
	pSilenceData = new BYTE[fullBufferSize];
	memset(pSilenceData, 0xff, fullBufferSize);

	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
		ERR("failed to set thread priority: %d", GetLastError());
	}

	while (*pActive) {
		/*
		Do not capture audio stream if video stream is behind
		MediaFoundation SinkWriter expects audio and video writes to be interleaved
		*/
		if (globalAudioDuration >= globalVideoDuration) {
			continue;
		}

		pAudioSource->NextFrame(&pData);
		if (pAudioSource->nNextPacketSize == 0) {
			// Write "silence"
			// 0xff means absence of sound 8-bit PCM: https://en.wikipedia.org/wiki/%CE%9C-law_algorithm
			pMediaWriter->WriteAudioFrame(pAudioSource->pwfx, pSilenceData, pAudioSource->numFramesRead, fullBufferDuration);
			globalAudioDuration += fullBufferDuration;
			Sleep(fullBufferDuration / REFTIMES_PER_MILLISEC / 2);
			continue;
		}

		while (pAudioSource->nNextPacketSize != 0) {
			bufferActualDuration = (double)REFTIMES_PER_SEC * pAudioSource->numFramesRead / pAudioSource->pwfx->nSamplesPerSec;
			pMediaWriter->WriteAudioFrame(pAudioSource->pwfx, pData, pAudioSource->numFramesRead, bufferActualDuration);
			globalAudioDuration = globalAudioDuration + bufferActualDuration;
			pAudioSource->NextFrame(&pData);
		}

		Sleep(fullBufferDuration / REFTIMES_PER_MILLISEC / 2);
	}

	delete[] pSilenceData;
}

void videoCaptureProc(BOOL *pActive, MediaWriter* pMediaWriter) {
	DXGISource videoSource{};
	DWORD* pData = nullptr;

	REFERENCE_TIME duration;
	DWORD lastTick, currentTick;

#if _DEBUG
	REFERENCE_TIME countFps = 1 * REFTIMES_PER_SEC;
	unsigned fps = 0;
	unsigned fpsAverage = 0;
#endif

	if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
		ERR("failed to set thread priority: %d", GetLastError());
	}

	lastTick = currentTick = timeGetTime();
	while (*pActive) {
		if (globalVideoDuration > globalAudioDuration) {
			continue;
		}
		if ((currentTick = timeGetTime()) - lastTick >= VIDEO_FRAMETICK_30FPS) {
			duration = (currentTick - lastTick) * REFTIMES_PER_MILLISEC;
			globalVideoDuration += duration;
			videoSource.NextFrame(&pData);
			pMediaWriter->WriteVideoFrame(globalVideoDuration, pData);
			lastTick = currentTick;
#if _DEBUG // display recording FPS
			fps += 1;
			if (globalVideoDuration >= countFps) {
				fpsAverage += fps;
				fps = 0;
				std::cout << "FPS: " << (fpsAverage / (countFps / REFTIMES_PER_SEC)) << std::endl;
				countFps += (1 * REFTIMES_PER_SEC);
			}
#endif
		}
	}
}



int main() {
	LoopbackSource* pAudioSource;
	VideoEncodeOpts videoOpts = { 
		DEFAULT_VIDEO_WIDTH, 
		DEFAULT_VIDEO_HEIGHT, 
		0, 
		0, 
		DEFAULT_VIDEO_FPS,
		DEFAULT_VIDEO_BIT_RATE,
		TRUE 
	};
	
	try {
		pAudioSource = new LoopbackSource;
	} catch (const char* msg) {
		ERR(L"Failed to initialize LoopbackSource: %s", msg);
	}

	AudioEncodeOpts audioOpts = { pAudioSource->pwfx };
	MediaWriter* pMediaWriter = new MediaWriter(&audioOpts, &videoOpts);

	BOOL* pActive = new BOOL(TRUE);
	std::string line;
	
	std::thread audioProc(audioCaptureProc, pActive, pMediaWriter, pAudioSource);
	std::thread videoProc(videoCaptureProc, pActive, pMediaWriter);

	// Block until user inputs ENTER
	while (std::getline(std::cin, line) && line.length() > 0) {
	}

	*pActive = FALSE;
	
	audioProc.join();
	videoProc.join();

	HRESULT hr = pMediaWriter->Finalize();
	if (FAILED(hr)) {
		ERR(L"Failed to Finalize MediaWriter: hr = 0x%08x", hr);
	}

	return 0;
}
