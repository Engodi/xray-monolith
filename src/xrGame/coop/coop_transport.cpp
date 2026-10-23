////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_transport.cpp
//	Created 	: 23.10.2026
//	Description : F0-02 transport bring-up for one harness instance
////////////////////////////////////////////////////////////////////////////

#include "coop/coop_transport.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "xnt/transport.h"
#include "xrn/core/transport_link.hpp"

namespace coop {
namespace {

namespace fs = std::filesystem;

// Published ephemeral port of the host, so a client with `XRN_HARNESS_HOST_PORT=0`
// can reach it. Fixed name: the client does not know the instance name.
constexpr const char* kPortFileName = "transport.port";
constexpr const char* kDefaultAddress = "127.0.0.1";
// The transport's own default (5 s) is shorter than a headless level load: a
// client started together with the host would give up before the host listens.
// 20 s matches the core harness (`peer_timeout_ms`); the env variable overrides.
constexpr std::uint32_t kDefaultTimeoutMs = 20000;
constexpr std::uint32_t kBackendStatusBytes = 512;

std::string env_value(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::uint32_t env_uint(const char* name, std::uint32_t fallback) {
    const std::string text = env_value(name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
}

std::string describe_backends() {
    char buffer[kBackendStatusBytes] = {};
    xnt_backend_status(buffer, kBackendStatusBytes);
    return std::string(buffer);
}

// The C ABI header must match the transport it is bound to: a silent mismatch
// would corrupt `xnt_config`/`xnt_event` on the boundary.
bool verify_abi(std::string& error) {
    const std::uint32_t transport_abi = xnt_abi_version();
    if (transport_abi != XNT_ABI_VERSION) {
        std::ostringstream text;
        text << "transport ABI mismatch: core expects " << XNT_ABI_VERSION << ", transport is "
             << transport_abi;
        error = text.str();
        return false;
    }
    return true;
}

std::string transport_error(const char* context) {
    const char* const last = xnt_last_error();
    std::string text = context;
    if (last != nullptr && *last != '\0') {
        text += ": ";
        text += last;
    }
    return text;
}

xnt_config make_config(const STransportConfig& config) {
    xnt_config raw{};
    raw.struct_size = static_cast<std::uint32_t>(sizeof(raw));
    raw.timeout_ms = config.timeout_ms;
    raw.keepalive_ms = config.keepalive_ms;
    return raw;
}

bool publish_port(const STransportConfig& config, std::uint16_t port, std::string& error) {
    if (config.run_dir.empty()) {
        error = "cannot publish the ephemeral port: no run directory";
        return false;
    }
    std::error_code code;
    const fs::path directory(config.run_dir);
    fs::create_directories(directory, code);
    const fs::path path = directory / kPortFileName;
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot publish the transport port: " + temporary.string();
            return false;
        }
        output << static_cast<unsigned>(port) << '\n';
        if (!output) {
            error = "cannot write the transport port: " + temporary.string();
            return false;
        }
    }
    fs::rename(temporary, path, code);
    if (code) {
        error = "cannot rename the transport port file: " + code.message();
        return false;
    }
    return true;
}

std::uint16_t read_published_port(const STransportConfig& config) {
    if (config.run_dir.empty()) {
        return 0;
    }
    std::ifstream input(fs::path(config.run_dir) / kPortFileName, std::ios::binary);
    if (!input) {
        return 0;
    }
    unsigned port = 0;
    input >> port;
    if (!input || port > 0xFFFFu) {
        return 0;
    }
    return static_cast<std::uint16_t>(port);
}

}  // namespace

STransportConfig transport_config_from_env() {
    STransportConfig config;
    // The harness profile is armed by the runner: `_ROLE` and `_RUN_DIR` are the
    // only variables every instance gets. Default = real UDP: a headless run that
    // silently stayed in-process would fake a network session.
    config.host = env_value("XRN_HARNESS_ROLE") == "host";
    config.udp = env_value("XRN_HARNESS_NET") != "inprocess";
    const std::string address = env_value("XRN_HARNESS_HOST");
    config.address = address.empty() ? kDefaultAddress : address;
    config.instance = env_value("XRN_HARNESS_INSTANCE");
    config.run_dir = env_value("XRN_HARNESS_RUN_DIR");
    if (config.host) {
        config.port = static_cast<std::uint16_t>(env_uint("XRN_HARNESS_PORT", 0) & 0xFFFFu);
    } else {
        config.port = static_cast<std::uint16_t>(env_uint("XRN_HARNESS_HOST_PORT", 0) & 0xFFFFu);
    }
    config.timeout_ms = env_uint("XRN_HARNESS_NET_TIMEOUT_MS", kDefaultTimeoutMs);
    config.keepalive_ms = env_uint("XRN_HARNESS_NET_KEEPALIVE_MS", 0);
    return config;
}

struct CTransport::SImpl {
    bool host = false;
    bool started = false;
    std::uint16_t port = 0;
    std::string error;
    std::string description;
    xnt_host* host_endpoint = nullptr;
    xnt_client* client_endpoint = nullptr;
    // The link is a view of the owning smart pointer below; `reset()` clears the
    // view first, so no pointer outlives its endpoint.
    xrn::core::IMessageLink* link = nullptr;
    std::unique_ptr<xrn::core::HostTransportLink> host_link;
    std::unique_ptr<xrn::core::ClientTransportLink> client_link;

