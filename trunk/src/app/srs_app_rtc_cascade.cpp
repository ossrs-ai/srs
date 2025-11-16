//
// Copyright (c) 2013-2024 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_rtc_cascade.hpp>

#include <sstream>

#include <srs_app_config.hpp>
#include <srs_app_factory.hpp>
#include <srs_app_rtc_api.hpp>
#include <srs_app_rtc_conn.hpp>
#include <srs_app_rtc_network.hpp>
#include <srs_app_rtc_server.hpp>
#include <srs_app_server.hpp>
#include <srs_app_st.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_kernel_consts.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_json.hpp>
#include <srs_protocol_rtc_stun.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_protocol_sdp.hpp>
#include <srs_protocol_stream.hpp>
#include <srs_protocol_utility.hpp>

// Forward declarations for packet type detection functions
extern bool srs_is_stun(const uint8_t *data, size_t size);
extern bool srs_is_rtp_or_rtcp(const uint8_t *data, size_t len);
extern bool srs_is_rtcp(const uint8_t *data, size_t len);

// Max size of signaling.
const int kMaxSignalingSize = 64 * 1024;

SrsRtcCascadeManager::SrsRtcCascadeManager(ISrsRtcApiServer *server)
{
    conn_manager_ = _srs_conn_manager;
    server_ = server;
    app_factory_ = _srs_app_factory;
}

SrsRtcCascadeManager::~SrsRtcCascadeManager()
{
    conn_manager_ = NULL;
    server_ = NULL;
    app_factory_ = NULL;
}

srs_error_t SrsRtcCascadeManager::on_tcp_client(ISrsListener *listener, srs_netfd_t stfd)
{
    srs_error_t err = srs_success;

    int fd = srs_netfd_fileno(stfd);
    std::string ip = srs_get_peer_ip(fd);
    int port = srs_get_peer_port(fd);

    // From now on, we always handle the stfd, so we set the original one to NULL.
    // This prevents double-free if any operation below fails.
    srs_netfd_t stfd2 = stfd;
    stfd = NULL;

    // Create TCP connection wrapper
    SrsTcpConnection *tcp_conn = new SrsTcpConnection(stfd2);
    SrsBufferedReadWriter *io = new SrsBufferedReadWriter(tcp_conn);

    // Create cascade connection
    SrsRtcCascadeConn *raw_conn = new SrsRtcCascadeConn(conn_manager_, server_, io, ip, port);

    // For RTC cascade connection, use resource executor to manage the resource.
    SrsSharedResource<ISrsRtcCascadeConn> *conn = new SrsSharedResource<ISrsRtcCascadeConn>(raw_conn);
    SrsExecutorCoroutine *executor = new SrsExecutorCoroutine(conn_manager_, conn, raw_conn, raw_conn);
    raw_conn->setup_owner(executor, executor);
    if ((err = executor->start()) != srs_success) {
        srs_freep(executor);
        return srs_error_wrap(err, "start executor");
    }

    srs_trace("RTC cascade connection from %s:%d, fd=%d", ip.c_str(), port, fd);

    return err;
}

ISrsRtcCascadeConn::ISrsRtcCascadeConn()
{
}

ISrsRtcCascadeConn::~ISrsRtcCascadeConn()
{
}

SrsRtcCascadeConn::SrsRtcCascadeConn(ISrsResourceManager *conn_manager, ISrsRtcApiServer *server, ISrsProtocolReadWriter *skt, std::string cip, int port)
{
    trd_ = NULL;
    owner_cid_ = NULL;
    cid_ = _srs_context->get_id();

    session_ = NULL;

    ip_ = cip;
    port_ = port;
    skt_ = skt;
    delta_ = new SrsNetworkDelta();
    delta_->set_io(skt_, skt_);
    pkt_ = new char[kMaxSignalingSize];

    server_ = server;
    conn_manager_ = conn_manager;
    // Subscribe to connection manager to receive disposal notifications
    conn_manager_->subscribe(this);

    stat_ = _srs_stat;
}

SrsRtcCascadeConn::~SrsRtcCascadeConn()
{
    // Unsubscribe from connection manager before cleanup
    conn_manager_->unsubscribe(this);

    dispose_session();

    srs_freepa(pkt_);
    srs_freep(delta_);
    srs_freep(skt_);

    conn_manager_ = NULL;
    stat_ = NULL;
    server_ = NULL;
}

