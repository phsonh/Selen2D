#include "Log.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace core::runtime::Log
{
    namespace
    {
        bool Initialized = false;

        std::ofstream LogFile;

        constexpr const char* COLOR_RESET =
            "\x1b[0m";

        constexpr const char* COLOR_YELLOW =
            "\x1b[33m";

        constexpr const char* COLOR_RED =
            "\x1b[31m";


#ifdef _WIN32
        void EnableVirtualTerminal(
            DWORD outputHandle
        )
        {
            HANDLE handle =
                GetStdHandle(outputHandle);

            if (
                handle == INVALID_HANDLE_VALUE ||
                handle == nullptr
                )
            {
                return;
            }

            DWORD mode = 0;

            if (!GetConsoleMode(handle, &mode))
                return;

            SetConsoleMode(
                handle,
                mode |
                ENABLE_VIRTUAL_TERMINAL_PROCESSING
            );
        }


        void EnableConsoleColor()
        {
            EnableVirtualTerminal(
                STD_OUTPUT_HANDLE
            );

            EnableVirtualTerminal(
                STD_ERROR_HANDLE
            );
        }
#endif


        std::string GetTimestamp()
        {
            using namespace std::chrono;

            auto now =
                system_clock::now();

            auto time =
                system_clock::to_time_t(now);

            std::tm localTime{};

#ifdef _WIN32
            localtime_s(
                &localTime,
                &time
            );
#else
            localtime_r(
                &time,
                &localTime
            );
#endif

            auto milliseconds =
                duration_cast<
                std::chrono::milliseconds
                >(
                    now.time_since_epoch()
                ) % 1000;

            std::ostringstream stream;

            stream
                << std::put_time(
                    &localTime,
                    "%Y-%m-%d %H:%M:%S"
                )
                << '.'
                << std::setfill('0')
                << std::setw(3)
                << milliseconds.count();

            return stream.str();
        }


        void Write(
            const char* level,
            std::string_view message,
            std::ostream& console,
            const char* color = nullptr
        )
        {
            const std::string timestamp =
                GetTimestamp();

            // 控制台
            if (color)
                console << color;

            console
                << '['
                << timestamp
                << "] ["
                << level
                << "] "
                << message;

            if (color)
                console << COLOR_RESET;

            console << '\n';


            // engine.log 不写颜色控制字符
            if (LogFile.is_open())
            {
                LogFile
                    << '['
                    << timestamp
                    << "] ["
                    << level
                    << "] "
                    << message
                    << '\n';

                LogFile.flush();
            }
        }
    }


    bool Init()
    {
        if (Initialized)
            return true;

#ifdef _WIN32
        EnableConsoleColor();
#endif

        LogFile.open(
            "engine.log",
            std::ios::out |
            std::ios::trunc
        );

        Initialized = true;

        if (!LogFile.is_open())
        {
            Warn(
                "Failed to open engine.log; file logging disabled for this run"
            );
        }
        else
        {
            Info(
                "File logging enabled: engine.log"
            );
        }

        Info(
            "Log runtime initialized"
        );

        return true;
    }


    void Shutdown()
    {
        if (!Initialized)
            return;

        Info(
            "Log runtime shutdown"
        );

        if (LogFile.is_open())
        {
            LogFile.flush();
            LogFile.close();
        }

        Initialized = false;
    }


    void Debug(
        std::string_view message
    )
    {
        Write(
            "DEBUG",
            message,
            std::cout
        );
    }


    void Info(
        std::string_view message
    )
    {
        Write(
            "INFO ",
            message,
            std::cout
        );
    }


    void Warn(
        std::string_view message
    )
    {
        Write(
            "WARN ",
            message,
            std::cerr,
            COLOR_YELLOW
        );
    }


    void Error(
        std::string_view message
    )
    {
        Write(
            "ERROR",
            message,
            std::cerr,
            COLOR_RED
        );
    }


    void Fatal(
        std::string_view message
    )
    {
        Write(
            "FATAL",
            message,
            std::cerr,
            COLOR_RED
        );
    }
}