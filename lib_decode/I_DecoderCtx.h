// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_decode_hls
   @{
   \file
 *****************************************************************************/

#pragma once

#include "lib_common_dec/StartCodeParam.h"
#include "lib_common_dec/ChannelState.h"
#include "lib_common_dec/I_Feeder.h"
#include "lib_common_dec/RbspParser.h"

#include "lib_parsing/I_PictMngr.h"
#include "lib_parsing/Concealment.h"
#include "lib_parsing/Aup.h"
#include "lib_common/BufferSeiMeta.h"
#include "lib_decode/I_DecScheduler.h"
#include "lib_decode/DecoderFrameCtx.h"
#include "lib_decode/lib_decode.h"

typedef enum
{
  SEND_NAL_UNTIL_LAST_VCL,
  SEND_REORDERED_SUFFIX,
  SEND_LAST_VCL,
  SEND_REMAINING_NAL,
}AL_DecodeNalStep;

typedef struct
{
  AL_ENut dps;
  AL_ENut vps;
  AL_ENut sps;
  AL_ENut pps;
  AL_ENut fd;
  AL_ENut apsPrefix;
  AL_ENut apsSuffix;
  AL_ENut ph;
  AL_ENut seiPrefix;
  AL_ENut seiSuffix;
  AL_ENut eos;
  AL_ENut eob;
}AL_NonVclNuts;

typedef struct AL_TDecCtx AL_TDecCtx;

typedef struct
{
  void (* parseDps)(AL_TAup*, AL_TRbspParser*);
  AL_PARSE_RESULT (* parseVps)(AL_TAup*, AL_TRbspParser*);
  AL_PARSE_RESULT (* parseSps)(AL_TAup*, AL_TRbspParser*, AL_TDecCtx*);
  AL_PARSE_RESULT (* parsePps)(AL_TAup*, AL_TRbspParser*, AL_TDecCtx*);
  AL_PARSE_RESULT (* parseAps)(AL_TAup*, AL_TRbspParser*, AL_TDecCtx*);
  AL_PARSE_RESULT (* parsePh)(AL_TAup*, AL_TRbspParser*, AL_TDecCtx*);
  bool (* parseSei)(AL_TAup*, AL_TRbspParser*, bool, AL_CB_ParsedSei*, AL_TSeiMetaData* pMeta);
  // return false when there is nothing to process
  bool (* decodeSliceData)(AL_TAup*, AL_TDecCtx*, AL_ENut, bool, int*);
  bool (* isSliceData)(AL_ENut nut);
  void (* finishPendingRequest)(AL_TDecCtx*);
  AL_NonVclNuts (* getNonVclNuts)(void);
  bool (* isNutError)(AL_ENut);
  // use in Split Input
  // return true if the Nal should be reordered to be send before sending the last Vcl Nal
  bool (* canNalBeReordered)(AL_ENut);
}AL_NalParser;

/****************************************************************************/
typedef struct
{
  TBuffer StreamBuf;
  AL_TCircMetaData StreamMeta;
  TBuffer ScdBufOut;
}AL_TDecScdBuffers;

/*************************************************************************//*!
   \brief Decoder Context structure
*****************************************************************************/
struct AL_TDecCtx
{
  AL_TFeeder* Feeder;
  AL_EDecInputMode eInputMode;

  TBuffer BufNoAE;            // Deanti-Emulated buffer used for high level syntax parsing
  TCircBuffer Stream;             // Input stream buffer
  TCircBuffer NalStream;
  AL_TBuffer* pInputBuffer;     // keep a reference to input buffer and its meta data

  // decoder IP handle
  AL_IDecScheduler* pScheduler;
  AL_HANDLE hChannel;
  AL_HANDLE hStartCodeChannel;
  AL_TAllocator* pAllocator;
  AL_EVENT hDecOutSettingsConfiguredEvt;
  AL_EChanState eChanState;

  AL_TDecCallBacks tDecCB;

  AL_SEMAPHORE Sem;
  AL_EVENT ScDetectionComplete;

  AL_MUTEX DecMutex;

  // Start code members
  TBuffer BufSCD;             // Holds the Start Code Detector Table results
  TBuffer SCTable;            //
  uint16_t uNumSC;             //
  AL_TScStatus ScdStatus;

  AL_TDecPicBufferAddrs BufAddrs;
  // decoder pool buffer
  TBuffer PoolSclLst[AL_DEC_SW_MAX_STACK_SIZE];      // Scaling List pool buffer
  TBuffer PoolCompData[AL_DEC_SW_MAX_STACK_SIZE];    // compressed MVDs + header + residuals pool buffer
  TBuffer PoolCompMap[AL_DEC_SW_MAX_STACK_SIZE];     // Compression map : LCU size + LCU offset pool buffer
  TBuffer PoolWP[AL_DEC_SW_MAX_STACK_SIZE];          // Weighted Pred Tables pool buffer
  TBuffer PoolListRefAddr[AL_DEC_SW_MAX_STACK_SIZE]; // Reference addresses for the board pool buffer
  TBuffer PoolVirtRefAddr[AL_DEC_SW_MAX_STACK_SIZE]; // Reference addresses for the reference pool buffer

  TBufferListRef ListRef;            // Picture Reference List buffer

  // slice toggle management
  TBuffer PoolSP[AL_DEC_SW_MAX_STACK_SIZE]; // Slice parameters
  AL_TDecPicParam PoolPP[AL_DEC_SW_MAX_STACK_SIZE]; // Picture parameters
  AL_TDecPicBuffers PoolPB[AL_DEC_SW_MAX_STACK_SIZE]; // Picture Buffers
  uint8_t uCurID; // ID of the last independent slice

  AL_TDecChanParam* pChanParam;
  AL_EDpbMode eDpbMode;
  int iStackSize;
  bool bForceFrameRate;
  bool bIntraOnlyProfile;
  bool bStillPictureProfile;

  // Trace stuff
  char sTracePrefix[8];
  int iTraceFirstFrame;
  int iTraceLastFrame;
  int iTraceCounter;
  bool bShouldPrintFrameDelimiter;

  // stream context status
  bool bFirstIsValid;
  bool bIsFirstPicture;
  int iStreamOffset[AL_DEC_SW_MAX_STACK_SIZE];
  int iCurOffset;
  int iCurNalStreamOffset;
  int32_t iCurPocLsb;
  union
  {
    uint8_t uNoRaslOutputFlag;
    uint8_t uNoIncorrectPicOutputFlag;
  };
  uint8_t uFrameIDRefList[AL_DEC_SW_MAX_STACK_SIZE][AL_MAX_NUM_REF];
  uint8_t uMvIDRefList[AL_DEC_SW_MAX_STACK_SIZE][AL_MAX_NUM_REF];
  uint8_t uNumRef[AL_DEC_SW_MAX_STACK_SIZE];

  // CurrentFrame context
  AL_TDecFrameCtx tCurrentFrameCtx;

  // error concealment context
  AL_TConceal tConceal;
  uint16_t uConcealMaxFps; // Clipping of framerate for stream having corrupted or invalid SPS header

  // Decoder toggle buffer
  TBufferPOC POC;          // Colocated POC buffer
  TBufferMV MV;            // Motion Vector buffer
  AL_TRecBuffers pRecs;    // Reconstructed buffers

  // decoder counters
  uint16_t uToggle;
  int iNumFrmBlk1;
  int iNumFrmBlk2;

  // reference frames and dpb manager
  AL_TPictMngrCtx PictMngr;
  AL_TAup aup;
  union
  {
    AL_TAvcSliceHdr AvcSliceHdr[2]; // Slice headers
    AL_THevcSliceHdr HevcSliceHdr[2]; // Slice headers
  };
  AL_ERR error;
  bool bIsFirstSPSChecked;
  bool bAreBuffersAllocated;
  bool bUseIFramesAsSyncPoint;
  AL_TStreamSettings tCurrentStreamSettings;
  AL_TStreamSettings tInitialStreamSettings;
  AL_TBuffer* eosBuffer;

  int iNumSlicesRemaining;

  AL_TPosition tOutputPosition;

  TMemDesc tMDChanParam;
  AL_NalParser parser;
};

/****************************************************************************/

/*@}*/
