/**
 * @file
 * @brief To know IO response time of file or device.
 * @author HOSHINO Takashi
 */
#define _FILE_OFFSET_BITS 64

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <queue>
#include <utility>
#include <tuple>
#include <algorithm>
#include <future>
#include <mutex>
#include <exception>
#include <limits>

#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ioreth.hpp"
#include "util.hpp"
#include "rand.hpp"

class Options
{
private:
    std::string programName_;
    size_t accessRange_;
    size_t blockSize_;
    std::vector<std::string> args_;
    Mode mode_;
    bool isShowEachResponse_;
    bool isShowVersion_;
    bool isShowHelp_;
    
    size_t period_;
    size_t count_;
    size_t nthreads_;
    size_t queueSize_;

public:
    Options(int argc, char* argv[])
        : accessRange_(0)
        , blockSize_(0)
        , args_()
        , mode_(READ_MODE)
        , isShowEachResponse_(false)
        , isShowVersion_(false)
        , isShowHelp_(false)
        , period_(0)
        , count_(0)
        , nthreads_(1)
        , queueSize_(1) {

        parse(argc, argv);

        if (isShowVersion_ || isShowHelp_) {
            return;
        }
        checkAndThrow();
    }

    void showVersion() {

        ::printf("iores version %s\n", IORETH_VERSION);
    }
    
    void showHelp() {

        ::printf("usage: %s [option(s)] [file or device]\n"
                 "options: \n"
                 "    -s size: access range in blocks.\n"
                 "    -b size: blocksize in bytes.\n"
                 "    -p secs: execute period in seconds.\n"
                 "    -c num:  number of IOs to execute.\n"
                 "             -p and -c is exclusive.\n"
                 "    -w:      write instead read.\n"
                 "    -m:      read/write mix instead read.\n"
                 "             -w and -m is exclusive.\n"
                 "    -t num:  number of threads in parallel.\n"
                 "             if 0, use aio instead thread.\n"
                 "    -q size: queue size per thread.\n"
                 "             this is meaningfull with -t 0.\n"
                 "    -r:      show response of each IO.\n"
                 "    -v:      show version.\n"
                 "    -h:      show this help.\n"
                 , programName_.c_str()
            );
    }

    const std::vector<std::string>& getArgs() const { return args_; }
    size_t getAccessRange() const { return accessRange_; }
    size_t getBlockSize() const { return blockSize_; }
    Mode getMode() const { return mode_; }
    bool isShowEachResponse() const { return isShowEachResponse_; }
    bool isShowVersion() const { return isShowVersion_; }
    bool isShowHelp() const { return isShowHelp_; }
    size_t getPeriod() const { return period_; }
    size_t getCount() const { return count_; }
    size_t getNthreads() const { return nthreads_; }
    size_t getQueueSize() const { return queueSize_; }

private:
    void parse(int argc, char* argv[]) {

        programName_ = argv[0];
        
        while (1) {
            int c = ::getopt(argc, argv, "s:b:p:c:t:q:wmrvh");

            if (c < 0) { break; }

            switch (c) {
            case 's': /* disk access range in blocks */
                accessRange_ = ::atol(optarg);
                break;
            case 'b': /* blocksize */
                blockSize_ = ::atol(optarg);
                break;
            case 'p': /* period */
                period_ = ::atol(optarg);
                break;
            case 'c': /* count */
                count_ = ::atol(optarg);
                break;
            case 'w': /* write */
                mode_ = WRITE_MODE;
                break;
            case 'm': /* mix */
                mode_ = MIX_MODE;
                break;
            case 't': /* nthreads */
                nthreads_ = ::atol(optarg);
                break;
            case 'q':
                queueSize_ = ::atol(optarg);
                break;
            case 'r': /* show each response */
                isShowEachResponse_ = true;
                break;
            case 'v': /* show version */
                isShowVersion_ = true;
                break;
            case 'h': /* help */
                isShowHelp_ = true;
                break;
            }
        }

        while (optind < argc) {
            args_.push_back(argv[optind++]);
        }
    }

