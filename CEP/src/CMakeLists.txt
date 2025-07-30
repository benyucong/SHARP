add_subdirectory(query)
add_subdirectory(common)
add_subdirectory(workload)
add_subdirectory(execution)
add_subdirectory(shedder)

add_library(cep STATIC ${ALL_OBJECT_FILES})

set(CEP_LIBS
        cep_query
        cep_common
        cep_workload
        cep_execution
        cep_shedder
)

target_link_libraries(
        cep
        ${CEP_LIBS}
        ${Boost_LIBRARIES}
)

target_include_directories(
        cep PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_executable(cep_demo execution/main.cpp)
target_link_libraries(cep_demo cep)