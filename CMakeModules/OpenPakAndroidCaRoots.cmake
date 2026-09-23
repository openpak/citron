# OpenPak: the public root store openpak-client loads on Android.
#
# There is no CA bundle file to point OpenSSL at on a phone, so on Android the library includes
# <openssl/cert.h> (a PEM blob, kCert) from the patched OpenSSL Eden's Android build uses
# (crueter-ci/OpenSSL). Citron keeps its own vcpkg OpenSSL; only that one header is taken from the
# package, pinned by version and SHA-512, and put on the library's include path by itself so every
# other <openssl/...> header still comes from vcpkg. The header is the same for every ABI.

set(OPENPAK_CA_OPENSSL_VERSION "4.0.1-1788234794-b64f68a94e")
set(_openpak_ca_name "openssl-android-aarch64-${OPENPAK_CA_OPENSSL_VERSION}")
set(_openpak_ca_sha512 "beda26d4151196a980da67bc595be7b538dead4d243f97288fec4e04204dc17566608ed5db1a48c95b6fe140a627a906926667c9479db6f0f2d66a7a6e6cad2a")
set(_openpak_ca_root "${CMAKE_BINARY_DIR}/openpak-ca")
set(OPENPAK_CA_INCLUDE_DIR "${_openpak_ca_root}/include")

if(NOT EXISTS "${OPENPAK_CA_INCLUDE_DIR}/openssl/cert.h")
  set(_openpak_ca_archive "${_openpak_ca_root}/${_openpak_ca_name}.tar.zst")
  file(DOWNLOAD
    "https://github.com/crueter-ci/OpenSSL/releases/download/${OPENPAK_CA_OPENSSL_VERSION}/${_openpak_ca_name}.tar.zst"
    "${_openpak_ca_archive}"
    EXPECTED_HASH SHA512=${_openpak_ca_sha512}
    STATUS _openpak_ca_status)
  list(GET _openpak_ca_status 0 _openpak_ca_error)
  if(_openpak_ca_error)
    message(FATAL_ERROR "OpenPak: could not download ${_openpak_ca_name}: ${_openpak_ca_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_openpak_ca_archive}" DESTINATION "${_openpak_ca_root}/pkg"
       PATTERNS "*/openssl/cert.h" "include/openssl/cert.h")
  file(GLOB_RECURSE _openpak_ca_header "${_openpak_ca_root}/pkg/*/cert.h")
  if(NOT _openpak_ca_header)
    message(FATAL_ERROR "OpenPak: ${_openpak_ca_name} carries no openssl/cert.h")
  endif()
  list(GET _openpak_ca_header 0 _openpak_ca_header)
  file(COPY "${_openpak_ca_header}" DESTINATION "${OPENPAK_CA_INCLUDE_DIR}/openssl")
  file(REMOVE_RECURSE "${_openpak_ca_root}/pkg")
  file(REMOVE "${_openpak_ca_archive}")
endif()
