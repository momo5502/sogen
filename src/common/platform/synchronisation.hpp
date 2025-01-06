#pragma once

typedef enum _EVENT_TYPE
{
    NotificationEvent,
    SynchronizationEvent
} EVENT_TYPE;

typedef enum _WAIT_TYPE
{
    WaitAll,
    WaitAny,
    WaitNotification,
    WaitDequeue,
    WaitDpc,
} WAIT_TYPE;
