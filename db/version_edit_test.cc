//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_edit.h"

#include "db/blob/blob_index.h"
#include "rocksdb/advanced_options.h"
#include "table/unique_id_impl.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/coding.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

static void TestEncodeDecode(const VersionEdit& edit) {
  // Encoding one `VersionEdit` and decoding it again should result in the
  // exact same `VersionEdit`. However, a special handling is applied to file
  // boundaries: `FileMetaData.smallest`, `FileMetaData.largest` when
  // user-defined timestamps should not be persisted. In that scenario, this
  // invariant does not hold. We disable this scenario in this util method to
  // enable all other test cases continue to verify this invariant, while the
  // special case is separately covered in test
  // `EncodeDecodeNewFile4HandleFileBoundary`.
  std::string encoded, encoded2;
  edit.EncodeTo(&encoded, 0 /* ts_sz */);
  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  parsed.EncodeTo(&encoded2, 0 /* ts_sz */);
  ASSERT_EQ(encoded, encoded2);
}

class VersionEditTest : public testing::Test {};

TEST_F(VersionEditTest, EncodeDecode) {
  static const uint64_t kBig = 1ull << 50;
  static const uint32_t kBig32Bit = 1ull << 30;

  VersionEdit edit;
  for (int i = 0; i < 4; i++) {
    TestEncodeDecode(edit);
    edit.AddFile(3, kBig + 300 + i, kBig32Bit + 400 + i, 0,
                 InternalKey("foo", kBig + 500 + i, kTypeValue),
                 InternalKey("zoo", kBig + 600 + i, kTypeDeletion),
                 kBig + 500 + i, kBig + 600 + i, false, Temperature::kUnknown,
                 kInvalidBlobFileNumber, 888, 678,
                 kBig + 300 + i /* epoch_number */, "234", "crc32c",
                 kNullUniqueId64x2, 0, 0, true);
    edit.DeleteFile(4, kBig + 700 + i);
  }

  edit.SetComparatorName("foo");
  edit.SetPersistUserDefinedTimestamps(true);
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, EncodeDecodeNewFile4) {
  static const uint64_t kBig = 1ull << 50;

  VersionEdit edit;
  edit.AddFile(3, 300, 3, 100, InternalKey("foo", kBig + 500, kTypeValue),
               InternalKey("zoo", kBig + 600, kTypeDeletion), kBig + 500,
               kBig + 600, true, Temperature::kUnknown, kInvalidBlobFileNumber,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               300 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, true);
  edit.AddFile(4, 301, 3, 100, InternalKey("foo", kBig + 501, kTypeValue),
               InternalKey("zoo", kBig + 601, kTypeDeletion), kBig + 501,
               kBig + 601, false, Temperature::kUnknown, kInvalidBlobFileNumber,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               301 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, false);
  edit.AddFile(5, 302, 0, 100, InternalKey("foo", kBig + 502, kTypeValue),
               InternalKey("zoo", kBig + 602, kTypeDeletion), kBig + 502,
               kBig + 602, true, Temperature::kUnknown, kInvalidBlobFileNumber,
               666, 888, 302 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, true);
  edit.AddFile(5, 303, 0, 100, InternalKey("foo", kBig + 503, kTypeBlobIndex),
               InternalKey("zoo", kBig + 603, kTypeBlobIndex), kBig + 503,
               kBig + 603, true, Temperature::kUnknown, 1001,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               303 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, true);

  edit.DeleteFile(4, 700);

  edit.SetComparatorName("foo");
  edit.SetPersistUserDefinedTimestamps(false);
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);
  TestEncodeDecode(edit);

  std::string encoded, encoded2;
  edit.EncodeTo(&encoded, 0 /* ts_sz */);
  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  auto& new_files = parsed.GetNewFiles();
  ASSERT_TRUE(new_files[0].second.marked_for_compaction);
  ASSERT_FALSE(new_files[1].second.marked_for_compaction);
  ASSERT_TRUE(new_files[2].second.marked_for_compaction);
  ASSERT_TRUE(new_files[3].second.marked_for_compaction);
  ASSERT_EQ(3u, new_files[0].second.fd.GetPathId());
  ASSERT_EQ(3u, new_files[1].second.fd.GetPathId());
  ASSERT_EQ(0u, new_files[2].second.fd.GetPathId());
  ASSERT_EQ(0u, new_files[3].second.fd.GetPathId());
  ASSERT_EQ(kInvalidBlobFileNumber,
            new_files[0].second.oldest_blob_file_number);
  ASSERT_EQ(kInvalidBlobFileNumber,
            new_files[1].second.oldest_blob_file_number);
  ASSERT_EQ(kInvalidBlobFileNumber,
            new_files[2].second.oldest_blob_file_number);
  ASSERT_EQ(1001, new_files[3].second.oldest_blob_file_number);
  ASSERT_TRUE(new_files[0].second.user_defined_timestamps_persisted);
  ASSERT_FALSE(new_files[1].second.user_defined_timestamps_persisted);
  ASSERT_TRUE(new_files[2].second.user_defined_timestamps_persisted);
  ASSERT_TRUE(new_files[3].second.user_defined_timestamps_persisted);
  ASSERT_FALSE(parsed.GetPersistUserDefinedTimestamps());
}

