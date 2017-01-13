/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <deque>
#include <fcntl.h>
#include <random>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <storaged.h>               // data structures
#include <storaged_utils.h>         // functions to test

#define MMC_DISK_STATS_PATH "/sys/block/mmcblk0/stat"
#define SDA_DISK_STATS_PATH "/sys/block/sda/stat"
#define EMMC_EXT_CSD_PATH "/d/mmc0/mmc0:0001/ext_csd"
#define INIT_TASK_IO_PATH "/proc/1/io"

static void pause(uint32_t sec) {
    const char* path = "/cache/test";
    int fd = open(path, O_WRONLY | O_CREAT);
    ASSERT_LT(-1, fd);
    char buffer[2048];
    memset(buffer, 1, sizeof(buffer));
    int loop_size = 100;
    for (int i = 0; i < loop_size; ++i) {
        ASSERT_EQ(2048, write(fd, buffer, sizeof(buffer)));
    }
    fsync(fd);
    close(fd);

    fd = open(path, O_RDONLY);
    ASSERT_LT(-1, fd);
    for (int i = 0; i < loop_size; ++i) {
        ASSERT_EQ(2048, read(fd, buffer, sizeof(buffer)));
    }
    close(fd);

    sleep(sec);
}

// the return values of the tested functions should be the expected ones
const char* DISK_STATS_PATH;
TEST(storaged_test, retvals) {
    struct disk_stats stats;
    struct emmc_info info;
    memset(&stats, 0, sizeof(struct disk_stats));
    memset(&info, 0, sizeof(struct emmc_info));

    int emmc_fd = open(EMMC_EXT_CSD_PATH, O_RDONLY);
    if (emmc_fd >= 0) {
        EXPECT_TRUE(parse_emmc_ecsd(emmc_fd, &info));
    }

    if (access(MMC_DISK_STATS_PATH, R_OK) >= 0) {
        DISK_STATS_PATH = MMC_DISK_STATS_PATH;
    } else if (access(SDA_DISK_STATS_PATH, R_OK) >= 0) {
        DISK_STATS_PATH = SDA_DISK_STATS_PATH;
    } else {
        return;
    }

    EXPECT_TRUE(parse_disk_stats(DISK_STATS_PATH, &stats));

    struct disk_stats old_stats;
    memset(&old_stats, 0, sizeof(struct disk_stats));
    old_stats = stats;

    const char wrong_path[] = "/this/is/wrong";
    EXPECT_FALSE(parse_disk_stats(wrong_path, &stats));

    // reading a wrong path should not damage the output structure
    EXPECT_EQ(0, memcmp(&stats, &old_stats, sizeof(disk_stats)));
}

TEST(storaged_test, disk_stats) {
    struct disk_stats stats;
    memset(&stats, 0, sizeof(struct disk_stats));

    ASSERT_TRUE(parse_disk_stats(DISK_STATS_PATH, &stats));

    // every entry of stats (except io_in_flight) should all be greater than 0
    for (uint i = 0; i < DISK_STATS_SIZE; ++i) {
        if (i == 8) continue; // skip io_in_flight which can be 0
        EXPECT_LT((uint64_t)0, *((uint64_t*)&stats + i));
    }

    // accumulation of the increments should be the same with the overall increment
    struct disk_stats base, tmp, curr, acc, inc[5];
    memset(&base, 0, sizeof(struct disk_stats));
    memset(&tmp, 0, sizeof(struct disk_stats));
    memset(&acc, 0, sizeof(struct disk_stats));

    for (uint i = 0; i < 5; ++i) {
        ASSERT_TRUE(parse_disk_stats(DISK_STATS_PATH, &curr));
        if (i == 0) {
            base = curr;
            tmp = curr;
            sleep(5);
            continue;
        }
        inc[i] = get_inc_disk_stats(&tmp, &curr);
        add_disk_stats(&inc[i], &acc);
        tmp = curr;
        pause(5);
    }
    struct disk_stats overall_inc;
    memset(&overall_inc, 0, sizeof(disk_stats));
    overall_inc= get_inc_disk_stats(&base, &curr);

    for (uint i = 0; i < DISK_STATS_SIZE; ++i) {
        if (i == 8) continue; // skip io_in_flight which can be 0
        EXPECT_EQ(*((uint64_t*)&overall_inc + i), *((uint64_t*)&acc + i));
    }
}

