set(TEST_BIN all-tests)

file(GLOB_RECURSE tests LIST_DIRECTORIES false *.cpp)
file(GLOB_RECURSE utils LIST_DIRECTORIES false ../util/*.cpp)
file(GLOB_RECURSE io LIST_DIRECTORIES false ../io/*.cpp)
file(GLOB_RECURSE sql LIST_DIRECTORIES false ../sql/*.cpp)
file(GLOB_RECURSE rules LIST_DIRECTORIES false ../rules/*.cpp)
file(GLOB_RECURSE event LIST_DIRECTORIES false ../event/*.cpp)
file(GLOB_RECURSE os-model LIST_DIRECTORIES false ../os-model/*.cpp)
# exclude prov-consumer here so we don't have to definitions of main in the test
file(GLOB_RECURSE consumer LIST_DIRECTORIES false ../consumer/a*.cpp ../consumer/s*.cpp)
file(GLOB_RECURSE provd LIST_DIRECTORIES false ../provd/provd-client.cpp)

set(SOURCES ${tests} ${utils} ${io} ${sql} ${rules} ${event} ${consumer} ${provd} ${os-model})

add_executable(${TEST_BIN} ${tests} ${utils} ${io} ${sql} ${rules} ${event} ${consumer} ${provd} ${os-model})
add_test(NAME ${TEST_BIN} COMMAND ${TEST_BIN})

target_include_directories(${TEST_BIN} PUBLIC /usr/local/include ../util ../io ../sql ../rules
  ../event ../consumer ../provd ../os-model ../lib/c-hglib/hglib)
# include openssl/md5.h on MacOS (assuming it has been installed through homebrew)
if (APPLE)
  target_include_directories(${TEST_BIN} PUBLIC /usr/local/Cellar/openssl@1.1/1.1.1g/include)
endif ()

target_link_directories(${TEST_BIN} PUBLIC /usr/local/lib ${PROJECT_SOURCE_DIR}/lib/c-hglib)
# include libcrypto on MacOS (assuming it has been installed through homebrew)
if (APPLE)
  target_link_directories(${TEST_BIN} PUBLIC /usr/local/Cellar/openssl@1.1/1.1.1g/lib)
endif ()

target_link_libraries(${TEST_BIN} PUBLIC gtest gmock odbc boost_regex hg crypto rdkafka++)
if (UNIX AND NOT APPLE)
	target_link_libraries(${TEST_BIN} PUBLIC auparse audit)
endif()

add_custom_command(
  TARGET ${TEST_BIN}
  COMMAND ${CMAKE_COMMAND} -E copy
  ${CMAKE_CURRENT_SOURCE_DIR}/resources/*
  ${CMAKE_BINARY_DIR}/test/
)
