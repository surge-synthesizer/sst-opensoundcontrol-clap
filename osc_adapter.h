#pragma once
#include "clap/plugin.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/helpers/event-list.hh"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <ostream>
#include <thread>
#include <memory>
#include <iostream>
#include <unordered_map>
#include "choc_SpinLock.h"
#include "clap/stream.h"
#include "oscpkt.hh"
#include "udp.hh"

namespace sst::osc_adapter
{

struct osc_clap_string_event
{
    clap_event_header header;
    char bytes[256];
};

inline clap_event_midi makeMIDI1Event(uint32_t time, int16_t port, uint8_t b0, uint8_t b1,
                                      uint8_t b2)
{
    clap_event_midi mev;
    mev.header.time = time;
    mev.header.flags = 0;
    mev.header.size = sizeof(clap_event_midi);
    mev.header.type = CLAP_EVENT_MIDI;
    mev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    mev.port_index = port;
    mev.data[0] = b0;
    mev.data[1] = b1;
    mev.data[2] = b2;
    return mev;
}

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

inline clap_event_note_expression makeNoteExpressionEvent(uint32_t time, int16_t port,
                                                          int16_t channel, int16_t key, int32_t nid,
                                                          clap_note_expression eid, double amount)
{
    clap_event_note_expression nexp;
    nexp.header.time = time;
    nexp.header.flags = 0;
    nexp.header.size = sizeof(clap_event_note_expression);
    nexp.header.type = CLAP_EVENT_NOTE_EXPRESSION;
    nexp.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    nexp.expression_id = eid;
    nexp.value = amount;
    nexp.port_index = port;
    nexp.channel = channel;
    nexp.key = key;
    nexp.note_id = nid;
    return nexp;
}

/* Remaps a value from a source range to a target range. Explodes if source range has zero size.
 */
template <typename Type>
inline Type mapvalue(Type sourceValue, Type sourceRangeMin, Type sourceRangeMax,
                     Type targetRangeMin, Type targetRangeMax)
{
    return targetRangeMin + ((targetRangeMax - targetRangeMin) * (sourceValue - sourceRangeMin)) /
                                (sourceRangeMax - sourceRangeMin);
}

struct OSCAdapter
{
    OSCAdapter(const clap_plugin *p, const clap_host *h) : targetPlugin(p), clapHost(h)
    {
        // std::endl is usually considered sus, but here we actually want to flush to the output as
        // soon as possible
        onUnhandledMessage = [this](oscpkt::Message *msg)
        { std::cout << "unhandled OSCmessage : " << msg->addressPattern() << std::endl; };
        onMainThread = []() {};
        stateExtension = (clap_plugin_state *)p->get_extension(p, CLAP_EXT_STATE);
        paramsExtension = (clap_plugin_params *)p->get_extension(p, CLAP_EXT_PARAMS);
        if (paramsExtension)
        {
            // std::ofstream outfile(R"(C:\develop\six-sines\param_addresses.txt)");
            for (size_t i = 0; i < paramsExtension->count(p); ++i)
            {
                clap_param_info pinfo;
                if (paramsExtension->get_info(p, i, &pinfo))
                {
                    indexToClapParamInfo[i] = pinfo;
                    idToClapParamInfo[pinfo.id] = pinfo;
                    auto address = "/param/" + makeOscAddressFromParameterName(pinfo.name);
                    // outfile << pinfo.name << " <- " << address << " [range " << pinfo.min_value
                    //        << " .. " << pinfo.max_value << "]\n";
                    addressToClapInfo[address] = pinfo;
                    idToAddress[pinfo.id] = address;
                    latestParamValues[pinfo.id] = pinfo.default_value;
                }
            }
            // for testing with TouchOsc
            addressToClapInfo["/2/fader1"] = addressToClapInfo["/param/main_level"];
            addressToClapInfo["/2/fader2"] = addressToClapInfo["/param/main_pan"];
// idToAddress[addressToClapInfo["/param/main_level"].id] = "/2/fader1";
#ifdef TEST_CUSTOM_STRING_MESSAGES
            osc_clap_string_event ev;
            ev.header.size = sizeof(osc_clap_string_event);
            ev.header.type = 666;
            ev.header.time = 0;
            ev.header.flags = 0;
            ev.header.space_id = 42;
            strcpy(ev.bytes, targetPlugin->desc->id);
            eventListIncoming.push((const clap_event_header *)&ev);
            strcpy(ev.bytes, targetPlugin->desc->name);
            eventListIncoming.push((const clap_event_header *)&ev);
#endif
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
    void postEventForOscOutput(const clap_event_header *ev)
    {
        if (wantEvent(ev->type))
        {
            std::lock_guard<choc::threading::SpinLock> locker(spinLock);
            eventListIncoming.push(ev);
        }
    }
    void handleInputMessages(oscpkt::UdpSocket &socket, oscpkt::PacketReader &pr)
    {
        if (socket.receiveNextPacket(30))
        {
            pr.init(socket.packetData(), socket.packetSize());
            oscpkt::Message *msg = nullptr;
            while (pr.isOk() && (msg = pr.popMessage()) != nullptr)
            {
                int32_t iarg0 = 0;
                int32_t iarg1 = 0;
                int32_t iarg2 = 0;
                float farg0 = 0.0f;
                float farg1 = 0.0f;

                // is it a named clap parameter?
                auto mit = addressToClapInfo.find(msg->addressPattern());
                if (mit != addressToClapInfo.end())
                {
                    if (msg->match(mit->first).popFloat(farg0).isOkNoMoreArgs())
                    {
                        farg0 = std::clamp(farg0, 0.0f, 1.0f);
                        double val = mapvalue<float>(farg0, 0.0f, 1.0f, mit->second.min_value,
                                                     mit->second.max_value);
                        auto pev = makeParameterValueEvent(0, -1, -1, -1, -1, mit->second.id, val,
                                                           mit->second.cookie);
                        addEventLocked((const clap_event_header *)&pev);
                    }
                }

                if (msg->match("/set_parameter").popInt32(iarg0).popFloat(farg0).isOkNoMoreArgs())
                {
                    // indexed parameter
                    handle_set_parameter(msg, iarg0, farg0);
                }
                else if (msg->match("/allsoundoff").isOkNoMoreArgs())
                {
                    auto mev = makeMIDI1Event(0, -1, 176, 120, 0);
                    addEventLocked((const clap_event_header *)&mev);
                }
                else if (msg->match("/allnotesoff").isOkNoMoreArgs())
                {
                    auto mev = makeMIDI1Event(0, -1, 176, 123, 0);
                    addEventLocked((const clap_event_header *)&mev);
                }
                else if (msg->match("/request_state").isOkNoMoreArgs())
                {
                    handle_state_request();
                }
                else if (msg->match("/mnote").popInt32(iarg0).popInt32(iarg1).isOkNoMoreArgs())
                {
                    handle_mnote_msg(msg, iarg0, iarg1, std::nullopt);
                }
                else if (msg->match("/mnote")
                             .popInt32(iarg0)
                             .popInt32(iarg1)
                             .popInt32(iarg2)
                             .isOkNoMoreArgs())
                {
                    handle_mnote_msg(msg, iarg0, iarg1, iarg2);
                }
                else if (msg->match("/fnote").popFloat(farg0).popInt32(iarg1).isOkNoMoreArgs())
                {
                    handle_fnote_msg(msg, farg0, iarg1, std::nullopt);
                }
                else if (msg->match("/fnote")
                             .popFloat(farg0)
                             .popInt32(iarg1)
                             .popInt32(iarg2)
                             .isOkNoMoreArgs())
                {
                    handle_fnote_msg(msg, farg0, iarg1, iarg2);
                }
                else if (msg->match("/mnote/rel").popFloat(farg0).popFloat(farg1).isOkNoMoreArgs())
                {

                    auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)farg0, -1,
                                             farg1 / 127.0);
                    addEventLocked((const clap_event_header *)&nev);
                }
                else if (msg->match("/nexp")
                             .popInt32(iarg0)
                             .popInt32(iarg1)
                             .popFloat(farg0)
                             .isOkNoMoreArgs())
                {
                    if (iarg1 >= CLAP_NOTE_EXPRESSION_VOLUME &&
                        iarg1 <= CLAP_NOTE_EXPRESSION_PRESSURE)
                    {
                        // expression types have varying allowed ranges, should clamp to those here
                        // or in the event maker function
                        auto expev = makeNoteExpressionEvent(0, -1, -1, -1, iarg0, iarg1, farg0);
                        addEventLocked((const clap_event_header *)&expev);
                    }
                }
                else
                {
                    if (onUnhandledMessage)
                        onUnhandledMessage(msg);
                }
            }
        }
    }
    void handle_state_request()
    {
        if (!(stateExtension && clapHost))
            return;
        spinLock.lock();
        onMainThread = [this]()
        {
            clap_ostream os;
            int64_t bytesWritten = 0;
            os.ctx = &bytesWritten;
            // returns the number of bytes written; -1 on write error
            // int64_t(CLAP_ABI *write)(const struct clap_ostream *stream, const void *buffer,
            // uint64_t size);
            os.write = [](const clap_ostream *stream, const void *buffer, uint64_t size)
            {
                auto i = (int64_t *)stream->ctx;
                *i += size;
                return (int64_t)size;
            };
            if (stateExtension->save(targetPlugin, &os))
            {
                std::cout << "state size is " << bytesWritten << " bytes" << std::endl;
            }
            else
                std::cout << "plugin did not save state" << std::endl;
            onMainThread = []() {};
        };
        spinLock.unlock();
        clapHost->request_callback(clapHost);
    }
    void handleOutputMessages(oscpkt::UdpSocket &socket, oscpkt::PacketWriter &pw)
    {
        // Locking here is very nasty as we are doing memory allocations, network traffic etc
        // and the audio thread might potentially have to wait but this shall suffice for some
        // initial testing
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
                if (hdr->space_id == 42 && hdr->type == 666)
                {
                    auto sevt = (osc_clap_string_event *)hdr;
                    oscpkt::Message repl;
                    repl.init("/clap_string").pushStr(sevt->bytes);
                    pw.init().addMessage(repl);
                    socket.sendPacketTo(pw.packetData(), pw.packetSize(), socket.packetOrigin());
                }
                if (hdr->type == CLAP_EVENT_NOTE_END)
                {
                    auto nev = (clap_event_note *)hdr;
                    std::cout << "got note end event " << nev->key << std::endl;
                }
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
                        pw.init().addMessage(repl);
                        socket.sendPacketTo(pw.packetData(), pw.packetSize(),
                                            socket.packetOrigin());
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
            // checking that the receiving socket actually throttles, and it does
            // std::chrono::time_point t = std::chrono::system_clock::now();
            // time_t t_time_t = std::chrono::system_clock::to_time_t(t);
            // std::cout << "time " << t_time_t << std::endl;
            handleOutputMessages(sendSock, pw);
            if (!receiveSock.isOk())
            {
                break;
            }
            handleInputMessages(receiveSock, pr);
        }
    }
    void handle_set_parameter(oscpkt::Message *msg, int iarg0, float farg0)
    {
        auto it = indexToClapParamInfo.find(iarg0);
        if (it != indexToClapParamInfo.end())
        {
            auto pev =
                makeParameterValueEvent(0, -1, -1, -1, -1, it->second.id, farg0, it->second.cookie);
            addEventLocked((const clap_event_header *)&pev);
        }
    }

