#include <atomic>
#include <thread>
#include <sys/file.h>
#include "Config.hpp"
#include "worker.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#define MODULE "WORKER"

#if defined(PLATFORM_T31)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

unsigned long long tDiffInMs(struct timeval *startTime)
{
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);

    long seconds = currentTime.tv_sec - startTime->tv_sec;
    long microseconds = currentTime.tv_usec - startTime->tv_usec;

    unsigned long long milliseconds = (seconds * 1000) + (microseconds / 1000);

    return milliseconds;
}

pthread_mutex_t Worker::sink_lock0;
pthread_mutex_t Worker::sink_lock1;
pthread_mutex_t Worker::sink_lock2;
EncoderSink *Worker::stream0_sink = new EncoderSink{"Stream0Sink", 0, false, nullptr};
EncoderSink *Worker::stream1_sink = new EncoderSink{"Stream1Sink", 1, false, nullptr};
AudioSink *Worker::audio_sink = new AudioSink{"AudioSink", nullptr};

Worker *Worker::createNew(
    std::shared_ptr<CFG> cfg)
{
    return new Worker(cfg);
}

int Worker::init()
{
    LOG_DEBUG("Worker::init()");
    int ret = 0;

#if defined(PLATFORM_T23)
    ret = IMP_OSD_SetPoolSize(cfg->general.osd_pool_size * 1024);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_OSD_SetPoolSize(" << (cfg->general.osd_pool_size * 1024) << ")");
#endif

    if (!impsystem)
    {
        impsystem = IMPSystem::createNew(cfg);
    }

    if (cfg->stream0.enabled)
    {
        if (!framesources[0])
        {
            framesources[0] = IMPFramesource::createNew(&cfg->stream0, &cfg->sensor, 0);
        }
        encoder[0] = IMPEncoder::createNew(&cfg->stream0, cfg, 0, 0, "stream0");
        framesources[0]->enable();
    }

    if (cfg->stream1.enabled)
    {
        if (!framesources[1])
        {
            framesources[1] = IMPFramesource::createNew(&cfg->stream1, &cfg->sensor, 1);
        }
        encoder[1] = IMPEncoder::createNew(&cfg->stream1, cfg, 1, 1, "stream1");
        framesources[1]->enable();
    }

    if (cfg->stream2.enabled)
    {
        encoder[2] = IMPEncoder::createNew(&cfg->stream2, cfg, 2, cfg->stream2.jpeg_channel, "stream2");
    }

#if !defined(PLATFORM_T23)
    ret = IMP_OSD_SetPoolSize(cfg->general.osd_pool_size * 1024);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_OSD_SetPoolSize(" << (cfg->general.osd_pool_size * 1024) << ")");
#endif

    if (cfg->motion.enabled)
    {
        LOG_DEBUG("Motion enabled");
        ret = motion.init(cfg);
        LOG_DEBUG_OR_ERROR(ret, "motion.init(cfg)");
    }

    return ret;
}

int Worker::deinit()
{
    int ret;

    if (framesources[1])
    {
        framesources[1]->disable();

        if (encoder[1])
        {
            encoder[1]->deinit();
        }

        // delete framesources[1];
        // framesources[1] = nullptr;

        delete encoder[1];
        encoder[1] = nullptr;
    }

    if (framesources[0])
    {
        framesources[0]->disable();

        if (encoder[2])
        {
            encoder[2]->deinit();
        }

        if (encoder[0])
        {
            encoder[0]->deinit();
        }

        // delete framesources[0];
        // framesources[0] = nullptr;

        delete encoder[2];
        encoder[2] = nullptr;

        delete encoder[0];
        encoder[0] = nullptr;
    }

    // delete impsystem;
    // impsystem = nullptr;

    return 0;
}

