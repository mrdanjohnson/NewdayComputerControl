#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Small FreeRTOS mutex wrapper used to serialize access to the command
// engine/ledger (HTTP task vs dispatcher task vs CLI).
class Mutex {
public:
    Mutex() : sem_(xSemaphoreCreateMutex()) {}
    ~Mutex() {
        if (sem_) vSemaphoreDelete(sem_);
    }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { xSemaphoreTake(sem_, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(sem_); }

private:
    SemaphoreHandle_t sem_;
};

class Guard {
public:
    explicit Guard(Mutex& m) : m_(m) { m_.lock(); }
    ~Guard() { m_.unlock(); }

private:
    Mutex& m_;
};
