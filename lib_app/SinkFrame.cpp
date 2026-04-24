// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_app/SinkFrame.h"
#include "lib_app/UnCompFrameWriter.h"
#include  "lib_common/DisplayInfoMeta.h"
#include <fstream>
#include <stdexcept>

class SinkUnCompFrame : public IFrameSink
{
public:
  SinkUnCompFrame(const std::shared_ptr<std::ostream>& recFile, AL_EFbStorageMode eStorageMode) :
    m_RecFile(recFile),
    m_Writer(m_RecFile, eStorageMode)
  {
    if(!m_RecFile || m_RecFile->fail())
      throw std::runtime_error("Invalid output file");
  }

  SinkUnCompFrame(const std::string& sRecFileName, AL_EFbStorageMode eStorageMode) :
    SinkUnCompFrame(std::shared_ptr<std::ostream>(new std::ofstream(sRecFileName.c_str(), std::ios::binary)), eStorageMode)
  {}

  void ProcessFrame(AL_TBuffer* pBuf) override
  {
    AL_TDisplayInfoMetaData* pMeta = reinterpret_cast<AL_TDisplayInfoMetaData*>(AL_Buffer_GetMetaData(pBuf, AL_META_TYPE_DISPLAY_INFO));

    AL_TCropInfo* pCrop = nullptr;

    if(pMeta)
      pCrop = &pMeta->tCrop;

    m_Writer.WriteFrame(pBuf, pCrop);
  }

private:
  std::shared_ptr<std::ostream> m_RecFile;

  UnCompFrameWriter m_Writer;
};

IFrameSink* createUnCompFrameSink(const std::shared_ptr<std::ostream>& recFile, AL_EFbStorageMode eStorageMode)
{
  return new SinkUnCompFrame(recFile, eStorageMode);
}

IFrameSink* createUnCompFrameSink(const std::string& sRecFile, AL_EFbStorageMode eStorageMode)
{
  return new SinkUnCompFrame(sRecFile, eStorageMode);
}
