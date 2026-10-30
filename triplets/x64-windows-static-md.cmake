# vcpkg 기본 triplet과 같고 C++ 표준만 다르다.
# Protobuf 포트는 C++17로 빌드되는데, 서버(C++20)와 표준이 다르면 라이브러리와 헤더의 타입이 어긋난다.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_CXX_STANDARD=20)
