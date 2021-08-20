/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "kvs/allocator.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* T31 headers */
#include <imp/imp_audio.h>

#include "aac_encoder/aac_encoder.h"

/* Thirdparty headers */
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/xlogging.h"

/* KVS headers */
#include "kvs/kvsapp.h"
#include "kvs/port.h"

#include "sample_config.h"
#include "t31_audio.h"

/* Thirdparty headers */
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/xlogging.h"

#define ERRNO_NONE 0
#define ERRNO_FAIL __LINE__

#if ENABLE_AUDIO_TRACK
typedef struct AudioConfiguration
{
    int devID;
    int chnID;
    IMPAudioIOAttr attr;
    IMPAudioIChnParam chnParam;
    int chnVol;
    int aigain;

    int channelNumber;

    // FDK AAC parameters
    int sampleRate;
    int channel;
    int bitRate;
} AudioConfiguration_t;

typedef struct T31Audio
{
    LOCK_HANDLE lock;

    pthread_t tid;
    bool isTerminating;
    bool isTerminated;

    // KVS
    KvsAppHandle kvsAppHandle;
    AudioTrackInfo_t *pAudioTrackInfo;

    AudioConfiguration_t xAudioConf;

    AacEncoderHandle xAacEncHandle;

    uint64_t uPcmTimestamp;
    uint8_t *pPcmBuf;
    size_t uPcmOffset;
    size_t uPcmBufSize;

    uint8_t *pFrameBuf;
    int xFrameBufSize;
} T31Audio_t;

static void sleepInMs(uint32_t ms)
{
    usleep(ms * 1000);
}

static int audioConfigurationInit(AudioConfiguration_t *pConf)
{
    int res = ERRNO_NONE;

    if (pConf == NULL)
    {
        LogError("Invalid parameter");
        res = ERRNO_FAIL;
    }
    else
    {
        memset(pConf, 0, sizeof(AudioConfiguration_t));

        pConf->devID = 1;

        pConf->chnID = 0;

        pConf->attr.samplerate = AUDIO_SAMPLE_RATE_16000;
        pConf->attr.bitwidth = AUDIO_BIT_WIDTH_16;
        pConf->attr.soundmode = AUDIO_SOUND_MODE_MONO;
        pConf->attr.frmNum = 40;
        pConf->attr.numPerFrm = 640; // it has to be multiple of (sample rate * 2 / 100)
        pConf->attr.chnCnt = 1;

        pConf->chnParam.usrFrmDepth = 40;

        pConf->chnVol = 60;

        pConf->aigain = 28;

        pConf->channelNumber = 1;

        pConf->sampleRate = 16000;
        pConf->channel = 1;
        pConf->bitRate = 128000;
    }

    return res;
}

int sendAudioFrame(T31Audio_t *pAudio, IMPAudioFrame *pFrame)
{
    int res = ERRNO_NONE;
    int xFrameLen = 0;
    uint8_t *pData = NULL;
    size_t uDataLen = 0;

    if (pAudio == NULL || pFrame == NULL)
    {
        LogError("Invalid parameter");
        res = ERRNO_FAIL;
    }
    else
    {
        int xFrameOffset = 0;
        size_t uCopySize = 0;
        // LogInfo("xFrameOffset:%d xPcmBufSize:%d len:%d", xFrameOffset, pAudio->xPcmBufSize, pFrame->len);
        if (pAudio->uPcmOffset == 0)
        {
            pAudio->uPcmTimestamp = getEpochTimestampInMs();
        }

        // FIXME: log audio here

        while (xFrameOffset < pFrame->len)
        {
            if (pAudio->uPcmBufSize - pAudio->uPcmOffset > pFrame->len - xFrameOffset)
            {
                /* Remaining data is not enough to fill the pcm buffer. */
                uCopySize = pFrame->len - xFrameOffset;
                memcpy(pAudio->pPcmBuf + pAudio->uPcmOffset, pFrame->virAddr + xFrameOffset, uCopySize);
                pAudio->uPcmOffset += uCopySize;
                xFrameOffset += uCopySize;
            }
            else
            {
                /* Remaining data is bigger than pcm buffer and able to do encode. */
                uCopySize = pAudio->uPcmBufSize - pAudio->uPcmOffset;
                memcpy(pAudio->pPcmBuf + pAudio->uPcmOffset, pFrame->virAddr + xFrameOffset, uCopySize);
                pAudio->uPcmOffset += uCopySize;
                xFrameOffset += uCopySize;
                pAudio->uPcmOffset = 0;

                xFrameLen = pAudio->xFrameBufSize;

                if (AacEncoder_encode(pAudio->xAacEncHandle, pAudio->pPcmBuf, pAudio->uPcmBufSize, pAudio->pFrameBuf, &xFrameLen) != 0)
                {
                    LogError("aac encode failed");
                }
                else
                {
                    uDataLen = xFrameLen;
                    if (uDataLen == 0 || (pData = (uint8_t *)KVS_MALLOC(uDataLen)) == NULL)
                    {
                        LogError("OOM: pData");
                    }
                    else
                    {
                        memcpy(pData, pAudio->pFrameBuf, uDataLen);
                        KvsApp_addFrame(pAudio->kvsAppHandle, pData, uDataLen, uDataLen, pAudio->uPcmTimestamp, TRACK_AUDIO);
                    }
                }

                uint64_t timediff = (pAudio->uPcmBufSize * 1000) / (pAudio->xAudioConf.sampleRate * 2);
                pAudio->uPcmTimestamp += timediff;
            }
        }
    }

    return res;
}

