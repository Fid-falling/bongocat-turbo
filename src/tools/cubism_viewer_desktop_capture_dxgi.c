#include "cubism_viewer_desktop_capture.h"

#include <stdlib.h>
#include <string.h>

/*
 * DXGI 桌面复制的实现部分(接口声明在 cubism_viewer_desktop_capture.h)。
 *
 * 为什么不用 BitBlt/GDI 截屏: 需要的是 DWM 合成后的真实画面以及它的呈现时间戳,
 * GDI 截屏既拿不到时间戳, 也抓不全带透明/硬件合成的窗口。
 * 定义了 COBJMACROS 后用 C 风格调用 COM, 引用计数全部显式 Release; 任何失败路径
 * 都必须先释放再返回, 否则这种长时间运行的采集工具会稳定泄漏。
 */
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

/*
 * 枚举所有适配器与输出, 找到包含 crop 的那块桌面输出, 在它上面建 D3D11 设备并
 * 开启复制。D3D_DRIVER_TYPE_UNKNOWN 是必须的: 一旦指定了具体适配器, 就不能再
 * 传 HARDWARE, 否则 D3D11CreateDevice 直接失败; BGRA_SUPPORT 是为桌面复制唯一
 * 会给出的 DXGI_FORMAT_B8G8R8A8_UNORM 准备的。
 * 中途用不上的输出/适配器要立刻释放: 只要 duplication 还活着, 桌面复制就一直
 * 占着这块输出。
 */
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

/*
 * 把整块桌面输出里的 crop 区域拷进 staging 纹理, 再映射到 CPU 内存。
 * staging 纹理只创建一次(缓存)且尺寸正好等于 crop: 每帧新建纹理明显更慢。
 * box 的坐标相对 output 左上角, 所以要减 output.left/top。
 * mapped.RowPitch 通常大于 width*4, 必须逐行拷贝 —— 整块 memcpy 会得到错位图像。
 * 只接受 B8G8R8A8_UNORM: 其它格式(比如 HDR 的 10 位格式)直接判失败, 免得后面把
 * 数据当成 8 位来解释。
 */
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

/*
 * 一直取帧直到 present 时间戳 >= target 的那一帧。超时(说明屏幕这一秒内没有更新)
 * 就继续等 —— 这个工具宁可等, 也不能返回一帧比请求更早的画面, 否则时序分析全部
 * 失真。
 * 提前取到的帧也必须 ReleaseFrame, 否则下一次 AcquireNextFrame 会一直返回同一帧。
 */
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
