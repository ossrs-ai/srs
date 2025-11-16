//
// Copyright (c) 2013-2024 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#ifndef SRS_APP_RTC_CASCADE_HPP
#define SRS_APP_RTC_CASCADE_HPP

#include <srs_core.hpp>

#include <string>
#include <vector>

#include <srs_app_listener.hpp>
#include <srs_protocol_json.hpp>

class ISrsProtocolReadWriter;
class ISrsResourceManager;
class ISrsStatistic;
class SrsNetworkDelta;
class SrsRtcConnection;
class ISrsRtcConnection;
class SrsRtcCascadeNetwork;
class SrsRequest;
class SrsRtcSource;
class ISrsRtcApiServer;

// The WebRTC Private TCP Cascade Manager handles server-to-server WebRTC connections
// over a unified TCP channel (default port 9999) that combines JSON signaling and
// WebRTC media transport.
class SrsRtcCascadeManager : public ISrsTcpHandler
{
// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    ISrsResourceManager *conn_manager_;
    ISrsRtcApiServer *server_;
    ISrsAppFactory *app_factory_;

public:
    SrsRtcCascadeManager(ISrsRtcApiServer *server);
    virtual ~SrsRtcCascadeManager();

public:
    // Interface ISrsTcpHandler
    virtual srs_error_t on_tcp_client(ISrsListener *listener, srs_netfd_t stfd);
};

// Interface for WebRTC Private TCP Cascade Connection
class ISrsRtcCascadeConn : public ISrsConnection
{
public:
    ISrsRtcCascadeConn();
    virtual ~ISrsRtcCascadeConn();
};

// The WebRTC Private TCP Cascade Connection handles a single TCP connection
// for the WebRTC Private TCP protocol, which includes:
// 1. JSON signaling (publish/play requests and responses)
// 2. STUN connectivity checks
// 3. DTLS handshake (for key exchange)
// 4. RTP/RTCP media transport (plaintext, not encrypted)
//
// It also implements ISrsProtocolReadWriter to provide RFC 4571 framing
// (2-byte length prefix) for all read/write operations.
class SrsRtcCascadeConn : public ISrsRtcCascadeConn,     // It's a resource.
                          public ISrsProtocolReadWriter, // RFC 4571 framing wrapper.
                          public ISrsCoroutineHandler,
                          public ISrsExecutorHandler,
                          public ISrsDisposingHandler
{
// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    ISrsResourceManager *conn_manager_;
    ISrsStatistic *stat_;

// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    std::string ip_;
    int port_;
    ISrsProtocolReadWriter *skt_;
    SrsNetworkDelta *delta_;

// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    // The RTC session created after JSON signaling
    ISrsRtcConnection *session_;
    // The RTC API server for creating sessions
    ISrsRtcApiServer *server_;

// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    // Packet buffer for reading RFC 4571 framed packets
    char *pkt_;

// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    // The owner coroutine
    ISrsInterruptable *trd_;
    ISrsContextIdSetter *owner_cid_;
    SrsContextId cid_;

public:
    SrsRtcCascadeConn(ISrsResourceManager *conn_manager, ISrsRtcApiServer *server, ISrsProtocolReadWriter *skt, std::string cip, int port);
    virtual ~SrsRtcCascadeConn();

public:
    // Setup the owner coroutine and context id setter for interruption and context switching.
    void setup_owner(ISrsInterruptable *owner_coroutine, ISrsContextIdSetter *owner_cid);

public:
    ISrsKbpsDelta *delta();
    // Interrupt transport by session
    void interrupt();

    // Interface ISrsResource
public:
    virtual std::string desc();
    virtual const SrsContextId &get_id();

    // Interface ISrsConnection
public:
    virtual std::string remote_ip();

    // Interface ISrsExecutorHandler
public:
    virtual void on_executor_done(ISrsInterruptable *executor);

    // Interface ISrsDisposingHandler
public:
    virtual void on_before_dispose(ISrsResource *c);
    virtual void on_disposing(ISrsResource *c);

    // Interface ISrsCoroutineHandler
public:
    virtual srs_error_t cycle();

    // Interface ISrsProtocolReadWriter
    // Provides RFC 4571 framing (2-byte length prefix) for all read/write operations
public:
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    virtual srs_error_t read_fully(void *buf, size_t size, ssize_t *nread);
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    virtual srs_error_t read(void *buf, size_t size, ssize_t *nread);
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    virtual srs_error_t write(void *buf, size_t size, ssize_t *nwrite);
    virtual srs_error_t writev(const iovec *iov, int iov_size, ssize_t *nwrite);

// clang-format off
SRS_DECLARE_PRIVATE: // clang-format on
    srs_error_t do_cycle();
    // Handle JSON signaling phase
    srs_error_t handle_signaling();
    // Read RFC 4571 framed packet (2-byte length + payload)
    srs_error_t read_packet(char *pkt, int *nb_pkt);
    // Handle different packet types after signaling
    srs_error_t on_packet(char *pkt, int nb_pkt);
    // Parse and handle JSON signaling messages
    srs_error_t on_json_message(char *data, int nb_data);
    // Handle publish request
    srs_error_t on_publish_request(SrsJsonObject *req);
    // Handle play request
    srs_error_t on_play_request(SrsJsonObject *req);
    // Send JSON response
    srs_error_t send_json_response(SrsJsonObject *resp);
    // Write RFC 4571 framed packet (used internally by write() and writev())
    srs_error_t write_packet(const char *data, int nb_data);
    // Dispose the RTC session
    void dispose_session();
};

#endif
