// Audible Recorder
// Captures audio from a single process tree (found via its window title) using
// WASAPI process loopback and writes it as Ogg Opus. Win32 UI, no external DLLs.
//
// Requires Windows 10 version 2004 (build 19041) or newer.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "opus.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// ---------------------------------------------------------------------------
// Minimal COM definitions (so we do not depend on SDK version / import libs)
// ---------------------------------------------------------------------------
static const GUID kIID_IUnknown            = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_IAgileObject        = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const GUID kIID_ActivateCompletion  = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const GUID kIID_IAudioClient        = {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioSessionManager2 = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID kIID_IAudioSessionControl2 = {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};
static const GUID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48a0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

struct IActAsyncOp : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT* activateResult, IUnknown** activatedInterface) = 0;
};
struct IActHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ActivateCompleted(IActAsyncOp* op) = 0;
};

struct LoopbackActivationParams {   // layout of AUDIOCLIENT_ACTIVATION_PARAMS
    int   activationType;           // 1 = PROCESS_LOOPBACK
    DWORD targetProcessId;
    int   loopbackMode;             // 0 = INCLUDE_TARGET_PROCESS_TREE
};

typedef HRESULT(WINAPI* PFN_ActivateAudioInterfaceAsync)(LPCWSTR, REFIID, PROPVARIANT*, IActHandler*, IActAsyncOp**);

class ActivationHandler final : public IActHandler {
public:
    ActivationHandler() : m_ref(1), m_hr(E_FAIL), m_client(nullptr) { m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_ActivateCompletion) || IsEqualGUID(riid, kIID_IAgileObject)) {
            *ppv = static_cast<IActHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActAsyncOp* op) override {
        HRESULT act = E_FAIL;
        IUnknown* unk = nullptr;
        HRESULT h = op->GetActivateResult(&act, &unk);
        if (SUCCEEDED(h) && SUCCEEDED(act) && unk) {
            unk->QueryInterface(kIID_IAudioClient, (void**)&m_client);
            unk->Release();
            m_hr = m_client ? S_OK : E_NOINTERFACE;
        } else {
            m_hr = FAILED(h) ? h : act;
        }
        SetEvent(m_event);
        return S_OK;
    }
    HRESULT Wait(DWORD ms, IAudioClient** out) {
        if (WaitForSingleObject(m_event, ms) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        *out = m_client;
        return m_hr;
    }
private:
    ~ActivationHandler() { CloseHandle(m_event); }
    LONG m_ref;
    HRESULT m_hr;
    IAudioClient* m_client;
    HANDLE m_event;
};

// ---------------------------------------------------------------------------
// Ogg Opus writer
// ---------------------------------------------------------------------------
struct OggWriter {
    FILE* f = nullptr;
    uint32_t serial = 0, seq = 0;
    std::vector<uint8_t> lace, body;
    uint32_t crcT[256];
    int64_t encoded = 0;   // frames (48 kHz) of audio packets added
    int preskip = 0;
    int pkts = 0;

