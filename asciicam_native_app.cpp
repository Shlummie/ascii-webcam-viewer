#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <dshow.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// The legacy qedit.h header was removed from modern Windows SDKs, although the
// DirectShow Sample Grabber COM interfaces remain available at runtime. Keep
// the small interface declarations local so the source builds with a current
// SDK without depending on an obsolete header.
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLength) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* mediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferSamples) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* bufferSize, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long callbackMethod) = 0;
};

namespace {

constexpr wchar_t kMainClassName[] = L"ASCIICAMNativeWindow";
constexpr wchar_t kPreviewClassName[] = L"ASCIICAMPreviewWindow";
constexpr wchar_t kAppTitle[] = L"ASCIICAM - Native C++";
constexpr wchar_t kAsciiChars[] = L" .'`^\",:;Il!i~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
const CLSID kClsidSampleGrabber = {0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
const CLSID kClsidNullRenderer = {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
constexpr int kRenderTimerId = 1;
constexpr int kLeftPanelWidth = 300;
constexpr int kMargin = 8;
constexpr int kControlWidth = 280;
constexpr int kButtonHeight = 28;
constexpr int kLabelHeight = 18;
constexpr int kEditHeight = 24;
constexpr int kComboHeight = 220;
constexpr int kTrackHeight = 30;
constexpr int kDetectedHeight = 38;
constexpr int kTipsHeight = 94;
constexpr int kStatusHeight = 26;

enum ControlId {
    IDC_PREVIEW = 100,
    IDC_STATUS = 101,
    IDC_START = 102,
    IDC_STOP = 103,
    IDC_DETECT = 104,
    IDC_SAVE = 105,
    IDC_CAMERA_LABEL = 106,
    IDC_CAMERA_EDIT = 107,
    IDC_DETECTED = 108,
    IDC_RES_LABEL = 109,
    IDC_RES_COMBO = 110,
    IDC_COLS_LABEL = 111,
    IDC_COLS_VALUE = 112,
    IDC_COLS_TRACK = 113,
    IDC_SCALE_LABEL = 114,
    IDC_SCALE_VALUE = 115,
    IDC_SCALE_TRACK = 116,
    IDC_FPS_LABEL = 117,
    IDC_FPS_VALUE = 118,
    IDC_FPS_TRACK = 119,
    IDC_BOOST_LABEL = 120,
    IDC_BOOST_VALUE = 121,
    IDC_BOOST_TRACK = 122,
    IDC_MIRROR = 123,
    IDC_INVERT = 124,
    IDC_TIPS = 125,
};

struct CapturePreset {
    const wchar_t* label;
    int width;
    int height;
};

constexpr CapturePreset kCapturePresets[] = {
    {L"640x480 (4:3)", 640, 480},
    {L"1280x720 (720p 16:9)", 1280, 720},
    {L"1920x1080 (1080p 16:9)", 1920, 1080},
};

struct FrameBuffer {
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    int stride = 0;
    int bytesPerPixel = 0;
    bool bottomUp = true;
    bool valid = false;
};

struct PreviewImage {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    bool valid = false;
};

int ClampInt(int value, int minimum, int maximum) {
    return std::max(minimum, std::min(value, maximum));
}

std::uint8_t ClampByte(int value) {
    return static_cast<std::uint8_t>(ClampInt(value, 0, 255));
}

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash != nullptr) {
        *lastSlash = L'\0';
    }
    return path;
}

std::wstring JoinPath(const std::wstring& left, const wchar_t* right) {
    std::wstring out = left;
    if (!out.empty() && out.back() != L'\\') {
        out.push_back(L'\\');
    }
    out.append(right);
    return out;
}

std::wstring FormatDouble(double value, int digits) {
    wchar_t buffer[64] = {};
    swprintf(buffer, 64, L"%.*f", digits, value);
    return buffer;
}

template <typename T>
void SafeRelease(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

void FreeMediaType(AM_MEDIA_TYPE& mediaType) {
    if (mediaType.cbFormat != 0) {
        CoTaskMemFree(mediaType.pbFormat);
        mediaType.cbFormat = 0;
        mediaType.pbFormat = nullptr;
    }
    if (mediaType.pUnk != nullptr) {
        mediaType.pUnk->Release();
        mediaType.pUnk = nullptr;
    }
}

class AsciiCamNativeApp;

class SampleGrabberCallback final : public ISampleGrabberCB {
public:
    explicit SampleGrabberCallback(AsciiCamNativeApp* owner)
        : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP SampleCB(double, IMediaSample*) override;
    STDMETHODIMP BufferCB(double, BYTE* buffer, LONG bufferLength) override;

private:
    volatile long refCount_ = 1;
    AsciiCamNativeApp* owner_ = nullptr;
};

class AsciiCamNativeApp {
public:
    explicit AsciiCamNativeApp(HINSTANCE instance)
        : instance_(instance),
          moduleDirectory_(ModuleDirectory()),
          settingsPath_(JoinPath(moduleDirectory_, L"asciicam_native.ini")) {
        grayToGlyph_.fill(0);
        LoadSettings();
    }

    bool Create() {
        INITCOMMONCONTROLSEX controls = {};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&controls);

        WNDCLASSEXW previewClass = {};
        previewClass.cbSize = sizeof(previewClass);
        previewClass.lpfnWndProc = PreviewWindowProc;
        previewClass.hInstance = instance_;
        previewClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        previewClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        previewClass.lpszClassName = kPreviewClassName;
        if (!RegisterClassExW(&previewClass)) {
            return false;
        }

        WNDCLASSEXW mainClass = {};
        mainClass.cbSize = sizeof(mainClass);
        mainClass.lpfnWndProc = MainWindowProc;
        mainClass.hInstance = instance_;
        mainClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        mainClass.lpszClassName = kMainClassName;
        if (!RegisterClassExW(&mainClass)) {
            return false;
        }

        window_ = CreateWindowExW(
            0,
            kMainClassName,
            kAppTitle,
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1200,
            760,
            nullptr,
            nullptr,
            instance_,
            this
        );
        return window_ != nullptr;
    }

    int Run() {
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    HWND window_ = nullptr;
    HWND previewWindow_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND startButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND detectButton_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND cameraLabel_ = nullptr;
    HWND cameraEdit_ = nullptr;
    HWND detectedLabel_ = nullptr;
    HWND resolutionLabel_ = nullptr;
    HWND resolutionCombo_ = nullptr;
    HWND colsLabel_ = nullptr;
    HWND colsValue_ = nullptr;
    HWND colsTrack_ = nullptr;
    HWND scaleLabel_ = nullptr;
    HWND scaleValue_ = nullptr;
    HWND scaleTrack_ = nullptr;
    HWND fpsLabel_ = nullptr;
    HWND fpsValue_ = nullptr;
    HWND fpsTrack_ = nullptr;
    HWND boostLabel_ = nullptr;
    HWND boostValue_ = nullptr;
    HWND boostTrack_ = nullptr;
    HWND mirrorCheck_ = nullptr;
    HWND invertCheck_ = nullptr;
    HWND tipsLabel_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::wstring moduleDirectory_;
    std::wstring settingsPath_;
    HFONT uiFont_ = nullptr;
    HFONT asciiFont_ = nullptr;

    int cameraIndex_ = 0;
    int capturePresetIndex_ = 1;
    int asciiCols_ = 110;
    int fpsCap_ = 15;
    int displayScalePercent_ = 200;
    int colorBoostPercent_ = 115;
    bool mirror_ = true;
    bool invert_ = false;
    bool running_ = false;

    int captureWidth_ = 0;
    int captureHeight_ = 0;
    int captureStride_ = 0;
    int captureBitsPerPixel_ = 0;
    bool captureBottomUp_ = true;

    std::mutex frameMutex_;
    FrameBuffer latestFrame_;
    PreviewImage previewImage_;

    IGraphBuilder* graph_ = nullptr;
    ICaptureGraphBuilder2* captureGraphBuilder_ = nullptr;
    IMediaControl* mediaControl_ = nullptr;
    IBaseFilter* sourceFilter_ = nullptr;
    IBaseFilter* sampleGrabberFilter_ = nullptr;
    ISampleGrabber* sampleGrabber_ = nullptr;
    IBaseFilter* nullRenderer_ = nullptr;
    SampleGrabberCallback* sampleGrabberCallback_ = nullptr;

    int cellWidth_ = 0;
    int cellHeight_ = 0;
    std::vector<std::uint8_t> glyphMasks_;
    std::array<std::uint8_t, 256> grayToGlyph_;

    bool hasPresentTime_ = false;
    std::chrono::steady_clock::time_point lastPresentTime_;
    double fpsEma_ = 0.0;

    static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        AsciiCamNativeApp* app = reinterpret_cast<AsciiCamNativeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = reinterpret_cast<AsciiCamNativeApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->window_ = hwnd;
        }
        if (app != nullptr) {
            return app->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK PreviewWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<AsciiCamNativeApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = reinterpret_cast<AsciiCamNativeApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app != nullptr) {
            return app->HandlePreviewMessage(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            if (!CreateUi()) {
                return -1;
            }
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_COMMAND:
            OnCommand(HIWORD(wParam), LOWORD(wParam), reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_HSCROLL:
            OnTrackbarScroll(reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_TIMER:
            if (wParam == kRenderTimerId) {
                RenderLatestFrame();
                return 0;
            }
            break;
        case WM_CLOSE:
            SaveSettings();
            StopCamera();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            if (asciiFont_ != nullptr) {
                DeleteObject(asciiFont_);
                asciiFont_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    LRESULT HandlePreviewMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_PAINT:
            PaintPreview(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool CreateUi() {
        uiFont_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        previewWindow_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            kPreviewClassName,
            L"",
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            100,
            100,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PREVIEW)),
            instance_,
            this
        );

        statusLabel_ = CreateStatic(IDC_STATUS, L"Idle");
        startButton_ = CreateButton(IDC_START, L"Start Camera");
        stopButton_ = CreateButton(IDC_STOP, L"Stop");
        detectButton_ = CreateButton(IDC_DETECT, L"Auto Detect Cameras");
        saveButton_ = CreateButton(IDC_SAVE, L"Save Settings");
        cameraLabel_ = CreateStatic(IDC_CAMERA_LABEL, L"Camera Index");
        cameraEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"0",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            100,
            kEditHeight,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CAMERA_EDIT)),
            instance_,
            nullptr
        );
        detectedLabel_ = CreateStatic(IDC_DETECTED, L"Detected cameras: not scanned");
        resolutionLabel_ = CreateStatic(IDC_RES_LABEL, L"Capture Resolution");
        resolutionCombo_ = CreateWindowExW(
            0,
            WC_COMBOBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0,
            0,
            100,
            kComboHeight,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RES_COMBO)),
            instance_,
            nullptr
        );
        colsLabel_ = CreateStatic(IDC_COLS_LABEL, L"ASCII Width (chars)");
        colsValue_ = CreateStatic(IDC_COLS_VALUE, L"");
        colsTrack_ = CreateTrackbar(IDC_COLS_TRACK, 40, 220, asciiCols_);
        scaleLabel_ = CreateStatic(IDC_SCALE_LABEL, L"Display Scale");
        scaleValue_ = CreateStatic(IDC_SCALE_VALUE, L"");
        scaleTrack_ = CreateTrackbar(IDC_SCALE_TRACK, 25, 800, displayScalePercent_);
        fpsLabel_ = CreateStatic(IDC_FPS_LABEL, L"FPS Cap");
        fpsValue_ = CreateStatic(IDC_FPS_VALUE, L"");
        fpsTrack_ = CreateTrackbar(IDC_FPS_TRACK, 5, 30, fpsCap_);
        boostLabel_ = CreateStatic(IDC_BOOST_LABEL, L"Color Boost");
        boostValue_ = CreateStatic(IDC_BOOST_VALUE, L"");
        boostTrack_ = CreateTrackbar(IDC_BOOST_TRACK, 50, 200, colorBoostPercent_);
        mirrorCheck_ = CreateCheckBox(IDC_MIRROR, L"Mirror Camera");
        invertCheck_ = CreateCheckBox(IDC_INVERT, L"Invert ASCII");
        tipsLabel_ = CreateStatic(
            IDC_TIPS,
            L"Tips:\r\n- Lower width/FPS for smoother performance.\r\n- Increase width for more detail.\r\n- Native build uses DirectShow webcam capture and GDI rendering.\r\n- Virtual camera output is not implemented in this native build."
        );

