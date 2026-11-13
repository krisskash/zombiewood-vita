cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR
      "VITASDK not set. Run: export VITASDK=/usr/local/vitasdk")
  endif()
endif()

project(zombiewood_vita C CXX)

set(VITA_APP_NAME "Zombiewood")
set(VITA_TITLEID  "ZMBW00001")
set(VITA_VERSION  "01.00")
option(VITA3K_BUILD "Build with Vita3K-compatible loader path" OFF)
set(VITA3K_VITAGL_DIR
  "${CMAKE_SOURCE_DIR}/../../third_party/vitaGL-vita3k-latest")

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# ──────────────────────────────────────────────────────────
# Compiler flags
# ──────────────────────────────────────────────────────────
set(CMAKE_C_FLAGS
  "${CMAKE_C_FLAGS} \
  -O2 -g \
  -fno-strict-aliasing \
  -fno-optimize-sibling-calls \
  -Wl,-q \
  -D_GNU_SOURCE \
  -DNDEBUG"
)

# ──────────────────────────────────────────────────────────
# Source files
# so_util.c must be copied from TheOfficialFloW/gtasa_vita
# ──────────────────────────────────────────────────────────
add_executable(zombiewood_vita
  main.c
  dynlib.c
  so_util.c
  audio_sl.c
  zw_prof.c
  zw_gxmstat.c
  zw_glq.c
)
# Measurement build only: cmake -DZW_GXMSTAT=1 times the SceGxm calls made inside draw calls (zw_gxmstat.c).
if(ZW_GXMSTAT)
  target_compile_definitions(zombiewood_vita PRIVATE ZW_GXMSTAT=1 ZW_DRAW_STATS=1 ZW_PERIODIC_LOG=1)
  foreach(f sceGxmDraw sceGxmSetUniformDataF sceGxmReserveVertexDefaultUniformBuffer sceGxmReserveFragmentDefaultUniformBuffer
            sceGxmSetVertexStream sceGxmSetFragmentTexture sceGxmSetVertexProgram sceGxmSetFragmentProgram sceGxmTextureValidate
            sceClibMemcpy sceGxmShaderPatcherCreateVertexProgram sceGxmShaderPatcherCreateFragmentProgram)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--wrap=${f}")
  endforeach()
endif()
# Measurement build only: cmake -DZW_CALLPROF=1 (call profiler, see zw_prof.c). Never ship it.
if(ZW_CALLPROF)
  target_compile_definitions(zombiewood_vita PRIVATE ZW_CALLPROF=1)
endif()
# Extra compile definitions for measurement builds, e.g. -DZW_DEFS="ZW_PERIODIC_LOG=1;ZW_GXM_AB=1"
if(ZW_DEFS)
  target_compile_definitions(zombiewood_vita PRIVATE ${ZW_DEFS})
endif()
# libvitaGL.a contains C++ object files that reference operator new/delete,
# exception machinery, std::__cxx11::basic_string, and __gxx_personality_v0.
# All sources here are .c, so CMake would link with gcc by default and skip
# libstdc++. Forcing the linker language to CXX makes CMake invoke g++ for
# the link, which pulls in libstdc++/libsupc++ and resolves those symbols.
set_target_properties(zombiewood_vita PROPERTIES LINKER_LANGUAGE CXX)

# ──────────────────────────────────────────────────────────
# Include directories
# ──────────────────────────────────────────────────────────
target_include_directories(zombiewood_vita PRIVATE
  ${CMAKE_SOURCE_DIR}
  $ENV{VITASDK}/arm-vita-eabi/include
)

if(VITA3K_BUILD)
  target_compile_definitions(zombiewood_vita PRIVATE VITA3K_BUILD=1)
  target_include_directories(zombiewood_vita BEFORE PRIVATE
    ${VITA3K_VITAGL_DIR}/source
  )
  if(NOT EXISTS "${VITA3K_VITAGL_DIR}/libvitaGL.a")
    message(FATAL_ERROR
      "Vita3K vitaGL library missing: ${VITA3K_VITAGL_DIR}/libvitaGL.a")
  endif()
  set(VITAGL_LIBRARY "${VITA3K_VITAGL_DIR}/libvitaGL.a")
else()
  set(VITAGL_LIBRARY vitaGL)
endif()

# ──────────────────────────────────────────────────────────
# Linked libraries
# Order matters: list static/stubs before shared libs
# ──────────────────────────────────────────────────────────
target_link_libraries(zombiewood_vita
  # Graphics
  ${VITAGL_LIBRARY}
  vitashark
  SceShaccCg_stub
  mathneon

  # Kernel / system stubs
  SceKernelModulemgr_stub
  SceKernelDmacMgr_stub
  SceAppMgr_stub
  SceAppUtil_stub

  # Display / GXM
  SceDisplay_stub
  SceGxm_stub

  # Input
  SceCtrl_stub
  SceTouch_stub
  SceMotion_stub

  # Audio
  SceAudio_stub
  SceAudioIn_stub

  # Power
  ScePower_stub

  # I/O / filesystem
  SceIofilemgr_stub
  SceRtc_stub

  # Networking (game makes connectivity checks)
  SceNet_stub
  SceNetCtl_stub
  SceSsl_stub
  SceHttp_stub

  # Common dialog (for vitaGL's dialog handling)
  SceCommonDialog_stub

  # v5.06: fast PNG decode
  png
  z

  # Standard C / POSIX
  m
  pthread
)

if(NOT VITA3K_BUILD)
  target_link_libraries(zombiewood_vita kubridge_stub)
endif()

# ──────────────────────────────────────────────────────────
# Self (signed ELF for Vita)
# ──────────────────────────────────────────────────────────
vita_create_self(zombiewood.self zombiewood_vita UNSAFE)

# ──────────────────────────────────────────────────────────
# VPK  (Title ID must be exactly 9 chars: 4 uppercase + 5 digits)
# v5.13: loader only. No Gameloft file is packaged; the player copies libZombieWood.so,
# libgenerator.so and both OBBs from their own Zombiewood 1.5.3 to ux0:data/zombiewood/.
# ──────────────────────────────────────────────────────────
# Bubble/LiveArea art (Gameloft artwork) is kept out of git; packed only if present locally.
set(ZW_SCE_SYS_FILES "")
if(EXISTS ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png)
  foreach(f icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
    list(APPEND ZW_SCE_SYS_FILES ${CMAKE_SOURCE_DIR}/sce_sys/${f} sce_sys/${f})
  endforeach()
  set(ZW_SCE_SYS_FILES FILE ${ZW_SCE_SYS_FILES})
endif()

vita_create_vpk(zombiewood.vpk ZMBW00001
  zombiewood.self
  NAME    "Zombiewood"
  VERSION "01.00"
  ${ZW_SCE_SYS_FILES}
)

# ──────────────────────────────────────────────────────────
# Convenience targets
# ──────────────────────────────────────────────────────────
add_custom_target(send
  COMMAND curl -T zombiewood.vpk ftp://$(VITA_IP):1337/ux0:/vpk/zombiewood.vpk
  DEPENDS zombiewood.vpk
  COMMENT "Uploading VPK to Vita via FTP"
)

message(STATUS "")
message(STATUS "=== Zombiewood Vita Build Configuration ===")
message(STATUS "  VITASDK : $ENV{VITASDK}")
message(STATUS "  Build   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vita3K  : ${VITA3K_BUILD}")
message(STATUS "  Output  : zombiewood.vpk  (Title ID: ZMBW00001)")
message(STATUS "===========================================")
message(STATUS "")
