//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "rocksdb/utilities/option_change_migration.h"

#include <cstdint>
#include <limits>
#include <set>

#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

class DBOptionChangeMigrationTests
    : public DBTestBase,
      public testing::WithParamInterface<
          std::tuple<int, int, bool, int, int, bool, uint64_t>> {
 public:
  DBOptionChangeMigrationTests()
      : DBTestBase("db_option_change_migration_test", /*env_do_fsync=*/true) {
    level1_ = std::get<0>(GetParam());
    compaction_style1_ = std::get<1>(GetParam());
    is_dynamic1_ = std::get<2>(GetParam());

    level2_ = std::get<3>(GetParam());
    compaction_style2_ = std::get<4>(GetParam());
    is_dynamic2_ = std::get<5>(GetParam());
    // This is set to be extremely large if not zero to avoid dropping any data
    // right after migration, which makes test verification difficult
    fifo_max_table_files_size_ = std::get<6>(GetParam());
  }

  // Required if inheriting from testing::WithParamInterface<>
  static void SetUpTestCase() {}
  static void TearDownTestCase() {}

  int level1_;
  int compaction_style1_;
  bool is_dynamic1_;

  int level2_;
  int compaction_style2_;
  bool is_dynamic2_;

  uint64_t fifo_max_table_files_size_;
};

TEST_P(DBOptionChangeMigrationTests, Migrate1) {
  Options old_options = CurrentOptions();
  old_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style1_);
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    old_options.level_compaction_dynamic_level_bytes = is_dynamic1_;
  }
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    old_options.max_open_files = -1;
  }
  old_options.level0_file_num_compaction_trigger = 3;
  old_options.write_buffer_size = 64 * 1024;
  old_options.target_file_size_base = 128 * 1024;
  // Make level target of L1, L2 to be 200KB and 600KB
  old_options.num_levels = level1_;
  old_options.max_bytes_for_level_multiplier = 3;
  old_options.max_bytes_for_level_base = 200 * 1024;

  Reopen(old_options);

  Random rnd(301);
  int key_idx = 0;

  // Generate at least 2MB of data
  for (int num = 0; num < 20; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  // Will make sure exactly those keys are in the DB after migration.
  std::set<std::string> keys;
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      keys.insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }
  Close();

  Options new_options = old_options;
  new_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style2_);
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    new_options.level_compaction_dynamic_level_bytes = is_dynamic2_;
  }
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    new_options.max_open_files = -1;
  }
  if (fifo_max_table_files_size_ != 0) {
    new_options.compaction_options_fifo.max_table_files_size =
        fifo_max_table_files_size_;
  }
  new_options.target_file_size_base = 256 * 1024;
  new_options.num_levels = level2_;
  new_options.max_bytes_for_level_base = 150 * 1024;
  new_options.max_bytes_for_level_multiplier = 4;
  ASSERT_OK(OptionChangeMigration(dbname_, old_options, new_options));
  Reopen(new_options);

  // Wait for compaction to finish and make sure it can reopen
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  Reopen(new_options);

  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (const std::string& key : keys) {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(key, it->key().ToString());
      it->Next();
    }
    ASSERT_TRUE(!it->Valid());
    ASSERT_OK(it->status());
  }
}

TEST_P(DBOptionChangeMigrationTests, Migrate2) {
  Options old_options = CurrentOptions();
  old_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style2_);
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    old_options.level_compaction_dynamic_level_bytes = is_dynamic2_;
  }
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    old_options.max_open_files = -1;
  }
  old_options.level0_file_num_compaction_trigger = 3;
  old_options.write_buffer_size = 64 * 1024;
  old_options.target_file_size_base = 128 * 1024;
  // Make level target of L1, L2 to be 200KB and 600KB
  old_options.num_levels = level2_;
  old_options.max_bytes_for_level_multiplier = 3;
  old_options.max_bytes_for_level_base = 200 * 1024;

  Reopen(old_options);

  Random rnd(301);
  int key_idx = 0;

  // Generate at least 2MB of data
  for (int num = 0; num < 20; num++) {
    GenerateNewFile(&rnd, &key_idx);
  }
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  // Will make sure exactly those keys are in the DB after migration.
  std::set<std::string> keys;
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      keys.insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  Close();

  Options new_options = old_options;
  new_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style1_);
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    new_options.level_compaction_dynamic_level_bytes = is_dynamic1_;
  }
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    new_options.max_open_files = -1;
  }
  if (fifo_max_table_files_size_ != 0) {
    new_options.compaction_options_fifo.max_table_files_size =
        fifo_max_table_files_size_;
  }
  new_options.target_file_size_base = 256 * 1024;
  new_options.num_levels = level1_;
  new_options.max_bytes_for_level_base = 150 * 1024;
  new_options.max_bytes_for_level_multiplier = 4;
  ASSERT_OK(OptionChangeMigration(dbname_, old_options, new_options));
  Reopen(new_options);
  // Wait for compaction to finish and make sure it can reopen
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  Reopen(new_options);

  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (const std::string& key : keys) {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(key, it->key().ToString());
      it->Next();
    }
    ASSERT_TRUE(!it->Valid());
    ASSERT_OK(it->status());
  }
}

