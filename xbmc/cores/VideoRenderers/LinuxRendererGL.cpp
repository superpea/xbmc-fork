/*
* XBoxMediaCenter
* Copyright (c) 2003 Frodo/jcmarshall
* Portions Copyright (c) by the authors of ffmpeg / xvid /mplayer
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#include "stdafx.h"
#include "LinuxRendererGL.h"
#include "../../Application.h"
#include "../../Util.h"
#include "../../Settings.h"
#include "../../XBVideoConfig.h"
#include "../../../guilib/Surface.h"
using namespace Surface;

// http://www.martinreddy.net/gfx/faqs/colorconv.faq

YUVRANGE yuv_range_lim =  { 16, 235, 16, 240, 16, 240 };
YUVRANGE yuv_range_full = {  0, 255,  0, 255,  0, 255 };

YUVCOEF yuv_coef_bt601 = {
     0.0f,   1.403f,
  -0.344f,  -0.714f,
   1.773f,     0.0f,
};

YUVCOEF yuv_coef_bt709 = {
     0.0f,  1.5701f,
 -0.1870f, -0.4664f,
  1.8556f,     0.0f, /* page above have the 1.8556f as negative */
};

YUVCOEF yuv_coef_ebu = {
    0.0f,  1.140f,
 -0.396f, -0.581f,
  2.029f,    0.0f, 
};

YUVCOEF yuv_coef_smtp240m = {
     0.0f,  1.5756f,
 -0.2253f, -0.5000f, /* page above have the 0.5000f as positive */
  1.8270f,     0.0f,  
};


CLinuxRendererGL::CLinuxRendererGL()
{
  m_pBuffer = NULL;
  m_textureTarget = GL_TEXTURE_2D;
  m_fSourceFrameRatio = 1.0f;
  m_iResolution = PAL_4x3;
  for (int i = 0; i < NUM_BUFFERS; i++)
  {
    m_pOSDYTexture[i] = 0;
    m_pOSDATexture[i] = 0;

    // possiblly not needed?
    //m_eventTexturesDone[i] = CreateEvent(NULL,FALSE,TRUE,NULL);
    //m_eventOSDDone[i] = CreateEvent(NULL,TRUE,TRUE,NULL);
  }
  m_shaderProgram = 0;
  m_fragmentShader = 0;
  m_renderMethod = RENDER_GLSL;
  m_yTex = 0;
  m_uTex = 0;
  m_vTex = 0;

  m_iYV12RenderBuffer = 0;

  memset(m_image, 0, sizeof(m_image));
  memset(m_YUVTexture, 0, sizeof(m_YUVTexture));

  m_rgbBuffer = NULL;
  m_rgbBufferSize = 0;
}

CLinuxRendererGL::~CLinuxRendererGL()
{
  UnInit();
  for (int i = 0; i < NUM_BUFFERS; i++)
  {
    //CloseHandle(m_eventTexturesDone[i]);
    //CloseHandle(m_eventOSDDone[i]);
  }
  if (m_pBuffer)
  {
    delete m_pBuffer;
  }
  if (m_rgbBuffer != NULL) {
    delete [] m_rgbBuffer;
    m_rgbBuffer = NULL;
  }
}

//********************************************************************************************************
void CLinuxRendererGL::DeleteOSDTextures(int index)
{
  CSingleLock lock(g_graphicsContext);
  if (m_pOSDYTexture[index])
  {
    g_graphicsContext.BeginPaint();
    if (glIsTexture(m_pOSDYTexture[index]))
      glDeleteTextures(1, &m_pOSDYTexture[index]);
    g_graphicsContext.EndPaint();
    m_pOSDYTexture[index] = 0;
  }
  if (m_pOSDATexture[index])
  {
    g_graphicsContext.BeginPaint();
    if (glIsTexture(m_pOSDATexture[index]))
      glDeleteTextures(1, &m_pOSDATexture[index]);
    g_graphicsContext.EndPaint();
    m_pOSDATexture[index] = 0;
    CLog::Log(LOGDEBUG, "Deleted OSD textures (%i)", index);
  }
  m_iOSDTextureHeight[index] = 0;
}

void CLinuxRendererGL::Setup_Y8A8Render()
{

}

//***************************************************************************************
// CalculateFrameAspectRatio()
//
// Considers the source frame size and output frame size (as suggested by mplayer)
// to determine if the pixels in the source are not square.  It calculates the aspect
// ratio of the output frame.  We consider the cases of VCD, SVCD and DVD separately,
// as these are intended to be viewed on a non-square pixel TV set, so the pixels are
// defined to be the same ratio as the intended display pixels.
// These formats are determined by frame size.
//***************************************************************************************
void CLinuxRendererGL::CalculateFrameAspectRatio(int desired_width, int desired_height)
{
  m_fSourceFrameRatio = (float)desired_width / desired_height;

  // Check whether mplayer has decided that the size of the video file should be changed
  // This indicates either a scaling has taken place (which we didn't ask for) or it has
  // found an aspect ratio parameter from the file, and is changing the frame size based
  // on that.
  if (m_iSourceWidth == desired_width && m_iSourceHeight == desired_height)
    return ;

  // mplayer is scaling in one or both directions.  We must alter our Source Pixel Ratio
  float fImageFrameRatio = (float)m_iSourceWidth / m_iSourceHeight;

  // OK, most sources will be correct now, except those that are intended
  // to be displayed on non-square pixel based output devices (ie PAL or NTSC TVs)
  // This includes VCD, SVCD, and DVD (and possibly others that we are not doing yet)
  // For this, we can base the pixel ratio on the pixel ratios of PAL and NTSC,
  // though we will need to adjust for anamorphic sources (ie those whose
  // output frame ratio is not 4:3) and for SVCDs which have 2/3rds the
  // horizontal resolution of the default NTSC or PAL frame sizes

  // The following are the defined standard ratios for PAL and NTSC pixels
  float fPALPixelRatio = 128.0f / 117.0f;
  float fNTSCPixelRatio = 4320.0f / 4739.0f;

  // Calculate the correction needed for anamorphic sources
  float fNon4by3Correction = m_fSourceFrameRatio / (4.0f / 3.0f);

  // Finally, check for a VCD, SVCD or DVD frame size as these need special aspect ratios
  if (m_iSourceWidth == 352)
  { // VCD?
    if (m_iSourceHeight == 240) // NTSC
      m_fSourceFrameRatio = fImageFrameRatio * fNTSCPixelRatio;
    if (m_iSourceHeight == 288) // PAL
      m_fSourceFrameRatio = fImageFrameRatio * fPALPixelRatio;
  }
  if (m_iSourceWidth == 480)
  { // SVCD?
    if (m_iSourceHeight == 480) // NTSC
      m_fSourceFrameRatio = fImageFrameRatio * 3.0f / 2.0f * fNTSCPixelRatio * fNon4by3Correction;
    if (m_iSourceHeight == 576) // PAL
      m_fSourceFrameRatio = fImageFrameRatio * 3.0f / 2.0f * fPALPixelRatio * fNon4by3Correction;
  }
  if (m_iSourceWidth == 720)
  { // DVD?
    if (m_iSourceHeight == 480) // NTSC
      m_fSourceFrameRatio = fImageFrameRatio * fNTSCPixelRatio * fNon4by3Correction;
    if (m_iSourceHeight == 576) // PAL
      m_fSourceFrameRatio = fImageFrameRatio * fPALPixelRatio * fNon4by3Correction;
  }
}