TEST(storaged_test, emmc_info) {
    struct emmc_info info, void_info;
    memset(&info, 0, sizeof(struct emmc_info));
    memset(&void_info, 0, sizeof(struct emmc_info));

    if (access(EMMC_EXT_CSD_PATH, R_OK) >= 0) {
        int emmc_fd = open(EMMC_EXT_CSD_PATH, O_RDONLY);
        ASSERT_GE(emmc_fd, 0);
        ASSERT_TRUE(parse_emmc_ecsd(emmc_fd, &info));
        // parse_emmc_ecsd() should put something in info.
        EXPECT_NE(0, memcmp(&void_info, &info, sizeof(struct emmc_info)));
    }
}

TEST(storaged_test, task_info) {
    // parse_task_info should read something other than 0 from /proc/1/*
    struct task_info task_info;
    memset(&task_info, 0, sizeof(task_info));

    if (!parse_task_info(1, &task_info)) return;

    EXPECT_EQ((uint32_t)1, task_info.pid);
    EXPECT_LT((uint64_t)0, task_info.rchar);
    EXPECT_LT((uint64_t)0, task_info.wchar);
    EXPECT_LT((uint64_t)0, task_info.syscr);
    EXPECT_LT((uint64_t)0, task_info.syscw);
    EXPECT_LT((uint64_t)0, task_info.read_bytes);
    EXPECT_LT((uint64_t)0, task_info.write_bytes);
    // cancelled_write_bytes of init could be 0, there is no need to test
    EXPECT_LE((uint64_t)0, task_info.starttime);
    EXPECT_NE((char*)NULL, strstr(task_info.cmd, "init"));

    // Entries in /proc/1/io should be increasing through time
    struct task_info task_old, task_new;
    memset(&task_old, 0, sizeof(task_old));
    memset(&task_new, 0, sizeof(task_new));

    // parse_task_info should succeed at this point
    ASSERT_TRUE(parse_task_info(1, &task_old));
    sleep(1);
    ASSERT_TRUE(parse_task_info(1, &task_new));

    EXPECT_EQ(task_old.pid, task_new.pid);
    EXPECT_LE(task_old.rchar, task_new.rchar);
    EXPECT_LE(task_old.wchar, task_new.wchar);
    EXPECT_LE(task_old.syscr, task_new.syscr);
    EXPECT_LE(task_old.syscw, task_new.syscw);
    EXPECT_LE(task_old.read_bytes, task_new.read_bytes);
    EXPECT_LE(task_old.write_bytes, task_new.write_bytes);
    EXPECT_LE(task_old.cancelled_write_bytes, task_new.cancelled_write_bytes);
    EXPECT_EQ(task_old.starttime, task_new.starttime);
    EXPECT_EQ(0, strcmp(task_old.cmd, task_new.cmd));
}

static double mean(std::deque<uint32_t> nums) {
    double sum = 0.0;
    for (uint32_t i : nums) {
    sum += i;
    }
    return sum / nums.size();
}

static double standard_deviation(std::deque<uint32_t> nums) {
    double sum = 0.0;
    double avg = mean(nums);
    for (uint32_t i : nums) {
    sum += ((double)i - avg) * ((double)i - avg);
    }
    return sqrt(sum / nums.size());
}

