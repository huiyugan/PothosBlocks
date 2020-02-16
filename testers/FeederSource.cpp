// Copyright (c) 2014-2017 Josh Blum
//                    2020 Nicholas Corgan
// SPDX-License-Identifier: BSL-1.0

#include <Pothos/Framework.hpp>
#include <random>
#include <chrono>
#include <thread>
#include <queue>
#include <algorithm>
#include <json.hpp>

using json = nlohmann::json;

class FeederSource : Pothos::Block
{
public:
    FeederSource(const Pothos::DType &dtype)
    {
        this->setupOutput(0, dtype, this->uid()); //unique domain to force copies
        this->registerCall(this, POTHOS_FCN_TUPLE(FeederSource, feedTestPlan));
        this->registerCall(this, POTHOS_FCN_TUPLE(FeederSource, feedBuffer));
        this->registerCall(this, POTHOS_FCN_TUPLE(FeederSource, feedLabel));
        this->registerCall(this, POTHOS_FCN_TUPLE(FeederSource, feedMessage));
        this->registerCall(this, POTHOS_FCN_TUPLE(FeederSource, feedPacket));
    }

    static Block *make(const Pothos::DType &dtype)
    {
        return new FeederSource(dtype);
    }

    std::string feedTestPlan(const std::string &testPlan);

    void feedBuffer(const Pothos::BufferChunk &buffer)
    {
        _buffers.push(buffer);
    }

    void feedLabel(const Pothos::Label &label)
    {
        _labels.push(label);
    }

    void feedMessage(const Pothos::Object &message)
    {
        _messages.push(message);
    }

    void feedPacket(const Pothos::Packet &packet)
    {
        _packets.push(packet);
    }

    void work(void)
    {
        auto outputPort = this->output(0);

        //do labels first so they remain ahead of buffers
        while (not _labels.empty())
        {
            auto buffElems = _buffers.empty()?0:_buffers.front().length/outputPort->dtype().size();
            if (_labels.front().index >= buffElems + outputPort->totalElements()) break;

            _labels.front().index -= outputPort->totalElements(); //abs -> rel
            outputPort->postLabel(_labels.front());
            _labels.pop();
        }
        while (not _buffers.empty())
        {
            outputPort->postBuffer(std::move(_buffers.front()));
            _buffers.pop();
            return;
        }
        while (not _messages.empty())
        {
            outputPort->postMessage(std::move(_messages.front()));
            _messages.pop();
            return;
        }
        while (not _packets.empty())
        {
            outputPort->postMessage(std::move(_packets.front()));
            _packets.pop();
            return;
        }

        //enter backoff + wait for additional user stimulus
        std::this_thread::sleep_for(std::chrono::nanoseconds(this->workInfo().maxTimeoutNs));
        this->yield();
    }

private:
    std::queue<Pothos::BufferChunk> _buffers;
    std::queue<Pothos::Label> _labels;
    std::queue<Pothos::Object> _messages;
    std::queue<Pothos::Packet> _packets;
};

static Pothos::BlockRegistry registerSocketSink(
    "/blocks/feeder_source", &FeederSource::make);


//http://stackoverflow.com/questions/440133/how-do-i-create-a-random-alpha-numeric-string-in-c
std::string random_string(size_t length)
{
    static const std::string alphanums =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    static std::mt19937 rg(std::chrono::system_clock::now().time_since_epoch().count());
    static std::uniform_int_distribution<> pick(0, alphanums.size() - 1);

    std::string s;

    s.reserve(length);

    while(length--)
        s += alphanums[pick(rg)];

    return s;
}

