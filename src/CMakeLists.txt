#[[
// clang-format off
]]#
# Copyright (c) 2018 The Open-Transactions developers
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
)

set(MODULE_NAME otagent)

file(GLOB cxx-sources "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB cxx-headers "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(${MODULE_NAME}
  ${cxx-sources}
  ${cxx-headers})

target_link_libraries(
  ${MODULE_NAME}
  PRIVATE
  Threads::Threads
  ${APP_SYSTEM_LIBRARIES}
  ${PROTOBUF_LITE_LIBRARIES}
  ${OPENTXS_PROTO_LIBRARIES}
  ${OPENTXS_LIBRARIES}
  ${Boost_SYSTEM_LIBRARIES}
  ${Boost_FILESYSTEM_LIBRARIES}
  ${Boost_PROGRAM_OPTIONS_LIBRARIES}
)

if(OT_WITH_QT)
   target_link_libraries(${MODULE_NAME} PRIVATE ${Qt5Core_LIBRARIES} ${Qt5Qml_LIBRARIES})
endif()

set_property(TARGET ${MODULE_NAME} PROPERTY CXX_STANDARD 17)

install(TARGETS ${MODULE_NAME} DESTINATION bin)

#[[
// clang-format on
]]#
