#pragma once

#include <string_view>

namespace core::runtime::Log
{
    bool Init();
    void Shutdown();

    void Debug(std::string_view message);
    void Info(std::string_view message);
    void Warn(std::string_view message);
    void Error(std::string_view message);
    void Fatal(std::string_view message);
}