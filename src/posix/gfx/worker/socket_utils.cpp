#include "mega/posix/gfx/worker/socket_utils.h"
#include "mega/logging.h"
#include "mega/clock.h"
#include <sys/un.h>

#include <filesystem>
#include <poll.h>

#include <chrono>
#include <system_error>
#include <unistd.h>

using std::chrono::milliseconds;
using std::error_code;
using std::chrono::duration_cast;
using std::system_category;

namespace fs = std::filesystem;

namespace mega {
namespace gfx {

bool SocketUtils::isRetryErrorNo(int errorNo)
{
    return errorNo == EAGAIN || errorNo == EWOULDBLOCK || errorNo == EINTR;
}

fs::path SocketUtils::toSocketPath(const std::string& name)
{
    return fs::path{"/tmp"} / ("MegaLimited" + std::to_string(getuid()))  / name;
}

bool SocketUtils::isPollError(int event)
{
    return event & (POLLERR | POLLHUP | POLLNVAL);
}

error_code SocketUtils::poll(std::vector<struct pollfd> fds, milliseconds timeout)
{
    // Remaining timeout in case of EINTR
    const ScopedSteadyClock clock;
    milliseconds remaining{timeout};

    int ret = 0;
    do
    {
        ret = ::poll(fds.data(), fds.size(), static_cast<int>(timeout.count()));
        remaining -= duration_cast<milliseconds>(clock.passedTime());
    } while (ret < 0 && errno == EINTR && remaining > milliseconds{0});

    if (ret < 0)
    {
        LOG_err << "Error in poll: " << errno;
        return error_code{errno, system_category()};
    }
    else if (ret == 0)
    {
        return error_code{ETIMEDOUT, system_category()};
    }

    return error_code{};
}

error_code SocketUtils::pollFd(int fd, short events, milliseconds timeout)
{
    // Poll
    std::vector<struct pollfd> fds{
        {.fd = fd, .events = events, .revents = 0}
    };

    if (auto errorCode = poll(fds, timeout); errorCode)
    {
        return errorCode;
    }

    // check if the poll returns an error event
    auto& polledFd = fds[0];
    if (isPollError(polledFd.revents))
    {
        return error_code{ECONNABORTED, system_category()};
    }

    return error_code{};
}

error_code SocketUtils::pollForRead(int fd, milliseconds timeout)
{
    return pollFd(fd, POLLIN, timeout);
}

error_code SocketUtils::pollForWrite(int fd, milliseconds timeout)
{
    return pollFd(fd, POLLOUT, timeout);
}

error_code SocketUtils::pollForAccept(int fd, milliseconds timeout)
{
    return pollFd(fd, POLLIN, timeout);
}

std::pair<error_code, int> SocketUtils::accept(int listeningFd, milliseconds timeout)
{
    do {
        auto errorCode = pollForAccept(listeningFd, timeout);
        if (errorCode)
        {
            return {errorCode, -1};   // error
        }

        auto dataSocket = ::accept(listeningFd, nullptr, nullptr);
        if (dataSocket < 0 && isRetryErrorNo(errno))
        {
            LOG_info << "Retry accept due to errno: " << errno; // retry
            continue;
        }
        else if (dataSocket < 0)
        {
            return {error_code{errno, system_category()}, -1};  // error
        }
        else
        {
            return {error_code{}, dataSocket};                  // success
        }
    } while (true);
}

error_code SocketUtils::write(int fd, const void* data, size_t n, milliseconds timeout)
{
    size_t offset = 0;
    while (offset < n) {
        // Poll
        if (auto errorCode = pollForWrite(fd, timeout); errorCode)
        {
            return errorCode;
        }

        // Write
        size_t remaining = n - offset;
        ssize_t written = ::write(fd, static_cast<const char *>(data) + offset, remaining);
        if (written < 0 && isRetryErrorNo(errno))
        {
            continue;                                    // retry
        }
        else if (written < 0)
        {
            return error_code{errno, system_category()}; // error
        }
        else
        {
            offset += static_cast<size_t>(written);      // success
        }
    }

    return error_code{};
}

//
// Read until count bytes or timeout
//
error_code SocketUtils::read(int fd, void* buf, size_t count, milliseconds timeout)
{
    size_t offset = 0;
    while (offset < count) {
        // Poll
        if (auto errorCode = pollForRead(fd, timeout); errorCode)
        {
            return errorCode;
        }

        // Read
        size_t remaining = count - offset;
        ssize_t bytesRead = ::read(fd, static_cast<char *>(buf) + offset, remaining);

        if (bytesRead < 0 && isRetryErrorNo(errno))
        {
            continue;                                    // retry
        }
        else if (bytesRead < 0)
        {
            return error_code{errno, system_category()}; // error
        }
        else if (bytesRead == 0 && offset < count)
        {
            return error_code{ECONNABORTED, system_category()}; // end of file and not read all needed
        }
        else
        {
            offset += static_cast<size_t>(bytesRead);      // success
        }
    }

    // Success
    return error_code{};
}

error_code SocketUtils::doBindAndListen(int fd, const std::string& fullPath)
{
    constexpr int MAX_QUEUE_LEN = 10;

    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, fullPath.c_str(), maxSocketPathLength());

    // Bind name
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&un), sizeof(un)) == -1)
    {
        LOG_err << "fail to bind UNIX domain socket name: " << fullPath << " errno: " << errno;
        return error_code{errno, system_category()};
    }

    // Listen
    if (::listen(fd, MAX_QUEUE_LEN) < 0)
    {
        LOG_err << "fail to listen UNIX domain socket name: " << fullPath << " errno: " << errno;
        return error_code{errno, system_category()};
    }

    // Success
    return error_code{};
}

constexpr size_t SocketUtils::maxSocketPathLength()
{
    return sizeof(sockaddr_un::sun_path) - 1;
}

std::pair<error_code, int> SocketUtils::listen(const std::string& name)
{
    const auto socketPath = SocketUtils::toSocketPath(name);

    // Check name, extra 1 for null terminated
    if (strlen(socketPath.c_str()) >= maxSocketPathLength())
    {
        LOG_err << "unix domain socket name is too long, " << socketPath.string();
        return {error_code{ENAMETOOLONG, system_category()}, -1};
    }

    // The name might exist
    // fail to unlink is not an error: such as not exists as for most cases
    if (::unlink(socketPath.c_str()) < 0)
    {
        LOG_info << "fail to unlink: " << socketPath.string() << " errno: " << errno;
    }

    // Create path
    std::error_code errorCode;
    fs::create_directories(socketPath.parent_path(), errorCode);

    // Create a UNIX domain socket
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_err << "fail to create a UNIX domain socket: " << name << " errno: " << errno;
        return {error_code{errno, system_category()}, -1};
    }

    // Bind and Listen on
    if (auto error_code = doBindAndListen(fd, socketPath.string()))
    {
        ::close(fd);
        return { error_code, -1};
    }

    // Success
    LOG_verbose << "listening on UNIX domain socket name: " << socketPath.string();

    return {error_code{}, fd};;
}

} // end of namespace
}