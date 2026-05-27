#ifndef BACKGROUND_H
#define BACKGROUND_H

#include "asio.hpp"

void set_current_thread_scheduler_policy();

class BackgroundService
{
  public:
    static BackgroundService &instance();
    asio::io_context &context();

  private:
    BackgroundService();
    ~BackgroundService();

    asio::io_context io_context;
    std::vector<std::thread> io_thds;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard;
};

#define BG_SERVICE (BackgroundService::instance().context())

#endif // BACKGROUND_H