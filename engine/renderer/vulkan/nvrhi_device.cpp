#include "nvrhi_device.h"

#include "device.h"
#include "instance.h"

#include <engine/core/log/log.h>

#include <nvrhi/validation.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
bool ValidationRequested()
{
    const char* value = std::getenv("MINIENGINE_NVRHI_VALIDATION");
    return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != 0;
}

std::vector<const char*> Names(const std::vector<std::string>& names)
{
    std::vector<const char*> pointers;
    pointers.reserve(names.size());
    for (const std::string& name : names)
    {
        pointers.push_back(name.c_str());
    }
    return pointers;
}
}

void NvrhiDevice::MessageLog::message(nvrhi::MessageSeverity severity, const char* messageText)
{
    switch (severity)
    {
    case nvrhi::MessageSeverity::Info:
        LOG_INFO("[nvrhi] {}", messageText);
        break;
    case nvrhi::MessageSeverity::Warning:
        LOG_WARN("[nvrhi] {}", messageText);
        break;
    case nvrhi::MessageSeverity::Error:
    case nvrhi::MessageSeverity::Fatal:
        LOG_ERROR("[nvrhi] {}", messageText);
        break;
    }
}

NvrhiDevice::NvrhiDevice(const VulkanInstance& instance, const VulkanDevice& device)
{
    std::vector<const char*> instanceExtensions = Names(instance.GetEnabledExtensions());
    std::vector<const char*> deviceExtensions = Names(device.GetEnabledExtensions());

    nvrhi::vulkan::DeviceDesc desc;
    desc.errorCB = &m_messageLog;
    desc.instance = instance.GetHandle();
    desc.physicalDevice = device.GetPhysicalDevice();
    desc.device = device.GetHandle();
    desc.graphicsQueue = device.GetGraphicsQueue();
    desc.graphicsQueueIndex = static_cast<int>(device.GetQueueFamilies().graphicsFamily.value());
    desc.instanceExtensions = instanceExtensions.data();
    desc.numInstanceExtensions = instanceExtensions.size();
    desc.deviceExtensions = deviceExtensions.data();
    desc.numDeviceExtensions = deviceExtensions.size();
    desc.bufferDeviceAddressSupported = device.BufferDeviceAddressEnabled();

    m_vulkanDevice = nvrhi::vulkan::createDevice(desc);
    if (!m_vulkanDevice)
    {
        throw std::runtime_error("Failed to create the NVRHI device");
    }
    m_device = m_vulkanDevice;
    if (ValidationRequested())
    {
        m_device = nvrhi::validation::createValidationLayer(m_vulkanDevice);
        LOG_INFO("NVRHI device created (validation layer on)");
    }
    else
    {
        LOG_INFO("NVRHI device created");
    }
}

NvrhiDevice::~NvrhiDevice()
{
    if (m_device)
    {
        m_device->waitForIdle();
        m_device->runGarbageCollection();
    }
}
}