    void init(FILE* fp, uint32_t ser, int pre) {
        f = fp; serial = ser; preskip = pre;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
            crcT[i] = r;
        }
    }
    uint32_t crc(const uint8_t* p, size_t n, uint32_t c) const {
        for (size_t i = 0; i < n; i++) c = (c << 8) ^ crcT[((c >> 24) & 0xFF) ^ p[i]];
        return c;
    }
    void pushPacket(const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n / 255; i++) lace.push_back(255);
        lace.push_back((uint8_t)(n % 255));
        body.insert(body.end(), d, d + n);
    }
    void page(uint8_t type, int64_t gp) {
        std::vector<uint8_t> h(27 + lace.size());
        memcpy(h.data(), "OggS", 4);
        h[4] = 0; h[5] = type;
        for (int i = 0; i < 8; i++) h[6 + i] = (uint8_t)((gp >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; i++) h[14 + i] = (uint8_t)((serial >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; i++) h[18 + i] = (uint8_t)((seq >> (8 * i)) & 0xFF);
        h[22] = h[23] = h[24] = h[25] = 0;
        h[26] = (uint8_t)lace.size();
        if (!lace.empty()) memcpy(&h[27], lace.data(), lace.size());
        uint32_t c = crc(h.data(), h.size(), 0);
        c = crc(body.data(), body.size(), c);
        for (int i = 0; i < 4; i++) h[22 + i] = (uint8_t)((c >> (8 * i)) & 0xFF);
        fwrite(h.data(), 1, h.size(), f);
        if (!body.empty()) fwrite(body.data(), 1, body.size(), f);
        fflush(f);
        seq++; lace.clear(); body.clear(); pkts = 0;
    }
    void writeHeaders(int channels, int inputRate) {
        uint8_t head[19];
        memcpy(head, "OpusHead", 8);
        head[8] = 1; head[9] = (uint8_t)channels;
        head[10] = (uint8_t)(preskip & 0xFF); head[11] = (uint8_t)((preskip >> 8) & 0xFF);
        for (int i = 0; i < 4; i++) head[12 + i] = (uint8_t)((inputRate >> (8 * i)) & 0xFF);
        head[16] = head[17] = 0; head[18] = 0;
        pushPacket(head, sizeof head);
        page(2 /*BOS*/, 0);

        const char* vendor = "Audible Recorder";
        uint32_t vl = (uint32_t)strlen(vendor);
        std::vector<uint8_t> tags;
        tags.insert(tags.end(), (const uint8_t*)"OpusTags", (const uint8_t*)"OpusTags" + 8);
        for (int i = 0; i < 4; i++) tags.push_back((uint8_t)((vl >> (8 * i)) & 0xFF));
        tags.insert(tags.end(), (const uint8_t*)vendor, (const uint8_t*)vendor + vl);
        for (int i = 0; i < 4; i++) tags.push_back(0);   // zero user comments
        pushPacket(tags.data(), tags.size());
        page(0, 0);
    }
    void flush(bool eos, int64_t gp) {
        if (lace.empty() && !eos) return;
        page(eos ? 4 : 0, gp);
    }
    void addAudio(const uint8_t* d, size_t n) {
        if (lace.size() + n / 255 + 1 > 255) flush(false, preskip + encoded);
        pushPacket(d, n);
        encoded += 960;
        if (++pkts >= 25) flush(false, preskip + encoded);
    }
};

// ---------------------------------------------------------------------------
// Recorder thread
// ---------------------------------------------------------------------------
#define WM_APP_STARTED (WM_APP + 1)
#define WM_APP_DONE    (WM_APP + 2)

struct RecState {
    DWORD pid = 0;
    wchar_t path[1024] = {};
    int bitrate = 96;
    HWND hwnd = nullptr;
    LONG session = 0;
    volatile LONG stop = 0;
    volatile LONG64 frames = 0;
    HANDLE thread = nullptr;
};
static RecState g_rec;
static wchar_t g_err[512];
static bool g_recording = false;
static LONG g_session = 0;

static void SetErr(const wchar_t* what, HRESULT hr) {
    wsprintfW(g_err, L"%s (0x%08X)", what, (unsigned)hr);
}

static DWORD WINAPI RecThread(LPVOID) {
    g_err[0] = 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    OpusEncoder* enc = nullptr;
    FILE* fp = nullptr;
    HANDLE evt = nullptr, proc = nullptr;
    OggWriter ogg;
    bool started = false;

    do {
        // --- activate process loopback client -----------------------------
        HMODULE mm = LoadLibraryW(L"mmdevapi.dll");
        PFN_ActivateAudioInterfaceAsync act = mm ? (PFN_ActivateAudioInterfaceAsync)GetProcAddress(mm, "ActivateAudioInterfaceAsync") : nullptr;
        if (!act) { SetErr(L"ActivateAudioInterfaceAsync not available (needs Windows 10 2004+)", E_NOTIMPL); break; }

        LoopbackActivationParams lp = {1, g_rec.pid, 0};
        PROPVARIANT pv;
        memset(&pv, 0, sizeof pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof lp;
        pv.blob.pBlobData = (BYTE*)&lp;

        ActivationHandler* handler = new ActivationHandler();
        IActAsyncOp* op = nullptr;
        HRESULT hr = act(L"VAD\\Process_Loopback", kIID_IAudioClient, &pv, handler, &op);
        if (SUCCEEDED(hr)) hr = handler->Wait(5000, &client);
        if (op) op->Release();
        handler->Release();
        if (FAILED(hr) || !client) { SetErr(L"Could not attach to the process's audio (needs Windows 10 2004+)", hr); break; }

        // --- initialise: 48 kHz, 16-bit stereo, event driven ---------------
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof wf);
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = 48000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = 48000 * 4;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                1000000, 0, &wf, nullptr);
        if (FAILED(hr)) { SetErr(L"Audio client initialisation failed", hr); break; }

        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(evt);
        if (FAILED(hr)) { SetErr(L"SetEventHandle failed", hr); break; }
        hr = client->GetService(kIID_IAudioCaptureClient, (void**)&cap);
        if (FAILED(hr)) { SetErr(L"GetService(IAudioCaptureClient) failed", hr); break; }

        // --- encoder + file -------------------------------------------------
        int err = 0;
        enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
        if (!enc || err != OPUS_OK) { SetErr(L"Opus encoder creation failed", (HRESULT)err); break; }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(g_rec.bitrate * 1000));
        opus_encoder_ctl(enc, OPUS_SET_VBR(1));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
        opus_int32 lookahead = 0;
        opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

        fp = _wfopen(g_rec.path, L"wb");
        if (!fp) { SetErr(L"Could not create the output file", HRESULT_FROM_WIN32(GetLastError())); break; }
        ogg.init(fp, (uint32_t)GetTickCount() ^ (uint32_t)(uintptr_t)fp ^ 0x4F707573u, lookahead);
        ogg.writeHeaders(2, 48000);

        proc = OpenProcess(SYNCHRONIZE, FALSE, g_rec.pid);

        hr = client->Start();
        if (FAILED(hr)) { SetErr(L"Start failed", hr); break; }
        started = true;
        PostMessageW(g_rec.hwnd, WM_APP_STARTED, 0, g_rec.session);

        // --- capture loop -----------------------------------------------------
        std::vector<int16_t> pcm;
        pcm.reserve(48000 * 2);
        unsigned char out[4000];
        DWORD lastAliveCheck = GetTickCount();
        bool failed = false;

        while (!g_rec.stop && !failed) {
            WaitForSingleObject(evt, 100);
            if (g_rec.stop) break;

            for (;;) {
                UINT32 avail = 0;
                hr = cap->GetNextPacketSize(&avail);
                if (FAILED(hr)) { SetErr(L"Capture failed (audio device changed?)", hr); failed = true; break; }
                if (avail == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) { SetErr(L"GetBuffer failed", hr); failed = true; break; }
                size_t old = pcm.size();
                pcm.resize(old + (size_t)frames * 2);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) memset(&pcm[old], 0, (size_t)frames * 4);
                else memcpy(&pcm[old], data, (size_t)frames * 4);
                cap->ReleaseBuffer(frames);
                InterlockedAdd64(&g_rec.frames, frames);
            }

            size_t off = 0;
            while (pcm.size() - off >= 960 * 2) {
                int n = opus_encode(enc, &pcm[off], 960, out, sizeof out);
                if (n < 0) { SetErr(L"Opus encode error", (HRESULT)n); failed = true; break; }
                if (n > 0) ogg.addAudio(out, (size_t)n);
                off += 960 * 2;
            }
            if (off) pcm.erase(pcm.begin(), pcm.begin() + off);

            if (GetTickCount() - lastAliveCheck > 1000) {
                lastAliveCheck = GetTickCount();
                if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
                    wsprintfW(g_err, L"The target process exited - recording stopped and saved.");
                    break;
                }
            }
        }

        // --- finish: pad last frame and write EOS page ---------------------
        if (!pcm.empty()) {
            // (drain any leftover partial frame)
            size_t have = pcm.size() / 2;   // frames
            if (have > 0 && have < 960) {
                pcm.resize(960 * 2, 0);
                int n = opus_encode(enc, pcm.data(), 960, out, sizeof out);
                if (n > 0) ogg.addAudio(out, (size_t)n);
            }
        }
        ogg.flush(true, (int64_t)lookahead + (int64_t)g_rec.frames);
    } while (0);

    if (client && started) client->Stop();
    if (cap) cap->Release();
    if (client) client->Release();
    if (enc) opus_encoder_destroy(enc);
    if (fp) fclose(fp);
    if (proc) CloseHandle(proc);
    if (evt) CloseHandle(evt);
    CoUninitialize();

    PostMessageW(g_rec.hwnd, WM_APP_DONE, 0, g_rec.session);
    return 0;
}

