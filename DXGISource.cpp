#include <DXGISource.h>

DXGISource::DXGISource() {
	outdupl_desc = DXGI_OUTDUPL_DESC();
	output_desc = DXGI_OUTPUT_DESC();
	pDx_tex_desc = D3D11_TEXTURE2D_DESC();
	pDx_device = nullptr;
	pDx_factory = nullptr;
	pDx_adapter = nullptr;
	pDx_output = nullptr;
	pDx_device = nullptr;
	pDx_context = nullptr;
	pDx_feature_level = nullptr;
	pDx_staging_tex = nullptr;
	pDx_duplication = nullptr;

	SetDxAdapter();
	SetDxOutput();
	SetDxDevice();
	SetDxOutputDuplication();
	SetDxStagingTex();
}

DXGISource::~DXGISource() {
	SafeRelease(&pDx_staging_tex);
	SafeRelease(&pDx_device);
	SafeRelease(&pDx_context);
	SafeRelease(&pDx_duplication);
	SafeRelease(&pDx_adapter);
	SafeRelease(&pDx_output);
	SafeRelease(&pDx_factory);
}

void DXGISource::SetDxAdapter() {
	IDXGIFactory1* factory = nullptr;

	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)& factory);

	// Enumerate the adapters
	UINT i = 0;
	IDXGIAdapter1* adapter = nullptr;
	IDXGIOutput* output = nullptr;

	while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
		if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
			pDx_adapter = adapter;
			return;
		}
		++i;
	}

	ERR("No adapters with outputs found");
	exit(EXIT_FAILURE);
}

void DXGISource::SetDxOutput() {
	IDXGIOutput* pOutput = nullptr;
	IDXGIAdapter* adapter = nullptr;
	HRESULT hr;

	if (pDx_adapter == NULL) {
		SetDxAdapter();
	}

	if (pDx_adapter->EnumOutputs(0, &pOutput) != DXGI_ERROR_NOT_FOUND) {
		hr = pOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)& pDx_output);
		if (FAILED(hr)) {
			ERR("failed to query the IDXGI1Output1 interface.");
			exit(EXIT_FAILURE);
		}
		return;
	}

	LOG("No output found");
	exit(EXIT_FAILURE);
}

void DXGISource::SetDxDevice() {
	HRESULT hr;

	if (pDx_adapter == NULL) {
		SetDxAdapter();
	}

	hr = D3D11CreateDevice(
		pDx_adapter,
		D3D_DRIVER_TYPE_UNKNOWN,
		NULL,
		NULL,
		NULL,
		0,
		D3D11_SDK_VERSION,
		&pDx_device,
		pDx_feature_level,
		&pDx_context
	);

	if (FAILED(hr)) {
		ERR("failed to create the D3D11 Device");
		if (E_INVALIDARG == hr) {
			LOG("Got INVALID arg passed into D3D11CreateDevice. Did you pass a adapter + driver...");
		}
		exit(EXIT_FAILURE);
	}
}

void DXGISource::SetDxOutputDuplication() {
	HRESULT hr;
	IDXGIOutput1** ppOutput = nullptr;

	if (pDx_output == NULL) {
		SetDxOutput();
	}

	if (pDx_device == NULL) {
		SetDxDevice();
	}

	hr = pDx_output->DuplicateOutput(pDx_device, &pDx_duplication);
	if (!SUCCEEDED(hr)) {
		ERR("failed to create the duplication output.\n");
		exit(EXIT_FAILURE);
	}

	if (pDx_duplication == NULL) {
		ERR("Error: NULL output duplication");
		exit(EXIT_FAILURE);
	}
}