std::vector<uint8_t> Worker::capture_jpeg_image(int encChn)
{
    std::vector<uint8_t> jpeg_data;
    int ret = 0;

    ret = IMP_Encoder_StartRecvPic(encChn);
    if (ret != 0)
    {
        std::cerr << "IMP_Encoder_StartRecvPic(" << encChn << ") failed: " << strerror(errno) << std::endl;
        return jpeg_data;
    }

    if (IMP_Encoder_PollingStream(encChn, 1000) == 0)
    {

        IMPEncoderStream stream;
        if (IMP_Encoder_GetStream(encChn, &stream, GET_STREAM_BLOCKING) == 0)
        {
            int nr_pack = stream.packCount;

            for (int i = 0; i < nr_pack; i++)
            {
                void *data_ptr;
                size_t data_len;

#if defined(PLATFORM_T31)
                IMPEncoderPack *pack = &stream.pack[i];
                uint32_t remSize = 0; // Declare remSize here
                if (pack->length)
                {
                    remSize = stream.streamSize - pack->offset;
                    data_ptr = (void *)((char *)stream.virAddr + ((remSize < pack->length) ? 0 : pack->offset));
                    data_len = (remSize < pack->length) ? remSize : pack->length;
                }
                else
                {
                    continue; // Skip empty packs
                }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
                data_ptr = reinterpret_cast<void *>(stream.pack[i].virAddr);
                data_len = stream.pack[i].length;
#endif

                // Write data to vector
                jpeg_data.insert(jpeg_data.end(), (uint8_t *)data_ptr, (uint8_t *)data_ptr + data_len);

#if defined(PLATFORM_T31)
                // Check the condition only under T31 platform, as remSize is used here
                if (remSize && pack->length > remSize)
                {
                    data_ptr = (void *)((char *)stream.virAddr);
                    data_len = pack->length - remSize;
                    jpeg_data.insert(jpeg_data.end(), (uint8_t *)data_ptr, (uint8_t *)data_ptr + data_len);
                }
#endif
            }

            IMP_Encoder_ReleaseStream(encChn, &stream); // Release stream after saving
        }
    }

    // ret = IMP_Encoder_StopRecvPic(encChn);
    if (ret != 0)
    {
        std::cerr << "IMP_Encoder_StopRecvPic(" << encChn << ") failed: " << strerror(errno) << std::endl;
    }

    return jpeg_data;
}

static int save_jpeg_stream(int fd, IMPEncoderStream *stream)
{
    int ret, i, nr_pack = stream->packCount;

    for (i = 0; i < nr_pack; i++)
    {
        void *data_ptr;
        size_t data_len;

#if defined(PLATFORM_T31)
        IMPEncoderPack *pack = &stream->pack[i];
        uint32_t remSize = 0; // Declare remSize here
        if (pack->length)
        {
            remSize = stream->streamSize - pack->offset;
            data_ptr = (void *)((char *)stream->virAddr + ((remSize < pack->length) ? 0 : pack->offset));
            data_len = (remSize < pack->length) ? remSize : pack->length;
        }
        else
        {
            continue; // Skip empty packs
        }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
        data_ptr = reinterpret_cast<void *>(stream->pack[i].virAddr);
        data_len = stream->pack[i].length;
#endif

        // Write data to file
        ret = write(fd, data_ptr, data_len);
        if (ret != static_cast<int>(data_len))
        {
            printf("Stream write error: %s\n", strerror(errno));
            return -1; // Return error on write failure
        }

#if defined(PLATFORM_T31)
        // Check the condition only under T31 platform, as remSize is used here
        if (remSize && pack->length > remSize)
        {
            ret = write(fd, (void *)((char *)stream->virAddr), pack->length - remSize);
            if (ret != static_cast<int>(pack->length - remSize))
            {
                printf("Stream write error (remaining part): %s\n", strerror(errno));
                return -1;
            }
        }
#endif
    }

    return 0;
}

