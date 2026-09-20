#pragma once
#include <cstdio>
#include <cstdarg>
#include <windows.h>
#include <string>

// Logging.h
// Central logging helper so every part of the mod writes to the same file
// the same way, instead of repeating fopen_s/fprintf/fclose everywhere.

// Set to false before shipping a release build -- silences all logging
// and disables the debug dump, so the mod doesn't write files to disk
// during normal play.
constexpr bool IsDebugModeOn = false;

// BUG FIX: "mm7loc_log.txt" used to be a bare relative path, dependent
// on the process's working directory (which might not be the game's
// folder, depending on how Steam/the launcher starts the process --
// the same problem we'd already solved before for JapaneseStrings.json).
// Resolved once, anchored to the DLL's OWN folder.
inline const std::string& GetLogFilePath()
{
    static std::string path = [] {
        char dllPath[MAX_PATH] = { 0 };
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetLogFilePath), &hSelf);
        GetModuleFileNameA(hSelf, dllPath, sizeof(dllPath));

        std::string result = dllPath;
        size_t lastSlash = result.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            result = result.substr(0, lastSlash + 1) + "mm7loc_log.txt";
        }
        else {
            result = "mm7loc_log.txt"; // fallback, shouldn't happen
        }
        return result;
        }();
    return path;
}

// Appends a line to the log. Use this for normal, ongoing logging.
inline void LogLine(const char* fmt, ...)
{
    if (!IsDebugModeOn) return;

    FILE* f = nullptr;
    fopen_s(&f, GetLogFilePath().c_str(), "a");
    if (!f) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

// Sends one line to the debugger output (DebugView / an attached
// debugger). Same on/off switch as LogLine: does nothing when
// IsDebugModeOn is false. Use this instead of calling OutputDebugStringA
// directly, so the switch covers every diagnostic channel.
inline void DebugOut(const char* msg)
{
    if (!IsDebugModeOn) return;
    OutputDebugStringA(msg);
}

// Overwrites the log instead of appending. Call this once, at the very
// start of MainThread, so each run starts with a clean log file.
inline void LogLineReset(const char* fmt, ...)
{
    if (!IsDebugModeOn) return;

    FILE* f = nullptr;
    fopen_s(&f, GetLogFilePath().c_str(), "w");
    if (!f) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}