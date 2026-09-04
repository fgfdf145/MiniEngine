#pragma once

namespace me
{

class IApplication
{
  public:
    virtual ~IApplication() = default;
    virtual int Run() = 0;
};
}
