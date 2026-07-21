// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Metal/MTLGfx.h"
#include "VideoBackends/Metal/MTLCaptureInterface.h"

#include "VideoBackends/Metal/MTLBoundingBox.h"
#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLPipeline.h"
#include "VideoBackends/Metal/MTLStateTracker.h"
#include "VideoBackends/Metal/MTLTexture.h"
#include "VideoBackends/Metal/MTLUtil.h"
#include "VideoBackends/Metal/MTLVertexFormat.h"
#include "VideoBackends/Metal/MTLVertexManager.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoBackendBase.h"

#include <fstream>
#include <mutex>

static std::function<void(id<MTLTexture>)> s_frame_capture_callback;

void Metal::Gfx::SetFrameCaptureCallback(std::function<void(id<MTLTexture>)> callback)
{
  s_frame_capture_callback = std::move(callback);
}

// C-linkage API for ARC-compiled callers (avoids MRCHelpers.h include)
// The mutex makes the capture handoff safe under dual-core: PresentBackbuffer
// publishes the ready slot from Metal's completion thread while the bridge
// takes it from the CPU thread's VI hook.
static std::mutex s_capture_mutex;
static constexpr int MAX_CAPTURE_SURFACES = 8;
static IOSurfaceRef s_capture_surfaces[MAX_CAPTURE_SURFACES];
static MRCOwned<id<MTLTexture>> s_capture_textures[MAX_CAPTURE_SURFACES];
static int s_capture_surface_count = 0;
static int s_capture_write_index = 0;   // video thread only
static int s_capture_ready_index = -1;  // guarded by s_capture_mutex

extern "C" void Dolphin_SetFrameCaptureSurfaces(const IOSurfaceRef* surfaces, int count)
{
  std::lock_guard<std::mutex> lock(s_capture_mutex);
  for (int i = 0; i < s_capture_surface_count; i++)
  {
    CFRelease(s_capture_surfaces[i]);
    s_capture_surfaces[i] = nullptr;
    s_capture_textures[i] = nullptr;
  }
  s_capture_surface_count = 0;
  s_capture_write_index = 0;
  s_capture_ready_index = -1;

  if (surfaces && count > 0)
  {
    if (count > MAX_CAPTURE_SURFACES)
      count = MAX_CAPTURE_SURFACES;
    for (int i = 0; i < count; i++)
      s_capture_surfaces[i] = (IOSurfaceRef)CFRetain(surfaces[i]);
    s_capture_surface_count = count;
    // Non-null sentinel — the actual blit happens in PresentBackbuffer().
    // (Textures wrapping the surfaces are created lazily there; g_device may
    // not exist yet when the frontend registers the ring.)
    s_frame_capture_callback = [](id<MTLTexture>) {};
  }
  else
  {
    s_frame_capture_callback = nullptr;
  }
}

extern "C" int Dolphin_TakeLatestFrameSurface(void)
{
  std::lock_guard<std::mutex> lock(s_capture_mutex);
  const int index = s_capture_ready_index;
  s_capture_ready_index = -1;
  return index;
}

Metal::Gfx::Gfx(MRCOwned<CAMetalLayer*> layer) : m_layer(std::move(layer))
{
  UpdateActiveConfig();
#if TARGET_OS_OSX
  [m_layer setDisplaySyncEnabled:g_ActiveConfig.bVSyncActive];
#endif

  SetupSurface();
  g_state_tracker->FlushEncoders();
}

Metal::Gfx::~Gfx() = default;

bool Metal::Gfx::IsHeadless() const
{
  return m_layer == nullptr;
}

// MARK: Texture Creation

static MTLTextureType FromAbstract(AbstractTextureType type, bool multisample)
{
  switch (type)
  {
  case AbstractTextureType::Texture_2D:
    return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
  case AbstractTextureType::Texture_2DArray:
    return multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
  case AbstractTextureType::Texture_CubeMap:
    return MTLTextureTypeCube;
  }

  ASSERT(false);
  return MTLTextureType2DArray;
}

