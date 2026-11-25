# Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
# Copyright (c) 2024-present The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# RIP-25: liboqs integration for ML-DSA-44 post-quantum signatures.
# Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>
# https://github.com/open-quantum-safe/liboqs

option(WITH_LIBOQS "Enable ML-DSA-44 post-quantum signatures (requires liboqs)." ON)

if(WITH_LIBOQS)
  # src/crypto/mldsa.cpp uses the context-string API and mldsa-native's seeded
  # keygen, both new in liboqs 0.16. An older liboqs would pass configuration and
  # then fail to link, so every way of finding it checks the version.
  set(LIBOQS_MIN_VERSION 0.16.0)

  # Try the official cmake config first — works for both vcpkg and depends builds
  # because the depends toolchain sets CMAKE_PREFIX_PATH to the deps install prefix.
  find_package(liboqs ${LIBOQS_MIN_VERSION} CONFIG QUIET)
  if(TARGET OQS::oqs)
    set(LIBOQS_FOUND TRUE)
    set(LIBOQS_TARGET OQS::oqs)
  endif()

  if(NOT LIBOQS_FOUND)
    # Fallback: pkg-config
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(LIBOQS QUIET liboqs>=${LIBOQS_MIN_VERSION})
    endif()

    if(NOT LIBOQS_FOUND)
      # Final fallback: manual header/library search
      find_path(LIBOQS_INCLUDE_DIR NAMES oqs/oqs.h)
      find_library(LIBOQS_LIBRARY NAMES oqs)
      if(LIBOQS_INCLUDE_DIR AND LIBOQS_LIBRARY)
        set(LIBOQS_HEADER_VERSION "")
        if(EXISTS "${LIBOQS_INCLUDE_DIR}/oqs/oqsconfig.h")
          file(STRINGS "${LIBOQS_INCLUDE_DIR}/oqs/oqsconfig.h" liboqs_version_line REGEX "#define OQS_VERSION_TEXT")
          string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" LIBOQS_HEADER_VERSION "${liboqs_version_line}")
        endif()
        if(LIBOQS_HEADER_VERSION VERSION_GREATER_EQUAL LIBOQS_MIN_VERSION)
          set(LIBOQS_FOUND TRUE)
          set(LIBOQS_LIBRARIES "${LIBOQS_LIBRARY}")
          set(LIBOQS_INCLUDE_DIRS "${LIBOQS_INCLUDE_DIR}")
        else()
          set(LIBOQS_TOO_OLD "${LIBOQS_INCLUDE_DIR} (version \"${LIBOQS_HEADER_VERSION}\")")
        endif()
      endif()
    endif()

    if(LIBOQS_FOUND AND NOT TARGET OQS::oqs)
      add_library(OQS::oqs UNKNOWN IMPORTED)
      set_target_properties(OQS::oqs PROPERTIES
        IMPORTED_LOCATION "${LIBOQS_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOQS_INCLUDE_DIRS}"
      )
      set(LIBOQS_TARGET OQS::oqs)
    endif()
  endif()

  if(LIBOQS_FOUND)
    target_compile_definitions(core_interface INTERFACE HAVE_LIBOQS)
    message(STATUS "liboqs found — ML-DSA-44 (RIP-25) post-quantum signatures enabled.")
  else()
    # Hard error, never a silent downgrade. ML-DSA-44 (RIP-25) verification is a
    # consensus rule once the deployment activates: a node built without liboqs
    # would evaluate every post-quantum output with the stub verifier (which
    # returns false) and could fork from liboqs-enabled nodes. Disabling
    # post-quantum support must therefore be a deliberate, explicit choice, not
    # an accident of a missing dependency.
    set(liboqs_too_old_note "")
    if(liboqs_CONSIDERED_VERSIONS)
      set(liboqs_too_old_note "An older liboqs was found and rejected: version ${liboqs_CONSIDERED_VERSIONS}.\n")
    elseif(LIBOQS_TOO_OLD)
      set(liboqs_too_old_note "An older liboqs was found and rejected: ${LIBOQS_TOO_OLD}.\n")
    endif()
    message(FATAL_ERROR
      "liboqs ${LIBOQS_MIN_VERSION} or newer not found, but WITH_LIBOQS is ON (the default).\n"
      "${liboqs_too_old_note}"
      "ML-DSA-44 (RIP-25) post-quantum signature verification is a consensus rule once the "
      "deployment activates, so a missing liboqs is a hard error rather than a silent downgrade.\n"
      "Fix one of:\n"
      "  - install liboqs ${LIBOQS_MIN_VERSION} or newer (https://github.com/open-quantum-safe/liboqs), "
      "or build via the depends system which provides it; or\n"
      "  - pass -DWITH_LIBOQS=OFF to opt out explicitly, ONLY for a build that will never act as a "
      "validating or mining node once RIP-25 has activated on its network."
    )
  endif()
else()
  # Explicit opt-out. Allowed, but loudly flagged: such a binary cannot verify
  # post-quantum outputs and is unsafe as a consensus node after activation.
  message(WARNING
    "WITH_LIBOQS=OFF: building WITHOUT ML-DSA-44 (RIP-25) post-quantum support.\n"
    "This binary MUST NOT be used as a validating or mining node once the RIP-25 deployment "
    "activates on the target network: it cannot verify post-quantum outputs and may fork from "
    "liboqs-enabled nodes."
  )
endif()
