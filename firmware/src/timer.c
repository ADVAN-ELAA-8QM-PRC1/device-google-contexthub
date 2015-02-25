#include <stdio.h>
#include <stdlib.h>
#include <seos.h>
#include <platform.h>
#include <timer.h>

#include <string.h>

#define TIMER_LIST_SIZE 16
#define TIMER_WAKEUP_BUFFER_US 100

static struct timer_item_t timer_list[TIMER_LIST_SIZE] = {{0}};
static unsigned items = 0;

//TODO:  Eliminate this once clock counter enabled
nanotime_t timer_time = {0};

/* nanotime_t should not contain nanosec_t values of greater than NS_PER_S */
bool nanotime_less_than(nanotime_t time_a, nanotime_t time_b)
{
    if (time_a.time_ns >= NS_PER_S || time_b.time_ns >= NS_PER_S)
        OS_log(LOG_WARN, "Comparing nanotime_t's with more than 1 bil ns.");
    if (((time_a.time_s | time_a.time_ns) == 0) ||
            ((time_b.time_s | time_b.time_ns) == 0))
        OS_log(LOG_DEBUG, "0-valued time being compared.  Could \
                it be uninitialized?");
    return (time_a.time_s < time_b.time_s) ||
        (time_a.time_s == time_b.time_s && time_a.time_ns < time_b.time_ns);
}

/* For nanotimes to be added, nanosecond member must be less than NS_PER_S */
nanotime_t nanotime_add(nanotime_t time_a, nanotime_t time_b)
{
    nanotime_t sum = {0};
    if ((time_a.time_ns >= NS_PER_S) || (time_b.time_ns >= NS_PER_S)) {
        OS_log(LOG_WARN, "nanotime_t unit extends beyond a single nanosecond.");
        return sum;
    }
    sum.time_ns = time_a.time_ns + time_b.time_ns;
    bool carry = (sum.time_ns >= NS_PER_S);
    if (carry)
        sum.time_ns -= NS_PER_S;
    sum.time_s = time_a.time_s + time_b.time_s +
        (carry ? 1 : 0);
    return sum;
}

nanotime_t nanotime_subtract(nanotime_t time_a, nanotime_t time_b)
{
    nanotime_t diff = {0};
    if ((time_a.time_ns >= NS_PER_S) || (time_b.time_ns >= NS_PER_S)) {
        OS_log(LOG_WARN, "nanotime_t unit extends beyond a single nanosecond.");
        return diff;
    }
    if (time_a.time_s < time_b.time_s || (time_a.time_s == time_b.time_s && time_a.time_ns < time_b.time_s)) {
        OS_log(LOG_WARN, "Trying to subtract a larger nanotime from smaller.");
        return diff;
    }

    diff.time_s = time_a.time_s - time_b.time_s;

    if (time_b.time_ns > time_a.time_ns) {
        diff.time_s--;
        time_a.time_ns += NS_PER_S;
    }
    diff.time_ns = time_a.time_ns - time_b.time_ns;

    return diff;
}

static unsigned nanotime_to_us(nanotime_t time)
{
    return time.time_s*1000000 + time.time_ns/1000;
}


void Timer_init(void)
{
}

void Timer_interrupt_handler(void)
{
    timer_item_t timer;
    nanotime_t curr_deadline;
    nanotime_t curr_time = OS_get_time();
    bool time_set = false;
    /* expire all expired timers */
    do {
        timer = Timer_expire_next();
        curr_deadline = timer.deadline;
        if (!time_set) {
            timer_time = nanotime_add(timer_time, timer.ideal_delay);
            time_set = true;
        }
        if(!timer.one_shot) {
            timer.deadline = nanotime_add(timer_time, timer.ideal_delay);
            Timer_insert_timer(timer);
        }
        task_wakeup_t task_wakeup = {EVENT_TIMER, EVENT_FLAG_NONE, timer.task, curr_deadline};
        OS_task_enqueue(task_wakeup);
    } while(nanotime_less_than(Timer_earliest()->deadline, curr_time));
    /* Set the next wakeup, if one exists */
    if (items != 0) {
        nanotime_t timer_delay = Timer_earliest()->ideal_delay;
        unsigned timer_delay_us = timer_delay.time_s*1000000 +
            timer_delay.time_ns/1000 - TIMER_WAKEUP_BUFFER_US;
        Platform_set_alarm(timer_delay_us);
    }
}

bool Timer_insert(task_t *task, nanotime_t period, nanosec_t max_jitter_ns,
        nanosec_t max_drift_ns, bool one_shot)
{
    nanotime_t deadline = nanotime_add(OS_get_time(), period);
    timer_item_t timer = {task, deadline, period, max_jitter_ns, max_drift_ns,
        one_shot};
    return Timer_insert_timer(timer);
}

//TODO If naive iteration of timers proves inefficient, switch to heap
bool Timer_insert_timer(timer_item_t timer)
{
    int i;

    if (items == TIMER_LIST_SIZE) {
        OS_log(LOG_WARN, "Timer insertion failed, timers full.\n");
        return false;
    }

    for(i = 0; i < TIMER_LIST_SIZE; i++) {
        if(timer_list[i].task == NULL) {
            timer_list[i] = timer;
            items++;
            break;
        }
    }

    Platform_set_alarm(nanotime_to_us(Timer_earliest()->deadline)
        - TIMER_WAKEUP_BUFFER_US);
    return true;
}

timer_item_t Timer_expire_next(void)
{
    timer_item_t *next;
    timer_item_t empty_timer = {0};
    timer_item_t temp = {0};

    if (items == 0)
        return empty_timer;

    /* Remove earliest deadline */
    next  = Timer_earliest();
    temp = *next;
    *next = empty_timer;
    items--;

    return temp;
}

bool Timer_is_active(void)
{
    return (items > 0);
}

//TODO: does access to timers need to be locked?
timer_item_t *Timer_earliest(void)
{
    int i;

    /* Return earliest deadline */
    timer_item_t *min = NULL;
    if (items == 0) {
        return min;
    }
    for (i = 0; i < TIMER_LIST_SIZE; i++) {
        if (timer_list[i].task != NULL) {
            if (min == NULL)
                min = &timer_list[i];
            else if (nanotime_less_than(timer_list[i].deadline,
                        min->deadline))
                min = &timer_list[i];
        }
    }
    return min;
}

void Timer_clear_timers_for_task(struct task_t *task)
{
    int i;

    timer_item_t empty_timer_item = {0};
    for (i = 0; i < TIMER_LIST_SIZE; i++) {
        if (timer_list[i].task == task) {
            /* Remove timer */
            timer_list[i] = empty_timer_item;
            items--;
        }
    }
}

