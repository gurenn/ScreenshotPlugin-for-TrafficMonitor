// tools/qa_wic.cpp - root-cause experiment for the "JPG quality has no effect" bug.
//
// It encodes the SAME synthetic 32bpp image twice at a very low and a very high
// JPEG quality, using two different orderings of the WIC property bag:
//   A) CreateNewFrame -> Initialize(bag) -> bag.Write("ImageQuality") -> ...  (what src/ImageWriter.cpp does)
//   B) CreateNewFrame -> bag.Write("ImageQuality") -> Initialize(bag) -> ...
// The file sizes tell which ordering the WIC JPEG encoder actually honours.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

template <typename T>
class ComPtr
{
public:
    ~ComPtr() { reset(); }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    T** receive()
    {
        reset();
        return &m_p;
    }
    void reset()
    {
        if (m_p != nullptr)
        {
            m_p->Release();
            m_p = nullptr;
        }
    }

private:
    T* m_p{nullptr};
};

bool WriteBag(IPropertyBag2* bag, float quality)
{
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    const HRESULT hr = bag->Write(1, &option, &value);
    VariantClear(&value);
    if (FAILED(hr))
    {
        printf("     bag->Write failed 0x%08X\n", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool EncodeJpeg(IWICImagingFactory* factory, IWICBitmap* bitmap, const wchar_t* path, float quality,
                bool writeBeforeInitialize, unsigned long long& outSize)
{
    outSize = 0;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.receive())))
    {
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.receive())))
    {
        return false;
    }
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)))
    {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(frame.receive(), bag.receive())) || frame.get() == nullptr ||
        bag.get() == nullptr)
    {
        return false;
    }

    if (writeBeforeInitialize)
    {
        WriteBag(bag.get(), quality); // variant B: options first
        if (FAILED(frame->Initialize(bag.get())))
        {
            return false;
        }
    }
    else
    {
        if (FAILED(frame->Initialize(bag.get())))
        {
            return false;
        }
        WriteBag(bag.get(), quality); // variant A: options after (src/ImageWriter.cpp)
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.receive())))
    {
        return false;
    }
    if (FAILED(converter->Initialize(bitmap, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&format);
    if (FAILED(frame->WriteSource(converter.get(), nullptr)))
    {
        return false;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    outSize = static_cast<unsigned long long>(size.QuadPart);
    return outSize > 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::wstring outDir = (argc > 1) ? std::wstring(argv[1], argv[1] + strlen(argv[1])) : std::wstring(L".");

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = 0;

    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.receive()))) ||
            factory.get() == nullptr)
        {
            printf("no WIC factory\n");
            rc = 1;
        }
        else
        {
            // A detailed synthetic image: deterministic noise over a gradient.
            const int width = 640;
            const int height = 480;
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4, 0);
            unsigned seed = 12345;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    seed = seed * 1103515245u + 12345u;
                    const unsigned char noise = static_cast<unsigned char>((seed >> 16) & 0x3F);
                    unsigned char* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                    p[0] = static_cast<unsigned char>((x * 255 / width + noise) & 0xFF);
                    p[1] = static_cast<unsigned char>((y * 255 / height + noise) & 0xFF);
                    p[2] = static_cast<unsigned char>(((x + y) * 127 / (width + height) + noise) & 0xFF);
                    p[3] = 255;
                }
            }

            ComPtr<IWICBitmap> bitmap;
            if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                                       static_cast<UINT>(pixels.size()), pixels.data(),
                                                       bitmap.receive())))
            {
                printf("CreateBitmapFromMemory failed\n");
                rc = 1;
            }
            else
            {
                struct Case
                {
                    const wchar_t* name;
                    float quality;
                    bool before;
                };
                const Case cases[] = {
                    {L"A_quality10_after_init.jpg", 0.10f, false},
                    {L"A_quality90_after_init.jpg", 0.90f, false},
                    {L"B_quality10_before_init.jpg", 0.10f, true},
                    {L"B_quality90_before_init.jpg", 0.90f, true},
                };

                unsigned long long sizes[4] = {0, 0, 0, 0};
                for (int i = 0; i < 4; ++i)
                {
                    std::wstring path = outDir + L"\\" + cases[i].name;
                    EncodeJpeg(factory.get(), bitmap.get(), path.c_str(), cases[i].quality, cases[i].before,
                               sizes[i]);
                    printf("  case %d  quality=%.2f  size=%llu\n", i, cases[i].quality, sizes[i]);
                }

                printf("\nvariant A (Write AFTER frame->Initialize, same as src/ImageWriter.cpp):\n");
                printf("  quality 0.10 -> %llu bytes\n  quality 0.90 -> %llu bytes\n", sizes[0], sizes[1]);
                printf("variant B (Write BEFORE frame->Initialize):\n");
                printf("  quality 0.10 -> %llu bytes\n  quality 0.90 -> %llu bytes\n", sizes[2], sizes[3]);

                const bool aWorks = sizes[1] > sizes[0] * 2;
                const bool bWorks = sizes[3] > sizes[2] * 2;
                printf("\nCONCLUSION: 'write after Initialize' honours ImageQuality = %s\n", aWorks ? "yes" : "NO");
                printf("            'write before Initialize' honours ImageQuality = %s\n", bWorks ? "yes" : "NO");
                fflush(stdout);
            }
        }
    }

    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    return rc;
}
