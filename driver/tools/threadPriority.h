
#pragma once

#include <thread>

    #include <pthread.h>

struct ThreadPriority {

    enum Mode { Normal, High, Realtime };

    static auto setPriority( ThreadPriority::Mode mode, float typicalProcessingTimeInMilliSeconds = 0, float maxProcessingTimeInMilliSeconds = 0 ) -> bool {

        if (!_setPriority( mode, typicalProcessingTimeInMilliSeconds, maxProcessingTimeInMilliSeconds )) {

            if ( mode == Mode::Realtime)
                return _setPriority( ThreadPriority::Mode::High );

            return false;
        }

        return true;
    }

    static auto _setPriority( ThreadPriority::Mode mode, float minProcessingTimeInMilliSeconds = 0, float maxProcessingTimeInMilliSeconds = 0 ) -> bool {
        // setcap cap_sys_nice=ep /usr/local/bin/your-program

        const int policy = SCHED_RR;
        const int minPrio = sched_get_priority_min(policy);
        const int maxPrio = sched_get_priority_max(policy);
        sched_param sch_params;

        switch(mode) {
            default:
            case Mode::Normal:
                sch_params.sched_priority = (minPrio + maxPrio) / 2;
                break;
            case Mode::High:
                sch_params.sched_priority = maxPrio - 3;
                break;
            case Mode::Realtime:
                sch_params.sched_priority = maxPrio - 1;
                break;
        }

        return pthread_setschedparam( pthread_self(), policy, &sch_params) == 0;
    }
};
