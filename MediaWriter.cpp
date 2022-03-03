#include <codecapi.h>
#include <thread>

#include <Common.h>
#include <MediaWriter.h>

#define STRIDE_WIDTH_BYTES 4 // 8-bit RGBA

HRESULT MediaWriter::Finalize() {
	return pWriter->Finalize();
}

/*
Calculates the buffer length that must be allocated to write the audio sample
Writes the audio sample to the sink writer
*/
HRESULT MediaWriter::WriteAudioFrame(const WAVEFORMATEX* pwfx, BYTE* pAudioFrame, UINT32 numFramesToRead, REFERENCE_TIME bufDuration) {
	HRESULT hr = S_OK;
	IMFSample* pSample = nullptr;
	IMFMediaBuffer* pMediaBuff = nullptr;
	BYTE* pData = nullptr;
	UINT32 lBytesToWrite; 
	
	if (numFramesToRead == 0) {
		lBytesToWrite = pwfx->nAvgBytesPerSec * bufDuration / REFTIMES_PER_SEC;
	}
	else {
		lBytesToWrite = numFramesToRead * pwfx->nBlockAlign;
	}

	hr = MFCreateSample(&pSample);
	if (FAILED(hr)) {
		ERR(L"Failed to create media sample: hr = 0x%08x", hr);
		return hr;
	}
	hr = MFCreateMemoryBuffer(lBytesToWrite, &pMediaBuff);
	if (FAILED(hr)) {
		ERR(L"Failed to create media buffer: hr = 0x%08x", hr);
		return hr;
	}
	hr = pSample->AddBuffer(pMediaBuff);
	if (FAILED(hr)) {
		ERR(L"Failed to add buffer to media sample: hr = 0x%08x", hr);
		return hr;
	}
	
	ULONGLONG sampleDuration = bufDuration;

	pSample->SetSampleTime(audioDuration);
	pSample->SetSampleDuration(sampleDuration);

	hr = pMediaBuff->Lock(&pData, nullptr, nullptr);
	if (FAILED(hr)) {
		ERR(L"Failed to write to buffer: hr = 0x%08x", hr);
		return hr;
	}
	hr = pMediaBuff->SetCurrentLength(lBytesToWrite);

	// Copy audio data to allocated buffer
	memcpy_s(pData, lBytesToWrite, pAudioFrame, lBytesToWrite);

	hr = pWriter->WriteSample(audioStreamIndex, pSample);

	if (FAILED(hr)) {
		ERR(L"Failed to write sample: hr = 0x%08x", hr);
		return hr;
	}
	audioDuration = audioDuration + sampleDuration;

	return hr;
}

/*
Crop a contiguous 2D array given the coordinates in videoOpts
*/
void MediaWriter::Crop2DArray(BYTE* pDest, BYTE* pData) {
	// TODO: use multiple threads/processes and memcpy
	long srcCbWidth = DEFAULT_VIDEO_WIDTH * STRIDE_WIDTH_BYTES;
	long cbWidth = pVideoOpts->width * STRIDE_WIDTH_BYTES;
	long offsetXStride = pVideoOpts->screenOffsetX * STRIDE_WIDTH_BYTES;
	for (unsigned long i = 0; i < DEFAULT_VIDEO_HEIGHT; i++) {
		for (unsigned long j = 0; j < cbWidth; j++) {
			pDest[j + i * cbWidth] = pData[offsetXStride + j + i * (srcCbWidth + pVideoOpts->screenOffsetY)];
		}
	}
}

/*
Receives a pointer to a contiguous 2D RGBA array.
Copies the array to a MF buffer and writes the sample to the sink writer
*/
HRESULT MediaWriter::WriteVideoFrame(const LONGLONG& rtStart, DWORD* videoFrameBuffer) {
	IMFSample* pSample = nullptr;

	LONG cbWidth = STRIDE_WIDTH_BYTES * pVideoOpts->width;
	const DWORD cbBuffer = cbWidth * pVideoOpts->height;

	BYTE* pData = nullptr;
	BYTE* pCropped = nullptr;
	
	MFTIME st = timeGetTime();
	HRESULT hr = p2dBuffer->Lock2D(&pData, &cbWidth);
	if (FAILED(hr)) {
		ERR(L"Failed to allocate 2D buffer: hr = 0x%08x", hr);
		return hr;
	}
	
	if (pVideoOpts->fullscreen) {
		pCropped = (BYTE*)videoFrameBuffer;
	}
	else {
		pCropped = new BYTE[cbBuffer];
		Crop2DArray(pCropped, (BYTE*)videoFrameBuffer);
	}
	
	hr = MFCopyImage(
		pData,
		cbWidth,
		pCropped,
		cbWidth,
		cbWidth,
		pVideoOpts->height
	);
	if (FAILED(hr)) {
		ERR(L"MFCopyImage: hr = 0x%08x", hr);
		return hr;
	}

	if (!pVideoOpts->fullscreen) {
		delete[] pCropped;
	}

	p2dBuffer->Unlock2D();

	pBuffer->SetCurrentLength(cbBuffer);
	hr = MFCreateSample(&pSample);
	if (FAILED(hr)) {
		ERR(L"MFCreateSample: hr = 0x%08x", hr);
		return hr;
	}
	hr = pSample->AddBuffer(pBuffer);
	hr = pSample->SetSampleTime(rtStart);
	hr = pSample->SetSampleDuration(REFTIMES_PER_SEC / pVideoOpts->fps);

	hr = pWriter->WriteSample(videoStreamIndex, pSample);
	if (FAILED(hr)) {
		ERR(L"Failed to write sample: hr = 0x%08x", hr);
		return hr;
	}

	SafeRelease(&pSample);
	return hr;
}

