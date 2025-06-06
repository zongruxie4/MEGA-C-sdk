#pragma once
#include "mega/gfx/worker/comms.h"
#include "mega/types.h"

#include <chrono>

namespace mega
{
namespace gfx {
namespace win_utils
{

std::wstring toFullPipeName(const std::string& name);

}

class NamedPipe : public IEndpoint
{
public:
    NamedPipe(HANDLE h, const std::string& name) : mPipeHandle(h), mName(name) {}

    NamedPipe(const NamedPipe&) = delete;

    NamedPipe(NamedPipe&& other);

    ~NamedPipe();

    bool isValid() const { return mPipeHandle != INVALID_HANDLE_VALUE; }
protected:
    enum class Type
    {
        Client,
        Server
    };

    HANDLE mPipeHandle;

    std::string mName;

private:
    bool doWrite(const void* data, size_t n, std::chrono::milliseconds timeout) override;

    bool doRead(void* data, size_t n, std::chrono::milliseconds timeout) override;

    //
    // The common part of doing an overlapped I/O.
    //
    // Overlapped I/O is a name used for asynchoruous I/O in the Windows API.
    // See https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-server-using-overlapped-i-o
    //
    bool doOverlappedOperation(std::function<bool(OVERLAPPED*)> op,
                               std::chrono::milliseconds timeout,
                               const std::string& opStr);

    virtual Type type() const = 0;
};

class WinOverlapped final
{
public:
    WinOverlapped();
    ~WinOverlapped();

    OVERLAPPED* data();

    bool isValid() const
    {
        return mOverlapped.hEvent != NULL;
    };

    // Return an error code and error string on error
    std::pair<std::error_code, std::string> waitForCompletion(DWORD mWaitMs);

private:
    OVERLAPPED mOverlapped;
};

} // namespace
}
