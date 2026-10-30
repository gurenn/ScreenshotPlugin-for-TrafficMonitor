// ImageWriter.cpp - WIC PNG/JPG/BMP encoding, unique file names, clipboard.
#include "ImageWriter.h"

#include <cstring>

namespace
{

// Minimal COM smart pointer: the project forbids ATL, so we roll our own.
template <typename T>
class CComPtrLite
{
public:
    CComPtrLite() : m_p(nullptr) {}
    ~CComPtrLite() { reset(); }

    CComPtrLite(const CComPtrLite&) = delete;
    CComPtrLite& operator=(const CComPtrLite&) = delete;

    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    T** receive()
    {
        reset();
        return &m_p;
    }
    void reset(T* p = nullptr)
    {
        if (m_p != nullptr)
        {
            m_p->Release();
        }
        m_p = p;
    }
    bool isNull() const { return m_p == nullptr; }

private:
    T* m_p;
};

GUID ContainerFormatFor(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Jpg:
        return GUID_ContainerFormatJpeg;
    case ImageFormat::Bmp:
        return GUID_ContainerFormatBmp;
    case ImageFormat::Png:
    default:
        return GUID_ContainerFormatPng;
    }
}

} // namespace

CImageWriter::CImageWriter()
{
}

CImageWriter::~CImageWriter()
{
    ShutdownThread();
}

CImageWriter::CThreadScope::CThreadScope(CImageWriter* writer, const std::atomic<bool>* shutdownFlag)
    : m_writer(writer), m_shutdownFlag(shutdownFlag)
{
    if (m_writer != nullptr)
    {
        m_writer->InitThread();
    }
}

CImageWriter::CThreadScope::~CThreadScope()
{
    if (m_writer == nullptr)
    {
        return;
    }
    if (m_shutdownFlag != nullptr && m_shutdownFlag->load())
    {
        // DLL 正在卸载：跳过 COM 收尾，避免在 loader lock 上二次阻塞（DEF-02）。
        return;
    }
    m_writer->ShutdownThread();
}

bool CImageWriter::InitThread()
{
    if (!m_comUsable)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr))
        {
            m_comInitialized = true;
            m_comUsable = true;
        }
        else if (hr == RPC_E_CHANGED_MODE)
        {
            // The host already initialised this thread as MTA; COM is usable.
            m_comInitialized = false;
            m_comUsable = true;
        }
        else
        {
            SNAP_LOGE(static_cast<SnapCode>(hr), L"CoInitializeEx failed");
            m_comUsable = false;
            return false;
        }
    }
    return true;
}

void CImageWriter::ShutdownThread()
{
    if (m_factory != nullptr)
    {
        m_factory->Release();
        m_factory = nullptr;
    }
    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
    m_comUsable = false;
}

bool CImageWriter::EnsureFactory(HRESULT& outHr)
{
    outHr = S_OK;
    if (m_factory != nullptr)
    {
        return true;
    }
    if (!InitThread())
    {
        outHr = E_FAIL;
        return false;
    }
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_factory));
    if (FAILED(hr))
    {
        // Windows 8+ exposes the newer factory; try it as a fallback.
        m_factory = nullptr;
        hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&m_factory));
    }
    if (FAILED(hr) || m_factory == nullptr)
    {
        m_factory = nullptr;
        outHr = hr;
        SNAP_LOGE(kErrWicFactory, L"CoCreateInstance(WICImagingFactory) failed");
        return false;
    }
    return true;
}

bool CImageWriter::MakeUniquePath(const std::wstring& desired, std::wstring& out)
{
    out.clear();
    if (desired.empty() || desired.size() >= MAX_PATH)
    {
        return false;
    }
    if (GetFileAttributesW(desired.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        out = desired;
        return true;
    }

    std::wstring dir = snap::GetDirectoryPart(desired);
    std::wstring name = snap::GetFileNamePart(desired);
    std::wstring stem = name;
    std::wstring ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
    {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }

    for (int index = 1; index <= 999; ++index)
    {
        std::wstring candidateName = stem + snap::FormatString(L"_%d", index) + ext;
        std::wstring candidate = snap::JoinPath(dir, candidateName);
        if (candidate.size() >= MAX_PATH)
        {
            return false;
        }
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            out = candidate;
            return true;
        }
    }

    std::wstring candidateName =
        stem + snap::FormatString(L"_%llu", static_cast<unsigned long long>(GetTickCount64() % 100000)) + ext;
    std::wstring candidate = snap::JoinPath(dir, candidateName);
    if (candidate.size() >= MAX_PATH)
    {
        return false;
    }
    out = candidate;
    return true;
}