TEST_P(DBOptionChangeMigrationTests, Migrate3) {
  Options old_options = CurrentOptions();
  old_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style1_);
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    old_options.level_compaction_dynamic_level_bytes = is_dynamic1_;
  }
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    old_options.max_open_files = -1;
  }
  old_options.level0_file_num_compaction_trigger = 3;
  old_options.write_buffer_size = 64 * 1024;
  old_options.target_file_size_base = 128 * 1024;
  // Make level target of L1, L2 to be 200KB and 600KB
  old_options.num_levels = level1_;
  old_options.max_bytes_for_level_multiplier = 3;
  old_options.max_bytes_for_level_base = 200 * 1024;

  Reopen(old_options);
  Random rnd(301);
  for (int num = 0; num < 20; num++) {
    for (int i = 0; i < 50; i++) {
      ASSERT_OK(Put(Key(num * 100 + i), rnd.RandomString(900)));
    }
    ASSERT_OK(Flush());
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
    if (num == 9) {
      // Issue a full compaction to generate some zero-out files
      CompactRangeOptions cro;
      cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
      ASSERT_OK(dbfull()->CompactRange(cro, nullptr, nullptr));
    }
  }
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  // Will make sure exactly those keys are in the DB after migration.
  std::set<std::string> keys;
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      keys.insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }
  Close();

  Options new_options = old_options;
  new_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style2_);
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    new_options.level_compaction_dynamic_level_bytes = is_dynamic2_;
  }
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    new_options.max_open_files = -1;
  }
  if (fifo_max_table_files_size_ != 0) {
    new_options.compaction_options_fifo.max_table_files_size =
        fifo_max_table_files_size_;
  }
  new_options.target_file_size_base = 256 * 1024;
  new_options.num_levels = level2_;
  new_options.max_bytes_for_level_base = 150 * 1024;
  new_options.max_bytes_for_level_multiplier = 4;
  ASSERT_OK(OptionChangeMigration(dbname_, old_options, new_options));
  Reopen(new_options);

  // Wait for compaction to finish and make sure it can reopen
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  Reopen(new_options);

  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (const std::string& key : keys) {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(key, it->key().ToString());
      it->Next();
    }
    ASSERT_TRUE(!it->Valid());
    ASSERT_OK(it->status());
  }
}

