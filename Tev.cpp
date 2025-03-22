#include "Tev.h"
#include <queue>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <time.h>
#include <sys/epoll.h>
#include <unistd.h>

/** This must be 1 to allow safely removal of fd handlers inside a fd handler */
#define TEV_MAX_EPOLL_EVENTS 1

class Tev::Impl
{
public:
    typedef std::int64_t Timestamp;
    struct FdHandler
    {
        std::function<void()> readHandler;
        std::function<void()> writeHandler;
    };
    struct Timeout
    {
        std::function<void()> callback;
    };

    int epollFd = -1;
    std::unordered_map<int, FdHandler> fdHandlers{};
    std::map<std::pair<Timestamp,TimeoutHandle>, Timeout> timers{};
    std::unordered_map<TimeoutHandle,Timestamp> timerIndex{};
    bool fdHandlerFreedInReadHandler = false;
    TimeoutHandle timeoutHandleSeed = 0;

    Impl()
        : epollFd(epoll_create1(0))
    {
    }

    ~Impl()
    {
        close(this->epollFd);
    }

    Impl(const Impl& ) = delete;
    Impl& operator=(const Impl& ) = delete;
    Impl(Impl &&) = delete;
    Impl& operator=(Impl&& ) = delete;

    Timestamp GetNowMs();
    TimeoutHandle GetNextTimeoutHandle();
    void SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead);
};

Tev::Impl::Timestamp Tev::Impl::GetNowMs()
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<Timestamp>(now.tv_sec) * 1000 
        + static_cast<Timestamp>(now.tv_nsec) / 1000000;
}

Tev::TimeoutHandle Tev::Impl::GetNextTimeoutHandle()
{
    this->timeoutHandleSeed++;
    if(this->timeoutHandleSeed == 0)
        this->timeoutHandleSeed++;
    return this->timeoutHandleSeed;
}

Tev::Tev():
    _impl(new Tev::Impl)
{
}

Tev::~Tev()
{
    delete _impl;
}

void Tev::MainLoop()
{
    int next_timeout;
    for(;;)
    {
        next_timeout = 0;
        /** Process due timers */
        /** Do this instead of additional syscall */
        if(this->_impl->timers.size() > 0)
        {
            auto now = this->_impl->GetNowMs();
            while(this->_impl->timers.size() > 0)
            {
                auto item = this->_impl->timers.begin();
                if(item->first.first > now)
                {
                    next_timeout = item->first.first - now;
                    break;
                }
                /** remove the item first */
                this->_impl->timerIndex.erase(item->first.second);
                auto callback = item->second.callback;
                this->_impl->timers.erase(item);
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
        if(next_timeout == 0 && this->_impl->fdHandlers.size() != 0)
            next_timeout = -1;
        /** Check exit condition */
        if(next_timeout == 0)
            break;
        /** Wait for events */
        struct epoll_event events[TEV_MAX_EPOLL_EVENTS];
        int nfds = epoll_wait(this->_impl->epollFd, events, TEV_MAX_EPOLL_EVENTS, next_timeout);
        for(int i = 0; i < nfds; i++)
        {
            auto handler = static_cast<Tev::Impl::FdHandler*>(events[i].data.ptr);
            if(!handler)
                continue;
            this->_impl->fdHandlerFreedInReadHandler = false;
            if(((events[i].events & EPOLLIN) || (events[i].events & EPOLLHUP)) && handler->readHandler)
            {
                try
                {
                    handler->readHandler();
                }
                catch(...)
                {
                    /** Ignore all error in the callback */
                }
                
            }
            if((events[i].events & EPOLLOUT) && (!this->_impl->fdHandlerFreedInReadHandler) && handler->writeHandler)
            {
                try
                {
                    handler->writeHandler();
                }
                catch(...)
                {
                    /** Ignore all error in the callback */
                }
            }
        }
    }
}

Tev::TimeoutHandle Tev::SetTimeout(std::function<void()> callback, std::int64_t timeoutMs)
{
    if(!callback)
        return 0;
    Tev::Impl::Timestamp target = this->_impl->GetNowMs() + timeoutMs;
    auto handle = this->_impl->GetNextTimeoutHandle();
    auto pair = this->_impl->timerIndex.insert(std::make_pair(handle, target));
    if(!pair.second)
        return 0;
    auto pair2 = this->_impl->timers.insert(std::make_pair(std::make_pair(target, handle), Tev::Impl::Timeout{callback}));
    if(!pair2.second)
    {
        this->_impl->timerIndex.erase(pair.first);
        return 0;
    }
    return handle;
}

void Tev::ClearTimeout(Tev::TimeoutHandle handle)
{
    auto item = this->_impl->timerIndex.find(handle);
    if(item == this->_impl->timerIndex.end())
        return;
    this->_impl->timers.erase(std::make_pair(item->second, handle));
    this->_impl->timerIndex.erase(item);
}

void Tev::SetReadHandler(int fd, std::function<void()> callback)
{
    this->_impl->SetReadWriteHandler(fd, callback, true);
}

void Tev::SetWriteHandler(int fd, std::function<void()> callback)
{
    this->_impl->SetReadWriteHandler(fd, callback, false);
}

void Tev::Impl::SetReadWriteHandler(int fd, std::function<void()> handler, bool isRead)
{
    /** create fdHandler if none */
    if(!fdHandlers.contains(fd))
    {
        fdHandlers[fd] = FdHandler();
    }
    FdHandler& fdHandler = fdHandlers[fd];
    /** adjust content of fdHandler */
    bool hadReadHandler = !!fdHandler.readHandler;
    bool hadWriteHandler = !!fdHandler.writeHandler;
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
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
        fdHandlers.erase(fd);
        fdHandlerFreedInReadHandler = true;
    }
    else if((!hadReadHandler) && (!hadWriteHandler))
    {
        struct epoll_event ev{};
        if(fdHandler.readHandler)
            ev.events |= EPOLLIN;
        if(fdHandler.writeHandler)
            ev.events |= EPOLLOUT;
        ev.data.ptr = &fdHandler;
        if(epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            fdHandlers.erase(fd);
            throw std::runtime_error("epoll_ctl failed");
        }
    }
    else if((hadReadHandler != !!fdHandler.readHandler) 
        || (hadWriteHandler != !!fdHandler.writeHandler))
    {
        struct epoll_event ev;
        if(fdHandler.readHandler)
            ev.events |= EPOLLIN;
        if(fdHandler.writeHandler)
            ev.events |= EPOLLOUT;
        ev.data.ptr = &fdHandler;
        if(epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &ev) == -1)
        {
            fdHandlers.erase(fd);
            throw std::runtime_error("epoll_ctl failed");
        }
    }
}
