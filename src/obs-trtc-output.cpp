#include <obs-module.h>
#include <ITRTCCloud.h>
#include <mutex>

// TRTC进房信息
#define SDK_APP_ID 0
#define USER_ID ""
#define USER_SIG ""
#define ROOM_ID 0

using namespace liteav;

class TRTCImpl: public ITRTCCloudCallback {
private:
	obs_output_t *output;
  int width;
  int height;
  char *videoData;
  uint32_t videoSize;
  uint32_t sampleRate;
  uint32_t channel;
  uint32_t sentBytes;
  char *audioData;
  uint32_t audioSize;
  bool sendReady;
  std::mutex statusMutex;
  std::condition_variable startCV;

  void _setLastError(const char * format, ...) {
    char strBuf[1024] = {0};
    va_list pArgs;
    va_start(pArgs, format);
    vsnprintf(strBuf, 1024 - 1, format, pArgs);
    va_end(pArgs);
    obs_output_set_last_error(this->output, strBuf);
    blog(LOG_ERROR, "setLastError | %s", strBuf);
  }
public:
	TRTCImpl(obs_output_t *output) {
		this->output = output;
    // 视频参数初始化
    this->width = (int)obs_output_get_width(output);
    this->height = (int)obs_output_get_height(output);
    this->videoSize = this->width * this->height * 3 / 2;
    this->videoData = new char[this->videoSize];
    video_scale_info videoInfo = {};
    videoInfo.format = VIDEO_FORMAT_I420;
    videoInfo.width = this->width;
    videoInfo.height = this->height;
    videoInfo.range = VIDEO_RANGE_DEFAULT;
    videoInfo.colorspace = VIDEO_CS_DEFAULT;
    obs_output_set_video_conversion(output, &videoInfo);
    // 音频参数初始化
    this->sampleRate = 48000;
    this->channel = 1;
    this->audioSize = this->sampleRate * 2 * this->channel;
    this->audioData = new char[this->audioSize];
    audio_convert_info audioInfo = {};
    audioInfo.samples_per_sec = this->sampleRate;
    audioInfo.format = AUDIO_FORMAT_16BIT;
    audioInfo.speakers = SPEAKERS_MONO;
    obs_output_set_audio_conversion(output, &audioInfo);

    this->sentBytes = 0;
    this->sendReady = false;
	}

  virtual ~TRTCImpl() {
    delete[] this->audioData;
    delete[] this->videoData;
  }

	void onCreate() {
		// 初始化TRTC
		ITRTCCloud *trtcCloud = getTRTCShareInstance();
		trtcCloud->addCallback(this);
	}

  void onDestroy() {
    // 销毁TRTC
    ITRTCCloud *trtcCloud = getTRTCShareInstance();
    trtcCloud->removeCallback(this);
    destroyTRTCShareInstance();
  }

	bool onStart() {
    if (this->sendReady) return true;
    ITRTCCloud *trtcCloud = getTRTCShareInstance();
    // TRTC进房
    TRTCParams params;
    params.sdkAppId = SDK_APP_ID;
    params.userId = USER_ID;
    params.userSig = USER_SIG;
    params.roomId = ROOM_ID;
    trtcCloud->enterRoom(params,  TRTCAppSceneLIVE);
    // 设置视频编码参数
    TRTCVideoEncParam encParam;
    encParam.videoResolution = TRTCVideoResolution_1920_1080;
    encParam.resMode = TRTCVideoResolutionModeLandscape;
    encParam.videoFps = 20;
    encParam.videoBitrate = 3000;
    encParam.minVideoBitrate = 300;
    encParam.enableAdjustRes = false;
    trtcCloud->setVideoEncoderParam(encParam);
    // 启用视频/音频自定义采集
    trtcCloud->enableCustomVideoCapture(TRTCVideoStreamTypeBig, true);
    trtcCloud->enableCustomAudioCapture(true);
    // 等待进房结果
    std::unique_lock<std::mutex> lck(this->statusMutex);
    this->startCV.wait(lck);
		return this->sendReady;
	}

  void onStop() {
    if (!this->sendReady) return;
    ITRTCCloud *trtcCloud = getTRTCShareInstance();
    // 禁用视频/音频自定义采集
    trtcCloud->enableCustomAudioCapture(false);
    trtcCloud->enableCustomVideoCapture(TRTCVideoStreamTypeBig, false);
    // TRTC退房
    trtcCloud->exitRoom();
  }

