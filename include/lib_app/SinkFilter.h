// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_app/Sink.h"
#include "lib_common/PicFormat.h"
#include "lib_common/DisplayInfoMeta.h"
#include <assert.h>

struct SinkFilter final : IFrameSink
{
  SinkFilter(AL_EOutputType eOutputID, std::unique_ptr<IFrameSink>& pSink) :
    m_eOutputID(eOutputID), m_pSink(std::move(pSink))
  {
    assert(m_pSink);
  };

  void ProcessFrame(AL_TBuffer* pBuf) override
  {
    AL_EOutputType eOutputID = AL_OUTPUT_MAIN;
    AL_TDisplayInfoMetaData* pMeta = reinterpret_cast<AL_TDisplayInfoMetaData*>(AL_Buffer_GetMetaData(pBuf, AL_META_TYPE_DISPLAY_INFO));

    if(pMeta)
      eOutputID = pMeta->eOutputID;

    if(eOutputID != m_eOutputID)
      return;

    m_pSink->ProcessFrame(pBuf);
  }

private:
  AL_EOutputType m_eOutputID;
  std::unique_ptr<IFrameSink> m_pSink;
};
