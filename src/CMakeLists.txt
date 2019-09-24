# Copyright (c) 2019 The Open-Transactions developers
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB cxx-sources "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB cxx-headers "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(otagent ${cxx-sources} ${cxx-headers})

target_link_libraries(
  otagent
  PRIVATE
    Threads::Threads
    opentxs
    Boost::system
    Boost::filesystem
    Boost::program_options
    ${ZMQ_LIBRARIES}
)

if(CMAKE_DL_LIBS)
  target_link_libraries(otagent PRIVATE ${CMAKE_DL_LIBS})
endif()

if(LIB_RT)
  target_link_libraries(otagent PRIVATE ${LIB_RT})
endif()

target_include_directories(
  otagent SYSTEM
  PRIVATE ${opentxs_INCLUDE_DIRS} ${ZMQ_INCLUDE_DIRS}
)

install(TARGETS otagent DESTINATION bin)
