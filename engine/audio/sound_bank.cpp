#include "sound_bank.h"

#include <engine/core/file/atomic_file.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <exception>

namespace me
{

namespace
{
const char* PropertyName(SoundProperty property)
{
    switch (property)
    {
    case SoundProperty::Pitch:
        return "pitch";
    case SoundProperty::Fade:
        return "fade";
    case SoundProperty::Volume:
        break;
    }
    return "volume";
}

SoundProperty PropertyFromName(const std::string& name)
{
    if (name == "pitch")
    {
        return SoundProperty::Pitch;
    }
    if (name == "fade")
    {
        return SoundProperty::Fade;
    }
    return SoundProperty::Volume;
}

void EmitAutomations(YAML::Emitter& out, const std::vector<SoundAutomation>& automations)
{
    if (automations.empty())
    {
        return;
    }
    out << YAML::Key << "automations" << YAML::Value << YAML::BeginSeq;
    for (const SoundAutomation& automation : automations)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "parameter" << YAML::Value << automation.parameter;
        out << YAML::Key << "property" << YAML::Value << PropertyName(automation.property);
        out << YAML::Key << "curve" << YAML::Value << YAML::BeginSeq;
        for (const SoundCurvePoint& point : automation.curve)
        {
            out << YAML::Flow << YAML::BeginSeq << point.x << point.y << point.shape << YAML::EndSeq;
        }
        out << YAML::EndSeq;
        if (!automation.mapping.empty())
        {
            out << YAML::Key << "mapping" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const auto& [from, to] : automation.mapping)
            {
                out << YAML::Flow << YAML::BeginSeq << from << to << YAML::EndSeq;
            }
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
}

std::vector<SoundAutomation> ReadAutomations(const YAML::Node& node)
{
    std::vector<SoundAutomation> automations;
    for (const YAML::Node& entry : node["automations"])
    {
        SoundAutomation automation;
        automation.parameter = entry["parameter"].as<std::string>();
        automation.property = PropertyFromName(entry["property"].as<std::string>("volume"));
        for (const YAML::Node& point : entry["curve"])
        {
            automation.curve.push_back({point[0].as<float>(), point[1].as<float>(), point[2].as<float>(0.0f)});
        }
        for (const YAML::Node& pair : entry["mapping"])
        {
            automation.mapping.emplace_back(pair[0].as<float>(), pair[1].as<float>());
        }
        automations.push_back(std::move(automation));
    }
    return automations;
}

// How far along a segment the value has moved at t (0 to 1) for the segment's shape.
float ShapeProgress(float t, float shape)
{
    if (shape == 0.0f)
    {
        return t;
    }
    // FMOD keeps its curve shape's formula to itself; a power keeps the two ends and bends the middle the
    // way the shape's sign says (crossfades come out near equal power with FMOD's default ±0.25).
    return std::pow(t, std::pow(4.0f, shape));
}
}

const SoundParameter* SoundEventDesc::FindParameter(std::string_view parameterName) const
{
    for (const SoundParameter& parameter : parameters)
    {
        if (parameter.name == parameterName)
        {
            return &parameter;
        }
    }
    return nullptr;
}

const SoundEventDesc* SoundBank::Find(std::string_view eventName) const
{
    for (const SoundEventDesc& event : events)
    {
        if (event.name == eventName)
        {
            return &event;
        }
    }
    return nullptr;
}

float EvaluateSoundCurve(const std::vector<SoundCurvePoint>& curve, float x)
{
    if (curve.empty())
    {
        return 0.0f;
    }
    if (x <= curve.front().x)
    {
        return curve.front().y;
    }
    for (size_t index = 1; index < curve.size(); ++index)
    {
        const SoundCurvePoint& from = curve[index - 1];
        const SoundCurvePoint& to = curve[index];
        if (x < to.x)
        {
            const float span = to.x - from.x;
            const float t = span > 0.0f ? (x - from.x) / span : 1.0f;
            return from.y + (to.y - from.y) * ShapeProgress(t, from.shape);
        }
    }
    return curve.back().y;
}

float EvaluateSoundMapping(const std::vector<std::pair<float, float>>& mapping, float v)
{
    if (mapping.empty())
    {
        return v;
    }
    if (v <= mapping.front().first)
    {
        return mapping.front().second;
    }
    for (size_t index = 1; index < mapping.size(); ++index)
    {
        const auto& [x0, y0] = mapping[index - 1];
        const auto& [x1, y1] = mapping[index];
        if (v < x1)
        {
            return x1 > x0 ? y0 + (y1 - y0) * (v - x0) / (x1 - x0) : y1;
        }
    }
    return mapping.back().second;
}

std::filesystem::path SoundBankPathForModel(const std::filesystem::path& modelPath)
{
    std::filesystem::path path = modelPath.parent_path() / modelPath.stem();
    path += ".sounds.yaml";
    return path;
}

bool SaveSoundBank(const std::filesystem::path& path, const SoundBank& bank, std::string& error)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "events" << YAML::Value << YAML::BeginSeq;
    for (const SoundEventDesc& event : bank.events)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << event.name;
        if (event.timelineLoops)
        {
            out << YAML::Key << "timeline_loops" << YAML::Value << true;
        }
        out << YAML::Key << "parameters" << YAML::Value << YAML::BeginSeq;
        for (const SoundParameter& parameter : event.parameters)
        {
            out << YAML::Flow << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << parameter.name;
            out << YAML::Key << "min" << YAML::Value << parameter.minimum;
            out << YAML::Key << "max" << YAML::Value << parameter.maximum;
            out << YAML::Key << "default" << YAML::Value << parameter.defaultValue;
            if (parameter.seekSpeed > 0.0f)
            {
                out << YAML::Key << "seek_speed" << YAML::Value << parameter.seekSpeed;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::Key << "buses" << YAML::Value << YAML::BeginSeq;
        for (const SoundBus& bus : event.buses)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "name" << YAML::Value << bus.name;
            out << YAML::Key << "parent" << YAML::Value << bus.parent;
            out << YAML::Key << "volume_db" << YAML::Value << bus.volumeDb;
            EmitAutomations(out, bus.automations);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::Key << "instruments" << YAML::Value << YAML::BeginSeq;
        for (const SoundInstrument& instrument : event.instruments)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "clips" << YAML::Value << YAML::BeginSeq;
            for (const SoundClip& clip : instrument.clips)
            {
                out << YAML::Flow << YAML::BeginMap << YAML::Key << "file" << YAML::Value << clip.file;
                out << YAML::Key << "volume_db" << YAML::Value << clip.volumeDb << YAML::EndMap;
            }
            out << YAML::EndSeq;
            if (!instrument.sheetParameter.empty())
            {
                out << YAML::Key << "sheet" << YAML::Value << instrument.sheetParameter;
            }
            out << YAML::Key << "start" << YAML::Value << instrument.start;
            out << YAML::Key << "length" << YAML::Value << instrument.length;
            out << YAML::Key << "looping" << YAML::Value << instrument.looping;
            out << YAML::Key << "volume_db" << YAML::Value << instrument.volumeDb;
            out << YAML::Key << "pitch_semitones" << YAML::Value << instrument.pitchSemitones;
            if (!instrument.autopitchParameter.empty())
            {
                out << YAML::Key << "autopitch" << YAML::Value << YAML::Flow << YAML::BeginMap;
                out << YAML::Key << "parameter" << YAML::Value << instrument.autopitchParameter;
                out << YAML::Key << "reference" << YAML::Value << instrument.autopitchReference;
                out << YAML::Key << "minimum" << YAML::Value << instrument.autopitchMinimum << YAML::EndMap;
            }
            out << YAML::Key << "bus" << YAML::Value << instrument.bus;
            EmitAutomations(out, instrument.automations);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    if (!out.good())
    {
        error = out.GetLastError();
        return false;
    }
    std::string text = "# A car's sounds, read from its FMOD bank (engine/audio/fmod_bank.h); played by SoundEventInstance.\n";
    text += out.c_str();
    text += '\n';
    return AtomicFile::Write(path, text, &error);
}

std::optional<SoundBank> LoadSoundBank(const std::filesystem::path& path, std::string& error)
{
    try
    {
        const YAML::Node root = YAML::LoadFile(path.string());
        SoundBank bank;
        for (const YAML::Node& eventNode : root["events"])
        {
            SoundEventDesc event;
            event.name = eventNode["name"].as<std::string>();
            event.timelineLoops = eventNode["timeline_loops"].as<bool>(false);
            for (const YAML::Node& node : eventNode["parameters"])
            {
                SoundParameter parameter;
                parameter.name = node["name"].as<std::string>();
                parameter.minimum = node["min"].as<float>(0.0f);
                parameter.maximum = node["max"].as<float>(1.0f);
                parameter.defaultValue = node["default"].as<float>(parameter.minimum);
                parameter.seekSpeed = node["seek_speed"].as<float>(0.0f);
                event.parameters.push_back(std::move(parameter));
            }
            for (const YAML::Node& node : eventNode["buses"])
            {
                SoundBus bus;
                bus.name = node["name"].as<std::string>("");
                bus.parent = node["parent"].as<int>(-1);
                bus.volumeDb = node["volume_db"].as<float>(0.0f);
                bus.automations = ReadAutomations(node);
                event.buses.push_back(std::move(bus));
            }
            for (const YAML::Node& node : eventNode["instruments"])
            {
                SoundInstrument instrument;
                for (const YAML::Node& clipNode : node["clips"])
                {
                    instrument.clips.push_back({clipNode["file"].as<std::string>(), clipNode["volume_db"].as<float>(0.0f)});
                }
                instrument.sheetParameter = node["sheet"].as<std::string>("");
                instrument.start = node["start"].as<float>(0.0f);
                instrument.length = node["length"].as<float>(0.0f);
                instrument.looping = node["looping"].as<bool>(false);
                instrument.volumeDb = node["volume_db"].as<float>(0.0f);
                instrument.pitchSemitones = node["pitch_semitones"].as<float>(0.0f);
                if (const YAML::Node autopitch = node["autopitch"])
                {
                    instrument.autopitchParameter = autopitch["parameter"].as<std::string>();
                    instrument.autopitchReference = autopitch["reference"].as<float>(1.0f);
                    instrument.autopitchMinimum = autopitch["minimum"].as<float>(0.0f);
                }
                instrument.bus = node["bus"].as<int>(-1);
                instrument.automations = ReadAutomations(node);
                if (instrument.bus >= static_cast<int>(event.buses.size()))
                {
                    error = "'" + event.name + "': an instrument names bus " + std::to_string(instrument.bus) + " of " + std::to_string(event.buses.size());
                    return std::nullopt;
                }
                event.instruments.push_back(std::move(instrument));
            }
            for (const SoundBus& bus : event.buses)
            {
                if (bus.parent >= static_cast<int>(event.buses.size()))
                {
                    error = "'" + event.name + "': bus '" + bus.name + "' has no parent " + std::to_string(bus.parent);
                    return std::nullopt;
                }
            }
            bank.events.push_back(std::move(event));
        }
        return bank;
    }
    catch (const std::exception& exception)
    {
        error = "Cannot read '" + path.string() + "': " + exception.what();
        return std::nullopt;
    }
}

}