TEST(storaged_test, stream_stats) {
    // 100 random numbers
    std::vector<uint32_t> data = {8147,9058,1270,9134,6324,975,2785,5469,9575,9649,1576,9706,9572,4854,8003,1419,4218,9157,7922,9595,6557,357,8491,9340,6787,7577,7431,3922,6555,1712,7060,318,2769,462,971,8235,6948,3171,9502,344,4387,3816,7655,7952,1869,4898,4456,6463,7094,7547,2760,6797,6551,1626,1190,4984,9597,3404,5853,2238,7513,2551,5060,6991,8909,9593,5472,1386,1493,2575,8407,2543,8143,2435,9293,3500,1966,2511,6160,4733,3517,8308,5853,5497,9172,2858,7572,7537,3804,5678,759,540,5308,7792,9340,1299,5688,4694,119,3371};
    std::deque<uint32_t> test_data;
    stream_stats sstats;
    for (uint32_t i : data) {
        test_data.push_back(i);
        sstats.add(i);

        EXPECT_EQ((int)standard_deviation(test_data), (int)sstats.get_std());
        EXPECT_EQ((int)mean(test_data), (int)sstats.get_mean());
    }

    for (uint32_t i : data) {
        test_data.pop_front();
        sstats.evict(i);

        EXPECT_EQ((int)standard_deviation(test_data), (int)sstats.get_std());
        EXPECT_EQ((int)mean(test_data), (int)sstats.get_mean());
    }

    // some real data
    std::vector<uint32_t> another_data = {113875,81620,103145,28327,86855,207414,96526,52567,28553,250311};
    test_data.clear();
    uint32_t window_size = 2;
    uint32_t idx;
    stream_stats sstats1;
    for (idx = 0; idx < window_size; ++idx) {
        test_data.push_back(another_data[idx]);
        sstats1.add(another_data[idx]);
    }
    EXPECT_EQ((int)standard_deviation(test_data), (int)sstats1.get_std());
    EXPECT_EQ((int)mean(test_data), (int)sstats1.get_mean());
    for (;idx < another_data.size(); ++idx) {
        test_data.pop_front();
        sstats1.evict(another_data[idx - window_size]);
        test_data.push_back(another_data[idx]);
        sstats1.add(another_data[idx]);
        EXPECT_EQ((int)standard_deviation(test_data), (int)sstats1.get_std());
        EXPECT_EQ((int)mean(test_data), (int)sstats1.get_mean());
    }
}

static void expect_increasing(struct task_info told, struct task_info tnew) {
    ASSERT_EQ(told.pid, tnew.pid);
    ASSERT_EQ(told.starttime, tnew.starttime);
    ASSERT_EQ(strcmp(told.cmd, tnew.cmd), 0);

    EXPECT_LE(told.rchar, tnew.rchar);
    EXPECT_LE(told.wchar, tnew.wchar);
    EXPECT_LE(told.syscr, tnew.syscr);
    EXPECT_LE(told.syscw, tnew.syscw);
    EXPECT_LE(told.read_bytes, tnew.read_bytes);
    EXPECT_LE(told.write_bytes, tnew.write_bytes);
    EXPECT_LE(told.cancelled_write_bytes, tnew.cancelled_write_bytes);
}

static void expect_equal(struct task_info told, struct task_info tnew) {
    ASSERT_EQ(told.pid, tnew.pid);
    ASSERT_EQ(told.starttime, tnew.starttime);
    ASSERT_EQ(strcmp(told.cmd, tnew.cmd), 0);

    EXPECT_EQ(told.rchar, tnew.rchar);
    EXPECT_EQ(told.wchar, tnew.wchar);
    EXPECT_EQ(told.syscr, tnew.syscr);
    EXPECT_EQ(told.syscw, tnew.syscw);
    EXPECT_EQ(told.read_bytes, tnew.read_bytes);
    EXPECT_EQ(told.write_bytes, tnew.write_bytes);
    EXPECT_EQ(told.cancelled_write_bytes, tnew.cancelled_write_bytes);
}

static std::set<uint32_t> find_overlap(std::unordered_map<uint32_t, struct task_info> t1,
                                       std::unordered_map<uint32_t, struct task_info> t2) {
    std::set<uint32_t> retval;
    for (auto i : t1) {
        if (t2.find(i.first) != t2.end()) {
            retval.insert(i.first);
        }
    }

    return retval;
}