MediaWriter::MediaWriter(AudioEncodeOpts* pAudioOpts, VideoEncodeOpts* pVideoOpts) {
	MFStartup(MF_VERSION);
	audioDuration = 0;
	this->pAudioOpts = pAudioOpts;
	this->pVideoOpts = pVideoOpts;

	const wchar_t* outputFileName = L"output.mp4";

	pWriter = nullptr;

	IMFSinkWriter* pSinkWriter = nullptr;
	IMFMediaType* pVideoOut = nullptr;
	IMFMediaType* pAudioOut = nullptr;
	IMFMediaType* pVideoIn = nullptr;
	IMFMediaType* pAudioIn = nullptr;

	// Video stream output
	IMFAttributes* pSinkAttrs;
	MFCreateAttributes(&pSinkAttrs, 0);

	pSinkAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	pSinkAttrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);

	MFCreateSinkWriterFromURL(outputFileName, nullptr, pSinkAttrs, &pSinkWriter);
	MFCreateMediaType(&pVideoOut);
	pVideoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pVideoOut->SetGUID(MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT);
	pVideoOut->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH264VProfile_Main);
	pVideoOut->SetUINT32(MF_MT_VIDEO_LEVEL, eAVEncH264VLevel5);
	pVideoOut->SetUINT32(MF_MT_AVG_BITRATE, pVideoOpts->bitrate);
	pVideoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	MFSetAttributeSize(pVideoOut, MF_MT_FRAME_SIZE, pVideoOpts->width, pVideoOpts->height);
	MFSetAttributeRatio(pVideoOut, MF_MT_FRAME_RATE, pVideoOpts->fps, 1);
	MFSetAttributeRatio(pVideoOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	pSinkWriter->AddStream(pVideoOut, &videoStreamIndex);
	
	// Audio stream output
	MFCreateMediaType(&pAudioOut);
	pAudioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pAudioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
	pAudioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, pAudioOpts->pwfx->wBitsPerSample);
	pAudioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pAudioOpts->pwfx->nSamplesPerSec);
	pAudioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, pAudioOpts->pwfx->nChannels);
	pSinkWriter->AddStream(pAudioOut, &audioStreamIndex);

	// Video input (stream 0)
	MFCreateMediaType(&pVideoIn);
	pVideoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	pVideoIn->SetGUID(MF_MT_SUBTYPE, VIDEO_INPUT_FORMAT);
	MFSetAttributeSize(pVideoIn, MF_MT_FRAME_SIZE, pVideoOpts->width, pVideoOpts->height);
	MFSetAttributeRatio(pVideoIn, MF_MT_FRAME_RATE, pVideoOpts->fps, 1);
	MFSetAttributeRatio(pVideoIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	pSinkWriter->SetInputMediaType(videoStreamIndex, pVideoIn, nullptr);

	// Audio input (stream 1)
	MFCreateMediaType(&pAudioIn);
	pAudioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pAudioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	pAudioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, pAudioOpts->pwfx->wBitsPerSample);
	pAudioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pAudioOpts->pwfx->nSamplesPerSec);
	pAudioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, pAudioOpts->pwfx->nChannels);
	pSinkWriter->SetInputMediaType(audioStreamIndex, pAudioIn, nullptr);

	HRESULT hr = pSinkWriter->BeginWriting();

	pWriter = pSinkWriter;
	pWriter->AddRef();

	// Create 2D buffer
	hr = MFCreate2DMediaBuffer(pVideoOpts->width, pVideoOpts->height, MFVideoFormat_ARGB32.Data1, FALSE, &pBuffer);
	if (FAILED(hr)) {
		ERR(L"MFCreate2DMediaBuffer: hr = 0x%08x", hr);
	}
	pBuffer->QueryInterface(__uuidof(IMF2DBuffer), (void**)& p2dBuffer);

	SafeRelease(&pSinkWriter);
	SafeRelease(&pVideoOut);
	SafeRelease(&pAudioOut);
	SafeRelease(&pAudioIn);
	SafeRelease(&pVideoIn);
}

MediaWriter::~MediaWriter() {
	SafeRelease(&pBuffer);
	SafeRelease(&p2dBuffer);
	SafeRelease(&pWriter);
	MFShutdown();
}