//***********************************************************************************************************
void CLinuxRendererGL::CopyAlpha(int w, int h, unsigned char* src, unsigned char *srca, int srcstride, unsigned char* dst, unsigned char* dsta, int dststride)
{
  for (int y = 0; y < h; ++y)
  {
    memcpy(dst, src, w);
    memcpy(dsta, srca, w);
    src += srcstride;
    srca += srcstride;
    dst += dststride;
    dsta += dststride;
  }
}

void CLinuxRendererGL::DrawAlpha(int x0, int y0, int w, int h, unsigned char *src, unsigned char *srca, int stride)
{

}

//********************************************************************************************************
void CLinuxRendererGL::RenderOSD()
{

}

//********************************************************************************************************
//Get resolution based on current mode.
RESOLUTION CLinuxRendererGL::GetResolution()
{
  if (g_graphicsContext.IsFullScreenVideo() || g_graphicsContext.IsCalibrating())
  {
    return m_iResolution;
  }
  return g_graphicsContext.GetVideoResolution();
}

float CLinuxRendererGL::GetAspectRatio()
{
  float fWidth = (float)m_iSourceWidth - g_stSettings.m_currentVideoSettings.m_CropLeft - g_stSettings.m_currentVideoSettings.m_CropRight;
  float fHeight = (float)m_iSourceHeight - g_stSettings.m_currentVideoSettings.m_CropTop - g_stSettings.m_currentVideoSettings.m_CropBottom;
  return m_fSourceFrameRatio * fWidth / fHeight * m_iSourceHeight / m_iSourceWidth;
}

void CLinuxRendererGL::GetVideoRect(RECT &rectSrc, RECT &rectDest)
{
  rectSrc = rs;
  rectDest = rd;
}

void CLinuxRendererGL::CalcNormalDisplayRect(float fOffsetX1, float fOffsetY1, float fScreenWidth, float fScreenHeight, float fInputFrameRatio, float fZoomAmount)
{
  // scale up image as much as possible
  // and keep the aspect ratio (introduces with black bars)
  // calculate the correct output frame ratio (using the users pixel ratio setting
  // and the output pixel ratio setting)

  float fOutputFrameRatio = fInputFrameRatio / g_settings.m_ResInfo[GetResolution()].fPixelRatio;

  // maximize the movie width
  float fNewWidth = fScreenWidth;
  float fNewHeight = fNewWidth / fOutputFrameRatio;

  if (fNewHeight > fScreenHeight)
  {
    fNewHeight = fScreenHeight;
    fNewWidth = fNewHeight * fOutputFrameRatio;
  }

  // Scale the movie up by set zoom amount
  fNewWidth *= fZoomAmount;
  fNewHeight *= fZoomAmount;

  // Centre the movie
  float fPosY = (fScreenHeight - fNewHeight) / 2;
  float fPosX = (fScreenWidth - fNewWidth) / 2;

  rd.left = (int)(fPosX + fOffsetX1);
  rd.right = (int)(rd.left + fNewWidth + 0.5f);
  rd.top = (int)(fPosY + fOffsetY1);
  rd.bottom = (int)(rd.top + fNewHeight + 0.5f);
}


void CLinuxRendererGL::ManageTextures()
{
  int neededbuffers = 0;
  m_NumYV12Buffers = 1;
  m_iYV12RenderBuffer = 0;
  return;

  //use 1 buffer in fullscreen mode and 2 buffers in windowed mode
  if (g_graphicsContext.IsFullScreenVideo())
  {
    if (m_NumOSDBuffers != 1)
    {
      m_iOSDRenderBuffer = 0;
      m_NumOSDBuffers = 1;
      m_OSDWidth = m_OSDHeight = 0;
      //delete second osd textures
      DeleteOSDTextures(1);
    }
    neededbuffers = 1;
  }
  else
  {
    if (m_NumOSDBuffers != 2)
    {
      m_NumOSDBuffers = 2;
      m_iOSDRenderBuffer = 0;
      m_OSDWidth = m_OSDHeight = 0;
      // buffers will be created on demand in DrawAlpha()
    }
    neededbuffers = 2;
  }

  if( m_NumYV12Buffers < neededbuffers )
  {
    for(int i = m_NumYV12Buffers; i<neededbuffers;i++)
      CreateYV12Texture(i);

    m_NumYV12Buffers = neededbuffers;
  }
  else if( m_NumYV12Buffers > neededbuffers )
  {
    // delete from the end
    int i = m_NumYV12Buffers-1;
    for(; i>=neededbuffers;i--)
    {
      // don't delete any frame that is in use
      if(m_image[i].flags & IMAGE_FLAG_DYNAMIC)
        break;
      DeleteYV12Texture(i);
    }
    if(m_iYV12RenderBuffer > i)
        m_iYV12RenderBuffer = i;
    m_NumYV12Buffers = i+1;
  }
}

void CLinuxRendererGL::ManageDisplay()
{
  const RECT& rv = g_graphicsContext.GetViewWindow();
  float fScreenWidth = (float)rv.right - rv.left;
  float fScreenHeight = (float)rv.bottom - rv.top;
  float fOffsetX1 = (float)rv.left;
  float fOffsetY1 = (float)rv.top;

  // source rect
  rs.left = g_stSettings.m_currentVideoSettings.m_CropLeft;
  rs.top = g_stSettings.m_currentVideoSettings.m_CropTop;
  rs.right = m_iSourceWidth - g_stSettings.m_currentVideoSettings.m_CropRight;
  rs.bottom = m_iSourceHeight - g_stSettings.m_currentVideoSettings.m_CropBottom;

  CalcNormalDisplayRect(fOffsetX1, fOffsetY1, fScreenWidth, fScreenHeight, GetAspectRatio() * g_stSettings.m_fPixelRatio, g_stSettings.m_fZoomAmount);
}