static std::set<std::string> find_overlap(std::unordered_map<std::string, struct task_info> t1,
                                          std::unordered_map<std::string, struct task_info> t2) {
    std::set<std::string> retval;
    for (auto i : t1) {
        if (t2.find(i.first) != t2.end()) {
            retval.insert(i.first);
        }
    }

    return retval;
}

static bool cmp_app_name(struct task_info i, struct task_info j) {
    return strcmp(i.cmd, j.cmd) > 0;
}

static void expect_match(std::vector<struct task_info> v1, std::vector<struct task_info> v2) {
    ASSERT_EQ(v1.size(), v2.size());
    std::sort(v1.begin(), v1.end(), cmp_app_name);
    std::sort(v2.begin(), v2.end(), cmp_app_name);

    for (uint i = 0; i < v1.size(); ++i) {
        expect_equal(v1[i], v2[i]);
    }
}

static void add_task_info(struct task_info* src, struct task_info* dst) {
    ASSERT_EQ(0, strcmp(src->cmd, dst->cmd));

    dst->pid = 0;
    dst->rchar += src->rchar;
    dst->wchar += src->wchar;
    dst->syscr += src->syscr;
    dst->syscw += src->syscw;
    dst->read_bytes += src->read_bytes;
    dst->write_bytes += src->write_bytes;
    dst->cancelled_write_bytes += src->cancelled_write_bytes;
    dst->starttime = 0;
}

static std::vector<struct task_info>
categorize_tasks(std::unordered_map<uint32_t, struct task_info> tasks) {
    std::unordered_map<std::string, struct task_info> tasks_cmd;
    for (auto i : tasks) {
        std::string cmd = i.second.cmd;
        if (tasks_cmd.find(cmd) == tasks_cmd.end()) {
            tasks_cmd[cmd] = i.second;
        } else {
            add_task_info(&i.second, &tasks_cmd[cmd]);
        }
    }

    std::vector<struct task_info> retval(tasks_cmd.size());
    int cnt = 0;
    for (auto i : tasks_cmd) {
        retval[cnt++] = i.second;
    }

    return retval;
}

#define TEST_LOOPS 20
TEST(storaged_test, tasks_t) {
    // pass this test if /proc/[pid]/io is not readable
    const char* test_paths[] = {"/proc/1/io", "/proc/1/comm", "/proc/1/cmdline", "/proc/1/stat"};
    for (uint i = 0; i < sizeof(test_paths) / sizeof(const char*); ++i) {
        if (access(test_paths[i], R_OK) < 0) return;
    }

    tasks_t tasks;
    EXPECT_EQ((uint32_t)0, tasks.mRunning.size());
    EXPECT_EQ((uint32_t)0, tasks.mOld.size());

    tasks.update_running_tasks();

    std::unordered_map<uint32_t, struct task_info> prev_running = tasks.mRunning;
    std::unordered_map<std::string, struct task_info> prev_old = tasks.mOld;

    // hashmap maintaining
    std::unordered_map<uint32_t, struct task_info> tasks_pid = tasks.mRunning;

    // get_running_tasks() should return something other than a null map
    std::unordered_map<uint32_t, struct task_info> test = tasks.get_running_tasks();
    EXPECT_LE((uint32_t)1, test.size());

    for (int i = 0; i < TEST_LOOPS; ++i) {
        tasks.update_running_tasks();

        std::set<uint32_t> overlap_running = find_overlap(prev_running, tasks.mRunning);
        std::set<std::string> overlap_old = find_overlap(prev_old, tasks.mOld);

        // overlap_running should capture init(pid == 1), since init never get killed
        EXPECT_LE((uint32_t)1, overlap_running.size());
        EXPECT_NE(overlap_running.find((uint32_t)1), overlap_running.end());
        // overlap_old should never capture init, since init never get killed
        EXPECT_EQ(overlap_old.find("init"), overlap_old.end());

        // overlapping entries in previous and current running-tasks map should have increasing contents
        for (uint32_t i : overlap_running) {
            expect_increasing(prev_running[i], tasks.mRunning[i]);
        }

        // overlapping entries in previous and current killed-tasks map should have increasing contents
        // and the map size should also be increasing
        for (std::string i : overlap_old) {
            expect_increasing(prev_old[i], tasks.mOld[i]);
        }
        EXPECT_LE(prev_old.size(), tasks.mRunning.size());

        // update app name & tasks_pid
        for (auto i : tasks.mRunning) {
            // test will fail if the pid got wrapped
            if (tasks_pid.find(i.first) != tasks_pid.end()) {
                expect_increasing(tasks_pid[i.first], i.second);
                tasks_pid[i.first] = i.second;
            } else {
                tasks_pid[i.first] = i.second;
            }
        }

        // get maintained tasks
        std::vector<struct task_info> test_tasks = categorize_tasks(tasks_pid);
        std::vector<struct task_info> real_tasks = tasks.get_tasks();

        expect_match(test_tasks, real_tasks);

        prev_running = tasks.mRunning;
        prev_old = tasks.mOld;

        pause(5);
    }
}

