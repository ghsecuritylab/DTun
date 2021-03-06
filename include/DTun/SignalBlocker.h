#ifndef _DTUN_SIGNAL_BLOCKER_H_
#define _DTUN_SIGNAL_BLOCKER_H_

#include "DTun/Types.h"
#include <boost/noncopyable.hpp>
#include <signal.h>

namespace DTun
{
    class DTUN_API SignalBlocker : boost::noncopyable
    {
    public:
        explicit SignalBlocker(bool withSigInt);
        ~SignalBlocker();

        /*
         * Called automatically on destruction.
         */
        void unblock() throw ();

    private:
        bool blocked_;
        sigset_t oldMask_;
    };
}

#endif
