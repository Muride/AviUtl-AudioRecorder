#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

// --- グローバル変数・定数 ---
#define PLUGIN_WINDOW_NAME L"AudioRecorderClient"
#define IDC_BTN_RECORD 1001
#define IDC_COMBO_DEVICE 1002
#define IDC_CHK_PLAY_SYNC 1003
#define IDC_EDIT_LAYER 1004
#define IDC_COMBO_RATE 1005
#define IDC_COMBO_CH 1006

EDIT_HANDLE* g_edit_handle = nullptr;
LOG_HANDLE* g_logger = nullptr;
CONFIG_HANDLE* g_config = nullptr;

HWND g_hwndBtnRecord = NULL;
HWND g_hwndComboDevice = NULL;
HWND g_hwndChkSync = NULL;
HWND g_hwndEditLayer = NULL;
HWND g_hwndComboRate = NULL;
HWND g_hwndComboCh = NULL;

bool g_isRecording = false;
int g_startFrame = 0;
std::wstring g_currentWavPath = L"";

// UIデザイン用（AviUtl準拠ダークモード）
HBRUSH g_hbrDark = NULL;
HBRUSH g_hbrEdit = NULL;
HFONT g_hFontSmall = NULL;

// 録音設定用
DWORD g_currentSampleRate = 48000;
WORD g_currentChannels = 1;

// 録音用変数
HWAVEIN g_hWaveIn = NULL;
WAVEHDR g_waveHdr1;
WAVEHDR g_waveHdr2;
char* g_buffer1 = nullptr;
char* g_buffer2 = nullptr;
std::ofstream g_wavFile;
DWORD g_dataSize = 0;

// --- プラグイン情報 ---
COMMON_PLUGIN_TABLE common_plugin_table = {
    L"簡易録音プラグイン (AudioRecorder)",
    L"AviUtl上で音声を録音し、即座にタイムラインに配置します。 By My-Coder"
};

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2003300; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) { g_logger = handle; }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) { g_config = handle; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) { return true; }
EXTERN_C __declspec(dllexport) void UninitializePlugin() {}
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) { return &common_plugin_table; }

