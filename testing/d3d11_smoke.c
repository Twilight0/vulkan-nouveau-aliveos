#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
    ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

static int check_hr(const char *what, HRESULT hr) {
    if (FAILED(hr)) {
        fprintf(stderr, "FAIL %s: hr=0x%08lx\n", what, (unsigned long)hr);
        return 0;
    }
    return 1;
}

static int approx_u8(uint8_t got, uint8_t exp) {
    int d = (int)got - (int)exp;
    return d >= -2 && d <= 2;
}

int main(void) {
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        fls, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (!check_hr("D3D11CreateDevice", hr)) return 1;
    printf("PASS D3D11CreateDevice (feature_level=0x%x)\n", (unsigned)fl);

    /* Query DXGI adapter */
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev);
    if (!check_hr("QueryInterface(IDXGIDevice)", hr)) return 1;
    hr = IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    if (!check_hr("GetAdapter", hr)) return 1;
    hr = IDXGIAdapter_GetDesc(adapter, &desc);
    if (!check_hr("GetDesc", hr)) return 1;
    printf("PASS Adapter: %ls (vendor=0x%04x device=0x%04x)\n",
           desc.Description, (unsigned)desc.VendorId, (unsigned)desc.DeviceId);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);

    /* Test 1: ClearRenderTargetView + CopyResource + Staging Map readback */
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D *rt_tex = NULL;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt_tex);
    if (!check_hr("CreateTexture2D(RT)", hr)) return 1;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stg_tex = NULL;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &stg_tex);
    if (!check_hr("CreateTexture2D(Staging)", hr)) return 1;

    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt_tex, NULL, &rtv);
    if (!check_hr("CreateRenderTargetView", hr)) return 1;

    const float clear_col[4] = { 0.25f, 0.50f, 0.75f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear_col);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg_tex, (ID3D11Resource *)rt_tex);

    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (!check_hr("Map(Clear)", hr)) return 1;
    const uint8_t *px = (const uint8_t *)mapped.pData;
    if (!approx_u8(px[0], 64) || !approx_u8(px[1], 128) || !approx_u8(px[2], 191) || !approx_u8(px[3], 255)) {
        fprintf(stderr, "FAIL d3d11_clear: got (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
        return 1;
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg_tex, 0);
    printf("PASS d3d11_clear (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);

    /* Test 2 & 3: Compile VS/PS + CS using d3dcompiler_47.dll */
    HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dc) {
        fprintf(stderr, "WARN d3dcompiler_47.dll not found; skipping shader draw/cs tests\n");
        goto done;
    }
    PFN_D3DCompile pD3DCompile = (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile");
    if (!pD3DCompile) {
        fprintf(stderr, "WARN D3DCompile not found; skipping shader draw/cs tests\n");
        goto done;
    }

    static const char *vs_src =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 p = float2(id & 2, (id << 1) & 2);\n"
        "  return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);\n"
        "}\n";
    static const char *ps_src =
        "float4 main() : SV_Target {\n"
        "  return float4(1.0f, 0.25f, 0.125f, 1.0f);\n"
        "}\n";

    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *err_blob = NULL;
    hr = pD3DCompile(vs_src, strlen(vs_src), "vs.hlsl", NULL, NULL, "main", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    if (!check_hr("D3DCompile(VS)", hr)) return 1;
    hr = pD3DCompile(ps_src, strlen(ps_src), "ps.hlsl", NULL, NULL, "main", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    if (!check_hr("D3DCompile(PS)", hr)) return 1;

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs);
    if (!check_hr("CreateVertexShader", hr)) return 1;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps);
    if (!check_hr("CreatePixelShader", hr)) return 1;

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg_tex, (ID3D11Resource *)rt_tex);

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (!check_hr("Map(Draw)", hr)) return 1;
    px = (const uint8_t *)mapped.pData;
    if (!approx_u8(px[0], 255) || !approx_u8(px[1], 64) || !approx_u8(px[2], 32) || !approx_u8(px[3], 255)) {
        fprintf(stderr, "FAIL d3d11_draw: got (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);
        return 1;
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg_tex, 0);
    printf("PASS d3d11_draw (%u,%u,%u,%u)\n", px[0], px[1], px[2], px[3]);

    ID3D11VertexShader_Release(vs);
    ID3D11PixelShader_Release(ps);
    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_blob);

    /* Test 3: Compute shader UAV write + staging readback */
    static const char *cs_src =
        "RWStructuredBuffer<uint> outbuf : register(u0);\n"
        "[numthreads(64, 1, 1)]\n"
        "void main(uint3 tid : SV_DispatchThreadID) {\n"
        "  outbuf[tid.x] = tid.x * 3u + 7u;\n"
        "}\n";
    ID3DBlob *cs_blob = NULL;
    hr = pD3DCompile(cs_src, strlen(cs_src), "cs.hlsl", NULL, NULL, "main", "cs_5_0", 0, 0, &cs_blob, &err_blob);
    if (!check_hr("D3DCompile(CS)", hr)) return 1;

    ID3D11ComputeShader *cs = NULL;
    hr = ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cs_blob), ID3D10Blob_GetBufferSize(cs_blob), NULL, &cs);
    if (!check_hr("CreateComputeShader", hr)) return 1;

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 64 * sizeof(uint32_t);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(uint32_t);
    ID3D11Buffer *uav_buf = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &uav_buf);
    if (!check_hr("CreateBuffer(UAV)", hr)) return 1;

    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;
    ID3D11Buffer *stg_buf = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &stg_buf);
    if (!check_hr("CreateBuffer(Staging)", hr)) return 1;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {0};
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = 64;
    ID3D11UnorderedAccessView *uav = NULL;
    hr = ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)uav_buf, &uavd, &uav);
    if (!check_hr("CreateUAV", hr)) return 1;

    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg_buf, (ID3D11Resource *)uav_buf);

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg_buf, 0, D3D11_MAP_READ, 0, &mapped);
    if (!check_hr("Map(CS)", hr)) return 1;
    const uint32_t *u32 = (const uint32_t *)mapped.pData;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t exp = i * 3u + 7u;
        if (u32[i] != exp) {
            fprintf(stderr, "FAIL d3d11_compute: idx=%u got=%u exp=%u\n", i, u32[i], exp);
            return 1;
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg_buf, 0);
    printf("PASS d3d11_compute (64 elements)\n");

    ID3D11UnorderedAccessView_Release(uav);
    ID3D11Buffer_Release(stg_buf);
    ID3D11Buffer_Release(uav_buf);
    ID3D11ComputeShader_Release(cs);
    ID3D10Blob_Release(cs_blob);

done:
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(stg_tex);
    ID3D11Texture2D_Release(rt_tex);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    return 0;
}
