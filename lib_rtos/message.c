#include "lib_rtos/message.h"
#include "lib_rtos/lib_rtos.h"

static AL_64U log_start_time_sec = 0;
static AL_MUTEX log_mutex = NULL;

void message_init()
{
    log_mutex = Rtos_CreateMutex();
    Rtos_GetMutex(log_mutex);
    log_start_time_sec = Rtos_GetTime();
    Rtos_ReleaseMutex(log_mutex);
}

double get_log_time_sec()
{
    double diff;
    Rtos_GetMutex(log_mutex);
    diff = Rtos_GetTime() - log_start_time_sec;
    Rtos_ReleaseMutex(log_mutex);
    return diff / 1000000.0;
}