void *Worker::jpeg_grabber(void *arg)
{
    Channel *channel = static_cast<Channel *>(arg);

    LOG_DEBUG("Start jpeg_grabber thread for stream " << channel->encChn);

    int ret;
    struct timeval ts;
    gettimeofday(&ts, NULL);

    ret = IMP_Encoder_StartRecvPic(channel->encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << channel->encChn << ")");
    if (ret != 0)
        return 0;

    channel->thread_signal.fetch_or(1);

    while (channel->thread_signal.load() & 1)
    {
        if (tDiffInMs(&ts) > 1000)
        {
            // LOG_DEBUG("IMP_Encoder_PollingStream 1 " << channel->encChn);
            if (IMP_Encoder_PollingStream(channel->encChn, channel->polling_timeout) == 0)
            {
                // LOG_DEBUG("IMP_Encoder_PollingStream 2 " << channel->encChn);

                IMPEncoderStream stream;
                if (IMP_Encoder_GetStream(channel->encChn, &stream, GET_STREAM_BLOCKING) == 0)
                {
                    // LOG_DEBUG("IMP_Encoder_GetStream" << channel->encChn);
                    //  Check for success
                    const char *tempPath = "/tmp/snapshot.tmp";         // Temporary path
                    const char *finalPath = channel->stream->jpeg_path; // Final path for the JPEG snapshot

                    // Open and create temporary file with read and write permissions
                    int snap_fd = open(tempPath, O_RDWR | O_CREAT | O_TRUNC, 0777);
                    if (snap_fd >= 0)
                    {
                        // Attempt to lock the temporary file for exclusive access
                        if (flock(snap_fd, LOCK_EX) == -1)
                        {
                            LOG_ERROR("Failed to lock JPEG snapshot for writing: " << tempPath);
                            close(snap_fd);
                            return 0; // Exit the function if unable to lock the file
                        }

                        // Save the JPEG stream to the file
                        save_jpeg_stream(snap_fd, &stream);

                        // Unlock and close the temporary file after writing is done
                        flock(snap_fd, LOCK_UN);
                        close(snap_fd);

                        // Atomically move the temporary file to the final destination
                        if (rename(tempPath, finalPath) != 0)
                        {
                            LOG_ERROR("Failed to move JPEG snapshot from " << tempPath << " to " << finalPath);
                            std::remove(tempPath); // Attempt to remove the temporary file if rename fails
                        }
                        else
                        {
                            // LOG_DEBUG("JPEG snapshot successfully updated");
                        }
                    }
                    else
                    {
                        LOG_ERROR("Failed to open JPEG snapshot for writing: " << tempPath);
                    }

                    IMP_Encoder_ReleaseStream(2, &stream); // Release stream after saving
                }
            }

            gettimeofday(&ts, NULL);
        }
        usleep(THREAD_SLEEP);
    }

    // ret = IMP_Encoder_StopRecvPic(channel->encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << channel->encChn << ")");

    return 0;
}

