extern "C" {
#include "DNodeProxyTCPClient.h"
#include <system/BThreadSignal.h>
#include <misc/offset.h>
}
#include "DMasterClient.h"
#include "Logger.h"
#include "DTun/Utils.h"
#include "DTun/SConnector.h"
#include "DTun/SConnection.h"
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>

#define STATE_CONNECTING 1
#define STATE_UP 2
#define STATE_ERR 3

namespace DNode
{
    extern DTun::SManager* theRemoteMgr;

    class ProxyTCPClient : boost::noncopyable
    {
    public:
        explicit ProxyTCPClient(BThreadSignal* reactorSignal)
        : eof_(false)
        , reactorSignal_(reactorSignal)
        , state_(STATE_CONNECTING)
        , sending_(0)
        , receiving_(0)
        , bytesSent_(0)
        , bytesReceived_(0)
        , connId_(0)
        , debugConnId_(0)
        , boundSock_(SYS_INVALID_SOCKET)
        {
            theMasterClient->changeNumOutConnections(1);
        }

        ~ProxyTCPClient()
        {
            DTun::UInt32 connId;

            {
                boost::mutex::scoped_lock lock(m_);
                connId = connId_;
                connId_ = 0;
                reactorSignal_ = NULL;
            }

            if (connId) {
                theMasterClient->cancelConnection(connId);
            }

            if (connector_) {
                connector_->close();
            }

            if (conn_) {
                conn_->close();
            }

            if (boundSock_ != SYS_INVALID_SOCKET) {
                DTun::closeSysSocketChecked(boundSock_);
                boundSock_ = SYS_INVALID_SOCKET;
            }

            theMasterClient->changeNumOutConnections(-1);

            if (debugConnId_) {
                LOG4CPLUS_INFO(logger(), "conn done, id = " << debugConnId_);
            }
        }

        bool start(DTun::UInt32 remoteIp, DTun::UInt16 remotePort)
        {
            SYSSOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == SYS_INVALID_SOCKET) {
                LOG4CPLUS_ERROR(logger(), "Cannot create UDP socket");
                return false;
            }

            SYSSOCKET boundSock = dup(s);
            if (boundSock == -1) {
                LOG4CPLUS_ERROR(logger(), "Cannot dup UDP socket");
                DTun::closeSysSocketChecked(s);
                return false;
            }

            struct sockaddr_in addr;

            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            int res = ::bind(s, (const struct sockaddr*)&addr, sizeof(addr));

            if (res == SYS_SOCKET_ERROR) {
                LOG4CPLUS_ERROR(logger(), "Cannot bind UDP socket");
                DTun::closeSysSocketChecked(s);
                DTun::closeSysSocketChecked(boundSock);
                return false;
            }

            boost::mutex::scoped_lock lock(m_);

            connId_ = theMasterClient->registerConnection(s, remoteIp, remotePort,
                boost::bind(&ProxyTCPClient::onConnectionRegister, this, _1, _2, _3));
            if (!connId_) {
                DTun::closeSysSocketChecked(boundSock);
                return false;
            }

            LOG4CPLUS_INFO(logger(), "new conn to " << DTun::ipPortToString(remoteIp, remotePort) << ", id = " << connId_);
            debugConnId_ = connId_;

            boundSock_ = boundSock;

            return true;
        }

        int getState() const
        {
            boost::mutex::scoped_lock lock(m_);
            return state_;
        }

        void send(const uint8_t* data, int dataLen)
        {
            boost::mutex::scoped_lock lock(m_);
            assert(conn_);
            assert(!sending_);

            sending_ = 1;

            conn_->write((const char*)data, (const char*)(data + dataLen),
                boost::bind(&ProxyTCPClient::onSend, this, _1, dataLen));
        }

        bool isSending(int& bytesSent) const
        {
            boost::mutex::scoped_lock lock(m_);
            bytesSent = bytesSent_;
            return sending_;
        }

        void receive(uint8_t* data, int dataAvail)
        {
            boost::mutex::scoped_lock lock(m_);
            assert(conn_);
            assert(!receiving_);

            receiving_ = 1;

            conn_->read((char*)data, (char*)(data + dataAvail),
                boost::bind(&ProxyTCPClient::onRecv, this, _1, _2), false);
        }

