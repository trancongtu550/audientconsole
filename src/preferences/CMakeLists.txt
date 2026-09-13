add_library(audient_preferences STATIC
    AppPreferences.cpp)

target_include_directories(audient_preferences
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_preferences)

target_compile_features(audient_preferences PUBLIC cxx_std_20)
