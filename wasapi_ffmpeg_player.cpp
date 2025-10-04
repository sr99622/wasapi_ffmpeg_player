// wasapi_ffmpeg_player.cpp
//
// Build with:
//   cl /EHsc wasapi_ffmpeg_player.cpp /I<ffmpeg-include> /link /LIBPATH:<ffmpeg-lib> avcodec.lib avformat.lib avutil.lib swresample.lib ole32.lib uuid.lib

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#define EXIT_ON_ERROR(hr, msg) if (FAILED(hr)) { std::cerr << msg << " hr=0x" << std::hex << hr << std::endl; return -1; }

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: wasapi_ffmpeg_player <audiofile>" << std::endl;
        return -1;
    }

    const char* filename = argv[1];

    // -------- FFmpeg: open file --------
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, filename, nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input file" << std::endl;
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        return -1;
    }

    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::cerr << "Failed to find audio stream" << std::endl;
        return -1;
    }

    AVStream* audio_stream = fmt_ctx->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "Decoder not found" << std::endl;
        return -1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) return -1;
    if (avcodec_parameters_to_context(codec_ctx, audio_stream->codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        return -1;
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        return -1;
    }

    // -------- WASAPI: setup --------
    HRESULT hr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    EXIT_ON_ERROR(hr, "CoCreateInstance failed");

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr, "GetDefaultAudioEndpoint failed");

    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr, "Activate failed");

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    EXIT_ON_ERROR(hr, "GetMixFormat failed");

    std::cout << "WASAPI Mix Format: "
              << pwfx->nSamplesPerSec << " Hz, "
              << pwfx->nChannels << " ch, "
              << pwfx->wBitsPerSample << " bits"
              << std::endl;

    // -------- FFmpeg SwrContext: match WASAPI mix format --------
    SwrContext* swr = nullptr;

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, pwfx->nChannels);

    AVSampleFormat out_sample_fmt;
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)pwfx;
        if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            out_sample_fmt = AV_SAMPLE_FMT_S16;
        } else if (wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            out_sample_fmt = AV_SAMPLE_FMT_FLT;
        } else {
            std::cerr << "Unsupported WASAPI mix format" << std::endl;
            return -1;
        }
    } else {
        std::cerr << "Unsupported WAVEFORMATEX tag" << std::endl;
        return -1;
    }

    if (swr_alloc_set_opts2(
            &swr,
            &out_ch_layout,                       // output layout (WASAPI mix)
            out_sample_fmt,                       // output format
            pwfx->nSamplesPerSec,                 // output sample rate
            &codec_ctx->ch_layout,                // input layout
            codec_ctx->sample_fmt,                // input format
            codec_ctx->sample_rate,               // input sample rate
            0, nullptr) < 0)
    {
        std::cerr << "Failed to initialize SwrContext" << std::endl;
        return -1;
    }
    if (swr_init(swr) < 0) {
        std::cerr << "Failed to swr_init()" << std::endl;
        return -1;
    }

    // -------- WASAPI Initialize --------
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  0,
                                  10000000, // 1 second buffer
                                  0,
                                  pwfx,
                                  nullptr);
    EXIT_ON_ERROR(hr, "Failed to initialize audio client");

    UINT32 bufferFrameCount;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    EXIT_ON_ERROR(hr, "GetBufferSize failed");

    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
    EXIT_ON_ERROR(hr, "GetService failed");

    // Pre-roll: fill the buffer before Start()
    {
        BYTE* pData = nullptr;
        hr = pRenderClient->GetBuffer(bufferFrameCount, &pData);
        if (SUCCEEDED(hr)) {
            memset(pData, 0, bufferFrameCount * pwfx->nBlockAlign); // silence
            pRenderClient->ReleaseBuffer(bufferFrameCount, 0);
        }
    }

    hr = pAudioClient->Start();
    EXIT_ON_ERROR(hr, "Start failed");

    // -------- Decode + Play loop --------
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_index) {
            if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    // Convert with swr
                    uint8_t* out_data[2] = { nullptr };
                    int out_linesize = 0;
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(swr, codec_ctx->sample_rate) + frame->nb_samples,
                        pwfx->nSamplesPerSec,
                        codec_ctx->sample_rate,
                        AV_ROUND_UP);

                    av_samples_alloc(out_data, &out_linesize, pwfx->nChannels,
                                     out_samples, out_sample_fmt, 1);

                    int converted = swr_convert(swr, out_data, out_samples,
                                                (const uint8_t**)frame->data, frame->nb_samples);

                    std::cout << "Converted: " << converted << " frames" << std::endl;

                    if (converted > 0) {
                        UINT32 numFramesPadding = 0;
                        pAudioClient->GetCurrentPadding(&numFramesPadding);
                        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;

                        std::cout << "Available: " << numFramesAvailable << " frames" << std::endl;

                        int offset = 0;
                        while (converted > 0) {
                            pAudioClient->GetCurrentPadding(&numFramesPadding);
                            numFramesAvailable = bufferFrameCount - numFramesPadding;

                            int toWrite = min((int)numFramesAvailable, converted);
                            if (toWrite > 0) {
                                BYTE* pData = nullptr;
                                hr = pRenderClient->GetBuffer(toWrite, &pData);
                                if (SUCCEEDED(hr)) {
                                    memcpy(pData,
                                           out_data[0] + offset * pwfx->nBlockAlign,
                                           toWrite * pwfx->nBlockAlign);
                                    pRenderClient->ReleaseBuffer(toWrite, 0);
                                    offset += toWrite;
                                    converted -= toWrite;
                                }
                            } else {
                                Sleep(5); // wait for buffer space
                            }
                        }
                    }

                    av_freep(&out_data[0]);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // -------- Cleanup --------
    pAudioClient->Stop();
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    if (pRenderClient) pRenderClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();
    CoTaskMemFree(pwfx);
    CoUninitialize();

    return 0;
}