static struct disk_perf disk_perf_multiply(struct disk_perf perf, double mul) {
    struct disk_perf retval;
    retval.read_perf = (double)perf.read_perf * mul;
    retval.read_ios = (double)perf.read_ios * mul;
    retval.write_perf = (double)perf.write_perf * mul;
    retval.write_ios = (double)perf.write_ios * mul;
    retval.queue = (double)perf.queue * mul;

    return retval;
}

static struct disk_stats disk_stats_add(struct disk_stats stats1, struct disk_stats stats2) {
    struct disk_stats retval;
    retval.read_ios = stats1.read_ios + stats2.read_ios;
    retval.read_merges = stats1.read_merges + stats2.read_merges;
    retval.read_sectors = stats1.read_sectors + stats2.read_sectors;
    retval.read_ticks = stats1.read_ticks + stats2.read_ticks;
    retval.write_ios = stats1.write_ios + stats2.write_ios;
    retval.write_merges = stats1.write_merges + stats2.write_merges;
    retval.write_sectors = stats1.write_sectors + stats2.write_sectors;
    retval.write_ticks = stats1.write_ticks + stats2.write_ticks;
    retval.io_in_flight = stats1.io_in_flight + stats2.io_in_flight;
    retval.io_ticks = stats1.io_ticks + stats2.io_ticks;
    retval.io_in_queue = stats1.io_in_queue + stats2.io_in_queue;
    retval.end_time = stats1.end_time + stats2.end_time;

    return retval;
}