void CLinuxRendererGL::ChooseBestResolution(float fps)
{
  bool bUsingPAL = g_videoConfig.HasPAL();    // current video standard:PAL or NTSC
  bool bCanDoWidescreen = g_videoConfig.HasWidescreen(); // can widescreen be enabled?
  bool bWideScreenMode = false;

  // If the resolution selection is on Auto the following rules apply :
  //
  // BIOS Settings     ||Display resolution
  // WS|480p|720p/1080i||4:3 Videos     |16:6 Videos
  // ------------------||------------------------------
  // - | X  |    X     || 480p 4:3      | 720p
  // - | X  |    -     || 480p 4:3      | 480p 4:3
  // - | -  |    X     || 720p          | 720p
  // - | -  |    -     || NTSC/PAL 4:3  |NTSC/PAL 4:3
  // X | X  |    X     || 720p          | 720p
  // X | X  |    -     || 480p 4:3      | 480p 16:9
  // X | -  |    X     || 720p          | 720p
  // X | -  |    -     || NTSC/PAL 4:3  |NTSC/PAL 16:9

  // Work out if the framerate suits PAL50 or PAL60
  bool bPal60 = false;
  if (bUsingPAL && g_guiSettings.GetInt("videoplayer.framerateconversions") == FRAME_RATE_USE_PAL60 && g_videoConfig.HasPAL60())
  {
    // yes we're in PAL
    // yes PAL60 is allowed
    // yes dashboard PAL60 settings is enabled
    // Calculate the framerate difference from a divisor of 120fps and 100fps
    // (twice 60fps and 50fps to allow for 2:3 IVTC pulldown)
    float fFrameDifference60 = fabs(120.0f / fps - floor(120.0f / fps + 0.5f));
    float fFrameDifference50 = fabs(100.0f / fps - floor(100.0f / fps + 0.5f));

    // Make a decision based on the framerate difference
    if (fFrameDifference60 < fFrameDifference50)
      bPal60 = true;
  }

  // If the display resolution was specified by the user then use it, unless
  // it's a PAL setting, whereby we use the above setting to autoswitch to PAL60
  // if appropriate
  RESOLUTION DisplayRes = (RESOLUTION) g_guiSettings.GetInt("videoplayer.displayresolution");
  if ( DisplayRes != AUTORES )
  {
    if (bPal60)
    {
      if (DisplayRes == PAL_16x9) DisplayRes = PAL60_16x9;
      if (DisplayRes == PAL_4x3) DisplayRes = PAL60_4x3;
    }
    CLog::Log(LOGNOTICE, "Display resolution USER : %s (%d)", g_settings.m_ResInfo[DisplayRes].strMode, DisplayRes);
    m_iResolution = DisplayRes;
    return;
  }

  // Work out if framesize suits 4:3 or 16:9
  // Uses the frame aspect ratio of 8/(3*sqrt(3)) (=1.53960) which is the optimal point
  // where the percentage of black bars to screen area in 4:3 and 16:9 is equal
  static const float fOptimalSwitchPoint = 8.0f / (3.0f*sqrt(3.0f));
  if (bCanDoWidescreen && m_fSourceFrameRatio > fOptimalSwitchPoint)
    bWideScreenMode = true;

  // We are allowed to switch video resolutions, so we must
  // now decide which is the best resolution for the video we have
  if (bUsingPAL)  // PAL resolutions
  {
    // Currently does not allow HDTV solutions, as it is my beleif
    // that the XBox hardware only allows HDTV resolutions for NTSC systems.
    // this may need revising as more knowledge is obtained.
    if (bPal60)
    {
      if (bWideScreenMode)
        m_iResolution = PAL60_16x9;
      else
        m_iResolution = PAL60_4x3;
    }
    else    // PAL50
    {
      if (bWideScreenMode)
        m_iResolution = PAL_16x9;
      else
        m_iResolution = PAL_4x3;
    }
  }
  else      // NTSC resolutions
  {
    if (bCanDoWidescreen)
    { // The TV set has a wide screen (16:9)
      // So we always choose the best HD widescreen resolution no matter what
      // the video aspect ratio is
      // If the TV has no HD support widescreen mode is chossen according to video AR

      if (g_videoConfig.Has1080i())     // Widescreen TV with 1080i res
      m_iResolution = HDTV_1080i;
      else if (g_videoConfig.Has720p()) // Widescreen TV with 720p res
      m_iResolution = HDTV_720p;
      else if (g_videoConfig.Has480p()) // Widescreen TV with 480p
      {
        if (bWideScreenMode) // Choose widescreen mode according to video AR
          m_iResolution = HDTV_480p_16x9;
        else
          m_iResolution = HDTV_480p_4x3;
    }
      else if (bWideScreenMode)         // Standard 16:9 TV set with no HD
        m_iResolution = NTSC_16x9;
      else
        m_iResolution = NTSC_4x3;
    }
    else
    { // The TV set has a 4:3 aspect ratio
      // So 4:3 video sources will best fit the screen with 4:3 resolution
      // We choose 16:9 resolution only for 16:9 video sources

      if (m_fSourceFrameRatio >= 16.0f / 9.0f)
    {
        // The video fits best into widescreen modes so they are
        // the first choices
        if (g_videoConfig.Has1080i())
          m_iResolution = HDTV_1080i;
        else if (g_videoConfig.Has720p())
          m_iResolution = HDTV_720p;
        else if (g_videoConfig.Has480p())
          m_iResolution = HDTV_480p_4x3;
        else
          m_iResolution = NTSC_4x3;
      }
      else
      {
        // The video fits best into 4:3 modes so 480p
        // is the first choice
        if (g_videoConfig.Has480p())
          m_iResolution = HDTV_480p_4x3;
        else if (g_videoConfig.Has1080i())
          m_iResolution = HDTV_1080i;
        else if (g_videoConfig.Has720p())
          m_iResolution = HDTV_720p;
        else
          m_iResolution = NTSC_4x3;
      }
    }
  }

  CLog::Log(LOGNOTICE, "Display resolution AUTO : %s (%d)", g_settings.m_ResInfo[m_iResolution].strMode, m_iResolution);
}

bool CLinuxRendererGL::ValidateRenderTarget()
{
  if (!m_pBuffer)
  {
    // try pbuffer first
    m_pBuffer = new CSurface(256, 256, false, g_graphicsContext.getScreenSurface(), NULL, NULL, false, false, true);
    if (m_pBuffer && !m_pBuffer->IsValid())
    {
      delete m_pBuffer;
      m_pBuffer = new CSurface(256, 256, false, g_graphicsContext.getScreenSurface(), g_graphicsContext.getScreenSurface(), NULL, false, false, false);
      if (m_pBuffer->IsValid())
        CLog::Log(LOGNOTICE, "GL: Created non-pbuffer OpenGL context");
    }
  }
  if (m_pBuffer && m_pBuffer->IsValid())
  {
    int maj, min;
    m_pBuffer->GetGLVersion(maj, min);
    if (maj<2)
    {
      CLog::Log(LOGINFO, "GL: OpenGL version %d.%d detected", maj, min);
      if (!glewIsSupported("GL_ARB_texture_rectangle"))
      {
        CLog::Log(LOGERROR, "GL: GL_ARB_texture_rectangle not supported and OpenGL version is not 2.x");
        CLog::Log(LOGERROR, "GL: Reverting to POT textures");
	m_renderMethod |= RENDER_POT;
        return true;
      }
      CLog::Log(LOGINFO, "GL: NPOT textures are supported through GL_ARB_texture_rectangle extension");
      m_textureTarget = GL_TEXTURE_RECTANGLE_ARB;
      glEnable(GL_TEXTURE_RECTANGLE_ARB);
    } else {
      CLog::Log(LOGINFO, "GL: OpenGL version %d.%d detected", maj, min);
      CLog::Log(LOGINFO, "GL: NPOT textures are supported natively");
    }
  }
  else {
    CLog::Log(LOGERROR, "GL: Could not create OpenGL context that is required for video playback");
    return false;
  }
  return true;
}

