#pragma once
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/helpers/event-list.hh"
#include <cctype>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <ostream>
#include <thread>
#include <memory>
#include <iostream>
#include <unordered_map>
#include "choc/text/choc_StringUtilities.h"
#include "choc/threading/choc_SpinLock.h"
#include "oscpkt.hh"
#include "udp.hh"

namespace sst::osc_adapter
{

inline clap_event_param_value makeParameterValueEvent(uint32_t time, int16_t port, int16_t channel,
                                                      int16_t key, int32_t note_id,
                                                      clap_id param_id, double value,
                                                      void *cookie = nullptr)
{
    clap_event_param_value pev;
    pev.header.flags = 0;
    pev.header.size = sizeof(clap_event_param_value);
    pev.header.type = CLAP_EVENT_PARAM_VALUE;
    pev.header.time = time;
    pev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    pev.port_index = port;
    pev.channel = channel;
    pev.key = key;
    pev.note_id = note_id;
    pev.param_id = param_id;
    pev.value = value;
    pev.cookie = cookie;
    return pev;
}

inline clap_event_note makeNoteEvent(uint32_t time, uint16_t etype, int16_t port, int16_t channel,
                                     int16_t key, int32_t note_id, double velocity)
{
    assert(etype >= CLAP_EVENT_NOTE_ON && etype <= CLAP_EVENT_NOTE_END);
    clap_event_note nev;
    nev.header.flags = 0;
    nev.header.size = sizeof(clap_event_note);
    nev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    nev.header.time = time;
    nev.header.type = etype;
    nev.port_index = port;
    nev.channel = channel;
    nev.key = key;
    nev.note_id = note_id;
    nev.velocity = velocity;
    return nev;
}

/* Remaps a value from a source range to a target range. Explodes if source range has zero size.
 */
template <typename Type>
Type mapvalue(Type sourceValue, Type sourceRangeMin, Type sourceRangeMax, Type targetRangeMin,
              Type targetRangeMax)
{
    return targetRangeMin + ((targetRangeMax - targetRangeMin) * (sourceValue - sourceRangeMin)) /
                                (sourceRangeMax - sourceRangeMin);
}

struct OSCAdapter
{
    OSCAdapter(const clap_plugin *p) : targetPlugin(p)
    {
        onUnhandledMessage = [this](oscpkt::Message *msg)
        { std::cout << "unhandled OSCmessage : " << msg->addressPattern() << std::endl; };
        paramsExtension = (clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        if (paramsExtension)
        {
            std::ofstream outfile(R"(C:\develop\six-sines\param_addresses.txt)");
            for (size_t i = 0; i < paramsExtension->count(p); ++i)
            {
                clap_param_info pinfo;
                if (paramsExtension->get_info(p, i, &pinfo))
                {
                    indexToClapParamInfo[i] = pinfo;
                    idToClapParamInfo[pinfo.id] = pinfo;
                    auto address = "/param/" + makeOscAddressFromParameterName(pinfo.name);
                    outfile << pinfo.name << " <- " << address << " [range " << pinfo.min_value
                            << " .. " << pinfo.max_value << "]\n";
                    addressToClapInfo[address] = pinfo;
                    idToAddress[pinfo.id] = address;
                    latestParamValues[pinfo.id] = pinfo.default_value;
                }
            }
            // for testing with TouchOsc 
            addressToClapInfo["/2/fader1"] = addressToClapInfo["/param/main_level"];
            addressToClapInfo["/2/fader2"] = addressToClapInfo["/param/main_pan"];
            // idToAddress[addressToClapInfo["/param/main_level"].id] = "/2/fader1";
        }
    }
    std::string makeOscAddressFromParameterName(const std::string &parname)
    {
        std::string result = parname;
        for (auto &c : result)
        {
            c = std::tolower(c);
            if (c == ' ')
                c = '_';
        }
        return result;
    }
    const clap_input_events *getInputEventQueue() { return eventList.clapInputEvents(); }
    const clap_output_events *getOutputEventQueue() { return eventListIncoming.clapOutputEvents(); }

    void startWith(uint32_t inputPort, uint32_t outputPort)
    {
        oscThreadShouldStop = false;
        oscThread = std::make_unique<std::thread>([=]() { runOscThread(inputPort, outputPort); });
    }
    void stop()
    {
        oscThreadShouldStop = true;
        if (oscThread)
        {
            oscThread->join();
            oscThread = nullptr;
        }
    }

    void handleOutputMessages(oscpkt::UdpSocket *socket, oscpkt::PacketWriter *pw)
    {
        // Locking here is very nasty as we are doing memory allocations, network traffic etc
        // and the audio thread might potentially have to wait but this shall suffice for some initial testing
        if (spinLock.try_lock())
        {
            auto evcount = eventListIncoming.size();
            if (evcount == 0)
            {
                spinLock.unlock();
                return;
            }
                
            for (size_t i = 0; i < evcount; ++i)
            {
                auto hdr = eventListIncoming.get(i);
                if (hdr->type == CLAP_EVENT_PARAM_VALUE)
                {
                    auto pev = (const clap_event_param_value *)hdr;
                    latestParamValues[pev->param_id] = pev->value;
                }
                else if (hdr->type == CLAP_EVENT_PARAM_GESTURE_END)
                {
                    auto pev = (const clap_event_param_gesture *)hdr;
                    oscpkt::Message repl;
                    auto it = latestParamValues.find(pev->param_id);
                    if (it != latestParamValues.end())
                    {
                        const auto &addr = idToAddress[pev->param_id];
                        repl.init(addr).pushFloat(it->second);
                        pw->init().addMessage(repl);
                        socket->sendPacketTo(pw->packetData(), pw->packetSize(),
                                             socket->packetOrigin());
                    }
                }
            }
            eventListIncoming.clear();
            spinLock.unlock();
        }
    }

    void runOscThread(uint32_t inputPort, uint32_t outputPort)
    {
        using namespace oscpkt;
        UdpSocket receiveSock;
        receiveSock.bindTo(inputPort);
        UdpSocket sendSock;

        sendSock.connectTo("localhost", outputPort);
        if (!receiveSock.isOk())
        {
            std::cout << "Error opening port " << inputPort << ": " << receiveSock.errorMessage()
                      << "\n";
            return;
        }
        if (!sendSock.isOk())
        {
            std::cout << "send socket not ok\n";
            return;
        }

        std::cout << "Server started, will listen to packets on port " << inputPort << std::endl;
        PacketReader pr;
        PacketWriter pw;

        while (!oscThreadShouldStop)
        {
            handleOutputMessages(&sendSock, &pw);
            if (!receiveSock.isOk())
            {
                break;
            }

            if (receiveSock.receiveNextPacket(30))
            {
                pr.init(receiveSock.packetData(), receiveSock.packetSize());
                oscpkt::Message *msg = nullptr;
                while (pr.isOk() && (msg = pr.popMessage()) != nullptr)
                {
                    int32_t iarg0 = 0;
                    int32_t iarg1 = 0;
                    float farg0 = 0.0f;
                    float farg1 = 0.0f;

                    // is it a named clap parameter?
                    auto mit = addressToClapInfo.find(msg->addressPattern());
                    if (mit != addressToClapInfo.end())
                    {
                        if (msg->match(mit->first).popFloat(farg0).isOkNoMoreArgs())
                        {
                            double val = mapvalue<float>(farg0, 0.0f, 1.0f, mit->second.min_value,
                                                         mit->second.max_value);
                            auto pev =
                                makeParameterValueEvent(0, -1, -1, -1, -1, mit->second.id, val);
                            addEventLocked((const clap_event_header *)&pev);
                        }
                    }

                    if (msg->match("/set_parameter")
                            .popInt32(iarg0)
                            .popFloat(farg0)
                            .isOkNoMoreArgs())
                    {
                        // indexed parameter
                        handle_set_parameter(msg, iarg0, farg0);
                    }
                    else if (msg->match("/mnote").popInt32(iarg0).popInt32(iarg1).isOkNoMoreArgs())
                    {
                        handle_mnote_msg(msg, iarg0, iarg1);
                    }
                    else if (msg->match("/fnote").popFloat(farg0).popInt32(iarg1).isOkNoMoreArgs())
                    {
                        handle_fnote_msg(msg, farg0, iarg1);
                    }
                    else if (msg->match("/mnote/rel")
                                 .popFloat(farg0)
                                 .popFloat(farg1)
                                 .isOkNoMoreArgs())
                    {

                        auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)farg0, -1,
                                                 farg1 / 127.0);
                        addEventLocked((const clap_event_header *)&nev);
                    }
                    else
                    {
                        if (onUnhandledMessage)
                            onUnhandledMessage(msg);
                    }
                }
            }
        }
    }
    void handle_set_parameter(oscpkt::Message *msg, int iarg0, float farg0)
    {
        auto it = indexToClapParamInfo.find(iarg0);
        if (it != indexToClapParamInfo.end())
        {
            auto pev = makeParameterValueEvent(0, -1, -1, -1, -1, it->second.id, farg0);
            addEventLocked((const clap_event_header *)&pev);
        }
    }