TEST_F(VersionEditTest, EncodeDecodeNewFile4HandleFileBoundary) {
  static const uint64_t kBig = 1ull << 50;
  size_t ts_sz = 16;
  static std::string min_ts(ts_sz, static_cast<unsigned char>(0));
  VersionEdit edit;
  std::string smallest = "foo";
  std::string largest = "zoo";
  // In real manifest writing scenarios, one `VersionEdit` should not contain
  // files with different `user_defined_timestamps_persisted` flag value.
  // This is just for testing file boundaries handling w.r.t persisting user
  // defined timestamps during `VersionEdit` encoding.
  edit.AddFile(
      3, 300, 3, 100, InternalKey(smallest + min_ts, kBig + 500, kTypeValue),
      InternalKey(largest + min_ts, kBig + 600, kTypeDeletion), kBig + 500,
      kBig + 600, true, Temperature::kUnknown, kInvalidBlobFileNumber,
      kUnknownOldestAncesterTime, kUnknownFileCreationTime,
      300 /* epoch_number */, kUnknownFileChecksum,
      kUnknownFileChecksumFuncName, kNullUniqueId64x2,
      0 /* compensated_range_deletion_size */, 0 /* tail_size */,
      false /* user_defined_timestamps_persisted */);
  edit.AddFile(3, 300, 3, 100,
               InternalKey(smallest + min_ts, kBig + 500, kTypeValue),
               InternalKey(largest + min_ts, kBig + 600, kTypeDeletion),
               kBig + 500, kBig + 600, true, Temperature::kUnknown,
               kInvalidBlobFileNumber, kUnknownOldestAncesterTime,
               kUnknownFileCreationTime, 300 /* epoch_number */,
               kUnknownFileChecksum, kUnknownFileChecksumFuncName,
               kNullUniqueId64x2, 0 /* compensated_range_deletion_size */,
               0 /* tail_size */, true /* user_defined_timestamps_persisted */);

  std::string encoded;
  edit.EncodeTo(&encoded, ts_sz);
  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  auto& new_files = parsed.GetNewFiles();
  ASSERT_TRUE(new_files.size() == 2);
  ASSERT_FALSE(new_files[0].second.user_defined_timestamps_persisted);
  // First file's boundaries do not contain user-defined timestamps.
  ASSERT_EQ(InternalKey(smallest, kBig + 500, kTypeValue).Encode(),
            new_files[0].second.smallest.Encode());
  ASSERT_EQ(InternalKey(largest, kBig + 600, kTypeDeletion).Encode(),
            new_files[0].second.largest.Encode());
  ASSERT_TRUE(new_files[1].second.user_defined_timestamps_persisted);
  // Second file's boundaries contain user-defined timestamps.
  ASSERT_EQ(InternalKey(smallest + min_ts, kBig + 500, kTypeValue).Encode(),
            new_files[1].second.smallest.Encode());
  ASSERT_EQ(InternalKey(largest + min_ts, kBig + 600, kTypeDeletion).Encode(),
            new_files[1].second.largest.Encode());
}

TEST_F(VersionEditTest, ForwardCompatibleNewFile4) {
  static const uint64_t kBig = 1ull << 50;
  VersionEdit edit;
  edit.AddFile(3, 300, 3, 100, InternalKey("foo", kBig + 500, kTypeValue),
               InternalKey("zoo", kBig + 600, kTypeDeletion), kBig + 500,
               kBig + 600, true, Temperature::kUnknown, kInvalidBlobFileNumber,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               300 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, true);
  edit.AddFile(4, 301, 3, 100, InternalKey("foo", kBig + 501, kTypeValue),
               InternalKey("zoo", kBig + 601, kTypeDeletion), kBig + 501,
               kBig + 601, false, Temperature::kUnknown, kInvalidBlobFileNumber,
               686, 868, 301 /* epoch_number */, "234", "crc32c",
               kNullUniqueId64x2, 0, 0, true);
  edit.DeleteFile(4, 700);

  edit.SetComparatorName("foo");
  edit.SetPersistUserDefinedTimestamps(true);
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);

  std::string encoded;

  // Call back function to add extra customized builds.
  bool first = true;
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "VersionEdit::EncodeTo:NewFile4:CustomizeFields", [&](void* arg) {
        std::string* str = reinterpret_cast<std::string*>(arg);
        PutVarint32(str, 33);
        const std::string str1 = "random_string";
        PutLengthPrefixedSlice(str, str1);
        if (first) {
          first = false;
          PutVarint32(str, 22);
          const std::string str2 = "s";
          PutLengthPrefixedSlice(str, str2);
        }
      });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();
  edit.EncodeTo(&encoded, 0 /* ts_sz */);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();

  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_TRUE(!first);
  auto& new_files = parsed.GetNewFiles();
  ASSERT_TRUE(new_files[0].second.marked_for_compaction);
  ASSERT_TRUE(!new_files[1].second.marked_for_compaction);
  ASSERT_EQ(3u, new_files[0].second.fd.GetPathId());
  ASSERT_EQ(3u, new_files[1].second.fd.GetPathId());
  ASSERT_EQ(1u, parsed.GetDeletedFiles().size());
  ASSERT_TRUE(parsed.GetPersistUserDefinedTimestamps());
}