static void *audioThread(void *arg)
{
    int res = ERRNO_NONE;
    T31Audio_t *pAudio = (T31Audio_t *)arg;
    IMPAudioFrame frm = {0};

    if (pAudio == NULL)
    {
        LogError("Invalid parameter");
        res = ERRNO_FAIL;
    }
    else if (
        IMP_AI_SetPubAttr(pAudio->xAudioConf.devID, &(pAudio->xAudioConf.attr)) != 0 || IMP_AI_Enable(pAudio->xAudioConf.devID) != 0 ||
        IMP_AI_SetChnParam(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, &(pAudio->xAudioConf.chnParam)) != 0 ||
        IMP_AI_EnableChn(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID) != 0 ||
        IMP_AI_SetVol(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, pAudio->xAudioConf.chnVol) != 0 ||
        IMP_AI_SetGain(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, pAudio->xAudioConf.aigain) != 0)
    {
        LogError("Failed to setup audio");
        res = ERRNO_FAIL;
    }
    else
    {
        while (1)
        {
            if (IMP_AI_PollingFrame(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, 1000) != 0)
            {
                LogError("Audio Polling Frame Data error");
                continue;
            }
            else if (IMP_AI_GetFrame(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, &frm, BLOCK) != 0)
            {
                LogError("Audio Get Frame Data error");
                res = ERRNO_FAIL;
                break;
            }
            else
            {
                // Compress and send frame
                if (sendAudioFrame(pAudio, &frm) != 0)
                {
                    LogError("Failed to send Audio frame");
                }

                if (IMP_AI_ReleaseFrame(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID, &frm) != 0)
                {
                    LogError("Audio release frame data error");
                    break;
                }
            }

            if (pAudio->isTerminating)
            {
                break;
            }
        }

        if (IMP_AI_DisableChn(pAudio->xAudioConf.devID, pAudio->xAudioConf.chnID) != 0 || IMP_AI_Disable(pAudio->xAudioConf.devID) != 0)
        {
            LogError("Audio device disable error");
            res = ERRNO_FAIL;
        }
    }

    pAudio->isTerminated = true;

    return (void *)res;
}

static int initAudioTrackInfo(T31Audio_t *pAudio)
{
    int res = ERRNO_NONE;
    AudioTrackInfo_t *pAudioTrackInfo = NULL;
    uint8_t *pCodecPrivateData = NULL;
    size_t uCodecPrivateDataLen = 0;

    if (pAudio == NULL)
    {
        LogError("Invalid parameter");
        res = ERRNO_FAIL;
    }
    else
    {
        if (pAudio->pAudioTrackInfo == NULL)
        {
            if ((pAudioTrackInfo = (AudioTrackInfo_t *)KVS_MALLOC(sizeof(AudioTrackInfo_t))) == NULL)
            {
                LogError("OOM: pAudioTrackInfo");
                res = ERRNO_FAIL;
            }
            else
            {
                memset(pAudioTrackInfo, 0, sizeof(AudioTrackInfo_t));
                pAudioTrackInfo->pTrackName = AUDIO_TRACK_NAME;
                pAudioTrackInfo->pCodecName = AUDIO_CODEC_NAME;
                pAudioTrackInfo->uFrequency = pAudio->xAudioConf.sampleRate;
                pAudioTrackInfo->uChannelNumber = pAudio->xAudioConf.channelNumber;

                if (Mkv_generateAacCodecPrivateData(
                        AUDIO_MPEG_OBJECT_TYPE, pAudioTrackInfo->uFrequency, pAudioTrackInfo->uChannelNumber, &pCodecPrivateData, &uCodecPrivateDataLen) != 0)
                {
                    LogError("Failed to generate codec private data");
                    res = ERRNO_FAIL;
                }
                else
                {
                    pAudioTrackInfo->pCodecPrivate = pCodecPrivateData;
                    pAudioTrackInfo->uCodecPrivateLen = (uint32_t)uCodecPrivateDataLen;

                    pAudio->pAudioTrackInfo = pAudioTrackInfo;
                }
            }
        }
    }

    if (res != ERRNO_NONE)
    {
        if (pAudioTrackInfo != NULL)
        {
            KVS_FREE(pAudioTrackInfo);
        }
    }

    return res;
}