std::unique_ptr<AbstractTexture> Metal::Gfx::CreateTexture(const TextureConfig& config,
                                                           std::string_view name)
{
  @autoreleasepool
  {
    MRCOwned<MTLTextureDescriptor*> desc = MRCTransfer([MTLTextureDescriptor new]);
    [desc setTextureType:FromAbstract(config.type, config.samples > 1)];
    [desc setPixelFormat:Util::FromAbstract(config.format)];
    [desc setWidth:config.width];
    [desc setHeight:config.height];
    [desc setMipmapLevelCount:config.levels];
    [desc setArrayLength:config.layers];
    [desc setSampleCount:config.samples];
    [desc setStorageMode:MTLStorageModePrivate];
    MTLTextureUsage usage = MTLTextureUsageShaderRead;
    if (config.IsRenderTarget())
      usage |= MTLTextureUsageRenderTarget;
    if (config.IsComputeImage())
      usage |= MTLTextureUsageShaderWrite;
    [desc setUsage:usage];
    id<MTLTexture> texture = [g_device newTextureWithDescriptor:desc];
    if (!texture)
      return nullptr;

    if (name.empty())
      [texture setLabel:[NSString stringWithFormat:@"Texture %d", m_texture_counter++]];
    else
      [texture setLabel:MRCTransfer([[NSString alloc] initWithBytes:name.data()
                                                             length:name.size()
                                                           encoding:NSUTF8StringEncoding])];
    return std::make_unique<Texture>(MRCTransfer(texture), config);
  }
}

std::unique_ptr<AbstractStagingTexture>
Metal::Gfx::CreateStagingTexture(StagingTextureType type, const TextureConfig& config)
{
  @autoreleasepool
  {
    const size_t stride = config.GetStride();
    const size_t buffer_size = stride * static_cast<size_t>(config.height);

    MTLResourceOptions options = MTLStorageModeShared;
    if (type == StagingTextureType::Upload)
      options |= MTLResourceCPUCacheModeWriteCombined;

    id<MTLBuffer> buffer = [g_device newBufferWithLength:buffer_size options:options];
    if (!buffer)
      return nullptr;
    [buffer
        setLabel:[NSString stringWithFormat:@"Staging Texture %d", m_staging_texture_counter++]];
    return std::make_unique<StagingTexture>(MRCTransfer(buffer), type, config);
  }
}

std::unique_ptr<AbstractFramebuffer>
Metal::Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                              std::vector<AbstractTexture*> additional_color_attachments)
{
  AbstractTexture* const either_attachment = color_attachment ? color_attachment : depth_attachment;
  return std::make_unique<Framebuffer>(
      color_attachment, depth_attachment, std::move(additional_color_attachments),
      either_attachment->GetWidth(), either_attachment->GetHeight(), either_attachment->GetLayers(),
      either_attachment->GetSamples());
}

// MARK: Pipeline Creation

std::unique_ptr<AbstractShader>
Metal::Gfx::CreateShaderFromSource(ShaderStage stage, std::string_view source,
                                   VideoCommon::ShaderIncluder* shader_includer,
                                   std::string_view name)
{
  std::optional<std::string> msl = Util::TranslateShaderToMSL(stage, source, shader_includer);
  if (!msl.has_value())
  {
    PanicAlertFmt("Failed to convert shader {} to MSL", name);
    return nullptr;
  }

  return CreateShaderFromMSL(stage, std::move(*msl), source, name);
}

std::unique_ptr<AbstractShader> Metal::Gfx::CreateShaderFromBinary(ShaderStage stage,
                                                                   const void* data, size_t length,
                                                                   std::string_view name)
{
  return CreateShaderFromMSL(stage, std::string(static_cast<const char*>(data), length), {}, name);
}

// clang-format off

static const char* StageFilename(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:   return "vs";
  case ShaderStage::Geometry: return "gs";
  case ShaderStage::Pixel:    return "ps";
  case ShaderStage::Compute:  return "cs";
  }
}

static NSString* GenericShaderName(ShaderStage stage)
{
  switch (stage)
  {
  case ShaderStage::Vertex:   return @"Vertex shader %d";
  case ShaderStage::Geometry: return @"Geometry shader %d";
  case ShaderStage::Pixel:    return @"Pixel shader %d";
  case ShaderStage::Compute:  return @"Compute shader %d";
  }
}

// clang-format on