    void handle_fnote_msg(oscpkt::Message *msg, float farg0, int iarg1)
    {
        // float argument is Hz
        // not implemented yet, but this would need to create clap note on
        // and clap note pitch expression
    }

    void handle_mnote_msg(oscpkt::Message *msg, int iarg0, int iarg1)
    {
        uint16_t et = CLAP_EVENT_NOTE_ON;
        double velo = 0.0;
        if (iarg1 > 0)
        {
            velo = iarg1 / 127.0;
        }
        else
        {
            et = CLAP_EVENT_NOTE_OFF;
        }
        auto nev = makeNoteEvent(0, et, -1, 0, (int16_t)iarg0, -1, velo);
        addEventLocked((const clap_event_header *)&nev);
    }
    std::function<void(oscpkt::Message *msg)> onUnhandledMessage;
    std::unique_ptr<std::thread> oscThread;
    std::atomic<bool> oscThreadShouldStop{false};
    clap::helpers::EventList eventList;
    clap::helpers::EventList eventListIncoming;
    const clap_plugin *targetPlugin = nullptr;
    clap_plugin_params *paramsExtension = nullptr;
    std::unordered_map<size_t, clap_param_info> indexToClapParamInfo;
    std::unordered_map<clap_id, clap_param_info> idToClapParamInfo;
    std::unordered_map<std::string, clap_param_info> addressToClapInfo;
    std::unordered_map<clap_id, std::string> idToAddress;
    std::unordered_map<clap_id, float> latestParamValues;
    choc::threading::SpinLock spinLock;

  private:
    void addEventLocked(const clap_event_header *h)
    {
        std::lock_guard<choc::threading::SpinLock> guard(spinLock);
        eventList.push(h);
    }
};
} // namespace sst::osc_adapter