    void checkAndThrow() {

        if (args_.size() != 1 || blockSize_ == 0) {
            throw std::runtime_error("specify blocksize (-b), and device.");
        }
        if (period_ == 0 && count_ == 0) {
            throw std::runtime_error("specify period (-p) or count (-c).");
        }
        if (nthreads_ == 0 && queueSize_ == 0) {
            throw std::runtime_error("queue size (-q) must be 1 or more when -t 0.");
        }
    }
};

/**
 * Single-threaded io response benchmark.
 */
class IoResponseBench
{
private:
    const int threadId_;
    BlockDevice& dev_;
    size_t blockSize_;
    size_t accessRange_;
    void* bufV_;
    char* buf_;
    std::queue<IoLog>& rtQ_;
    PerformanceStatistics& stat_;
    bool isShowEachResponse_;
    XorShift128 rand_;

    std::mutex& mutex_; //shared among threads.
    
public:
    /**
     * @param dev block device.
     * @param bs block size.
     * @param accessRange in blocks.
     */
    IoResponseBench(int threadId, BlockDevice& dev, size_t blockSize,
                    size_t accessRange, std::queue<IoLog>& rtQ,
                    PerformanceStatistics& stat,
                    bool isShowEachResponse, std::mutex& mutex)
        : threadId_(threadId)
        , dev_(dev)
        , blockSize_(blockSize)
        , accessRange_(calcAccessRange(accessRange, blockSize, dev))
        , bufV_(nullptr)
        , buf_(nullptr)
        , rtQ_(rtQ)
        , stat_(stat)
        , isShowEachResponse_(isShowEachResponse)
        , rand_(getSeed())
        , mutex_(mutex) {
#if 0
        ::printf("blockSize %zu accessRange %zu isShowEachResponse %d\n",
                 blockSize_, accessRange_, isShowEachResponse_);
#endif
        size_t alignSize = 512;
        while (alignSize < blockSize_) {
            alignSize *= 2;
        }
        if(::posix_memalign(&bufV_, alignSize, blockSize_) != 0) {
            throw std::runtime_error("posix_memalign failed");
        }
        buf_ = static_cast<char*>(bufV_);
        
        for (size_t i = 0; i < blockSize_; i++) {
            buf_[i] = static_cast<char>(rand_.get(256));
        }
    }
    ~IoResponseBench() {

        ::free(bufV_);
    }
    void execNtimes(size_t n) {

        for (size_t i = 0; i < n; i++) {
            IoLog log = execBlockIO();
            if (isShowEachResponse_) { rtQ_.push(log); }
            stat_.updateRt(log.response);
        }
        putStat();
    }
    void execNsecs(size_t n) {

        double begin, end;
        begin = getTime(); end = begin;

        while (end - begin < static_cast<double>(n)) {

            IoLog log = execBlockIO();
            if (isShowEachResponse_) { rtQ_.push(log); }
            stat_.updateRt(log.response);
            end = getTime();
        }
        putStat();
    }
    
private:
    /**
     * @return response time.
     */
    IoLog execBlockIO() {
        
        double begin, end;
        size_t blockId = rand_.get(accessRange_);
        size_t oft = blockId * blockSize_;
        begin = getTime();
        bool isWrite = false;
        
        switch(dev_.getMode()) {
        case READ_MODE:  isWrite = false; break;
        case WRITE_MODE: isWrite = true; break;
        case MIX_MODE:   isWrite = (rand_.get(2) == 0); break;
        }
        
        if (isWrite) {
            dev_.write(oft, blockSize_, buf_);
        } else {
            dev_.read(oft, blockSize_, buf_);
        }
        end = getTime();
        return IoLog(threadId_, isWrite, blockId, begin, end - begin);
    }

    void putStat() const {
        std::lock_guard<std::mutex> lk(mutex_);

        ::printf("id %d ", threadId_);
        stat_.print();
    }

