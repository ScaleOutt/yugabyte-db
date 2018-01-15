//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef YB_ROCKSDB_DB_VERSION_EDIT_H
#define YB_ROCKSDB_DB_VERSION_EDIT_H

#include <algorithm>
#include <set>
#include <utility>
#include <vector>
#include <string>

#include <boost/optional.hpp>

#include "yb/rocksdb/cache.h"
#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/util/arena.h"
#include "yb/rocksdb/util/autovector.h"

namespace rocksdb {

class VersionSet;
class VersionEditPB;

const uint64_t kFileNumberMask = 0x3FFFFFFFFFFFFFFF;

extern uint64_t PackFileNumberAndPathId(uint64_t number, uint64_t path_id);

// A copyable structure contains information needed to read data from an SST
// file. It can contains a pointer to a table reader opened for the file, or
// file number and size, which can be used to create a new table reader for it.
// The behavior is undefined when a copied of the structure is used when the
// file is not in any live version any more.
// SST can be either one file containing both meta data and data or it can be split into
// multiple files: one metadata file and number of data files (S-Blocks aka storage-blocks).
// As of 2017-03-10 there is at most one data file.
// Base file is a file which contains SST metadata. So, if SST is either one base file, or
// in case SST is split into multiple files, base file is a metadata file.
struct FileDescriptor {
  // Table reader in table_reader_handle
  TableReader* table_reader;
  uint64_t packed_number_and_path_id;
  uint64_t total_file_size;  // total file(s) size in bytes
  uint64_t base_file_size;  // base file size in bytes

  FileDescriptor() : FileDescriptor(0, 0, 0, 0) {}

  FileDescriptor(uint64_t number, uint32_t path_id, uint64_t _total_file_size,
      uint64_t _base_file_size)
      : table_reader(nullptr),
        packed_number_and_path_id(PackFileNumberAndPathId(number, path_id)),
        total_file_size(_total_file_size),
        base_file_size(_base_file_size) {}

  FileDescriptor& operator=(const FileDescriptor& fd) {
    table_reader = fd.table_reader;
    packed_number_and_path_id = fd.packed_number_and_path_id;
    total_file_size = fd.total_file_size;
    base_file_size = fd.base_file_size;
    return *this;
  }

  uint64_t GetNumber() const {
    return packed_number_and_path_id & kFileNumberMask;
  }
  uint32_t GetPathId() const {
    return static_cast<uint32_t>(
        packed_number_and_path_id / (kFileNumberMask + 1));
  }
  uint64_t GetTotalFileSize() const { return total_file_size; }
  uint64_t GetBaseFileSize() const { return base_file_size; }
};

enum class UpdateBoundariesType {
  ALL,
  SMALLEST,
  LARGEST,
};

struct FileMetaData {
  typedef FileBoundaryValues<InternalKey> BoundaryValues;

  int refs;
  FileDescriptor fd;
  bool being_compacted;     // Is this file undergoing compaction?
  BoundaryValues smallest;  // The smallest values in this file
  BoundaryValues largest;   // The largest values in this file
  OpId last_op_id;          // Last op_id in file.
  bool imported = false;    // Was this file imported from another DB.

  // Needs to be disposed when refs becomes 0.
  Cache::Handle* table_reader_handle;

  // Stats for compensating deletion entries during compaction

  // File size compensated by deletion entry.
  // This is updated in Version::UpdateAccumulatedStats() first time when the
  // file is created or loaded.  After it is updated (!= 0), it is immutable.
  uint64_t compensated_file_size;
  // These values can mutate, but they can only be read or written from
  // single-threaded LogAndApply thread
  uint64_t num_entries;            // the number of entries.
  uint64_t num_deletions;          // the number of deletion entries.
  uint64_t raw_key_size;           // total uncompressed key size.
  uint64_t raw_value_size;         // total uncompressed value size.
  bool init_stats_from_file;   // true if the data-entry stats of this file
                               // has initialized from file.

  bool marked_for_compaction;  // True if client asked us nicely to compact this
                               // file.

  FileMetaData();

  // REQUIRED: Keys must be given to the function in sorted order (it expects
  // the last key to be the largest).
  void UpdateBoundaries(InternalKey key, const FileBoundaryValuesBase& source);

  // Update all boundaries except key.
  void UpdateBoundariesExceptKey(const FileBoundaryValuesBase& source, UpdateBoundariesType type);
};

class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() { }