TEST_F(VersionEditTest, NewFile4NotSupportedField) {
  static const uint64_t kBig = 1ull << 50;
  VersionEdit edit;
  edit.AddFile(3, 300, 3, 100, InternalKey("foo", kBig + 500, kTypeValue),
               InternalKey("zoo", kBig + 600, kTypeDeletion), kBig + 500,
               kBig + 600, true, Temperature::kUnknown, kInvalidBlobFileNumber,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               300 /* epoch_number */, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, false);

  edit.SetComparatorName("foo");
  edit.SetPersistUserDefinedTimestamps(false);
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);

  std::string encoded;

  // Call back function to add extra customized builds.
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "VersionEdit::EncodeTo:NewFile4:CustomizeFields", [&](void* arg) {
        std::string* str = reinterpret_cast<std::string*>(arg);
        const std::string str1 = "s";
        PutLengthPrefixedSlice(str, str1);
      });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();
  edit.EncodeTo(&encoded, 0 /* ts_sz */);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();

  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_NOK(s);
}

TEST_F(VersionEditTest, EncodeEmptyFile) {
  VersionEdit edit;
  edit.AddFile(0, 0, 0, 0, InternalKey(), InternalKey(), 0, 0, false,
               Temperature::kUnknown, kInvalidBlobFileNumber,
               kUnknownOldestAncesterTime, kUnknownFileCreationTime,
               1 /*epoch_number*/, kUnknownFileChecksum,
               kUnknownFileChecksumFuncName, kNullUniqueId64x2, 0, 0, true);
  std::string buffer;
  ASSERT_TRUE(!edit.EncodeTo(&buffer, 0 /* ts_sz */));
}

TEST_F(VersionEditTest, ColumnFamilyTest) {
  VersionEdit edit;
  edit.SetColumnFamily(2);
  edit.AddColumnFamily("column_family");
  edit.SetMaxColumnFamily(5);
  TestEncodeDecode(edit);

  edit.Clear();
  edit.SetColumnFamily(3);
  edit.DropColumnFamily();
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, MinLogNumberToKeep) {
  VersionEdit edit;
  edit.SetMinLogNumberToKeep(13);
  TestEncodeDecode(edit);

  edit.Clear();
  edit.SetMinLogNumberToKeep(23);
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, AtomicGroupTest) {
  VersionEdit edit;
  edit.MarkAtomicGroup(1);
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, IgnorableField) {
  VersionEdit ve;
  std::string encoded;

  // Size of ignorable field is too large
  PutVarint32Varint64(&encoded, 2 /* kLogNumber */, 66);
  // This is a customized ignorable tag
  PutVarint32Varint64(&encoded,
                      0x2710 /* A field with kTagSafeIgnoreMask set */,
                      5 /* fieldlength 5 */);
  encoded += "abc";  // Only fills 3 bytes,
  ASSERT_NOK(ve.DecodeFrom(encoded));

  encoded.clear();
  // Error when seeing unidentified tag that is not ignorable
  PutVarint32Varint64(&encoded, 2 /* kLogNumber */, 66);
  // This is a customized ignorable tag
  PutVarint32Varint64(&encoded, 666 /* A field with kTagSafeIgnoreMask unset */,
                      3 /* fieldlength 3 */);
  encoded += "abc";  //  Fill 3 bytes
  PutVarint32Varint64(&encoded, 3 /* next file number */, 88);
  ASSERT_NOK(ve.DecodeFrom(encoded));

  // Safely ignore an identified but safely ignorable entry
  encoded.clear();
  PutVarint32Varint64(&encoded, 2 /* kLogNumber */, 66);
  // This is a customized ignorable tag
  PutVarint32Varint64(&encoded,
                      0x2710 /* A field with kTagSafeIgnoreMask set */,
                      3 /* fieldlength 3 */);
  encoded += "abc";  //  Fill 3 bytes
  PutVarint32Varint64(&encoded, 3 /* kNextFileNumber */, 88);

  ASSERT_OK(ve.DecodeFrom(encoded));

  ASSERT_TRUE(ve.HasLogNumber());
  ASSERT_TRUE(ve.HasNextFile());
  ASSERT_EQ(66, ve.GetLogNumber());
  ASSERT_EQ(88, ve.GetNextFile());
}

TEST_F(VersionEditTest, DbId) {
  VersionEdit edit;
  edit.SetDBId("ab34-cd12-435f-er00");
  TestEncodeDecode(edit);

  edit.Clear();
  edit.SetDBId("34ba-cd12-435f-er01");
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, BlobFileAdditionAndGarbage) {
  VersionEdit edit;

  const std::string checksum_method_prefix = "Hash";
  const std::string checksum_value_prefix = "Value";

  for (uint64_t blob_file_number = 1; blob_file_number <= 10;
       ++blob_file_number) {
    const uint64_t total_blob_count = blob_file_number << 10;
    const uint64_t total_blob_bytes = blob_file_number << 20;

    std::string checksum_method(checksum_method_prefix);
    AppendNumberTo(&checksum_method, blob_file_number);

    std::string checksum_value(checksum_value_prefix);
    AppendNumberTo(&checksum_value, blob_file_number);

    edit.AddBlobFile(blob_file_number, total_blob_count, total_blob_bytes,
                     checksum_method, checksum_value);

    const uint64_t garbage_blob_count = total_blob_count >> 2;
    const uint64_t garbage_blob_bytes = total_blob_bytes >> 1;

    edit.AddBlobFileGarbage(blob_file_number, garbage_blob_count,
                            garbage_blob_bytes);
  }

  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, AddWalEncodeDecode) {
  VersionEdit edit;
  for (uint64_t log_number = 1; log_number <= 20; log_number++) {
    WalMetadata meta;
    bool has_size = rand() % 2 == 0;
    if (has_size) {
      meta.SetSyncedSizeInBytes(rand() % 1000);
    }
    edit.AddWal(log_number, meta);
  }
  TestEncodeDecode(edit);
}