SnapCode CImageWriter::Save(const CapturedFrame& frame, const CPluginSettings& settings, std::wstring& outPath,
                            std::wstring& outErr)
{
    outPath.clear();
    outErr.clear();

    if (!frame.IsValid())
    {
        return kErrInternal;
    }

    std::wstring dirErr;
    std::wstring dir = settings.EffectiveSaveDir();
    bool usedFallback = false;
    bool dirOk = settings.EnsureSaveDir(dirErr);
    if (!dirOk)
    {
        if (settings.data.fallbackToPicturesRoot)
        {
            std::wstring pictures = snap::GetPicturesDirectory();
            if (!pictures.empty() && snap::EnsureDirectory(pictures) && snap::IsDirectoryWritable(pictures))
            {
                dir = pictures;
                usedFallback = true;
                dirOk = true;
            }
        }
        if (!dirOk)
        {
            // EnsureSaveDir() 的 err 出参本身就是路径，不要拼成 "path | path"。
            if (dirErr.empty() || dirErr == dir)
            {
                outErr = dir;
            }
            else
            {
                outErr = dir + L" | " + dirErr;
            }
            SNAP_LOGE(kErrDirNotWritable, L"save directory is not writable: %s", dir.c_str());
            return kErrDirNotWritable;
        }
    }

    if (!snap::IsDirectoryWritable(dir))
    {
        outErr = dir;
        SNAP_LOGE(kErrDirNotWritable, L"save directory is not writable: %s", dir.c_str());
        return kErrDirNotWritable;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::wstring fileName = settings.BuildFileName(now);
    std::wstring desired = snap::JoinPath(dir, fileName);
    if (desired.size() >= MAX_PATH)
    {
        outErr = desired;
        SNAP_LOGE(kErrWrite, L"resulting path exceeds MAX_PATH");
        return kErrWrite;
    }

    std::wstring uniquePath;
    if (!MakeUniquePath(desired, uniquePath))
    {
        outErr = desired;
        SNAP_LOGE(kErrWrite, L"cannot build a unique file name for %s", desired.c_str());
        return kErrWrite;
    }

    SnapCode code = EncodeToFile(frame, uniquePath, settings.data.format, settings.data.jpgQuality, outErr);
    if (code != kOk)
    {
        DeleteFileW(uniquePath.c_str());
        return code;
    }

    outPath = uniquePath;
    if (usedFallback)
    {
        outErr = dir;
    }
    SNAP_LOG(L"saved %s (%dx%d)", uniquePath.c_str(), frame.width, frame.height);
    return kOk;
}

SnapCode CImageWriter::EncodeToFile(const CapturedFrame& frame, const std::wstring& path, ImageFormat format,
                                    int quality, std::wstring& outErr)
{
    outErr.clear();

    HRESULT factoryHr = S_OK;
    if (!EnsureFactory(factoryHr))
    {
        outErr = snap::FormatString(L"0x%08X", static_cast<unsigned int>(factoryHr));
        return kErrWicFactory;
    }

    CComPtrLite<IWICBitmap> bitmap;
    HRESULT hr = m_factory->CreateBitmapFromMemory(
        static_cast<UINT>(frame.width), static_cast<UINT>(frame.height), GUID_WICPixelFormat32bppBGRA,
        static_cast<UINT>(frame.stride), static_cast<UINT>(frame.stride) * static_cast<UINT>(frame.height),
        static_cast<BYTE*>(frame.bits), bitmap.receive());
    if (FAILED(hr) || bitmap.isNull())
    {
        outErr = snap::FormatString(L"CreateBitmapFromMemory 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"CreateBitmapFromMemory failed");
        return kErrWicEncoder;
    }

    CComPtrLite<IWICBitmapEncoder> encoder;
    hr = m_factory->CreateEncoder(ContainerFormatFor(format), nullptr, encoder.receive());
    if (FAILED(hr) || encoder.isNull())
    {
        outErr = snap::FormatString(L"CreateEncoder 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"CreateEncoder failed for %s", ImageFormatDisplayName(format));
        return kErrWicEncoder;
    }

    CComPtrLite<IWICStream> stream;
    hr = m_factory->CreateStream(stream.receive());
    if (FAILED(hr) || stream.isNull())
    {
        outErr = snap::FormatString(L"CreateStream 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"IWICImagingFactory::CreateStream failed");
        return kErrWicEncoder;
    }
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"InitializeFromFilename 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWrite, L"InitializeFromFilename failed: %s", path.c_str());
        return kErrWrite;
    }

    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"Encoder::Initialize 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"encoder Initialize failed");
        return kErrWicEncoder;
    }

    CComPtrLite<IWICBitmapFrameEncode> frameEncode;
    CComPtrLite<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frameEncode.receive(), properties.receive());
    if (FAILED(hr) || frameEncode.isNull())
    {
        outErr = snap::FormatString(L"CreateNewFrame 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"CreateNewFrame failed");
        return kErrWicEncoder;
    }

    // WIC's JPEG encoder reads the property bag during IWICBitmapFrameEncode::
    // Initialize() and only then picks the quantisation tables.  ImageQuality
    // must therefore be written BEFORE Initialize, otherwise the encoder
    // silently ignores it and every quality produces byte-identical DQT tables
    // (DEF-01, reproduced by QA with tools/qa_wic.cpp).
    if (format == ImageFormat::Jpg && !properties.isNull())
    {
        PROPBAG2 option{};
        option.dwType = PROPBAG2_TYPE_DATA;
        option.vt = VT_R4;
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        int clamped = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
        value.fltVal = static_cast<float>(clamped) / 100.0f;
        HRESULT qualityHr = properties->Write(1, &option, &value);
        VariantClear(&value);
        if (FAILED(qualityHr))
        {
            outErr = snap::FormatString(L"ImageQuality 0x%08X", static_cast<unsigned int>(qualityHr));
            SNAP_LOGE(kErrWicEncoder, L"setting ImageQuality failed");
            return kErrWicEncoder;
        }
        SNAP_LOG(L"jpeg quality = %d/100", clamped);
    }

    hr = frameEncode->Initialize(properties.get());
    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"Frame::Initialize 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"frame Initialize failed");
        return kErrWicEncoder;
    }

    // PNG keeps the 32bpp alpha; JPG/BMP are best served by 24bpp BGR.
    if (format == ImageFormat::Png)
    {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        frameEncode->SetPixelFormat(&pixelFormat);
        hr = frameEncode->WriteSource(bitmap.get(), nullptr);
    }
    else
    {
        CComPtrLite<IWICFormatConverter> converter;
        hr = m_factory->CreateFormatConverter(converter.receive());
        if (SUCCEEDED(hr) && !converter.isNull())
        {
            hr = converter->Initialize(bitmap.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                       nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        if (FAILED(hr) || converter.isNull())
        {
            outErr = snap::FormatString(L"CreateFormatConverter 0x%08X", static_cast<unsigned int>(hr));
            SNAP_LOGE(kErrWicEncoder, L"format conversion failed");
            return kErrWicEncoder;
        }
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
        frameEncode->SetPixelFormat(&pixelFormat);
        hr = frameEncode->WriteSource(converter.get(), nullptr);
    }

    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"WriteSource 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWicEncoder, L"WriteSource failed");
        return kErrWicEncoder;
    }

    hr = frameEncode->Commit();
    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"Frame::Commit 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWrite, L"frame Commit failed");
        return kErrWrite;
    }

    hr = encoder->Commit();
    if (FAILED(hr))
    {
        outErr = snap::FormatString(L"Encoder::Commit 0x%08X", static_cast<unsigned int>(hr));
        SNAP_LOGE(kErrWrite, L"encoder Commit failed: %s", path.c_str());
        return kErrWrite;
    }

    return kOk;
}

SnapCode CImageWriter::EncodeToMemoryPng(const CapturedFrame& frame, std::vector<BYTE>& outBytes,
                                         std::wstring& outErr)
{
    outBytes.clear();
    outErr.clear();

    HRESULT factoryHr = S_OK;
    if (!EnsureFactory(factoryHr))
    {
        outErr = snap::FormatString(L"0x%08X", static_cast<unsigned int>(factoryHr));
        return kErrWicFactory;
    }

    CComPtrLite<IWICBitmap> bitmap;
    HRESULT hr = m_factory->CreateBitmapFromMemory(
        static_cast<UINT>(frame.width), static_cast<UINT>(frame.height), GUID_WICPixelFormat32bppBGRA,
        static_cast<UINT>(frame.stride), static_cast<UINT>(frame.stride) * static_cast<UINT>(frame.height),
        static_cast<BYTE*>(frame.bits), bitmap.receive());
    if (FAILED(hr) || bitmap.isNull())
    {
        return kErrWicEncoder;
    }

    CComPtrLite<IStream> memoryStream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, memoryStream.receive());
    if (FAILED(hr) || memoryStream.isNull())
    {
        return kErrWicEncoder;
    }

    CComPtrLite<IWICBitmapEncoder> encoder;
    hr = m_factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.receive());
    if (FAILED(hr) || encoder.isNull())
    {
        return kErrWicEncoder;
    }

    hr = encoder->Initialize(memoryStream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        return kErrWicEncoder;
    }

    CComPtrLite<IWICBitmapFrameEncode> frameEncode;
    CComPtrLite<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frameEncode.receive(), properties.receive());
    if (FAILED(hr) || frameEncode.isNull())
    {
        return kErrWicEncoder;
    }

    hr = frameEncode->Initialize(properties.get());
    if (FAILED(hr))
    {
        return kErrWicEncoder;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    frameEncode->SetPixelFormat(&pixelFormat);
    hr = frameEncode->WriteSource(bitmap.get(), nullptr);
    if (FAILED(hr))
    {
        return kErrWicEncoder;
    }
    if (FAILED(frameEncode->Commit()) || FAILED(encoder->Commit()))
    {
        return kErrWicEncoder;
    }

    HGLOBAL handle = nullptr;
    if (FAILED(GetHGlobalFromStream(memoryStream.get(), &handle)) || handle == nullptr)
    {
        return kErrWicEncoder;
    }
    const SIZE_T size = GlobalSize(handle);
    if (size == 0)
    {
        return kErrWicEncoder;
    }
    const BYTE* data = static_cast<const BYTE*>(GlobalLock(handle));
    if (data == nullptr)
    {
        return kErrWicEncoder;
    }
    outBytes.assign(data, data + size);
    GlobalUnlock(handle);
    return kOk;
}

