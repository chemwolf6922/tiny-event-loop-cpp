#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <map>
#include <queue>
#include <chrono>
#include <sys/epoll.h>
#include <unistd.h>

class Tev
{
public:
    class Timeout
    {
    public:
        friend class Tev;

        Timeout()
            : _clearFunc{nullptr}, _isValidFunc{nullptr}
        {
        }

        ~Timeout()
        {
            try
            {
                Clear();
            }
            catch(...)
            {
                /** Ignore errors in destructor */
            }
        }

        Timeout(const Timeout&) = delete;
        Timeout& operator=(const Timeout&) = delete;

        Timeout(Timeout&& other) noexcept
            : _clearFunc(std::move(other._clearFunc)), _isValidFunc(std::move(other._isValidFunc))
        {
        }

        Timeout& operator=(Timeout&& other)
        {
            if (this != &other)
            {
                Clear();
                _clearFunc = std::move(other._clearFunc);
                _isValidFunc = std::move(other._isValidFunc);
            }
            return *this;
        }

        /** Triggering the timeout will not set this to true. */
        bool operator==(std::nullptr_t) const
        {
            if (!_isValidFunc)
            {
                return true;
            }
            return !_isValidFunc();
        }

        /** Clear an already cleared/triggered timeout is fine. */
        void Clear()
        {
            if (_clearFunc)
            {
                auto clearFunc = std::move(_clearFunc);
                _clearFunc = nullptr;
                clearFunc();
            }
        }

    private:
        std::function<void()> _clearFunc;
        std::function<bool()> _isValidFunc;

        explicit Timeout(std::function<void()> clearFunc, std::function<bool()> isValidFunc)
            : _clearFunc(std::move(clearFunc)), _isValidFunc(std::move(isValidFunc))
        {
        }
    };

    class FdHandler
    {
    public:
        friend class Tev;
        FdHandler()
            : _clearFunc{nullptr}, _fd(-1), _isRead(false)
        {
        }

        ~FdHandler()
        {
            try
            {
                Clear();
            }
            catch(...)
            {
                /** Ignore errors in destructor */
            }
        }

        FdHandler(const FdHandler&) = delete;
        FdHandler& operator=(const FdHandler&) = delete;

        FdHandler(FdHandler&& other) noexcept
            : _clearFunc(std::move(other._clearFunc)), _isValidFunc(std::move(other._isValidFunc)), _fd(other._fd), _isRead(other._isRead)
        {
            other._fd = -1;
        }

        FdHandler& operator=(FdHandler&& other)
        {
            if (this != &other)
            {
                if (_fd != other._fd || _isRead != other._isRead)
                {
                    /** 
                     * If the fd and type matches. DO NOT call clear. 
                     * This is just a callback update.
                     */
                    Clear();
                }
                _clearFunc = std::move(other._clearFunc);
                _isValidFunc = std::move(other._isValidFunc);
                _fd = other._fd;
                _isRead = other._isRead;
                other._fd = -1;
            }
            return *this;
        }

        bool operator==(std::nullptr_t) const
        {
            if (!_isValidFunc)
            {
                return true;
            }
            return !_isValidFunc();
        }

        /** Clear an already cleared fd handler is fine */
        void Clear()
        {
            if (_clearFunc)
            {
                auto clearFunc = std::move(_clearFunc);
                _clearFunc = nullptr;
                clearFunc();
            }
        }

    private:
        std::function<void()> _clearFunc;
        std::function<bool()> _isValidFunc;
        int _fd{-1};
        bool _isRead{false};

        FdHandler(std::function<void()> clearFunc, std::function<bool()> isValidFunc, int fd, bool isRead)
            : _clearFunc(std::move(clearFunc)), _isValidFunc(std::move(isValidFunc)), _fd(fd), _isRead(isRead)
        {
        }
    };

    Tev()
        : _epollFd(epoll_create1(EPOLL_CLOEXEC))
    {
        if (static_cast<int>(_epollFd) < 0)
        {
            throw std::runtime_error("Failed to create epoll file descriptor");
        }
    }

    ~Tev() = default;

    Tev(const Tev&) = delete;
    Tev& operator=(const Tev&) = delete;
    Tev(Tev&&) = delete;
    Tev& operator=(Tev&&) = delete;

