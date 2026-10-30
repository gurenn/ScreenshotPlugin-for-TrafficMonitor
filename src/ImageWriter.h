// ImageWriter.h - WIC encoding, unique naming, clipboard support.
#pragma once

#include "CaptureEngine.h"

class CImageWriter
{
public:
    // RAII scope for the COM/WIC lifetime of one capture job.
    //
    // Why this exists (DEF-02): releasing the IWICImagingFactory or calling
    // CoUninitialize from the worker thread's *exit* path can need the loader
    // lock (FreeLibrary of windowscodecs.dll etc.).  DllMain(PROCESS_DETACH)
    // runs under that very lock, so the bounded join there would time out, the
    // image would be unmapped and the worker would fault inside our code.
    // Keeping COM/WIC strictly inside a job means an idle worker thread owns no
    // COM apartment and no WIC object, so its exit is pure Win32.
    //
    // If shutdownFlag becomes true the destructor skips the COM teardown on
    // purpose: the process is already unloading, the DLL is about to be
    // unmapped, and teardown could block on the loader lock again.
    class CThreadScope
    {
    public:
        CThreadScope(CImageWriter* writer, const std::atomic<bool>* shutdownFlag);
        ~CThreadScope();

        CThreadScope(const CThreadScope&) = delete;
        CThreadScope& operator=(const CThreadScope&) = delete;

    private:
        CImageWriter* m_writer;
        const std::atomic<bool>* m_shutdownFlag;
    };

    CImageWriter();
    ~CImageWriter();

    CImageWriter(const CImageWriter&) = delete;
    CImageWriter& operator=(const CImageWriter&) = delete;

    // Per-thread COM/WIC initialisation.  Safe to call repeatedly.
    bool InitThread();
    void ShutdownThread();

    SnapCode Save(const CapturedFrame& frame, const CPluginSettings& settings, std::wstring& outPath,
                  std::wstring& outErr);
    SnapCode CopyToClipboard(const CapturedFrame& frame, HWND hOwner);

    static bool MakeUniquePath(const std::wstring& desired, std::wstring& out);

private:
    bool EnsureFactory(HRESULT& outHr);
    SnapCode EncodeToFile(const CapturedFrame& frame, const std::wstring& path, ImageFormat format, int quality,
                          std::wstring& outErr);
    SnapCode EncodeToMemoryPng(const CapturedFrame& frame, std::vector<BYTE>& outBytes, std::wstring& outErr);

    IWICImagingFactory* m_factory{nullptr};
    bool m_comInitialized{false};
    bool m_comUsable{false};
};
