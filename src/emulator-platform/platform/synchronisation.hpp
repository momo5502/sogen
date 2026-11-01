#pragma once

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-use-enum-class)

namespace sogen
{

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

    typedef enum _MUTANT_INFORMATION_CLASS
    {
        MutantBasicInformation,
        MutantOwnerInformation
    } MUTANT_INFORMATION_CLASS;

    struct MUTANT_BASIC_INFORMATION
    {
        LONG CurrentCount;
        BOOLEAN OwnedByCaller;
        BOOLEAN AbandonedState;
    };

    struct MUTANT_OWNER_INFORMATION
    {
        CLIENT_ID64 ClientId;
    };

    typedef enum _SEMAPHORE_INFORMATION_CLASS
    {
        SemaphoreBasicInformation
    } SEMAPHORE_INFORMATION_CLASS;

    struct SEMAPHORE_BASIC_INFORMATION
    {
        LONG CurrentCount;
        LONG MaximumCount;
    };

    // NOLINTEND(modernize-use-using,cppcoreguidelines-use-enum-class)
} // namespace sogen
