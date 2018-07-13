#ifndef _DTUN_SYSCONNECTION_H_
#define _DTUN_SYSCONNECTION_H_

#include "DTun/SysHandler.h"
#include "DTun/SConnection.h"
#include <boost/thread/mutex.hpp>
#include <list>

namespace DTun
{
    class SysConnection : public SysHandler, public SConnection
    {
    public:
        SysConnection(SysReactor& reactor, const boost::shared_ptr<SysHandle>& handle);
        ~SysConnection();

        virtual void write(const char* first, const char* last, const WriteCallback& callback);

        virtual void read(char* first, char* last, const ReadCallback& callback, bool readAll);

        virtual void close();

        virtual int getPollEvents() const;

        virtual void handleRead();
        virtual void handleWrite();

    private:
        struct WriteReq
        {
            const char* first;
            const char* last;
            WriteCallback callback;
        };

        struct ReadReq
        {
            char* first;
            char* last;
            ReadCallback callback;
        };

        mutable boost::mutex m_;
        std::list<WriteReq> writeQueue_;
        std::list<ReadReq> readQueue_;
    };
}

#endif