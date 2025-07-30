add_library(
        cep_workload
        OBJECT
        workload.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:cep_workload>
        PARENT_SCOPE)