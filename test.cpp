#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "Tev.h"
#include <iostream>
#include <optional>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    Tev tev{};
    std::optional<Tev::Timeout> timer{std::nullopt};
    int fds[2] = {-1,-1};
    int rc = pipe2(fds, O_NONBLOCK);
    if (rc != 0)
    {
        std::cerr << "pipe2 failed" << std::endl;
        return 1;
    }

    auto readHandler = tev.SetReadHandler(fds[0], [&](){
    });

    /** Assign the read handler again. This should not cause any trouble. */
    readHandler = tev.SetReadHandler(fds[0], [&](){
        char buf[1024];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if(n > 0){
            buf[n] = 0;
            std::cout << "Read: " << buf << std::endl;
        }
    });

    auto sendTimeout = tev.SetTimeout([&](){
        char buf[] = "Hello";
        ssize_t n = write(fds[1], buf, sizeof(buf));
        (void)n;
    }, 500);

    auto clearReadHandlerTimeout = tev.SetTimeout([&](){
        readHandler.Clear();
        if (readHandler != nullptr)
        {
            abort();
        }
    },3000);

    std::function<void()> repeat = [&](){
        timer = tev.SetTimeout([&repeat](){
            std::cout << "Hello" << std::endl;
            repeat();
        }, 1000);
    };
    repeat();
    
    auto clearTimerTimeout = tev.SetTimeout([&](){
        tev.RunInNextCycle([&](){
            tev.RunInNextCycle([&](){
                timer->Clear();
                if (timer != nullptr)
                {
                    abort();
                }
            });
        });
    },5000);

    tev.MainLoop();

    close(fds[0]);
    close(fds[1]);

    return 0;
}