bool CLinuxRendererGL::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags)
{
  m_fps = fps;
  m_iSourceWidth = width;
  m_iSourceHeight = height;

  // calculate the input frame aspect ratio
  CalculateFrameAspectRatio(d_width, d_height);
  ChooseBestResolution(m_fps);
  SetViewMode(g_stSettings.m_currentVideoSettings.m_ViewMode);
  ManageDisplay();

  // make sure we have a valid context that supports rendering
  if (!ValidateRenderTarget())
    return false;

  CreateYV12Texture(0);

  if (m_rgbBuffer != NULL) {
     delete [] m_rgbBuffer;
     m_rgbBuffer = NULL;
  }
 
  m_rgbBufferSize = width*height*4;
  m_rgbBuffer = new BYTE[m_rgbBufferSize];

  return true;
}

int CLinuxRendererGL::NextYV12Texture()
{
  return 0; //(m_iYV12RenderBuffer + 1) % m_NumYV12Buffers;
}

int CLinuxRendererGL::GetImage(YV12Image *image, int source, bool readonly)
{
  if (!image) return -1;

  //CSingleLock lock(g_graphicsContext);
   
  source = 0;

  if (!m_image[source].plane[0]) {
     CLog::Log(LOGDEBUG, "CLinuxRenderer::GetImage - image planes not allocated");
     return -1;
  }

  if (m_image[source].flags != 0) {
     CLog::Log(LOGDEBUG, "CLinuxRenderer::GetImage - request image but none to give");
     return -1;
  }

  m_image[source].flags = readonly?IMAGE_FLAG_READING:IMAGE_FLAG_WRITING;

  // copy the image - should be operator of YV12Image
  for (int p=0;p<MAX_PLANES;p++) {
     image->plane[p]=m_image[source].plane[p];
     image->stride[p] = m_image[source].stride[p];
  }
  image->width = m_image[source].width;
  image->height = m_image[source].height;
  image->flags = m_image[source].flags;
  image->cshift_x = m_image[source].cshift_x;
  image->cshift_y = m_image[source].cshift_y;
  image->texcoord_x = m_image[source].texcoord_x;
  image->texcoord_y = m_image[source].texcoord_y;

  return 0;
}

void CLinuxRendererGL::ReleaseImage(int source, bool preserve)
{
  
  // Eventual FIXME
  if (source!=0)
    source=0;

  m_image[source].flags = 0;

  YV12Image &im = m_image[source];
  YUVFIELDS &fields = m_YUVTexture[source];
  
  m_image[source].flags &= ~IMAGE_FLAG_INUSE;
  m_image[source].flags = 0;

  // if we don't have a shader, fallback to SW YUV2RGB for now
  
  if (m_renderMethod & RENDER_SW)
  {
    struct SwsContext *context = m_dllSwScale.sws_getContext(im.width, im.height, PIX_FMT_YUV420P, im.width, im.height, PIX_FMT_RGB32, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *src[] = { im.plane[0], im.plane[1], im.plane[2] };
    int     srcStride[] = { im.stride[0], im.stride[1], im.stride[2] };
    uint8_t *dst[] = { m_rgbBuffer, 0, 0 };
    int     dstStride[] = { m_iSourceWidth*4, 0, 0 };
    int ret = m_dllSwScale.sws_scale(context, src, srcStride, 0, im.height, dst, dstStride);
    
    m_dllSwScale.sws_freeContext(context);
  }
  
  
  g_graphicsContext.BeginPaint(m_pBuffer);
  glEnable(GL_TEXTURE_2D);
  VerifyGLState();
  glBindTexture(m_textureTarget, fields[0][0]);
  VerifyGLState();
  if (m_renderMethod & RENDER_SW)
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width, im.height, GL_BGRA, GL_UNSIGNED_BYTE, m_rgbBuffer);
  else
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width, im.height, GL_LUMINANCE, GL_UNSIGNED_BYTE, im.plane[0]);
  VerifyGLState();
  if (m_renderMethod & RENDER_GLSL)
  {    
    glBindTexture(m_textureTarget, fields[0][1]);
    VerifyGLState();
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width/2, im.height/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, im.plane[1]);
    VerifyGLState();
    glBindTexture(m_textureTarget, fields[0][2]);
    VerifyGLState();
    glTexSubImage2D(m_textureTarget, 0, 0, 0, im.width/2, im.height/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, im.plane[2]);
    VerifyGLState();
  }
  g_graphicsContext.EndPaint(m_pBuffer);
}

void CLinuxRendererGL::Reset()
{
  for(int i=0; i<m_NumYV12Buffers; i++)
  {
    /* reset all image flags, this will cleanup textures later */
    m_image[i].flags = 0;
    /* reset texure locks, abit uggly, could result in tearing */
    //SetEvent(m_eventTexturesDone[i]); 
  }
}

void CLinuxRendererGL::Update(bool bPauseDrawing)
{
  if (!m_bConfigured) return;
  //CSingleLock lock(g_graphicsContext);
  ManageDisplay();
  ManageTextures();
}

void CLinuxRendererGL::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  //if (!m_YUVTexture[m_iYV12RenderBuffer][FIELD_FULL][0]) return ;
  if (!m_YUVTexture[0][FIELD_FULL][0]) return ;

  //CSingleLock lock(g_graphicsContext);
  ManageDisplay();
  ManageTextures();

  g_graphicsContext.BeginPaint();

  if (clear) 
  {
    glClearColor(m_clearColour&0xff000000,
		 m_clearColour&0x00ff0000,
		 m_clearColour&0x0000ff00,
		 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0,0,0,0);
    if (alpha<255) 
    {
#warning Alpha blending currently disabled
      //glDisable(GL_BLEND);
    } else {
      //glDisable(GL_BLEND);
    }
  }
  glDisable(GL_BLEND);
  Render(flags);
  VerifyGLState();
  glEnable(GL_BLEND);
  g_graphicsContext.EndPaint();
}

void CLinuxRendererGL::FlipPage(int source)
{  
  CLog::Log(LOGNOTICE, "Calling FlipPage");
  //if( source >= 0 && source < m_NumYV12Buffers )
  m_iYV12RenderBuffer = source;
  //else
  //m_iYV12RenderBuffer = NextYV12Texture();
  
  /* we always decode into to the next buffer */
  //++m_iOSDRenderBuffer %= m_NumOSDBuffers;
  
  /* if osd wasn't rendered this time around, previuse should not be */
  /* displayed on next frame */
  
  if( !m_OSDRendered )
    m_OSDWidth = m_OSDHeight = 0;
  
  m_OSDRendered = false;
  
  g_graphicsContext.BeginPaint();
  g_graphicsContext.Flip();
  g_graphicsContext.EndPaint();

  return;
}