void SrsRtcCascadeConn::dispose_session()
{
    // Clean up session if it was created. The manager will handle the actual deletion.
    // Note: The destructor will also call expire(), but expire() is safe to call multiple times
    // because conn_manager_->remove() checks if the session is already in the zombie list
    if (session_) {
        session_->expire();
        session_ = NULL; // Set to NULL to avoid double-expire in destructor
    }
}

void SrsRtcCascadeConn::setup_owner(ISrsInterruptable *owner_coroutine, ISrsContextIdSetter *owner_cid)
{
    trd_ = owner_coroutine;
    owner_cid_ = owner_cid;
}

ISrsKbpsDelta *SrsRtcCascadeConn::delta()
{
    return delta_;
}

void SrsRtcCascadeConn::interrupt()
{
    if (trd_) {
        trd_->interrupt();
    }
}

std::string SrsRtcCascadeConn::desc()
{
    return "RtcCascade";
}

const SrsContextId &SrsRtcCascadeConn::get_id()
{
    return cid_;
}

std::string SrsRtcCascadeConn::remote_ip()
{
    return ip_;
}

void SrsRtcCascadeConn::on_executor_done(ISrsInterruptable *executor)
{
    trd_ = NULL;
}

void SrsRtcCascadeConn::on_before_dispose(ISrsResource *c)
{
    // Check if the resource being disposed is our session
    if (session_ && c == session_) {
        // Set session to NULL immediately to prevent use-after-free
        session_ = NULL;

        // Interrupt the thread to stop processing packets
        interrupt();
    }
}

void SrsRtcCascadeConn::on_disposing(ISrsResource *c)
{
    // Nothing to do here - all cleanup done in on_before_dispose
}

