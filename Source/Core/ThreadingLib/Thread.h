#pragma once

//==============================================================================
// Joe Schutte
//==============================================================================

namespace Shooty
{

    //==============================================================================
    // Forward declaration
    //==============================================================================
    typedef void(*ThreadFunction)(void*);
    typedef void* ThreadHandle;

    //==============================================================================
    // Thread
    //==============================================================================
    #define InvalidThreadHandle 0

    ThreadHandle CreateThread(ThreadFunction function, void* userData);
    void         ShutdownThread(ThreadHandle threadHandle);
}