TEST_P(DBOptionChangeMigrationTests, Migrate4) {
  Options old_options = CurrentOptions();
  old_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style2_);
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    old_options.level_compaction_dynamic_level_bytes = is_dynamic2_;
  }
  if (old_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    old_options.max_open_files = -1;
  }
  old_options.level0_file_num_compaction_trigger = 3;
  old_options.write_buffer_size = 64 * 1024;
  old_options.target_file_size_base = 128 * 1024;
  // Make level target of L1, L2 to be 200KB and 600KB
  old_options.num_levels = level2_;
  old_options.max_bytes_for_level_multiplier = 3;
  old_options.max_bytes_for_level_base = 200 * 1024;

  Reopen(old_options);
  Random rnd(301);
  for (int num = 0; num < 20; num++) {
    for (int i = 0; i < 50; i++) {
      ASSERT_OK(Put(Key(num * 100 + i), rnd.RandomString(900)));
    }
    ASSERT_OK(Flush());
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
    if (num == 9) {
      // Issue a full compaction to generate some zero-out files
      CompactRangeOptions cro;
      cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
      ASSERT_OK(dbfull()->CompactRange(cro, nullptr, nullptr));
    }
  }
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  // Will make sure exactly those keys are in the DB after migration.
  std::set<std::string> keys;
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      keys.insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  Close();

  Options new_options = old_options;
  new_options.compaction_style =
      static_cast<CompactionStyle>(compaction_style1_);
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleLevel) {
    new_options.level_compaction_dynamic_level_bytes = is_dynamic1_;
  }
  if (new_options.compaction_style == CompactionStyle::kCompactionStyleFIFO) {
    new_options.max_open_files = -1;
  }
  if (fifo_max_table_files_size_ != 0) {
    new_options.compaction_options_fifo.max_table_files_size =
        fifo_max_table_files_size_;
  }
  new_options.target_file_size_base = 256 * 1024;
  new_options.num_levels = level1_;
  new_options.max_bytes_for_level_base = 150 * 1024;
  new_options.max_bytes_for_level_multiplier = 4;
  ASSERT_OK(OptionChangeMigration(dbname_, old_options, new_options));
  Reopen(new_options);
  // Wait for compaction to finish and make sure it can reopen
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  Reopen(new_options);

  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (const std::string& key : keys) {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(key, it->key().ToString());
      it->Next();
    }
    ASSERT_TRUE(!it->Valid());
    ASSERT_OK(it->status());
  }
}