    void MainLoop()
    {
        int next_timeout;
        for(;;)
        {
            next_timeout = 0;
            /** Process due timers and run next callbacks */
            /** Do this instead of additional syscall */
            if (!_timers.empty() || !_nextCycleCallbacks.empty())
            {
                auto now = GetTimestamp();
                while(!_timers.empty() || !_nextCycleCallbacks.empty())
                {
                    while (!_nextCycleCallbacks.empty())
                    {
                        auto callback = std::move(_nextCycleCallbacks.front());
                        _nextCycleCallbacks.pop();
                        if(callback)
                        {
                            try
                            {
                                callback();
                            }
                            catch(...)
                            {
                                /** Ignore all error in the callback */
                            }
                        }
                    }
                    /** 
                     * At this point. Run in next cycle might have created new timers.
                     * But there are no more run in next cycle callbacks.
                     */
                    if (!_timers.empty())
                    {
                        auto item = _timers.begin();
                        if(item->first.first > now)
                        {
                            next_timeout = item->first.first - now;
                            break;
                        }
                        /** remove the item first */
                        _timerIndex.erase(item->first.second);
                        auto callback = std::move(item->second);
                        _timers.erase(item);
                        if(callback)
                        {
                            try
                            {
                                callback();
                            }
                            catch(...)
                            {
                                /** Ignore all error in the callback */
                            }
                        }
                    }
                    /**
                     * At this point. There might still be timers to execute.
                     * And there also might be new run in next cycle callbacks created in the executed timer.
                     */
                }
            }
            /** Are there files to wait for */
            if(next_timeout == 0 && _fdHandlers.size() != 0)
            {
                next_timeout = -1;
            }   
            /** Check exit condition */
            if(next_timeout == 0)
            {
                break;
            }
            /** Wait for events */
            struct epoll_event events[TEV_MAX_EPOLL_EVENTS];
            int nfds = epoll_wait(static_cast<int>(_epollFd), events, TEV_MAX_EPOLL_EVENTS, next_timeout);
            for(int i = 0; i < nfds; i++)
            {
                auto fd = events[i].data.fd;
                auto item = _fdHandlers.find(fd);
                if (item == _fdHandlers.end())
                {
                    continue;
                }
                auto& handler = item->second;
                _fdHandlerFreedInReadHandler = false;
                if(((events[i].events & EPOLLIN) || (events[i].events & EPOLLHUP)) && handler.readHandler)
                {
                    try
                    {
                        handler.readHandler();
                    }
                    catch(...)
                    {
                        /** Ignore all error in the callback */
                    }
                    
                }
                if(((events[i].events & EPOLLOUT) || (events[i].events & EPOLLHUP)) && (!_fdHandlerFreedInReadHandler) && handler.writeHandler)
                {
                    try
                    {
                        handler.writeHandler();
                    }
                    catch(...)
                    {
                        /** Ignore all error in the callback */
                    }
                }
            }
        }
    }

    Timeout SetTimeout(std::function<void()> callback, std::int64_t timeoutMs)
    {
        if(!callback)
        {
            throw std::invalid_argument("No timeout callback provided");
        }
        auto target = GetTimestamp() + timeoutMs;
        auto handle = _timeoutHandleSeed++;
        auto pair = _timerIndex.insert(std::make_pair(handle, target));
        if(!pair.second)
        {
            throw std::runtime_error("Failed to insert the timeout handle");
        }
        auto pair2 = _timers.insert(std::make_pair(std::make_pair(target, handle), std::move(callback)));
        if(!pair2.second)
        {
            _timerIndex.erase(pair.first);
            throw std::runtime_error("Failed to insert the timeout");
        }
        return Timeout{
            [this, handle]() {
                ClearTimeout(handle);
            },
            [this, handle]() -> bool {
                return _timerIndex.contains(handle);
            }
        };
    }

    /** 
     * Keep good track of your fd handlers.
     * Try to avoid keeping multiple read/write handlers of the same fd.
     * For all of those will point to the same resource and may clear it at unexpected times.
     */
    FdHandler SetReadHandler(int fd, std::function<void()> callback)
    {
        SetReadWriteHandler(fd, callback, true);
        return FdHandler{
            [this, fd]() {
                SetReadWriteHandler(fd, nullptr, true, false);
            },
            [this, fd]() -> bool {
                auto item = _fdHandlers.find(fd);
                if (item == _fdHandlers.end())
                {
                    return false;
                }
                return item->second.readHandler != nullptr;
            }, fd, true
        };
    }

