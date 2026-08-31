cmake_minimum_required(VERSION 3.28)

project(veclet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(BUF_EXECUTABLE NAMES buf REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

function(veclet_imported_executable output_variable target_name)
  get_target_property(imported_configurations
                      "${target_name}"
                      IMPORTED_CONFIGURATIONS)
  foreach(configuration IN LISTS imported_configurations)
    string(TOUPPER "${configuration}" configuration_upper)
    get_target_property(candidate
                        "${target_name}"
                        "IMPORTED_LOCATION_${configuration_upper}")
    if(candidate AND EXISTS "${candidate}")
      set("${output_variable}" "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()

  get_target_property(candidate "${target_name}" IMPORTED_LOCATION)
  if(candidate AND EXISTS "${candidate}")
    set("${output_variable}" "${candidate}" PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR "Cannot locate imported executable ${target_name}")
endfunction()

veclet_imported_executable(VECLET_PROTOC_EXECUTABLE protobuf::protoc)
veclet_imported_executable(VECLET_GRPC_CPP_PLUGIN_EXECUTABLE
                           gRPC::grpc_cpp_plugin)

configure_file(cmake/buf.gen.cpp.yaml.in
               "${CMAKE_CURRENT_BINARY_DIR}/buf.gen.cpp.yaml"
               @ONLY)

enable_testing()
add_subdirectory(cpp)