std::unique_ptr<AbstractShader> Metal::Gfx::CreateShaderFromMSL(ShaderStage stage, std::string msl,
                                                                std::string_view glsl,
                                                                std::string_view name)
{
  @autoreleasepool
  {
    NSError* err = nullptr;
    auto DumpBadShader = [&](std::string_view msg) {
      static int counter = 0;
      std::string filename = VideoBackendBase::BadShaderFilename(StageFilename(stage), counter++);
      std::ofstream stream(filename);
      if (stream.good())
      {
        stream << msl << std::endl;
        stream << "/*" << std::endl;
        stream << msg << std::endl;
        stream << "Error:" << std::endl;
        stream << [[err localizedDescription] UTF8String] << std::endl;
        if (!glsl.empty())
        {
          stream << "Original GLSL:" << std::endl;
          stream << glsl << std::endl;
        }
        else
        {
          stream << "Shader was created with cached MSL so no GLSL is available." << std::endl;
        }
      }

      stream << std::endl;
      stream << "Dolphin Version: " << Common::GetScmRevStr() << std::endl;
      stream << "Video Backend: " << g_video_backend->GetDisplayName() << std::endl;
      stream << "*/" << std::endl;
      stream.close();

      PanicAlertFmt("{} (written to {})\n", msg, filename);
    };

    auto lib = MRCTransfer([g_device newLibraryWithSource:[NSString stringWithUTF8String:msl.data()]
                                                  options:nil
                                                    error:&err]);
    if (err)
    {
      DumpBadShader(fmt::format("Failed to compile {}", name));
      return nullptr;
    }
    auto fn = MRCTransfer([lib newFunctionWithName:@"main0"]);
    if (!fn)
    {
      DumpBadShader(fmt::format("Shader {} is missing its main0 function", name));
      return nullptr;
    }
    if (!name.empty())
      [fn setLabel:MRCTransfer([[NSString alloc] initWithBytes:name.data()
                                                        length:name.size()
                                                      encoding:NSUTF8StringEncoding])];
    else
      [fn setLabel:[NSString stringWithFormat:GenericShaderName(stage),
                                              m_shader_counter[static_cast<u32>(stage)]++]];
    [lib setLabel:[fn label]];
    if (stage == ShaderStage::Compute)
    {
      MTLComputePipelineReflection* reflection = nullptr;
      auto desc = [MTLComputePipelineDescriptor new];
      [desc setComputeFunction:fn];
      [desc setLabel:[fn label]];
      MRCOwned<id<MTLComputePipelineState>> pipeline =
          MRCTransfer([g_device newComputePipelineStateWithDescriptor:desc
                                                              options:MTLPipelineOptionArgumentInfo
                                                           reflection:&reflection
                                                                error:&err]);
      if (err)
      {
        DumpBadShader(fmt::format("Failed to compile compute pipeline {}", name));
        return nullptr;
      }
      return std::make_unique<ComputePipeline>(stage, reflection, std::move(msl), std::move(fn),
                                               std::move(pipeline));
    }
    return std::make_unique<Shader>(stage, std::move(msl), std::move(fn));
  }
}

std::unique_ptr<NativeVertexFormat>
Metal::Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  @autoreleasepool
  {
    return std::make_unique<VertexFormat>(vtx_decl);
  }
}

std::unique_ptr<AbstractPipeline> Metal::Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                             const void* cache_data,
                                                             size_t cache_data_length)
{
  return g_object_cache->CreatePipeline(config);
}

void Metal::Gfx::Flush()
{
  @autoreleasepool
  {
    g_state_tracker->FlushEncoders();
  }
}

void Metal::Gfx::WaitForGPUIdle()
{
  @autoreleasepool
  {
    g_state_tracker->FlushEncoders();
    g_state_tracker->WaitForFlushedEncoders();
  }
}

void Metal::Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

#if TARGET_OS_OSX
  if (bits & CONFIG_CHANGE_BIT_VSYNC)
    [m_layer setDisplaySyncEnabled:g_ActiveConfig.bVSyncActive];
#endif

  if (bits & CONFIG_CHANGE_BIT_ANISOTROPY)
  {
    g_object_cache->ReloadSamplers();
    g_state_tracker->ReloadSamplers();
  }
}

void Metal::Gfx::ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable,
                             bool alpha_enable, bool z_enable, u32 color, u32 z)
{
  u32 framebuffer_width = m_current_framebuffer->GetWidth();
  u32 framebuffer_height = m_current_framebuffer->GetHeight();
  // All Metal render passes are fullscreen, so we can only run a fast clear if the target is too
  if (target_rc == MathUtil::Rectangle<int>(0, 0, framebuffer_width, framebuffer_height))
  {
    // Determine whether the EFB has an alpha channel. If it doesn't, we can clear the alpha
    // channel to 0xFF. This hopefully allows us to use the fast path in most cases.
    if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16 ||
        bpmem.zcontrol.pixel_format == PixelFormat::RGB8_Z24 ||
        bpmem.zcontrol.pixel_format == PixelFormat::Z24)
    {
      // Force alpha writes, and clear the alpha channel. This is different from the other backends,
      // where the existing values of the alpha channel are preserved.
      alpha_enable = true;
      color &= 0x00FFFFFF;
    }

    bool c_ok = (color_enable && alpha_enable) ||
                g_state_tracker->GetCurrentFramebuffer()->GetColorFormat() ==
                    AbstractTextureFormat::Undefined;
    bool z_ok = z_enable || g_state_tracker->GetCurrentFramebuffer()->GetDepthFormat() ==
                                AbstractTextureFormat::Undefined;
    if (c_ok && z_ok)
    {
      @autoreleasepool
      {
        // clang-format off
        MTLClearColor clear_color = MTLClearColorMake(
            static_cast<double>((color >> 16) & 0xFF) / 255.0,
            static_cast<double>((color >>  8) & 0xFF) / 255.0,
            static_cast<double>((color >>  0) & 0xFF) / 255.0,
            static_cast<double>((color >> 24) & 0xFF) / 255.0);
        // clang-format on
        float z_normalized = static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
        if (!g_backend_info.bSupportsReversedDepthRange)
          z_normalized = 1.f - z_normalized;
        g_state_tracker->BeginClearRenderPass(clear_color, z_normalized);
        return;
      }
    }
  }

  g_state_tracker->EnableEncoderLabel(false);
  AbstractGfx::ClearRegion(target_rc, color_enable, alpha_enable, z_enable, color, z);
  g_state_tracker->EnableEncoderLabel(true);
}

