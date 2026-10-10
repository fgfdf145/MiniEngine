#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// A car's sounds as FMOD Studio arranged them (read from an Assetto Corsa .bank by ReadFmodBank), kept
// as plain data: an event per sound (the engine heard from outside, a gear change, the wind...), driven by
// named parameters (rpms, throttle...) that place its instruments and automate their buses. Saved as
// YAML next to the sound files (SaveSoundBank) and played by SoundEventInstance.

// A point of an automation curve. Between two points the value runs from this one's y to the next one's
// by shape: 0 is a straight line, positive holds y longer before it moves, negative moves sooner.
struct SoundCurvePoint
{
    float x = 0.0f;
    float y = 0.0f;
    float shape = 0.0f;
};

// What an automation changes.
enum class SoundProperty
{
    // Decibels, added together.
    Volume,
    // Semitones, added together.
    Pitch,
    // A linear gain from 0 to 1 (an instrument's crossfade with its neighbours), multiplied together.
    Fade,
};

// A property of a bus or an instrument following a parameter: the curve gives a value at the parameter's
// value, and the mapping (pairs of curve value and property value, in order) turns it into the property's
// own units. An empty mapping takes the curve's value as it is.
struct SoundAutomation
{
    std::string parameter;
    SoundProperty property = SoundProperty::Volume;
    std::vector<SoundCurvePoint> curve;
    std::vector<std::pair<float, float>> mapping;
};

// A mixing track inside an event; its volume and automations apply to everything routed through it and,
// through its parent, the parent's on top. The event's master bus has no parent.
struct SoundBus
{
    std::string name; // FMOD's id, for reading the YAML by eye
    int parent = -1;
    float volumeDb = 0.0f;
    std::vector<SoundAutomation> automations;
};

// One of an instrument's sound files: a multi instrument picks one of several each time it starts.
struct SoundClip
{
    std::string file; // relative to the bank's YAML
    float volumeDb = 0.0f;
};

// A sound placed in an event: on a parameter sheet (sheetParameter) it plays while the parameter is
// between start and start + length; on the event's timeline (sheetParameter empty) it plays from start
// seconds after the event starts.
struct SoundInstrument
{
    std::vector<SoundClip> clips;
    std::string sheetParameter;
    float start = 0.0f;
    float length = 0.0f;
    bool looping = false;
    float volumeDb = 0.0f;
    float pitchSemitones = 0.0f;
    // Auto pitch: plays at its own pitch with the parameter at the reference value, at `autopitchMinimum`
    // of it with the parameter at its minimum, and in proportion between (and beyond) the two.
    std::string autopitchParameter;
    float autopitchReference = 1.0f;
    float autopitchMinimum = 0.0f;
    int bus = -1;
    std::vector<SoundAutomation> automations;
};

struct SoundParameter
{
    std::string name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    // How fast the value follows what it is set to, in parameter units a second; 0 is at once.
    float seekSpeed = 0.0f;
};

struct SoundEventDesc
{
    std::string name;
    std::vector<SoundParameter> parameters;
    std::vector<SoundBus> buses;
    std::vector<SoundInstrument> instruments;
    // The timeline loops (its sounds play on until the event is stopped).
    bool timelineLoops = false;

    const SoundParameter* FindParameter(std::string_view parameterName) const;
};

struct SoundBank
{
    std::vector<SoundEventDesc> events;

    const SoundEventDesc* Find(std::string_view eventName) const;
};

// The curve's value at x: its first point's y before it, its last point's after it.
float EvaluateSoundCurve(const std::vector<SoundCurvePoint>& curve, float x);
// The mapping's value at v (piecewise linear, held past its ends); v itself when the mapping is empty.
float EvaluateSoundMapping(const std::vector<std::pair<float, float>>& mapping, float v);

// `<stem>.sounds.yaml`, the bank a model's sounds are kept in next to it.
std::filesystem::path SoundBankPathForModel(const std::filesystem::path& modelPath);

bool SaveSoundBank(const std::filesystem::path& path, const SoundBank& bank, std::string& error);
std::optional<SoundBank> LoadSoundBank(const std::filesystem::path& path, std::string& error);

}