INSTANTIATE_TEST_CASE_P(
    DBOptionChangeMigrationTests, DBOptionChangeMigrationTests,
    ::testing::Values(
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 0 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        true /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 0 /* new compaction style */,
                        true /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        true /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 0 /* new compaction style */,
                        false, 0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 0 /* new compaction style */,
                        true /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(1 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        1 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        true /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        true /* is dynamic leveling in old option */,
                        1 /* old num_levels */, 1 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(1 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 0 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(4 /* old num_levels */, 0 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        1 /* old num_levels */, 2 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 0 /* old compaction style */,
                        true /* is dynamic leveling in old option */,
                        2 /* old num_levels */, 2 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(3 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        3 /* old num_levels */, 2 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        0 /*fifo max_table_files_size*/),
        std::make_tuple(1 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 2 /* new compaction style */,
                        false /* is dynamic leveling in new option */, 0),
        std::make_tuple(
            4 /* old num_levels */, 0 /* old compaction style */,
            false /* is dynamic leveling in old option */,
            1 /* old num_levels */, 2 /* new compaction style */,
            false /* is dynamic leveling in new option */,
            std::numeric_limits<uint64_t>::max() /*fifo max_table_files_size*/),
        std::make_tuple(
            3 /* old num_levels */, 0 /* old compaction style */,
            true /* is dynamic leveling in old option */,
            2 /* old num_levels */, 2 /* new compaction style */,
            false /* is dynamic leveling in new option */,
            std::numeric_limits<uint64_t>::max() /*fifo max_table_files_size*/),
        std::make_tuple(
            3 /* old num_levels */, 1 /* old compaction style */,
            false /* is dynamic leveling in old option */,
            3 /* old num_levels */, 2 /* new compaction style */,
            false /* is dynamic leveling in new option */,
            std::numeric_limits<uint64_t>::max() /*fifo max_table_files_size*/),
        std::make_tuple(1 /* old num_levels */, 1 /* old compaction style */,
                        false /* is dynamic leveling in old option */,
                        4 /* old num_levels */, 2 /* new compaction style */,
                        false /* is dynamic leveling in new option */,
                        std::numeric_limits<
                            uint64_t>::max() /*fifo max_table_files_size*/)));

class DBOptionChangeMigrationTest : public DBTestBase {
 public:
  DBOptionChangeMigrationTest()
      : DBTestBase("db_option_change_migration_test2", /*env_do_fsync=*/true) {}
};

TEST_F(DBOptionChangeMigrationTest, CompactedSrcToUniversal) {
  Options old_options = CurrentOptions();
  old_options.compaction_style = CompactionStyle::kCompactionStyleLevel;
  old_options.max_compaction_bytes = 200 * 1024;
  old_options.level_compaction_dynamic_level_bytes = false;
  old_options.level0_file_num_compaction_trigger = 3;
  old_options.write_buffer_size = 64 * 1024;
  old_options.target_file_size_base = 128 * 1024;
  // Make level target of L1, L2 to be 200KB and 600KB
  old_options.num_levels = 4;
  old_options.max_bytes_for_level_multiplier = 3;
  old_options.max_bytes_for_level_base = 200 * 1024;

  Reopen(old_options);
  Random rnd(301);
  for (int num = 0; num < 20; num++) {
    for (int i = 0; i < 50; i++) {
      ASSERT_OK(Put(Key(num * 100 + i), rnd.RandomString(900)));
    }
  }
  ASSERT_OK(Flush());
  CompactRangeOptions cro;
  cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
  ASSERT_OK(dbfull()->CompactRange(cro, nullptr, nullptr));

  // Will make sure exactly those keys are in the DB after migration.
  std::set<std::string> keys;
  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      keys.insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  Close();

  Options new_options = old_options;
  new_options.compaction_style = CompactionStyle::kCompactionStyleUniversal;
  new_options.target_file_size_base = 256 * 1024;
  new_options.num_levels = 1;
  new_options.max_bytes_for_level_base = 150 * 1024;
  new_options.max_bytes_for_level_multiplier = 4;
  ASSERT_OK(OptionChangeMigration(dbname_, old_options, new_options));
  Reopen(new_options);
  // Wait for compaction to finish and make sure it can reopen
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable());
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  Reopen(new_options);

  {
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    it->SeekToFirst();
    for (const std::string& key : keys) {
      ASSERT_TRUE(it->Valid());
      ASSERT_EQ(key, it->key().ToString());
      it->Next();
    }
    ASSERT_TRUE(!it->Valid());
    ASSERT_OK(it->status());
  }
}

// Test class for multi-column family option migration
class DBOptionChangeMigrationMultiCFTest : public DBTestBase {
 public:
  DBOptionChangeMigrationMultiCFTest()
      : DBTestBase("db_option_change_migration_multi_cf_test",
                   /*env_do_fsync=*/true) {}

  void SetUp() override {
    DBTestBase::SetUp();
    // Create three column families
    Options options = CurrentOptions();
    CreateAndReopenWithCF({"cf1", "cf2"}, options);
  }
};

TEST_F(DBOptionChangeMigrationMultiCFTest, LevelToUniversal) {
  std::vector<std::string> cf_names = {"default", "cf1", "cf2"};

  // Setup original options - level-based compaction
  // Create fresh DBOptions instead of copying from database we'll close
  DBOptions db_options_old;
  db_options_old.create_if_missing = false;
  std::vector<ColumnFamilyDescriptor> cf_descs_old;

  for (int i = 0; i < static_cast<int>(handles_.size()); i++) {
    // Create fresh ColumnFamilyOptions instead of copying from existing
    // database
    ColumnFamilyOptions cf_opt;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleLevel;
    cf_opt.level0_file_num_compaction_trigger = 3;
    cf_opt.write_buffer_size = 64 * 1024;
    cf_opt.target_file_size_base = 128 * 1024;
    cf_opt.num_levels = 4;
    cf_opt.max_bytes_for_level_multiplier = 3;
    cf_opt.max_bytes_for_level_base = 200 * 1024;
    cf_opt.level_compaction_dynamic_level_bytes = false;

    cf_descs_old.emplace_back(cf_names[i], cf_opt);
  }

  // Close current DB before migration
  Close();

  // Open DB with old options to create some data
  Options old_options = CurrentOptions();
  old_options.create_if_missing = false;
  old_options.compaction_style = CompactionStyle::kCompactionStyleLevel;
  std::vector<ColumnFamilyHandle*> new_handles;
  DB* db;

  ASSERT_OK(DB::Open(db_options_old, dbname_, cf_descs_old, &new_handles, &db));

  // Generate data in each column family
  Random rnd(301);
  int key_idx = 0;

  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    for (int num = 0; num < 5; num++) {
      for (int i = 0; i < 50; i++) {
        ASSERT_OK(db->Put(WriteOptions(), new_handles[cf], Key(key_idx++),
                          rnd.RandomString(500)));
      }
      ASSERT_OK(db->Flush(FlushOptions(), new_handles[cf]));
    }
  }

  // Wait for compaction to finish
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    ASSERT_OK(
        static_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable(new_handles[cf]));
    ASSERT_OK(static_cast<DBImpl*>(db)->TEST_WaitForCompact());
  }

  // Will make sure exactly those keys are in the DB after migration.
  std::vector<std::set<std::string>> cf_keys(cf_names.size());
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      cf_keys[cf].insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;

  // Setup new options - universal compaction
  DBOptions db_options_new = db_options_old;
  std::vector<ColumnFamilyDescriptor> cf_descs_new;

  for (size_t i = 0; i < cf_names.size(); i++) {
    ColumnFamilyOptions cf_opt = cf_descs_old[i].options;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleUniversal;
    cf_opt.target_file_size_base = 256 * 1024;
    cf_opt.num_levels = 1;
    cf_opt.max_bytes_for_level_base = 150 * 1024;
    cf_opt.max_bytes_for_level_multiplier = 4;

    cf_descs_new.emplace_back(cf_names[i], cf_opt);
  }

  // Perform the multi-CF option migration
  ASSERT_OK(OptionChangeMigrationMultiCF(
      dbname_, db_options_old, db_options_new, cf_descs_old, cf_descs_new));

  // Reopen the DB with new options
  ASSERT_OK(DB::Open(db_options_new, dbname_, cf_descs_new, &new_handles, &db));

  // Verify data in each column family
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (const std::string& key : cf_keys[cf]) {
      ASSERT_TRUE(it->Valid()) << "CF " << cf << " missing key " << key;
      ASSERT_EQ(key, it->key().ToString()) << "CF " << cf << " key mismatch";
      it->Next();
    }
    ASSERT_TRUE(!it->Valid()) << "CF " << cf << " has extra keys";
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;
}