void Metal::Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  g_state_tracker->SetPipeline(static_cast<const Pipeline*>(pipeline));
}

void Metal::Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  // Shouldn't be bound as a texture.
  if (AbstractTexture* color = framebuffer->GetColorAttachment())
    g_state_tracker->UnbindTexture(static_cast<Texture*>(color)->GetMTLTexture());
  if (AbstractTexture* depth = framebuffer->GetDepthAttachment())
    g_state_tracker->UnbindTexture(static_cast<Texture*>(depth)->GetMTLTexture());

  m_current_framebuffer = framebuffer;
  g_state_tracker->SetCurrentFramebuffer(static_cast<Framebuffer*>(framebuffer));
}

void Metal::Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  @autoreleasepool
  {
    SetFramebuffer(framebuffer);
    g_state_tracker->BeginRenderPass(MTLLoadActionDontCare);
  }
}

void Metal::Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                        const ClearColor& color_value, float depth_value)
{
  @autoreleasepool
  {
    SetFramebuffer(framebuffer);
    MTLClearColor color =
        MTLClearColorMake(color_value[0], color_value[1], color_value[2], color_value[3]);
    g_state_tracker->BeginClearRenderPass(color, depth_value);
  }
}

void Metal::Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  g_state_tracker->SetScissor(rc);
}

void Metal::Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  g_state_tracker->SetTexture(
      index, texture ? static_cast<const Texture*>(texture)->GetMTLTexture() : nullptr);
}

void Metal::Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  g_state_tracker->SetSampler(index, state);
}

void Metal::Gfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  g_state_tracker->SetTexture(index + VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS,
                              texture ? static_cast<const Texture*>(texture)->GetMTLTexture() :
                                        nullptr);
}

void Metal::Gfx::UnbindTexture(const AbstractTexture* texture)
{
  g_state_tracker->UnbindTexture(static_cast<const Texture*>(texture)->GetMTLTexture());
}

void Metal::Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                             float far_depth)
{
  g_state_tracker->SetViewport(x, y, width, height, near_depth, far_depth);
}

void Metal::Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  @autoreleasepool
  {
    g_state_tracker->Draw(base_vertex, num_vertices);
  }
}

void Metal::Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  @autoreleasepool
  {
    g_state_tracker->DrawIndexed(base_index, num_indices, base_vertex);
  }
}

void Metal::Gfx::DispatchComputeShader(const AbstractShader* shader,  //
                                       u32 groupsize_x, u32 groupsize_y, u32 groupsize_z,
                                       u32 groups_x, u32 groups_y, u32 groups_z)
{
  @autoreleasepool
  {
    g_state_tracker->SetPipeline(static_cast<const ComputePipeline*>(shader));
    g_state_tracker->DispatchComputeShader(groupsize_x, groupsize_y, groupsize_z,  //
                                           groups_x, groups_y, groups_z);
  }
}

bool Metal::Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  @autoreleasepool
  {
    CheckForSurfaceChange();
    CheckForSurfaceResize();
    m_drawable = MRCRetain([m_layer nextDrawable]);
    m_backbuffer->UpdateBackbufferTexture([m_drawable texture]);
    SetAndClearFramebuffer(m_backbuffer.get(), clear_color);
    return m_drawable != nullptr;
  }
}

