set(TARGET_TUTORIAL tutorial)

set(SRC_TUTORIAL
	tutorialmain.cpp
	tutorial_callbacklist.cpp
	tutorial_eventdispatcher.cpp
	tutorial_eventqueue.cpp
	tutorial_hetercallbacklist.cpp
	tutorial_hetereventdispatcher.cpp
	tutorial_argumentadapter.cpp
	tutorial_anydata.cpp
	tip_use_type_as_id.cpp
)

add_executable(
	${TARGET_TUTORIAL}
	${SRC_TUTORIAL}
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${TARGET_TUTORIAL} Threads::Threads)

eventpp_configure_test(${TARGET_TUTORIAL})
add_test(NAME ${TARGET_TUTORIAL} COMMAND ${TARGET_TUTORIAL})
set_tests_properties(${TARGET_TUTORIAL} PROPERTIES TIMEOUT 60)