srs_error_t SrsRtcCascadeConn::cycle()
{
    srs_error_t err = do_cycle();

    // Update statistics
    stat_->on_disconnect(get_id().c_str(), err);
    stat_->kbps_add_delta(get_id().c_str(), delta_);

    // Clean up session if established
    dispose_session();

    // For timeout, treat as success
    if (srs_error_code(err) == ERROR_SOCKET_TIMEOUT) {
        srs_freep(err);
    }

    if (err == srs_success) {
        srs_trace("RTC cascade connection finished");
    } else {
        srs_warn("RTC cascade connection failed: %s", srs_error_summary(err).c_str());
        srs_freep(err);
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::do_cycle()
{
    srs_error_t err = srs_success;

    // Update context id
    _srs_context->set_id(cid_);
    owner_cid_->set_cid(cid_);

    // Phase 1: Handle JSON signaling
    if ((err = handle_signaling()) != srs_success) {
        return srs_error_wrap(err, "handle signaling");
    }

    // Phase 2: Handle WebRTC media packets
    while (true) {
        if (!trd_) {
            return err;
        }
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "cascade conn pull");
        }

        int npkt = kMaxSignalingSize;
        if ((err = read_packet(pkt_, &npkt)) != srs_success) {
            return srs_error_wrap(err, "read packet");
        }

        if ((err = on_packet(pkt_, npkt)) != srs_success) {
            return srs_error_wrap(err, "process packet");
        }
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::handle_signaling()
{
    srs_error_t err = srs_success;

    // Read first packet which should be JSON signaling
    int npkt = kMaxSignalingSize;
    if ((err = read_packet(pkt_, &npkt)) != srs_success) {
        return srs_error_wrap(err, "read signaling packet");
    }

    // Check if it's JSON (starts with '{')
    if (npkt < 1 || pkt_[0] != '{') {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "first packet must be JSON signaling, got byte 0x%02x", (uint8_t)pkt_[0]);
    }

    // Parse and handle JSON message
    if ((err = on_json_message(pkt_, npkt)) != srs_success) {
        return srs_error_wrap(err, "handle json message");
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::read_packet(char *pkt, int *nb_pkt)
{
    srs_error_t err = srs_success;

    // Read length in 2 bytes (RFC 4571)
    ssize_t nread = 0;
    uint8_t b[2];
    if ((err = skt_->read_fully((char *)b, sizeof(b), &nread)) != srs_success) {
        return srs_error_wrap(err, "read packet length");
    }

    // Parse length (big-endian)
    uint16_t npkt = uint16_t(b[0]) << 8 | uint16_t(b[1]);
    if (npkt > *nb_pkt) {
        return srs_error_new(ERROR_RTC_TCP_SIZE, "packet size=%u exceeds buffer %d", npkt, *nb_pkt);
    }

    // Read packet payload
    if ((err = skt_->read_fully(pkt, npkt, &nread)) != srs_success) {
        return srs_error_wrap(err, "read packet payload %d bytes", npkt);
    }

    *nb_pkt = npkt;

    return err;
}

srs_error_t SrsRtcCascadeConn::on_packet(char *pkt, int nb_pkt)
{
    srs_error_t err = srs_success;

    // Session must be established at this point
    if (!session_) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "session not established or disposed");
    }

    // Identify packet type by first byte
    bool is_stun = srs_is_stun((uint8_t *)pkt, nb_pkt);
    bool is_rtp_or_rtcp = srs_is_rtp_or_rtcp((uint8_t *)pkt, nb_pkt);
    bool is_rtcp = srs_is_rtcp((uint8_t *)pkt, nb_pkt);

    // Keep session alive
    session_->alive();

    // Handle STUN packets
    if (is_stun) {
        SrsStunPacket ping;
        if ((err = ping.decode(pkt, nb_pkt)) != srs_success) {
            return srs_error_wrap(err, "decode stun packet");
        }
        return session_->cascade()->on_stun(&ping, pkt, nb_pkt);
    }

    // Handle RTP packets
    if (is_rtp_or_rtcp && !is_rtcp) {
        return session_->cascade()->on_rtp(pkt, nb_pkt);
    }

    // Handle RTCP packets
    if (is_rtcp) {
        return session_->cascade()->on_rtcp(pkt, nb_pkt);
    }

    // Handle DTLS packets
    return session_->cascade()->on_dtls(pkt, nb_pkt);
}

srs_error_t SrsRtcCascadeConn::on_json_message(char *data, int nb_data)
{
    srs_error_t err = srs_success;

    // Parse JSON
    SrsJsonAny *json_raw = SrsJsonAny::loads(std::string(data, nb_data));
    if (!json_raw || !json_raw->is_object()) {
        srs_freep(json_raw);
        return srs_error_new(ERROR_RTC_TCP_PACKET, "invalid JSON message");
    }
    SrsUniquePtr<SrsJsonAny> json(json_raw);

    SrsJsonObject *obj = json->to_object();

    // Get message type
    SrsJsonAny *type_any = obj->get_property("type");
    if (!type_any || !type_any->is_string()) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "missing or invalid 'type' field");
    }

    std::string type = type_any->to_str();

    // Handle different message types
    if (type == "publish") {
        return on_publish_request(obj);
    } else if (type == "play") {
        return on_play_request(obj);
    } else {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "unknown message type: %s", type.c_str());
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::on_publish_request(SrsJsonObject *req)
{
    srs_error_t err = srs_success;

    // Extract stream path
    SrsJsonAny *stream_any = req->get_property("stream");
    if (!stream_any || !stream_any->is_string()) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "missing or invalid 'stream' field");
    }
    std::string stream_url = stream_any->to_str();

    // Extract SDP offer
    SrsJsonAny *sdp_any = req->get_property("sdp");
    if (!sdp_any || !sdp_any->is_string()) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "missing or invalid 'sdp' field");
    }
    std::string sdp_offer = sdp_any->to_str();

    // Parse stream URL (format: /app/stream)
    size_t pos = stream_url.find('/', 1);
    if (pos == std::string::npos) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "invalid stream URL format: %s", stream_url.c_str());
    }

    std::string app = stream_url.substr(1, pos - 1);
    std::string stream = stream_url.substr(pos + 1);

    // Create RTC session using session manager
    SrsRtcUserConfig ruc;
    ruc.req_ = new SrsRequest(); // ruc will own and free this
    ruc.req_->app_ = app;
    ruc.req_->stream_ = stream;
    ruc.req_->vhost_ = SRS_CONSTS_RTMP_DEFAULT_VHOST;
    ruc.publish_ = true;
    ruc.dtls_ = true;
    ruc.srtp_ = false; // Use plaintext RTP/RTCP
    ruc.remote_sdp_str_ = sdp_offer;
    if ((err = ruc.remote_sdp_.parse(sdp_offer)) != srs_success) {
        return srs_error_wrap(err, "parse sdp");
    }

    SrsSdp local_sdp;
    local_sdp.session_config_.dtls_role_ = "passive";

    if ((err = server_->create_rtc_session(&ruc, local_sdp, &session_)) != srs_success) {
        return srs_error_wrap(err, "create publish session");
    }

    // Set the socket for cascade network - use this connection as the socket
    // since it implements ISrsProtocolReadWriter with RFC 4571 framing
    SrsRtcCascadeNetwork *network = dynamic_cast<SrsRtcCascadeNetwork *>(session_->cascade());
    network->set_socket(this);

    // Encode local SDP to string
    std::ostringstream os;
    if ((err = local_sdp.encode(os)) != srs_success) {
        return srs_error_wrap(err, "encode local sdp");
    }
    std::string local_sdp_str = os.str();

    // Build JSON response
    SrsJsonObject *resp_raw = SrsJsonAny::object();
    SrsUniquePtr<SrsJsonObject> resp(resp_raw);

    resp->set("code", SrsJsonAny::integer(0));
    resp->set("session", SrsJsonAny::str(session_->username().c_str()));
    resp->set("sdp", SrsJsonAny::str(local_sdp_str.c_str()));

    // Send response
    if ((err = send_json_response(resp.get())) != srs_success) {
        return srs_error_wrap(err, "send publish response");
    }

    srs_trace("RTC cascade publish session created, stream=%s, username=%s", stream_url.c_str(), session_->username().c_str());

    return err;
}

