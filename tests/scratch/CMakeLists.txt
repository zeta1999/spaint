####################################
# CMakeLists.txt for tests/scratch #
####################################

IF(WITH_ALGLIB)
  ADD_SUBDIRECTORY(alglib)
ENDIF()

IF(WITH_ARRAYFIRE)
  ADD_SUBDIRECTORY(arrayfire)
ENDIF()

ADD_SUBDIRECTORY(boost)

IF(WITH_CUDA)
  ADD_SUBDIRECTORY(cuda)
ENDIF()

ADD_SUBDIRECTORY(eigen)

IF(BUILD_INFERMOUS)
  ADD_SUBDIRECTORY(infermous)
ENDIF()

ADD_SUBDIRECTORY(infinitam)

IF(WITH_LEAP)
  ADD_SUBDIRECTORY(leap)
ENDIF()

ADD_SUBDIRECTORY(lodepng)

IF(WITH_OPENCV)
  ADD_SUBDIRECTORY(opencv)
ENDIF()

IF(WITH_OVR)
  ADD_SUBDIRECTORY(ovr)
ENDIF()

ADD_SUBDIRECTORY(rafl)
ADD_SUBDIRECTORY(sdl)

IF(WITH_TBB)
  ADD_SUBDIRECTORY(tbb)
ENDIF()

ADD_SUBDIRECTORY(tvgutil)

IF(WITH_VICON)
  ADD_SUBDIRECTORY(vicon)
ENDIF()

ADD_SUBDIRECTORY(voice)

IF(WITH_ZED)
  ADD_SUBDIRECTORY(zed)
ENDIF()
