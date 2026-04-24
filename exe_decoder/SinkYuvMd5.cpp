// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include <fstream>
#include "SinkYuvMd5.h"
#include "lib_app/MD5.h"
#include "lib_app/YuvIO.h"

extern "C"
{
#include "lib_common/PixMapBuffer.h"
}

using namespace std;

class YuvMd5Calculator : public IFrameSink, Md5Calculator
{
public:
  YuvMd5Calculator(std::string& path) :
    Md5Calculator(path)
  {}

  ~YuvMd5Calculator(void)
  {
    if(IsFileOpen())
    {
      Md5Output();
    }
  }

  void ProcessFrame(AL_TBuffer* pBuf) override
  {
    ComputeMd5SumFrame(pBuf, GetCMD5());
  }
};

IFrameSink* createYuvMd5Calculator(std::string path)
{
  return new YuvMd5Calculator(path);
}
