#include "cubism_viewer_desktop_capture.h"

#include <stdlib.h>
#include <string.h>

static void release_unknown(IUnknown *value) {
    if (value) IUnknown_Release(value);
}

void desktop_capture_destroy(DesktopCapture *capture) {
    release_unknown((IUnknown *)capture->staging);
    release_unknown((IUnknown *)capture->duplication);
    release_unknown((IUnknown *)capture->context);
    release_unknown((IUnknown *)capture->device);
    memset(capture, 0, sizeof(*capture));
}

int desktop_capture_init(DesktopCapture *capture, RECT crop) {
    IDXGIFactory1 *factory = NULL;
    UINT adapter_index;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    memset(capture, 0, sizeof(*capture)); capture->crop = crop;
    if (FAILED(hr)) return 0;
    for (adapter_index = 0; ; ++adapter_index) {
        IDXGIAdapter1 *adapter = NULL;
        UINT output_index;
        hr = IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        for (output_index = 0; ; ++output_index) {
            IDXGIOutput *output = NULL;
            IDXGIOutput1 *output1 = NULL;
            DXGI_OUTPUT_DESC description;
            D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
            D3D_FEATURE_LEVEL level;
            hr = IDXGIAdapter1_EnumOutputs(adapter, output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;
            memset(&description, 0, sizeof(description));
            IDXGIOutput_GetDesc(output, &description);
            if (!description.AttachedToDesktop || crop.left <
                description.DesktopCoordinates.left || crop.top <
                description.DesktopCoordinates.top || crop.right >
                description.DesktopCoordinates.right || crop.bottom >
                description.DesktopCoordinates.bottom) {
                release_unknown((IUnknown *)output); continue;
            }
            hr = D3D11CreateDevice((IDXGIAdapter *)adapter,
                D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION,
                &capture->device, &level, &capture->context);
            if (FAILED(hr)) { release_unknown((IUnknown *)output); continue; }
            hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
                (void **)&output1);
            if (SUCCEEDED(hr)) hr = IDXGIOutput1_DuplicateOutput(output1,
                (IUnknown *)capture->device, &capture->duplication);
            release_unknown((IUnknown *)output1);
            release_unknown((IUnknown *)output);
            if (SUCCEEDED(hr)) {
                capture->output = description.DesktopCoordinates;
                release_unknown((IUnknown *)adapter);
                release_unknown((IUnknown *)factory);
                return 1;
            }
            release_unknown((IUnknown *)capture->context); capture->context = NULL;
            release_unknown((IUnknown *)capture->device); capture->device = NULL;
        }
        release_unknown((IUnknown *)adapter);
    }
    release_unknown((IUnknown *)factory);
    desktop_capture_destroy(capture);
    return 0;
}

static int copy_frame(DesktopCapture *capture, ID3D11Texture2D *source,
    Frame *frame) {
    D3D11_TEXTURE2D_DESC source_description;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_BOX box;
    int width = capture->crop.right - capture->crop.left;
    int height = capture->crop.bottom - capture->crop.top;
    int y;
    ID3D11Texture2D_GetDesc(source, &source_description);
    if (source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return 0;
    if (!capture->staging) {
        D3D11_TEXTURE2D_DESC description;
        memset(&description, 0, sizeof(description));
        description.Width = (UINT)width; description.Height = (UINT)height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = source_description.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(capture->device, &description,
            NULL, &capture->staging))) return 0;
    }
    box.left = (UINT)(capture->crop.left - capture->output.left);
    box.top = (UINT)(capture->crop.top - capture->output.top); box.front = 0;
    box.right = (UINT)(capture->crop.right - capture->output.left);
    box.bottom = (UINT)(capture->crop.bottom - capture->output.top); box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(capture->context,
        (ID3D11Resource *)capture->staging, 0, 0, 0, 0,
        (ID3D11Resource *)source, 0, &box);
    memset(&mapped, 0, sizeof(mapped));
    if (FAILED(ID3D11DeviceContext_Map(capture->context,
        (ID3D11Resource *)capture->staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return 0;
    frame->size = (size_t)width * height * 4;
    frame->pixels = (unsigned char *)malloc(frame->size);
    if (!frame->pixels) {
        ID3D11DeviceContext_Unmap(capture->context,
            (ID3D11Resource *)capture->staging, 0); return 0;
    }
    for (y = 0; y < height; ++y)
        memcpy(frame->pixels + (size_t)y * width * 4,
            (const unsigned char *)mapped.pData + (size_t)y * mapped.RowPitch,
            (size_t)width * 4);
    ID3D11DeviceContext_Unmap(capture->context,
        (ID3D11Resource *)capture->staging, 0);
    return 1;
}

int desktop_capture_at(DesktopCapture *capture, LONGLONG target, Frame *frame) {
    for (;;) {
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource *resource = NULL;
        HRESULT hr;
        int selected, copied = 0;
        memset(&info, 0, sizeof(info));
        hr = IDXGIOutputDuplication_AcquireNextFrame(capture->duplication,
            1000, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) return 0;
        selected = info.LastPresentTime.QuadPart >= target;
        if (selected) {
            ID3D11Texture2D *texture = NULL;
            hr = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D,
                (void **)&texture);
            if (SUCCEEDED(hr)) copied = copy_frame(capture, texture, frame);
            release_unknown((IUnknown *)texture);
            frame->present = info.LastPresentTime.QuadPart;
        }
        release_unknown((IUnknown *)resource);
        IDXGIOutputDuplication_ReleaseFrame(capture->duplication);
        if (selected) return copied;
    }
}
