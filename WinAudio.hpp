#ifndef WINAUDIO_HPP
#define WINAUDIO_HPP

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <comdef.h>
#include <vector>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <tchar.h>
#include <exception>

extern "C" {
#include <libswresample/swresample.h>
}

#include "Queue.hpp"
#include "Frame.hpp"
#include "Reader.hpp"
#include "Exception.hpp"

namespace avio {

class WinAudio {
public:

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;
    UINT32 bufferFrameCount;
    UINT32 numFramesPadding;
    WAVEFORMATEX* pwfx = nullptr;
    REFERENCE_TIME hnsBufferDuration = 10000000; // 1s
    avio::Queue<avio::Frame>* input;

    WinAudio() {
        CoInitialize(nullptr);

        error(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator),
                "CoCreateInstance");

        error(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice),
                "IMMDeviceEnumerator::GetDefaultAudioEndpoint");

        error(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient),
                "IMMDevice::Activate");

        error(pAudioClient->GetMixFormat(&pwfx), 
                "IAudioClient::GetMixFormat");

        error(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, pwfx, nullptr),
                "IAudioClient::Initialize");

        pAudioClient->GetBufferSize(&bufferFrameCount);

        error(pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient),
                "IAudioClient::GetService");

        BYTE* pData = nullptr;
        error(pRenderClient->GetBuffer(bufferFrameCount, &pData), "IAudioRenderClient::GetBuffer");
        memset(pData, 0, bufferFrameCount * pwfx->nBlockAlign);
        error(pRenderClient->ReleaseBuffer(bufferFrameCount, 0), "IAudioRenderClient::ReleaseBuffer");
        error(pAudioClient->Start(), "IAudioClient::Start");
    }

    ~WinAudio() {
        std::cout << "WinAudio destructor" << std::endl;
        if (pAudioClient) pAudioClient->Stop();
        if (pRenderClient) pRenderClient->Release();
        if (pAudioClient) pAudioClient->Release();
        if (pDevice) pDevice->Release();
        if (pEnumerator) pEnumerator->Release();
        if (pwfx) CoTaskMemFree(pwfx);
        CoUninitialize();
    }

    int run() {
        avio::Frame frame = input->pop();
        if (frame.is_null()) {
            std::cout << "win audio recvd null frame" << std::endl;
            return 0;
        }

        int offset = 0;
        int converted = frame.samples();
        while (converted > 0) {
            error(pAudioClient->GetCurrentPadding(&numFramesPadding), "IAudioClient::GetCurrentPadding");
            UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
            int to_write = min((int)numFramesAvailable, converted);
            if (to_write > 0) {
                BYTE* pData = nullptr;
                error(pRenderClient->GetBuffer(to_write, &pData), "IAudioRenderClient::GetBuffer");
                memcpy(pData, frame.data() + offset * pwfx->nBlockAlign, to_write * pwfx->nBlockAlign);
                error(pRenderClient->ReleaseBuffer(to_write, 0), "IAudioRenderClient::GetBuffer");
                offset += to_write;
                converted -= to_write;
            }
            else {
                Sleep(5);
            }
        }

        return 1;
    }

    std::string getMixFormat() const {

        std::string format = "WAVE_FORMAT_UNKNOWN";
        std::string sub_format = "MIX_FORMAT_UNKOWN";

        std::stringstream str;
        str << "aformat=sample_fmts=";

        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            format = "WAVE_FORMAT_EXTENSIBLE";
            WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)pwfx;
            if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                sub_format = "KSDATAFORMAT_SUBTYPE_PCM";
                str << "s16";
            }
            else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                sub_format = "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT";
                str << "flt";
            }
            else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_DRM) {
                sub_format = "KSDATAFORMAT_SUBTYPE_DRM";
            }
            else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_ALAW) {
                sub_format = "KSDATAFORMAT_SUBTYPE_ALAW";
            }
            else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_MULAW) {
                sub_format = "KSDATAFORMAT_SUBTYPE_MULAW";
            }
            else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_ADPCM) {
                sub_format = "KSDATAFORMAT_SUBTYPE_ADPCM";
            }
        }

        str << ":channel_layouts=";
        if (pwfx->nChannels == 1) {
            str << "mono";
        }
        else if (pwfx->nChannels == 2) {
            str << "stereo";
        }

        str << ":sample_rates=" << pwfx->nSamplesPerSec;

        std::cout << "wFormatTag: " << format << std::endl;
        std::cout << "SubFormat: " << sub_format << std::endl;

        std::cout << "channels:     " << pwfx->nChannels       << "\n"
                  << "samples/sec:  " << pwfx->nSamplesPerSec  << "\n"
                  << "avg bytes/sec " << pwfx->nAvgBytesPerSec << "\n"
                  << "block align   " << pwfx->nBlockAlign     << "\n"
                  << "bits/sample   " << pwfx->wBitsPerSample  << "\n"
                  << "extra size    " << pwfx->cbSize          << "\n"
                  << std::endl;

        return std::string(str.str());
    }

    std::vector<std::string> getDeviceNames() {
        LPWSTR pwszID = nullptr;
        IPropertyStore* pProps = nullptr;
        std::vector<std::string> result;
        IMMDevice* pEndpoint = nullptr;
        IMMDeviceCollection *pCollection = nullptr;

        try {
            error(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection), 
                    "IMMDeviceEnumerator::EnumAudioEndpoints");
            UINT count;
            error(pCollection->GetCount(&count), "IMMDeviceCollection::GetCount");
            for (ULONG i = 0; i < count; i++) {
                error(pCollection->Item(i, &pEndpoint), "IMMDeviceCollection::Item");
                error(pEndpoint->GetId(&pwszID), "IMMDevice::GetId");
                error(pEndpoint->OpenPropertyStore(STGM_READ, &pProps), "IMMDevice::OpenPropertyStore");
                PROPVARIANT varName;
                PropVariantInit(&varName);
                error(pProps->GetValue(PKEY_Device_FriendlyName, &varName), "IPropertyStore::GetValue");
                if (varName.vt != VT_EMPTY) {
                    std::string name = ConvertLPWSTRToString(varName.pwszVal);
                    result.push_back(name);
                }
                CoTaskMemFree(pwszID);
                pwszID = nullptr;
                pProps->Release();
                pEndpoint->Release();
            }
        }
        catch (const std::exception& e) {
            std::cout << e.what() << std::endl;
        }
        if (pCollection) pCollection->Release();
        return result;
    }

    std::string ConvertLPWSTRToString(LPWSTR lpwstr) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, lpwstr, -1, NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, lpwstr, -1, &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    std::string TCHARToString(const TCHAR* tcharStr) {
        #ifdef UNICODE
            int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, tcharStr, -1, nullptr, 0, nullptr, nullptr);
            std::vector<char> buffer(sizeNeeded);
            WideCharToMultiByte(CP_UTF8, 0, tcharStr, -1, buffer.data(), sizeNeeded, nullptr, nullptr);
            return std::string(buffer.data());
        #else
            return std::string(tcharStr);
        #endif
    }

    error(HRESULT hr, const std::string& msg) {
        if (FAILED(hr)) {
            _com_error err(hr);
            std::stringstream str;
            str << msg << " : " << TCHARToString(err.ErrorMessage());
            throw std::runtime_error(str.str());
        }
    }
};

}

#endif // WINAUDIO_HPP