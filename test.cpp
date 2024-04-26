#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tev-cpp.h"
#include <iostream>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>


int main(int argc, char const *argv[])
{
    Tev tev;
    Tev::TimeoutHandle timer;
    int fds[2] = {-1,-1};
    assert(pipe2(fds, O_NONBLOCK) == 0);

    tev.SetReadHandler(fds[0], [&tev, &fds](){
        char buf[1024];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if(n > 0){
            buf[n] = 0;
            std::cout << "Read: " << buf << std::endl;
        }
    });

    tev.SetTimeout([&tev, &fds](){
        char buf[] = "Hello";
        ssize_t n = write(fds[1], buf, sizeof(buf));
        (void)n;
    }, 500);

    tev.SetTimeout([&tev, &fds](){
        tev.SetReadHandler(fds[0],nullptr);
    },3000);

    std::function<void()> repeat = [&repeat, &tev, &timer](){
        timer = tev.SetTimeout([&repeat](){
            std::cout << "Hello" << std::endl;
            repeat();
        }, 1000);
    };
    repeat();
    
    tev.SetTimeout([&tev, &timer](){
        tev.ClearTimeout(timer);
    },5000);

    tev.MainLoop();

    close(fds[0]);
    close(fds[1]);

    return 0;
}
