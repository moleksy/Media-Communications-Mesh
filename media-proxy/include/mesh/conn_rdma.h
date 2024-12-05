#ifndef CONN_RDMA_H
#define CONN_RDMA_H

#include "logger.h"
#include "mesh/conn.h"
#include "concurrency.h"
#include "libfabric_ep.h"
#include "libfabric_mr.h"
#include "libfabric_cq.h"
#include "libfabric_dev.h"
#include "mcm_dp.h"
#include <mutex>
#include <cstring>
#include <cstddef>
#include <atomic>
#include <queue>

#ifndef RDMA_DEFAULT_TIMEOUT
#define RDMA_DEFAULT_TIMEOUT 1
#endif
#ifndef MAX_BUFFER_SIZE
#define MAX_BUFFER_SIZE 1 << 30
#endif
#ifndef CQ_BATCH_SIZE
#define CQ_BATCH_SIZE 64
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

namespace mesh {

namespace connection {

/**
 * Rdma
 *
 * Base class for RDMA connections, derived from the `Connection` class.
 * Provides common RDMA-related functionality and acts as a foundation
 * for specialized RDMA Tx and Rx classes.
 */
class Rdma : public Connection {
  public:
    Rdma();
    virtual ~Rdma();

  //Used only for Unit tests, provides access to protected members
  #ifdef UNIT_TESTS_ENABLED
      size_t get_buffer_queue_size() const { return buffer_queue.size(); }
      bool is_buffer_queue_empty() const { return buffer_queue.empty(); }
      Kind get_kind() const { return _kind; }
      void* get_buffer_block() const { return buffer_block; }
  #endif

    // Queue synchronization
    void init_buf_available();
    void notify_buf_available();
    void wait_buf_available();

  protected:
    // Configure the RDMA session
    virtual Result configure(context::Context& ctx, const mcm_conn_param& request,
                             const std::string& dev_port, libfabric_ctx *& dev_handle, Kind kind,
                             direction dir);

    // Overrides from Connection
    virtual Result on_establish(context::Context& ctx) override;
    virtual void on_delete(context::Context& ctx) override;
    virtual Result on_shutdown(context::Context& ctx) override;

    // RDMA-specific methods

    // Configure RDMA endpoint
    Result configure_endpoint(context::Context& ctx);

    // Cleanup RDMA resources
    Result cleanup_resources(context::Context& ctx);

    // Error handler for logging and recovery
    void handle_error(context::Context& ctx, const char *step);

    // RDMA-specific members
    libfabric_ctx* mDevHandle; // RDMA device handle
    ep_ctx_t* ep_ctx;          // RDMA endpoint context
    ep_cfg_t ep_cfg;           // RDMA endpoint configuration
    size_t trx_sz;             // Data transfer size
    bool init;                 // Indicates if RDMA is initialized
    void* buffer_block;        // Pointer to the allocated buffer block
    int queue_size;            // Number of buffers in the queue

    // Queue for managing buffers
    std::queue<void*> buffer_queue; // Queue holding available buffers
    std::mutex queue_mutex;         // Mutex for buffer queue synchronization
    std::condition_variable_any queue_cv; // Condition variable for buffer availability

    // // RDMA thread logic
    // virtual void process_buffers_thread(context::Context& ctx) = 0;
    // virtual void rdma_cq_thread(context::Context& ctx) = 0;
    virtual Result start_threads(context::Context& ctx) { return Result::success; }

    Result init_queue_with_elements(size_t capacity, size_t trx_sz);
    Result add_to_queue(void *element);
    Result consume_from_queue(context::Context& ctx, void **element);
    void cleanup_queue();

    std::jthread handle_process_buffers_thread; // Thread for processing buffers
    std::jthread handle_rdma_cq_thread;         // Thread for completion queue operations
    context::Context process_buffers_thread_ctx; // Context for buffer processing thread
    context::Context rdma_cq_thread_ctx;        // Context for completion queue thread

    std::mutex cq_mutex;              // Mutex for completion queue synchronization
    std::condition_variable_any cq_cv; // Condition variable for completion queue events
    bool event_ready = false;         // Indicates if an event is ready in the CQ

    void notify_cq_event();
    void shutdown_rdma(context::Context& ctx);

    //Helper functions
    std::string kind_to_string(Kind kind);

    std::atomic<bool> buf_available; // Indicates buffer availability in the queue
};

} // namespace connection

} // namespace mesh

#endif // CONN_RDMA_H
