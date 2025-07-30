add_library(
        cep_common
        OBJECT
        event.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:cep_common>
        PARENT_SCOPE)