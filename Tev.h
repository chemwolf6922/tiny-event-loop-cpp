#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <map>

class Tev
{
public:
    class Timeout
    {
    public:
        friend class Tev;
        ~Timeout();
        Timeout(const Timeout&) = delete;
        Timeout& operator=(const Timeout&) = delete;
        Timeout(Timeout&&) noexcept;
        Timeout& operator=(Timeout&&);
        /** Triggering the timeout will not set this to true. */
        bool operator==(std::nullptr_t) const;
        /** Clear an already cleared/triggered timeout is fine. */
        void Clear();
    private:
        std::function<void()> _clearFunc;
        Timeout(std::function<void()> clearFunc);
        bool _cleared{false};
    };
    /** They are the same thing. */
    using FdHandler = Timeout;

    Tev();
    ~Tev() = default;

    Tev(const Tev&) = delete;
    Tev& operator=(const Tev&) = delete;
    Tev(Tev&&) = delete;
    Tev& operator=(Tev&&) = delete;

    void MainLoop();

    Timeout SetTimeout(std::function<void()> callback, std::int64_t timeoutMs);

    FdHandler SetReadHandler(int fd, std::function<void()> callback);
    FdHandler SetWriteHandler(int fd, std::function<void()> callback);

    void RunInNextCycle(std::function<void()> callback);

private:
    using Timestamp = std::int64_t;
    using TimeoutHandle = std::uint64_t;
    class UniqueFd
    {
    public:
        explicit UniqueFd(int fd);
        ~UniqueFd();
        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;
        UniqueFd(UniqueFd&& other) noexcept;
        UniqueFd& operator=(UniqueFd&& other) noexcept;
        void Close();
        explicit operator int() const;
    private:
        int _fd;
    };
    struct FdHandlerImpl
    {
        std::function<void()> readHandler{nullptr};
        std::function<void()> writeHandler{nullptr};
    };
    struct TimeoutImpl
    {
        std::function<void()> callback{nullptr};
    };

    /** This must be 1 to allow safely removal of fd handlers inside a fd handler */
    static constexpr int TEV_MAX_EPOLL_EVENTS = 1;

    UniqueFd _epollFd;
    std::unordered_map<int, FdHandlerImpl> _fdHandlers{};
    std::map<std::pair<Timestamp, TimeoutHandle>, TimeoutImpl> _timers{};
    std::unordered_map<TimeoutHandle, Timestamp> _timerIndex{};
    bool _fdHandlerFreedInReadHandler{false};
    TimeoutHandle _timeoutHandleSeed{0};

    Timestamp GetTimestamp();
    void ClearTimeout(TimeoutHandle handle);
    void SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead, bool prohibitNullCallback = true);
};
