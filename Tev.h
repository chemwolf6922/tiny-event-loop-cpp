#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <map>
#include <queue>

class Tev
{
public:
    class Timeout
    {
    public:
        friend class Tev;
        Timeout();
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
        bool _cleared{false};

        explicit Timeout(std::function<void()> clearFunc);
    };
    class FdHandler
    {
    public:
        friend class Tev;
        FdHandler();
        ~FdHandler();
        FdHandler(const FdHandler&) = delete;
        FdHandler& operator=(const FdHandler&) = delete;
        FdHandler(FdHandler&&) noexcept;
        FdHandler& operator=(FdHandler&&);
        bool operator==(std::nullptr_t) const;
        /** Clear an already cleared fd handler is fine */
        void Clear();
    private:
        std::function<void()> _clearFunc;
        int _fd{-1};
        bool _isRead{false};
        bool _cleared{false};

        FdHandler(std::function<void()> clearFunc, int fd, bool isRead);
    };

    Tev();
    ~Tev() = default;

    Tev(const Tev&) = delete;
    Tev& operator=(const Tev&) = delete;
    Tev(Tev&&) = delete;
    Tev& operator=(Tev&&) = delete;

    void MainLoop();

    Timeout SetTimeout(std::function<void()> callback, std::int64_t timeoutMs);

    /** 
     * Keep good track of your fd handlers.
     * Try to avoid keeping multiple read/write handlers of the same fd.
     * For all of those will point to the same resource and may clear it at unexpected times.
     */
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

    /** This must be 1 to allow safely removal of fd handlers inside a fd handler */
    static constexpr int TEV_MAX_EPOLL_EVENTS = 1;

    UniqueFd _epollFd;
    std::unordered_map<int, FdHandlerImpl> _fdHandlers{};
    bool _fdHandlerFreedInReadHandler{false};
    std::map<std::pair<Timestamp, TimeoutHandle>, std::function<void()>> _timers{};
    std::unordered_map<TimeoutHandle, Timestamp> _timerIndex{};
    TimeoutHandle _timeoutHandleSeed{0};
    std::queue<std::function<void()>> _nextCycleCallbacks{};

    Timestamp GetTimestamp();
    void ClearTimeout(TimeoutHandle handle);
    void SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead, bool prohibitNullCallback = true);
};
