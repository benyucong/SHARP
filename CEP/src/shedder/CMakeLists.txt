add_library(
        cep_shedder
        OBJECT
        shedder.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:cep_shedder>
        PARENT_SCOPE)