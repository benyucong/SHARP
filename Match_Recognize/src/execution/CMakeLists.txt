add_library(
        cep_execution
        OBJECT
        main.cpp
        experiment.cpp
        getopt.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:cep_execution>
        PARENT_SCOPE)