static std::string PrefixEncodedWalAdditionWithLength(
    const std::string& encoded) {
  std::string ret;
  PutVarint32(&ret, Tag::kWalAddition2);
  PutLengthPrefixedSlice(&ret, encoded);
  return ret;
}

TEST_F(VersionEditTest, AddWalDecodeBadLogNumber) {
  std::string encoded;

  {
    // No log number.
    std::string encoded_edit = PrefixEncodedWalAdditionWithLength(encoded);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    ASSERT_TRUE(s.ToString().find("Error decoding WAL log number") !=
                std::string::npos)
        << s.ToString();
  }

  {
    // log number should be varint64,
    // but we only encode 128 which is not a valid representation of varint64.
    char c = 0;
    unsigned char* ptr = reinterpret_cast<unsigned char*>(&c);
    *ptr = 128;
    encoded.append(1, c);

    std::string encoded_edit = PrefixEncodedWalAdditionWithLength(encoded);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    ASSERT_TRUE(s.ToString().find("Error decoding WAL log number") !=
                std::string::npos)
        << s.ToString();
  }
}

TEST_F(VersionEditTest, AddWalDecodeBadTag) {
  constexpr WalNumber kLogNumber = 100;
  constexpr uint64_t kSizeInBytes = 100;

  std::string encoded;
  PutVarint64(&encoded, kLogNumber);

  {
    // No tag.
    std::string encoded_edit = PrefixEncodedWalAdditionWithLength(encoded);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    ASSERT_TRUE(s.ToString().find("Error decoding tag") != std::string::npos)
        << s.ToString();
  }

  {
    // Only has size tag, no terminate tag.
    std::string encoded_with_size = encoded;
    PutVarint32(&encoded_with_size,
                static_cast<uint32_t>(WalAdditionTag::kSyncedSize));
    PutVarint64(&encoded_with_size, kSizeInBytes);

    std::string encoded_edit =
        PrefixEncodedWalAdditionWithLength(encoded_with_size);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    ASSERT_TRUE(s.ToString().find("Error decoding tag") != std::string::npos)
        << s.ToString();
  }

  {
    // Only has terminate tag.
    std::string encoded_with_terminate = encoded;
    PutVarint32(&encoded_with_terminate,
                static_cast<uint32_t>(WalAdditionTag::kTerminate));

    std::string encoded_edit =
        PrefixEncodedWalAdditionWithLength(encoded_with_terminate);
    VersionEdit edit;
    ASSERT_OK(edit.DecodeFrom(encoded_edit));
    auto& wal_addition = edit.GetWalAdditions()[0];
    ASSERT_EQ(wal_addition.GetLogNumber(), kLogNumber);
    ASSERT_FALSE(wal_addition.GetMetadata().HasSyncedSize());
  }
}

TEST_F(VersionEditTest, AddWalDecodeNoSize) {
  constexpr WalNumber kLogNumber = 100;

  std::string encoded;
  PutVarint64(&encoded, kLogNumber);
  PutVarint32(&encoded, static_cast<uint32_t>(WalAdditionTag::kSyncedSize));
  // No real size after the size tag.

  {
    // Without terminate tag.
    std::string encoded_edit = PrefixEncodedWalAdditionWithLength(encoded);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    ASSERT_TRUE(s.ToString().find("Error decoding WAL file size") !=
                std::string::npos)
        << s.ToString();
  }

  {
    // With terminate tag.
    PutVarint32(&encoded, static_cast<uint32_t>(WalAdditionTag::kTerminate));

    std::string encoded_edit = PrefixEncodedWalAdditionWithLength(encoded);
    VersionEdit edit;
    Status s = edit.DecodeFrom(encoded_edit);
    ASSERT_TRUE(s.IsCorruption());
    // The terminate tag is misunderstood as the size.
    ASSERT_TRUE(s.ToString().find("Error decoding tag") != std::string::npos)
        << s.ToString();
  }
}