unsigned int CLinuxRendererGL::DrawSlice(unsigned char *src[], int stride[], int w, int h, int x, int y)
{
  BYTE *s;
  BYTE *d;
  int i, p;
  
  int index = NextYV12Texture();
  if( index < 0 )
    return -1;

  YV12Image &im = m_image[index];
  // copy Y
  p = 0;
  d = (BYTE*)im.plane[p] + im.stride[p] * y + x;
  s = src[p];
  for (i = 0;i < h;i++)
  {
    memcpy(d, s, w);
    s += stride[p];
    d += im.stride[p];
  }

  w >>= im.cshift_x; h >>= im.cshift_y;
  x >>= im.cshift_x; y >>= im.cshift_y;

  // copy U
  p = 1;
  d = (BYTE*)im.plane[p] + im.stride[p] * y + x;
  s = src[p];
  for (i = 0;i < h;i++)
  {
    memcpy(d, s, w);
    s += stride[p];
    d += im.stride[p];
  }

  // copy V
  p = 2;
  d = (BYTE*)im.plane[p] + im.stride[p] * y + x;
  s = src[p];
  for (i = 0;i < h;i++)
  {
    memcpy(d, s, w);
    s += stride[p];
    d += im.stride[p];
  }

  return 0;
}

unsigned int CLinuxRendererGL::PreInit()
{
  CSingleLock lock(g_graphicsContext);
  m_bConfigured = false;
  UnInit();
  m_iResolution = PAL_4x3;

  m_iOSDRenderBuffer = 0;
  m_iYV12RenderBuffer = 0;
  m_NumOSDBuffers = 0;
  m_NumYV12Buffers = 1;
  m_OSDHeight = m_OSDWidth = 0;
  m_OSDRendered = false;

  m_iOSDTextureWidth = 0;
  m_iOSDTextureHeight[0] = 0;
  m_iOSDTextureHeight[1] = 0;

  // setup the background colour
  m_clearColour = 0 ; //(g_advancedSettings.m_videoBlackBarColour & 0xff) * 0x010101;

  // make sure we have a valid context that supports rendering
  if (!ValidateRenderTarget())
    return false;

  if (!m_dllAvUtil.Load() || !m_dllAvCodec.Load() || !m_dllSwScale.Load()) 
	CLog::Log(LOGERROR,"CLinuxRendererGL::PreInit - failed to load rescale libraries!");

  m_dllSwScale.sws_rgb2rgb_init(SWS_CPU_CAPS_MMX2);

  if (!m_shaderProgram && glCreateProgram && 0)
  {

    const char* shaderv = 
      "void main()"
      "{"
      "gl_TexCoord[0] = gl_MultiTexCoord0;"
      "gl_TexCoord[1] = gl_MultiTexCoord1;"
      "gl_TexCoord[2] = gl_MultiTexCoord2;"
      "gl_Position = ftransform();"
      "}";

    const char* shaderf = 
      "uniform sampler2D ytex;"
      "uniform sampler2D utex;"
      "uniform sampler2D vtex;"
      "uniform float brightness;"
      "uniform float contrast;"
      "void main()"
      "{"
      "vec4 yuv, rgb;"
      "yuv.r = texture2D(ytex, gl_TexCoord[0].xy).r ;"
      "yuv.g = texture2D(utex, gl_TexCoord[1].xy).r ;"
      "yuv.b = texture2D(vtex, gl_TexCoord[2].xy).r ;"
      "yuv.r = ((yuv.r-0.5)*contrast)+0.5;"
      "yuv.r = clamp(yuv.r+brightness, 0.0, 1.0);"
      "yuv.r = 1.1643*(yuv.r-0.0625);"
      "yuv.g = yuv.g - 0.5;"
      "yuv.b = yuv.b - 0.5;"
      "rgb.r = clamp(yuv.r+1.5958*yuv.b, 0.0, 1.0);"
      "rgb.g = clamp(yuv.r-0.39173*yuv.g-0.81290*yuv.b, 0.0, 1.0);"
      "rgb.b = clamp(yuv.r+2.017*yuv.g, 0.0, 1.0);"
      "rgb = rgb + vec4(brightness);"
      "rgb.a = 1.0;"
      "gl_FragColor = rgb;"
      "}";

    const char* shaderfrect = 
      "uniform sampler2DRect ytex;"
      "uniform sampler2DRect utex;"
      "uniform sampler2DRect vtex;"
      "void main()"
      "{"
      "float y = texture2DRect(ytex, gl_TexCoord[0].xy).r ;"
      "float u = texture2DRect(utex, gl_TexCoord[1].xy).r ;"
      "float v = texture2DRect(vtex, gl_TexCoord[2].xy).r ;"
      "y = 1.1643*(y-0.0625);"
      "u = u - 0.5;"
      "v = v - 0.5;"
      "float r = clamp(y+1.5958*v, 0.0, 1.0);"
      "float g = clamp(y-0.39173*u-0.81290*v, 0.0, 1.0);"
      "float b = clamp(y+2.017*u, 0.0, 1.0);"
      "gl_FragColor = vec4(r, g, b, 1.0);"
      "}";

    GLint params[4]; 

    g_graphicsContext.BeginPaint(m_pBuffer);
    m_shaderProgram = glCreateProgram();
    m_fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    m_vertexShader = glCreateShader(GL_VERTEX_SHADER);
    if (m_textureTarget==GL_TEXTURE_2D)
    {
      glShaderSource(m_fragmentShader, 1, &shaderf, 0);
    } else {
      glShaderSource(m_fragmentShader, 1, &shaderfrect, 0);
    }
    glShaderSource(m_vertexShader, 1, &shaderv, 0);
    glCompileShader(m_fragmentShader);
    glCompileShader(m_vertexShader);
    glGetShaderiv(m_fragmentShader, GL_COMPILE_STATUS, params);
    if (params[0]!=GL_TRUE) 
    {
      GLchar log[512];
      OutputDebugString("Error compiling shader\n");
      glGetShaderInfoLog(m_fragmentShader, 512, NULL, log);
      OutputDebugString((const char*)log);
      OutputDebugString("\n");
    }
    glGetShaderiv(m_vertexShader, GL_COMPILE_STATUS, params);
    if (params[0]!=GL_TRUE) 
    {
      GLchar log[512];
      OutputDebugString("Error compiling shader\n");
      glGetShaderInfoLog(m_vertexShader, 512, NULL, log);
      OutputDebugString((const char*)log);
      OutputDebugString("\n");
    }
    glAttachShader(m_shaderProgram, m_fragmentShader);
    glAttachShader(m_shaderProgram, m_vertexShader);
    VerifyGLState();
    glLinkProgram(m_shaderProgram);
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, params);
    if (params[0]!=GL_TRUE) 
    {
      GLchar log[512];
      OutputDebugString("Error linking shader\n");
      glGetProgramInfoLog(m_shaderProgram, 512, NULL, log);
      OutputDebugString((const char*)log);
      OutputDebugString("\n");
    }
    glValidateProgram(m_shaderProgram);
    glGetProgramiv(m_shaderProgram, GL_VALIDATE_STATUS, params);
    if (params[0]!=GL_TRUE) 
    {
      GLchar log[512];
      OutputDebugString("Error validating shader\n");
      glGetProgramInfoLog(m_shaderProgram, 512, NULL, log);
      OutputDebugString((const char*)log);
      OutputDebugString("\n");
    }
    m_yTex = glGetUniformLocation(m_shaderProgram, "ytex");
    VerifyGLState();
    m_uTex = glGetUniformLocation(m_shaderProgram, "utex");
    VerifyGLState();
    m_vTex = glGetUniformLocation(m_shaderProgram, "vtex");
    VerifyGLState();
    m_brightness = glGetUniformLocation(m_shaderProgram, "brightness");
    VerifyGLState();
    m_contrast = glGetUniformLocation(m_shaderProgram, "contrast");
    VerifyGLState();
    g_graphicsContext.EndPaint(m_pBuffer);
    CLog::Log(LOGNOTICE, "GL: Successfully loaded GLSL shader");
    m_renderMethod = RENDER_GLSL;
  } else if (glewIsSupported("GL_ARB_fragment_shader")) {    
    // TODO
    CLog::Log(LOGNOTICE, "GL: Could not create GLSL shader since glCreateProgram not present");
    m_renderMethod = RENDER_SW ;
  } else {
    m_renderMethod = RENDER_SW ;
    CLog::Log(LOGNOTICE, "GL: Could not create ARB shader since GL_ARB_fragment_shader not present, falling back to SW colorspace conversion");
  }

  return 0;
}

