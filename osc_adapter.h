#pragma once
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/helpers/event-list.hh"
#include <thread>
#include <memory>
#include <iostream>
#include <unordered_map>
#include "threading/choc_SpinLock.h"
#include "oscpkt.hh"
#include "udp.hh"

namespace sst::osc_adapter
{
struct OSCAdapter
{
    OSCAdapter(const clap_plugin *p) : targetPlugin(p)
    {
        paramsExtension = (clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        if (!paramsExtension)
            throw std::runtime_error("plugin doesn't implement parameters");
        for (size_t i = 0; i < paramsExtension->count(p); ++i)
        {
            clap_param_info pinfo;
            if (paramsExtension->get_info(p, i, &pinfo))
            {
                indexToClapParamInfo[i] = pinfo;
            }
        }
    }
    const clap_input_events *getInputEventQueue() { return eventList.clapInputEvents(); }
    clap_output_events *getOutputEventQueue(); // auto thread event queue

    void startWith(uint32_t inputPort, uint32_t outputPort)
    {
        oscThreadShouldStop = false;
        oscThread =
            std::make_unique<std::thread>([=, this]() { runOscThread(inputPort, outputPort); });
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
                    if (msg->match("/set_parameter")
                            .popInt32(iarg0)
                            .popFloat(darg0)
                            .isOkNoMoreArgs())
                    {
                        auto it = indexToClapParamInfo.find(iarg0);
                        if (it != indexToClapParamInfo.end())
                        {
                            // constructing clap events like this is a bit verbose,
                            // will probably want some kind of abstraction for this
                            clap_event_param_value pev;
                            pev.header.flags = 0;
                            pev.header.size = sizeof(clap_event_param_value);
                            pev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                            pev.header.time = 0;
                            pev.header.type = CLAP_EVENT_PARAM_VALUE;
                            pev.cookie = it->second.cookie;
                            pev.channel = -1;
                            pev.port_index = -1;
                            pev.key = -1;
                            pev.param_id = it->second.id;
                            pev.value = darg0;
                            spinLock.lock();
                            eventList.push((const clap_event_header *)&pev);
                            spinLock.unlock();
                        }
                    }
                    else if (msg->match("/mnote").popFloat(darg0).popFloat(darg1).isOkNoMoreArgs())
                    {
                        clap_event_note nev;
                        nev.header.flags = 0;
                        nev.header.size = sizeof(clap_event_note);
                        nev.header.type = CLAP_EVENT_NOTE_ON;
                        nev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                        nev.header.time = 0;
                        nev.channel = 0;
                        nev.port_index = -1;
                        // need to figure out how to handle optional osc arguments with oscpkt
                        nev.note_id = -1;
                        nev.key = (int)darg0;
                        // Clap note velocity is float 0..1
                        nev.velocity = darg1 / 127;
                        spinLock.lock();
                        eventList.push((const clap_event_header *)&nev);
                        spinLock.unlock();
                    }
                    else
                    {
                        handleCustomMessage(msg);
                    }
                }
            }
        }
    }
    // Subclasses can implement the method to handle custom OSC messages.
    // The signature is likely to change since we probably won't end up
    // using oscpkt in the end.
    virtual void handleCustomMessage(oscpkt::Message *msg) {}
    std::unique_ptr<std::thread> oscThread;
    std::atomic<bool> oscThreadShouldStop{false};
    clap::helpers::EventList eventList;
    const clap_plugin *targetPlugin = nullptr;
    clap_plugin_params *paramsExtension = nullptr;
    std::unordered_map<size_t, clap_param_info> indexToClapParamInfo;
    choc::threading::SpinLock spinLock;
};
} // namespace sst::osc_adapter