    FdHandler SetWriteHandler(int fd, std::function<void()> callback)
    {
        SetReadWriteHandler(fd, callback, false);
        return FdHandler{
            [this, fd]() {
                SetReadWriteHandler(fd, nullptr, false, false);
            },
            [this, fd]() -> bool {
                auto item = _fdHandlers.find(fd);
                if (item == _fdHandlers.end())
                {
                    return false;
                }
                return item->second.writeHandler != nullptr;
            }, fd, false
        };
    }

    void RunInNextCycle(std::function<void()> callback)
    {
        if(!callback)
        {
            throw std::invalid_argument("No callback provided for RunInNextCycle");
        }
        _nextCycleCallbacks.push(std::move(callback));
    }

private:
    using Timestamp = std::int64_t;
    using TimeoutHandle = std::uint64_t;
    class UniqueFd
    {
    public:
        explicit UniqueFd(int fd)
            : _fd(fd)
        {
        }

        ~UniqueFd()
        {
            Close();
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : _fd(other._fd)
        {
            other._fd = -1;
        }

        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this != &other)
            {
                Close();
                _fd = other._fd;
                other._fd = -1;
            }
            return *this;
        }

        void Close()
        {
            if (_fd != -1)
            {
                close(_fd);
                _fd = -1;
            }
        }

        explicit operator int() const
        {
            return _fd;
        }

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

    Timestamp GetTimestamp()
    {
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }

    void ClearTimeout(TimeoutHandle handle)
    {
        auto item = _timerIndex.find(handle);
        if(item == _timerIndex.end())
        {
            return;
        }
        _timers.erase(std::make_pair(item->second, handle));
        _timerIndex.erase(item);
    }

    void SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead, bool prohibitNullCallback = true)
    {
        if (prohibitNullCallback && handler == nullptr)
        {
            throw std::invalid_argument("No callback provided for the fd handler");
        }
        /** create fdHandler if none */
        auto& fdHandler = _fdHandlers[fd];
        /** adjust content of fdHandler */
        bool hadReadHandler = fdHandler.readHandler != nullptr;
        bool hadWriteHandler = fdHandler.writeHandler != nullptr;
        if(isRead)
        {
            fdHandler.readHandler = handler;
        }
        else
        {
            fdHandler.writeHandler = handler;
        }
        /** Change epoll settings */
        if((!fdHandler.readHandler) && (!fdHandler.writeHandler))
        {
            if (hadReadHandler || hadWriteHandler)
            {
                epoll_ctl(static_cast<int>(_epollFd), EPOLL_CTL_DEL, fd, nullptr);
            }
            _fdHandlers.erase(fd);
            _fdHandlerFreedInReadHandler = true;
        }
        else if((!hadReadHandler) && (!hadWriteHandler))
        {
            struct epoll_event ev{};
            if(fdHandler.readHandler)
            {
                ev.events |= EPOLLIN;
            }
            if(fdHandler.writeHandler)
            {
                ev.events |= EPOLLOUT;
            }
            ev.data.fd = fd;
            if(epoll_ctl(static_cast<int>(_epollFd), EPOLL_CTL_ADD, fd, &ev) == -1)
            {
                _fdHandlers.erase(fd);
                throw std::runtime_error("epoll_ctl failed");
            }
        }
        else if((hadReadHandler != !!fdHandler.readHandler) 
            || (hadWriteHandler != !!fdHandler.writeHandler))
        {
            struct epoll_event ev{};
            if(fdHandler.readHandler)
            {
                ev.events |= EPOLLIN;
            }
            if(fdHandler.writeHandler)
            {
                ev.events |= EPOLLOUT;
            }
            ev.data.fd = fd;
            if(epoll_ctl(static_cast<int>(_epollFd), EPOLL_CTL_MOD, fd, &ev) == -1)
            {
                _fdHandlers.erase(fd);
                throw std::runtime_error("epoll_ctl failed");
            }
        }
    }
};