TEST_F(VersionEditTest, AddWalDebug) {
  constexpr int n = 2;
  constexpr std::array<uint64_t, n> kLogNumbers{{10, 20}};
  constexpr std::array<uint64_t, n> kSizeInBytes{{100, 200}};

  VersionEdit edit;
  for (int i = 0; i < n; i++) {
    edit.AddWal(kLogNumbers[i], WalMetadata(kSizeInBytes[i]));
  }

  const WalAdditions& wals = edit.GetWalAdditions();

  ASSERT_TRUE(edit.IsWalAddition());
  ASSERT_EQ(wals.size(), n);
  for (int i = 0; i < n; i++) {
    const WalAddition& wal = wals[i];
    ASSERT_EQ(wal.GetLogNumber(), kLogNumbers[i]);
    ASSERT_EQ(wal.GetMetadata().GetSyncedSizeInBytes(), kSizeInBytes[i]);
  }

  std::string expected_str = "VersionEdit {\n";
  for (int i = 0; i < n; i++) {
    std::stringstream ss;
    ss << "  WalAddition: log_number: " << kLogNumbers[i]
       << " synced_size_in_bytes: " << kSizeInBytes[i] << "\n";
    expected_str += ss.str();
  }
  expected_str += "  ColumnFamily: 0\n}\n";
  ASSERT_EQ(edit.DebugString(true), expected_str);

  std::string expected_json = "{\"EditNumber\": 4, \"WalAdditions\": [";
  for (int i = 0; i < n; i++) {
    std::stringstream ss;
    ss << "{\"LogNumber\": " << kLogNumbers[i] << ", "
       << "\"SyncedSizeInBytes\": " << kSizeInBytes[i] << "}";
    if (i < n - 1) {
      ss << ", ";
    }
    expected_json += ss.str();
  }
  expected_json += "], \"ColumnFamily\": 0}";
  ASSERT_EQ(edit.DebugJSON(4, true), expected_json);
}

TEST_F(VersionEditTest, DeleteWalEncodeDecode) {
  VersionEdit edit;
  edit.DeleteWalsBefore(rand() % 100);
  TestEncodeDecode(edit);
}

TEST_F(VersionEditTest, DeleteWalDebug) {
  constexpr int n = 2;
  constexpr std::array<uint64_t, n> kLogNumbers{{10, 20}};

  VersionEdit edit;
  edit.DeleteWalsBefore(kLogNumbers[n - 1]);

  const WalDeletion& wal = edit.GetWalDeletion();

  ASSERT_TRUE(edit.IsWalDeletion());
  ASSERT_EQ(wal.GetLogNumber(), kLogNumbers[n - 1]);

  std::string expected_str = "VersionEdit {\n";
  {
    std::stringstream ss;
    ss << "  WalDeletion: log_number: " << kLogNumbers[n - 1] << "\n";
    expected_str += ss.str();
  }
  expected_str += "  ColumnFamily: 0\n}\n";
  ASSERT_EQ(edit.DebugString(true), expected_str);

  std::string expected_json = "{\"EditNumber\": 4, \"WalDeletion\": ";
  {
    std::stringstream ss;
    ss << "{\"LogNumber\": " << kLogNumbers[n - 1] << "}";
    expected_json += ss.str();
  }
  expected_json += ", \"ColumnFamily\": 0}";
  ASSERT_EQ(edit.DebugJSON(4, true), expected_json);
}

TEST_F(VersionEditTest, FullHistoryTsLow) {
  VersionEdit edit;
  ASSERT_FALSE(edit.HasFullHistoryTsLow());
  std::string ts = test::EncodeInt(0);
  edit.SetFullHistoryTsLow(ts);
  TestEncodeDecode(edit);
}

// Tests that if RocksDB is downgraded, the new types of VersionEdits
// that have a tag larger than kTagSafeIgnoreMask can be safely ignored.
TEST_F(VersionEditTest, IgnorableTags) {
  SyncPoint::GetInstance()->SetCallBack(
      "VersionEdit::EncodeTo:IgnoreIgnorableTags", [&](void* arg) {
        bool* ignore = static_cast<bool*>(arg);
        *ignore = true;
      });
  SyncPoint::GetInstance()->EnableProcessing();

  constexpr uint64_t kPrevLogNumber = 100;
  constexpr uint64_t kLogNumber = 200;
  constexpr uint64_t kNextFileNumber = 300;
  constexpr uint64_t kColumnFamilyId = 400;

  VersionEdit edit;
  // Add some ignorable entries.
  for (int i = 0; i < 2; i++) {
    edit.AddWal(i + 1, WalMetadata(i + 2));
  }
  edit.SetDBId("db_id");
  // Add unignorable entries.
  edit.SetPrevLogNumber(kPrevLogNumber);
  edit.SetLogNumber(kLogNumber);
  // Add more ignorable entries.
  edit.DeleteWalsBefore(100);
  // Add unignorable entry.
  edit.SetNextFile(kNextFileNumber);
  // Add more ignorable entries.
  edit.SetFullHistoryTsLow("ts");
  // Add unignorable entry.
  edit.SetColumnFamily(kColumnFamilyId);

  std::string encoded;
  ASSERT_TRUE(edit.EncodeTo(&encoded, 0 /* ts_sz */));

  VersionEdit decoded;
  ASSERT_OK(decoded.DecodeFrom(encoded));

  // Check that all ignorable entries are ignored.
  ASSERT_FALSE(decoded.HasDbId());
  ASSERT_FALSE(decoded.HasFullHistoryTsLow());
  ASSERT_FALSE(decoded.IsWalAddition());
  ASSERT_FALSE(decoded.IsWalDeletion());
  ASSERT_TRUE(decoded.GetWalAdditions().empty());
  ASSERT_TRUE(decoded.GetWalDeletion().IsEmpty());

  // Check that unignorable entries are still present.
  ASSERT_EQ(edit.GetPrevLogNumber(), kPrevLogNumber);
  ASSERT_EQ(edit.GetLogNumber(), kLogNumber);
  ASSERT_EQ(edit.GetNextFile(), kNextFileNumber);
  ASSERT_EQ(edit.GetColumnFamily(), kColumnFamilyId);

  SyncPoint::GetInstance()->DisableProcessing();
}