        if (!previewWindow_ || !statusLabel_ || !startButton_ || !stopButton_ || !detectButton_ || !saveButton_ ||
            !cameraLabel_ || !cameraEdit_ || !detectedLabel_ || !resolutionLabel_ || !resolutionCombo_ ||
            !colsLabel_ || !colsValue_ || !colsTrack_ || !scaleLabel_ || !scaleValue_ || !scaleTrack_ ||
            !fpsLabel_ || !fpsValue_ || !fpsTrack_ || !boostLabel_ || !boostValue_ || !boostTrack_ ||
            !mirrorCheck_ || !invertCheck_ || !tipsLabel_) {
            return false;
        }

        SendMessageW(cameraEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        SendMessageW(resolutionCombo_, CB_RESETCONTENT, 0, 0);
        for (const auto& preset : kCapturePresets) {
            SendMessageW(resolutionCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.label));
        }
        SendMessageW(resolutionCombo_, CB_SETCURSEL, static_cast<WPARAM>(capturePresetIndex_), 0);

        SendMessageW(mirrorCheck_, BM_SETCHECK, mirror_ ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(invertCheck_, BM_SETCHECK, invert_ ? BST_CHECKED : BST_UNCHECKED, 0);

        wchar_t cameraText[16] = {};
        swprintf(cameraText, 16, L"%d", cameraIndex_);
        SetWindowTextW(cameraEdit_, cameraText);

        if (!BuildGlyphMasks()) {
            MessageBoxW(window_, L"Failed to build ASCII glyph masks.", kAppTitle, MB_ICONERROR | MB_OK);
            return false;
        }

        UpdateValueLabels();
        Layout();
        UpdateStatusText(L"Idle");
        return true;
    }