TEST_F(DBOptionChangeMigrationMultiCFTest, UniversalToLevel) {
  std::vector<std::string> cf_names = {"default", "cf1", "cf2"};

  // Setup original options - universal compaction
  // Create fresh DBOptions instead of copying from database we'll close
  DBOptions db_options_old;
  db_options_old.create_if_missing = false;
  std::vector<ColumnFamilyDescriptor> cf_descs_old;

  for (int i = 0; i < static_cast<int>(handles_.size()); i++) {
    // Create fresh ColumnFamilyOptions instead of copying from existing
    // database
    ColumnFamilyOptions cf_opt;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleUniversal;
    cf_opt.level0_file_num_compaction_trigger = 3;
    cf_opt.write_buffer_size = 64 * 1024;
    cf_opt.target_file_size_base = 128 * 1024;
    cf_opt.num_levels = 1;

    cf_descs_old.emplace_back(cf_names[i], cf_opt);
  }

  // Close current DB before migration
  Close();

  // Open DB with old options to create some data
  Options old_options = CurrentOptions();
  old_options.create_if_missing = false;
  old_options.compaction_style = CompactionStyle::kCompactionStyleUniversal;
  std::vector<ColumnFamilyHandle*> new_handles;
  DB* db;

  ASSERT_OK(DB::Open(db_options_old, dbname_, cf_descs_old, &new_handles, &db));

  // Generate data in each column family
  Random rnd(301);
  int key_idx = 0;

  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    for (int num = 0; num < 5; num++) {
      for (int i = 0; i < 50; i++) {
        ASSERT_OK(db->Put(WriteOptions(), new_handles[cf], Key(key_idx++),
                          rnd.RandomString(500)));
      }
      ASSERT_OK(db->Flush(FlushOptions(), new_handles[cf]));
    }
  }

  // Wait for compaction to finish
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    ASSERT_OK(
        static_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable(new_handles[cf]));
    ASSERT_OK(static_cast<DBImpl*>(db)->TEST_WaitForCompact());
  }

  // Will make sure exactly those keys are in the DB after migration.
  std::vector<std::set<std::string>> cf_keys(cf_names.size());
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      cf_keys[cf].insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;

  // Setup new options - level-based compaction
  DBOptions db_options_new = db_options_old;
  std::vector<ColumnFamilyDescriptor> cf_descs_new;

  for (size_t i = 0; i < cf_names.size(); i++) {
    ColumnFamilyOptions cf_opt = cf_descs_old[i].options;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleLevel;
    cf_opt.target_file_size_base = 256 * 1024;
    cf_opt.num_levels = 4;
    cf_opt.max_bytes_for_level_base = 150 * 1024;
    cf_opt.max_bytes_for_level_multiplier = 4;
    cf_opt.level_compaction_dynamic_level_bytes = true;

    cf_descs_new.emplace_back(cf_names[i], cf_opt);
  }

  // Perform the multi-CF option migration
  ASSERT_OK(OptionChangeMigrationMultiCF(
      dbname_, db_options_old, db_options_new, cf_descs_old, cf_descs_new));

  // Reopen the DB with new options
  ASSERT_OK(DB::Open(db_options_new, dbname_, cf_descs_new, &new_handles, &db));

  // Verify data in each column family
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (const std::string& key : cf_keys[cf]) {
      ASSERT_TRUE(it->Valid()) << "CF " << cf << " missing key " << key;
      ASSERT_EQ(key, it->key().ToString()) << "CF " << cf << " key mismatch";
      it->Next();
    }
    ASSERT_TRUE(!it->Valid()) << "CF " << cf << " has extra keys";
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;
}

