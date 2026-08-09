if(NOT DEFINED STUNT_INPUT_SOURCE OR
   NOT DEFINED STUNT_INPUT_DESTINATION)
    message(FATAL_ERROR "input configuration copy paths were not provided")
endif()

# Seed a usable per-build configuration, but never replace bindings the user
# has edited beside the executable.
if(NOT EXISTS "${STUNT_INPUT_DESTINATION}")
    file(COPY_FILE
        "${STUNT_INPUT_SOURCE}"
        "${STUNT_INPUT_DESTINATION}")
endif()