    void handle_fnote_msg(oscpkt::Message *msg, float farg0, int iarg1, std::optional<int> iarg2)
    {
        farg0 = std::clamp(farg0, 8.175798915643707f, 12543.853951415975f);
        double floatpitch = 69.0 + std::log2(farg0 / 440.0) * 12.0;
        int key = (int)floatpitch;
        double detune = floatpitch - key;
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
        int nid = iarg2.value_or(-1);
        auto nev = makeNoteEvent(0, et, -1, 0, key, nid, velo);
        auto expev =
            makeNoteExpressionEvent(0, -1, -1, key, nid, CLAP_NOTE_EXPRESSION_TUNING, detune);
        std::lock_guard<choc::threading::SpinLock> guard(spinLock);
        eventList.push((const clap_event_header *)&nev);
        eventList.push((const clap_event_header *)&expev);
    }

    void handle_mnote_msg(oscpkt::Message *msg, int iarg0, int iarg1, std::optional<int> iarg2)
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
        int32_t nid = iarg2.value_or(-1);
        auto nev = makeNoteEvent(0, et, -1, 0, iarg0, nid, velo);
        addEventLocked((const clap_event_header *)&nev);
    }
    bool wantEvent(int32_t eventType) const
    {
        return eventType == CLAP_EVENT_PARAM_VALUE || eventType == CLAP_EVENT_PARAM_GESTURE_END;
    }
    std::function<void(oscpkt::Message *msg)> onUnhandledMessage;
    std::function<void()> onMainThread;
    std::unique_ptr<std::thread> oscThread;
    std::atomic<bool> oscThreadShouldStop{false};
    clap::helpers::EventList eventList;
    clap::helpers::EventList eventListIncoming;
    const clap_plugin *targetPlugin = nullptr;
    const clap_host *clapHost = nullptr;
    clap_plugin_params *paramsExtension = nullptr;
    clap_plugin_state *stateExtension = nullptr;
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
