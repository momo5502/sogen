#pragma once

// NOLINTBEGIN(modernize-use-using)

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

struct EVENT_BASIC_INFORMATION
{
    EVENT_TYPE EventType;
    LONG EventState;
};

// NOLINTEND(modernize-use-using)