    HWND CreateStatic(int controlId, const wchar_t* text) {
        HWND handle = CreateWindowExW(
            0,
            L"STATIC",
            text,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,
            0,
            100,
            20,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance_,
            nullptr
        );
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        return handle;
    }

    HWND CreateButton(int controlId, const wchar_t* text) {
        HWND handle = CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,
            0,
            100,
            kButtonHeight,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance_,
            nullptr
        );
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        return handle;
    }

    HWND CreateCheckBox(int controlId, const wchar_t* text) {
        HWND handle = CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0,
            0,
            100,
            kButtonHeight,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance_,
            nullptr
        );
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        return handle;
    }

    HWND CreateTrackbar(int controlId, int minimum, int maximum, int value) {
        HWND handle = CreateWindowExW(
            0,
            TRACKBAR_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            0,
            0,
            100,
            kTrackHeight,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance_,
            nullptr
        );
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        SendMessageW(handle, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
        SendMessageW(handle, TBM_SETPOS, TRUE, value);
        return handle;
    }

    void Layout() {
        RECT client = {};
        GetClientRect(window_, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        int panelX = kMargin;
        int panelW = kControlWidth;
        int previewX = kLeftPanelWidth + kMargin;
        int previewW = std::max(100, width - previewX - kMargin);
        int previewH = std::max(100, height - kStatusHeight - (kMargin * 2));

        int y = kMargin;
        MoveWindow(startButton_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 6;
        MoveWindow(stopButton_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 6;
        MoveWindow(detectButton_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 6;
        MoveWindow(saveButton_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 12;

        MoveWindow(cameraLabel_, panelX, y, panelW, kLabelHeight, TRUE);
        y += kLabelHeight + 2;
        MoveWindow(cameraEdit_, panelX, y, 80, kEditHeight, TRUE);
        y += kEditHeight + 4;
        MoveWindow(detectedLabel_, panelX, y, panelW, kDetectedHeight, TRUE);
        y += kDetectedHeight + 8;

        MoveWindow(resolutionLabel_, panelX, y, panelW, kLabelHeight, TRUE);
        y += kLabelHeight + 2;
        MoveWindow(resolutionCombo_, panelX, y, panelW, 220, TRUE);
        y += 30;

        LayoutTrackbar(colsLabel_, colsValue_, colsTrack_, panelX, y, panelW);
        y += kLabelHeight + kTrackHeight + 10;
        LayoutTrackbar(scaleLabel_, scaleValue_, scaleTrack_, panelX, y, panelW);
        y += kLabelHeight + kTrackHeight + 10;
        LayoutTrackbar(fpsLabel_, fpsValue_, fpsTrack_, panelX, y, panelW);
        y += kLabelHeight + kTrackHeight + 10;
        LayoutTrackbar(boostLabel_, boostValue_, boostTrack_, panelX, y, panelW);
        y += kLabelHeight + kTrackHeight + 12;

        MoveWindow(mirrorCheck_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 4;
        MoveWindow(invertCheck_, panelX, y, panelW, kButtonHeight, TRUE);
        y += kButtonHeight + 8;
        MoveWindow(tipsLabel_, panelX, y, panelW, kTipsHeight, TRUE);

        MoveWindow(previewWindow_, previewX, kMargin, previewW, previewH, TRUE);
        MoveWindow(statusLabel_, kMargin, height - kStatusHeight - 4, width - (kMargin * 2), kStatusHeight, TRUE);
    }

    void LayoutTrackbar(HWND label, HWND value, HWND track, int x, int y, int width) {
        int valueWidth = 56;
        MoveWindow(label, x, y, width - valueWidth - 4, kLabelHeight, TRUE);
        MoveWindow(value, x + width - valueWidth, y, valueWidth, kLabelHeight, TRUE);
        MoveWindow(track, x, y + kLabelHeight, width, kTrackHeight, TRUE);
    }

    void OnCommand(int notifyCode, int controlId, HWND control) {
        switch (controlId) {
        case IDC_START:
            if (notifyCode == BN_CLICKED) {
                StartCamera();
            }
            break;
        case IDC_STOP:
            if (notifyCode == BN_CLICKED) {
                StopCamera();
                UpdateStatusText(L"Stopped");
            }
            break;
        case IDC_DETECT:
            if (notifyCode == BN_CLICKED) {
                DetectCameras();
            }
            break;
        case IDC_SAVE:
            if (notifyCode == BN_CLICKED) {
                SyncStateFromControls();
                SaveSettings();
                UpdateStatusText(L"Settings saved");
            }
            break;
        case IDC_RES_COMBO:
            if (notifyCode == CBN_SELCHANGE) {
                capturePresetIndex_ = static_cast<int>(SendMessageW(resolutionCombo_, CB_GETCURSEL, 0, 0));
                if (running_) {
                    StartCamera();
                }
            }
            break;
        case IDC_MIRROR:
            if (notifyCode == BN_CLICKED) {
                mirror_ = SendMessageW(mirrorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
            break;
        case IDC_INVERT:
            if (notifyCode == BN_CLICKED) {
                invert_ = SendMessageW(invertCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            }
            break;
        case IDC_CAMERA_EDIT:
            if (notifyCode == EN_CHANGE) {
                cameraIndex_ = ReadCameraIndexControl();
            }
            break;
        default:
            (void)control;
            break;
        }
    }

    void OnTrackbarScroll(HWND trackbar) {
        if (trackbar == nullptr) {
            return;
        }

        if (trackbar == colsTrack_) {
            asciiCols_ = static_cast<int>(SendMessageW(colsTrack_, TBM_GETPOS, 0, 0));
        } else if (trackbar == scaleTrack_) {
            displayScalePercent_ = static_cast<int>(SendMessageW(scaleTrack_, TBM_GETPOS, 0, 0));
            InvalidateRect(previewWindow_, nullptr, FALSE);
        } else if (trackbar == fpsTrack_) {
            fpsCap_ = static_cast<int>(SendMessageW(fpsTrack_, TBM_GETPOS, 0, 0));
            ResetRenderTimer();
        } else if (trackbar == boostTrack_) {
            colorBoostPercent_ = static_cast<int>(SendMessageW(boostTrack_, TBM_GETPOS, 0, 0));
        }

        UpdateValueLabels();
    }

    void UpdateValueLabels() {
        SetWindowTextW(colsValue_, std::to_wstring(asciiCols_).c_str());
        SetWindowTextW(scaleValue_, FormatDouble(displayScalePercent_ / 100.0, 2).c_str());
        SetWindowTextW(fpsValue_, std::to_wstring(fpsCap_).c_str());
        SetWindowTextW(boostValue_, FormatDouble(colorBoostPercent_ / 100.0, 2).c_str());
    }

    void SyncStateFromControls() {
        cameraIndex_ = ReadCameraIndexControl();
        capturePresetIndex_ = ClampInt(static_cast<int>(SendMessageW(resolutionCombo_, CB_GETCURSEL, 0, 0)), 0, static_cast<int>(std::size(kCapturePresets)) - 1);
        asciiCols_ = static_cast<int>(SendMessageW(colsTrack_, TBM_GETPOS, 0, 0));
        displayScalePercent_ = static_cast<int>(SendMessageW(scaleTrack_, TBM_GETPOS, 0, 0));
        fpsCap_ = static_cast<int>(SendMessageW(fpsTrack_, TBM_GETPOS, 0, 0));
        colorBoostPercent_ = static_cast<int>(SendMessageW(boostTrack_, TBM_GETPOS, 0, 0));
        mirror_ = SendMessageW(mirrorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        invert_ = SendMessageW(invertCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    int ReadCameraIndexControl() {
        wchar_t buffer[16] = {};
        GetWindowTextW(cameraEdit_, buffer, 16);
        int value = _wtoi(buffer);
        return ClampInt(value, 0, 9);
    }

    void UpdateStatusText(const std::wstring& text) {
        SetWindowTextW(statusLabel_, text.c_str());
    }

    bool BuildGlyphMasks() {
        if (asciiFont_ != nullptr) {
            DeleteObject(asciiFont_);
            asciiFont_ = nullptr;
        }

        HDC screen = GetDC(window_);
        if (screen == nullptr) {
            return false;
        }

        asciiFont_ = CreateFontW(
            12,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN,
            L"Consolas"
        );
        if (asciiFont_ == nullptr) {
            ReleaseDC(window_, screen);
            return false;
        }

        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(window_, screen);
        if (dc == nullptr) {
            return false;
        }

        HGDIOBJ oldFont = SelectObject(dc, asciiFont_);
        SIZE sizeM = {};
        SIZE sizeMg = {};
        GetTextExtentPoint32W(dc, L"M", 1, &sizeM);
        GetTextExtentPoint32W(dc, L"Mg", 2, &sizeMg);
        cellWidth_ = std::max(1L, sizeM.cx);
        cellHeight_ = std::max(1L, sizeMg.cy + 2);

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = cellWidth_;
        info.bmiHeader.biHeight = -cellHeight_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) {
            SelectObject(dc, oldFont);
            DeleteDC(dc);
            return false;
        }

        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        int glyphCount = static_cast<int>(wcslen(kAsciiChars));
        glyphMasks_.assign(static_cast<std::size_t>(glyphCount) * static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_), 0);

        int paletteSize = std::max(0, glyphCount - 1);
        for (int value = 0; value < 256; ++value) {
            grayToGlyph_[static_cast<std::size_t>(value)] = static_cast<std::uint8_t>((value * paletteSize) / 255);
        }

        auto* pixelBytes = static_cast<std::uint8_t*>(bits);
        for (int glyphIndex = 0; glyphIndex < glyphCount; ++glyphIndex) {
            std::memset(pixelBytes, 0, static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_) * 4u);
            wchar_t glyph[2] = {kAsciiChars[glyphIndex], L'\0'};
            TextOutW(dc, 0, 0, glyph, 1);
            auto* glyphOut = glyphMasks_.data() + static_cast<std::size_t>(glyphIndex) * static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_);
            for (int y = 0; y < cellHeight_; ++y) {
                for (int x = 0; x < cellWidth_; ++x) {
                    const std::size_t srcOffset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(cellWidth_) + static_cast<std::size_t>(x)) * 4u;
                    glyphOut[static_cast<std::size_t>(y) * static_cast<std::size_t>(cellWidth_) + static_cast<std::size_t>(x)] = pixelBytes[srcOffset];
                }
            }
        }

        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        SelectObject(dc, oldFont);
        DeleteDC(dc);
        return true;
    }

    void LoadSettings() {
        cameraIndex_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"CameraIndex", cameraIndex_, settingsPath_.c_str()), 0, 9);
        capturePresetIndex_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"CapturePresetIndex", capturePresetIndex_, settingsPath_.c_str()), 0, static_cast<int>(std::size(kCapturePresets)) - 1);
        asciiCols_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"AsciiCols", asciiCols_, settingsPath_.c_str()), 40, 220);
        fpsCap_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"FpsCap", fpsCap_, settingsPath_.c_str()), 5, 30);
        displayScalePercent_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"DisplayScalePercent", displayScalePercent_, settingsPath_.c_str()), 25, 800);
        colorBoostPercent_ = ClampInt(GetPrivateProfileIntW(L"Settings", L"ColorBoostPercent", colorBoostPercent_, settingsPath_.c_str()), 50, 200);
        mirror_ = GetPrivateProfileIntW(L"Settings", L"Mirror", mirror_ ? 1 : 0, settingsPath_.c_str()) != 0;
        invert_ = GetPrivateProfileIntW(L"Settings", L"Invert", invert_ ? 1 : 0, settingsPath_.c_str()) != 0;
    }

    void SaveSettings() {
        SyncStateFromControls();

        WritePrivateProfileStringW(L"Settings", L"CameraIndex", std::to_wstring(cameraIndex_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"CapturePresetIndex", std::to_wstring(capturePresetIndex_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"AsciiCols", std::to_wstring(asciiCols_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"FpsCap", std::to_wstring(fpsCap_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"DisplayScalePercent", std::to_wstring(displayScalePercent_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"ColorBoostPercent", std::to_wstring(colorBoostPercent_).c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"Mirror", mirror_ ? L"1" : L"0", settingsPath_.c_str());
        WritePrivateProfileStringW(L"Settings", L"Invert", invert_ ? L"1" : L"0", settingsPath_.c_str());
    }

    void DetectCameras() {
        std::vector<int> found;
        for (int index = 0; index <= 9; ++index) {
            if (ProbeCameraIndex(index)) {
                found.push_back(index);
            }
        }

        std::wstring text = L"Detected cameras: ";
        if (found.empty()) {
            text += L"none found (0-9)";
            UpdateStatusText(L"No cameras detected in indexes 0-9");
        } else {
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (i > 0) {
                    text += L", ";
                }
                text += std::to_wstring(found[i]);
            }
            cameraIndex_ = found.front();
            wchar_t cameraText[16] = {};
            swprintf(cameraText, 16, L"%d", cameraIndex_);
            SetWindowTextW(cameraEdit_, cameraText);
            UpdateStatusText(L"Detected camera indexes");
        }
        SetWindowTextW(detectedLabel_, text.c_str());
    }

    bool ProbeCameraIndex(int cameraIndex) {
        IBaseFilter* filter = nullptr;
        const bool found = CreateVideoCaptureFilterByIndex(cameraIndex, &filter);
        SafeRelease(filter);
        return found;
    }

    bool CreateVideoCaptureFilterByIndex(int targetIndex, IBaseFilter** outFilter) {
        if (outFilter == nullptr) {
            return false;
        }
        *outFilter = nullptr;

        ICreateDevEnum* deviceEnum = nullptr;
        IEnumMoniker* enumMoniker = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnum));
        if (FAILED(hr)) {
            return false;
        }

        hr = deviceEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        SafeRelease(deviceEnum);
        if (hr != S_OK || enumMoniker == nullptr) {
            SafeRelease(enumMoniker);
            return false;
        }

        bool found = false;
        int index = 0;
        IMoniker* moniker = nullptr;
        while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
            if (index == targetIndex) {
                hr = moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(outFilter));
                found = SUCCEEDED(hr) && *outFilter != nullptr;
                moniker->Release();
                break;
            }
            moniker->Release();
            moniker = nullptr;
            ++index;
        }

        SafeRelease(enumMoniker);
        return found;
    }

    std::vector<std::wstring> EnumerateCameraNames() {
        std::vector<std::wstring> names;
        ICreateDevEnum* deviceEnum = nullptr;
        IEnumMoniker* enumMoniker = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnum));
        if (FAILED(hr)) {
            return names;
        }

        hr = deviceEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        SafeRelease(deviceEnum);
        if (hr != S_OK || enumMoniker == nullptr) {
            SafeRelease(enumMoniker);
            return names;
        }

        IMoniker* moniker = nullptr;
        while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
            IPropertyBag* propertyBag = nullptr;
            hr = moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void**>(&propertyBag));
            if (SUCCEEDED(hr) && propertyBag != nullptr) {
                VARIANT value;
                VariantInit(&value);
                hr = propertyBag->Read(L"FriendlyName", &value, nullptr);
                if (SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal != nullptr) {
                    names.emplace_back(value.bstrVal);
                } else {
                    names.emplace_back(L"(unnamed camera)");
                }
                VariantClear(&value);
            } else {
                names.emplace_back(L"(unnamed camera)");
            }
            SafeRelease(propertyBag);
            moniker->Release();
            moniker = nullptr;
        }

        SafeRelease(enumMoniker);
        return names;
    }

    bool StartCamera() {
        SyncStateFromControls();
        StopCamera();
        const CapturePreset& preset = kCapturePresets[capturePresetIndex_];

        latestFrame_ = {};
        previewImage_ = {};
        fpsEma_ = 0.0;
        hasPresentTime_ = false;

        HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to create the DirectShow filter graph.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&captureGraphBuilder_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to create the DirectShow capture graph builder.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = captureGraphBuilder_->SetFiltergraph(graph_);
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to attach the capture graph builder.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        if (!CreateVideoCaptureFilterByIndex(cameraIndex_, &sourceFilter_)) {
            std::wstring message = L"Could not open webcam at index " + std::to_wstring(cameraIndex_) + L".";
            MessageBoxW(window_, message.c_str(), kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = graph_->AddFilter(sourceFilter_, L"Video Capture");
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to add the camera source filter.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sampleGrabberFilter_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to create the DirectShow sample grabber.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }
        hr = graph_->AddFilter(sampleGrabberFilter_, L"Sample Grabber");
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to add the sample grabber filter.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = sampleGrabberFilter_->QueryInterface(IID_PPV_ARGS(&sampleGrabber_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to query the sample grabber interface.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        AM_MEDIA_TYPE requestedType = {};
        requestedType.majortype = MEDIATYPE_Video;
        requestedType.subtype = MEDIASUBTYPE_RGB24;
        requestedType.formattype = FORMAT_VideoInfo;
        sampleGrabber_->SetOneShot(FALSE);
        sampleGrabber_->SetBufferSamples(FALSE);
        hr = sampleGrabber_->SetMediaType(&requestedType);
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to configure the sample grabber media type.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        sampleGrabberCallback_ = new SampleGrabberCallback(this);
        hr = sampleGrabber_->SetCallback(sampleGrabberCallback_, 1);
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to install the sample grabber callback.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to create the DirectShow null renderer.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }
        hr = graph_->AddFilter(nullRenderer_, L"Null Renderer");
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to add the null renderer filter.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = captureGraphBuilder_->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, sourceFilter_, sampleGrabberFilter_, nullRenderer_);
        if (FAILED(hr)) {
            hr = captureGraphBuilder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter_, sampleGrabberFilter_, nullRenderer_);
        }
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to connect the DirectShow capture graph.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        AM_MEDIA_TYPE connectedType = {};
        hr = sampleGrabber_->GetConnectedMediaType(&connectedType);
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to read the connected media type.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        auto* videoInfo = reinterpret_cast<VIDEOINFOHEADER*>(connectedType.pbFormat);
        if (videoInfo == nullptr) {
            FreeMediaType(connectedType);
            MessageBoxW(window_, L"Connected media type did not include video format data.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        captureWidth_ = std::abs(videoInfo->bmiHeader.biWidth);
        captureHeight_ = std::abs(videoInfo->bmiHeader.biHeight);
        captureBitsPerPixel_ = videoInfo->bmiHeader.biBitCount;
        captureBottomUp_ = videoInfo->bmiHeader.biHeight > 0;
        captureStride_ = ((captureWidth_ * captureBitsPerPixel_ + 31) / 32) * 4;
        FreeMediaType(connectedType);

        if (captureWidth_ <= 0 || captureHeight_ <= 0 || captureBitsPerPixel_ != 24) {
            MessageBoxW(window_, L"DirectShow did not provide 24-bit RGB frames from this camera.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = graph_->QueryInterface(IID_PPV_ARGS(&mediaControl_));
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to acquire the DirectShow media control.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        hr = mediaControl_->Run();
        if (FAILED(hr)) {
            MessageBoxW(window_, L"Failed to start the DirectShow capture graph.", kAppTitle, MB_ICONERROR | MB_OK);
            StopCamera();
            return false;
        }

        running_ = true;
        ResetRenderTimer();

        wchar_t status[256] = {};
        swprintf(
            status,
            256,
            L"Camera %d started | requested %dx%d, actual %dx%d",
            cameraIndex_,
            preset.width,
            preset.height,
            captureWidth_,
            captureHeight_
        );
        UpdateStatusText(status);
        return true;
    }

    void StopCamera() {
        KillTimer(window_, kRenderTimerId);
        running_ = false;
        if (mediaControl_ != nullptr) {
            mediaControl_->Stop();
        }
        if (sampleGrabber_ != nullptr) {
            sampleGrabber_->SetCallback(nullptr, 1);
        }
        if (sampleGrabberCallback_ != nullptr) {
            sampleGrabberCallback_->Release();
            sampleGrabberCallback_ = nullptr;
        }
        SafeRelease(mediaControl_);
        SafeRelease(sampleGrabber_);
        SafeRelease(nullRenderer_);
        SafeRelease(sampleGrabberFilter_);
        SafeRelease(sourceFilter_);
        SafeRelease(captureGraphBuilder_);
        SafeRelease(graph_);
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            latestFrame_ = {};
        }
        previewImage_ = {};
        InvalidateRect(previewWindow_, nullptr, FALSE);
    }

    void ResetRenderTimer() {
        KillTimer(window_, kRenderTimerId);
        if (running_) {
            UINT interval = static_cast<UINT>(std::max(1, 1000 / std::max(1, fpsCap_)));
            SetTimer(window_, kRenderTimerId, interval, nullptr);
        }
    }

public:
    void OnDirectShowFrame(const BYTE* buffer, LONG bufferLength) {
        const int bytesPerPixel = captureBitsPerPixel_ / 8;
        const std::size_t expectedSize = static_cast<std::size_t>(captureStride_) * static_cast<std::size_t>(captureHeight_);
        if (buffer == nullptr || bufferLength <= 0 || bytesPerPixel <= 0 || static_cast<std::size_t>(bufferLength) < expectedSize) {
            return;
        }

        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_.bytes.assign(buffer, buffer + expectedSize);
        latestFrame_.width = captureWidth_;
        latestFrame_.height = captureHeight_;
        latestFrame_.stride = captureStride_;
        latestFrame_.bytesPerPixel = bytesPerPixel;
        latestFrame_.bottomUp = captureBottomUp_;
        latestFrame_.valid = true;
    }

private:

    void RenderLatestFrame() {
        FrameBuffer frame;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            if (!latestFrame_.valid || latestFrame_.bytes.empty()) {
                UpdateStatusText(L"Running | waiting for camera frames...");
                return;
            }
            frame = latestFrame_;
        }

        auto start = std::chrono::steady_clock::now();
        const int cols = std::max(8, asciiCols_);
        const int rows = std::max(4, static_cast<int>((static_cast<double>(frame.height) / static_cast<double>(frame.width)) * cols * (static_cast<double>(cellWidth_) / static_cast<double>(cellHeight_))));
        const int outWidth = cols * cellWidth_;
        const int outHeight = rows * cellHeight_;

        std::vector<std::uint8_t> rendered(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4u, 0);
        const double boost = colorBoostPercent_ / 100.0;
        const int glyphCount = static_cast<int>(wcslen(kAsciiChars));

        for (int row = 0; row < rows; ++row) {
            int srcY0 = (row * frame.height) / rows;
            int srcY1 = ((row + 1) * frame.height) / rows;
            if (srcY1 <= srcY0) {
                srcY1 = std::min(frame.height, srcY0 + 1);
            }

            for (int col = 0; col < cols; ++col) {
                int srcX0 = (col * frame.width) / cols;
                int srcX1 = ((col + 1) * frame.width) / cols;
                if (srcX1 <= srcX0) {
                    srcX1 = std::min(frame.width, srcX0 + 1);
                }

                std::uint64_t sumB = 0;
                std::uint64_t sumG = 0;
                std::uint64_t sumR = 0;
                std::uint64_t count = 0;

                for (int sy = srcY0; sy < srcY1; ++sy) {
                    int sourceY = frame.bottomUp ? (frame.height - 1 - sy) : sy;
                    const auto* rowPtr = frame.bytes.data() + static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(frame.stride);
                    for (int sx = srcX0; sx < srcX1; ++sx) {
                        int sourceX = mirror_ ? (frame.width - 1 - sx) : sx;
                        const auto* pixel = rowPtr + static_cast<std::size_t>(sourceX) * static_cast<std::size_t>(frame.bytesPerPixel);
                        sumB += pixel[0];
                        sumG += pixel[1];
                        sumR += pixel[2];
                        ++count;
                    }
                }

                if (count == 0) {
                    continue;
                }

                int b = ClampInt(static_cast<int>(std::round((static_cast<double>(sumB) / count) * boost)), 0, 255);
                int g = ClampInt(static_cast<int>(std::round((static_cast<double>(sumG) / count) * boost)), 0, 255);
                int r = ClampInt(static_cast<int>(std::round((static_cast<double>(sumR) / count) * boost)), 0, 255);

                int gray = ((77 * r) + (150 * g) + (29 * b)) >> 8;
                int glyphIndex = grayToGlyph_[static_cast<std::size_t>(ClampInt(gray, 0, 255))];
                if (invert_) {
                    glyphIndex = glyphCount - 1 - glyphIndex;
                }

                const auto* glyphMask = glyphMasks_.data() + static_cast<std::size_t>(glyphIndex) * static_cast<std::size_t>(cellWidth_) * static_cast<std::size_t>(cellHeight_);
                for (int gy = 0; gy < cellHeight_; ++gy) {
                    const std::size_t outRow = (static_cast<std::size_t>(row) * static_cast<std::size_t>(cellHeight_) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(outWidth);
                    for (int gx = 0; gx < cellWidth_; ++gx) {
                        const std::uint8_t alpha = glyphMask[static_cast<std::size_t>(gy) * static_cast<std::size_t>(cellWidth_) + static_cast<std::size_t>(gx)];
                        if (alpha == 0) {
                            continue;
                        }
                        const std::size_t outOffset = (outRow + static_cast<std::size_t>(col) * static_cast<std::size_t>(cellWidth_) + static_cast<std::size_t>(gx)) * 4u;
                        rendered[outOffset + 0u] = static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(b) + 127u) / 255u);
                        rendered[outOffset + 1u] = static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(g) + 127u) / 255u);
                        rendered[outOffset + 2u] = static_cast<std::uint8_t>((static_cast<unsigned int>(alpha) * static_cast<unsigned int>(r) + 127u) / 255u);
                        rendered[outOffset + 3u] = 0u;
                    }
                }
            }
        }

        previewImage_.bgra.swap(rendered);
        previewImage_.width = outWidth;
        previewImage_.height = outHeight;
        previewImage_.valid = true;
        InvalidateRect(previewWindow_, nullptr, FALSE);

        auto now = std::chrono::steady_clock::now();
        if (hasPresentTime_) {
            const std::chrono::duration<double> dt = now - lastPresentTime_;
            if (dt.count() > 0.0) {
                const double instantFps = 1.0 / dt.count();
                fpsEma_ = (fpsEma_ == 0.0) ? instantFps : (fpsEma_ * 0.85 + instantFps * 0.15);
            }
        }
        lastPresentTime_ = now;
        hasPresentTime_ = true;

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        wchar_t status[256] = {};
        swprintf(
            status,
            256,
            L"Running | %dx%d chars | render %lld ms | actual fps %.1f (cap %d) | renderer native-cpp | vcam off",
            cols,
            rows,
            static_cast<long long>(elapsed),
            fpsEma_,
            fpsCap_
        );
        UpdateStatusText(status);
    }

    void PaintPreview(HWND hwnd) {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);

        RECT client = {};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        if (!previewImage_.valid || previewImage_.bgra.empty() || previewImage_.width <= 0 || previewImage_.height <= 0) {
            SetTextColor(dc, RGB(220, 220, 220));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, running_ ? L"Waiting for frames..." : L"Click Start Camera", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &paint);
            return;
        }

        const double scale = std::max(0.25, displayScalePercent_ / 100.0);
        const int destWidth = std::max(1, static_cast<int>(std::lround(previewImage_.width * scale)));
        const int destHeight = std::max(1, static_cast<int>(std::lround(previewImage_.height * scale)));
        const int destX = (client.right - client.left - destWidth) / 2;
        const int destY = (client.bottom - client.top - destHeight) / 2;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = previewImage_.width;
        info.bmiHeader.biHeight = -previewImage_.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(
            dc,
            destX,
            destY,
            destWidth,
            destHeight,
            0,
            0,
            previewImage_.width,
            previewImage_.height,
            previewImage_.bgra.data(),
            &info,
            DIB_RGB_COLORS,
            SRCCOPY
        );

        EndPaint(hwnd, &paint);
    }
};

STDMETHODIMP SampleGrabberCallback::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
        *object = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SampleGrabberCallback::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

STDMETHODIMP_(ULONG) SampleGrabberCallback::Release() {
    const ULONG count = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP SampleGrabberCallback::SampleCB(double, IMediaSample*) {
    return E_NOTIMPL;
}

STDMETHODIMP SampleGrabberCallback::BufferCB(double, BYTE* buffer, LONG bufferLength) {
    if (owner_ != nullptr) {
        owner_->OnDirectShowFrame(buffer, bufferLength);
    }
    return S_OK;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"CoInitializeEx failed for the native camera app.", kAppTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    AsciiCamNativeApp app(instance);
    if (!app.Create()) {
        MessageBoxW(nullptr, L"Failed to create ASCIICAM native window.", kAppTitle, MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    (void)showCommand;
    const int exitCode = app.Run();
    CoUninitialize();
    return exitCode;
}