TEST(storaged_test, disk_stats_monitor) {
    // asserting that there is one file for diskstats
    ASSERT_TRUE(access(MMC_DISK_STATS_PATH, R_OK) >= 0 || access(SDA_DISK_STATS_PATH, R_OK) >= 0);
    // testing if detect() will return the right value
    disk_stats_monitor dsm_detect;
    // feed monitor with constant perf data for io perf baseline
    // using constant perf is reasonable since the functionality of stream_stats
    // has already been tested
    struct disk_perf norm_perf = {
        .read_perf = 10 * 1024,
        .read_ios = 50,
        .write_perf = 5 * 1024,
        .write_ios = 25,
        .queue = 5
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> rand(0.8, 1.2);

    for (uint i = 0; i < dsm_detect.mWindow; ++i) {
        struct disk_perf perf = disk_perf_multiply(norm_perf, rand(gen));

        dsm_detect.add(&perf);
        dsm_detect.mBuffer.push(perf);
        EXPECT_EQ(dsm_detect.mBuffer.size(), (uint64_t)i + 1);
    }

    dsm_detect.mValid = true;
    dsm_detect.update_mean();
    dsm_detect.update_std();

    for (double i = 0; i < 2 * dsm_detect.mSigma; i += 0.5) {
        struct disk_perf test_perf;
        struct disk_perf test_mean = dsm_detect.mMean;
        struct disk_perf test_std = dsm_detect.mStd;

        test_perf.read_perf = (double)test_mean.read_perf - i * test_std.read_perf;
        test_perf.read_ios = (double)test_mean.read_ios - i * test_std.read_ios;
        test_perf.write_perf = (double)test_mean.write_perf - i * test_std.write_perf;
        test_perf.write_ios = (double)test_mean.write_ios - i * test_std.write_ios;
        test_perf.queue = (double)test_mean.queue + i * test_std.queue;

        EXPECT_EQ((i > dsm_detect.mSigma), dsm_detect.detect(&test_perf));
    }

    // testing if stalled disk_stats can be correctly accumulated in the monitor
    disk_stats_monitor dsm_acc;
    struct disk_stats norm_inc = {
        .read_ios = 200,
        .read_merges = 0,
        .read_sectors = 200,
        .read_ticks = 200,
        .write_ios = 100,
        .write_merges = 0,
        .write_sectors = 100,
        .write_ticks = 100,
        .io_in_flight = 0,
        .io_ticks = 600,
        .io_in_queue = 300,
        .start_time = 0,
        .end_time = 100,
        .counter = 0,
        .io_avg = 0
    };

    struct disk_stats stall_inc = {
        .read_ios = 200,
        .read_merges = 0,
        .read_sectors = 20,
        .read_ticks = 200,
        .write_ios = 100,
        .write_merges = 0,
        .write_sectors = 10,
        .write_ticks = 100,
        .io_in_flight = 0,
        .io_ticks = 600,
        .io_in_queue = 1200,
        .start_time = 0,
        .end_time = 100,
        .counter = 0,
        .io_avg = 0
    };

    struct disk_stats stats_base;
    memset(&stats_base, 0, sizeof(stats_base));

    int loop_size = 100;
    for (int i = 0; i < loop_size; ++i) {
        stats_base = disk_stats_add(stats_base, norm_inc);
        dsm_acc.update(&stats_base);
        EXPECT_EQ(dsm_acc.mValid, (uint32_t)(i + 1) >= dsm_acc.mWindow);
        EXPECT_FALSE(dsm_acc.mStall);
    }

    stats_base = disk_stats_add(stats_base, stall_inc);
    dsm_acc.update(&stats_base);
    EXPECT_TRUE(dsm_acc.mValid);
    EXPECT_TRUE(dsm_acc.mStall);

    for (int i = 0; i < 10; ++i) {
        stats_base = disk_stats_add(stats_base, norm_inc);
        dsm_acc.update(&stats_base);
        EXPECT_TRUE(dsm_acc.mValid);
        EXPECT_FALSE(dsm_acc.mStall);
    }
}

static void expect_increasing(struct disk_stats stats1, struct disk_stats stats2) {
    EXPECT_LE(stats1.read_ios, stats2.read_ios);
    EXPECT_LE(stats1.read_merges, stats2.read_merges);
    EXPECT_LE(stats1.read_sectors, stats2.read_sectors);
    EXPECT_LE(stats1.read_ticks, stats2.read_ticks);

    EXPECT_LE(stats1.write_ios, stats2.write_ios);
    EXPECT_LE(stats1.write_merges, stats2.write_merges);
    EXPECT_LE(stats1.write_sectors, stats2.write_sectors);
    EXPECT_LE(stats1.write_ticks, stats2.write_ticks);

    EXPECT_LE(stats1.io_ticks, stats2.io_ticks);
    EXPECT_LE(stats1.io_in_queue, stats2.io_in_queue);
}

TEST(storaged_test, disk_stats_publisher) {
    // asserting that there is one file for diskstats
    ASSERT_TRUE(access(MMC_DISK_STATS_PATH, R_OK) >= 0 || access(SDA_DISK_STATS_PATH, R_OK) >= 0);
    disk_stats_publisher dsp;
    struct disk_stats prev;
    memset(&prev, 0, sizeof(prev));

    for (int i = 0; i < TEST_LOOPS; ++i) {
        dsp.update();
        expect_increasing(prev, dsp.mPrevious);
        prev = dsp.mPrevious;
        pause(10);
    }
}