void DXGISource::SetDxStagingTex() {
	HRESULT hr;

	if (pDx_output == NULL) {
		SetDxOutput();
	}

	if (pDx_device == NULL) {
		SetDxDevice();
	}
	else if (pDx_context == NULL) {
		ERR("DeviceContext is NULL, but Device isnt...");
		exit(EXIT_FAILURE);
	}

	hr = pDx_output->GetDesc(&output_desc);
	if (FAILED(hr)) {
		ERR("failed to get the DXGI_OUTPUT_DESC from the output (monitor).");
		exit(EXIT_FAILURE);
	}

	// Create the staging texture that we need to download the pixels from gpu
	pDx_tex_desc.Width = output_desc.DesktopCoordinates.right;
	pDx_tex_desc.Height = output_desc.DesktopCoordinates.bottom;
	pDx_tex_desc.MipLevels = 1;
	pDx_tex_desc.ArraySize = 1;
	pDx_tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* This is the default data when using desktop duplication, see https://msdn.microsoft.com/en-us/library/windows/desktop/hh404611(v=vs.85).aspx */
	pDx_tex_desc.SampleDesc.Count = 1;
	pDx_tex_desc.SampleDesc.Quality = 0;
	pDx_tex_desc.Usage = D3D11_USAGE_STAGING;
	pDx_tex_desc.BindFlags = 0;
	pDx_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	pDx_tex_desc.MiscFlags = 0;

	hr = pDx_device->CreateTexture2D(&pDx_tex_desc, NULL, &pDx_staging_tex);

	if (hr == E_INVALIDARG) {
		ERR("received E_INVALIDARG when trying to create the texture");
		exit(EXIT_FAILURE);
	}
	else if (hr != S_OK) {
		ERR("failed to create the 2D texture, error: %d", hr);
		exit(EXIT_FAILURE);
	}
}

void DXGISource::NextFrame(DWORD** ppData) {
	HRESULT hr;

	// Access a couple of frames
	DXGI_OUTDUPL_FRAME_INFO frame_info;
	IDXGIResource* desktop_resource = NULL;
	ID3D11Texture2D* tex = NULL;
	DXGI_MAPPED_RECT mapped_rect;
	BOOL mustRelease = FALSE;

	hr = pDx_duplication->AcquireNextFrame(0, &frame_info, &desktop_resource);
	if (DXGI_ERROR_ACCESS_LOST == hr) {
		ERR("Received a DXGI_ERROR_ACCESS_LOST");
	}
	else if (DXGI_ERROR_INVALID_CALL == hr) {
		ERR("Received a DXGI_ERROR_INVALID_CALL");
	}
	else if (S_OK == hr) {
		mustRelease = TRUE;

		// Get the texture interface

		hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)& tex);
		if (FAILED(hr)) {
			ERR("failed to query the ID3D11Texture2D interface on the IDXGIResource we got.");
			exit(EXIT_FAILURE);
		}

		// Map the desktop surface

		hr = pDx_duplication->MapDesktopSurface(&mapped_rect);
		if (S_OK == hr) {
			LOG("We got access to the desktop surface");
			hr = pDx_duplication->UnMapDesktopSurface();
			if (S_OK != hr) {
				ERR("failed to unmap the desktop surface after successfully mapping it.");
			}
			// TODO: implement
		}
		else if (DXGI_ERROR_UNSUPPORTED == hr) {
			// Capture pixel data from GPU memory
			pDx_context->CopyResource(pDx_staging_tex, tex);

			D3D11_MAPPED_SUBRESOURCE map;
			HRESULT map_result = pDx_context->Map(pDx_staging_tex,
				0,
				D3D11_MAP_READ,
				0,
				&map);

			if (S_OK == map_result) {
				*ppData = (DWORD*)map.pData;
			}
			else {
				ERR("failed to map to staging tex. Cannot access the pixels");
			}

			pDx_context->Unmap(pDx_staging_tex, 0);
		}
		else if (DXGI_ERROR_INVALID_CALL == hr) {
			ERR("MapDesktopSurface returned DXGI_ERROR_INVALID_CALL.");
		}
		else if (DXGI_ERROR_ACCESS_LOST == hr) {
			ERR("MapDesktopSurface returned DXGI_ERROR_ACCESS_LOST.");
		}
		else if (E_INVALIDARG == hr) {
			ERR("MapDesktopSurface returned E_INVALIDARG.");
		}
		else {
			ERR("MapDesktopSurface returned an unknown error.");
		}
	}

	// Clean up
	if (NULL != tex) {
		tex->Release();
		tex = NULL;
	}

	if (NULL != desktop_resource) {
		desktop_resource->Release();
		desktop_resource = NULL;
	}

	if (mustRelease == TRUE) {
		hr = pDx_duplication->ReleaseFrame();
		if (FAILED(hr)) {
			ERR("Failed to release the duplication frame.");
		}
	}
}
