#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_stopRequested = 0;

void handleSignal(int) {
    g_stopRequested = 1;
}

class Event {
public:
    Event(int id, const std::string& payload)
        : id(id),
          payload(payload),
          createdAt(std::chrono::steady_clock::now()),
          internalState(new int(id * 10)) {}

    ~Event() {
        delete internalState;
    }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    int getId() const {
        return id;
    }

    const std::string& getPayload() const {
        return payload;
    }

    long long getLifetimeUs() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now - createdAt).count();
    }

    void process() {
        *internalState += 1;
    }

private:
    int id;
    std::string payload;
    std::chrono::steady_clock::time_point createdAt;
    int* internalState;
};

class Monitor {
public:
    Monitor() : ready(false), stopping(false), data(nullptr) {
        pthread_mutex_init(&mutex, nullptr);
        pthread_cond_init(&condition, nullptr);
    }

    ~Monitor() {
        delete data;
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    bool provide(Event* event) {
        pthread_mutex_lock(&mutex);
        if (stopping || ready) {
            pthread_mutex_unlock(&mutex);
            return false;
        }

        data = event;
        ready = true;
        std::cout << "[Producer] Sent event #" << event->getId()
                  << " | ptr=" << static_cast<const void*>(event)
                  << " | payload=" << event->getPayload() << std::endl;

        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&mutex);
        return true;
    }

    bool consume() {
        pthread_mutex_lock(&mutex);
        while (!ready && !stopping) {
            pthread_cond_wait(&condition, &mutex);
        }

        if (!ready && stopping) {
            pthread_mutex_unlock(&mutex);
            return false;
        }

        Event* event = data;
        data = nullptr;
        ready = false;

        std::cout << "[Consumer] Received event #" << event->getId()
                  << " | ptr=" << static_cast<const void*>(event)
                  << " | payload=" << event->getPayload()
                  << " | lifetime_us=" << event->getLifetimeUs() << std::endl;

        event->process();
        delete event;

        pthread_mutex_unlock(&mutex);
        return true;
    }

    void stop() {
        pthread_mutex_lock(&mutex);
        stopping = true;
        pthread_cond_broadcast(&condition);
        pthread_mutex_unlock(&mutex);
    }

    bool isStopping() {
        pthread_mutex_lock(&mutex);
        bool result = stopping;
        pthread_mutex_unlock(&mutex);
        return result;
    }

private:
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool ready;
    bool stopping;
    Event* data;
};

class Producer {
public:
    Producer(Monitor& mon, int limit) : monitor(mon), maxEvents(limit) {}

    static void* threadEntry(void* arg) {
        static_cast<Producer*>(arg)->run();
        return nullptr;
    }

    void run() {
        int eventId = 1;
        while (!g_stopRequested && (maxEvents <= 0 || eventId <= maxEvents)) {
            sleep(1);
            if (g_stopRequested) {
                break;
            }

            Event* event = new Event(eventId, "data_" + std::to_string(eventId));
            while (!monitor.provide(event)) {
                if (g_stopRequested || monitor.isStopping()) {
                    delete event;
                    monitor.stop();
                    return;
                }
                usleep(10000);
            }
            ++eventId;
        }
        monitor.stop();
    }

private:
    Monitor& monitor;
    int maxEvents;
};

class Consumer {
public:
    explicit Consumer(Monitor& mon) : monitor(mon) {}

    static void* threadEntry(void* arg) {
        static_cast<Consumer*>(arg)->run();
        return nullptr;
    }

    void run() {
        while (monitor.consume()) {
        }
    }

private:
    Monitor& monitor;
};

}

int main(int argc, char* argv[]) {
    struct sigaction sa;
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int maxEvents = 0;
    if (argc > 1) {
        maxEvents = std::atoi(argv[1]);
    }

    Monitor monitor;
    Producer producer(monitor, maxEvents);
    Consumer consumer(monitor);

    pthread_t consumerThread;
    pthread_t producerThread;

    if (pthread_create(&consumerThread, nullptr, Consumer::threadEntry, &consumer) != 0) {
        std::cerr << "Failed to create consumer thread" << std::endl;
        return 1;
    }

    if (pthread_create(&producerThread, nullptr, Producer::threadEntry, &producer) != 0) {
        std::cerr << "Failed to create producer thread" << std::endl;
        monitor.stop();
        pthread_join(consumerThread, nullptr);
        return 1;
    }

    pthread_join(producerThread, nullptr);
    pthread_join(consumerThread, nullptr);

    return 0;
}