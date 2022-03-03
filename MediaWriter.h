#pragma once
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfapi.h>

// Format constants
const UINT32 DEFAULT_VIDEO_WIDTH = 2560;
const UINT32 DEFAULT_VIDEO_HEIGHT = 1080;
const UINT32 DEFAULT_VIDEO_FPS = 30;
const UINT32 DEFAULT_VIDEO_BIT_RATE = 12000000;
const GUID   VIDEO_ENCODING_FORMAT = MFVideoFormat_H264;
const GUID   VIDEO_INPUT_FORMAT = MFVideoFormat_ARGB32;

typedef struct VideoEncodeOpts {
	unsigned width;
	unsigned height;
	unsigned screenOffsetX;
	unsigned screenOffsetY;
	unsigned fps;
	unsigned bitrate;
	BOOL fullscreen;
};

typedef struct AudioEncodeOpts {
	WAVEFORMATEX* pwfx;
};

class MediaWriter {
public:
	MediaWriter(AudioEncodeOpts*, VideoEncodeOpts*);
	~MediaWriter();
	HRESULT WriteVideoFrame(const LONGLONG&, DWORD*);
	HRESULT WriteAudioFrame(const WAVEFORMATEX*, BYTE*, UINT32, REFERENCE_TIME bufDuration);
	void Crop2DArray(BYTE* pDest, BYTE* pData);
	HRESULT Finalize();
	ULONGLONG audioDuration = 0;
private:
	IMFSinkWriter* pWriter;
	DWORD audioStreamIndex = 0;
	DWORD videoStreamIndex = 0;
	IMFMediaBuffer* pBuffer = nullptr;
	IMF2DBuffer* p2dBuffer = nullptr;
	AudioEncodeOpts* pAudioOpts;
	VideoEncodeOpts* pVideoOpts;
};
