project(libmvsim LANGUAGES CXX VERSION ${MVSIM_VERSION})

include(CMakePackageConfigHelpers)

# ========================== Dependencies ====================================
# It must be at the same directory than protobuf_generate_cpp()
option(MVSIM_WITH_PROTOBUF "Use protobuf for communications" ON)

if (MVSIM_WITH_PROTOBUF)
	find_package(Protobuf)
	if (Protobuf_FOUND)
		#
	else()
		set(MVSIM_WITH_PROTOBUF OFF CACHE BOOL "Use protobuf for communications" FORCE)
	endif()
endif()

option(MVSIM_WITH_ZMQ "Use ZMQ for communications" ON)

if (MVSIM_WITH_ZMQ)
	find_package(PkgConfig)
	pkg_check_modules(PC_ZeroMQ QUIET zmq)

	# use the hint from above to find where 'zmq.hpp' is located
	find_path(ZeroMQ_INCLUDE_DIR
			NAMES zmq.hpp
			PATHS ${PC_ZeroMQ_INCLUDE_DIRS}
			)

	# use the hint from about to find the location of libzmq
	find_library(ZeroMQ_LIBRARY
			NAMES zmq
			PATHS ${PC_ZeroMQ_LIBRARY_DIRS}
			)

	if ($ENV{VERBOSE})
		message(STATUS "ZeroMQ_LIBRARY: ${ZeroMQ_LIBRARY}")
		message(STATUS "ZeroMQ_INCLUDE_DIR: ${ZeroMQ_INCLUDE_DIR}")
	endif()

	if (ZeroMQ_LIBRARY AND ZeroMQ_INCLUDE_DIR)
		#
	else()
		set(MVSIM_WITH_ZMQ OFF CACHE BOOL "Use ZMQ for communications" FORCE)
	endif()
endif()

option(MVSIM_WITH_PYTHON "Build python wrappers" ON)
if (MVSIM_WITH_PYTHON)
	if(NOT ${CMAKE_VERSION} VERSION_LESS "3.12.0")
		find_package(Python3 COMPONENTS Interpreter Development)
	else()
		# When cmake 3.12 is available everywhere, delete this branch of the if()
		find_program(Python3_EXECUTABLE NAMES python3)
		set(Python3_VERSION_MAJOR ${Python_VERSION_MAJOR})
		set(Python3_VERSION_MINOR ${Python_VERSION_MINOR})
	endif()

  # Enforce using python3:
  set(Python_ADDITIONAL_VERSIONS ${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR})
  set(PYBIND11_PYTHON_VERSION ${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR})
  find_package(pybind11 CONFIG)
  set(MVSIM_PYTHON_BUILD_DIRECTORY ${mvsim_BINARY_DIR}/python)

  if (pybind11_FOUND AND pybind11_VERSION VERSION_LESS 2.2)
	message(STATUS "Warning: Disabling Python bindings since pybind11 >=2.2 is required but only found version ${pybind11_VERSION}")
	set(MVSIM_WITH_PYTHON OFF CACHE BOOL "Build python wrappers" FORCE)
	set(pybind11_FOUND OFF)
  endif()

  if (NOT pybind11_FOUND)
	  set(MVSIM_WITH_PYTHON OFF CACHE BOOL "Build python wrappers" FORCE)
  endif()
endif()


# ========================== Functions ====================================

