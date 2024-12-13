/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 Intel Corporation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <getopt.h>
#include <thread>

#include "api_server_tcp.h"

#include <csignal>
#include "concurrency.h"
#include "client_api.h"
#include "logger.h"
#include "manager_local.h"
#include "proxy_api.h"
#include "metrics_collector.h"

#include <execinfo.h>
#include <dlfcn.h>
#include "mesh/conn_rdma_rx.h"
#include "mesh/conn_rdma_tx.h"
#include <vector>
#include <tuple>
#include <iomanip>
#include <thread>

#ifndef IMTL_CONFIG_PATH
#define IMTL_CONFIG_PATH "./imtl.json"
#endif

#define DEFAULT_DEV_PORT "0000:31:00.0"
#define DEFAULT_DP_IP "192.168.96.1"
#define DEFAULT_GRPC_PORT "8001"
#define DEFAULT_TCP_PORT "8002"

/* print a description of all supported options */
void usage(FILE* fp, const char* path)
{
    /* take only the last portion of the path */
    const char* basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    fprintf(fp, "Usage: %s [OPTION]\n", basename);
    fprintf(fp, "-h, --help\t\t"
                "Print this help and exit.\n");
    fprintf(fp, "-d, --dev=dev_port\t"
                "PCI device port (defaults: %s).\n",
        DEFAULT_DEV_PORT);
    fprintf(fp, "-i, --ip=ip_address\t"
                "IP address for media data transportation (defaults: %s).\n",
        DEFAULT_DP_IP);
    fprintf(fp, "-g, --grpc=port_number\t"
                "Port number gRPC controller (defaults: %s).\n",
        DEFAULT_GRPC_PORT);
    fprintf(fp, "-t, --tcp=port_number\t"
                "Port number for TCP socket controller (defaults: %s).\n",
        DEFAULT_TCP_PORT);
}

void PrintStackTrace() {
    const int max_frames = 128;
    void* buffer[max_frames];
    int num_frames = backtrace(buffer, max_frames);
    char** symbols = backtrace_symbols(buffer, num_frames);

    std::cerr << "Stack trace:" << std::endl;
    for (int i = 0; i < num_frames; ++i) {
        Dl_info info;
        if (dladdr(buffer[i], &info) && info.dli_sname) {
            char* demangled = nullptr;
            int status = -1;
            if (info.dli_sname[0] == '_') {
                demangled = abi::__cxa_demangle(info.dli_sname, nullptr, 0, &status);
            }
            std::cerr << "  " << (status == 0 ? demangled : info.dli_sname) << " + " 
                      << ((char*)buffer[i] - (char*)info.dli_saddr) << " at " 
                      << info.dli_fname << std::endl;
            free(demangled);
        } else {
            std::cerr << symbols[i] << std::endl;
        }
    }

    free(symbols);
}

void SignalHandler(int signal) {
    std::cerr << "Error: signal " << signal << std::endl;
    PrintStackTrace();
    exit(1);
}

using namespace mesh;

class EmulatedReceiver : public connection::Connection
{
  public:
    EmulatedReceiver(context::Context& ctx)
    {
        _kind = connection::Kind::receiver;
        set_state(ctx, connection::State::configured);
    }

    connection::Result on_establish(context::Context& ctx) override
    {
        set_state(ctx, connection::State::active);
        return connection::Result::success;
    }

    connection::Result on_shutdown(context::Context& ctx) override
    {
        return connection::Result::success;
    }

    connection::Result on_receive(context::Context& ctx, void *ptr, uint32_t sz,
                                  uint32_t& sent) override
    {
        log::info("Data received");
        return connection::Result::success;
    }

    connection::Result configure(context::Context& ctx)
    {
        set_state(ctx, connection::State::configured);
        return connection::Result::success;
    }
};

class EmulatedTransmitter : public connection::Connection
{
  public:
    EmulatedTransmitter(context::Context& ctx)
    {
        _kind = connection::Kind::transmitter;
        set_state(ctx, connection::State::configured);
    }

    connection::Result on_establish(context::Context& ctx) override
    {
        set_state(ctx, connection::State::active);
        return connection::Result::success;
    }

    connection::Result on_shutdown(context::Context& ctx) override
    {
        return connection::Result::success;
    }

    connection::Result configure(context::Context& ctx)
    {
        set_state(ctx, connection::State::configured);
        return connection::Result::success;
    }

    connection::Result transmit_plaintext(context::Context& ctx,
                                          const void *ptr, size_t sz)
    {
        // Cast const void* to void* safely for transmit
        return transmit(ctx, const_cast<void *>(ptr), sz);
    }
};

