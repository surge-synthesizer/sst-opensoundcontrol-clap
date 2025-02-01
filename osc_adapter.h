#pragma once
// #include "clap/plugin.h"
// #include "clap/events.h"
#include "clap/events.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
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
#include "clap/id.h"
#include "clap/stream.h"
#include "oscpkt.hh"
#include "udp.hh"
#include "sst/cpputils/ring_buffer.h"

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
    // -1 for key theoretically supported for note offs, but hmm...
    assert(key >= 0 && key <= 127);
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

struct light_clap_param_info
{
    clap_id id = CLAP_INVALID_ID;
    clap_param_info_flags flags = 0;
    void *cookie = nullptr;
    double min_value = 0.0;
    double max_value = 0.0;
    double default_value = 0.0;
};

inline light_clap_param_info fromFullParamInfo(clap_param_info *pinfo)
{
    light_clap_param_info info;
    info.cookie = pinfo->cookie;
    info.default_value = pinfo->default_value;
    info.min_value = pinfo->min_value;
    info.max_value = pinfo->max_value;
    info.flags = pinfo->flags;
    info.id = pinfo->id;
    return info;
}

struct OSCAdapter
{
    template <typename Callback> inline void forEachInputEvent(Callback &&f)
    {
        auto msg = fromOscThread.pop();
        while (msg.has_value())
        {
            auto ev = (const clap_event_header *)&(*msg);
            f(ev);
            msg = fromOscThread.pop();
        }
    }
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
                    indexToClapParamInfo[i] = fromFullParamInfo(&pinfo);
                    idToClapParamInfo[pinfo.id] = fromFullParamInfo(&pinfo);
                    auto address = "/param/" + makeOscAddressFromParameterName(pinfo.name);
                    // outfile << pinfo.name << " <- " << address << " [range " << pinfo.min_value
                    //        << " .. " << pinfo.max_value << "]\n";
                    addressToClapInfo[address] = fromFullParamInfo(&pinfo);
                    idToAddress[pinfo.id] = address;
                    latestParamValues[pinfo.id] = pinfo.default_value;
                }
            }
            // for testing with TouchOsc
            addressToClapInfo["/2/fader1"] = addressToClapInfo["/param/main_level"];
            addressToClapInfo["/2/fader2"] = addressToClapInfo["/param/main_pan"];
            addressToClapInfo["/2/fader3"] = addressToClapInfo["/param/op_1_feedback_level"];

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
        if (outputPortOk.load() && wantEvent(ev->type))
        {
            toOscThread.push(*(clap_multi_event *)ev);
        }
    }
    void handleInputMessages(oscpkt::UdpSocket &socket, oscpkt::PacketReader &pr)
    {
        if (socket.receiveNextPacket(oscReceiveTimeOut))
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
                float farg2 = 0.0f;
                // is it a named clap parameter?
                auto mit = addressToClapInfo.find(msg->addressPattern());
                if (mit != addressToClapInfo.end())
                {
                    if (msg->match(mit->first).popFloat(farg0).isOkNoMoreArgs())
                    {
                        auto value = getMappedParameterValue(&mit->second, farg0);
                        auto pev = makeParameterValueEvent(0, -1, -1, -1, -1, mit->second.id, value,
                                                           mit->second.cookie);
                        fromOscThread.push(*(clap_multi_event *)&pev);
                    }
                }

                if (msg->match("/set_parameter").popInt32(iarg0).popFloat(farg0).isOkNoMoreArgs())
                {
                    // indexed parameter
                    handle_set_parameter(iarg0, farg0);
                }
                else if (msg->match("/allsoundoff").isOkNoMoreArgs())
                {
                    auto mev = makeMIDI1Event(0, -1, 176, 120, 0);
                    fromOscThread.push(*(clap_multi_event *)&mev);
                }
                else if (msg->match("/allnotesoff").isOkNoMoreArgs())
                {
                    auto mev = makeMIDI1Event(0, -1, 176, 123, 0);
                    fromOscThread.push(*(clap_multi_event *)&mev);
                }
                else if (msg->match("/request_state").isOkNoMoreArgs())
                {
                    // we don't support this for now as it's complicated
                    // handle_state_request();
                }
                else if (msg->match("/mnote").popFloat(farg0).popFloat(farg1).isOkNoMoreArgs())
                {
                    handle_mnote_msg(farg0, farg1, std::nullopt);
                }
                else if (msg->match("/mnote")
                             .popFloat(farg0)
                             .popFloat(farg1)
                             .popFloat(farg2)
                             .isOkNoMoreArgs())
                {
                    handle_mnote_msg(farg0, farg1, farg2);
                }
                else if (msg->match("/fnote").popFloat(farg0).popFloat(farg1).isOkNoMoreArgs())
                {
                    handle_fnote_msg(farg0, farg1, std::nullopt);
                }
                else if (msg->match("/fnote")
                             .popFloat(farg0)
                             .popFloat(farg1)
                             .popFloat(farg2)
                             .isOkNoMoreArgs())
                {
                    handle_fnote_msg(farg0, farg1, farg2);
                }
                else if (msg->match("/mnote/rel").popFloat(farg0).popFloat(farg1).isOkNoMoreArgs())
                {

                    auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)farg0, -1,
                                             farg1 / 127.0);
                    fromOscThread.push(*(clap_multi_event *)&nev);
                }
                else if (msg->match("/mnote/rel")
                             .popFloat(farg0)
                             .popFloat(farg1)
                             .popFloat(farg2)
                             .isOkNoMoreArgs())
                {
                    auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)farg0,
                                             (int32_t)farg2, farg1 / 127.0);
                    fromOscThread.push(*(clap_multi_event *)&nev);
                }
                else if (msg->match("/fnote/rel").popFloat(farg0).popFloat(farg1).isOkNoMoreArgs())
                {
                    // this 2 argument fnote/rel path doesn't work yet
                    // auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, (int16_t)farg0, -1,
                    //                         farg1 / 127.0);
                    // fromOscThread.push(*(clap_multi_event *)&nev);
                }
                else if (msg->match("/fnote/rel")
                             .popFloat(farg0)
                             .popFloat(farg1)
                             .popFloat(farg2)
                             .isOkNoMoreArgs())
                {
                    auto key_and_detune = frequencyToKeyAndDetune(farg0);
                    auto nev = makeNoteEvent(0, CLAP_EVENT_NOTE_OFF, -1, 0, key_and_detune.first,
                                             (int32_t)farg2, farg1 / 127.0);
                    fromOscThread.push(*(clap_multi_event *)&nev);
                }
                else if (msg->match("/vnote")
                             .popFloat(farg0)
                             .popFloat(farg1)
                             .popFloat(farg2)
                             .isOkNoMoreArgs())
                {
                    handle_vnote_msg(farg0, farg1, farg2);
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
                        if (iarg1 == CLAP_NOTE_EXPRESSION_VOLUME)
                            farg0 = std::clamp(farg0, 0.000001f, 4.0f);
                        else if (iarg1 == CLAP_NOTE_EXPRESSION_TUNING)
                            farg0 = std::clamp(farg0, -120.0f, 120.0f);
                        else
                            farg0 = std::clamp(farg0, 0.0f, 1.0f);
                        auto expev = makeNoteExpressionEvent(0, -1, -1, -1, iarg0, iarg1, farg0);
                        fromOscThread.push(*(clap_multi_event *)&expev);
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

        clapHost->request_callback(clapHost);
    }
    void handleOutputMessages(oscpkt::UdpSocket &socket, oscpkt::PacketWriter &pw)
    {
        auto oscmsg = toOscThread.pop();
        while (oscmsg.has_value())
        {
            auto hdr = (const clap_event_header *)&(*oscmsg);
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
                    socket.sendPacketTo(pw.packetData(), pw.packetSize(), socket.packetOrigin());
                }
            }
            oscmsg = toOscThread.pop();
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
            std::cout << "Error opening receive port " << inputPort << ": "
                      << receiveSock.errorMessage() << "\n";
            return;
        }
        if (!sendSock.isOk())
        {
            std::cout << "send socket not ok\n";
            outputPortOk.store(false);
        }
        else
            outputPortOk.store(true);

        std::cout << "Server started, will listen to packets on port " << inputPort << std::endl;
        PacketReader pr;
        PacketWriter pw;

        while (!oscThreadShouldStop)
        {
            if (outputPortOk.load())
                handleOutputMessages(sendSock, pw);
            if (!receiveSock.isOk())
            {
                break;
            }
            handleInputMessages(receiveSock, pr);
        }
    }
    float getMappedParameterValue(light_clap_param_info *info, float value)
    {
        if (pluginParametersNormalized)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            value = mapvalue<float>(value, 0.0f, 1.0f, info->min_value, info->max_value);
            return value;
        }
        return std::clamp<float>(value, info->min_value, info->max_value);
    }
    void handle_set_parameter(int iarg0, float farg0)
    {
        auto it = indexToClapParamInfo.find(iarg0);
        if (it != indexToClapParamInfo.end())
        {
            auto value = getMappedParameterValue(&it->second, farg0);
            auto pev =
                makeParameterValueEvent(0, -1, -1, -1, -1, it->second.id, value, it->second.cookie);
            fromOscThread.push(*(clap_multi_event *)&pev);
        }
    }
    // EuroRack/VCV Rack convention : 0 volts is Middle C/261.6255653005986 Hz,
    // -1 volts is octave down from that, +1 volts is octave up from that
    static std::pair<int, double> voltageToKeyAndDetune(float volts)
    {
        double floatpitch = 60.0 + volts * 12.0;
        int key = (int)floatpitch;
        double detune = floatpitch - key;
        return {key, detune};
    }
    static std::pair<int, double> frequencyToKeyAndDetune(float hz)
    {
        // should we clamp or ignore out of bounds events?
        hz = std::clamp(hz, 8.175798915643707f, 12543.853951415975f);
        double floatpitch = 69.0 + std::log2(hz / 440.0) * 12.0;
        int key = (int)floatpitch;
        double detune = floatpitch - key;
        return {key, detune};
    }
    void handle_vnote_msg(float farg0, float farg1, float farg2)
    {
        uint16_t et = CLAP_EVENT_NOTE_ON;
        double velo = 0.0;
        if (farg1 > 0)
        {
            velo = farg1 / 127.0;
        }
        else
        {
            et = CLAP_EVENT_NOTE_OFF;
        }
        farg0 = std::clamp(farg0, -5.0f, 5.0f);
        auto key_and_detune = voltageToKeyAndDetune(farg0);
        auto nev = makeNoteEvent(0, et, -1, 0, key_and_detune.first, (int32_t)farg2, velo);
        auto expev = makeNoteExpressionEvent(0, -1, -1, -1, (int32_t)farg2,
                                             CLAP_NOTE_EXPRESSION_TUNING, key_and_detune.second);
        fromOscThread.push(*(clap_multi_event *)&nev);
        if (et == CLAP_EVENT_NOTE_ON)
            fromOscThread.push(*(clap_multi_event *)&expev);
    }
    void handle_fnote_msg(float farg0, int iarg1, std::optional<int> iarg2)
    {
        auto key_and_detune = frequencyToKeyAndDetune(farg0);
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
        auto nev = makeNoteEvent(0, et, -1, 0, key_and_detune.first, nid, velo);
        auto expev = makeNoteExpressionEvent(0, -1, -1, key_and_detune.first, nid,
                                             CLAP_NOTE_EXPRESSION_TUNING, key_and_detune.second);
        fromOscThread.push(*(clap_multi_event *)&nev);
        if (et == CLAP_EVENT_NOTE_ON)
            fromOscThread.push(*(clap_multi_event *)&expev);
    }

    void handle_mnote_msg(int iarg0, int iarg1, std::optional<int> iarg2)
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
        fromOscThread.push(*(clap_multi_event *)&nev);
    }
    bool wantEvent(int32_t eventType) const
    {
        return eventType == CLAP_EVENT_PARAM_VALUE || eventType == CLAP_EVENT_PARAM_GESTURE_END;
    }
    std::function<void(oscpkt::Message *msg)> onUnhandledMessage;
    std::function<void()> onMainThread;
    bool pluginParametersNormalized = true;
    std::atomic<bool> outputPortOk{false};
    int oscReceiveTimeOut = 30; // milliseconds
    const clap_plugin *targetPlugin = nullptr;
    const clap_host *clapHost = nullptr;
    clap_plugin_params *paramsExtension = nullptr;
    clap_plugin_state *stateExtension = nullptr;
    std::unordered_map<size_t, light_clap_param_info> indexToClapParamInfo;
    std::unordered_map<clap_id, light_clap_param_info> idToClapParamInfo;
    std::unordered_map<std::string, light_clap_param_info> addressToClapInfo;
    std::unordered_map<clap_id, std::string> idToAddress;
    std::unordered_map<clap_id, float> latestParamValues;

    union clap_multi_event
    {
        clap_event_header header;
        clap_event_param_value parval;
        clap_event_param_gesture pargest;
        clap_event_note notev;
        clap_event_note_expression nexpev;
        clap_event_midi midi;
    };
    // might need to tune the sizes
    alignas(32) sst::cpputils::SimpleRingBuffer<clap_multi_event, 1024> fromOscThread;
    alignas(32) sst::cpputils::SimpleRingBuffer<clap_multi_event, 1024> toOscThread;
    std::unique_ptr<std::thread> oscThread;
    std::atomic<bool> oscThreadShouldStop{false};
};
} // namespace sst::osc_adapter