        bool isReceiving(int& bytesReceived) const
        {
            boost::mutex::scoped_lock lock(m_);
            bytesReceived = bytesReceived_;
            return receiving_;
        }

        void setErr()
        {
            boost::mutex::scoped_lock lock(m_);
            state_ = STATE_ERR;
            signalReactor();
            reactorSignal_ = NULL;
        }

        inline bool isEOF() const { return eof_; }

    private:
        void onConnectionRegister(int err, DTun::UInt32 remoteIp, DTun::UInt16 remotePort)
        {
            LOG4CPLUS_TRACE(logger(), "ProxyTCPClient::onConnectionRegister(" << err << ", " << DTun::ipPortToString(remoteIp, remotePort) << ")");

            boost::mutex::scoped_lock lock(m_);
            if (!reactorSignal_) {
                return;
            }

            connId_ = 0;

            if (err) {
                state_ = STATE_ERR;
                signalReactor();
                return;
            }

            boost::shared_ptr<DTun::SHandle> handle = theRemoteMgr->createStreamSocket();
            if (!handle) {
                state_ = STATE_ERR;
                signalReactor();
                return;
            }

            if (!handle->bind(boundSock_)) {
                state_ = STATE_ERR;
                signalReactor();
                return;
            }

            boundSock_ = SYS_INVALID_SOCKET;

            connector_ = handle->createConnector();

            if (!connector_->connect(DTun::ipToString(remoteIp), DTun::portToString(remotePort),
                boost::bind(&ProxyTCPClient::onConnect, this, _1), DTun::SConnector::ModeRendezvousConn)) {
                state_ = STATE_ERR;
                signalReactor();
                return;
            }
        }

        void onConnect(int err)
        {
            LOG4CPLUS_TRACE(logger(), "ProxyTCPClient::onConnect(" << err << ")");

            boost::mutex::scoped_lock lock(m_);

            boost::shared_ptr<DTun::SHandle> handle = connector_->handle();

            connector_->close();

            if (err) {
                handle->close();
                state_ = STATE_ERR;
            }

            if (!reactorSignal_) {
                if (!err) {
                    handle->close();
                }
                return;
            }

            if (!err) {
                conn_ = handle->createConnection();
                boost::shared_ptr<std::vector<char> > rcvBuff =
                    boost::make_shared<std::vector<char> >(1);
                conn_->read(&(*rcvBuff)[0], &(*rcvBuff)[0] + rcvBuff->size(),
                    boost::bind(&ProxyTCPClient::onHandshakeRecv, this, _1, _2, rcvBuff), true);
            } else {
                signalReactor();
            }
        }

        void onSend(int err, int numBytes)
        {
            LOG4CPLUS_TRACE(logger(), "ProxyTCPClient::onSend(" << err << ", " << numBytes << ")");

            boost::mutex::scoped_lock lock(m_);

            sending_ = 0;
            bytesSent_ = numBytes;

            if (err) {
                state_ = STATE_ERR;
            }

            signalReactor();
        }

        void onRecv(int err, int numBytes)
        {
            LOG4CPLUS_TRACE(logger(), "ProxyTCPClient::onRecv(" << err << ", " << numBytes << ")");

            boost::mutex::scoped_lock lock(m_);

            receiving_ = 0;
            bytesReceived_ = numBytes;

            if (err) {
                if (err == DTUN_ERR_CONN_CLOSED) {
                    eof_ = true;
                }
                state_ = STATE_ERR;
            }

            signalReactor();
        }

        void onHandshakeRecv(int err, int numBytes, const boost::shared_ptr<std::vector<char> >& rcvBuff)
        {
            LOG4CPLUS_TRACE(logger(), "ProxyTCPClient::onHandshakeRecv(" << err << ", " << numBytes << ")");

            boost::mutex::scoped_lock lock(m_);

            if (err) {
                state_ = STATE_ERR;
            } else if ((uint8_t)(*rcvBuff)[0] != 0xE1) {
                LOG4CPLUS_ERROR(logger(), "invalid handshake: " << (int)(uint8_t)(*rcvBuff)[0]);
                state_ = STATE_ERR;
            } else {
                state_ = STATE_UP;
            }

            signalReactor();
        }

