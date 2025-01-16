#pragma once
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/helpers/event-list.hh"
#include <cstdint>
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

struct DefaultCustomResponder
{
    void handleCustomMessage(oscpkt::Message *msg)
    {
        std::cout << "unhandled OSC message : " << msg->addressPattern() << std::endl;
    }
};

template <typename CustomResponder = DefaultCustomResponder> struct OSCAdapter
{
    OSCAdapter(const clap_plugin *p) : targetPlugin(p)
    {
        paramsExtension = (clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        if (paramsExtension)
        {
            for (size_t i = 0; i < paramsExtension->count(p); ++i)
            {
                clap_param_info pinfo;
                if (paramsExtension->get_info(p, i, &pinfo))
                {
                    indexToClapParamInfo[i] = pinfo;
                    idToClapParamInfo[pinfo.id] = pinfo;
                    auto address = "/param/" + makeOscAddressFromParameterName(pinfo.name);
                    addressToClapInfo[address] = pinfo;
                }
            }
        }
    }
    std::string makeOscAddressFromParameterName(std::string parname)
    {
        auto lc = choc::text::toLowerCase(parname);
        lc = choc::text::replace(lc, " ", "_");
        return lc;
    }
    const clap_input_events *getInputEventQueue() { return eventList.clapInputEvents(); }
    clap_output_events *getOutputEventQueue();

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
    void runOscThread(uint32_t inputPort, uint32_t outputPort)
    {
        using namespace oscpkt;
        UdpSocket sock;
        sock.bindTo(inputPort);
        if (!sock.isOk())
        {
            std::cout << "Error opening port " << inputPort << ": " << sock.errorMessage() << "\n";
            return;
        }

        std::cout << "Server started, will listen to packets on port " << inputPort << std::endl;
        PacketReader pr;
        PacketWriter pw;

        while (!oscThreadShouldStop)
        {
            if (!sock.isOk())
            {
                break;
            }

            if (sock.receiveNextPacket(30))
            {
                pr.init(sock.packetData(), sock.packetSize());
                oscpkt::Message *msg = nullptr;
                while (pr.isOk() && (msg = pr.popMessage()) != nullptr)
                {
                    int32_t iarg0 = CLAP_INVALID_ID;
                    float darg0 = 0.0f;
                    float darg1 = 0.0f;
                    auto mit = addressToClapInfo.find(msg->addressPattern());
                    if (mit != addressToClapInfo.end())
                    {
                        if (msg->match(mit->first).popFloat(darg0).isOkNoMoreArgs())
                        {
                            auto pev =
                                makeParameterValueEvent(0, -1, -1, -1, -1, mit->second.id, darg0);
                            addEventLocked((const clap_event_header *)&pev);
                        }
                    }
                    if (msg->match("/set_parameter")
                            .popInt32(iarg0)
                            .popFloat(darg0)
                            .isOkNoMoreArgs())
                    {
                        auto it = indexToClapParamInfo.find(iarg0);
                        if (it != indexToClapParamInfo.end())
                        {
                            auto pev =
                                makeParameterValueEvent(0, -1, -1, -1, -1, it->second.id, darg0);
                            addEventLocked((const clap_event_header *)&pev);
                        }
                    }
                    else if (msg->match("/mnote").popFloat(darg0).popFloat(darg1).isOkNoMoreArgs())
                    {
                        uint16_t et = CLAP_EVENT_NOTE_ON;
                        double velo = 0.0;
                        if (darg1 > 0.0f)
                        {
                            velo = darg1 / 127;
                        }
                        else
                        {
                            et = CLAP_EVENT_NOTE_OFF;
                        }
                        auto nev = makeNoteEvent(0, et, -1, 0, (int16_t)darg0, -1, velo);
                        addEventLocked((const clap_event_header *)&nev);
                    }
                    else if (msg->match("/mnote/rel")
                                 .popFloat(darg0)
                                 .popFloat(darg1)
                                 .isOkNoMoreArgs())
                    {
                        auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)darg0, -1,
                                                 darg1 / 127.0);
                        addEventLocked((const clap_event_header *)&nev);
                    }
                    else
                    {
                        customResponder.handleCustomMessage(msg);
                    }
                }
            }
        }
    }
    CustomResponder customResponder;
    std::unique_ptr<std::thread> oscThread;
    std::atomic<bool> oscThreadShouldStop{false};
    clap::helpers::EventList eventList;
    const clap_plugin *targetPlugin = nullptr;
    clap_plugin_params *paramsExtension = nullptr;
    std::unordered_map<size_t, clap_param_info> indexToClapParamInfo;
    std::unordered_map<clap_id, clap_param_info> idToClapParamInfo;
    std::unordered_map<std::string, clap_param_info> addressToClapInfo;
    choc::threading::SpinLock spinLock;

  private:
    void addEventLocked(const clap_event_header *h)
    {
        std::lock_guard<choc::threading::SpinLock> guard(spinLock);
        eventList.push(h);
    }
};
} // namespace sst::osc_adapter