    uint32_t getSeed() const {

        Rand<uint32_t, std::uniform_int_distribution<uint32_t> >
            rand(0, std::numeric_limits<uint32_t>::max());
        return rand.get();
    }
};

void do_work(int threadId, const Options& opt,
             std::queue<IoLog>& rtQ, PerformanceStatistics& stat,
             std::mutex& mutex)
{
    const bool isDirect = true;

    BlockDevice bd(opt.getArgs()[0], opt.getMode(), isDirect);
    
    IoResponseBench bench(threadId, bd, opt.getBlockSize(), opt.getAccessRange(),
                          rtQ, stat, opt.isShowEachResponse(), mutex);
    if (opt.getPeriod() > 0) {
        bench.execNsecs(opt.getPeriod());
    } else {
        bench.execNtimes(opt.getCount());
    }
}

void worker_start(std::vector<std::future<void> >& workers, int n, const Options& opt,
                  std::vector<std::queue<IoLog> >& rtQs,
                  std::vector<PerformanceStatistics>& stats,
                  std::mutex& mutex)
{
    rtQs.resize(n);
    stats.resize(n);
    for (int i = 0; i < n; i++) {

        std::future<void> f = std::async(
            std::launch::async, do_work, i, std::ref(opt), std::ref(rtQs[i]),
            std::ref(stats[i]), std::ref(mutex));
        workers.push_back(std::move(f));
    }
}

void worker_join(std::vector<std::future<void> >& workers)
{
    std::for_each(workers.begin(), workers.end(),
                  [](std::future<void>& f) { f.get(); });
}

void pop_and_show_logQ(std::queue<IoLog>& logQ)
{
    while (! logQ.empty()) {
        IoLog& log = logQ.front();
        log.print();
        logQ.pop();
    }
}

void execThreadExperiment(const Options& opt)
{
    const size_t nthreads = opt.getNthreads();
    assert(nthreads > 0);

    std::vector<std::queue<IoLog> > logQs;
    std::vector<PerformanceStatistics> stats;
    
    std::vector<std::future<void> > workers;
    double begin, end;
    std::mutex mutex;
    
    begin = getTime();
    worker_start(workers, nthreads, opt, logQs, stats, mutex);
    worker_join(workers);
    end = getTime();

    assert(logQs.size() == nthreads);
    std::for_each(logQs.begin(), logQs.end(), pop_and_show_logQ);

    PerformanceStatistics stat = mergeStats(stats.begin(), stats.end());
    ::printf("---------------\n"
             "all ");
    stat.print();
    printThroughput(opt.getBlockSize(), stat.getCount(), end - begin);
}

/**
 * Io response bench with aio.
 */
class AioResponseBench
{
private:
    const BlockDevice& dev_;
    const size_t blockSize_;
    const size_t queueSize_;
    const size_t accessRange_;
    const bool isShowEachResponse_;
    const Mode mode_;
    
    BlockBuffer bb_;
    Rand<size_t, std::uniform_int_distribution<size_t> > rand_;
    std::queue<IoLog> logQ_;
    PerformanceStatistics stat_;
    Aio aio_;
    

public:
    AioResponseBench(const BlockDevice& dev, size_t blockSize, size_t queueSize,
                     size_t accessRange, bool isShowEachResponse)
        : dev_(dev)
        , blockSize_(blockSize)
        , queueSize_(queueSize)
        , accessRange_(calcAccessRange(accessRange, blockSize, dev))
        , isShowEachResponse_(isShowEachResponse)
        , mode_(dev.getMode())
        , bb_(queueSize * 2, blockSize)
        , rand_(0, std::numeric_limits<size_t>::max())
        , logQ_()
        , stat_()
        , aio_(dev.getFd(), queueSize) {

        assert(blockSize_ % 512 == 0);
        assert(queueSize_ > 0);
        assert(accessRange_ > 0);
    }        
    