TEST(FileMetaDataTest, UpdateBoundariesBlobIndex) {
  FileMetaData meta;

  {
    constexpr uint64_t file_number = 10;
    constexpr uint32_t path_id = 0;
    constexpr uint64_t file_size = 0;

    meta.fd = FileDescriptor(file_number, path_id, file_size);
  }

  constexpr char key[] = "foo";

  constexpr uint64_t expected_oldest_blob_file_number = 20;

  // Plain old value (does not affect oldest_blob_file_number)
  {
    constexpr char value[] = "value";
    constexpr SequenceNumber seq = 200;

    ASSERT_OK(meta.UpdateBoundaries(key, value, seq, kTypeValue));
    ASSERT_EQ(meta.oldest_blob_file_number, kInvalidBlobFileNumber);
  }

  // Non-inlined, non-TTL blob index (sets oldest_blob_file_number)
  {
    constexpr uint64_t blob_file_number = 25;
    static_assert(blob_file_number > expected_oldest_blob_file_number,
                  "unexpected");

    constexpr uint64_t offset = 1000;
    constexpr uint64_t size = 100;

    std::string blob_index;
    BlobIndex::EncodeBlob(&blob_index, blob_file_number, offset, size,
                          kNoCompression);

    constexpr SequenceNumber seq = 201;

    ASSERT_OK(meta.UpdateBoundaries(key, blob_index, seq, kTypeBlobIndex));
    ASSERT_EQ(meta.oldest_blob_file_number, blob_file_number);
  }

  // Another one, with the oldest blob file number (updates
  // oldest_blob_file_number)
  {
    constexpr uint64_t offset = 2000;
    constexpr uint64_t size = 300;

    std::string blob_index;
    BlobIndex::EncodeBlob(&blob_index, expected_oldest_blob_file_number, offset,
                          size, kNoCompression);

    constexpr SequenceNumber seq = 202;

    ASSERT_OK(meta.UpdateBoundaries(key, blob_index, seq, kTypeBlobIndex));
    ASSERT_EQ(meta.oldest_blob_file_number, expected_oldest_blob_file_number);
  }

  // Inlined TTL blob index (does not affect oldest_blob_file_number)
  {
    constexpr uint64_t expiration = 9876543210;
    constexpr char value[] = "value";

    std::string blob_index;
    BlobIndex::EncodeInlinedTTL(&blob_index, expiration, value);

    constexpr SequenceNumber seq = 203;

    ASSERT_OK(meta.UpdateBoundaries(key, blob_index, seq, kTypeBlobIndex));
    ASSERT_EQ(meta.oldest_blob_file_number, expected_oldest_blob_file_number);
  }

  // Non-inlined TTL blob index (does not affect oldest_blob_file_number, even
  // though file number is smaller)
  {
    constexpr uint64_t expiration = 9876543210;
    constexpr uint64_t blob_file_number = 15;
    static_assert(blob_file_number < expected_oldest_blob_file_number,
                  "unexpected");

    constexpr uint64_t offset = 2000;
    constexpr uint64_t size = 500;

    std::string blob_index;
    BlobIndex::EncodeBlobTTL(&blob_index, expiration, blob_file_number, offset,
                             size, kNoCompression);

    constexpr SequenceNumber seq = 204;

    ASSERT_OK(meta.UpdateBoundaries(key, blob_index, seq, kTypeBlobIndex));
    ASSERT_EQ(meta.oldest_blob_file_number, expected_oldest_blob_file_number);
  }

  // Corrupt blob index
  {
    constexpr char corrupt_blob_index[] = "!corrupt!";
    constexpr SequenceNumber seq = 205;

    ASSERT_TRUE(
        meta.UpdateBoundaries(key, corrupt_blob_index, seq, kTypeBlobIndex)
            .IsCorruption());
    ASSERT_EQ(meta.oldest_blob_file_number, expected_oldest_blob_file_number);
  }

  // Invalid blob file number
  {
    constexpr uint64_t offset = 10000;
    constexpr uint64_t size = 1000;

    std::string blob_index;
    BlobIndex::EncodeBlob(&blob_index, kInvalidBlobFileNumber, offset, size,
                          kNoCompression);

    constexpr SequenceNumber seq = 206;

    ASSERT_TRUE(meta.UpdateBoundaries(key, blob_index, seq, kTypeBlobIndex)
                    .IsCorruption());
    ASSERT_EQ(meta.oldest_blob_file_number, expected_oldest_blob_file_number);
  }
}

