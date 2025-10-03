//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/utilities/option_change_migration.h"

#include <memory>
#include <unordered_map>

#include "rocksdb/db.h"

namespace ROCKSDB_NAMESPACE {
namespace {
// Return a version of Options `opts` that allow us to open/write into a DB
// without triggering an automatic compaction or stalling. This is guaranteed
// by disabling automatic compactions and using huge values for stalling
// triggers.
Options GetNoCompactionOptions(const Options& opts) {
  Options ret_opts = opts;
  ret_opts.disable_auto_compactions = true;
  ret_opts.level0_slowdown_writes_trigger = 999999;
  ret_opts.level0_stop_writes_trigger = 999999;
  ret_opts.soft_pending_compaction_bytes_limit = 0;
  ret_opts.hard_pending_compaction_bytes_limit = 0;
  return ret_opts;
}

// For multi-CF: Return a version of DBOptions that allow us to open/write into
// a DB without triggering an automatic compaction or stalling. This is
// guaranteed by setting appropriate background job limits and disabling certain
// features.
DBOptions GetNoCompactionDBOptions(const DBOptions& opts) {
  DBOptions ret_opts = opts;
  ret_opts.avoid_flush_during_shutdown = true;
  ret_opts.max_background_compactions =
      1;  // Limit resources but allow compaction
  ret_opts.max_background_flushes = 1;
  return ret_opts;
}

// For multi-CF: Return a version of ColumnFamilyOptions that disable automatic
// compactions and write stalling. Used during migration to ensure we can
// perform manual compactions.
ColumnFamilyOptions GetNoCompactionCFOptions(const ColumnFamilyOptions& opts) {
  ColumnFamilyOptions ret_opts = opts;
  ret_opts.disable_auto_compactions = true;
  ret_opts.level0_slowdown_writes_trigger = 999999;
  ret_opts.level0_stop_writes_trigger = 999999;
  ret_opts.soft_pending_compaction_bytes_limit = 0;
  ret_opts.hard_pending_compaction_bytes_limit = 0;
  return ret_opts;
}

Status OpenDb(const Options& options, const std::string& dbname,
              std::unique_ptr<DB>* db) {
  db->reset();
  DB* tmpdb;
  Status s = DB::Open(options, dbname, &tmpdb);
  if (s.ok()) {
    db->reset(tmpdb);
  }
  return s;
}

// l0_file_size specifies size of file on L0. Files will be range partitioned
// after a full compaction so they are likely qualified to put on L0. If
// left as 0, the files are compacted in a single file and put to L0. Otherwise,
// will try to compact the files as size l0_file_size.
Status CompactToLevel(const Options& options, const std::string& dbname,
                      int dest_level, uint64_t l0_file_size, bool need_reopen) {
  std::unique_ptr<DB> db;
  Options no_compact_opts = GetNoCompactionOptions(options);
  if (dest_level == 0) {
    if (l0_file_size == 0) {
      // Single file.
      l0_file_size = 999999999999999;
    }
    // L0 has strict sequenceID requirements to files to it. It's safer
    // to only put one compacted file to there.
    // This is only used for converting to universal compaction with
    // only one level. In this case, compacting to one file is also
    // optimal.
    no_compact_opts.target_file_size_base = l0_file_size;
    no_compact_opts.max_compaction_bytes = l0_file_size;
  }
  Status s = OpenDb(no_compact_opts, dbname, &db);
  if (!s.ok()) {
    return s;
  }
  CompactRangeOptions cro;
  cro.change_level = true;
  cro.target_level = dest_level;
  if (dest_level == 0) {
    // cannot use kForceOptimized because the compaction is expected to
    // generate one output file
    cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  }
  s = db->CompactRange(cro, nullptr, nullptr);

  if (s.ok() && need_reopen) {
    // Need to restart DB to rewrite the manifest file.
    // In order to open a DB with specific num_levels, the manifest file should
    // contain no record that mentiones any level beyond num_levels. Issuing a
    // full compaction will move all the data to a level not exceeding
    // num_levels, but the manifest may still contain previous record mentioning
    // a higher level. Reopening the DB will force the manifest to be rewritten
    // so that those records will be cleared.
    db.reset();
    s = OpenDb(no_compact_opts, dbname, &db);
  }
  return s;
}

// Helper function for multi-CF: compact a column family to a specific level
Status CompactColumnFamilyToLevel(DB* db, ColumnFamilyHandle* cf_handle,
                                  int dest_level, uint64_t l0_file_size) {
  CompactRangeOptions cro;
  cro.change_level = true;
  cro.target_level = dest_level;

  if (dest_level == 0) {
    // Cannot use kForceOptimized because the compaction is expected to
    // generate one output file
    cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;

    if (l0_file_size > 0) {
      // For L0, we may want to control the output file size
      ColumnFamilyOptions cf_options = db->GetOptions(cf_handle);

      // Save original values to restore later
      uint64_t original_target_file_size = cf_options.target_file_size_base;
      uint64_t original_max_compaction_bytes = cf_options.max_compaction_bytes;

      // Set target file size for the compaction
      Status s = db->SetOptions(
          cf_handle, {{"target_file_size_base", std::to_string(l0_file_size)},
                      {"max_compaction_bytes", std::to_string(l0_file_size)}});

      if (!s.ok()) {
        return s;
      }

      // Perform the compaction
      s = db->CompactRange(cro, cf_handle, nullptr, nullptr);

      // Restore original options
      Status restore_status = db->SetOptions(
          cf_handle,
          {{"target_file_size_base", std::to_string(original_target_file_size)},
           {"max_compaction_bytes",
            std::to_string(original_max_compaction_bytes)}});

      if (s.ok() && !restore_status.ok()) {
        return restore_status;
      }

      return s;
    }
  }

  // Execute the actual compaction for this column family
  return db->CompactRange(cro, cf_handle, nullptr, nullptr);
}

Status MigrateToUniversal(std::string dbname, const Options& old_opts,
                          const Options& new_opts) {
  if (old_opts.num_levels <= new_opts.num_levels ||
      old_opts.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    return Status::OK();
  } else {
    bool need_compact = false;
    {
      std::unique_ptr<DB> db;
      Options opts = GetNoCompactionOptions(old_opts);
      Status s = OpenDb(opts, dbname, &db);
      if (!s.ok()) {
        return s;
      }
      ColumnFamilyMetaData metadata;
      db->GetColumnFamilyMetaData(&metadata);
      if (!metadata.levels.empty() &&
          metadata.levels.back().level >= new_opts.num_levels) {
        need_compact = true;
      }
    }
    if (need_compact) {
      return CompactToLevel(old_opts, dbname, new_opts.num_levels - 1,
                            /*l0_file_size=*/0, true);
    }
    return Status::OK();
  }
}

// Migrate a column family to Universal compaction style (for multi-CF support)
Status MigrateToUniversalCF(DB* db, ColumnFamilyHandle* cf_handle,
                            const ColumnFamilyOptions& old_opts,
                            const ColumnFamilyOptions& new_opts,
                            const ColumnFamilyMetaData& metadata) {
  if (old_opts.num_levels <= new_opts.num_levels ||
      old_opts.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    return Status::OK();
  }

  // Check if compaction is needed
  bool need_compact = false;
  if (!metadata.levels.empty() &&
      metadata.levels.back().level >= new_opts.num_levels) {
    need_compact = true;
  }

  if (need_compact) {
    return CompactColumnFamilyToLevel(db, cf_handle, new_opts.num_levels - 1,
                                      /*l0_file_size=*/0);
  }

  return Status::OK();
}

Status MigrateToLevelBase(std::string dbname, const Options& old_opts,
                          const Options& new_opts) {
  if (!new_opts.level_compaction_dynamic_level_bytes) {
    if (old_opts.num_levels == 1) {
      return Status::OK();
    }
    // Compact everything to level 1 to guarantee it can be safely opened.
    Options opts = old_opts;
    opts.target_file_size_base = new_opts.target_file_size_base;
    // Although sometimes we can open the DB with the new option without error,
    // We still want to compact the files to avoid the LSM tree to stuck
    // in bad shape. For example, if the user changed the level size
    // multiplier from 4 to 8, with the same data, we will have fewer
    // levels. Unless we issue a full comaction, the LSM tree may stuck
    // with more levels than needed and it won't recover automatically.
    return CompactToLevel(opts, dbname, 1, /*l0_file_size=*/0, true);
  } else {
    // Compact everything to the last level to guarantee it can be safely
    // opened.
    if (old_opts.num_levels == 1) {
      return Status::OK();
    } else if (new_opts.num_levels > old_opts.num_levels) {
      // Dynamic level mode requires data to be put in the last level first.
      return CompactToLevel(new_opts, dbname, new_opts.num_levels - 1,
                            /*l0_file_size=*/0, false);
    } else {
      Options opts = old_opts;
      opts.target_file_size_base = new_opts.target_file_size_base;
      return CompactToLevel(opts, dbname, new_opts.num_levels - 1,
                            /*l0_file_size=*/0, true);
    }
  }
}

// Migrate a column family to Level-based compaction style (for multi-CF
// support)
Status MigrateToLevelBaseCF(DB* db, ColumnFamilyHandle* cf_handle,
                            const ColumnFamilyOptions& old_opts,
                            const ColumnFamilyOptions& new_opts,
                            const ColumnFamilyMetaData& /* metadata */) {
  if (!new_opts.level_compaction_dynamic_level_bytes) {
    if (old_opts.num_levels == 1) {
      return Status::OK();
    }
    // Compact everything to level 1 to guarantee it can be safely opened
    return CompactColumnFamilyToLevel(db, cf_handle, 1, /*l0_file_size=*/0);
  } else {
    // Dynamic level bytes mode
    if (old_opts.num_levels == 1) {
      return Status::OK();
    } else if (new_opts.num_levels > old_opts.num_levels) {
      // Dynamic level mode requires data to be put in the last level first
      return CompactColumnFamilyToLevel(db, cf_handle, new_opts.num_levels - 1,
                                        /*l0_file_size=*/0);
    } else {
      return CompactColumnFamilyToLevel(db, cf_handle, new_opts.num_levels - 1,
                                        /*l0_file_size=*/0);
    }
  }
}

// Determine the migration strategy for a column family based on compaction
// style (for multi-CF support)
Status MigrateColumnFamilyOptions(DB* db, ColumnFamilyHandle* cf_handle,
                                  const ColumnFamilyOptions& old_opts,
                                  const ColumnFamilyOptions& new_opts,
                                  const ColumnFamilyMetaData& metadata) {
  if (old_opts.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    // LSM generated by FIFO compaction can be opened by any compaction
    return Status::OK();
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleUniversal) {
    return MigrateToUniversalCF(db, cf_handle, old_opts, new_opts, metadata);
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleLevel) {
    return MigrateToLevelBaseCF(db, cf_handle, old_opts, new_opts, metadata);
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleFIFO) {
    return CompactColumnFamilyToLevel(db, cf_handle, 0, 0 /* l0_file_size */);
  } else {
    return Status::NotSupported(
        "Do not know how to migrate to this compaction style for column "
        "family");
  }
}

// Prepare column family descriptors with no-compaction options (for multi-CF
// support)
std::vector<ColumnFamilyDescriptor> PrepareNoCompactionCFDescriptors(
    const std::vector<ColumnFamilyDescriptor>& cf_descs) {
  std::vector<ColumnFamilyDescriptor> result;
  result.reserve(cf_descs.size());

  for (const auto& cf_desc : cf_descs) {
    result.emplace_back(cf_desc.name,
                        GetNoCompactionCFOptions(cf_desc.options));
  }

  return result;
}

}  // namespace

Status OptionChangeMigration(std::string dbname, const Options& old_opts,
                             const Options& new_opts) {
  if (old_opts.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    // LSM generated by FIFO compaction can be opened by any compaction.
    return Status::OK();
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleUniversal) {
    return MigrateToUniversal(dbname, old_opts, new_opts);
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleLevel) {
    return MigrateToLevelBase(dbname, old_opts, new_opts);
  } else if (new_opts.compaction_style ==
             CompactionStyle::kCompactionStyleFIFO) {
    return CompactToLevel(old_opts, dbname, 0, 0 /* l0_file_size */, true);
  } else {
    return Status::NotSupported(
        "Do not how to migrate to this compaction style");
  }
}

// Implementation of multi-column family option migration
Status OptionChangeMigrationMultiCF(
    const std::string& dbname, const DBOptions& db_options_old,
    const DBOptions& db_options_new,
    const std::vector<ColumnFamilyDescriptor>& cf_descs_old,
    const std::vector<ColumnFamilyDescriptor>& cf_descs_new) {
  // Validate input
  if (cf_descs_old.size() != cf_descs_new.size()) {
    return Status::InvalidArgument(
        "Number of column families must match between old and new options");
  }

  // Map column family names to their corresponding options
  std::unordered_map<std::string, ColumnFamilyOptions> old_cf_opts;
  std::unordered_map<std::string, ColumnFamilyOptions> new_cf_opts;

  for (size_t i = 0; i < cf_descs_old.size(); i++) {
    old_cf_opts[cf_descs_old[i].name] = cf_descs_old[i].options;
    new_cf_opts[cf_descs_new[i].name] = cf_descs_new[i].options;

    // Ensure column family names match
    if (cf_descs_old[i].name != cf_descs_new[i].name) {
      return Status::InvalidArgument(
          "Column family names must match between old and new options");
    }
  }

  // Prepare column family descriptors with no-compaction options
  std::vector<ColumnFamilyDescriptor> no_compaction_cf_descs =
      PrepareNoCompactionCFDescriptors(cf_descs_old);

  // Open DB with all column families with read-write access
  // and prepare options for migration
  DB* db_raw = nullptr;
  std::vector<ColumnFamilyHandle*> handles;

  DBOptions db_options = GetNoCompactionDBOptions(db_options_old);
  Status s =
      DB::Open(db_options, dbname, no_compaction_cf_descs, &handles, &db_raw);

  if (!s.ok()) {
    return s;
  }

  std::unique_ptr<DB> db(db_raw);

  // Ensure handles get closed even on error
  std::vector<std::unique_ptr<ColumnFamilyHandle>> handle_guards;
  for (auto* handle : handles) {
    handle_guards.emplace_back(handle);
  }

  // Process each column family individually
  for (size_t i = 0; i < handles.size(); i++) {
    const std::string& cf_name = cf_descs_old[i].name;
    const ColumnFamilyOptions& old_cf_opt = old_cf_opts[cf_name];
    const ColumnFamilyOptions& new_cf_opt = new_cf_opts[cf_name];

    // Get metadata about this column family's current state
    ColumnFamilyMetaData metadata;
    db->GetColumnFamilyMetaData(handles[i], &metadata);

    // Determine migration strategy for this CF
    s = MigrateColumnFamilyOptions(db.get(), handles[i], old_cf_opt, new_cf_opt,
                                   metadata);

    if (!s.ok()) {
      // Provide context about which CF failed
      return Status::Corruption("Failed to migrate column family '" + cf_name +
                                "': " + s.ToString());
    }
  }

  // If we're changing the number of levels or other fundamental options,
  // we need to reopen the DB to rewrite the manifest
  bool need_reopen = false;
  for (size_t i = 0; i < cf_descs_old.size(); i++) {
    const ColumnFamilyOptions& old_cf_opt = cf_descs_old[i].options;
    const ColumnFamilyOptions& new_cf_opt = cf_descs_new[i].options;

    if (old_cf_opt.num_levels != new_cf_opt.num_levels ||
        old_cf_opt.compaction_style != new_cf_opt.compaction_style) {
      need_reopen = true;
      break;
    }
  }

  // Close all handles before closing DB
  for (auto& guard : handle_guards) {
    guard.reset();
  }

  // Close the database to ensure everything is flushed
  db.reset();

  // If necessary, reopen with new options to rewrite the manifest
  if (need_reopen) {
    // Use new options with auto-compaction disabled
    std::vector<ColumnFamilyDescriptor> reopen_cf_descs;
    reopen_cf_descs.reserve(cf_descs_new.size());

    for (const auto& cf_desc : cf_descs_new) {
      ColumnFamilyOptions cf_opts = GetNoCompactionCFOptions(cf_desc.options);
      reopen_cf_descs.emplace_back(cf_desc.name, cf_opts);
    }

    DBOptions reopen_db_options = GetNoCompactionDBOptions(db_options_new);
    DB* reopen_db_raw = nullptr;
    std::vector<ColumnFamilyHandle*> reopen_handles;

    s = DB::Open(reopen_db_options, dbname, reopen_cf_descs, &reopen_handles,
                 &reopen_db_raw);

    // Clean up handles and DB
    if (s.ok()) {
      for (auto* handle : reopen_handles) {
        reopen_db_raw->DestroyColumnFamilyHandle(handle);
      }
      delete reopen_db_raw;
    }
  }

  return s;
}

}  // namespace ROCKSDB_NAMESPACE
