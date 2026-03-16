#pragma once

class ILogicLayer
{
public:
    virtual ~ILogicLayer() = default;
};

class IEditorLogicLayer : public ILogicLayer
{
public:
    ~IEditorLogicLayer() override = default;
};

class IGameLogicLayer : public ILogicLayer
{
public:
    ~IGameLogicLayer() override = default;
};