srs_error_t SrsRtcCascadeConn::on_play_request(SrsJsonObject *req)
{
    srs_error_t err = srs_success;

    // Extract stream path
    SrsJsonAny *stream_any = req->get_property("stream");
    if (!stream_any || !stream_any->is_string()) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "missing or invalid 'stream' field");
    }
    std::string stream_url = stream_any->to_str();

    // Extract SDP offer
    SrsJsonAny *sdp_any = req->get_property("sdp");
    if (!sdp_any || !sdp_any->is_string()) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "missing or invalid 'sdp' field");
    }
    std::string sdp_offer = sdp_any->to_str();

    // Parse stream URL (format: /app/stream)
    size_t pos = stream_url.find('/', 1);
    if (pos == std::string::npos) {
        return srs_error_new(ERROR_RTC_TCP_PACKET, "invalid stream URL format: %s", stream_url.c_str());
    }

    std::string app = stream_url.substr(1, pos - 1);
    std::string stream = stream_url.substr(pos + 1);

    // Create RTC session using session manager
    SrsRtcUserConfig ruc;
    ruc.req_ = new SrsRequest(); // ruc will own and free this
    ruc.req_->app_ = app;
    ruc.req_->stream_ = stream;
    ruc.req_->vhost_ = SRS_CONSTS_RTMP_DEFAULT_VHOST;
    ruc.publish_ = false;
    ruc.dtls_ = true;
    ruc.srtp_ = false; // Use plaintext RTP/RTCP
    ruc.remote_sdp_str_ = sdp_offer;
    if ((err = ruc.remote_sdp_.parse(sdp_offer)) != srs_success) {
        return srs_error_wrap(err, "parse sdp");
    }

    SrsSdp local_sdp;
    local_sdp.session_config_.dtls_role_ = "passive";

    if ((err = server_->create_rtc_session(&ruc, local_sdp, &session_)) != srs_success) {
        return srs_error_wrap(err, "create play session");
    }

    // Set the socket for cascade network - use this connection as the socket
    // since it implements ISrsProtocolReadWriter with RFC 4571 framing
    SrsRtcCascadeNetwork *network = dynamic_cast<SrsRtcCascadeNetwork *>(session_->cascade());
    network->set_socket(this);

    // Encode local SDP to string
    std::ostringstream os;
    if ((err = local_sdp.encode(os)) != srs_success) {
        return srs_error_wrap(err, "encode local sdp");
    }
    std::string local_sdp_str = os.str();

    // Build JSON response
    SrsJsonObject *resp_raw = SrsJsonAny::object();
    SrsUniquePtr<SrsJsonObject> resp(resp_raw);

    resp->set("code", SrsJsonAny::integer(0));
    resp->set("session", SrsJsonAny::str(session_->username().c_str()));
    resp->set("sdp", SrsJsonAny::str(local_sdp_str.c_str()));

    // Send response
    if ((err = send_json_response(resp.get())) != srs_success) {
        return srs_error_wrap(err, "send play response");
    }

    srs_trace("RTC cascade play session created, stream=%s, username=%s", stream_url.c_str(), session_->username().c_str());

    return err;
}

