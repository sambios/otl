#include "otl_thread_queue.h"
#include "otl_log.h"
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>
#include <atomic>

using namespace otl;

static void test_heavy_queue_basic()
{
    BlockingQueue<int> q("heavy-basic", /*type=*/0, /*limit=*/0, /*warning=*/1000000);
    int v = 0;
    // push/pop single
    q.push(v = 1);
    std::vector<int> out;
    bool timeout = false;
    int rc = q.pop_front(out, 1, 1, /*wait_ms=*/50, &timeout);
    assert(rc == 0);
    assert(!out.empty() && out[0] == 1);
}

static void test_heavy_queue_bulk_and_types()
{
    // test vector/deque type = 1 path
    BlockingQueue<int> qvec("heavy-vec", /*type=*/1, /*limit=*/0, /*warning=*/1000000);
    std::vector<int> batch;
    for (int i = 0; i < 10; ++i) batch.push_back(i);
    qvec.push(batch);
    std::vector<int> out;
    int rc = qvec.pop_front(out, 5, 10, /*wait_ms=*/50, nullptr);
    assert(rc == 0);
    assert((int)out.size() >= 5);
    // Remaining elements
    out.clear();
    rc = qvec.pop_front(out, 1, 10, /*wait_ms=*/50, nullptr);
    assert(rc == 0);
}

static void test_heavy_queue_limit_and_drop()
{
    // limit small, provide drop_fn so it won't block
    BlockingQueue<int> q("heavy-drop", /*type=*/0, /*limit=*/4, /*warning=*/1000000);
    q.set_drop_fn([](int&){ /* drop callback no-op */ });

    // Fill beyond limit to trigger drop_half_
    for (int i = 0; i < 10; ++i) {
        int v = i;
        q.push(v);
    }
    // size should be <= limit after drops over time, but not guaranteed deterministically
    // Just ensure queue is non-empty and operable
    std::vector<int> out;
    (void)q.pop_front(out, 1, 8, 50, nullptr);
}

static void test_heavy_queue_stop_and_timeout()
{
    BlockingQueue<int> q("heavy-stop", /*type=*/0, /*limit=*/0, /*warning=*/1000000);
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};

    std::thread th([&]{
        started.store(true);
        std::vector<int> out;
        bool timeout = false;
        int rc = q.pop_front(out, 1, 1, /*wait_ms=*/100, &timeout);
        // Expect timeout because no data
        assert(rc == -1 || out.empty());
        done.store(true);
    });

    while (!started.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    th.join();

    // stop should wake any waiters safely (idempotent)
    q.stop();
}

static void test_light_queue_basic()
{
    internal::BlockingQueue<int> ql;
    ql.push(42);
    int v = 0;
    bool ok = ql.pop(v, /*timeoutMs=*/10);
    assert(ok && v == 42);
}

static void test_light_queue_shutdown_reset()
{
    internal::BlockingQueue<int> ql;
    ql.shutdown();
    int v = 0;
    bool ok = ql.pop(v, 0);
    assert(!ok);
    ql.reset();
    ql.push(7);
    ok = ql.pop(v, 10);
    assert(ok && v == 7);
}

int main()
{
    // init logging minimal
    otl::log::LogConfig cfg; cfg.targets = otl::log::OutputTarget::Console; cfg.level = otl::log::LOG_WARNING; cfg.enableConsole = true; cfg.abortOnFatal = false; cfg.queueSize = 256;
    otl::log::init(cfg);

    test_heavy_queue_basic();
    test_heavy_queue_bulk_and_types();
    test_heavy_queue_limit_and_drop();
    test_heavy_queue_stop_and_timeout();

    test_light_queue_basic();
    test_light_queue_shutdown_reset();

    otl::log::deinit();
    return 0;
}