void CLinuxRendererGL::UnInit()
{
  CSingleLock lock(g_graphicsContext);

  // YV12 textures, subtitle and osd stuff
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteYV12Texture(i);
    DeleteOSDTextures(i);
  }
  
  if (m_shaderProgram)
  {
    glDeleteShader(m_vertexShader);
    VerifyGLState();
    glDeleteShader(m_fragmentShader);
    VerifyGLState();
    glDeleteProgram(m_shaderProgram);
    VerifyGLState();
    m_fragmentShader = 0;
    m_vertexShader = 0;
    m_shaderProgram = 0;
    m_yTex = 0;
    m_uTex = 0;
    m_vTex = 0;
  }
  if (m_pBuffer)
  {
    delete m_pBuffer;
    m_pBuffer = 0;
  } 

  if (m_rgbBuffer != NULL) { 
     delete [] m_rgbBuffer;
     m_rgbBuffer = NULL;
  }

}

void CLinuxRendererGL::Render(DWORD flags)
{
  g_graphicsContext.BeginPaint();
  RenderLowMem(flags);
  VerifyGLState();

  if( flags & RENDER_FLAG_NOOSD ) 
  {
    g_graphicsContext.EndPaint();
    return;
  }

  // FIXME: OSD disabled for now
  /* general stuff */
  //RenderOSD();

  if (g_graphicsContext.IsFullScreenVideo())
  {
    if (g_application.NeedRenderFullScreen())
    { // render our subtitles and osd
      g_application.RenderFullScreen();
      VerifyGLState();
    }
    g_application.RenderMemoryStatus();
    VerifyGLState();
  }
  g_graphicsContext.EndPaint();
}

void CLinuxRendererGL::SetViewMode(int iViewMode)
{
  if (iViewMode < VIEW_MODE_NORMAL || iViewMode > VIEW_MODE_CUSTOM) iViewMode = VIEW_MODE_NORMAL;
  g_stSettings.m_currentVideoSettings.m_ViewMode = iViewMode;

  if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_NORMAL)
  { // normal mode...
    g_stSettings.m_fPixelRatio = 1.0;
    g_stSettings.m_fZoomAmount = 1.0;
    return ;
  }
  if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_CUSTOM)
  {
    g_stSettings.m_fZoomAmount = g_stSettings.m_currentVideoSettings.m_CustomZoomAmount;
    g_stSettings.m_fPixelRatio = g_stSettings.m_currentVideoSettings.m_CustomPixelRatio;
    return ;
  }

  // get our calibrated full screen resolution
  float fOffsetX1 = (float)g_settings.m_ResInfo[m_iResolution].Overscan.left;
  float fOffsetY1 = (float)g_settings.m_ResInfo[m_iResolution].Overscan.top;
  float fScreenWidth = (float)(g_settings.m_ResInfo[m_iResolution].Overscan.right - g_settings.m_ResInfo[m_iResolution].Overscan.left);
  float fScreenHeight = (float)(g_settings.m_ResInfo[m_iResolution].Overscan.bottom - g_settings.m_ResInfo[m_iResolution].Overscan.top);
  // and the source frame ratio
  float fSourceFrameRatio = GetAspectRatio();

  if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_ZOOM)
  { // zoom image so no black bars
    g_stSettings.m_fPixelRatio = 1.0;
    // calculate the desired output ratio
    float fOutputFrameRatio = fSourceFrameRatio * g_stSettings.m_fPixelRatio / g_settings.m_ResInfo[m_iResolution].fPixelRatio;
    // now calculate the correct zoom amount.  First zoom to full height.
    float fNewHeight = fScreenHeight;
    float fNewWidth = fNewHeight * fOutputFrameRatio;
    g_stSettings.m_fZoomAmount = fNewWidth / fScreenWidth;
    if (fNewWidth < fScreenWidth)
    { // zoom to full width
      fNewWidth = fScreenWidth;
      fNewHeight = fNewWidth / fOutputFrameRatio;
      g_stSettings.m_fZoomAmount = fNewHeight / fScreenHeight;
    }
  }
  else if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_STRETCH_4x3)
  { // stretch image to 4:3 ratio
    g_stSettings.m_fZoomAmount = 1.0;
    if (m_iResolution == PAL_4x3 || m_iResolution == PAL60_4x3 || m_iResolution == NTSC_4x3 || m_iResolution == HDTV_480p_4x3)
    { // stretch to the limits of the 4:3 screen.
      // incorrect behaviour, but it's what the users want, so...
      g_stSettings.m_fPixelRatio = (fScreenWidth / fScreenHeight) * g_settings.m_ResInfo[m_iResolution].fPixelRatio / fSourceFrameRatio;
    }
    else
    {
      // now we need to set g_stSettings.m_fPixelRatio so that
      // fOutputFrameRatio = 4:3.
      g_stSettings.m_fPixelRatio = (4.0f / 3.0f) / fSourceFrameRatio;
    }
  }
  else if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_STRETCH_14x9)
  { // stretch image to 14:9 ratio
    // now we need to set g_stSettings.m_fPixelRatio so that
    // fOutputFrameRatio = 14:9.
    g_stSettings.m_fPixelRatio = (14.0f / 9.0f) / fSourceFrameRatio;
    // calculate the desired output ratio
    float fOutputFrameRatio = fSourceFrameRatio * g_stSettings.m_fPixelRatio / g_settings.m_ResInfo[m_iResolution].fPixelRatio;
    // now calculate the correct zoom amount.  First zoom to full height.
    float fNewHeight = fScreenHeight;
    float fNewWidth = fNewHeight * fOutputFrameRatio;
    g_stSettings.m_fZoomAmount = fNewWidth / fScreenWidth;
    if (fNewWidth < fScreenWidth)
    { // zoom to full width
      fNewWidth = fScreenWidth;
      fNewHeight = fNewWidth / fOutputFrameRatio;
      g_stSettings.m_fZoomAmount = fNewHeight / fScreenHeight;
    }
  }
  else if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_STRETCH_16x9)
  { // stretch image to 16:9 ratio
    g_stSettings.m_fZoomAmount = 1.0;
    if (m_iResolution == PAL_4x3 || m_iResolution == PAL60_4x3 || m_iResolution == NTSC_4x3 || m_iResolution == HDTV_480p_4x3)
    { // now we need to set g_stSettings.m_fPixelRatio so that
      // fOutputFrameRatio = 16:9.
      g_stSettings.m_fPixelRatio = (16.0f / 9.0f) / fSourceFrameRatio;
    }
    else
    { // stretch to the limits of the 16:9 screen.
      // incorrect behaviour, but it's what the users want, so...
      g_stSettings.m_fPixelRatio = (fScreenWidth / fScreenHeight) * g_settings.m_ResInfo[m_iResolution].fPixelRatio / fSourceFrameRatio;
    }
  }
  else // if (g_stSettings.m_currentVideoSettings.m_ViewMode == VIEW_MODE_ORIGINAL)
  { // zoom image so that the height is the original size
    g_stSettings.m_fPixelRatio = 1.0;
    // get the size of the media file
    // calculate the desired output ratio
    float fOutputFrameRatio = fSourceFrameRatio * g_stSettings.m_fPixelRatio / g_settings.m_ResInfo[m_iResolution].fPixelRatio;
    // now calculate the correct zoom amount.  First zoom to full width.
    float fNewWidth = fScreenWidth;
    float fNewHeight = fNewWidth / fOutputFrameRatio;
    if (fNewHeight > fScreenHeight)
    { // zoom to full height
      fNewHeight = fScreenHeight;
      fNewWidth = fNewHeight * fOutputFrameRatio;
    }
    // now work out the zoom amount so that no zoom is done
    g_stSettings.m_fZoomAmount = (m_iSourceHeight - g_stSettings.m_currentVideoSettings.m_CropTop - g_stSettings.m_currentVideoSettings.m_CropBottom) / fNewHeight;
  }
}