SnapCode CImageWriter::CopyToClipboard(const CapturedFrame& frame, HWND hOwner)
{
    if (!frame.IsValid())
    {
        return kErrClipboard;
    }

    // Format 1: a 24bpp bottom-up CF_DIB.  This is the shape that Paint,
    // WeChat, Word and friends all understand.
    const int width = frame.width;
    const int height = frame.height;
    const int rowBytes = ((width * 3) + 3) & ~3;
    const SIZE_T pixelBytes = static_cast<SIZE_T>(rowBytes) * static_cast<SIZE_T>(height);
    const SIZE_T totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;

    HGLOBAL dibHandle = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
    if (dibHandle == nullptr)
    {
        return kErrClipboard;
    }
    void* raw = GlobalLock(dibHandle);
    if (raw == nullptr)
    {
        GlobalFree(dibHandle);
        return kErrClipboard;
    }

    BITMAPINFOHEADER* header = static_cast<BITMAPINFOHEADER*>(raw);
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height; // positive: bottom-up
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);
    header->biXPelsPerMeter = 0;
    header->biYPelsPerMeter = 0;
    header->biClrUsed = 0;
    header->biClrImportant = 0;

    BYTE* destination = static_cast<BYTE*>(raw) + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < height; ++y)
    {
        const BYTE* source =
            static_cast<const BYTE*>(frame.bits) + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(frame.stride);
        BYTE* row = destination + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
        for (int x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = source[x * 4 + 0];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + 2];
        }
    }
    GlobalUnlock(dibHandle);

    std::vector<BYTE> pngBytes;
    std::wstring pngErr;
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    EncodeToMemoryPng(frame, pngBytes, pngErr);

    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt)
    {
        if (OpenClipboard(hOwner) != FALSE)
        {
            opened = true;
            break;
        }
        Sleep(20);
    }
    if (!opened)
    {
        GlobalFree(dibHandle);
        SNAP_LOGE(kErrClipboard, L"OpenClipboard failed");
        return kErrClipboard;
    }

    EmptyClipboard();

    bool dibOk = false;
    if (SetClipboardData(CF_DIB, dibHandle) != nullptr)
    {
        dibOk = true; // ownership transferred to the system
    }
    else
    {
        GlobalFree(dibHandle);
    }

    bool pngOk = false;
    if (!pngBytes.empty() && pngFormat != 0)
    {
        HGLOBAL pngHandle = GlobalAlloc(GMEM_MOVEABLE, pngBytes.size());
        if (pngHandle != nullptr)
        {
            void* pngRaw = GlobalLock(pngHandle);
            if (pngRaw != nullptr)
            {
                memcpy(pngRaw, pngBytes.data(), pngBytes.size());
                GlobalUnlock(pngHandle);
                if (SetClipboardData(pngFormat, pngHandle) != nullptr)
                {
                    pngOk = true;
                }
                else
                {
                    GlobalFree(pngHandle);
                }
            }
            else
            {
                GlobalFree(pngHandle);
            }
        }
    }

    CloseClipboard();

    if (!dibOk && !pngOk)
    {
        SNAP_LOGE(kErrClipboard, L"SetClipboardData failed for both CF_DIB and PNG");
        return kErrClipboard;
    }
    return kOk;
}
