// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <d3d11.h>
#include <map>

#include "VideoCommon/PixelShaderGen.h"

namespace DX11
{
class PixelShaderCache
{
public:
  static void Init();
  static void Reload();
  static void Clear();
  static void Shutdown();
  static bool SetShader();  // TODO: Should be renamed to LoadShader
  static bool InsertByteCode(const PixelShaderUid& uid, const void* bytecode,
                             unsigned int bytecodelen);

  static ID3D11PixelShader* GetActiveShader() { return last_entry->shader; }
  static ID3D11Buffer*& GetConstantBuffer();

  static ID3D11PixelShader* GetColorMatrixProgram(bool multisampled);
  static ID3D11PixelShader* GetColorCopyProgram(bool multisampled);
  static ID3D11PixelShader* GetDepthMatrixProgram(bool multisampled);
  static ID3D11PixelShader* GetClearProgram();
  static ID3D11PixelShader* GetAnaglyphProgram();
  static ID3D11PixelShader* GetDepthResolveProgram();
  static ID3D11PixelShader* ReinterpRGBA6ToRGB8(bool multisampled);
  static ID3D11PixelShader* ReinterpRGB8ToRGBA6(bool multisampled);

  static void InvalidateMSAAShaders();

private:
  struct PSCacheEntry
  {
    ID3D11PixelShader* shader;

    PSCacheEntry() : shader(nullptr) {}
    void Destroy() { SAFE_RELEASE(shader); }
  };

  typedef std::map<PixelShaderUid, PSCacheEntry> PSCache;

  static void LoadShaderCache();

  static PSCache PixelShaders;
  static const PSCacheEntry* last_entry;
  static PixelShaderUid last_uid;
};

}  // namespace DX11
