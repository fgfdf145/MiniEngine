#pragma once

class IApplication
{
public:
    virtual ~IApplication() = default;
    virtual int Run() = 0;
};
