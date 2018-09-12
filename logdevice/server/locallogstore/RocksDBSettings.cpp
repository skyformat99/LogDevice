/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/server/locallogstore/RocksDBSettings.h"

#include <alloca.h>
#include <cstdio>
#include <cstring>
#include <string>

#include <boost/program_options.hpp>
#include <folly/Memory.h>
#include <rocksdb/table.h>

#include "logdevice/common/commandline_util_chrono.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/settings/util.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/util.h"
#include "logdevice/common/settings/Validators.h"

using namespace facebook::logdevice::setting_validators;

/**
 * @file Settings for rocksdb-based LocalLogStore implementations. Some of these
 *       options are passed through to rocksdb::Options.
 */

namespace facebook { namespace logdevice {

// common prefix of all command-line options that configure rocksdb
static const char* rocksdb_option_prefix = "rocksdb";

// add "uc-" prefix to universal compaction options
static const char* universal_compaction_opt_prefix = "uc";

/**
 * Convert the name of a rocksdb::Options field into the name of a
 * command-line option that configures that field. The conversion consists
 * od adding a rocksdb- prefix and replacing all underscores with dashes
 * to conform to LD command line options conventions.
 *
 * @param field_name   name of rocksdb::Options field as a string
 * @param buf          output buffer. This should be large enough to contain
 *                     the "rocksdb-" prefix, field_name, and the terminating
 *                     NUL.
 * @param buflen       output buffer length
 * @return NUL-terminated option name in buf
 */
static const char* rocksdbOptionName(const char* field_name,
                                     char* buf,
                                     size_t buflen) {
  if (strncmp(field_name, "uc_", strlen("uc_")) == 0) {
    snprintf(buf,
             buflen,
             "%s-%s-%s",
             rocksdb_option_prefix,
             universal_compaction_opt_prefix,
             field_name + strlen("uc_"));
  } else {
    snprintf(buf, buflen, "%s-%s", rocksdb_option_prefix, field_name);
  }

  for (char* p = buf; *p; p++) {
    if (*p == '_') {
      *p = '-';
    }
  }

  return buf;
}

#define OPTNAME_impl(field, len) \
  rocksdbOptionName(field, (char*)alloca(len), len)
#define OPTNAME(field)                                           \
  OPTNAME_impl(#field,                                           \
               strlen(rocksdb_option_prefix) + 1 +               \
                   strlen(universal_compaction_opt_prefix) + 1 + \
                   strlen(#field) + 1)

void RocksDBSettings::defineSettings(SettingEasyInit& init) {
  using namespace SettingFlag;

  init

      (OPTNAME(compaction_style),
       &compaction_style,
       "universal",
       [](const std::string& val) {
         if (val == "universal") {
           return rocksdb::kCompactionStyleUniversal;
         } else if (val == "level") {
           return rocksdb::kCompactionStyleLevel;
         } else {
           throw boost::program_options::error(
               "invalid value '" + val + "'for option --" +
               OPTNAME(compaction_style) + ". Expected 'universal' or 'level'");
         }
       },
       "compaction style: 'universal' (default) or 'level'; if using 'level', "
       "also set --num-levels to at least 2",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(compression_type),
       &compression,
       "none",
       [](const std::string& val) {
         if (val == "snappy") {
           return rocksdb::kSnappyCompression;
         } else if (val == "none") {
           return rocksdb::kNoCompression;
         } else if (val == "zlib") {
           return rocksdb::kZlibCompression;
         } else if (val == "bzip2") {
           return rocksdb::kBZip2Compression;
         } else if (val == "lz4") {
           return rocksdb::kLZ4Compression;
         } else if (val == "lz4hc") {
           return rocksdb::kLZ4HCCompression;
         } else if (val == "xpress") {
           return rocksdb::kXpressCompression;
         } else if (val == "zstd") {
           return rocksdb::kZSTD;
         } else {
           throw boost::program_options::error("invalid value '" + val +
                                               "' for option --" +
                                               OPTNAME(compression_type));
         }
       },
       "compression algorithm: 'snappy' (default), 'none', 'zlib', 'bzip2', "
       "'lz4', 'lz4hc', 'zstd'",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(enable_statistics),
       &statistics,
       "true",
       nullptr,
       "if set, instruct RocksDB to collect various statistics",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(compaction_access_sequential),
       &compaction_access_sequential,
       "true",
       nullptr,
       "suggest to the OS that input files will be accessed sequentially "
       "during compaction",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(
      OPTNAME(compaction_ratelimit),
      &compaction_rate_limit_,
      "30M/1s",
      [](const std::string& val) {
        rate_limit_t r;
        int rv = parse_rate_limit(val.c_str(), &r);
        if (rv != 0 || r.first == 0) {
          throw boost::program_options::error(
              "invalid value '" + val +
              "' for option --compaction-ratelimit; expected rate limit in "
              "format <count><suffix>/<duration><unit>, e.g. 5M/1s or "
              "\"unlimited\".");
        }
        return r;
      },
      "limits how fast compactions can read uncompressed data, in bytes; "
      "format is <count><suffix>/<duration><unit>. Example: 5M/500ms means "
      "compaction will read 5MB per 500ms. This is applied to each compaction "
      "independently (e.g. if multiple shards are compacting simultaneously "
      "the total rate can be over the limit). Unlimited by default. IMPORTANT: "
      "This limits the rate of uncompressed data. If rocksdb compressed data "
      "2X, the actual disk read rate will be around 1/2 of this limit.",
      SERVER,
      SettingsCategory::RocksDB);

  init(OPTNAME(sst_delete_bytes_per_sec),
       &sst_delete_bytes_per_sec,
       "0",
       nullptr,
       "ratelimit in bytes/sec on deletion of SST files per node; 0 for "
       "unlimited.",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(advise_random_on_open),
       &advise_random_on_open,
       "false",
       nullptr,
       "if true, will hint the underlying file system that the file access "
       "pattern is random when an SST file is opened",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(update_stats_on_db_open),
       &update_stats_on_db_open,
       "false",
       nullptr,
       "load stats from property blocks of several files when opening the "
       "database in order to optimize compaction decisions. May significantly "
       "impact the time needed to open the db.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(cache_index),
       &cache_index_,
       "false",
       nullptr,
       "put index and filter blocks in the block cache, allowing them to be "
       "evicted",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(force_no_compaction_optimizations),
       &force_no_compaction_optimizations_,
       "false",
       nullptr,
       "Kill switch for disabling uasge of kRemoveAndSkipUntil and "
       "los_whitelist in RocksDBCompactionFilter. There should be no reason to "
       "ever disable them unless there's some critical bug there. Please "
       "remove this option if it's at least 2017-07-01, and you haven't heard "
       "of any issues caused by compaction optimizations.",
       SERVER | DEPRECATED,
       SettingsCategory::RocksDB);
#ifdef LOGDEVICED_ROCKSDB_INSERT_HINT
  init(OPTNAME(enable_insert_hint),
       &enable_insert_hint_,
       "true",
       nullptr,
       "Enable rocksdb insert hint optimization. May reduce CPU usage for "
       "inserting keys into rocksdb, with small memory overhead.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);
#endif

#ifdef LOGDEVICED_ROCKSDB_CACHE_INDEX_HIGH_PRI
  init(OPTNAME(cache_index_with_high_priority),
       &cache_index_with_high_priority_,
       "false",
       nullptr,
       "Cache index and filter block in high pri pool of block cache, making "
       "them less likely to be evicted than data blocks.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(cache_high_pri_pool_ratio),
       &cache_high_pri_pool_ratio_,
       "0.0",
       [](double val) {
         if (val < 0.0 || val > 1.0) {
           throw boost::program_options::error(
               "value of --rocksdb-cache-high-pri-pool-ratio must be in the "
               "range [0.0, 1.0]");
         }
       },
       "Ratio of rocksdb block cache reserve for index and filter blocks, if "
       "--rocksdb-cache-index-with-high-priority is enabled.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);
#endif

#ifdef LOGDEVICED_ROCKSDB_READ_AMP_STATS
  init(OPTNAME(read_amp_bytes_per_bit),
       &read_amp_bytes_per_bit_,
       "32",
       nullptr,
       "If greater than 0, will create a bitmap to estimate rocksdb read "
       "amplification and expose the result through "
       "READ_AMP_ESTIMATE_USEFUL_BYTES and READ_AMP_TOTAL_READ_BYTES stats.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);
#endif

  init(OPTNAME(partitioned),
       &partitioned,
       "true",
       nullptr,
       "Deprecated. Setting this to false will store all log records in an "
       "unpartitioned column family, which is no longer supported.",
       SERVER | REQUIRES_RESTART | DEPRECATED,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_compactions_enabled),
       &partition_compactions_enabled,
       "true",
       nullptr,
       "perform background compactions for space reclamation in LogsDB",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_compaction_schedule),
       &partition_compaction_schedule,
       "auto",
       [](const std::string& val) -> folly::Optional<compaction_schedule_t> {
         folly::Optional<compaction_schedule_t> sched;
         int rv = parse_compaction_schedule(val, sched);
         if (rv != 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-compaction-schedule is invalid");
         }
         return sched;
       },
       "If set, indicate that the node wil run compaction. This is a list of "
       "durations indicating at what age to compact partition.  e.g. \"3d, "
       "7d\" means that each partition will be compacted twice: when all logs "
       "with backlog of up to 3 days are trimmed from it, and when all logs "
       "with backlog of up to 7 days are trimmed from it. \"auto\" (default) "
       "means use all backlog durations from config. \"disabled\" disables "
       "partition compactions.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(proactive_compaction_enabled),
       &proactive_compaction_enabled,
       "false",
       nullptr,
       "If set, indicate that we're going to proactively compact all "
       "partitions (besides two latest) that were never compacted. Compacting "
       "will be done in low priority background thread",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(disable_iterate_upper_bound),
       &disable_iterate_upper_bound,
       "false",
       nullptr,
       "disable iterate_upper_bound optimization in RocksDB",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(partition_duration),
       &partition_duration_,
       "15min",
       [](std::chrono::seconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-duration must be non-negative; " +
               std::to_string(val.count()) + "s given.");
         }
       },
       "create a new partition when the latest one becomes this old; 0 means "
       "infinity",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(unconfigured_log_trimming_grace_period),
       &unconfigured_log_trimming_grace_period_,
       "4d",
       [](std::chrono::seconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-unconfigured-log-trimming-grace-period must "
               "be non-negative; " +
               std::to_string(val.count()) + "s given.");
         }
       },
       "A grace period to delay trimming of records that are no longer in the "
       "config. The intent is to allow the oncall enough time to restore a "
       "backup of the config, in case the log(s) shouldn't have been removed.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_file_limit),
       &partition_file_limit_,
       "200",
       nullptr,
       "create a new partition when the number of level-0 files in the "
       "existing partition exceeds this threshold; 0 means infinity",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_file_num_threshold),
       &partition_partial_compaction_file_num_threshold_,
       "10",
       [](size_t val) {
         if (val < 2) {
           throw boost::program_options::error(
               "value of "
               "--rocksdb-partition-partial-compaction-file-num-threshold must "
               "be larger than 1");
         }
       },
       "don't consider file ranges for partial compactions (used during "
       "rebuilding) that are shorter than this",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_max_files),
       &partition_partial_compaction_max_files_,
       "100",
       nullptr,
       "the maximum number of files to compact in a single partial compaction",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_file_size_threshold),
       &partition_partial_compaction_file_size_threshold_,
       "50000000",
       nullptr,
       "the largest L0 files that it is beneficial to compact on their own. "
       "Note that we can still compact larger files than this if that enables "
       "usto compact a longer range of consecutive files.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_max_file_size),
       &partition_partial_compaction_max_file_size_,
       "0",
       nullptr,
       "the maximum size of an l0 file to consider for compaction. If not set, "
       "defaults to 2x "
       "--rocksdb-partition-partial-compaction-file-size-threshold",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_largest_file_share),
       &partition_partial_compaction_largest_file_share_,
       "0.7",
       [](double val) {
         if (val <= 0.0 || val > 1.0) {
           throw boost::program_options::error(
               "value of "
               "--rocksdb-partition-partial-compaction-largest-file-share must "
               "be in the range (0.0, 1.0]");
         }
       },
       "Partial compaction candidate file ranges that contain a file that "
       "comprises a larger propotion of the total file size in the range than "
       "this setting, will not be considered.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_max_num_per_loop),
       &partition_partial_compaction_max_num_per_loop_,
       "4",
       nullptr,
       "How many partial compactions to do in a row before re-checking if "
       "there are higher priority things to do (like dropping partitions). "
       "This value is not important; used for tests.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_partial_compaction_stall_trigger),
       &partition_partial_compaction_stall_trigger_,
       "50",
       nullptr,
       "Stall rebuilding writes if partial compactions are outstanding in at "
       "least this many partitions. 0 means infinity.",
       SERVER,
       SettingsCategory::LogsDB);

  init(
      OPTNAME(partition_count_soft_limit),
      &partition_count_soft_limit_,
      "2000",
      [](size_t val) {
        if (val == 0) {
          throw boost::program_options::error(
              "value of --rocksdb-partition-count-soft-limit must be positive");
        }
      },
      "If the number of partitions in a shard reaches this value, some "
      "measures will be taken to limit the creation of new partitions: "
      "partition age limit is tripled; partition file limit is ignored; "
      "partitions are not pre-created on startup; partitions are not prepended "
      "for records with small timestamp. This limit is intended mostly as "
      "protection against timestamp outliers: e.g. if we receive a STORE with "
      "zero timestamp, without this limit we would create over a million "
      "partitions to cover the time range from 1970 to now.",
      SERVER,
      SettingsCategory::LogsDB);

  init(OPTNAME(partition_timestamp_granularity),
       &partition_timestamp_granularity_,
       "5s",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-timestamp-granularity must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "minimum and maximum timestamps of a partition will be updated this "
       "often",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(new_partition_timestamp_margin),
       &new_partition_timestamp_margin_,
       "10s",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-new-partition-timestamp-margin must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "Newly created partitions will get starting timestamp `now + "
       "new_partition_timestamp_margin`. This absorbs the latency of creating "
       "partition and possible small clock skew between sequencer and storage "
       "node. If creating partition takes longer than that, or clock skew is "
       "greater than that, FindTime may be inaccurate. For reference, as of "
       "August 2017, creating a partition typically takes ~200-800ms on HDD "
       "with ~1100 existing partitions.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_hi_pri_check_period),
       &partition_hi_pri_check_period_,
       "2s",
       [](std::chrono::milliseconds val) {
         if (val.count() <= 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-hi-pri-check-period must be "
               "positive; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "how often a background thread will check if new partition should be "
       "created",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(partition_lo_pri_check_period),
       &partition_lo_pri_check_period_,
       "30s",
       [](std::chrono::milliseconds val) {
         if (val.count() <= 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-lo-pri-check-period must be "
               "positive; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "how often a background thread will trim logs and check if old "
       "partitions should be dropped or compacted, and do the drops and "
       "compactions",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(prepended_partition_min_lifetime),
       &prepended_partition_min_lifetime_,
       "300s",
       nullptr,
       "Avoid dropping newly prepended partitions for this amount of time.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(min_manual_flush_interval),
       &min_manual_flush_interval,
       "120s",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --min-manual-flush-interval must be non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "How often a background thread will flush buffered writes if either the "
       "data age, partition idle, or data amount triggers indicate a flush "
       "should occur. 0 disables all manual flushes",
       SERVER,
       SettingsCategory::RocksDB);

  // TODO (#10761838):
  //   This may be too big for rebuilding without WAL. When enabling
  //   rebuilding without WAL consider tweaking this option and/or skipping
  //   this trigger when WAL-less rebuilding is disabled.
  init(OPTNAME(partition_data_age_flush_trigger),
       &partition_data_age_flush_trigger,
       "600s",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-parittion-data-age-flush-trigger must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "Maximum wait after data are written before being flushed to stable "
       "storage. 0 disables the trigger.",
       SERVER,
       SettingsCategory::RocksDB);

  init(
      OPTNAME(partition_idle_flush_trigger),
      &partition_idle_flush_trigger,
      "300s",
      [](std::chrono::milliseconds val) {
        if (val.count() < 0) {
          throw boost::program_options::error(
              "value of --rocksdb-partition-idle-flush-trigger must be "
              "non-negative; " +
              std::to_string(val.count()) + "ms given.");
        }
      },
      "Maximum wait after writes to a time partition cease before any "
      "uncommitted data are flushed to stable storage. 0 disables the trigger.",
      SERVER,
      SettingsCategory::RocksDB);

  init(OPTNAME(partition_redirty_grace_period),
       &partition_redirty_grace_period,
       "5s",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-partition-redirty-grace-period must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "Minumum guaranteed time period for a node to re-dirty a partition "
       "after a MemTable is flushed without incurring a syncronous write "
       "penalty to update the partition dirty metadata.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(metadata_compaction_period),
       &metadata_compaction_period,
       "1h",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-metadata-compaction-period must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "Metadata column family will be compacted at least this often if it has "
       "more than one sst file. This is needed to avoid performance issues in "
       "rare cases. Full scenario: suppose all writes to this node stopped; "
       "eventually all logs will be fully trimmed, and logsdb directory will "
       "be emptied by deleting each key; these deletes will usually be flushed "
       "in sst files different than the ones where the original entries are; "
       "this makes iterator operations very expensive because merging iterator "
       "has to skip all these deleted entries in linear time; this is "
       "especially bad for findTime. If we compact every hour, this badness "
       "would last for at most an hour.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(directory_consistency_check_period),
       &directory_consistency_check_period,
       "5min",
       [](std::chrono::milliseconds val) {
         if (val.count() < 0) {
           throw boost::program_options::error(
               "value of --rocksdb-directory-consistency-check-period must be "
               "non-negative; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "LogsDB will compare all on-disk directory entries with the in-memory "
       "directory no more frequently than once per this period of time.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(free_disk_space_threshold_low),
       &free_disk_space_threshold_low,
       "0",
       [](const double& val) {
         if (val < 0.0 || val >= 1.0) {
           throw boost::program_options::error(
               "value of --rocksdb-free-disk-space-threshold-low must be in "
               "the range [0.0, 1.0)");
         }
       },
       "Keep free disk space above this fraction of disk size by marking node "
       "full if we exceed it, and let the sequencer initiate space-based "
       "retention. Only counts logdevice data, so storing other data on the "
       "disk could cause it to fill up even with space-based retention "
       "enabled. 0 means disabled.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(sbr_force),
       &sbr_force,
       "false",
       nullptr,
       "If true, space based retention will be done on the storage side, "
       "irrespective of whether sequencer initiated it or not. This is meant "
       "to make a node's storage available in case there is a critical bug.",
       SERVER | EXPERIMENTAL,
       SettingsCategory::LogsDB);

  init(OPTNAME(verify_checksum_during_store),
       &verify_checksum_during_store,
       "true",
       nullptr,
       "If true, verify checksum on every store. Reject store on failure and "
       "return E::CHECKSUM_MISMATCH.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(low_ioprio),
       &low_ioprio,
       "3,0",
       [](const std::string& val) -> folly::Optional<std::pair<int, int>> {
         folly::Optional<std::pair<int, int>> prio;
         if (parse_ioprio(val, &prio) != 0) {
           throw boost::program_options::error(
               "value of --rocksdb-low-ioprio must be of the form "
               "<class>,<data> e.g. 2,6; " +
               val + " given.");
         }
         return prio;
       },
       "IO priority to request for low-pri rocksdb threads. This works only if "
       "current IO scheduler supports IO priorities.See man ioprio_set for "
       "possible values. \"any\" or \"\" to keep the default. ",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::ResourceManagement);

  init(OPTNAME(worker_blocking_io_threshold),
       &worker_blocking_io_threshold_,
       "10ms",
       nullptr,
       "Log a message if a blocking file deletion takes at least this long on "
       "a Worker thread",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(stall_cache_ttl),
       &stall_cache_ttl_,
       "100ms",
       [](std::chrono::milliseconds val) {
         if (val.count() <= 0) {
           throw boost::program_options::error(
               "value of --rocksdb-stall-cache-ttl must be positive; " +
               std::to_string(val.count()) + "ms given.");
         }
       },
       "How often to re-check whether we should stall low-pri writes",
       SERVER,
       SettingsCategory::ResourceManagement);

  init(OPTNAME(allow_fallocate),
       &allow_fallocate,
       "true",
       nullptr,
       "If false, fallocate() calls are bypassed in rocksdb",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(auto_create_shards),
       &auto_create_shards,
       "false",
       nullptr,
       "Auto-create shard data directories if they do not exist",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::Storage);

  init(OPTNAME(background_wal_sync),
       &background_wal_sync,
       "true",
       nullptr,
       "Perform all RocksDB WAL syncs on a background thread rather than "
       "synchronously on a 'fast' storage thread executing the write.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(use_copyset_index),
       &use_copyset_index,
       "true",
       nullptr,
       "If set to true, the read path will use the copyset index to skip "
       "records that do not pass copyset filters. This greatly improves the "
       "efficiency of reading and rebuilding if records are large (1KB or "
       "bigger). For small records, the overhead of maintaining the copyset "
       "index negates the savings. **WARNING**: if this setting is enabled, "
       "records written without --write-sticky-copysets will be skipped by the "
       "copyset filter and will not be delivered to readers. Enable "
       "--write-sticky-copysets first and wait for all data records written "
       "before --write-sticky-copysets was enabled (if any) to be trimmed "
       "before enabling this setting.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::LogsDB);

  init(OPTNAME(read_find_time_index),
       &read_find_time_index,
       "false",
       nullptr,
       "If set to true, the operation findTime will use the findTime index to "
       "seek to the LSN instead of doing a binary search in the partition.",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(read_only),
       &read_only,
       "false",
       nullptr,
       "Open LogsDB in read-only mode",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::LogsDB);

  init(OPTNAME(flush_block_policy),
       &flush_block_policy_,
       "each_log",
       [](const std::string& val) {
         if (val == "default") {
           return RocksDBSettings::FlushBlockPolicyType::DEFAULT;
         } else if (val == "each_log") {
           return RocksDBSettings::FlushBlockPolicyType::EACH_LOG;
         } else if (val == "each_copyset") {
           return RocksDBSettings::FlushBlockPolicyType::EACH_COPYSET;
         } else {
           throw boost::program_options::error(
               "invalid value '" + val + "'for option --" +
               OPTNAME(flush_block_policy) +
               ". Expected 'default', 'each_log' or 'each_copyset'");
         }
       },
       "Controls how RocksDB splits SST file data into blocks. 'default' "
       "starts a new block when --rocksdb-block-size is reached. 'each_log', "
       "in addition to what 'default' does, starts a new block when log ID "
       "changes. 'each_copyset', in addition to what 'each_log' does, starts a "
       "new block when copyset changes. Both 'each_*' don't start a new block "
       "if current block is smaller than --rocksdb-min-block-size. 'each_log' "
       "should be safe to use in all cases. 'each_copyset' should only be used "
       "when sticky copysets are enabled with --write-sticky-copysets "
       "(otherwise it would start a block for almost every record).",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(track_iterator_versions),
       &track_iterator_versions,
       "false",
       nullptr,
       "Track iterator versions for the \"info iterators\" admin command",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(test_corrupt_stores),
       &test_corrupt_stores,
       "false",
       nullptr,
       "Used for testing only. If true, a node will report all stores it "
       "receives as corrupted.",
       SERVER,
       SettingsCategory::Testing);

  init(OPTNAME(bloom_bits_per_key),
       &bloom_bits_per_key_,
#ifdef LOGDEVICED_ROCKSDB_BLOOM_UNBROKEN
       "10",
       [](int val) {
         if (val < 0) {
           throw boost::program_options::error(
               "invalid value '" + std::to_string(val) + "'for option --" +
               OPTNAME(bloom_bits_per_key) + ". Expected nonnegative value.");
         }
       },
#else
       "0",
       [](int val) {
         if (val != 0) {
           throw boost::program_options::error(
               "bloom filters are broken in this version of rocksdb. Please "
               "use --" +
               std::string(OPTNAME(bloom_bits_per_key)) + "=0.");
         }
       },
#endif
       "Controls the size of bloom filters in sst files. Set to 0 to disable "
       "bloom filters. \"Key\" in the bloom filter is log ID and entry type "
       "(data record, CSI entry or findTime index entry). Iterators then use "
       "this information to skip files that don't contain any records of the "
       "requested log. The default value of 10 corresponds to false positive "
       "rate of ~1%. Note that LogsDB already skips partitions that don't have "
       "the requested logs, so bloom filters only help for somewhat bursty "
       "write patterns - when only a subset of files in a partition contain a "
       "given log. However, even if appends to a log are steady, sticky "
       "copysets may make the streams of STOREs to individual nodes bursty.",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(metadata_bloom_bits_per_key),
       &metadata_bloom_bits_per_key_,
       "0",
#ifdef LOGDEVICED_ROCKSDB_BLOOM_UNBROKEN
       [](int val) {
         if (val < 0) {
           throw boost::program_options::error(
               "invalid value '" + std::to_string(val) + "'for option --" +
               OPTNAME(metadata_bloom_bits_per_key) +
               ". Expected nonnegative value.");
         }
       },
#else
       [](int val) {
         if (val != 0) {
           throw boost::program_options::error(
               "bloom filters are broken in this version of rocksdb. Please "
               "use --" +
               std::string(OPTNAME(metadata_bloom_bits_per_key)) + "=0.");
         }
       },
#endif
       "Similar to --rocksdb-bloom-bits-per-key but for metadata column "
       "family. You probably don't want to enable this. This option is here "
       "just for completeness. It's not expected to have any positive effect "
       "since almost all reads from metadata column family bypass bloom "
       "filters (with total_order_seek = true).",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(bloom_block_based),
       &bloom_block_based_,
       "false",
       nullptr,
       "If true, rocksdb will use a separate bloom filter for each block of "
       "sst file. These small bloom filters will be at least 9 bytes each "
       "(even if bloom-bits-per-key is smaller). For data records, usually "
       "each block contains only one log, so the bloom filter size will be "
       "around max(72, bloom_bits_per_key) + 2 * bloom_bits_per_key  per log "
       "per sst (the \"2\" corresponds to CSI and findTime index entries; if "
       "one or both is disabled, it's correspondingly smaller).",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(partition_size_limit),
       &partition_size_limit_,
       "6G",
       parse_nonnegative<ssize_t>(),
       "create a new partition when size of the latest partition exceeds this "
       "threshold; 0 means infinity",
       SERVER,
       SettingsCategory::LogsDB);

  init(OPTNAME(bytes_per_sync),
       &bytes_per_sync,
       "1048576",
       parse_nonnegative<ssize_t>(),
       "when writing files (except WAL), sync once per this many bytes "
       "written. 0 turns off incremental syncing, the whole file will be "
       "synced after it's written",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(wal_bytes_per_sync),
       &wal_bytes_per_sync,
       "1M",
       parse_nonnegative<ssize_t>(),
       "when writing WAL, sync once per this many bytes written. 0 turns off "
       "incremental syncing",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(bytes_written_since_flush_trigger),
       &bytes_written_since_flush_trigger,
       "0",
       parse_nonnegative<ssize_t>(),
       "The maximum amount of buffered writes which will be accumulated before "
       "write data is flushed to stable storage. 0 disables the trigger",
       SERVER,
       SettingsCategory::RocksDB);

  init(OPTNAME(block_size),
       &block_size_,
       "500K",
       parse_positive<ssize_t>(),
       "approximate size of the uncompressed data block; rocksdb memory usage "
       "for index is around [total data size] / block_size * 50 bytes; on HDD "
       "consider using a much bigger value to keep memory usage reasonable",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(metadata_block_size),
       &metadata_block_size_,
       "0",
       parse_nonnegative<ssize_t>(),
       "approximate size of the uncompressed data block for metadata column "
       "family (if --rocksdb-partitioned); if zero, same as "
       "--rocksdb-block-size",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(min_block_size),
       &min_block_size_,
       "16384",
       parse_positive<ssize_t>(),
       "minimum size of the uncompressed data block; only used when "
       "--rocksdb-flush-block-policy is not default; on SSD consider reducing "
       "this value",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(cache_size),
       &cache_size_,
       "10G",
       parse_memory_budget(),
       "size of uncompressed RocksDB block cache",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(cache_numshardbits),
       &cache_numshardbits_,
       "4",
       parse_positive<ssize_t>(),
       "This setting is not important. Width in bits of the number of shards "
       "into which to partition the uncompressed block cache. See "
       "rocksdb/cache.h.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(metadata_cache_size),
       &metadata_cache_size_,
       "1G",
       parse_positive<ssize_t>(),
       "size of uncompressed RocksDB block cache for metadata",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(metadata_cache_numshardbits),
       &metadata_cache_numshardbits_,
       "4",
       parse_nonnegative<ssize_t>(),
       "This setting is not important. Width in bits of the number of shards "
       "into which to partition the uncompressed block cache for metadata. See "
       "rocksdb/cache.h.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(compressed_cache_size),
       &compressed_cache_size_,
       "0",
       parse_nonnegative<ssize_t>(),
       "size of compressed RocksDB block cache (0 to turn off)",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(compressed_cache_numshardbits),
       &compressed_cache_numshardbits_,
       "0",
       parse_nonnegative<ssize_t>(),
       "This setting is not important. Width in bits of the number of shards "
       "into which to partition the compressed block cache, if enabled. See "
       "rocksdb/cache.h.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(num_bg_threads_lo),
       &num_bg_threads_lo,
       "-1",
       parse_validate_lower_bound<ssize_t>(-1),
       "Number of low-priority rocksdb background threads to run. These "
       "threads are shared among all shards. If -1, num_shards * "
       "max_background_compactions is used.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(num_bg_threads_hi),
       &num_bg_threads_hi,
       "-1",
       parse_validate_lower_bound<ssize_t>(-1),
       "Number of high-priority rocksdb background threads to run. These "
       "threads are shared among all shards. If -1, num_shards * "
       "max_background_flushes is used.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(num_metadata_locks),
       &num_metadata_locks,
       "256",
       parse_positive<ssize_t>(),
       "number of lock stripes to use to perform LogsDB metadata updates",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::LogsDB);

  init(OPTNAME(skip_list_lookahead),
       &skip_list_lookahead,
       "3",
       parse_nonnegative<ssize_t>(),
       "number of keys to examine in the neighborhood of the current key when "
       "searching within a skiplist (0 to disable the optimization)",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_open_files),
       &max_open_files,
       "10000",
       parse_validate_lower_bound<ssize_t>(-1),
       "maximum number of concurrently open RocksDB files; -1 for unlimited",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(index_block_restart_interval),
       &index_block_restart_interval,
       "16",
       parse_positive<ssize_t>(),
       "Number of keys between restart points for prefix encoding of keys in "
       "index blocks.  Typically one of two values: 1 for no prefix encoding, "
       "16 for prefix encoding (smaller memory footprint of the index).",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(compaction_readahead_size),
       &compaction_readahead_size,
#ifdef LOGDEVICED_ROCKSDB_HAS_FILTER_V2
       "4096",
#else
       "4194304",
#endif
       parse_nonnegative<ssize_t>(),
       "if non-zero, perform reads of this size (in bytes) when doing "
       "compaction; big readahead can decrease efficiency of compactions that "
       "remove a lot of records (compaction skips trimmed records using seeks)",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(level0_file_num_compaction_trigger),
       &level0_file_num_compaction_trigger,
       "10",
       parse_positive<ssize_t>(),
       "trigger L0 compaction at this many L0 files. This applies to the "
       "unpartitioned and metadata column families only, not to LogsDB data "
       "partitions.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(level0_slowdown_writes_trigger),
       &level0_slowdown_writes_trigger,
       "25",
       parse_positive<ssize_t>(),
       "start throttling writers at this many L0 files. This applies to the "
       "unpartitioned and metadata column families only, not to LogsDB data "
       "partitions.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(level0_stop_writes_trigger),
       &level0_stop_writes_trigger,
       "30",
       parse_positive<ssize_t>(),
       "stop accepting writes (block writers) at this many L0 files. This "
       "applies to the unpartitioned and metadata column families only, not to "
       "LogsDB data partitions.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_background_compactions),
       &max_background_compactions,
       "2",
       parse_positive<ssize_t>(),
       "Maximum number of concurrent rocksdb-initiated background compactions "
       "per shard. Note that this value is not important since most "
       "compactions are not \"background\" as far as rocksdb is concerned. "
       "They're done from _logsdb_ thread and are limited to one per shard at "
       "a time, regardless of this option.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_background_flushes),
       &max_background_flushes,
       "15",
       parse_positive<ssize_t>(),
       "maximum number of concurrent background memtable flushes per shard. "
       "Flushes run on the rocksdb hipri thread pool",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_bytes_for_level_base),
       &max_bytes_for_level_base,
       "10G",
       parse_positive<ssize_t>(),
       "maximum combined data size for L1",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_bytes_for_level_multiplier),
       &max_bytes_for_level_multiplier,
       "8",
       parse_positive<ssize_t>(),
       "L_n -> L_n+1 data size multiplier",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(max_write_buffer_number),
       &max_write_buffer_number,
       "2",
       parse_positive<ssize_t>(),
       "maximum number of concurrent write buffers",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(num_levels),
       &num_levels,
       "1",
       parse_positive<ssize_t>(),
       "number of LSM-tree levels if level compaction is used",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(target_file_size_base),
       &target_file_size_base,
       "67108864",
       parse_positive<ssize_t>(),
       "target L1 file size for compaction",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(uc_min_merge_width),
       &uc_min_merge_width,
       "2",
       parse_positive<ssize_t>(),
       "minimum number of files in a single universal compaction run",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(uc_max_merge_width),
       &uc_max_merge_width,
       std::to_string(UINT_MAX).c_str(),
       parse_positive<ssize_t>(),
       "maximum number of files in a single universal compaction run",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(uc_max_size_amplification_percent),
       &uc_max_size_amplification_percent,
       "200",
       parse_positive<ssize_t>(),
       "target size amplification percentage for universal compaction",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(uc_size_ratio),
       &uc_size_ratio,
       "1M",
       parse_positive<ssize_t>(),
       "arg is a percentage. If the candidate set size for compaction is arg% "
       "smaller than the next file size, then include next file in the "
       "candidate set.",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(
      OPTNAME(write_buffer_size),
      &write_buffer_size,
      "100G", // >> memtable-size-per-node to make this irrelevant
      parse_memory_budget(),
      "When any RocksDB memtable ('write buffer') reaches this size it is made "
      "immitable, then flushed into a newly created L0 file. This setting may "
      "soon be superceded by a more dynamic --memtable-size-per-node limit. ",
      SERVER | REQUIRES_RESTART,
      SettingsCategory::RocksDB);

  init("rocksdb-max-total-wal-size",
       &max_total_wal_size,
       "2500M",
       parse_positive<ssize_t>(),
       "limit on the total size of active write-ahead logs for shard, enforced "
       "by rocksdb; when exceeded, memtables backed by oldest logs will "
       "automatically be flushed. You'll probably never need this because wal "
       "size limit is enforced through two other mechanisms already: manual "
       "flushes in logdevice (--rocksdb-*-flush-trigger options) and "
       "--rocksdb-db-write-buffer-size",
       SERVER | REQUIRES_RESTART | DEPRECATED,
       SettingsCategory::RocksDB);

  init(OPTNAME(db_write_buffer_size),
       &db_write_buffer_size,
       "0",
       [](const char* name, const std::string& value) {
         if (value == "0") {
           return (size_t)0;
         } else {
           return parse_memory_budget()(name, value);
         }
       },
       "Soft limit on the total size of memtables per shard; when exceeded, "
       "oldest memtables will automatically be flushed. This may soon be "
       "superseded by a more global --rocksdb-memtable-size-per-node limit "
       "that should be set to <num_shards> * what you'd set this to. ",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);

  init(OPTNAME(memtable_size_per_node),
       &memtable_size_per_node,
       "10G", // RocksDB targets 7/8 of this to avoid exceeding it
       parse_memory_budget(),
       "soft limit on the total size of memtables per node; when exceeded, "
       "oldest memtable in the shard whose growth took the total memory usage "
       "over the threshold will automatically be flushed. This is a soft limit "
       "in the sense that flushing may fall behind or freeing memory be "
       "delayed for other reasons, causing us to exceed the limit. "
       "--rocksdb-db-write-buffer-size overrides this if it is set, but it "
       "will be deprecated eventually.",
       SERVER | REQUIRES_RESTART | EXPERIMENTAL,
       SettingsCategory::RocksDB);

  init(OPTNAME(arena_block_size),
       &arena_block_size,
       "4194304",
       parse_positive<ssize_t>(),
       "granularity of memtable allocations",
       SERVER | REQUIRES_RESTART,
       SettingsCategory::RocksDB);
}

rocksdb::Options RocksDBSettings::toRocksDBOptions() const {
  rocksdb::Options options;
  options.compaction_style = compaction_style;
  options.compression = compression;
  options.access_hint_on_compaction_start = compaction_access_sequential
      ? rocksdb::DBOptions::SEQUENTIAL
      : rocksdb::DBOptions::NORMAL;
  options.advise_random_on_open = advise_random_on_open;
  options.skip_stats_update_on_db_open = !update_stats_on_db_open;
  options.allow_fallocate = allow_fallocate;
  options.max_open_files = max_open_files;
  options.bytes_per_sync = bytes_per_sync;
  options.wal_bytes_per_sync = wal_bytes_per_sync;
  options.compaction_readahead_size = compaction_readahead_size;
  options.level0_file_num_compaction_trigger =
      level0_file_num_compaction_trigger;
  options.level0_slowdown_writes_trigger = level0_slowdown_writes_trigger;
  options.level0_stop_writes_trigger = level0_stop_writes_trigger;
  options.max_background_compactions = max_background_compactions;
  options.max_background_flushes = max_background_flushes;
  options.max_bytes_for_level_base = max_bytes_for_level_base;
  options.max_bytes_for_level_multiplier = max_bytes_for_level_multiplier;
  options.max_write_buffer_number = max_write_buffer_number;
  options.num_levels = num_levels;
  options.target_file_size_base = target_file_size_base;
  options.write_buffer_size = write_buffer_size;
  options.max_total_wal_size = max_total_wal_size;
  options.db_write_buffer_size = db_write_buffer_size;
  options.arena_block_size = arena_block_size;

  options.compaction_options_universal.min_merge_width = uc_min_merge_width;
  options.compaction_options_universal.max_merge_width = uc_max_merge_width;
  options.compaction_options_universal.max_size_amplification_percent =
      uc_max_size_amplification_percent;
  options.compaction_options_universal.size_ratio = uc_size_ratio;

  return options;
}

RocksDBSettings RocksDBSettings::defaultTestSettings() {
  RocksDBSettings settings = create_default_settings<RocksDBSettings>();
  settings.allow_fallocate = false;
  settings.auto_create_shards = true;
  return settings;
}

}} // namespace facebook::logdevice