// ---------------------------------------------------------------------------
// Target discovery: windows matching the title + processes playing audio
// ---------------------------------------------------------------------------
struct Candidate {
    DWORD pid;
    std::wstring label;
};
static std::vector<Candidate> g_cands;

static std::wstring ExeName(DWORD pid) {
    std::wstring r = L"?";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return r;
    wchar_t b[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, b, &n)) {
        const wchar_t* sl = wcsrchr(b, L'\\');
        r = sl ? sl + 1 : b;
    }
    CloseHandle(h);
    return r;
}

struct UStr { USHORT Length, MaximumLength; PWSTR Buffer; };

// True if the process was started with our separate-profile switch (browser or one of its children).
static bool IsIsolated(DWORD pid) {
    typedef LONG(NTAPI * PFN_NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_NtQIP f = nt ? (PFN_NtQIP)GetProcAddress(nt, "NtQueryInformationProcess") : nullptr;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool r = false;
    if (f && h) {
        static BYTE buf[8192];
        ULONG ret = 0;
        if (f(h, 60 /*ProcessCommandLineInformation*/, buf, sizeof buf, &ret) >= 0) {
            UStr* u = (UStr*)buf;
            if (u->Buffer && u->Length) {
                std::wstring cl(u->Buffer, u->Length / sizeof(wchar_t));
                r = cl.find(L"AudibleRecorder\\BrowserProfile") != std::wstring::npos;
            }
        }
    }
    if (h) CloseHandle(h);
    return r;
}

static bool IsBrowserExe(const std::wstring& exe) {
    wchar_t b[MAX_PATH];
    lstrcpynW(b, exe.c_str(), MAX_PATH);
    CharLowerBuffW(b, (DWORD)lstrlenW(b));
    const wchar_t* names[] = {L"chrome.exe", L"msedge.exe", L"brave.exe", L"firefox.exe", L"opera.exe", L"vivaldi.exe"};
    for (int i = 0; i < 6; i++)
        if (wcscmp(b, names[i]) == 0) return true;
    return false;
}

static void AddCandidate(DWORD pid, const wchar_t* kind, const wchar_t* extra) {
    wchar_t buf[700];
    wsprintfW(buf, L"%s%s  %s  (PID %lu)  %s", IsIsolated(pid) ? L"Isolated - safe  |  " : L"", kind,
              ExeName(pid).c_str(), (unsigned long)pid, extra);
    g_cands.push_back({pid, buf});
}

struct EnumCtx {
    wchar_t needle[256];
    HWND self;
};

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    EnumCtx* c = (EnumCtx*)lp;
    if (h == c->self || !IsWindowVisible(h)) return TRUE;
    wchar_t t[512], low[512];
    int n = GetWindowTextW(h, t, 512);
    if (n <= 0) return TRUE;
    lstrcpynW(low, t, 512);
    CharLowerBuffW(low, (DWORD)n);
    if (!wcsstr(low, c->needle)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;
    wchar_t extra[540];
    wsprintfW(extra, L"\"%s\"", t);
    AddCandidate(pid, L"[Window]", extra);
    return TRUE;
}

static bool HasPid(DWORD pid) {
    for (size_t i = 0; i < g_cands.size(); i++)
        if (g_cands[i].pid == pid) return true;
    return false;
}

// Lists processes that currently have an ACTIVE audio session on the default output device.
static void AddAudioSessions() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* ses = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en))) return;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&ses))) {
        int count = 0;
        ses->GetCount(&count);
        for (int i = 0; i < count; i++) {
            IAudioSessionControl* ctl = nullptr;
            if (FAILED(ses->GetSession(i, &ctl)) || !ctl) continue;
            AudioSessionState st = AudioSessionStateInactive;
            ctl->GetState(&st);
            IAudioSessionControl2* ctl2 = nullptr;
            if (st == AudioSessionStateActive && SUCCEEDED(ctl->QueryInterface(kIID_IAudioSessionControl2, (void**)&ctl2)) && ctl2) {
                DWORD pid = 0;
                if (ctl2->IsSystemSoundsSession() != S_OK && SUCCEEDED(ctl2->GetProcessId(&pid)) && pid && !HasPid(pid))
                    AddCandidate(pid, L"[Playing audio]", L"");
                ctl2->Release();
            }
            ctl->Release();
        }
    }
    if (ses) ses->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    en->Release();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
