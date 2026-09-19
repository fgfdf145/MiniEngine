#include <engine/renderer/motion_history.h>

#include <glm/ext/matrix_transform.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

glm::mat4 Translation(float x)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

void FirstFrameReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> models = {Translation(1.0f)};

    const MotionFrame frame = history.Advance(Translation(5.0f), keys, models);

    Require(frame.previousViewProjection == Translation(5.0f), "the first frame must report the current camera");
    Require(frame.previousModels.size() == 1, "one previous model per draw");
    Require(frame.previousModels[0] == Translation(1.0f), "the first frame must report the current model");
}

void SecondFrameReportsLastFrame()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> second = {Translation(2.0f)};

    history.Advance(Translation(5.0f), keys, first);
    const MotionFrame frame = history.Advance(Translation(6.0f), keys, second);

    Require(frame.previousViewProjection == Translation(5.0f), "the camera must report last frame");
    Require(frame.previousModels[0] == Translation(1.0f), "a known key must report last frame's model");
}

void NewKeyReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> firstKeys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> firstModels = {Translation(1.0f)};
    const std::array<MotionKey, 2> secondKeys = {MotionKey{7, 0}, MotionKey{8, 0}};
    const std::array<glm::mat4, 2> secondModels = {Translation(2.0f), Translation(9.0f)};

    history.Advance(glm::mat4(1.0f), firstKeys, firstModels);
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), secondKeys, secondModels);

    Require(frame.previousModels[0] == Translation(1.0f), "the known key keeps its history");
    Require(frame.previousModels[1] == Translation(9.0f), "a new key must report its current model");
}

void ReorderedKeysKeepTheirOwnHistory()
{
    MotionHistory history;
    const std::array<MotionKey, 2> firstKeys = {MotionKey{1, 0}, MotionKey{1, 1}};
    const std::array<glm::mat4, 2> firstModels = {Translation(1.0f), Translation(2.0f)};
    const std::array<MotionKey, 2> secondKeys = {MotionKey{1, 1}, MotionKey{1, 0}};
    const std::array<glm::mat4, 2> secondModels = {Translation(20.0f), Translation(10.0f)};

    history.Advance(glm::mat4(1.0f), firstKeys, firstModels);
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), secondKeys, secondModels);

    Require(frame.previousModels[0] == Translation(2.0f), "slot 0 now holds submesh 1 and must get its history");
    Require(frame.previousModels[1] == Translation(1.0f), "slot 1 now holds submesh 0 and must get its history");
}

void RemovedKeyIsForgotten()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{3, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> third = {Translation(3.0f)};

    history.Advance(glm::mat4(1.0f), keys, first);
    history.Advance(glm::mat4(1.0f), {}, {});
    const MotionFrame frame = history.Advance(glm::mat4(1.0f), keys, third);

    Require(frame.previousModels[0] == Translation(3.0f), "a key absent for a frame must lose its history");
}

void ResetReportsNoMotion()
{
    MotionHistory history;
    const std::array<MotionKey, 1> keys = {MotionKey{7, 0}};
    const std::array<glm::mat4, 1> first = {Translation(1.0f)};
    const std::array<glm::mat4, 1> second = {Translation(2.0f)};

    history.Advance(Translation(5.0f), keys, first);
    history.Reset();
    const MotionFrame frame = history.Advance(Translation(6.0f), keys, second);

    Require(frame.previousViewProjection == Translation(6.0f), "after Reset the camera must report no motion");
    Require(frame.previousModels[0] == Translation(2.0f), "after Reset every draw must report no motion");
}

void DuplicateKeysAreRejected()
{
    MotionHistory history;
    const std::array<MotionKey, 2> keys = {MotionKey{7, 0}, MotionKey{7, 0}};
    const std::array<glm::mat4, 2> models = {Translation(1.0f), Translation(2.0f)};

    bool threw = false;
    try
    {
        history.Advance(glm::mat4(1.0f), keys, models);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Require(threw, "duplicate keys must throw std::invalid_argument");
}

void MismatchedSpansAreRejected()
{
    MotionHistory history;
    const std::array<MotionKey, 2> keys = {MotionKey{7, 0}, MotionKey{7, 1}};
    const std::array<glm::mat4, 1> models = {Translation(1.0f)};

    bool threw = false;
    try
    {
        history.Advance(glm::mat4(1.0f), keys, models);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Require(threw, "keys and models of different lengths must throw std::invalid_argument");
}
}

int main()
{
    try
    {
        FirstFrameReportsNoMotion();
        SecondFrameReportsLastFrame();
        NewKeyReportsNoMotion();
        ReorderedKeysKeepTheirOwnHistory();
        RemovedKeyIsForgotten();
        ResetReportsNoMotion();
        DuplicateKeysAreRejected();
        MismatchedSpansAreRejected();
    }
    catch (const std::exception& error)
    {
        std::cerr << "motion history tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "motion history tests passed\n";
    return 0;
}
