#include <iostream>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/unistd.h>
#include <functional>
#include <stdexcept>
#include <chrono>

#include <string>
#include <cuchar>
#include <stdio.h>

#define CHECKRVAL(RVAL)                                                     \
    if((RVAL) < 0) {						                                \
        char msg[256] = {0};                                                \
        snprintf(msg, sizeof(msg), #RVAL                                    \
            " failed with errno %d at line %d of file " __FILE__, errno,    \
            int(__LINE__));                                                 \
        throw std::runtime_error(msg); }
#define CHECKTYPE(VAR, CALL, TYPE)					                        \
    TYPE VAR = (CALL);						                                \
    CHECKRVAL(VAR)
#define CHECKFD(VAR, CALL) CHECKTYPE (VAR, CALL, int)
#define CHECKERR(CALL) { CHECKFD(__result__, (CALL)); }

class Fd;

class PollFd
{
public:
    PollFd(): epollfd(epoll_create1(0)), maxevents(0)
    {

    }
    virtual ~PollFd()
    {
        close(epollfd);
    }
protected:
    void addFd(Fd &fd, uint32_t events = EPOLLIN | EPOLLRDHUP | EPOLLERR);
    void delFd(Fd &fd);
    void updateEventMask(Fd &fd, uint32_t events = EPOLLIN | EPOLLRDHUP | EPOLLERR);
public:
    int getPollFd()
    {
        return epollfd;
    }
    void processFds(int timeout = -1);
private:
    int epollfd;
    int maxevents;
};

class Fd: public virtual PollFd
{
public:
    virtual void process(uint32_t events) = 0;// = 0;
//    {

//    }

    virtual int getFd()
    {
        return fd;
    }
    Fd(const std::string &name_, int fd);
    void configure(uint32_t events = EPOLLIN | EPOLLRDHUP | EPOLLERR);
    virtual ~Fd()
    {
        close(fd);
    }
    operator int()
    {
        return fd;
    }
    const std::string& getName() const
    {
        return name;
    }
    void updateEventMask(uint32_t events = EPOLLIN | EPOLLRDHUP | EPOLLERR);
    void updateFd(int newFd, uint32_t events = EPOLLIN | EPOLLRDHUP | EPOLLERR);
protected:
    int fd;
    std::string name;
};
class TimerFd: public Fd
{
public:
    TimerFd(const std::string& name, uint64_t interval_ = 0);
    void setTimeout(uint64_t interval_)
    {
        interval = interval_;
        rearmTimer();
    }
    uint64_t getTimeout()
    {
        return interval;
    }
    void disarmTimer();
    void rearmTimer();
private:
    virtual void process(uint32_t events) final override;
    uint64_t interval;
    virtual void processTimeout() = 0;
};


void PollFd::addFd(Fd& fd, uint32_t events)
{
    struct epoll_event event = {events, &fd};
    CHECKERR(epoll_ctl(epollfd, EPOLL_CTL_ADD, fd.getFd(), &event));
    maxevents++;
}

void PollFd::delFd(Fd& fd)
{
    struct epoll_event event = {0, &fd};
    CHECKERR(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd.getFd(), &event));
    maxevents--;
}

void PollFd::updateEventMask(Fd &fd, uint32_t events)
{
    struct epoll_event event = {events, &fd};
    CHECKERR(epoll_ctl(epollfd, EPOLL_CTL_MOD, fd.getFd(), &event));
}

void PollFd::processFds(int timeout)
{
    struct epoll_event events[maxevents];
    int polled;
    do
    {
        CHECKERR(polled = epoll_wait(epollfd, events, maxevents, timeout));
        for(int i = 0; i < polled; i++)
        {
            auto &event = events[i];
            Fd* fd = static_cast<Fd*>(event.data.ptr);
            fd->process(event.events);
        }
    }
    while(polled);
}

Fd::Fd(const std::string &name_, int fd_)
        : fd(fd_)
        , name(name_)
{
    CHECKERR(fd);
}

void Fd::configure(uint32_t events)
{
    PollFd::addFd(*this, events);
}

void Fd::updateEventMask(uint32_t events)
{
    PollFd::updateEventMask(*this, events);
}

void Fd::updateFd(int newFd, uint32_t events)
{
    PollFd::delFd(*this);
    close(fd);
    fd = newFd;
    PollFd::addFd(*this, events);
}

//! -------------------------------------------------------------------------

TimerFd::TimerFd(const std::string& name, uint64_t interval_):
        Fd(name, timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)),
        interval(interval_)
{
    //! TODO: check in production.
    PollFd::addFd(*this);
}

void TimerFd::disarmTimer()
{
    struct itimerspec itimerspec = {{0, 0}, {0, 0}};
    CHECKERR(timerfd_settime(fd, 0, &itimerspec, NULL));
}

void TimerFd::rearmTimer()
{
    //interval = 1000000;

    const auto secs  = interval / 1000000000;
    const auto nsecs = interval % 1000000000;

    itimerspec timerValue = {{0, 0}, {secs, nsecs}};

//    struct itimerspec itimerspec = {{0, 0},
//        {time_t(interval / 1000000000), uint32_t(interval % 1000000000)}};

    CHECKERR(timerfd_settime(fd, 0, &timerValue, NULL));
    CHECKERR(timerfd_gettime(fd, &timerValue));

}

void TimerFd::process(uint32_t events) {
    uint64_t fired;
    int rresult = read(fd, &fired, sizeof(fired));
    if (rresult >= 0) {
        processTimeout();
    } else if (errno != EAGAIN) {
        CHECKERR(rresult);
    }
}

namespace hft::experimental {
    template<typename F, typename ...Args>
    class TimeoutCallback;
    //! @brief Class to call registered callback on timeout within epollfd context
    template<typename F, typename ...Args>
    class TimeoutCallback<F(Args...)> : public TimerFd {
        std::function<F(Args...)> callback_;
        int epoll_fd_;
        static int inline number_ = 0;
    public:
        using callback_type = decltype(callback_);

        explicit TimeoutCallback(callback_type f, int epoll_fd, uint64_t timeout_sec = 10)
                : TimerFd(std::to_string(number_++) + "_timeoutCallback"
                , timeout_sec * 1'000'000'000)
                , epoll_fd_(epoll_fd)
                , callback_(f)
        {
            struct epoll_event event = {.events = EPOLLIN | EPOLLOUT,.data = {.ptr = this}};
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, getPollFd(), &event);
            rearmTimer();
        }
        void processTimeout() override {
            std::invoke(callback_);
            disarmTimer();
        }
        ~TimeoutCallback() override {
            auto res = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, getPollFd(), nullptr);
            std::cerr << res;
        }
    };
}
#include <sys/epoll.h>
#include <vector>
#include <any>
#include <array>
void v() {
    std::cerr << "well it works\n";
}

int main() {
    int epollfd = epoll_create1(0);
    auto a = hft::experimental::TimeoutCallback<void()>(v, epollfd, 1);
    auto b = hft::experimental::TimeoutCallback<void()>(v, epollfd, 2);
    auto c = hft::experimental::TimeoutCallback<void()>(v, epollfd, 3);

    for (int i = 0; i < 10; i++) {
        epoll_event event {};
        epoll_wait(epollfd, &event, 1, -1);
        auto b = static_cast<hft::experimental::TimeoutCallback<void()>*>(event.data.ptr);
        b->processTimeout();
    }
}

