#include "Tev.h"
#include <queue>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <chrono>
#include <sys/epoll.h>
#include <unistd.h>

/** Tev */

Tev::Tev()
    : _epollFd(epoll_create1(EPOLL_CLOEXEC))
{
    if (static_cast<int>(_epollFd) < 0)
    {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
}

void Tev::MainLoop()
{
    int next_timeout;
    for(;;)
    {
        /** Process run in next cycle callbacks. */
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
        next_timeout = 0;
        /** Process due timers */
        /** Do this instead of additional syscall */
        if(_timers.size() > 0)
        {
            auto now = GetTimestamp();
            while(_timers.size() > 0)
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

Tev::Timestamp Tev::GetTimestamp()
{
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

Tev::Timeout Tev::SetTimeout(std::function<void()> callback, std::int64_t timeoutMs)
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
        }
    };
}

void Tev::ClearTimeout(Tev::TimeoutHandle handle)
{
    auto item = _timerIndex.find(handle);
    if(item == _timerIndex.end())
    {
        return;
    }
    _timers.erase(std::make_pair(item->second, handle));
    _timerIndex.erase(item);
}

Tev::FdHandler Tev::SetReadHandler(int fd, std::function<void()> callback)
{
    SetReadWriteHandler(fd, callback, true);
    return FdHandler{
        [this, fd]() {
            SetReadWriteHandler(fd, nullptr, true, false);
        }, fd, true
    };
}

Tev::FdHandler Tev::SetWriteHandler(int fd, std::function<void()> callback)
{
    SetReadWriteHandler(fd, callback, false);
    return FdHandler{
        [this, fd]() {
            SetReadWriteHandler(fd, nullptr, false, false);
        }, fd, false
    };
}

void Tev::SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead, bool prohibitNullCallback)
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

void Tev::RunInNextCycle(std::function<void()> callback)
{
    if(!callback)
    {
        throw std::invalid_argument("No callback provided for RunInNextCycle");
    }
    _nextCycleCallbacks.push(std::move(callback));
}

/** Timeout */

Tev::Timeout::Timeout()
    : _clearFunc{nullptr}, _cleared(true)
{
}

Tev::Timeout::Timeout(std::function<void()> clearFunc)
    : _clearFunc(std::move(clearFunc))
{
}

Tev::Timeout::~Timeout()
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

Tev::Timeout::Timeout(Tev::Timeout&& other) noexcept
    : _clearFunc(std::move(other._clearFunc)), _cleared(other._cleared)
{
    other._cleared = true;
}

Tev::Timeout& Tev::Timeout::operator=(Tev::Timeout&& other)
{
    if (this != &other)
    {
        Clear();
        _clearFunc = std::move(other._clearFunc);
        _cleared = other._cleared;
        other._cleared = true;
    }
    return *this;
}

bool Tev::Timeout::operator==(std::nullptr_t) const
{
    return _cleared;
}

void Tev::Timeout::Clear()
{
    if (_cleared)
    {
        return;
    }
    _cleared = true;
    if (_clearFunc)
    {
        _clearFunc();
    }
}

/** Fd handler */

Tev::FdHandler::FdHandler()
    : _clearFunc{nullptr}, _fd(-1), _isRead(false), _cleared(true)
{
}

Tev::FdHandler::FdHandler(std::function<void()> clearFunc, int fd, bool isRead)
    : _clearFunc(std::move(clearFunc)), _fd(fd), _isRead(isRead)
{
}

Tev::FdHandler::~FdHandler()
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

Tev::FdHandler::FdHandler(Tev::FdHandler&& other) noexcept
    : _clearFunc(std::move(other._clearFunc)), _fd(other._fd), _isRead(other._isRead), _cleared(other._cleared)
{
    other._fd = -1;
    other._cleared = true;
}

Tev::FdHandler& Tev::FdHandler::operator=(Tev::FdHandler&& other)
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
        _fd = other._fd;
        _isRead = other._isRead;
        _cleared = other._cleared;
        other._fd = -1;
        other._cleared = true;
    }
    return *this;
}

bool Tev::FdHandler::operator==(std::nullptr_t) const
{
    return _cleared;
}

void Tev::FdHandler::Clear()
{
    if (_cleared)
    {
        return;
    }
    _cleared = true;
    if (_clearFunc)
    {
        _clearFunc();
    }
}

/** Unique fd */

Tev::UniqueFd::UniqueFd(int fd)
    : _fd(fd)
{
}

Tev::UniqueFd::~UniqueFd()
{
    Close();
}

Tev::UniqueFd::UniqueFd(Tev::UniqueFd&& other) noexcept
    : _fd(other._fd)
{
    other._fd = -1;
}

Tev::UniqueFd& Tev::UniqueFd::operator=(Tev::UniqueFd&& other) noexcept
{
    if (this != &other)
    {
        Close();
        _fd = other._fd;
        other._fd = -1;
    }
    return *this;
}

void Tev::UniqueFd::Close()
{
    if (_fd != -1)
    {
        close(_fd);
        _fd = -1;
    }
}

Tev::UniqueFd::operator int() const
{
    return _fd;
}
