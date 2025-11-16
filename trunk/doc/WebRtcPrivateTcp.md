# WebRTC Private TCP Protocol

## Problem Statement

SRS ecosystem includes various components that need efficient, low-latency communication with SRS servers:

- **rust-turbo**: High-performance Rust-based media processing component that handles WebRTC with DTLS/SRTP for external clients
- **Internal Services**: Custom media processing or analysis services within your infrastructure

While SRS supports standard protocols (RTMP, HTTP-FLV, WebRTC over HTTP API), these have limitations for **internal system communication**:

1. **RTMP**: Legacy protocol, limited codec support, higher latency
2. **HTTP-FLV**: Pull-only, not suitable for bidirectional communication
3. **WebRTC over HTTP API**: Requires separate HTTP signaling + media channels, designed for browsers

## Solution: WebRTC Private TCP Protocol

A **unified TCP channel** (default port 9999) that combines:

- **JSON signaling**: Simple WHIP/WHEP-like protocol for session establishment
- **WebRTC media**: Standard STUN/DTLS/RTP/RTCP transport (plaintext, not encrypted)
- **Single connection**: Both signaling and media over one TCP socket

This protocol is designed for:

1. **Internal Components**: rust-turbo connecting to SRS for high-performance media processing
2. **Custom Services**: Third-party services that need efficient WebRTC access to SRS

**Note**: This is an **internal protocol** for SRS ecosystem components.

## Transport Layer

The protocol uses **RFC 4571** framing over TCP:

```
┌─────────────────┬──────────────────────────────┐
│  Length (2B)    │  Packet Content (variable)   │
│  Big-endian     │  JSON/STUN/DTLS/RTP/RTCP     │
└─────────────────┴──────────────────────────────┘
```

- **Length**: 16-bit unsigned integer, network byte order (big-endian)
- **Content**: Variable-length packet data

## Packet Type Multiplexing

Different packet types are distinguished by **first byte inspection**:

| First Byte Range | Packet Type | Description |
|-----------------|-------------|-------------|
| `0x00 - 0x01` | STUN | ICE connectivity checks |
| `0x14 - 0x3F` | DTLS | Key exchange (20-63) |
| `0x80 - 0xBF` | RTP/RTCP | Media packets (V=2) |
| `0x7B` | JSON | Signaling (`{`) |

**Key Insight**: JSON packets starting with `{` (0x7B) do **not conflict** with any WebRTC packet types, enabling safe multiplexing. All JSON messages must be objects (not arrays).

## Connection Flow

**Example: rust-turbo Publishing to SRS**

```
rust-turbo                                  SRS Server
     │                                         │
     ├──────── TCP Connect to :9999 ──────────>│
     │                                         │
     ├──── JSON Signaling (Publish) ──────────>│
     │<───── JSON Response (SDP Answer) ───────┤
     │                                         │
     ├────────── STUN Binding Request ────────>│
     │<───────── STUN Binding Response ────────┤
     │                                         │
     ├────────── DTLS ClientHello ────────────>│
     │<───────── DTLS ServerHello ─────────────┤
     │      (DTLS Handshake - Key Exchange)    │
     │                                         │
     ├────────── RTP packets (plaintext) ─────>│
     ├────────── RTCP packets (plaintext) ────>│
     │<───────── RTCP feedback (plaintext) ────┤
     │                                         │
```

**Note**: After DTLS handshake, RTP/RTCP packets are sent in **plaintext** (not encrypted with SRTP/SRTCP).

**Why DTLS handshake but plaintext RTP/RTCP?**

This protocol is designed to work with **rust-turbo**, which handles WebRTC connections with external clients using full DTLS/SRTP encryption. The DTLS handshake between SRS and rust-turbo allows rust-turbo to obtain the necessary keys for SRTP encryption/decryption with external clients. However, the communication between SRS and rust-turbo itself uses **plaintext RTP/RTCP** because:

1. **Internal Communication**: SRS and rust-turbo are internal system components, typically on the same network or trusted infrastructure
2. **Performance**: Plaintext transport reduces CPU overhead for encryption/decryption between internal components
3. **Simplicity**: Easier to debug and monitor internal traffic without encryption overhead

For other use scenarios, you can implement your own service using this protocol. Since it's designed for **internal system communication**, plaintext is acceptable and provides better performance.

## JSON Message Format

All JSON messages are framed with RFC 4571 length prefix:

```
┌──────┬────────────────────────┐
│ 0x00 │ 0x4A (74 bytes)        │  ← Length
├──────┴────────────────────────┤
│ { "type": "publish", ... }    │  ← JSON content
└───────────────────────────────┘
```

## JSON Publish Request

**Direction**: Publisher → Receiver

```json
{
  "type": "publish",
  "stream": "/live/livestream",
  "sdp": "v=0\r\no=- 1234567890 2 IN IP4 192.168.1.100\r\ns=SRSPublishSession\r\nt=0 0\r\na=group:BUNDLE 0 1\r\na=ice-lite\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:4ZcD\r\na=ice-pwd:bILRCdg4YchH1OqRIvvscZAP\r\na=fingerprint:sha-256 ...\r\na=setup:actpass\r\na=mid:0\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 106\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:4ZcD\r\na=ice-pwd:bILRCdg4YchH1OqRIvvscZAP\r\na=fingerprint:sha-256 ...\r\na=setup:actpass\r\na=mid:1\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:106 H264/90000\r\na=fmtp:106 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
}
```