    void reset() {
        link = nullptr;
        host_link.reset();
        client_link.reset();
        if (host_endpoint != nullptr) {
            xnt_host_destroy(host_endpoint);
            host_endpoint = nullptr;
        }
        if (client_endpoint != nullptr) {
            xnt_client_destroy(client_endpoint);
            client_endpoint = nullptr;
        }
        started = false;
    }
};

CTransport::CTransport() : m_impl(new SImpl()) {}

CTransport::~CTransport() { m_impl->reset(); }

bool CTransport::start(const STransportConfig& config) {
    SImpl& impl = *m_impl;
    if (impl.started) {
        return true;
    }
    impl.host = config.host;
    impl.error.clear();
    impl.description.clear();

    if (!verify_abi(impl.error)) {
        return false;
    }

    std::uint16_t port = config.port;
    if (!config.host && port == 0) {
        // Ephemeral host port: the host published it in the run directory.
        port = read_published_port(config);
        if (port == 0) {
            impl.error = "cannot resolve the host port: " + std::string(kPortFileName) +
                         " is missing in the run directory";
            return false;
        }
    }

    const xnt_config raw = make_config(config);
    if (config.host) {
        impl.host_endpoint = xnt_host_create(config.address.c_str(), port, &raw);
        if (impl.host_endpoint == nullptr) {
            impl.error = transport_error("xnt_host_create");
            return false;
        }
        impl.port = xnt_host_local_port(impl.host_endpoint);
        impl.host_link.reset(new xrn::core::HostTransportLink(impl.host_endpoint));
        impl.link = impl.host_link.get();
        if (port == 0 && !publish_port(config, impl.port, impl.error)) {
            impl.reset();
            return false;
        }
    } else {
        impl.client_endpoint = xnt_client_create(config.address.c_str(), port, &raw);
        if (impl.client_endpoint == nullptr) {
            impl.error = transport_error("xnt_client_create");
            return false;
        }
        impl.port = port;
        impl.client_link.reset(new xrn::core::ClientTransportLink(impl.client_endpoint));
        impl.link = impl.client_link.get();
    }

    std::ostringstream description;
    description << "xnt " << xnt_version() << " (abi " << xnt_abi_version() << ") udp "
                << (config.host ? "host " : "client ") << config.address << ':' << impl.port
                << " - backends: " << describe_backends();
    impl.description = description.str();
    impl.started = true;
    return true;
}

void CTransport::stop() {
    SImpl& impl = *m_impl;
    impl.reset();
    impl.description.clear();
    impl.error.clear();
    impl.port = 0;
}

bool CTransport::running() const noexcept {
    return m_impl->started && m_impl->link != nullptr;
}

xrn::core::IMessageLink* CTransport::link() const noexcept { return m_impl->link; }

const std::string& CTransport::error() const noexcept { return m_impl->error; }

const std::string& CTransport::description() const noexcept { return m_impl->description; }

std::uint16_t CTransport::port() const noexcept { return m_impl->port; }

bool CTransport::client_closed() const noexcept {
    const SImpl& impl = *m_impl;
    if (impl.client_endpoint == nullptr) {
        return false;
    }
    return xnt_client_state(impl.client_endpoint) == XNT_STATE_CLOSED;
}

}  // namespace coop