TEST_F(DBOptionChangeMigrationMultiCFTest, MixedCompactionStyles) {
  std::vector<std::string> cf_names = {"default", "cf1", "cf2"};

  // Setup original options - mixed compaction styles
  // Create fresh DBOptions instead of copying from database we'll close
  DBOptions db_options_old;
  db_options_old.create_if_missing = false;
  std::vector<ColumnFamilyDescriptor> cf_descs_old;

  // Different compaction styles for different CFs
  CompactionStyle styles[3] = {CompactionStyle::kCompactionStyleLevel,
                               CompactionStyle::kCompactionStyleUniversal,
                               CompactionStyle::kCompactionStyleLevel};

  for (int i = 0; i < static_cast<int>(handles_.size()); i++) {
    // Create fresh ColumnFamilyOptions instead of copying from existing
    // database
    ColumnFamilyOptions cf_opt;
    cf_opt.compaction_style = styles[i];
    cf_opt.level0_file_num_compaction_trigger = 3;
    cf_opt.write_buffer_size = 64 * 1024;
    cf_opt.target_file_size_base = 128 * 1024;

    if (styles[i] == CompactionStyle::kCompactionStyleLevel) {
      cf_opt.num_levels = 4;
      cf_opt.max_bytes_for_level_multiplier = 3;
      cf_opt.max_bytes_for_level_base = 200 * 1024;
      cf_opt.level_compaction_dynamic_level_bytes = false;
    } else {
      cf_opt.num_levels = 1;
    }

    cf_descs_old.emplace_back(cf_names[i], cf_opt);
  }

  // Close current DB before migration
  Close();

  // Open DB with old options to create some data
  std::vector<ColumnFamilyHandle*> new_handles;
  DB* db;

  ASSERT_OK(DB::Open(db_options_old, dbname_, cf_descs_old, &new_handles, &db));

  // Generate data in each column family
  Random rnd(301);
  int key_idx = 0;

  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    for (int num = 0; num < 5; num++) {
      for (int i = 0; i < 50; i++) {
        ASSERT_OK(db->Put(WriteOptions(), new_handles[cf], Key(key_idx++),
                          rnd.RandomString(500)));
      }
      ASSERT_OK(db->Flush(FlushOptions(), new_handles[cf]));
    }
  }

  // Wait for compaction to finish
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    ASSERT_OK(
        static_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable(new_handles[cf]));
    ASSERT_OK(static_cast<DBImpl*>(db)->TEST_WaitForCompact());
  }

  // Will make sure exactly those keys are in the DB after migration.
  std::vector<std::set<std::string>> cf_keys(cf_names.size());
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (; it->Valid(); it->Next()) {
      cf_keys[cf].insert(it->key().ToString());
    }
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;

  // Setup new options - switch compaction styles
  DBOptions db_options_new = db_options_old;
  std::vector<ColumnFamilyDescriptor> cf_descs_new;

  // Flip compaction styles
  CompactionStyle new_styles[3] = {CompactionStyle::kCompactionStyleUniversal,
                                   CompactionStyle::kCompactionStyleLevel,
                                   CompactionStyle::kCompactionStyleFIFO};

  for (size_t i = 0; i < cf_names.size(); i++) {
    ColumnFamilyOptions cf_opt = cf_descs_old[i].options;
    cf_opt.compaction_style = new_styles[i];
    cf_opt.target_file_size_base = 256 * 1024;

    if (new_styles[i] == CompactionStyle::kCompactionStyleLevel) {
      cf_opt.num_levels = 4;
      cf_opt.max_bytes_for_level_base = 150 * 1024;
      cf_opt.max_bytes_for_level_multiplier = 4;
      cf_opt.level_compaction_dynamic_level_bytes = true;
    } else if (new_styles[i] == CompactionStyle::kCompactionStyleUniversal) {
      cf_opt.num_levels = 1;
    } else if (new_styles[i] == CompactionStyle::kCompactionStyleFIFO) {
      cf_opt.num_levels = 1;
      // Set to a large value to avoid data loss during test
      cf_opt.compaction_options_fifo.max_table_files_size =
          std::numeric_limits<uint64_t>::max();
    }

    cf_descs_new.emplace_back(cf_names[i], cf_opt);
  }

  // Perform the multi-CF option migration
  ASSERT_OK(OptionChangeMigrationMultiCF(
      dbname_, db_options_old, db_options_new, cf_descs_old, cf_descs_new));

  // Reopen the DB with new options
  ASSERT_OK(DB::Open(db_options_new, dbname_, cf_descs_new, &new_handles, &db));

  // Verify data in each column family
  for (size_t cf = 0; cf < cf_names.size(); cf++) {
    std::unique_ptr<Iterator> it(
        db->NewIterator(ReadOptions(), new_handles[cf]));
    it->SeekToFirst();
    for (const std::string& key : cf_keys[cf]) {
      ASSERT_TRUE(it->Valid()) << "CF " << cf << " missing key " << key;
      ASSERT_EQ(key, it->key().ToString()) << "CF " << cf << " key mismatch";
      it->Next();
    }
    ASSERT_TRUE(!it->Valid()) << "CF " << cf << " has extra keys";
    ASSERT_OK(it->status());
  }

  // Close DB and clean up handles
  for (auto* handle : new_handles) {
    delete handle;
  }
  delete db;
}