const size_t payload_sizes[] = {1024, 1 << 20, 8 << 20}; // 1 KB, 1 MB, 8 MB
const int queue_sizes[] = {1, 8, 32}; // Different queue sizes

// Main context with cancellation
auto ctx = context::WithCancel(context::Background());

int main(int argc, char* argv[])
{
    signal(SIGSEGV, SignalHandler);

    std::string grpc_port = DEFAULT_GRPC_PORT;
    std::string tcp_port = DEFAULT_TCP_PORT;
    std::string dev_port = DEFAULT_DEV_PORT;
    std::string dp_ip = DEFAULT_DP_IP;
    int help_flag = 0;
    int opt;
    struct option longopts[] = {
        { "help", no_argument, &help_flag, 1 },
        { "dev", required_argument, NULL, 'd' },
        { "ip", required_argument, NULL, 'i' },
        { "grpc", required_argument, NULL, 'g' },
        { "tcp", required_argument, NULL, 't' },
        { 0 }
    };

    /* infinite loop, to be broken when we are done parsing options */
    while (1) {
        opt = getopt_long(argc, argv, "hd:i:g:t:", longopts, 0);
        if (opt == -1) {
            break;
        }

        switch (opt) {
        case 'h':
            help_flag = 1;
            break;
        case 'd':
            dev_port = optarg;
            break;
        case 'i':
            dp_ip = optarg;
            break;
        case 'g':
            grpc_port = optarg;
            break;
        case 't':
            tcp_port = optarg;
            break;
        case '?':
            usage(stderr, argv[0]);
            return 1;
        default:
            break;
        }
    }

    if (help_flag) {
        usage(stdout, argv[0]);
        return 0;
    }

    // Declare mDevHandle
    libfabric_ctx *mDevHandle = nullptr;

    log::info("Configuring connection parameters")("grpc_port", grpc_port)("tcp_port", tcp_port);

    mcm_conn_param request = {};
    {
        // Initialize request based on tcp_port
        if (tcp_port == "8002") {
            request.type = is_rx;
            // Ensure the string length does not exceed the destination buffer size
            if (dp_ip.size() >= sizeof(request.local_addr.ip)) {
                log::error("IP address is too long to fit in the local_addr.ip field");
                return 1; // Handle the error as needed
            }

            // Copy dp_ip into request.local_addr.ip
            std::strncpy(request.local_addr.ip, dp_ip.c_str(), sizeof(request.local_addr.ip) - 1);
            request.local_addr.ip[sizeof(request.local_addr.ip) - 1] =
                '\0'; // Null-terminate the string
        // Convert port number to string and copy it into request.local_addr.port
        std::snprintf(request.local_addr.port, sizeof(request.local_addr.port), "%d", 8002);
        } else {
            request.type = is_tx;
                        // Ensure the string length does not exceed the destination buffer size
            if (dp_ip.size() >= sizeof(request.local_addr.ip)) {
                log::error("IP address is too long to fit in the local_addr.ip field");
                return 1; // Handle the error as needed
            }

            // Copy dp_ip into request.local_addr.ip
            std::strncpy(request.remote_addr.ip, dp_ip.c_str(), sizeof(request.remote_addr.ip) - 1);
            request.remote_addr.ip[sizeof(request.remote_addr.ip) - 1] =
                '\0'; // Null-terminate the string
        // Convert port number to string and copy it into request.remote_addr.port
        std::snprintf(request.remote_addr.port, sizeof(request.remote_addr.port), "%d", 8002);
        }

        // Initialize other required fields of request
        // request.payload_type = PAYLOAD_TYPE_ST20_VIDEO;
        // request.payload_codec = PAYLOAD_CODEC_NONE;
        // Initialize payload_args if needed
        request.payload_args.rdma_args.transfer_size =
            4*1024*1024; // Example transfer size
        request.payload_args.rdma_args.queue_size = 32;
        // request.width = 1920;
        // request.height = 1080;
        // request.fps = 30;
        // request.pix_fmt = PIX_FMT_YUV422P_10BIT_LE;
        // request.payload_type_nr = 0;
        // request.payload_mtl_flags_mask = 0;
        // request.payload_mtl_pacing = 0;
    }

        // Intercept shutdown signals to cancel the main context
    auto signal_handler = [](int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
            log::info("Shutdown signal received")("signal", sig);
            ctx.cancel();
        }
    };
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

        connection::Result res;

    if (tcp_port == "8002") {
        log::info("Starting RX Path");
        EmulatedReceiver *emulated_rx = new EmulatedReceiver(ctx);
        emulated_rx->configure(ctx);
        emulated_rx->establish(ctx);

        // Setup Rx connection
        connection::RdmaRx *conn_rx = new connection::RdmaRx();
        log::info("Configuring RDMA RX connection");
        res = conn_rx->configure(ctx, request, mDevHandle);
        if (res != connection::Result::success) {
            log::error("Failed to configure RDMA RX connection")(
                "result", connection::result2str(res));
            delete conn_rx;
            delete emulated_rx;
            return 1;
        }

        log::info("Establishing RDMA RX connection...");
        try {
            res = conn_rx->establish(ctx);
            if (res == connection::Result::error_already_initialized) {
                log::debug(
                    "RDMA RX connection is already initialized. Continuing...");
                // Continue execution as the connection is already in an
                // established state
            } else if (res != connection::Result::success) {
                log::error("Failed to establish RDMA RX connection")(
                    "result", connection::result2str(res));
                delete conn_rx;
                delete emulated_rx;
                return 1;
            }
        } catch (const std::exception& e) {
            log::fatal("Exception during RDMA RX establishment")("error",
                                                                 e.what());
            delete conn_rx;
            delete emulated_rx;
            return 1;
        }

        log::info("Linking RDMA RX to Emulated Receiver...");
        conn_rx->set_link(ctx, emulated_rx);

        int sleep_duration = 600000;
        log::info("Sleeping to allow RX processing")("duration_ms",sleep_duration);
        mesh::thread::Sleep(ctx, std::chrono::milliseconds(sleep_duration));

        log::info("Shutting down RDMA RX connection...");
        // Shutdown Rx connection
        res = conn_rx->shutdown(ctx);
        if (res != connection::Result::success) {
            log::error("Failed to shut down RDMA RX connection")(
                       "result", mesh::connection::result2str(res));
        }

        delete conn_rx;
        delete emulated_rx;

        log::info("RX Path completed.");

    } else {
    log::info("Starting TX Path");
    connection::RdmaTx *conn_tx = new connection::RdmaTx();
    EmulatedTransmitter *emulated_tx = new EmulatedTransmitter(ctx);

    log::info("Configuring RDMA TX connection");
    res = conn_tx->configure(ctx, request, mDevHandle);
    if (res != connection::Result::success) {
        log::error("Failed to configure RDMA TX connection")("result", connection::result2str(res));
        delete conn_tx;
        delete emulated_tx;
        return 1;
    }

    log::info("Establishing RdmaTx connection...");
    res = conn_tx->establish(ctx);
    if (res != connection::Result::success) {
        log::error("Failed to establish RDMA TX connection")("result", connection::result2str(res));
        delete conn_tx;
        delete emulated_tx;
        return 1;
    }

    log::info("Configuring EmulatedTransmitter...");
    res = emulated_tx->configure(ctx);
    if (res != connection::Result::success) {
        log::error("Configure EmulatedTransmitter failed")("result", connection::result2str(res));
        delete conn_tx;
        delete emulated_tx;
        return 1;
    }

    log::info("Establishing EmulatedTransmitter...");
    res = emulated_tx->establish(ctx);
    if (res != connection::Result::success) {
        log::error("Establish EmulatedTransmitter failed")("result", connection::result2str(res));
        delete conn_tx;
        delete emulated_tx;
        return 1;
    }

    log::info("Linking EmulatedTransmitter with RdmaTx...");
    emulated_tx->set_link(ctx, conn_tx);

    size_t data_size = 4 * 1024 * 1024;
    char *test_data = new char[data_size];
    std::fill(test_data, test_data + data_size, 0); // Zero-initialize the buffer
    std::strcpy(test_data, "Hello RDMA World!");

    // Transmitter thread
    std::thread transmitter_thread([&, emulated_tx, test_data, data_size]() {
        try {
            for (int i = 0; i < 5000; ++i) {
                if (ctx.cancelled()) {
                    break;
                }
                log::info("Transmitting data")("iteration", i + 1);
                emulated_tx->transmit_plaintext(ctx, test_data, data_size);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        } catch (const std::exception &e) {
            log::error("Exception in transmitter thread")("message", e.what());
        } catch (...) {
            log::error("Unknown exception in transmitter thread");
        }
    });

    thread::Sleep(ctx, std::chrono::milliseconds(100000));
    res = conn_tx->shutdown(ctx);
    transmitter_thread.join(); // Ensure the thread finishes before cleanup

    log::info("Shutting down RDMA TX connection...");

    if (res != connection::Result::success) {
        log::error("Shutdown TX failed")("result", connection::result2str(res));
    }

    delete[] test_data;
    delete conn_tx;
    delete emulated_tx;

    log::info("TX Path completed.");
}

    log::info("Application exited gracefully");

    return 0;
}