static int initAacEncoder(T31Audio_t *pAudio)
{
    int res = ERRNO_NONE;
    AacEncoderHandle xAacEncHandle = NULL;

    if (pAudio == NULL)
    {
        LogError("Invalid parameter");
        res = ERRNO_FAIL;
    }
    else if (
        (xAacEncHandle =
             AacEncoder_create(pAudio->xAudioConf.sampleRate, pAudio->xAudioConf.channel, pAudio->xAudioConf.bitRate, AAC_OBJECT_TYPE_AAC_LC, &(pAudio->uPcmBufSize))) == NULL)
    {
        LogError("Failed to init aac encoder");
        res = ERRNO_FAIL;
    }
    else
    {
        pAudio->uPcmTimestamp = 0;
        pAudio->uPcmOffset = 0;

        if ((pAudio->pPcmBuf = (uint8_t *)KVS_MALLOC(pAudio->uPcmBufSize)) == NULL)
        {
            LogError("OOM: pPcmBuf");
            res = ERRNO_FAIL;
        }
        else if ((pAudio->pFrameBuf = (uint8_t *)KVS_MALLOC(pAudio->xFrameBufSize)) == NULL)
        {
            LogError("OOM: pFrameBuf");
            res = ERRNO_FAIL;
        }
        else
        {
            pAudio->xAacEncHandle = xAacEncHandle;
        }
    }

    // TODO: add error handling

    return res;
}

T31AudioHandle T31Audio_create(KvsAppHandle kvsAppHandle)
{
    int res = ERRNO_NONE;
    T31Audio_t *pAudio = NULL;

    if ((pAudio = (T31Audio_t *)KVS_MALLOC(sizeof(T31Audio_t))) == NULL)
    {
        LogError("OOM: pAudio");
        res = ERRNO_FAIL;
    }
    else
    {
        memset(pAudio, 0, sizeof(T31Audio_t));

        pAudio->isTerminating = false;
        pAudio->isTerminated = false;

        pAudio->kvsAppHandle = kvsAppHandle;
        pAudio->xFrameBufSize = 8 * 1024;

        if ((pAudio->lock = Lock_Init()) == NULL)
        {
            LogError("Failed to initialize lock");
            res = ERRNO_FAIL;
        }
        else if (audioConfigurationInit(&(pAudio->xAudioConf)) != ERRNO_NONE)
        {
            LogError("failed to init audio configuration");
            res = ERRNO_FAIL;
        }
        else if (initAudioTrackInfo(pAudio) != ERRNO_NONE)
        {
            LogError("Failed to init audio track info");
            res = ERRNO_FAIL;
        }
        else if (initAacEncoder(pAudio) != ERRNO_NONE)
        {
            LogError("Failed to init aac encoder");
            res = ERRNO_FAIL;
        }
        else if (pthread_create(&(pAudio->tid), NULL, audioThread, pAudio) != 0)
        {
            LogError("Failed to create video thread");
            res = ERRNO_FAIL;
        }
    }

    if (res != ERRNO_NONE)
    {
        T31Audio_terminate(pAudio);
        pAudio = NULL;
    }

    return pAudio;
}

void T31Audio_terminate(T31AudioHandle handle)
{
    T31Audio_t *pAudio = (T31Audio_t *)handle;

    if (pAudio != NULL)
    {
        pAudio->isTerminating = true;
        while (!pAudio->isTerminated)
        {
            sleepInMs(10);
        }

        pthread_join(pAudio->tid, NULL);

        KVS_FREE(pAudio);
    }
}

AudioTrackInfo_t *T31Audio_getAudioTrackInfoClone(T31AudioHandle handle)
{
    int res = ERRNO_NONE;
    AudioTrackInfo_t *pAudioTrackInfo = NULL;
    T31Audio_t *pAudio = (T31Audio_t *)handle;

    if (pAudio == NULL)
    {
        res = ERRNO_FAIL;
    }
    else if (Lock(pAudio->lock) != LOCK_OK)
    {
        LogError("Failed to lock");
        res = ERRNO_FAIL;
    }
    else
    {
        if (pAudio->pAudioTrackInfo != NULL)
        {
            if ((pAudioTrackInfo = (AudioTrackInfo_t *)KVS_MALLOC(sizeof(AudioTrackInfo_t))) == NULL)
            {
                LogError("OOM: pAudioTrackInfo");
                res = ERRNO_FAIL;
            }
            else
            {
                memcpy(pAudioTrackInfo, pAudio->pAudioTrackInfo, sizeof(AudioTrackInfo_t));
            }
        }

        Unlock(pAudio->lock);
    }

    return pAudioTrackInfo;
}

#endif /* ENABLE_AUDIO_TRACK */