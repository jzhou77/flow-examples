
add_flow_target(EXECUTABLE NAME hello SRCS hello.cpp test.h)
target_link_libraries(hello PUBLIC flow)


set(CALC_SRCS
  calc.cpp
  calc.actor.cpp)
add_flow_target(EXECUTABLE NAME calc SRCS ${CALC_SRCS})
target_link_libraries(calc PUBLIC flow)


set(VOID_SRCS
  test.h
  void.cpp
  void.actor.cpp)
add_flow_target(EXECUTABLE NAME void SRCS ${VOID_SRCS})
target_link_libraries(void PUBLIC flow)


set(LOOP_SRCS
  test.h
  loop.cpp
  loop.actor.cpp
  delay.actor.cpp
  except.actor.cpp
  broken.actor.cpp)
add_flow_target(EXECUTABLE NAME loop SRCS ${LOOP_SRCS})
target_link_libraries(loop PUBLIC flow)

set(DSLTEST_SRCS
  dsltest.cpp
  dsltest.actor.cpp
  ActorFuzz.h
  ActorFuzz.actor.cpp)
add_flow_target(EXECUTABLE NAME dsltest SRCS ${DSLTEST_SRCS})
target_link_libraries(dsltest PUBLIC flow)


