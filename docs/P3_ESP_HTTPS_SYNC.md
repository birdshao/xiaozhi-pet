# P3 real ESP32 HTTPS event transport (opt-in, not yet active on the board)

**Goal:** send the child's locally committed Pet events to the separate Python PetLearning API. Uses ESP-IDF 6.1 HTTP client and the one-blob PetAtomicLedger, not a second ASR/WebSocket/Opus implementation.

## Implemented modules

- EspPetHttpsTransport: certificate-verified HTTPS POST with per-device bearer token injected at runtime by trusted provisioning code. Rejects plaintext HTTP, redirects, URL credentials, malformed JSON, oversized response (>767 bytes), and a response other than exactly one accepted per-event ACK with integer seq/version. Uses cJSON; never prints token or raw response.
- FreertosPetHttpWorker: separate network task with bounded 4-second request timeout. Offline backoff 1, 2, 4 ... 60 seconds, resending the same durable message ID and seq, not allocating new ones. After a positive parsed ACK, rechecks the expected id/sequence and sends a copied receipt into the Pet worker queue.
- FreertosAtomicPetService (PR #31): only the single Pet task confirms ACK/dequeues atomically with the Pet checkpoint; it then publishes the new head to the network worker. UI/EPD/audio callback must not do HTTP work.

Network task callbacks use a length-1 overwrite queue because every notification always represents the CURRENT head: no later head can appear until a successful durable prior ACK. No NVS operation occurs on the network task.

## Deployment integration (still pending)

This code is built as an optional component and is **not yet activated in the Waveshare board integration**. Start network worker before starting the atomic Pet task and keep both for the device lifetime. A simplified construction sketch, not a drop-in board patch:

    static FreertosAtomicPetService pet;
    static FreertosPetHttpWorker network(
        pet, provisioned_https_origin, provisioned_business_device_id,
        provisioned_device_token);
    if (!network.Start()) { /* refuse online sync */ }
    pet.Start(FreertosAtomicPetService::StartMode::FreshNoLegacy,
              provisioned_business_device_id,
              ui_commit_callback,
              &FreertosPetHttpWorker::OnOutbound,
              &network, &initial_snapshot);

The UI/EPD committed-event callback will need its own context multiplexing. Existing devices with legacy kid_pet or kid_bridge data MUST NOT use FreshNoLegacy. First perform a supervised migration that proves the existing bridge has no unsent records and the authenticated server cursor is zero. ExistingOnly is the proper mode for a previously initialized atomic ledger. Do not run legacy and atomic Pet workers simultaneously.

The current Python PetLearning API is bound to 127.0.0.1 on its development host and is NOT directly reachable by an ESP32 over LAN. Provision an explicitly authenticated HTTPS reverse proxy with a valid hostname certificate and scoped device credentials. ESP-IDF requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE enabled; transport refuses HTTPS when this is disabled, rather than allowing unverified certificates. Never skip certificate hostname checks. Don't expose the API directly over HTTP.

Tokens must be securely provisioned per device and supplied to the constructor from the authorized host code. Never embed a real token in source, build artifacts, screenshots, CI logs or raw flash backups. Token rotation, parent provisioning authorization and server rate limits are needed before unsupervised child use.

## Remaining business gates

- The 16-event offline queue can fill; further projectable child interactions are not accepted. Product needs an explicit longer-offline UX / compression / reconciliation policy, not silent dropping.
- Server quiet/stories and PetController may reject inconsistent events. Negative ACKs stay durable; a reconciliation protocol is required.
- Hardware acceptance and initial Pet mode UI/voice integration remain pending. No physical device write or test is triggered here.

Official ESP-IDF API reference:
https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/protocols/esp_http_client.html
