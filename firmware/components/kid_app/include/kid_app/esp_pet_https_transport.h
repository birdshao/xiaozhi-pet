#pragma once

#include "kid_app/pet_device_bridge_sender.h"

#include "esp_http_client.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// ESP-IDF only: HTTPS/TLS and server-receipt decoder.
// Certificate hostname verification is compulsory. Rejects redirects,
// insecure HTTP, oversized or malformed responses. No credentials are logged.
class EspPetHttpsTransport final : public DeviceBridgeTransport {
public:
    EspPetHttpsTransport(const char* https_origin, const char* device_id,
                         const char* device_token);
    bool Configured() const { return configured_; }
    bool PostJson(const char* body, size_t size,
                  PetDeviceBridge::ServerAck* ack) override;

private:
    struct Reply {
        char bytes[768]{};
        size_t used = 0;
        bool overflow = false;
    };
    static esp_err_t OnHttpEvent(esp_http_client_event_t* event);
    bool ParseAck(const Reply& reply, PetDeviceBridge::ServerAck* out);

    bool configured_ = false;
    char url_[256]{};
    char auth_[256]{};
    char message_id_[96]{};  // owned copy until next PostJson
};

}  // namespace kid_app