void Metal::Gfx::PresentBackbuffer()
{
  @autoreleasepool
  {
    g_state_tracker->EndRenderPass();
    if (m_drawable)
    {
      if (s_frame_capture_callback)
      {
        // Zero-copy offscreen capture path: blit the drawable texture into the
        // next slot of the frontend's IOSurface ring on the GPU, and publish
        // the slot index from the command buffer's COMPLETION handler. The
        // frame never touches the CPU — the frontend displays the IOSurface
        // directly. (An earlier version blitted to a staging buffer and
        // memcpy'd it out after WaitForFlushedEncoders — a GPU-completion
        // stall on the video thread that grew with scene cost, plus two
        // full-frame CPU copies per present.) The frame the frontend consumes
        // can trail by one present, which the VI-hook handoff tolerates.
        id<MTLTexture> tex = [m_drawable texture];
        const int w = (int)tex.width;
        const int h = (int)tex.height;

        int slot = -1;
        id<MTLTexture> dst = nil;
        {
          std::lock_guard<std::mutex> lock(s_capture_mutex);
          if (s_capture_surface_count > 0 && w > 0 && h > 0)
          {
            slot = s_capture_write_index;
            s_capture_write_index = (slot + 1) % s_capture_surface_count;

            IOSurfaceRef surface = s_capture_surfaces[slot];
            if (!s_capture_textures[slot] && (int)IOSurfaceGetWidth(surface) == w &&
                (int)IOSurfaceGetHeight(surface) == h)
            {
              MTLTextureDescriptor* desc = [MTLTextureDescriptor
                  texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                               width:w
                                              height:h
                                           mipmapped:NO];
              s_capture_textures[slot] =
                  MRCTransfer([g_device newTextureWithDescriptor:desc iosurface:surface plane:0]);
            }
            dst = s_capture_textures[slot];
          }
        }

        if (dst)
        {
          id<MTLBlitCommandEncoder> blit =
              [g_state_tracker->GetRenderCmdBuf() blitCommandEncoder];
          [blit copyFromTexture:tex
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:dst
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
          [blit endEncoding];

          [g_state_tracker->GetRenderCmdBuf()
              addCompletedHandler:[slot](id<MTLCommandBuffer>) {
                std::lock_guard<std::mutex> lock(s_capture_mutex);
                // Guard against a ring swap between encode and completion.
                if (slot < s_capture_surface_count)
                  s_capture_ready_index = slot;
              }];
        }

        m_backbuffer->UpdateBackbufferTexture(nullptr);
        g_state_tracker->FlushEncoders();

        m_drawable = nullptr;
        return;
      }
      else
      {
        // Normal on-screen present path
        if (g_ActiveConfig.iUsePresentDrawable == TriState::On ||
            (g_ActiveConfig.iUsePresentDrawable == TriState::Auto && g_ActiveConfig.bVSyncActive))
          [g_state_tracker->GetRenderCmdBuf() presentDrawable:m_drawable];
        else
          [g_state_tracker->GetRenderCmdBuf()
              addScheduledHandler:[drawable = std::move(m_drawable)](id) { [drawable present]; }];
        m_backbuffer->UpdateBackbufferTexture(nullptr);
        m_drawable = nullptr;
      }
    }
    g_state_tracker->FlushEncoders();
  }
}

void Metal::Gfx::CheckForSurfaceChange()
{
  if (!g_presenter->SurfaceChangedTestAndClear())
    return;
  m_layer = MRCRetain(static_cast<CAMetalLayer*>(g_presenter->GetNewSurfaceHandle()));
  SetupSurface();
}

void Metal::Gfx::CheckForSurfaceResize()
{
  if (!g_presenter->SurfaceResizedTestAndClear())
    return;
  SetupSurface();
}

void Metal::Gfx::SetupSurface()
{
  auto info = GetSurfaceInfo();

  [m_layer setDrawableSize:{static_cast<double>(info.width), static_cast<double>(info.height)}];

  TextureConfig cfg(info.width, info.height, 1, 1, 1, info.format, AbstractTextureFlag_RenderTarget,
                    AbstractTextureType::Texture_2DArray);
  m_bb_texture = std::make_unique<Texture>(nullptr, cfg);
  m_backbuffer = std::make_unique<Framebuffer>(
      m_bb_texture.get(), nullptr, std::vector<AbstractTexture*>{}, info.width, info.height, 1, 1);

  if (g_presenter)
    g_presenter->SetBackbuffer(info);
}

SurfaceInfo Metal::Gfx::GetSurfaceInfo() const
{
  if (!m_layer)  // Headless
    return {};

  CGSize size = [m_layer bounds].size;
  const float scale = [m_layer contentsScale];

  return {static_cast<u32>(size.width * scale), static_cast<u32>(size.height * scale), scale,
          Util::ToAbstract([m_layer pixelFormat])};
}