void CLinuxRendererGL::AutoCrop(bool bCrop)
{
  if (!m_YUVTexture[0][FIELD_FULL][PLANE_Y]) return ;
  // FIXME: no cropping for now
  { // reset to defaults
    g_stSettings.m_currentVideoSettings.m_CropLeft = 0;
    g_stSettings.m_currentVideoSettings.m_CropRight = 0;
    g_stSettings.m_currentVideoSettings.m_CropTop = 0;
    g_stSettings.m_currentVideoSettings.m_CropBottom = 0;
  }
  SetViewMode(g_stSettings.m_currentVideoSettings.m_ViewMode);
}

void CLinuxRendererGL::RenderLowMem(DWORD flags)
{
  //CSingleLock lock(g_graphicsContext);
  int index = 0; //m_iYV12RenderBuffer;
  YV12Image &im = m_image[index];

  // set scissors if we are not in fullscreen video
  if ( !(g_graphicsContext.IsFullScreenVideo() || g_graphicsContext.IsCalibrating() ))
  {
    g_graphicsContext.ClipToViewWindow();
  }

  g_graphicsContext.BeginPaint();

  glDisable(GL_DEPTH_TEST);

  //See RGB renderer for comment on this
#define CHROMAOFFSET_HORIZ 0.25f

  // Y
  glEnable(m_textureTarget);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(m_textureTarget, m_YUVTexture[index][FIELD_FULL][0]);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

  if (m_renderMethod & RENDER_GLSL)
  {
    static GLfloat brightness = 0;
    static GLfloat contrast   = 0;

    // U
    glActiveTexture(GL_TEXTURE1);
    glEnable(m_textureTarget);
    glBindTexture(m_textureTarget, m_YUVTexture[index][FIELD_FULL][1]);
    
    // V
    glActiveTexture(GL_TEXTURE2);
    glEnable(m_textureTarget);
    glBindTexture(m_textureTarget, m_YUVTexture[index][FIELD_FULL][2]);
    
    glActiveTexture(GL_TEXTURE0);
    VerifyGLState();
    
    glUseProgram(m_shaderProgram);
    VerifyGLState();
    glUniform1i(m_yTex, 0);
    VerifyGLState();
    glUniform1i(m_uTex, 1);
    VerifyGLState();
    glUniform1i(m_vTex, 2);
    VerifyGLState();
    brightness =  ((GLfloat)g_stSettings.m_currentVideoSettings.m_Brightness - 50.0)/100.0;
    contrast =  ((GLfloat)g_stSettings.m_currentVideoSettings.m_Contrast)/50.0;
    glUniform1f(m_brightness, brightness);
    glUniform1f(m_contrast, contrast);
  }

  glBegin(GL_QUADS);
  
  if (m_textureTarget==GL_TEXTURE_2D)
  {
    // Use regular normalized texture coordinates

    glMultiTexCoord2f(GL_TEXTURE0, 0, 0);
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, 0, 0);
      glMultiTexCoord2f(GL_TEXTURE2, 0, 0);
    }
    glVertex4f((float)rd.left, (float)rd.top, 0, 1.0f );
    
    glMultiTexCoord2f(GL_TEXTURE0, im.texcoord_x, 0);
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, im.texcoord_x, 0);
      glMultiTexCoord2f(GL_TEXTURE2, im.texcoord_x, 0);
    }
    glVertex4f((float)rd.right, (float)rd.top, 0, 1.0f);
    
    glMultiTexCoord2f(GL_TEXTURE0, im.texcoord_x, im.texcoord_y);
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, im.texcoord_x, im.texcoord_y);
      glMultiTexCoord2f(GL_TEXTURE2, im.texcoord_x, im.texcoord_y);
    }
    glVertex4f((float)rd.right, (float)rd.bottom, 0, 1.0f);
    
    glMultiTexCoord2f(GL_TEXTURE0, 0, im.texcoord_y);
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, 0, im.texcoord_y);
      glMultiTexCoord2f(GL_TEXTURE2, 0, im.texcoord_y);
    }
    glVertex4f((float)rd.left, (float)rd.bottom, 0, 1.0f);

  }  else {

    // Use supported rectangle texture extension (texture coordinates
    // are not normalized)

    glMultiTexCoord2f(GL_TEXTURE0, (float)rs.left, (float)rs.top );
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, (float)rs.left / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.top / 2.0f);
      glMultiTexCoord2f(GL_TEXTURE2, (float)rs.left / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.top / 2.0f );
    }
    glVertex4f((float)rd.left, (float)rd.top, 0, 1.0f );
    
    glMultiTexCoord2f(GL_TEXTURE0, (float)rs.right, (float)rs.top );
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, (float)rs.right / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.top / 2.0f );
      glMultiTexCoord2f(GL_TEXTURE2, (float)rs.right / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.top / 2.0f );
    }
    glVertex4f((float)rd.right, (float)rd.top, 0, 1.0f);
    
    glMultiTexCoord2f(GL_TEXTURE0, (float)rs.right, (float)rs.bottom );
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, (float)rs.right / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.bottom / 2.0f );
      glMultiTexCoord2f(GL_TEXTURE2, (float)rs.right / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.bottom / 2.0f );
    }
    glVertex4f((float)rd.right, (float)rd.bottom, 0, 1.0f);
    
    glMultiTexCoord2f(GL_TEXTURE0, (float)rs.left, (float)rs.bottom );
    if (m_renderMethod & RENDER_GLSL)
    {
      glMultiTexCoord2f(GL_TEXTURE1, (float)rs.left / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.bottom / 2.0f );
      glMultiTexCoord2f(GL_TEXTURE2, (float)rs.left / 2.0f + CHROMAOFFSET_HORIZ, (float)rs.bottom / 2.0f );
    }
    glVertex4f((float)rd.left, (float)rd.bottom, 0, 1.0f);
  }
  glEnd();

  VerifyGLState();

  if (m_renderMethod & RENDER_GLSL)
  {
    glUseProgram(0);
    VerifyGLState();
    glActiveTexture(GL_TEXTURE1);
    glDisable(m_textureTarget);
    glActiveTexture(GL_TEXTURE2);
    glDisable(m_textureTarget);
  }

  glActiveTexture(GL_TEXTURE0);
  glDisable(m_textureTarget);
  VerifyGLState();
  g_graphicsContext.EndPaint();
}

