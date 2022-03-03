#pragma once
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxgi.lib")
#pragma warning(disable:4996)

#include <Common.h>
#include <stdio.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <vector>
#include <d3d11.h>
#include <string>

const unsigned VIDEO_FRAMETICK_30FPS = 33;

class DXGISource {
public:
	DXGISource();
	~DXGISource();
	void NextFrame(DWORD**);
private:
	void SetDxAdapter();
	void SetDxOutput();
	void SetDxDevice();
	void SetDxOutputDuplication();
	void SetDxStagingTex();

	DXGI_OUTDUPL_DESC outdupl_desc;
	IDXGIFactory1* pDx_factory;
	IDXGIAdapter1* pDx_adapter;
	IDXGIOutput1* pDx_output;
	ID3D11Device* pDx_device;
	ID3D11DeviceContext* pDx_context;
	D3D_FEATURE_LEVEL* pDx_feature_level;
	DXGI_OUTPUT_DESC output_desc;
	D3D11_TEXTURE2D_DESC pDx_tex_desc;
	ID3D11Texture2D* pDx_staging_tex;
	IDXGIOutputDuplication* pDx_duplication;
};
