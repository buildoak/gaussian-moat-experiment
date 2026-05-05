file(GLOB_RECURSE VERIFY_SOURCES
  "${ROOT}/include/*.hpp"
  "${ROOT}/include/*.h"
  "${ROOT}/src/*.cpp"
  "${ROOT}/postflight/src/*.cpp"
)

set(BAD "")
foreach(path IN LISTS VERIFY_SOURCES)
  file(READ "${path}" CONTENT)
  if(CONTENT MATCHES "#[ \t]*include[ \t]*[<\"]campaign/" OR
     CONTENT MATCHES "#[ \t]*include[ \t]*[<\"]cuda_campaign/")
    list(APPEND BAD "${path}")
  endif()
endforeach()

if(BAD)
  message(FATAL_ERROR "verification sources must not include campaign/cuda_campaign headers: ${BAD}")
endif()