std::string FeederSource::feedTestPlan(const std::string &testPlanStr)
{
    const auto testPlan = json::parse(testPlanStr);

    //test plan data
    json expectedResult(json::object());
    json expectedValues(json::array());
    json expectedLabels(json::array());
    json expectedMessages(json::array());
    json expectedPackets(json::array());

    //results to feed into the block
    std::vector<Pothos::BufferChunk> buffers;
    std::vector<Pothos::Label> labels;
    std::vector<Pothos::Object> messages;
    std::vector<Pothos::Packet> packets;

    //random generation
    std::random_device rd;
    std::mt19937 gen(rd());

    //defaults
    const bool enableBuffers = testPlan.value("enableBuffers", false);
    const bool enableLabels = testPlan.value("enableLabels", false);
    const bool enableMessages = testPlan.value("enableMessages", false);
    const bool enablePackets = testPlan.value("enablePackets", false);
    const int minTrials = testPlan.value("minTrials", 10);
    const int maxTrials = testPlan.value("maxTrials", 100);
    const int minSize = testPlan.value("minSize", 10);
    const int maxSize = testPlan.value("maxSize", 100);

    /*******************************************************************
     * Random buffer generation:
     *  - supports a stream of randomly sized buffers
     *  - or in packet mode it loads the packet payload
     ******************************************************************/
    unsigned long long totalElements = 0;
    if (enableBuffers or enablePackets)
    {
        //create random distributions
        const auto elemDType = this->output(0)->dtype();
        std::uniform_int_distribution<int> bufferDist(
            testPlan.value("minBuffers", minTrials),
            testPlan.value("maxBuffers", maxTrials));
        std::uniform_int_distribution<int> elementsDist(
            testPlan.value("minBufferSize", minSize)/elemDType.size(),
            testPlan.value("maxBufferSize", maxSize)/elemDType.size());
        const int valueSize = (1 << elemDType.size()*8);
        const int signedOff = elemDType.isSigned()?valueSize/2:0;
        std::uniform_real_distribution<float> valueDist(
            testPlan.value("minValue", -signedOff),
            testPlan.value("maxValue", valueSize-signedOff-1));

        //constraints on random numbers of elements produced
        const int totalMultiple = testPlan.value("totalMultiple", 1);
        const int bufferMultiple = testPlan.value("bufferMultiple", 1);

        //generate the buffers and elements
        const size_t numBuffs = bufferDist(gen);
        for (size_t bufno = 0; bufno < numBuffs; bufno++)
        {
            int numElems = elementsDist(gen);

            //round up to multiple and re-enforce the distribution bounds
            numElems = ((numElems + bufferMultiple - 1)/bufferMultiple)*bufferMultiple;
            if (numElems > elementsDist.b()) numElems -= bufferMultiple;
            if (numElems < elementsDist.a()) numElems += bufferMultiple;

            //pad last buffer to multiple when specified
            if ((bufno+1) == numBuffs)
            {
                const size_t extra = (totalElements + numElems) % totalMultiple;
                if (extra != 0) numElems += totalMultiple - extra;
            }

            //create random elements
            totalElements += numElems;
            Pothos::BufferChunk buff(elemDType, numElems);
            for (int i = 0; i < numElems; i++)
            {
                auto value = valueDist(gen);
                expectedValues.push_back(value);
                if (elemDType.size() == 1) buff.as<char *>()[i] = char(value);
                else if (elemDType.size() == 2) buff.as<short *>()[i] = short(value);
                else if (elemDType.size() == 4)
                {
                    if(elemDType.isFloat()) buff.as<float *>()[i] = value;
                    else buff.as<int *>()[i] = int(value);
                }
                else throw Pothos::AssertionViolationException("FeederSource::feedTestPlan()", "cant handle this dtype: " + elemDType.toString());
            }

            //store the resulting test plan
            if (enablePackets)
            {
                json expectedPacket(json::object());
                expectedPacket["expectedValues"] = expectedValues;
                expectedValues = json::array(); //clear for next packet
                expectedPackets.push_back(expectedPacket);

                Pothos::Packet packet;
                packet.payload = buff;
                packets.push_back(packet);
            }
            else buffers.push_back(buff);
        }
    }

    /*******************************************************************
     * Random label generation:
     *  - create randomly positioned labels anywhere in the stream
     *  - or in packet mode the labels translate to packet positions
     ******************************************************************/
    if (enableLabels and totalElements > 0)
    {
        //create random distributions
        std::uniform_int_distribution<int> labelDist(
            testPlan.value("minLabels", minTrials),
            testPlan.value("maxLabels", maxTrials));
        std::uniform_int_distribution<int> dataSizeDist(
            testPlan.value("minLabelSize", minSize),
            testPlan.value("maxLabelSize", maxSize));
        std::uniform_int_distribution<unsigned long long> indexDist(0, totalElements-1);

        //generate random label indexes and sort them
        std::vector<unsigned long long> labelIndexes;
        const size_t numLabels = labelDist(gen);
        for (size_t lblno = 0; lblno < numLabels; lblno++)
        {
            auto index = indexDist(gen);
            if (std::find(labelIndexes.begin(), labelIndexes.end(), index) == labelIndexes.end())
            {
                labelIndexes.push_back(index);
            }
        }
        std::sort(labelIndexes.begin(), labelIndexes.end());

        //generate random labels
        for (auto index : labelIndexes)
        {
            Pothos::Label lbl;
            lbl.index = index;
            auto data = random_string(dataSizeDist(gen));
            lbl.data = Pothos::Object(data);
            lbl.id = "id"+std::to_string(lbl.index);

            //in packet mode, adjust for relative packet offset
            size_t packetIndex = 0;
            if (enablePackets) for (const auto &packet : packets)
            {
                if (lbl.index < packet.payload.elements()) break;
                lbl.index -= packet.payload.elements();
                packetIndex++;
            }

            //create label descriptor
            json expectedLabel(json::object());
            expectedLabel["index"] = lbl.index;
            expectedLabel["data"] = data;
            expectedLabel["id"] = lbl.id;

            //store the resulting test plan
            if (enablePackets)
            {
                auto &expectedPacket = expectedPackets[packetIndex];
                expectedPacket["expectedLabels"].push_back(expectedLabel);
                packets.at(packetIndex).labels.push_back(lbl);
            }
            else
            {
                //in buffer mode, adjust for absolute start
                lbl.index += this->output(0)->totalElements();
                labels.push_back(lbl);
                expectedLabels.push_back(expectedLabel);
            }
        }
    }

    /*******************************************************************
     * Random message generation:
     *  - messages contain random string data
     ******************************************************************/
    if (enableMessages)
    {
        //create random distributions
        std::uniform_int_distribution<int> messageDist(
            testPlan.value("minMessages", minTrials),
            testPlan.value("maxMessages", maxTrials));
        std::uniform_int_distribution<int> dataSizeDist(
            testPlan.value("minMessageSize", minSize),
            testPlan.value("maxMessageSize", maxSize));

        const size_t numMessages = messageDist(gen);
        for (size_t msgno = 0; msgno < numMessages; msgno++)
        {
            auto data = random_string(dataSizeDist(gen));
            messages.emplace_back(data);
            expectedMessages.push_back(std::move(data));
        }
    }

    //load the test plan data when used
    if (not expectedValues.empty()) expectedResult["expectedValues"] = expectedValues;
    if (not expectedLabels.empty()) expectedResult["expectedLabels"] = expectedLabels;
    if (not expectedMessages.empty()) expectedResult["expectedMessages"] = expectedMessages;
    if (not expectedPackets.empty()) expectedResult["expectedPackets"] = expectedPackets;

    //feed the generated data
    for (const auto &lbl : labels) this->feedLabel(lbl);
    for (const auto &buff : buffers) this->feedBuffer(buff);
    for (const auto &msg : messages) this->feedMessage(msg);
    for (const auto &pkt : packets) this->feedPacket(pkt);

    return expectedResult.dump();
}