enum { ID_TITLE = 101, ID_PATH, ID_BROWSE, ID_BITRATE, ID_REC, ID_STATUS, ID_TIME, ID_REFRESH, ID_TARGET, ID_LAUNCH };

static HWND g_hTitle, g_hPath, g_hBrowse, g_hBitrate, g_hRec, g_hStatus, g_hTime, g_hRefresh, g_hTarget, g_hLaunch;
static HFONT g_font;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static void Say(const wchar_t* s) { SetWindowTextW(g_hStatus, s); }

static void DefaultPath(wchar_t* out, size_t cch) {
    wchar_t prof[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (n > 0 && n < MAX_PATH)
        wsprintfW(out, L"%s\\Music\\AudibleRecording-%04d%02d%02d-%02d%02d%02d.opus", prof, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else
        wsprintfW(out, L"AudibleRecording-%04d%02d%02d-%02d%02d%02d.opus", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    (void)cch;
}

static void SetEditable(bool on) {
    EnableWindow(g_hTitle, on);
    EnableWindow(g_hPath, on);
    EnableWindow(g_hBrowse, on);
    EnableWindow(g_hBitrate, on);
    EnableWindow(g_hRefresh, on);
    EnableWindow(g_hTarget, on);
    EnableWindow(g_hLaunch, on);
}

static void RefreshTargets(HWND hwnd) {
    DWORD prev = 0;
    int sel = (int)SendMessageW(g_hTarget, CB_GETCURSEL, 0, 0);
    if (sel >= 0) prev = (DWORD)SendMessageW(g_hTarget, CB_GETITEMDATA, sel, 0);

    g_cands.clear();
    EnumCtx c;
    memset(&c, 0, sizeof c);
    GetWindowTextW(g_hTitle, c.needle, 256);
    CharLowerBuffW(c.needle, (DWORD)lstrlenW(c.needle));
    c.self = hwnd;
    if (c.needle[0]) EnumWindows(EnumProc, (LPARAM)&c);
    AddAudioSessions();

    SendMessageW(g_hTarget, CB_RESETCONTENT, 0, 0);
    int pick = 0;
    for (size_t i = 0; i < g_cands.size(); i++) {
        int idx = (int)SendMessageW(g_hTarget, CB_ADDSTRING, 0, (LPARAM)g_cands[i].label.c_str());
        SendMessageW(g_hTarget, CB_SETITEMDATA, idx, (LPARAM)g_cands[i].pid);
        if (g_cands[i].pid == prev) pick = (int)i;
    }
    if (g_cands.empty()) {
        Say(L"No matching window or audio-playing process found. Start playback, then press Refresh.");
    } else {
        SendMessageW(g_hTarget, CB_SETCURSEL, pick, 0);
        wchar_t m[100];
        wsprintfW(m, L"%d target(s) found. Pick the one to record.", (int)g_cands.size());
        Say(m);
    }
}

static void LaunchIsolatedBrowser(HWND) {
    wchar_t la[MAX_PATH] = {}, pf[MAX_PATH] = {}, pf86[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
    wchar_t paths[6][MAX_PATH + 64];
    wsprintfW(paths[0], L"%s\\Google\\Chrome\\Application\\chrome.exe", pf);
    wsprintfW(paths[1], L"%s\\Google\\Chrome\\Application\\chrome.exe", pf86);
    wsprintfW(paths[2], L"%s\\Google\\Chrome\\Application\\chrome.exe", la);
    wsprintfW(paths[3], L"%s\\Microsoft\\Edge\\Application\\msedge.exe", pf86);
    wsprintfW(paths[4], L"%s\\Microsoft\\Edge\\Application\\msedge.exe", pf);
    paths[5][0] = 0;
    const wchar_t* exe = nullptr;
    for (int i = 0; i < 5; i++)
        if (GetFileAttributesW(paths[i]) != INVALID_FILE_ATTRIBUTES) { exe = paths[i]; break; }
    if (!exe) { Say(L"Chrome/Edge not found in the usual locations."); return; }

    wchar_t dir[MAX_PATH + 64];
    wsprintfW(dir, L"%s\\AudibleRecorder", la);
    CreateDirectoryW(dir, nullptr);
    wsprintfW(dir, L"%s\\AudibleRecorder\\BrowserProfile", la);

    wchar_t cmd[1024];
    wsprintfW(cmd, L"\"%s\" --user-data-dir=\"%s\" --no-first-run --new-window https://www.audible.ca/library/audiobooks", exe, dir);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Say(L"Separate browser opened. Sign in, start the Audible Cloud Player there, then press Refresh and pick its [Window] entry.");
    } else {
        Say(L"Could not start the browser.");
    }
}

static void ShowElapsed() {
    LONG64 secs = g_rec.frames / 48000;
    wchar_t b[64];
    wsprintfW(b, L"%02d:%02d:%02d", (int)(secs / 3600), (int)((secs / 60) % 60), (int)(secs % 60));
    SetWindowTextW(g_hTime, b);
}

static void StartRecording(HWND hwnd) {
    wchar_t path[1024];
    GetWindowTextW(g_hPath, path, 1024);
    if (!path[0]) { Say(L"Choose an output file."); return; }

    int sel = (int)SendMessageW(g_hTarget, CB_GETCURSEL, 0, 0);
    if (sel < 0) {
        RefreshTargets(hwnd);
        sel = (int)SendMessageW(g_hTarget, CB_GETCURSEL, 0, 0);
    }
    if (sel < 0) return;   // RefreshTargets already explained
    DWORD pid = (DWORD)SendMessageW(g_hTarget, CB_GETITEMDATA, sel, 0);
    if (!pid) { Say(L"Pick a target process first."); return; }
    if (IsBrowserExe(ExeName(pid)) && !IsIsolated(pid)) {
        int a = MessageBoxW(hwnd,
            L"This is a regular browser instance. Audio from ALL of its tabs and windows (for example YouTube) will be recorded.\n\n"
            L"To record only Audible, press No, click \"Open Audible in separate browser\", play Audible there, press Refresh and pick the \"Isolated - safe\" entry.\n\n"
            L"Record anyway?",
            L"Audible Recorder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (a != IDYES) return;
    }

    BOOL ok = FALSE;
    UINT br = GetDlgItemInt(hwnd, ID_BITRATE, &ok, FALSE);
    if (!ok) br = 96;
    if (br < 16) br = 16;
    if (br > 320) br = 320;

    g_rec = RecState();
    g_rec.pid = pid;
    lstrcpynW(g_rec.path, path, 1024);
    g_rec.bitrate = (int)br;
    g_rec.hwnd = hwnd;
    g_rec.session = ++g_session;
    g_rec.thread = CreateThread(nullptr, 0, RecThread, nullptr, 0, nullptr);
    if (!g_rec.thread) { Say(L"Could not start the recording thread."); return; }

    g_recording = true;
    SetEditable(false);
    SetWindowTextW(g_hRec, L"Stop");
    ShowElapsed();
    wchar_t m[200];
    wsprintfW(m, L"Starting... (process %lu)", (unsigned long)pid);
    Say(m);
    SetTimer(hwnd, 1, 500, nullptr);
}

static void EndRecording(HWND hwnd, bool userStop) {
    if (!g_recording) return;
    InterlockedExchange(&g_rec.stop, 1);
    if (g_rec.thread) {
        WaitForSingleObject(g_rec.thread, 8000);
        CloseHandle(g_rec.thread);
        g_rec.thread = nullptr;
    }
    g_recording = false;
    KillTimer(hwnd, 1);
    ShowElapsed();
    SetEditable(true);
    SetWindowTextW(g_hRec, L"Record");
    wchar_t m[1300];
    if (g_err[0] && !userStop) {
        lstrcpynW(m, g_err, 1300);
    } else if (g_err[0]) {
        lstrcpynW(m, g_err, 1300);
    } else {
        wsprintfW(m, L"Saved: %s", g_rec.path);
    }
    Say(m);
    // fresh file name for the next take
    wchar_t p[1024];
    DefaultPath(p, 1024);
    SetWindowTextW(g_hPath, p);
}

static void Browse(HWND hwnd) {
    wchar_t path[1024];
    GetWindowTextW(g_hPath, path, 1024);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Opus audio (*.opus)\0*.opus\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"opus";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) SetWindowTextW(g_hPath, path);
}

static HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);

        Make(hwnd, L"STATIC", L"Window title:", SS_LEFT, 16, 20, 100, 20, -1);
        g_hTitle = Make(hwnd, L"EDIT", L"Audible Cloud Player", ES_AUTOHSCROLL | WS_TABSTOP, 120, 17, 300, 24, ID_TITLE, WS_EX_CLIENTEDGE);
        g_hRefresh = Make(hwnd, L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 428, 16, 76, 26, ID_REFRESH);

        Make(hwnd, L"STATIC", L"Record from:", SS_LEFT, 16, 56, 100, 20, -1);
        g_hTarget = Make(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 120, 53, 384, 220, ID_TARGET);

        Make(hwnd, L"STATIC", L"Save to:", SS_LEFT, 16, 92, 100, 20, -1);
        wchar_t p[1024];
        DefaultPath(p, 1024);
        g_hPath = Make(hwnd, L"EDIT", p, ES_AUTOHSCROLL | WS_TABSTOP, 120, 89, 300, 24, ID_PATH, WS_EX_CLIENTEDGE);
        g_hBrowse = Make(hwnd, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 428, 88, 76, 26, ID_BROWSE);

        Make(hwnd, L"STATIC", L"Bitrate (kbps):", SS_LEFT, 16, 128, 100, 20, -1);
        g_hBitrate = Make(hwnd, L"EDIT", L"96", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 120, 125, 60, 24, ID_BITRATE, WS_EX_CLIENTEDGE);
        g_hLaunch = Make(hwnd, L"BUTTON", L"Open Audible in separate browser", BS_PUSHBUTTON | WS_TABSTOP, 250, 124, 254, 26, ID_LAUNCH);

        g_hRec = Make(hwnd, L"BUTTON", L"Record", BS_PUSHBUTTON | WS_TABSTOP, 16, 166, 110, 34, ID_REC);
        g_hTime = Make(hwnd, L"STATIC", L"00:00:00", SS_LEFT, 140, 174, 80, 20, ID_TIME);
        g_hStatus = Make(hwnd, L"STATIC", L"", SS_LEFT, 16, 212, 488, 44, ID_STATUS);
        RefreshTargets(hwnd);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REC:
            if (g_recording) EndRecording(hwnd, true);
            else StartRecording(hwnd);
            break;
        case ID_BROWSE:
            Browse(hwnd);
            break;
        case ID_REFRESH:
            RefreshTargets(hwnd);
            break;
        case ID_LAUNCH:
            LaunchIsolatedBrowser(hwnd);
            break;
        }
        return 0;
    case WM_TIMER:
        if (g_recording) ShowElapsed();
        return 0;
    case WM_APP_STARTED:
        if (g_recording && lp == g_rec.session) {
            wchar_t m[300];
            wsprintfW(m, L"Recording process %lu -> %s", (unsigned long)g_rec.pid, g_rec.path);
            Say(m);
        }
        return 0;
    case WM_APP_DONE:
        if (g_recording && lp == g_rec.session) EndRecording(hwnd, false);
        return 0;
    case WM_CLOSE:
        if (g_recording) EndRecording(hwnd, true);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"AudibleRecorderWnd";
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(520), S(266)};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Audible Recorder", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