    void execNtimes(size_t nTimes) {

        size_t pending = 0;
        size_t c = 0;

        // Fill the queue.
        while (pending < queueSize_ && c < nTimes) {
            prepareIo(bb_.next());
            pending++;
            c++;
        }
        aio_.submit();
        // Wait and fill.
        while (c < nTimes) {
            assert(pending == queueSize_);

            waitAnIo();
            pending--;

            prepareIo(bb_.next()); 
            pending++;
            c++;
            aio_.submit();
        }
        // Wait remaining.
        while (pending > 0) {
            waitAnIo();
            pending--;
        }
    }

    void execNsecs(size_t nSecs) {

        double begin, end;
        begin = getTime(); end = begin;

        size_t pending = 0;

        // Fill the queue.
        while (pending < queueSize_) {
            prepareIo(bb_.next());
            pending++;
        }
        aio_.submit();
        // Wait and fill.
        while (end - begin < static_cast<double>(nSecs)) {
            assert(pending == queueSize_);

            end = waitAnIo();
            pending--;

            prepareIo(bb_.next()); 
            pending++;
            aio_.submit();
        }
        // Wait pending.
        while (pending > 0) {
            waitAnIo();
            pending--;
        }
    }

    PerformanceStatistics& getStat() { return stat_; }
    std::queue<IoLog>& getIoLogQueue() { return logQ_; }
    
private:
    bool decideIsWrite() {

        bool isWrite = false;
        
        switch(mode_) {
        case READ_MODE:
            isWrite = false;
            break;
        case WRITE_MODE:
            isWrite = true;
            break;
        case MIX_MODE:
            isWrite = rand_.get(2) == 0;
            break;
        default:
            assert(false);
        }
        return isWrite;
    }
    
    void prepareIo(char *buf) {

        size_t blockId = rand_.get(accessRange_);
        
        if (decideIsWrite()) {
            aio_.prepareWrite(blockId * blockSize_, blockSize_, buf);
        } else {
            aio_.prepareRead(blockId * blockSize_, blockSize_, buf);
        }
    }

    double waitAnIo() {

        auto* ptr = aio_.waitOne();
        auto log = toIoLog(ptr);
        stat_.updateRt(log.response);
        if (isShowEachResponse_) {
            logQ_.push(log);
        }
        return ptr->endTime;
    }
    
    IoLog toIoLog(AioData *ptr) {

        return IoLog(0, ptr->isWrite, ptr->oft / ptr->size,
                     ptr->beginTime, ptr->endTime - ptr->beginTime);
    }
};

void execAioExperiment(const Options& opt)
{
    assert(opt.getNthreads() == 0);
    const size_t queueSize = opt.getQueueSize();
    assert(queueSize > 0);
    
    const bool isDirect = true;
    BlockDevice bd(opt.getArgs()[0], opt.getMode(), isDirect);
    
    AioResponseBench bench(bd, opt.getBlockSize(), opt.getQueueSize(),
                           opt.getAccessRange(),
                           opt.isShowEachResponse());
    
    double begin, end;
    begin = getTime();
    if (opt.getPeriod() > 0) {
        bench.execNsecs(opt.getPeriod());
    } else {
        bench.execNtimes(opt.getCount());
    }
    end = getTime();

    pop_and_show_logQ(bench.getIoLogQueue());
    auto& stat = bench.getStat();
    ::printf("all ");
    stat.print();
    printThroughput(opt.getBlockSize(), stat.getCount(), end - begin);
}

int main(int argc, char* argv[])
{
    try {
        Options opt(argc, argv);

        if (opt.isShowVersion()) {
            opt.showVersion();
        } else if (opt.isShowHelp()) {
            opt.showHelp();
        } else {
            if (opt.getNthreads() == 0) {
                execAioExperiment(opt);
            } else {
                execThreadExperiment(opt);
            }
        }
    } catch (const std::runtime_error& e) {
        ::printf("error: %s\n", e.what());
    } catch (...) {
        ::printf("caught another error.\n");
    }
    
    return 0;
}
