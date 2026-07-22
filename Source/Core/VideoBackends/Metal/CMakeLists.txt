add_library(videometal
  MRCHelpers.h
  MTLBoundingBox.mm
  MTLBoundingBox.h
  MTLCaptureInterface.h
  MTLGfx.mm
  MTLGfx.h
  MTLMain.mm
  MTLObjectCache.h
  MTLObjectCache.mm
  MTLPerfQuery.mm
  MTLPerfQuery.h
  MTLPipeline.mm
  MTLPipeline.h
  MTLShader.mm
  MTLShader.h
  MTLStateTracker.mm
  MTLStateTracker.h
  MTLTexture.mm
  MTLTexture.h
  MTLUtil.mm
  MTLUtil.h
  MTLVertexFormat.mm
  MTLVertexFormat.h
  MTLVertexManager.mm
  MTLVertexManager.h
  VideoBackend.h
)

find_library(METAL_LIBRARY Metal)
find_library(QUARTZCORE_LIBRARY QuartzCore)
# MTLGfx.mm's zero-copy capture path (Dolphin_SetFrameCaptureSurfaces) uses the
# IOSurface C API; the iOS build links it via the GCDeltaCore Xcode project.
find_library(IOSURFACE_LIBRARY IOSurface)

target_link_libraries(videometal
PUBLIC
  common
  videocommon
PRIVATE
  spirv_cross
  ${FOUNDATION_LIBRARY}
  ${METAL_LIBRARY}
  ${QUARTZCORE_LIBRARY}
  ${IOSURFACE_LIBRARY}
)

target_compile_options(videometal PRIVATE -fno-objc-arc)
