/**
 * This file is part of Special K.
 *
 * Special K is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Special K is distributed in the hope that it will be useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Special K.
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/

#include <SpecialK/stdafx.h>

#ifdef  __SK_SUBSYSTEM__
#undef  __SK_SUBSYSTEM__
#endif
#define __SK_SUBSYSTEM__ L" DStorage "

#include <SpecialK/render/dstorage/dstorage.h>

DStorageCreateCompressionCodec_pfn DStorageCreateCompressionCodec_Original = nullptr;
DStorageGetFactory_pfn             DStorageGetFactory_Original             = nullptr;
DStorageSetConfiguration_pfn       DStorageSetConfiguration_Original       = nullptr;
DStorageSetConfiguration1_pfn      DStorageSetConfiguration1_Original      = nullptr;

bool SK_DStorage_UsingDLL      = false;
bool SK_DStorage_UsingGDeflate = false;

DSTORAGE_CONFIGURATION SK_DStorage_LastConfig = { };

void
SK_DStorage_ApplyConfigOverrides (DSTORAGE_CONFIGURATION *pConfig)
{
  if (config.render.dstorage.disable_bypass_io)
    pConfig->DisableBypassIO = true;

  if (config.render.dstorage.disable_gpu_decomp)
    pConfig->DisableGpuDecompression = true;

  if (config.render.dstorage.disable_telemetry)
    pConfig->DisableTelemetry = true;
}

void
SK_DStorage_ApplyConfigOverrides1 (DSTORAGE_CONFIGURATION1 *pConfig1)
{
  SK_DStorage_ApplyConfigOverrides (pConfig1);

  if (config.render.dstorage.force_file_buffering)
    pConfig1->ForceFileBuffering = true;
}

HRESULT
WINAPI
DStorageSetConfiguration_Detour (DSTORAGE_CONFIGURATION const *configuration)
{
  SK_LOG_FIRST_CALL

  if (! configuration)
    return E_POINTER;

  auto                               cfg = *configuration;
  SK_DStorage_ApplyConfigOverrides (&cfg);

  HRESULT hr =
    DStorageSetConfiguration_Original (&cfg);

  if (SUCCEEDED (hr))
  {
    SK_DStorage_LastConfig = cfg;
  }

  return hr;
}

HRESULT
WINAPI
DStorageSetConfiguration1_Detour (DSTORAGE_CONFIGURATION1 const *configuration1)
{
  SK_LOG_FIRST_CALL

  if (! configuration1)
    return E_POINTER;

  auto                                cfg1 = *configuration1;
  SK_DStorage_ApplyConfigOverrides1 (&cfg1);

  static DSTORAGE_CONFIGURATION1                   defaults = { };
  SK_RunOnce (DStorageSetConfiguration1_Original (&defaults));

  HRESULT hr =
    DStorageSetConfiguration1_Original (&cfg1);

  if (SUCCEEDED (hr))
  {
    SK_DStorage_LastConfig = cfg1;

    if (SK_DStorage_LastConfig.NumSubmitThreads == 0)
        SK_DStorage_LastConfig.NumSubmitThreads = defaults.NumSubmitThreads;

    if (SK_DStorage_LastConfig.NumBuiltInCpuDecompressionThreads == -1)
        SK_DStorage_LastConfig.NumBuiltInCpuDecompressionThreads = defaults.NumBuiltInCpuDecompressionThreads;
  }

  return hr;
}

HRESULT
WINAPI
DStorageCreateCompressionCodec_Detour (
  DSTORAGE_COMPRESSION_FORMAT format,
  UINT32                      numThreads,
  REFIID                      riid,
  void                      **ppv )
{
  SK_LOG_FIRST_CALL

  HRESULT hr =
    DStorageCreateCompressionCodec_Original (format, numThreads, riid, ppv);

  if (SUCCEEDED (hr))
  {
    if (format == DSTORAGE_COMPRESSION_FORMAT_GDEFLATE)
      SK_DStorage_UsingGDeflate = true;

    SK_RunOnce (
      SK_LOGi0 (L"Game is using GDeflate!")
    );
  }

  return hr;
}

const wchar_t*
SK_DStorage_PriorityToStr (DSTORAGE_PRIORITY prio)
{
  switch (prio)
  {
    case DSTORAGE_PRIORITY_LOW:      return L"Low";
    case DSTORAGE_PRIORITY_NORMAL:   return L"Normal";
    case DSTORAGE_PRIORITY_HIGH:     return L"High";
    case DSTORAGE_PRIORITY_REALTIME: return L"Realtime";
    default:
      SK_LOGi0 (L"Unknown DStorage Priority: %d", prio);
      return    L"Unknown";
  }
}

DSTORAGE_PRIORITY
SK_DStorage_PriorityFromStr (const wchar_t *wszPrio)
{
  if (! _wcsicmp (wszPrio, L"Low"))
    return DSTORAGE_PRIORITY_LOW;

  if (! _wcsicmp (wszPrio, L"Normal"))
    return DSTORAGE_PRIORITY_NORMAL;

  if (! _wcsicmp (wszPrio, L"High"))
    return DSTORAGE_PRIORITY_HIGH;

  if (! _wcsicmp (wszPrio, L"Realtime"))
    return DSTORAGE_PRIORITY_REALTIME;

  return DSTORAGE_PRIORITY_NORMAL;
}

class SK_IWrapDStorageFactory : IDStorageFactory
{
public:
  SK_IWrapDStorageFactory (IDStorageFactory *pFactory)
  {
    pReal = pFactory;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject) override
  {
    return
      pReal->QueryInterface (riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef (void) override
  {
    return
      pReal->AddRef ();
  }
  
  ULONG STDMETHODCALLTYPE Release (void) override
  {
    return
      pReal->Release ();
  }

  HRESULT STDMETHODCALLTYPE CreateQueue (const DSTORAGE_QUEUE_DESC *desc, REFIID riid, _COM_Outptr_ void **ppv) override
  {
    DSTORAGE_QUEUE_DESC override_desc = *desc;

    SK_LOGi0 (
      L"SK_IWrapDStorageFactory::CreateQueue (Priority=%ws, Name=%hs)",
        SK_DStorage_PriorityToStr (desc->Priority),
                                   desc->Name != nullptr ?
                                   desc->Name : "Unnamed" );

    if (SK_GetCurrentGameID () == SK_GAME_ID::RatchetAndClank_RiftApart)
    {
      if (desc->Name != nullptr)
      {
        auto &dstorage =
          SK_GetDLLConfig ()->get_section (L"RatchetAndClank.DStorage");

        if (0 == _stricmp (desc->Name, "Bulk"))
        {
          if (! dstorage.contains_key  (L"BulkPriority"))
                dstorage.add_key_value (L"BulkPriority", L"Normal");

          override_desc.Priority =
            SK_DStorage_PriorityFromStr (dstorage.get_value (L"BulkPriority").c_str ());
        }

        else if (0 == _stricmp (desc->Name, "Loose reads"))
        {
          if (! dstorage.contains_key  (L"LooseReadPriority"))
                dstorage.add_key_value (L"LooseReadPriority", L"High");

          override_desc.Priority =
            SK_DStorage_PriorityFromStr (dstorage.get_value (L"LooseReadPriority").c_str ());
        }

        else if (0 == _stricmp (desc->Name, "Texture"))
        {
          if (! dstorage.contains_key  (L"TexturePriority"))
                dstorage.add_key_value (L"TexturePriority", L"Low");

          override_desc.Priority =
            SK_DStorage_PriorityFromStr (dstorage.get_value (L"TexturePriority").c_str ());
        }

        else if (0 == _stricmp (desc->Name, "NxStorage Index"))
        {
          if (! dstorage.contains_key  (L"NxStorageIndexPriority"))
                dstorage.add_key_value (L"NxStorageIndexPriority", L"Realtime");

          override_desc.Priority =
            SK_DStorage_PriorityFromStr (dstorage.get_value (L"NxStorageIndexPriority").c_str ());
        }
      }
    }

    return
      pReal->CreateQueue (&override_desc, riid, ppv);
  }

  HRESULT STDMETHODCALLTYPE OpenFile (_In_z_ const WCHAR *path, REFIID riid, _COM_Outptr_ void **ppv) override
  {
    return
      pReal->OpenFile (path, riid, ppv);
  }

  HRESULT STDMETHODCALLTYPE CreateStatusArray (UINT32 capacity, _In_opt_ PCSTR name, REFIID riid, _COM_Outptr_ void **ppv) override
  {
    return
      pReal->CreateStatusArray (capacity, name, riid, ppv);
  }

  void STDMETHODCALLTYPE SetDebugFlags (UINT32 flags) override
  {
    return
      pReal->SetDebugFlags (flags);
  }
  
  HRESULT STDMETHODCALLTYPE SetStagingBufferSize (UINT32 size) override
  {
    return
      pReal->SetStagingBufferSize (size);
  }

private:
  IDStorageFactory *pReal = nullptr;
};

HRESULT
WINAPI
DStorageGetFactory_Detour ( REFIID riid,
                            void **ppv )
{
  SK_LOG_FIRST_CALL

  if (riid == __uuidof (IDStorageFactory))
  {
    void *pv2 = nullptr;

    HRESULT hr =
      DStorageGetFactory_Original (riid, &pv2);

    if (SUCCEEDED (hr) && ppv != nullptr)
    {
      static SK_IWrapDStorageFactory
              wrapped_factory ((IDStorageFactory *)pv2);
      *ppv = &wrapped_factory;

      return hr;
    }
  }

  wchar_t                wszGUID [41] = { };
  StringFromGUID2 (riid, wszGUID, 40);

  SK_LOGi0 (L"Unsupported Factory IID: %ws", wszGUID);

  return
    DStorageGetFactory_Original (riid, ppv);
}

void SK_DStorage_Init (void)
{
  SK_DStorage_UsingDLL =
    GetModuleHandleW (L"dstorage.dll") != nullptr;

  if (SK_DStorage_IsLoaded ())
  {
    SK_RunOnce (
    {
      SK_CreateDLLHook2 (L"dstorage.dll", "DStorageCreateCompressionCodec",
                                           DStorageCreateCompressionCodec_Detour,
                  static_cast_p2p <void> (&DStorageCreateCompressionCodec_Original) );

      SK_CreateDLLHook2 (L"dstorage.dll", "DStorageGetFactory",
                                           DStorageGetFactory_Detour,
                  static_cast_p2p <void> (&DStorageGetFactory_Original) );

      SK_CreateDLLHook2 (L"dstorage.dll", "DStorageSetConfiguration",
                                           DStorageSetConfiguration_Detour,
                  static_cast_p2p <void> (&DStorageSetConfiguration_Original) );

      SK_CreateDLLHook2 (L"dstorage.dll", "DStorageSetConfiguration1",
                                           DStorageSetConfiguration1_Detour,
                  static_cast_p2p <void> (&DStorageSetConfiguration1_Original) );

      SK_ApplyQueuedHooks ();
    });
  }
}

bool SK_DStorage_IsLoaded (void)
{
  return
    SK_DStorage_UsingDLL;
}

bool SK_DStorage_IsUsingGDeflate (void)
{
  return
    SK_DStorage_UsingGDeflate;
}

UINT32 SK_DStorage_GetNumSubmitThreads (void)
{
  return
    SK_DStorage_LastConfig.NumSubmitThreads;
}

INT32 SK_DStorage_GetNumBuiltInCpuDecompressionThreads (void)
{
  return
    SK_DStorage_LastConfig.NumBuiltInCpuDecompressionThreads;
}