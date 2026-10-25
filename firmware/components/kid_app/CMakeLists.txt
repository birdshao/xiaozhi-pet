# Dedicated ESP-IDF component. Firmware side depends only on this component.
idf_component_register(
    SRCS "src/scene_model.cc" "src/pet_controller.cc" "src/pet_scene_presenter.cc"
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