TEST_F(VersionEditTest, ResumableCompactionProgress) {
  // Test the bidirectional serialization of ResumableCompactionProgress
  VersionEdit edit_original;

  // Create test data for ResumableCompactionProgress
  ResumableCompactionProgress progress;

  // Create first subcompaction progress with various fields populated
  ResumableSubcompactionProgress subcompaction1;
  subcompaction1.next_internal_key_to_compact = "test_key_123";
  subcompaction1.num_processed_input_records = 1000;
  subcompaction1.num_processed_output_records = 800;
  subcompaction1.num_processed_proximal_level_output_records = 200;

  // Create some test FileMetaData for output files
  FileMetaData file1;
  file1.fd = FileDescriptor(100, 0, 1024, 50, 150);
  file1.smallest = InternalKey("a", 50, kTypeValue);
  file1.largest = InternalKey("z", 150, kTypeValue);
  file1.oldest_ancester_time = 12345;
  file1.file_creation_time = 67890;
  file1.epoch_number = 10;
  file1.file_checksum = "checksum1";
  file1.file_checksum_func_name = "crc32c";
  file1.marked_for_compaction = false;
  file1.temperature = Temperature::kUnknown;

  FileMetaData file2;
  file2.fd = FileDescriptor(101, 0, 2048, 200, 250);
  file2.smallest = InternalKey("b", 200, kTypeValue);
  file2.largest = InternalKey("y", 250, kTypeValue);
  file2.oldest_ancester_time = 12346;
  file2.file_creation_time = 67891;
  file2.epoch_number = 11;
  file2.file_checksum = "checksum2";
  file2.file_checksum_func_name = "crc32c";
  file2.marked_for_compaction = true;
  file2.temperature = Temperature::kHot;

  // Store files in temporary allocation for serialization
  subcompaction1.temporary_output_files_allocation.push_back(file1);
  subcompaction1.temporary_proximal_level_output_files_allocation.push_back(
      file2);

  // Set up pointers to the allocated files (simulate normal operation)
  subcompaction1.output_files.push_back(
      &subcompaction1.temporary_output_files_allocation[0]);
  subcompaction1.proximal_level_output_files.push_back(
      &subcompaction1.temporary_proximal_level_output_files_allocation[0]);

  // Create second subcompaction progress with minimal data
  ResumableSubcompactionProgress subcompaction2;
  subcompaction2.next_internal_key_to_compact =
      "start_key_456";  // Must not be empty
  subcompaction2.num_processed_input_records = 500;
  subcompaction2.num_processed_output_records = 400;
  subcompaction2.num_processed_proximal_level_output_records =
      0;  // No proximal level files

  // Add subcompactions to progress
  progress.push_back(subcompaction1);
  progress.push_back(subcompaction2);

  // Set the progress in VersionEdit
  edit_original.SetResumableCompactionProgress(progress);

  // Verify the edit reports it has resumable compaction progress
  ASSERT_TRUE(edit_original.HasResumableCompactionProgress());

  // Encode the VersionEdit
  std::string encoded;
  ASSERT_TRUE(edit_original.EncodeTo(&encoded, 0 /* ts_sz */));

  // Decode the VersionEdit
  VersionEdit edit_decoded;
  Status s = edit_decoded.DecodeFrom(encoded);
  ASSERT_OK(s) << s.ToString();

  // Verify the decoded edit has resumable compaction progress
  ASSERT_TRUE(edit_decoded.HasResumableCompactionProgress());

  const ResumableCompactionProgress& decoded_progress =
      edit_decoded.GetResumableCompactionProgress();

  // Verify the number of subcompactions matches original
  ASSERT_EQ(decoded_progress.size(), progress.size());

  // Verify first subcompaction progress against original
  const ResumableSubcompactionProgress& decoded_sub1 = decoded_progress[0];
  ASSERT_EQ(decoded_sub1.next_internal_key_to_compact,
            subcompaction1.next_internal_key_to_compact);
  ASSERT_EQ(decoded_sub1.num_processed_input_records,
            subcompaction1.num_processed_input_records);
  ASSERT_EQ(decoded_sub1.num_processed_output_records,
            subcompaction1.num_processed_output_records);
  ASSERT_EQ(decoded_sub1.num_processed_proximal_level_output_records,
            subcompaction1.num_processed_proximal_level_output_records);

  // Verify output files were correctly deserialized
  ASSERT_EQ(decoded_sub1.temporary_output_files_allocation.size(),
            subcompaction1.temporary_output_files_allocation.size());
  ASSERT_EQ(
      decoded_sub1.temporary_proximal_level_output_files_allocation.size(),
      subcompaction1.temporary_proximal_level_output_files_allocation.size());

  // Verify file metadata matches original file1
  const FileMetaData& decoded_file1 =
      decoded_sub1.temporary_output_files_allocation[0];
  ASSERT_EQ(decoded_file1.fd.GetNumber(), file1.fd.GetNumber());
  ASSERT_EQ(decoded_file1.fd.GetFileSize(), file1.fd.GetFileSize());
  ASSERT_EQ(decoded_file1.fd.smallest_seqno, file1.fd.smallest_seqno);
  ASSERT_EQ(decoded_file1.fd.largest_seqno, file1.fd.largest_seqno);
  ASSERT_EQ(decoded_file1.smallest.user_key().ToString(),
            file1.smallest.user_key().ToString());
  ASSERT_EQ(decoded_file1.largest.user_key().ToString(),
            file1.largest.user_key().ToString());
  ASSERT_EQ(decoded_file1.oldest_ancester_time, file1.oldest_ancester_time);
  ASSERT_EQ(decoded_file1.file_creation_time, file1.file_creation_time);
  ASSERT_EQ(decoded_file1.epoch_number, file1.epoch_number);
  ASSERT_EQ(decoded_file1.file_checksum, file1.file_checksum);
  ASSERT_EQ(decoded_file1.file_checksum_func_name,
            file1.file_checksum_func_name);
  ASSERT_EQ(decoded_file1.marked_for_compaction, file1.marked_for_compaction);
  ASSERT_EQ(decoded_file1.temperature, file1.temperature);

  // Verify file metadata matches original file2
  const FileMetaData& decoded_file2 =
      decoded_sub1.temporary_proximal_level_output_files_allocation[0];
  ASSERT_EQ(decoded_file2.fd.GetNumber(), file2.fd.GetNumber());
  ASSERT_EQ(decoded_file2.fd.GetFileSize(), file2.fd.GetFileSize());
  ASSERT_EQ(decoded_file2.fd.smallest_seqno, file2.fd.smallest_seqno);
  ASSERT_EQ(decoded_file2.fd.largest_seqno, file2.fd.largest_seqno);
  ASSERT_EQ(decoded_file2.smallest.user_key().ToString(),
            file2.smallest.user_key().ToString());
  ASSERT_EQ(decoded_file2.largest.user_key().ToString(),
            file2.largest.user_key().ToString());
  ASSERT_EQ(decoded_file2.oldest_ancester_time, file2.oldest_ancester_time);
  ASSERT_EQ(decoded_file2.file_creation_time, file2.file_creation_time);
  ASSERT_EQ(decoded_file2.epoch_number, file2.epoch_number);
  ASSERT_EQ(decoded_file2.file_checksum, file2.file_checksum);
  ASSERT_EQ(decoded_file2.file_checksum_func_name,
            file2.file_checksum_func_name);
  ASSERT_EQ(decoded_file2.marked_for_compaction, file2.marked_for_compaction);
  ASSERT_EQ(decoded_file2.temperature, file2.temperature);

  // Verify second subcompaction progress against original
  const ResumableSubcompactionProgress& decoded_sub2 = decoded_progress[1];
  ASSERT_EQ(decoded_sub2.next_internal_key_to_compact,
            subcompaction2.next_internal_key_to_compact);
  ASSERT_EQ(decoded_sub2.num_processed_input_records,
            subcompaction2.num_processed_input_records);
  ASSERT_EQ(decoded_sub2.num_processed_output_records,
            subcompaction2.num_processed_output_records);
  ASSERT_EQ(decoded_sub2.num_processed_proximal_level_output_records,
            subcompaction2.num_processed_proximal_level_output_records);

  // Verify no output files for second subcompaction (matches original)
  ASSERT_EQ(decoded_sub2.temporary_output_files_allocation.size(),
            subcompaction2.temporary_output_files_allocation.size());
  ASSERT_EQ(
      decoded_sub2.temporary_proximal_level_output_files_allocation.size(),
      subcompaction2.temporary_proximal_level_output_files_allocation.size());

  // Test the ToString() method against the decoded progress (which has files in
  // temporary allocation)
  std::string debug_str = decoded_sub1.ToString();
  ASSERT_TRUE(debug_str.find("next_key=SET") != std::string::npos);
  ASSERT_TRUE(debug_str.find(
                  "input_records=" +
                  std::to_string(subcompaction1.num_processed_input_records)) !=
              std::string::npos);
  ASSERT_TRUE(debug_str.find("output_files=1") !=
              std::string::npos);  // decoded_sub1 should have 1 output file
  ASSERT_TRUE(debug_str.find("proximal_files=1") !=
              std::string::npos);  // decoded_sub1 should have 1 proximal file
  ASSERT_TRUE(
      debug_str.find(
          "output_records=" +
          std::to_string(subcompaction1.num_processed_output_records)) !=
      std::string::npos);
  ASSERT_TRUE(
      debug_str.find(
          "proximal_records=" +
          std::to_string(
              subcompaction1.num_processed_proximal_level_output_records)) !=
      std::string::npos);

  std::string debug_str2 = decoded_sub2.ToString();
  ASSERT_TRUE(debug_str2.find("next_key=SET") != std::string::npos);
  ASSERT_TRUE(debug_str2.find(
                  "input_records=" +
                  std::to_string(subcompaction2.num_processed_input_records)) !=
              std::string::npos);
  ASSERT_TRUE(debug_str2.find("output_files=0") !=
              std::string::npos);  // decoded_sub2 should have 0 output files
  ASSERT_TRUE(debug_str2.find("proximal_files=0") !=
              std::string::npos);  // decoded_sub2 should have 0 proximal files

  // Test Clear() method
  ResumableSubcompactionProgress test_clear = decoded_sub1;
  test_clear.Clear();
  ASSERT_EQ(test_clear.next_internal_key_to_compact, "");
  ASSERT_EQ(test_clear.num_processed_input_records, 0);
  ASSERT_EQ(test_clear.num_processed_output_records, 0);
  ASSERT_EQ(test_clear.num_processed_proximal_level_output_records, 0);
  ASSERT_EQ(test_clear.temporary_output_files_allocation.size(), 0);
  ASSERT_EQ(test_clear.temporary_proximal_level_output_files_allocation.size(),
            0);

  // Test ClearResumableCompactionProgress()
  edit_decoded.ClearResumableCompactionProgress();
  ASSERT_FALSE(edit_decoded.HasResumableCompactionProgress());
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
