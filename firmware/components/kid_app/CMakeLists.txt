# Dedicated ESP-IDF component. Firmware side depends only on this component.
idf_component_register(
    SRCS "src/scene_model.cc" "src/pet_controller.cc" "src/pet_scene_presenter.cc"
         "src/pet_persistence.cc" "src/nvs_pet_blob_store.cc"
         "src/pet_event_executor.cc" "src/freertos_pet_service.cc"
         "src/audio_coordinator.cc" "src/pet_refresh_gate.cc"
         "src/freertos_pet_scene_mailbox.cc"
         "src/freertos_pet_display_worker.cc"
         "src/pet_device_bridge.cc" "src/pet_device_bridge_sender.cc"
         "src/nvs_bridge_blob_store.cc"
         "src/pet_atomic_ledger.cc" "src/nvs_atomic_pet_store.cc"
         "src/freertos_atomic_pet_service.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash freertos esp_timer
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