**Fields**:

- `type`: Must be `"publish"`
- `stream`: Stream path (e.g., `/live/livestream`, `/app/stream`)
- `sdp`: SDP offer in standard WebRTC format

## JSON Publish Response

**Direction**: Receiver → Publisher

**Success Response**:

```json
{
  "code": 0,
  "session": "4ZcD:bILRCdg4YchH1OqRIvvscZAP",
  "sdp": "v=0\r\no=- 9876543210 2 IN IP4 192.168.1.200\r\ns=SRSReceiveSession\r\nt=0 0\r\na=group:BUNDLE 0 1\r\na=ice-lite\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 192.168.1.200\r\na=ice-ufrag:bILR\r\na=ice-pwd:4ZcDYchH1OqRIvvscZAP\r\na=fingerprint:sha-256 ...\r\na=setup:passive\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 106\r\nc=IN IP4 192.168.1.200\r\na=ice-ufrag:bILR\r\na=ice-pwd:4ZcDYchH1OqRIvvscZAP\r\na=fingerprint:sha-256 ...\r\na=setup:passive\r\na=mid:1\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:106 H264/90000\r\na=fmtp:106 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
}
```

**Error Response**:

```json
{
  "code": 1001,
  "message": "Stream already publishing",
  "details": "Stream /live/livestream is busy"
}
```

**Fields**:

- `code`: 0 for success, non-zero for error
- `session`: ICE username (ufrag pair) for session identification
- `sdp`: SDP answer in standard WebRTC format
- `message`: Error message (only for errors)
- `details`: Detailed error information (optional)

## JSON Play Request

**Direction**: Player → Publisher

```json
{
  "type": "play",
  "stream": "/live/livestream",
  "sdp": "v=0\r\no=- 1111111111 2 IN IP4 192.168.1.150\r\ns=SRSPlaySession\r\nt=0 0\r\na=group:BUNDLE 0 1\r\na=ice-lite\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:xYz9\r\na=ice-pwd:pQrS1234567890AbCdEf\r\na=fingerprint:sha-256 ...\r\na=setup:actpass\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 106\r\nc=IN IP4 0.0.0.0\r\na=ice-ufrag:xYz9\r\na=ice-pwd:pQrS1234567890AbCdEf\r\na=fingerprint:sha-256 ...\r\na=setup:actpass\r\na=mid:1\r\na=recvonly\r\na=rtcp-mux\r\na=rtpmap:106 H264/90000\r\n"
}
```

**Fields**: Same as publish request, but `type` is `"play"` and SDP direction is `recvonly`.

## JSON Play Response

**Direction**: Publisher → Player

Same format as publish response.

## WebRTC Media Protocol

After JSON signaling completes, the connection follows standard WebRTC flow:

1. ICE Connectivity (STUN)

**STUN Binding Request** (first byte: 0x00):

```
┌──────┬──────┬────────────────────────────────┐
│ 0x00 │ 0x5C │ STUN Binding Request (92 bytes)│
└──────┴──────┴────────────────────────────────┘
```

**STUN Binding Response** (first byte: 0x01):

```
┌──────┬──────┬────────────────────────────────┐
│ 0x00 │ 0x68 │ STUN Binding Response (104 B)  │
└──────┴──────┴────────────────────────────────┘
```

Session lookup: STUN username attribute contains ICE ufrag pair from SDP.

2. DTLS Handshake

**DTLS ClientHello** (first byte: 0x16 = 22):

```
┌──────┬──────┬────────────────────────────────┐
│ 0x01 │ 0x2A │ DTLS ClientHello (298 bytes)   │
└──────┴──────┴────────────────────────────────┘
```

DTLS handshake is performed to exchange keys. rust-turbo uses these keys to set up SRTP encryption/decryption for its external WebRTC clients. However, the actual media transport **between SRS and rust-turbo** uses **plaintext RTP/RTCP** (not encrypted).

3. RTP/RTCP Media Transport (Plaintext)

**RTP Packet** (first byte: 0x80-0x9F, V=2, no marker):

```
┌──────┬──────┬────────────────────────────────┐
│ 0x05 │ 0xDC │ RTP packet (1500 bytes)        │
└──────┴──────┴────────────────────────────────┘
```

**RTCP Packet** (first byte: 0x80-0x9F, PT=192-223):

```
┌──────┬──────┬────────────────────────────────┐
│ 0x00 │ 0x40 │ RTCP packet (64 bytes)         │
└──────┴──────┴────────────────────────────────┘
```

**Important**: RTP/RTCP packets are sent in **plaintext** without SRTP/SRTCP encryption. The DTLS handshake provides keys for rust-turbo to encrypt/decrypt SRTP with external clients, but the transport between rust-turbo and SRS uses plaintext for performance and simplicity in internal communication.

## SRS Server Configuration

Enable WebRTC Private TCP listener on SRS server:

```nginx
rtc_server {
    enabled on;

    # WebRTC Private TCP listener for internal components
    private_tcp {
        enabled on;
        listen 9999;
    }
}
```

