set(WISH_EXAMPLES_PRIVATE_SOURCES
	main.cpp
)

add_executable(wish_examples ${WISH_EXAMPLES_PRIVATE_SOURCES})

target_link_libraries(wish_examples PRIVATE wish)