srs_error_t SrsRtcCascadeConn::send_json_response(SrsJsonObject *resp)
{
    srs_error_t err = srs_success;

    // Serialize JSON to string
    std::string json_str = resp->dumps();

    // Write as RFC 4571 framed packet
    if ((err = write_packet(json_str.c_str(), json_str.length())) != srs_success) {
        return srs_error_wrap(err, "write json response");
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::write_packet(const char *data, int nb_data)
{
    srs_error_t err = srs_success;

    // Write length prefix (2 bytes, big-endian)
    uint8_t len_buf[2];
    len_buf[0] = (nb_data >> 8) & 0xFF;
    len_buf[1] = nb_data & 0xFF;

    if ((err = skt_->write((char *)len_buf, sizeof(len_buf), NULL)) != srs_success) {
        return srs_error_wrap(err, "write packet length");
    }

    // Write packet payload
    if ((err = skt_->write((void *)data, nb_data, NULL)) != srs_success) {
        return srs_error_wrap(err, "write packet payload %d bytes", nb_data);
    }

    return err;
}

// ISrsProtocolReadWriter implementation - provides RFC 4571 framing

void SrsRtcCascadeConn::set_recv_timeout(srs_utime_t tm)
{
    skt_->set_recv_timeout(tm);
}

srs_utime_t SrsRtcCascadeConn::get_recv_timeout()
{
    return skt_->get_recv_timeout();
}

srs_error_t SrsRtcCascadeConn::read_fully(void *buf, size_t size, ssize_t *nread)
{
    srs_error_t err = srs_success;

    // Read RFC 4571 framed packet: 2-byte length + payload
    ssize_t nread_local = 0;
    uint8_t len_buf[2];
    if ((err = skt_->read_fully((char *)len_buf, sizeof(len_buf), &nread_local)) != srs_success) {
        return srs_error_wrap(err, "read packet length");
    }

    // Parse length (big-endian)
    uint16_t pkt_len = uint16_t(len_buf[0]) << 8 | uint16_t(len_buf[1]);
    if (pkt_len > size) {
        return srs_error_new(ERROR_RTC_TCP_SIZE, "packet size=%u exceeds buffer %d", pkt_len, (int)size);
    }

    // Read packet payload
    if ((err = skt_->read_fully(buf, pkt_len, &nread_local)) != srs_success) {
        return srs_error_wrap(err, "read packet payload %d bytes", pkt_len);
    }

    if (nread) {
        *nread = pkt_len;
    }

    return err;
}

int64_t SrsRtcCascadeConn::get_recv_bytes()
{
    return skt_->get_recv_bytes();
}

int64_t SrsRtcCascadeConn::get_send_bytes()
{
    return skt_->get_send_bytes();
}

srs_error_t SrsRtcCascadeConn::read(void *buf, size_t size, ssize_t *nread)
{
    // For RFC 4571 framing, read() is the same as read_fully() - we always read complete frames
    return read_fully(buf, size, nread);
}

void SrsRtcCascadeConn::set_send_timeout(srs_utime_t tm)
{
    skt_->set_send_timeout(tm);
}

srs_utime_t SrsRtcCascadeConn::get_send_timeout()
{
    return skt_->get_send_timeout();
}

srs_error_t SrsRtcCascadeConn::write(void *buf, size_t size, ssize_t *nwrite)
{
    srs_error_t err = srs_success;

    // Write RFC 4571 framed packet: 2-byte length + payload
    if ((err = write_packet((const char *)buf, size)) != srs_success) {
        return srs_error_wrap(err, "write framed packet");
    }

    if (nwrite) {
        *nwrite = size;
    }

    return err;
}

srs_error_t SrsRtcCascadeConn::writev(const iovec *iov, int iov_size, ssize_t *nwrite)
{
    srs_error_t err = srs_success;

    // Calculate total size
    size_t total_size = 0;
    for (int i = 0; i < iov_size; i++) {
        total_size += iov[i].iov_len;
    }

    // Write length prefix (2 bytes, big-endian)
    uint8_t len_buf[2];
    len_buf[0] = (total_size >> 8) & 0xFF;
    len_buf[1] = total_size & 0xFF;

    if ((err = skt_->write((char *)len_buf, sizeof(len_buf), NULL)) != srs_success) {
        return srs_error_wrap(err, "write packet length");
    }

    // Write all iovec buffers
    ssize_t nwrite_local = 0;
    if ((err = skt_->writev(iov, iov_size, &nwrite_local)) != srs_success) {
        return srs_error_wrap(err, "write packet payload %d bytes", (int)total_size);
    }

    if (nwrite) {
        *nwrite = total_size;
    }

    return err;
}