        void signalReactor()
        {
            if (reactorSignal_) {
                while (!BThreadSignal_Thread_Signal(reactorSignal_)) {
                    LOG4CPLUS_ERROR(logger(), "BThreadSignal_Thread_Signal failed");
                    ::usleep(1000);
                }
            }
        }

        bool eof_;
        mutable boost::mutex m_;
        BThreadSignal* reactorSignal_;
        int state_;
        int sending_;
        int receiving_;
        int bytesSent_;
        int bytesReceived_;
        DTun::UInt32 connId_;
        DTun::UInt32 debugConnId_;
        SYSSOCKET boundSock_;
        boost::shared_ptr<DTun::SConnection> conn_;
        boost::shared_ptr<DTun::SConnector> connector_;
    };
}

typedef struct {
    struct DNodeTCPClient base;
    BThreadSignal reactor_signal;
    int state;
    int was_connected;
    int sending;
    int receiving;
    int bytes_sent;
    int bytes_received;
    BTimer conn_timer;
    DNode::ProxyTCPClient* client;
    StreamPassInterface send_iface;
    StreamRecvInterface recv_iface;
} DNodeProxyTCPClient;

extern "C" StreamPassInterface* DNodeProxyTCPClient_GetSendInterface(struct DNodeTCPClient* dtcp_client_)
{
    DNodeProxyTCPClient* dtcp_client = (DNodeProxyTCPClient*)dtcp_client_;

    ASSERT(dtcp_client->state == STATE_UP)

    return &dtcp_client->send_iface;
}

extern "C" StreamRecvInterface* DNodeProxyTCPClient_GetRecvInterface(struct DNodeTCPClient* dtcp_client_)
{
    DNodeProxyTCPClient* dtcp_client = (DNodeProxyTCPClient*)dtcp_client_;

    ASSERT(dtcp_client->state == STATE_UP)

    return &dtcp_client->recv_iface;
}

extern "C" void DNodeProxyTCPClient_RecvHandler(DNodeProxyTCPClient* dtcp_client, uint8_t* data, int data_avail)
{
    LOG4CPLUS_TRACE(DNode::logger(), "ProxyTCPClient_RecvHandler(" << data_avail << ")");

    ASSERT(!dtcp_client->receiving)

    dtcp_client->receiving = 1;
    dtcp_client->client->receive(data, data_avail);
}

extern "C" void DNodeProxyTCPClient_SendHandler(DNodeProxyTCPClient* dtcp_client, uint8_t* data, int data_len)
{
    LOG4CPLUS_TRACE(DNode::logger(), "ProxyTCPClient_SendHandler(" << data_len << ")");

    ASSERT(!dtcp_client->sending)

    dtcp_client->sending = 1;
    dtcp_client->client->send(data, data_len);
}

extern "C" void DNodeProxyTCPClient_SignalHandler(BThreadSignal* reactor_signal)
{
    DNodeProxyTCPClient* dtcp_client = UPPER_OBJECT(reactor_signal, DNodeProxyTCPClient, reactor_signal);

    LOG4CPLUS_TRACE(DNode::logger(), "ProxyTCPClient_SignalHandler");

    int new_state = dtcp_client->client->getState();

    if (dtcp_client->state != new_state) {
        dtcp_client->state = new_state;
        if (dtcp_client->state == STATE_UP) {
            dtcp_client->was_connected = 1;
            BReactor_RemoveTimer(dtcp_client->reactor_signal.reactor, &dtcp_client->conn_timer);
            StreamPassInterface_Init(&dtcp_client->send_iface,
                (StreamPassInterface_handler_send)DNodeProxyTCPClient_SendHandler, dtcp_client,
                BReactor_PendingGroup(reactor_signal->reactor));
            StreamRecvInterface_Init(&dtcp_client->recv_iface,
                (StreamRecvInterface_handler_recv)DNodeProxyTCPClient_RecvHandler, dtcp_client,
                BReactor_PendingGroup(reactor_signal->reactor));
            dtcp_client->base.handler(dtcp_client->base.handler_data, DNODE_TCPCLIENT_EVENT_UP);
        } else if (dtcp_client->state == STATE_ERR) {
            dtcp_client->base.handler(dtcp_client->base.handler_data,
                dtcp_client->client->isEOF() ? DNODE_TCPCLIENT_EVENT_ERROR_CLOSED : DNODE_TCPCLIENT_EVENT_ERROR);
        }
    }

    if (new_state != STATE_UP) {
        return;
    }

    int bytes = 0;
    if (dtcp_client->receiving && !dtcp_client->client->isReceiving(bytes)) {
        dtcp_client->receiving = 0;
        StreamRecvInterface_Done(&dtcp_client->recv_iface, bytes);
    }
    if (dtcp_client->sending && !dtcp_client->client->isSending(bytes)) {
        dtcp_client->sending = 0;
        StreamPassInterface_Done(&dtcp_client->send_iface, bytes);
    }
}