// --- ユーティリティ: WAVヘッダー書き込み ---
void WriteWavHeader(std::ofstream& file, DWORD dataChunkSize, DWORD sampleRate, WORD numChannels) {
    DWORD riffSize = 36 + dataChunkSize;
    DWORD fmtSize = 16;
    WORD audioFormat = 1; // PCM
    DWORD byteRate = sampleRate * numChannels * 2;
    WORD blockAlign = numChannels * 2;
    WORD bitsPerSample = 16;

    file.seekp(0);
    file.write("RIFF", 4);
    file.write((char*)&riffSize, 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write((char*)&fmtSize, 4);
    file.write((char*)&audioFormat, 2);
    file.write((char*)&numChannels, 2);
    file.write((char*)&sampleRate, 4);
    file.write((char*)&byteRate, 4);
    file.write((char*)&blockAlign, 2);
    file.write((char*)&bitsPerSample, 2);
    file.write("data", 4);
    file.write((char*)&dataChunkSize, 4);
}

// --- 録音開始処理 ---
void StartRecording(HWND hwnd) {
    if (g_isRecording) return;

    int deviceIdx = SendMessage(g_hwndComboDevice, CB_GETCURSEL, 0, 0);
    if (deviceIdx == CB_ERR) deviceIdx = WAVE_MAPPER;

    int rateIdx = SendMessage(g_hwndComboRate, CB_GETCURSEL, 0, 0);
    g_currentSampleRate = (rateIdx == 0) ? 48000 : 44100;

    int chIdx = SendMessage(g_hwndComboCh, CB_GETCURSEL, 0, 0);
    g_currentChannels = (chIdx == 0) ? 1 : 2;

    g_currentWavPath = L"";
    g_startFrame = 0;
    
    g_edit_handle->call_edit_section_param(nullptr, [](void*, EDIT_SECTION* edit) {
        if (edit->get_project_file) {
            PROJECT_FILE* proj = edit->get_project_file(g_edit_handle);
            if (proj && proj->get_project_file_path) {
                LPCWSTR path = proj->get_project_file_path();
                if (path && wcslen(path) > 0) {
                    g_currentWavPath = path;
                }
            }
        }
        if (edit->info) {
            g_startFrame = edit->info->frame;
        }
    });

    time_t t = time(nullptr);
    struct tm tmInfo;
    localtime_s(&tmInfo, &t);
    wchar_t timeStr[64];
    swprintf_s(timeStr, L"Record_%04d%02d%02d_%02d%02d%02d.wav", 
        tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday, tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);

    if (g_currentWavPath.empty()) {
        wchar_t szFile[MAX_PATH] = L"";
        wcscpy_s(szFile, timeStr);
        OPENFILENAMEW ofn = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"WAV File\0*.wav\0All Files\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"wav";
        
        if (!GetSaveFileNameW(&ofn)) return;
        g_currentWavPath = szFile;
    } else {
        size_t lastSlash = g_currentWavPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            g_currentWavPath = g_currentWavPath.substr(0, lastSlash + 1) + timeStr;
        } else {
            g_currentWavPath += L"\\";
            g_currentWavPath += timeStr;
        }
    }

    g_wavFile.open(g_currentWavPath, std::ios::binary);
    if (!g_wavFile.is_open()) {
        MessageBoxW(hwnd, L"ファイルを作成できませんでした。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }
    g_dataSize = 0;
    WriteWavHeader(g_wavFile, 0, g_currentSampleRate, g_currentChannels);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = g_currentChannels;
    wfx.nSamplesPerSec = g_currentSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveInOpen(&g_hWaveIn, deviceIdx, &wfx, (DWORD_PTR)hwnd, 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
        g_wavFile.close();
        MessageBoxW(hwnd, L"マイクを開けませんでした。設定を変更して再試行してください。", L"エラー", MB_OK | MB_ICONERROR);
        return;
    }

    int bufferSize = wfx.nAvgBytesPerSec / 2;
    g_buffer1 = new char[bufferSize];
    g_buffer2 = new char[bufferSize];

    g_waveHdr1.lpData = g_buffer1; g_waveHdr1.dwBufferLength = bufferSize; g_waveHdr1.dwFlags = 0;
    g_waveHdr2.lpData = g_buffer2; g_waveHdr2.dwBufferLength = bufferSize; g_waveHdr2.dwFlags = 0;

    waveInPrepareHeader(g_hWaveIn, &g_waveHdr1, sizeof(WAVEHDR));
    waveInPrepareHeader(g_hWaveIn, &g_waveHdr2, sizeof(WAVEHDR));
    waveInAddBuffer(g_hWaveIn, &g_waveHdr1, sizeof(WAVEHDR));
    waveInAddBuffer(g_hWaveIn, &g_waveHdr2, sizeof(WAVEHDR));

    if (SendMessage(g_hwndChkSync, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        keybd_event(VK_SPACE, 0, 0, 0);
        keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
    }

    waveInStart(g_hWaveIn);
    g_isRecording = true;
    
    // ボタンの再描画を強制する
    InvalidateRect(g_hwndBtnRecord, NULL, TRUE);
    if(g_logger) g_logger->info(g_logger, L"録音を開始しました。");

    EnableWindow(g_hwndComboRate, FALSE);
    EnableWindow(g_hwndComboCh, FALSE);
}

// --- 録音停止処理 ---
void StopRecording() {
    if (!g_isRecording) return;
    g_isRecording = false;

    if (SendMessage(g_hwndChkSync, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        keybd_event(VK_SPACE, 0, 0, 0);
        keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
    }

    waveInStop(g_hWaveIn);
    waveInReset(g_hWaveIn);
    waveInUnprepareHeader(g_hWaveIn, &g_waveHdr1, sizeof(WAVEHDR));
    waveInUnprepareHeader(g_hWaveIn, &g_waveHdr2, sizeof(WAVEHDR));
    waveInClose(g_hWaveIn);

    delete[] g_buffer1;
    delete[] g_buffer2;

    if (g_wavFile.is_open()) {
        WriteWavHeader(g_wavFile, g_dataSize, g_currentSampleRate, g_currentChannels);
        g_wavFile.close();
    }

    // ボタンの再描画を強制する
    InvalidateRect(g_hwndBtnRecord, NULL, TRUE);

    EnableWindow(g_hwndComboRate, TRUE);
    EnableWindow(g_hwndComboCh, TRUE);

    wchar_t layerText[16];
    GetWindowTextW(g_hwndEditLayer, layerText, 16);
    int targetLayer = _wtoi(layerText) - 1;
    if (targetLayer < 0) targetLayer = 0;

    g_edit_handle->call_edit_section_param(&targetLayer, [](void* param, EDIT_SECTION* edit) {
        int layer = *(int*)param;
        int frameRate = edit->info->rate;
        int frameScale = edit->info->scale;
        double audioSeconds = (double)g_dataSize / (g_currentSampleRate * g_currentChannels * 2.0);
        int exactFrameLen = (int)(audioSeconds * frameRate / frameScale);
        if (exactFrameLen <= 0) exactFrameLen = 1;

        if (edit->create_object_from_media_file(g_currentWavPath.c_str(), layer, g_startFrame, exactFrameLen)) {
            if(g_logger) g_logger->info(g_logger, L"録音した音声をタイムラインに配置しました。");
        } else {
            if(g_logger) g_logger->warn(g_logger, L"音声の配置に失敗しました。");
        }
    });
}

// --- ウィンドウプロシージャ ---
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == IDC_BTN_RECORD) {
                if (g_isRecording) StopRecording();
                else StartRecording(hwnd);
                SetFocus(NULL);
            }
            // チェックボックスの文字（ラベル）をクリックしたとき、チェックボックスを反転させる
            else if (LOWORD(wparam) == 1007) {
                int state = SendMessage(g_hwndChkSync, BM_GETCHECK, 0, 0);
                SendMessage(g_hwndChkSync, BM_SETCHECK, state == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            }
            break;

        case MM_WIM_DATA: {
            WAVEHDR* pHdr = (WAVEHDR*)lparam;
            if (pHdr->dwBytesRecorded > 0 && g_wavFile.is_open()) {
                g_wavFile.write(pHdr->lpData, pHdr->dwBytesRecorded);
                g_dataSize += pHdr->dwBytesRecorded;
            }
            if (g_isRecording) {
                waveInAddBuffer(g_hWaveIn, pHdr, sizeof(WAVEHDR));
            }
            break;
        }

        // ★ ボタンを自力で描画（オーナードロー）して赤フチを実現！
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lparam;
            if (pdis->CtlID == IDC_BTN_RECORD) {
                // 背景色の決定（録音中は薄い赤、待機中はグレー）
                HBRUSH hbrBg = CreateSolidBrush(g_isRecording ? RGB(70, 30, 30) : RGB(60, 60, 60));
                FillRect(pdis->hDC, &pdis->rcItem, hbrBg);
                DeleteObject(hbrBg);

                // フチの色の決定（録音中は真っ赤、待機中は濃いグレー）
                HBRUSH hbrBorder = CreateSolidBrush(g_isRecording ? RGB(255, 60, 60) : RGB(40, 40, 40));
                FrameRect(pdis->hDC, &pdis->rcItem, hbrBorder);
                if (g_isRecording) { // 録音中はフチを太くする
                    RECT rcInner = pdis->rcItem;
                    InflateRect(&rcInner, -1, -1);
                    FrameRect(pdis->hDC, &rcInner, hbrBorder);
                }
                DeleteObject(hbrBorder);

                // テキストの描画
                SetTextColor(pdis->hDC, g_isRecording ? RGB(255, 180, 180) : RGB(240, 240, 240));
                SetBkMode(pdis->hDC, TRANSPARENT);
                LPCWSTR text = g_isRecording ? L"■ 録音停止" : L"● 録音開始";
                
                HFONT hOldFont = (HFONT)SelectObject(pdis->hDC, g_hFontSmall);
                DrawTextW(pdis->hDC, text, -1, &pdis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(pdis->hDC, hOldFont);
                
                return TRUE;
            }
            break;
        }

        // ダークモード化のおまじない
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wparam;
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_hbrDark;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wparam;
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(20, 20, 20)); // AviUtlより少し暗い色で凹みを表現
            return (LRESULT)g_hbrEdit;
        }

        case WM_DESTROY:
            if (g_isRecording) StopRecording();
            if (g_hbrDark) DeleteObject(g_hbrDark);
            if (g_hbrEdit) DeleteObject(g_hbrEdit);
            if (g_hFontSmall) DeleteObject(g_hFontSmall);
            break;
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

// --- 子ウィンドウ全フォント適用コールバック ---
BOOL CALLBACK EnumChildProc(HWND hwndChild, LPARAM lParam) {
    SendMessage(hwndChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

// --- プラグイン登録処理 ---
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    // AviUtl準拠の背景色(RGB 32,32,32)と小さめフォントの生成
    g_hbrDark = CreateSolidBrush(RGB(32, 32, 32));
    g_hbrEdit = CreateSolidBrush(RGB(20, 20, 20));
    g_hFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Meiryo UI");

    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
    wcex.lpszClassName = PLUGIN_WINDOW_NAME;
    wcex.lpfnWndProc = wnd_proc;
    wcex.hInstance = GetModuleHandle(0);
    wcex.hbrBackground = g_hbrDark;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(0, PLUGIN_WINDOW_NAME, L"簡易録音", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 260, 240, nullptr, nullptr, GetModuleHandle(0), nullptr);

    if (!hwnd) return;

    // マイク選択
    CreateWindowExW(0, L"STATIC", L"マイク:", WS_CHILD | WS_VISIBLE, 10, 12, 40, 20, hwnd, NULL, NULL, NULL);
    g_hwndComboDevice = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 55, 10, 190, 100, hwnd, (HMENU)IDC_COMBO_DEVICE, NULL, NULL);
    
    int numDevs = waveInGetNumDevs();
    for (int i = 0; i < numDevs; i++) {
        WAVEINCAPSW wic;
        if (waveInGetDevCapsW(i, &wic, sizeof(WAVEINCAPSW)) == MMSYSERR_NOERROR) {
            SendMessageW(g_hwndComboDevice, CB_ADDSTRING, 0, (LPARAM)wic.szPname);
        }
    }
    SendMessage(g_hwndComboDevice, CB_SETCURSEL, 0, 0);

    // 音質設定
    CreateWindowExW(0, L"STATIC", L"音質:", WS_CHILD | WS_VISIBLE, 10, 47, 40, 20, hwnd, NULL, NULL, NULL);
    g_hwndComboRate = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 50, 45, 90, 100, hwnd, (HMENU)IDC_COMBO_RATE, NULL, NULL);
    SendMessageW(g_hwndComboRate, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
    SendMessageW(g_hwndComboRate, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
    SendMessage(g_hwndComboRate, CB_SETCURSEL, 0, 0);

    g_hwndComboCh = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 150, 45, 95, 100, hwnd, (HMENU)IDC_COMBO_CH, NULL, NULL);
    SendMessageW(g_hwndComboCh, CB_ADDSTRING, 0, (LPARAM)L"Mono (1ch)");
    SendMessageW(g_hwndComboCh, CB_ADDSTRING, 0, (LPARAM)L"Stereo (2ch)");
    SendMessage(g_hwndComboCh, CB_SETCURSEL, 0, 0);

    // レイヤー設定
    CreateWindowExW(0, L"STATIC", L"配置レイヤー (例: 1):", WS_CHILD | WS_VISIBLE, 10, 82, 130, 20, hwnd, NULL, NULL, NULL);
    g_hwndEditLayer = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1", WS_CHILD | WS_VISIBLE | ES_NUMBER, 140, 80, 50, 20, hwnd, (HMENU)IDC_EDIT_LAYER, NULL, NULL);

    // プレビュー同期 (チェックボックスの四角と文字を分割して白文字化)
    g_hwndChkSync = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 115, 15, 20, hwnd, (HMENU)IDC_CHK_PLAY_SYNC, NULL, NULL);
    // 1007はラベルクリック用のID
    CreateWindowExW(0, L"STATIC", L"録音時にプレビューも再生する", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 28, 117, 210, 20, hwnd, (HMENU)1007, NULL, NULL);
    SendMessage(g_hwndChkSync, BM_SETCHECK, BST_CHECKED, 0);

    // 録音ボタン（BS_OWNERDRAWを追加して自作描画を有効化）
    g_hwndBtnRecord = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 10, 150, 235, 40, hwnd, (HMENU)IDC_BTN_RECORD, NULL, NULL);

    // 全部品にフォント適用
    EnumChildWindows(hwnd, EnumChildProc, (LPARAM)g_hFontSmall);

    host->register_window_client(PLUGIN_WINDOW_NAME, hwnd);
    g_edit_handle = host->create_edit_handle();
}