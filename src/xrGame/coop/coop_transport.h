////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_transport.h
//	Created 	: 23.10.2026
//	Description : F0-02 transport bring-up for one harness instance
//
//	The coop core talks to peers exclusively through `xrn::core::IMessageLink`.
//	Two implementations exist in the core:
//
//	  * `InProcessNetwork` - deterministic queues, host and client in one thread;
//	  * `HostTransportLink` / `ClientTransportLink` - the real F0-02 transport
//	    (`xnt`: UDP sockets carrying the reliability/fragmentation layer, with the
//	    GameNetworkingSockets seam as its LAN backend).
//
//	This file is the only place where an engine process obtains the real link: it
//	loads the transport (C ABI, `xnt/transport.h`), verifies the ABI, creates the
//	host or the client endpoint and hands the link to the runtime. Errors are
//	reported, never faked - a failed bring-up leaves the caller without a link and
//	with `error()` set, so the run artifact shows the failure instead of silently
//	falling back to the in-process queues.
//
//	Profile (the F0-11 harness environment; loopback by default, so a local e2e
//	needs no network setup - the debug profile just works):
//
//	  XRN_HARNESS_NET        `udp` (default in a harness profile) | `inprocess`
//	  XRN_HARNESS_HOST       address the host listens on / clients connect to
//	  XRN_HARNESS_PORT       host: port to bind (0 = ephemeral, published below)
//	  XRN_HARNESS_HOST_PORT  clients: port the host binds
//	  XRN_HARNESS_RUN_DIR    where an ephemeral host port is published
//
//	An ephemeral host port is published as `<run-dir>/transport.port`, so a client
//	with `XRN_HARNESS_HOST_PORT=0` can still find the host.
//
//	NFR-ARCH-9: the engine keeps a thin adapter; the transport stays a separate,
//	engine-free module of the coop repository.
////////////////////////////////////////////////////////////////////////////

#ifndef COOP_TRANSPORT_H_INCLUDED
#define COOP_TRANSPORT_H_INCLUDED

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "xrn/core/link.hpp"

namespace coop {

// How this process reaches its peers. Read once from the harness environment.
struct STransportConfig {
    // False only when the profile explicitly asks for the in-process queues:
    // every real harness run must go over the network.
    bool udp = true;
    bool host = false;
    // Host: bind address. Client: address of the host.
    std::string address = "127.0.0.1";
    // Host: port to bind. Client: port of the host (0 = published by the host).
    std::uint16_t port = 0;
    std::string instance;
    std::string run_dir;
    // 0 keeps the transport defaults (250 ms keepalive). `timeout_ms` defaults to
    // 20 s from the environment: a headless host may load a level for seconds
    // before it listens, and a client must not give up before that.
    std::uint32_t timeout_ms = 0;
    std::uint32_t keepalive_ms = 0;
};

// Reads the harness environment into a config. Pure: no socket is opened, so the
// profile selection is unit-testable.
[[nodiscard]] STransportConfig transport_config_from_env();

// One transport endpoint (host or client) plus the link the core session uses.
//
// Single-threaded, like the transport itself: `poll` runs inside the core tick.
class CTransport final {
public:
    CTransport();
    ~CTransport();

    CTransport(const CTransport&) = delete;
    CTransport& operator=(const CTransport&) = delete;
    CTransport(CTransport&&) = delete;
    CTransport& operator=(CTransport&&) = delete;

    // Creates the endpoint of `config.host` role and the matching core link.
    // False = no link, `error()` explains why (including the transport's own
    // last error). A client whose host is not up yet still succeeds: the
    // transport retries inside its connect timeout, the session polls.
    bool start(const STransportConfig& config);

    // Tears the endpoint down. The link must outlive its session: call this after
    // the session that owns the link is gone.
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] xrn::core::IMessageLink* link() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    // "xnt 0.1.0 (abi 1) udp host 127.0.0.1:47000 - backends: ..." for the log and
    // the debug surface. Empty until a successful start().
    [[nodiscard]] const std::string& description() const noexcept;
    // Host: the port actually bound. Client: the port it connects to.
    [[nodiscard]] std::uint16_t port() const noexcept;
    // Client only: the transport gave up (connect timeout / host went away). The
    // owner decides whether to recreate the endpoint; the transport never
    // reconnects behind its back.
    [[nodiscard]] bool client_closed() const noexcept;

private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};

}  // namespace coop

#endif  // COOP_TRANSPORT_H_INCLUDED
