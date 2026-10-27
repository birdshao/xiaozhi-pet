#pragma once

#include "kid_app/esp_pet_https_transport.h"
#include "kid_app/freertos_atomic_pet_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace kid_app {

// Dedicated network task; HTTP may block here, never on pet, EPD or audio
// callbacks. Caller owns the worker for the whole device session.
// Construct/start this before FreertosAtomicPetService::Start, and pass
// &FreertosPetHttpWorker::OnOutbound as its outbound callback.
class FreertosPetHttpWorker {
public:
    FreertosPetHttpWorker(FreertosAtomicPetService& pet,
                         const char* https_origin,
                         const char* device_id,
                         const char* device_token);

    bool Start();
    static void OnOutbound(const PetDeviceBridge::Record& event, void* context);

private:
    static void Entry(void* context);
    void Run();

    FreertosAtomicPetService& pet_;
    EspPetHttpsTransport transport_;
    char device_id_[64]{};
    QueueHandle_t pending_ = nullptr;
    TaskHandle_t task_ = nullptr;
};

}  // namespace kid_app