function(mvsim_common_target_settings MODULENAME_)
  set(TARGETNAME_ "mvsim-${MODULENAME_}")

  # To make code portable between using directly the mvsim source tree, or
  # importing exported cmake targets:
  add_library(mvsim::${MODULENAME_}  ALIAS ${TARGETNAME_})

  if (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR CMAKE_COMPILER_IS_GNUCXX)
	  target_compile_options(${TARGETNAME_} PRIVATE
		  -Wall -pedantic
		  -Wsuggest-override
		  -Werror=return-type
		  -Wshadow
		  # -Werror=suggest-override # Removed for protobuf headers in u18.04
	  )

	  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
		  target_compile_options(${TARGETNAME_} PRIVATE
			  "-O3"
		  )
	  endif()
  endif()

  if(MSVC)
	  target_compile_definitions(${TARGETNAME_} PRIVATE
		  _CRT_SECURE_NO_WARNINGS
	  )
	  target_compile_options(${TARGETNAME_} PRIVATE
		  /wd4275
		  /wd4251
	  )
  endif()

  set_target_properties(
	${TARGETNAME_}
  PROPERTIES
	SOVERSION ${PROJECT_VERSION}
	VERSION ${PROJECT_VERSION}
	ARCHIVE_OUTPUT_DIRECTORY "lib"
	LIBRARY_OUTPUT_DIRECTORY "lib"
	WINDOWS_EXPORT_ALL_SYMBOLS ON
  )

  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
      target_include_directories(${TARGETNAME_}
          PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
          )
  endif()

  target_include_directories(${TARGETNAME_}
	  PRIVATE
		  ${CMAKE_CURRENT_SOURCE_DIR}/src
		  ${ZeroMQ_INCLUDE_DIR}
  )

  if (MVSIM_WITH_PROTOBUF)
    target_compile_definitions(${TARGETNAME_} PUBLIC MVSIM_HAS_PROTOBUF)
  endif()

  if (MVSIM_WITH_ZMQ)
    target_compile_definitions(${TARGETNAME_} PUBLIC MVSIM_HAS_ZMQ)
  endif()

  # ==== Install & export target ========
  install(TARGETS	${TARGETNAME_}
		  EXPORT 	${TARGETNAME_}-targets
		  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  endif()

  install(EXPORT ${TARGETNAME_}-targets
	  FILE ${TARGETNAME_}-targets.cmake
	  NAMESPACE mvsim::
	  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TARGETNAME_}
  )
  export(
	TARGETS ${TARGETNAME_}
	FILE "${CMAKE_BINARY_DIR}/cmake/${TARGETNAME_}-targets.cmake"
	NAMESPACE mvsim::
  )

  configure_package_config_file(
	  ${mvsim_SOURCE_DIR}/cmake/mvsim-config.cmake.in
	  ${CMAKE_BINARY_DIR}/cmake/${TARGETNAME_}-config.cmake
	  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TARGETNAME_}
  )

  write_basic_package_version_file(
	  ${CMAKE_BINARY_DIR}/cmake/${TARGETNAME_}-config-version.cmake
	  VERSION ${PROJECT_VERSION}
	  COMPATIBILITY AnyNewerVersion
  )

  install(
	  FILES
		  ${CMAKE_BINARY_DIR}/cmake/${TARGETNAME_}-config.cmake
		  ${CMAKE_BINARY_DIR}/cmake/${TARGETNAME_}-config-version.cmake
	  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TARGETNAME_}
  )
endfunction()


# ========================== Targets ====================================
add_subdirectory(simulator)

if (MVSIM_WITH_PROTOBUF)
  add_subdirectory(comms)
  add_subdirectory(msgs)
endif()


# ========================== Install Python libs ==========================
if (pybind11_FOUND)
  #set(MVSIM_PYTHON_VERSION "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}"
	#    CACHE STRING "The version of Python to build MVSIM wrapper against."
	#    FORCE)

	# Generate setup.py.
	configure_file(${mvsim_SOURCE_DIR}/modules/setup.py.in
		${MVSIM_PYTHON_BUILD_DIRECTORY}/setup.py)

	install(
	FILES
	  ${PROTO_PY_FILES}
	DESTINATION
	  ${CMAKE_INSTALL_INCLUDEDIR}/mvsim/mvsim-msgs/
	)

	set(PYTHON_INSTALL_DIRECTORY "${Python3_SITELIB}"
	CACHE PATH "Install prefix for python modules with 'make python-install'")

	add_custom_target(python-install
			COMMAND ${Python3_EXECUTABLE} ${MVSIM_PYTHON_BUILD_DIRECTORY}/setup.py install --root=${PYTHON_INSTALL_DIRECTORY}/.. --install-layout=deb --single-version-externally-managed
			DEPENDS mvsim-msgs pymvsim_comms
			WORKING_DIRECTORY ${MVSIM_PYTHON_BUILD_DIRECTORY})
endif()

# ========================== Show results ====================================
message(STATUS "----- Configured libmvsim (version ${PROJECT_VERSION}) ------")
message(STATUS "MRPT version   : ${MRPT_VERSION}")
message(STATUS "libbox2d       : ${BOX2D_VERSION_STRING}")
message(STATUS "Protobuf       : ${MVSIM_WITH_PROTOBUF} ${Protobuf_VERSION}")
message(STATUS "ZeroMQ         : ${MVSIM_WITH_ZMQ} ${ZeroMQ_VERSION}")
message(STATUS "Python wrappers: ${MVSIM_WITH_PYTHON} (pybind11: ${pybind11_VERSION})")
message(STATUS "Python interp  : ${Python3_EXECUTABLE}")
message(STATUS "")
