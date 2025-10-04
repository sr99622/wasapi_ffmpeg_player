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

#include "Player.hpp"
#include "Drain.hpp"
#include "WinAudio.hpp"

int main(int argc, char* argv[]) {
    try {

        avio::WinAudio audio;
        std::vector<std::string> devices = audio.getDeviceNames();
        for (int i = 0; i < devices.size(); i++) {
            std::cout << "Device Name: " << devices[i] << std::endl;
        }

        std::string filename;
        if (argc > 1) {
            filename = argv[1];
            std::cout << filename << std::endl;
        }
        std::cout << "hello" << std::endl;
        avio::Reader reader(filename);
        std::cout << "fornat: " << reader.str_sample_format() << std::endl;
        std::cout << "layout: " << reader.str_channel_layout() << std::endl;
        avio::Queue<avio::Packet> pkts(128);
        avio::Queue<avio::Frame> frames(128);
        avio::Queue<avio::Frame> output(128);
        reader.audio_pkts = &pkts;
        avio::Decoder decoder(&reader, AVMEDIA_TYPE_AUDIO, &pkts, &frames);

        std::string afilter = audio.getMixFormat();
        std::cout << "afilter: " << afilter << std::endl;

        //std::string arg = "aformat=sample_fmts=flt:channel_layouts=stereo:sample_rates=48000";
        avio::Filter filter(&decoder, audio.getMixFormat(), &frames, &output);
        audio.input = &output;

        std::thread reader_thread([&] { while (reader.read()) {} });
        std::thread decoder_thread([&] { while (decoder.decode()) {} });
        std::thread filter_thread([&] { while (filter.filter()) {} });
        std::thread audio_thread([&] { while (audio.run()) {} });

        reader_thread.join();
        decoder_thread.join();
        filter_thread.join();
        audio_thread.join();

        std::cout << "DONE" << std::endl;
    }
    catch (const std::exception& ex) {
        std::cout << ex.what() << std::endl;
    }
}