  void receiveVideo(struct video_data *frame) {
    if (!this->sendReady) return;
    // 将帧数据平铺
    uint32_t offset = 0;
    for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
      const uint8_t *data = frame->data[plane];
      uint32_t lineSize = frame->linesize[plane];
      if (data && offset + lineSize <= this->videoSize) {
        memcpy(this->videoData + offset, data, lineSize);
        offset += lineSize;
      }
    }
    // 塞入TRTC
    ITRTCCloud *trtcCloud = getTRTCShareInstance();
    TRTCVideoFrame trtcFrame;
    trtcFrame.timestamp = frame->timestamp;
    trtcFrame.videoFormat = TRTCVideoPixelFormat_I420;
    trtcFrame.bufferType = TRTCVideoBufferType_Buffer;
    trtcFrame.length = this->videoSize;
    trtcFrame.data = this->videoData;
    trtcFrame.width = this->width;
    trtcFrame.height = this->height;
    trtcCloud->sendCustomVideoData(TRTCVideoStreamTypeBig, &trtcFrame);
  }

  void receiveAudio(struct audio_data *frame) {
    if (!this->sendReady) return;
    // 将帧数据平铺
    uint32_t offset = 0;
    for (int plane = 0; plane < MAX_AV_PLANES; plane++) {
      const uint8_t *data = frame->data[plane];
      uint32_t lineSize = frame->frames * 2; // 16位深，每个采样点2byte
      if (data && offset + lineSize <= this->audioSize) {
        memcpy(this->audioData + offset, data, lineSize);
        offset += lineSize;
      }
    }
    // 塞入TRTC
    ITRTCCloud *trtcCloud = getTRTCShareInstance();
    TRTCAudioFrame trtcFrame;
    trtcFrame.audioFormat = liteav::TRTCAudioFrameFormatPCM;
    trtcFrame.length = this->audioSize;
    trtcFrame.data = this->audioData;
    trtcFrame.sampleRate = this->sampleRate;
    trtcFrame.channel = this->channel;
    trtcFrame.timestamp = frame->timestamp;
    trtcCloud->sendCustomAudioData(&trtcFrame);

  }

  uint32_t getSendBytes() {
    return this->sentBytes;
  }

	virtual void onError(TXLiteAVError errCode, const char *errMsg, void *extraInfo) {
		blog(LOG_ERROR, "TRTC onError: %d, %s", errCode, errMsg);
    if (this->sendReady) {
      // 出错后停止输出
      this->_setLastError("TRTC onError: %d, %s", errCode, errMsg);
      obs_output_signal_stop(this->output, OBS_OUTPUT_ERROR);
    }
	}

	virtual void onWarning(TXLiteAVWarning warningCode, const char *warningMsg, void *extraInfo) {
		blog(LOG_WARNING, "TRTC onWarning: %d, %s", warningCode, warningMsg);
	}


	virtual void onEnterRoom(int result) {
    std::unique_lock<std::mutex> lck(this->statusMutex);
    if (result < 0) {
      this->_setLastError("enter room failed: %d", result);
    } else {
      this->sendReady = true;
    }
    this->startCV.notify_all();
	}

	virtual void onExitRoom(int reason) {
    std::unique_lock<std::mutex> lck(this->statusMutex);
    this->sendReady = false;
	}

  virtual void onStatistics(const TRTCStatistics& statistics) {
    this->sentBytes = statistics.sentBytes;
  }
};

static const char *trtc_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("TRTCOutput");
}

static void *trtc_output_create(obs_data_t *settings, obs_output_t *output)
{
  blog(LOG_DEBUG, "trtc_output_create");
	TRTCImpl *impl = new TRTCImpl(output);
	impl->onCreate();
	UNUSED_PARAMETER(settings);
	return impl;
}

static void trtc_output_destroy(void *data)
{
  blog(LOG_DEBUG, "trtc_output_destroy");
	TRTCImpl *impl = (TRTCImpl *)data;
	if (impl) {
		impl->onDestroy();
		delete impl;
	}
}

static bool trtc_output_start(void *data)
{
  blog(LOG_DEBUG, "trtc_output_start");
	TRTCImpl *impl = (TRTCImpl *)data;
	return impl->onStart();
}

static void trtc_output_stop(void *data, uint64_t ts)
{
  blog(LOG_DEBUG, "trtc_output_stop");
	TRTCImpl *impl = (TRTCImpl *)data;
  impl->onStop();
}

static void receive_video(void *data, struct video_data *frame)
{
  blog(LOG_DEBUG, "receive_video");
	TRTCImpl *impl = (TRTCImpl *)data;
  impl->receiveVideo(frame);
}

static void receive_audio(void *data, struct audio_data *frame)
{
  blog(LOG_DEBUG, "receive_audio");
	TRTCImpl *impl = (TRTCImpl *)data;
  impl->receiveAudio(frame);
}

static uint64_t trtc_output_total_bytes(void *data)
{
	TRTCImpl *impl = (TRTCImpl *)data;
	return impl->getSendBytes();
}

void RegisterTRTCOutput()
{
	obs_output_info info = {};
	info.id = "trtc_output";
	info.flags = OBS_OUTPUT_AUDIO | OBS_OUTPUT_VIDEO,
	info.get_name = trtc_output_getname,
	info.create = trtc_output_create,
	info.destroy = trtc_output_destroy,
	info.start = trtc_output_start,
	info.stop = trtc_output_stop,
	info.raw_video = receive_video,
	info.get_total_bytes = trtc_output_total_bytes,
	info.raw_audio = receive_audio,
	obs_register_output(&info);
}
