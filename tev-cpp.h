#pragma once

#include <functional>
#include <cstdint>

class Tev
{
public:
    Tev();
    ~Tev();

    Tev(const Tev&) = delete;
    Tev& operator=(const Tev&) = delete;
    Tev(Tev&&) = delete;
    Tev& operator=(Tev&&) = delete;

    void MainLoop();

    typedef std::uint32_t TimeoutHandle;
    TimeoutHandle SetTimeout(std::function<void()> callback, std::int64_t timeoutMs);
    void ClearTimeout(TimeoutHandle handle);
    
    void SetReadHandler(int fd, std::function<void()> callback);
    void SetWriteHandler(int fd, std::function<void()> callback);

private:
    class Impl;
    Impl *_impl;
};