  void Clear();

  void SetComparatorName(const Slice& name) {
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    last_sequence_ = seq;
  }
  void SetFlushedOpId(const OpId& value) {
    flushed_op_id_ = value;
  }
  void SetFlushedOpId(int64_t term, int64_t index) {
    SetFlushedOpId(OpId(term, index));
  }
  void SetMaxColumnFamily(uint32_t max_column_family) {
    max_column_family_ = max_column_family;
  }

  void InitNewDB();

  // Add the specified file at the specified number.
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  void AddTestFile(int level,
                   const FileDescriptor& fd,
                   const FileMetaData::BoundaryValues& smallest,
                   const FileMetaData::BoundaryValues& largest,
                   bool marked_for_compaction) {
    assert(smallest.seqno <= largest.seqno);
    FileMetaData f;
    f.fd = fd;
    f.fd.table_reader = nullptr;
    f.smallest = smallest;
    f.largest = largest;
    f.last_op_id = OpId(1, largest.seqno);
    f.marked_for_compaction = marked_for_compaction;
    new_files_.emplace_back(level, f);
  }

  void AddFile(int level, const FileMetaData& f) {
    assert(f.smallest.seqno <= f.largest.seqno);
    new_files_.emplace_back(level, f);
  }

  void AddCleanedFile(int level, const FileMetaData& f) {
    assert(f.smallest.seqno <= f.largest.seqno);
    FileMetaData nf;
    nf.fd = f.fd;
    nf.fd.table_reader = nullptr;
    nf.smallest = f.smallest;
    nf.largest = f.largest;
    nf.last_op_id = f.last_op_id;
    nf.marked_for_compaction = f.marked_for_compaction;
    nf.imported = f.imported;
    new_files_.emplace_back(level, nf);
  }

  // Delete the specified "file" from the specified "level".
  void DeleteFile(int level, uint64_t file) {
    deleted_files_.insert({level, file});
  }

  // Number of edits
  size_t NumEntries() { return new_files_.size() + deleted_files_.size(); }

  bool IsColumnFamilyAdd() {
    return column_family_name_ ? true : false;
  }

  bool IsColumnFamilyManipulation() {
    return IsColumnFamilyAdd() || is_column_family_drop_;
  }

  void SetColumnFamily(uint32_t column_family_id) {
    column_family_ = column_family_id;
  }

  // set column family ID by calling SetColumnFamily()
  void AddColumnFamily(const std::string& name) {
    assert(!is_column_family_drop_);
    assert(!column_family_name_);
    assert(NumEntries() == 0);
    column_family_name_ = name;
  }

  // set column family ID by calling SetColumnFamily()
  void DropColumnFamily() {
    assert(!is_column_family_drop_);
    assert(!column_family_name_);
    assert(NumEntries() == 0);
    is_column_family_drop_ = true;
  }

  // return true on success.
  bool AppendEncodedTo(std::string* dst) const;
  Status DecodeFrom(BoundaryValuesExtractor* extractor, const Slice& src);

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;

  const DeletedFileSet& GetDeletedFiles() { return deleted_files_; }
  const std::vector<std::pair<int, FileMetaData>>& GetNewFiles() {
    return new_files_;
  }

  std::string DebugString(bool hex_key = false) const;
  std::string DebugJSON(int edit_num, bool hex_key = false) const;

 private:
  friend class VersionSet;
  friend class Version;

  bool EncodeTo(VersionEditPB* out) const;

  int max_level_;
  boost::optional<std::string> comparator_;
  boost::optional<uint64_t> log_number_;
  boost::optional<uint64_t> prev_log_number_;
  boost::optional<uint64_t> next_file_number_;
  boost::optional<uint32_t> max_column_family_;
  boost::optional<SequenceNumber> last_sequence_;
  OpId flushed_op_id_;

  DeletedFileSet deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;

  // Each version edit record should have column_family_id set
  // If it's not set, it is default (0)
  uint32_t column_family_;
  // a version edit can be either column_family add or
  // column_family drop. If it's column family add,
  // it also includes column family name.
  bool is_column_family_drop_;
  boost::optional<std::string> column_family_name_;
};

}  // namespace rocksdb

#endif  // YB_ROCKSDB_DB_VERSION_EDIT_H