extern "C" void DNodeProxyTCPClient_ConnTimerHandler(DNodeProxyTCPClient* dtcp_client)
{
    ASSERT(!dtcp_client->was_connected)

    LOG4CPLUS_TRACE(DNode::logger(), "DNodeProxyTCPClient_ConnTimerHandler");

    dtcp_client->client->setErr();
}

extern "C" void DNodeProxyTCPClient_Destroy(struct DNodeTCPClient* dtcp_client_)
{
    DNodeProxyTCPClient* dtcp_client = (DNodeProxyTCPClient*)dtcp_client_;

    if (dtcp_client->was_connected) {
        StreamPassInterface_Free(&dtcp_client->send_iface);
        StreamRecvInterface_Free(&dtcp_client->recv_iface);
    } else {
        BReactor_RemoveTimer(dtcp_client->reactor_signal.reactor, &dtcp_client->conn_timer);
    }

    delete dtcp_client->client;

    BThreadSignal_Free(&dtcp_client->reactor_signal);

    free(dtcp_client);
}

extern "C" struct DNodeTCPClient* DNodeProxyTCPClient_Create(BAddr dest_addr, DNodeTCPClient_handler handler, void* handler_data, BReactor* reactor)
{
    DNodeProxyTCPClient* dtcp_client;

    ASSERT(dest_addr.type == BADDR_TYPE_IPV4 || dest_addr.type == BADDR_TYPE_IPV6)

    if (!(dtcp_client = (DNodeProxyTCPClient*)malloc(sizeof(*dtcp_client)))) {
        LOG4CPLUS_ERROR(DNode::logger(), "malloc failed");
        return NULL;
    }

    // init arguments
    dtcp_client->base.get_sender_interface = &DNodeProxyTCPClient_GetSendInterface;
    dtcp_client->base.get_recv_interface = &DNodeProxyTCPClient_GetRecvInterface;
    dtcp_client->base.destroy = &DNodeProxyTCPClient_Destroy;
    dtcp_client->base.handler = handler;
    dtcp_client->base.handler_data = handler_data;

    if (!BThreadSignal_Init(&dtcp_client->reactor_signal, reactor, DNodeProxyTCPClient_SignalHandler)) {
        LOG4CPLUS_ERROR(DNode::logger(), "BThreadSignal_Init");
        free(dtcp_client);
        return NULL;
    }

    dtcp_client->state = STATE_CONNECTING;
    dtcp_client->was_connected = 0;
    dtcp_client->sending = 0;
    dtcp_client->receiving = 0;
    dtcp_client->bytes_sent = 0;
    dtcp_client->bytes_received = 0;

    BTimer_Init(&dtcp_client->conn_timer, 30000, (BTimer_handler)&DNodeProxyTCPClient_ConnTimerHandler, dtcp_client);
    BReactor_SetTimer(reactor, &dtcp_client->conn_timer);

    dtcp_client->client = new DNode::ProxyTCPClient(&dtcp_client->reactor_signal);

    if (!dtcp_client->client->start(dest_addr.ipv4.ip, dest_addr.ipv4.port)) {
        delete dtcp_client->client;
        BReactor_RemoveTimer(reactor, &dtcp_client->conn_timer);
        BThreadSignal_Free(&dtcp_client->reactor_signal);
        free(dtcp_client);
        return NULL;
    }

    return &dtcp_client->base;
}