void CLinuxRendererGL::CreateThumbnail(SDL_Surface * surface, unsigned int width, unsigned int height)
{
  //CSingleLock lock(g_graphicsContext);
}

//********************************************************************************************************
// YV12 Texture creation, deletion, copying + clearing
//********************************************************************************************************
void CLinuxRendererGL::DeleteYV12Texture(int index)
{
  YV12Image &im = m_image[index];
  YUVFIELDS &fields = m_YUVTexture[index];

  if( fields[FIELD_FULL][0] == 0 ) return;

  /* finish up all textures, and delete them */
  g_graphicsContext.BeginPaint(m_pBuffer);
  for(int f = 0;f<MAX_FIELDS;f++) 
  {
    for(int p = 0;p<MAX_PLANES;p++) 
    {
      if( fields[f][p] )
      {
	if (glIsTexture(fields[f][p]))
	  glDeleteTextures(1, &fields[f][p]);
	fields[f][p] = 0;
      }
    }
  }
  g_graphicsContext.EndPaint(m_pBuffer);

  for(int p = 0;p<MAX_PLANES;p++) 
  {
    if (im.plane[p]) 
    {
      delete[] im.plane[p];
      im.plane[p] = NULL;
    }
  }
  CLog::Log(LOGDEBUG, "Deleted YV12 texture %i", index);
}

void CLinuxRendererGL::ClearYV12Texture(int index)
{
  YV12Image &im = m_image[index];

  //memset(im.plane[0], 0,   im.stride[0] * im.height);
  //memset(im.plane[1], 128, im.stride[1] * im.height>>im.cshift_y );
  //memset(im.plane[2], 128, im.stride[2] * im.height>>im.cshift_y );

}

bool CLinuxRendererGL::CreateYV12Texture(int index)
{
  DeleteYV12Texture(index);

  /* since we also want the field textures, pitch must be texture aligned */
  DWORD dwTextureSize;
  unsigned stride, p;

  YV12Image &im = m_image[index];
  YUVFIELDS &fields = m_YUVTexture[index];

  im.height = m_iSourceHeight;
  im.width = m_iSourceWidth;

  im.stride[0] = m_iSourceWidth;
  im.stride[1] = m_iSourceWidth/2;
  im.stride[2] = m_iSourceWidth/2;
  im.plane[0] = new BYTE[m_iSourceWidth * m_iSourceHeight];
  im.plane[1] = new BYTE[(m_iSourceWidth/2) * (m_iSourceHeight/2)];
  im.plane[2] = new BYTE[(m_iSourceWidth/2) * (m_iSourceHeight/2)];

  im.cshift_x = 1;
  im.cshift_y = 1;
  im.texcoord_x = 1.0;
  im.texcoord_y = 1.0;

  g_graphicsContext.BeginPaint(m_pBuffer);

  glEnable(m_textureTarget);
  for(int f = 0;f<MAX_FIELDS;f++) 
  {
    for(p = 0;p<MAX_PLANES;p++) 
    {
      if (!glIsTexture(fields[f][p])) 
      {
	glGenTextures(1, &fields[f][p]);
	VerifyGLState();
      }
    }
  }

  // YUV 
  p = 0;
  glBindTexture(m_textureTarget, fields[0][0]);
  if (m_renderMethod & RENDER_SW)
  {
    // require Power Of Two textures?
    if (m_renderMethod & RENDER_POT)
    {
      static unsigned long np2x = 0, np2y = 0;
      np2x = NP2(im.width);
      np2y = NP2(im.height);
      CLog::Log(LOGNOTICE, "GL: Creating power of two texture of size %d x %d", np2x, np2y);
      glTexImage2D(m_textureTarget, 0, GL_RGBA, np2x, np2y, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
      im.texcoord_x = ((float)im.width / (float)np2x);
      im.texcoord_y = ((float)im.height / (float)np2y);
    }
    else
    {
      glTexImage2D(m_textureTarget, 0, GL_RGBA, im.width, im.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    }
  }
  else
    glTexImage2D(m_textureTarget, 0, GL_LUMINANCE, im.width, im.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP);
  VerifyGLState();

  if (m_renderMethod & RENDER_GLSL)
  {
    glBindTexture(m_textureTarget, fields[0][1]);
    glTexImage2D(m_textureTarget, 0, GL_LUMINANCE, im.width/2, im.height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP);
    VerifyGLState();
    
    glBindTexture(m_textureTarget, fields[0][2]);
    glTexImage2D(m_textureTarget, 0, GL_LUMINANCE, im.width/2, im.height/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL); 
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP);
    VerifyGLState();
  }

  g_graphicsContext.EndPaint(m_pBuffer);
  return true;
}

void CLinuxRendererGL::TextureCallback(DWORD dwContext)
{
  SetEvent((HANDLE)dwContext);
}
