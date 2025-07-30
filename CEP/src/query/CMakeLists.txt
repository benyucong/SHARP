add_library(
        cep_query
        OBJECT
        query.cpp
        pattern.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:cep_query>
        PARENT_SCOPE)