void *Worker::stream_grabber(void *arg)
{
    Channel *channel = static_cast<Channel *>(arg);

    LOG_DEBUG("Start stream_grabber thread for stream " << channel->encChn);

    int ret;
    int flags{0};
    uint32_t bps;
    uint32_t fps;
    int64_t nal_ts;
    uint32_t error_count;
    unsigned long long ms;
    struct timeval imp_time_base;

    gettimeofday(&imp_time_base, NULL);
    IMP_System_RebaseTimeStamp(imp_time_base.tv_sec * (uint64_t)1000000);

    ret = IMP_Encoder_StartRecvPic(channel->encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StartRecvPic(" << channel->encChn << ")");
    if (ret != 0)
        return 0;

    channel->thread_signal.fetch_or(1);

    while (channel->thread_signal.load() & 1)
    {
        if (video[channel->encChn]->onDataCallback != nullptr)
        {
            if (IMP_Encoder_PollingStream(channel->encChn, channel->polling_timeout) == 0)
            {
                IMPEncoderStream stream;
                if (IMP_Encoder_GetStream(channel->encChn, &stream, GET_STREAM_BLOCKING) != 0)
                {
                    LOG_ERROR("IMP_Encoder_GetStream(" << channel->encChn << ") failed");
                    error_count++;
                    continue;
                }

                int64_t nal_ts = stream.pack[stream.packCount - 1].timestamp;

                struct timeval encoder_time;
                encoder_time.tv_sec = nal_ts / 1000000;
                encoder_time.tv_usec = nal_ts % 1000000;

                for (uint32_t i = 0; i < stream.packCount; ++i)
                {
                    fps++;
                    bps += stream.pack[i].length;

                    pthread_mutex_lock(&channel->lock);
                    if (video[channel->encChn]->onDataCallback != nullptr)
                    {
#if defined(PLATFORM_T31)
                        uint8_t *start = (uint8_t *)stream.virAddr + stream.pack[i].offset;
                        uint8_t *end = start + stream.pack[i].length;
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
                        uint8_t *start = (uint8_t *)stream.pack[i].virAddr;
                        uint8_t *end = (uint8_t *)stream.pack[i].virAddr + stream.pack[i].length;
#endif
                        H264NALUnit nalu;

                        nalu.imp_ts = stream.pack[i].timestamp;
                        nalu.time = encoder_time;

                        // We use start+4 because the encoder inserts 4-byte MPEG
                        //'startcodes' at the beginning of each NAL. Live555 complains
                        nalu.data.insert(nalu.data.end(), start + 4, end);
                        if (channel->sink->IDR == false)
                        {
#if defined(PLATFORM_T31)
                            if (stream.pack[i].nalType.h264NalType == 7 ||
                                stream.pack[i].nalType.h264NalType == 8 ||
                                stream.pack[i].nalType.h264NalType == 5)
                            {
                                channel->sink->IDR = true;
                            }
                            else if (stream.pack[i].nalType.h265NalType == 32)
                            {
                                channel->sink->IDR = true;
                            }
#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23)
                            if (stream.pack[i].dataType.h264Type == 7 ||
                                stream.pack[i].dataType.h264Type == 8 ||
                                stream.pack[i].dataType.h264Type == 5)
                            {
                                channel->sink->IDR = true;
                            }
#elif defined(PLATFORM_T30)
                            if (stream.pack[i].dataType.h264Type == 7 ||
                                stream.pack[i].dataType.h264Type == 8 ||
                                stream.pack[i].dataType.h264Type == 5)
                            {
                                channel->sink->IDR = true;
                            }
                            else if (stream.pack[i].dataType.h265Type == 32)
                            {
                                channel->sink->IDR = true;
                            }
#endif
                        }

                        if (channel->sink->IDR == true)
                        {
                            if (!video[channel->encChn]->msgChannel->write(nalu)) {
                                LOG_ERROR("stream encChn:" << channel->encChn << ", size:" << nalu.data.size()
                                                           << ", pC:" << stream.packCount << ", pS:" << nalu.data.size() << ", pN:"
                                                           << i << " clogged!");                                
                            } else {
                                if(video[channel->encChn]->onDataCallback)
                                    video[channel->encChn]->onDataCallback();
                            }
                        }
                    }
                    pthread_mutex_unlock(&channel->lock);
                }

                IMP_Encoder_ReleaseStream(channel->encChn, &stream);

                ms = tDiffInMs(&channel->stream->osd.stats.ts);
                if (ms > 1000)
                {
                    channel->stream->osd.stats.bps = ((bps * 8) * 1000 / ms) / 1000;
                    bps = 0;
                    channel->stream->osd.stats.fps = fps * 1000 / ms;
                    fps = 0;
                    gettimeofday(&channel->stream->osd.stats.ts, NULL);

                    /*
                    IMPEncoderCHNStat encChnStats;
                    IMP_Encoder_Query(channel->encChn, &encChnStats);
                    LOG_DEBUG("ChannelStats::" << channel->encChn <<
                                ", registered:" << encChnStats.registered <<
                                ", leftPics:" << encChnStats.leftPics <<
                                ", leftStreamBytes:" << encChnStats.leftStreamBytes <<
                                ", leftStreamFrames:" << encChnStats.leftStreamFrames <<
                                ", curPacks:" << encChnStats.curPacks <<
                                ", work_done:" << encChnStats.work_done);
                    */
                }

                //setting thread_signal to already run. Osd can be updated.
                //is triggered first by RTSP, so don't reset it until rtsp restarts
                //otherwise osd updates won't work after encoder restart
                if (!(flags & 1))
                {
                    channel->thread_signal.fetch_or(2);
                    flags |= 1;
                }
            }
            else
            {
                error_count++;
                LOG_DDEBUG("IMP_Encoder_PollingStream(" << channel->encChn << ", " << channel->polling_timeout << ") timeout !");
                usleep(THREAD_SLEEP);
            }
        }
        else
        {          
            channel->stream->osd.stats.bps = 0;
            channel->stream->osd.stats.fps = 1;
            usleep(THREAD_SLEEP);
        }
    }

    ret = IMP_Encoder_StopRecvPic(channel->encChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_StopRecvPic(" << channel->encChn << ")");

    return 0;
}

void *Worker::audio_grabber(void *arg)
{
    AudioChannel *channel = static_cast<AudioChannel *>(arg);

    LOG_DEBUG("Start audio_grabber thread for device " << channel->devId << " and channel " << channel->inChn);

    while(true) {
            
        if (IMP_AI_PollingFrame(channel->devId, channel->inChn, 1000) == 0)
        {
            IMPAudioFrame frame;
            if (IMP_AI_GetFrame(channel->devId, channel->inChn, &frame, IMPBlock::BLOCK) != 0)
            {
                LOG_ERROR("IMP_AI_GetFrame(" << channel->devId << ", " << channel->inChn << ") failed");
            }

            if (channel->sink->data_available_callback != nullptr)
            {

                int64_t audio_ts = frame.timeStamp;
                struct timeval encoder_time;
                encoder_time.tv_sec = audio_ts / 1000000;
                encoder_time.tv_usec = audio_ts % 1000000;

                AudioFrame af;
                af.time = encoder_time;

                uint8_t *start = (uint8_t *)frame.virAddr;
                uint8_t *end = start + frame.len;
                LOG_DEBUG("insert");
                af.data.insert(af.data.end(), start, end);
                LOG_DEBUG("callback");
                if (channel->sink->data_available_callback(af))
                {
                    LOG_ERROR("stream audio:" << channel->devId << ", size:" << frame.len << " clogged!");
                }
                LOG_DEBUG("release");
                if(IMP_AI_ReleaseFrame(channel->devId, channel->inChn, &frame) < 0) {
                    LOG_ERROR("IMP_AI_ReleaseFrame(" << channel->devId << ", " << channel->inChn << ", &frame) failed");
                }

                LOG_DEBUG(channel->devId << ", " << channel->inChn << " == " << frame.len);
            }
        } else {

            LOG_DEBUG(channel->devId << ", " << channel->inChn << " POLLING TIMEOUT");
        }

        usleep(1000 * 1000);
    }

    return 0;
}

void *Worker::update_osd(void *arg) {

    Worker *worker = static_cast<Worker *>(arg);

    LOG_DEBUG("start osd update thread.");

    while(worker->osd_thread_signal.load() & 1) {
        for(auto chn : worker->channels) {
            if(chn != nullptr) {
                if(chn->thread_signal.load() & 3) {
                    if((worker->encoder[chn->encChn]->osd != nullptr)) {
                        if((worker->encoder[chn->encChn]->osd->flag & 16)) {
                            worker->encoder[chn->encChn]->osd->updateDisplayEverySecond();
                        } else {
                            worker->encoder[chn->encChn]->osd->start();
                        }
                    }
                }
            }
        }
        usleep(THREAD_SLEEP*2);
    }

    LOG_DEBUG("exit osd update thread.");
    return 0;
}

void Worker::run()
{
    LOG_DEBUG("Worker::run()");

    int ret;

    // 256 = exit thread
    while ((cfg->worker_thread_signal.load() & 256) != 256)
    {
        // 1 = init and start
        if (cfg->worker_thread_signal.load() & 1)
        {
            ret = init();
            LOG_DEBUG_OR_ERROR(ret, "init()");
            if (ret != 0)
                return;

            int policy = SCHED_RR;

            pthread_attr_init(&osd_thread_attr);
            pthread_attr_init(&jpeg_thread_attr);
            pthread_attr_init(&stream_thread_attr);

            pthread_attr_setschedpolicy(&osd_thread_attr, policy);
            pthread_attr_setschedpolicy(&jpeg_thread_attr, policy);
            pthread_attr_setschedpolicy(&stream_thread_attr, policy);

            int max_priority = sched_get_priority_max(policy);
            int min_priority = sched_get_priority_min(policy);

            osd_thread_sheduler.sched_priority = min_priority;  //(int)max_priority * 0.1;
            jpeg_thread_sheduler.sched_priority = min_priority; //(int)max_priority * 0.1;
            stream_thread_sheduler.sched_priority = (int)max_priority * 0.8;

            pthread_attr_setschedparam(&osd_thread_attr, &osd_thread_sheduler);
            pthread_attr_setschedparam(&jpeg_thread_attr, &jpeg_thread_sheduler);
            pthread_attr_setschedparam(&stream_thread_attr, &stream_thread_sheduler);

            /*
            audio0 = IMPAudio::createNew(cfg, 0, 0);
            audio_channel = new AudioChannel{0, 0, cfg->general.imp_polling_timeout, audio_sink, sink_lock2};
            pthread_create(&audio_threads[0], nullptr, audio_grabber, audio_channel);
            */

            if (cfg->rtsp_thread_signal == 0)
            {
                delay_osd = false;
            }
            else
            {
                delay_osd = true;
                cfg->rtsp_thread_signal = 0;
            }

            if (cfg->stream0.enabled)
            {
                channels[0] = new Channel{0, &cfg->stream0, cfg->general.imp_polling_timeout, stream0_sink, sink_lock0};
                start_stream(0);
            }

            if (cfg->stream1.enabled)
            {
                channels[1] = new Channel{1, &cfg->stream1, cfg->general.imp_polling_timeout, stream1_sink, sink_lock1};
                start_stream(1);
            }

            if (cfg->stream2.enabled)
            {
                channels[2] = new Channel{2, &cfg->stream2, cfg->general.imp_polling_timeout};
                pthread_create(&worker_threads[2], &jpeg_thread_attr, jpeg_grabber, channels[2]);
            }

            osd_thread_signal.fetch_or(1);
            pthread_create(&osd_thread, &osd_thread_attr, update_osd, this);
            
            cfg->worker_thread_signal.fetch_or(2);
        }

        // 1 & 2 = the threads are running, we can go to sleep
        if (cfg->worker_thread_signal.load() == 3)
        {
            LOG_DEBUG("The worker control thread goes into sleep mode");
            cfg->worker_thread_signal.wait(3);
            LOG_DEBUG("Worker control thread wakeup");
        }

        // 4 = Stop threads
        if (cfg->worker_thread_signal.load() & 4)
        {
            //stop osd thread
            osd_thread_signal.fetch_xor(1);
            LOG_DEBUG("stop signal is sent to osd thread " << cfg->worker_thread_signal.load());
            if (pthread_join(osd_thread, NULL) == 0)
            {
                LOG_DEBUG("wait for exit osd thread done.");
            }

            if (channels[0])
            {
                exit_stream(0);
            }

            if (channels[1])
            {
                exit_stream(1);
            }

            if (channels[2])
            {
                exit_stream(2);
            }

            pthread_attr_destroy(&osd_thread_attr);
            pthread_attr_destroy(&jpeg_thread_attr);
            pthread_attr_destroy(&stream_thread_attr);

            deinit();

            // remove stop and set stopped, will be handled in main
            cfg->worker_thread_signal.fetch_xor(4);
            cfg->worker_thread_signal.fetch_or(8);
        }
    }
    usleep(THREAD_SLEEP);
}

void Worker::start_stream(int encChn)
{
    int i = 0;

    pthread_mutex_init(&channels[encChn]->lock, NULL);
    gettimeofday(&channels[encChn]->stream->osd.stats.ts, NULL);
    pthread_create(&worker_threads[encChn], &stream_thread_attr, stream_grabber, channels[encChn]);
}

void Worker::exit_stream(int encChn)
{
    LOG_DEBUG("stop signal is sent to stream_grabber for stream" << encChn);
    channels[encChn]->thread_signal.fetch_xor(1);
    if (pthread_join(worker_threads[encChn], NULL) == 0)
    {
        LOG_DEBUG("wait for stream_grabber exit stream" << encChn);
    }
    LOG_DEBUG("stream_grabber for stream" << encChn << " has been terminated");
    pthread_mutex_destroy(&channels[encChn]->lock);
    delete channels[encChn];
    channels[encChn] = nullptr;
}