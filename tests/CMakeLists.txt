#[[
// clang-format off
]]#
# Copyright (c) 2018 The Open-Transactions developers
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set(MODULE_NAME unittests-otagent)

set(cxx-sources
  main.cpp
  OTTestEnvironment.cpp
)

include_directories(
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/tests
  ${GTEST_INCLUDE_DIRS}
)

add_executable(${MODULE_NAME} ${cxx-sources})
add_dependencies(unittests-otagent otagent)
target_link_libraries(${MODULE_NAME} PRIVATE opentxs ${PROTOBUF_LITE_LIBRARIES} ${GTEST_BOTH_LIBRARIES} pthread)

if(OT_WITH_QT)
   target_link_libraries(${MODULE_NAME} PRIVATE ${Qt5Core_LIBRARIES} ${Qt5Qml_LIBRARIES})
endif()

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/tests)
add_test(${MODULE_NAME} ${PROJECT_BINARY_DIR}/tests/${MODULE_NAME} --gtest_output=xml:gtestresults.xml)
set_property(TARGET ${MODULE_NAME} PROPERTY CXX_STANDARD 17)

#[[
// clang-format on
]]#