TEST_F(DBOptionChangeMigrationMultiCFTest, ErrorHandling) {
  std::vector<std::string> cf_names = {"default", "cf1", "cf2"};

  // Setup original options
  // Create fresh DBOptions instead of copying from database we'll close
  DBOptions db_options_old;
  db_options_old.create_if_missing = false;
  std::vector<ColumnFamilyDescriptor> cf_descs_old;

  for (int i = 0; i < static_cast<int>(handles_.size()); i++) {
    // Create fresh ColumnFamilyOptions instead of copying from existing
    // database
    ColumnFamilyOptions cf_opt;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleLevel;
    cf_descs_old.emplace_back(cf_names[i], cf_opt);
  }

  // Close current DB before migration
  Close();

  // Create new options with mismatched column family count
  DBOptions db_options_new = db_options_old;
  std::vector<ColumnFamilyDescriptor> cf_descs_new;

  // Use known size of cf_names instead of handles_.size() after Close()
  for (size_t i = 0; i < cf_names.size(); i++) {
    // Create fresh ColumnFamilyOptions instead of copying from cf_descs_old
    ColumnFamilyOptions cf_opt;
    cf_opt.compaction_style = CompactionStyle::kCompactionStyleUniversal;
    cf_descs_new.emplace_back(cf_names[i], cf_opt);
  }

  // Add an extra CF descriptor
  cf_descs_new.emplace_back("extra_cf", ColumnFamilyOptions());

  // Test with mismatched CF count - should return InvalidArgument
  Status s = OptionChangeMigrationMultiCF(
      dbname_, db_options_old, db_options_new, cf_descs_old, cf_descs_new);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_NE(s.ToString().find("Number of column families must match"),
            std::string::npos);

  // Fix the count but use a different name for one CF
  cf_descs_new.pop_back();  // Remove extra CF
  // Create fresh ColumnFamilyOptions to avoid corruption
  ColumnFamilyOptions fresh_cf_opt;
  fresh_cf_opt.compaction_style = CompactionStyle::kCompactionStyleUniversal;
  cf_descs_new[1] = ColumnFamilyDescriptor("different_name", fresh_cf_opt);

  // Test with mismatched CF names - should return InvalidArgument
  s = OptionChangeMigrationMultiCF(dbname_, db_options_old, db_options_new,
                                   cf_descs_old, cf_descs_new);
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_NE(s.ToString().find("Column family names must match"),
            std::string::npos);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
