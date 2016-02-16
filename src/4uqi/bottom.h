/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING for License information.
 */

#include "0root/root.h"

#include <map>

#include "1base/error.h"
#include "2config/db_config.h"
#include "4uqi/plugin_wrapper.h"
#include "4uqi/statements.h"
#include "4uqi/scanvisitor.h"
#include "4uqi/scanvisitorfactoryhelper.h"

// Always verify that a file of level N does not include headers > N!

#ifndef UPS_ROOT_H
#  error "root.h was not included"
#endif

namespace upscaledb {

// If the vector is full then delete the old minimum to make space.
// Then append the new value.
template<typename T1, typename T2>
static inline T1
store_max_value(T1 new_maximum, T1 old_maximum, T2 value,
                std::map<T1, T2> &storage, size_t limit)
{
  typedef typename std::map<T1, T2>::value_type ValueType;

  if (unlikely(storage.size() < limit)) {
    storage.insert(ValueType(new_maximum, value));
    return (new_maximum > old_maximum ? new_maximum : old_maximum);
  }

  if (new_maximum < old_maximum) {
    storage.erase(storage.find(old_maximum));
    storage.insert(ValueType(new_maximum, value));
    return storage.rbegin()->first;
  }
  return old_maximum;
}

template<typename Key, typename Record>
struct BottomScanVisitorBase : public NumericalScanVisitor {
  BottomScanVisitorBase(const DbConfig *cfg, SelectStatement *stmt)
    : NumericalScanVisitor(stmt),
      max_key(std::numeric_limits<typename Key::type>::min()),
      max_record(std::numeric_limits<typename Record::type>::min()),
      key_type(cfg->key_type), record_type(cfg->record_type) {
    if (statement->limit == 0)
      statement->limit = 1;
  }

  // Assigns the result to |result|
  virtual void assign_result(uqi_result_t *result) {
    uqi_result_initialize(result, key_type, record_type);

    if (isset(statement->function.flags, UQI_STREAM_KEY)) {
      for (typename std::map<Key, Record>::iterator it = stored_keys.begin();
                      it != stored_keys.end(); it++) {
        const Key &key = it->first;
        const Record &record = it->second;
        uqi_result_add_row(result, key.ptr(), key.size(), record.ptr(),
                        record.size());
      }
    }
    else {
      for (typename std::map<Record, Key>::iterator it = stored_records.begin();
                      it != stored_records.end(); it++) {
        const Record &record = it->first;
        const Key &key = it->second;
        uqi_result_add_row(result, key.ptr(), key.size(), record.ptr(),
                        record.size());
      }
    }
  }

  // The maximum value currently stored in |keys|
  Key max_key;

  // The current set of keys
  std::map<Key, Record> stored_keys;

  // The maximum value currently stored in |records|
  Record max_record;

  // The current set of records
  std::map<Record, Key> stored_records;

  // The types for keys and records
  int key_type;
  int record_type;
};

template<typename Key, typename Record>
struct BottomScanVisitor : public BottomScanVisitorBase<Key, Record> {
  typedef BottomScanVisitorBase<Key, Record> P;

  BottomScanVisitor(const DbConfig *cfg, SelectStatement *stmt)
    : BottomScanVisitorBase<Key, Record>(cfg, stmt) {
  }

  // Operates on a single key
  virtual void operator()(const void *key_data, uint16_t key_size, 
                  const void *record_data, uint32_t record_size, 
                  size_t duplicate_count) {
    Key key(key_data, key_size);
    Record record(record_data, record_size);

    if (isset(P::statement->function.flags, UQI_STREAM_KEY)) {
      P::max_key = store_max_value(key, P::max_key, record,
                      P::stored_keys, P::statement->limit);
    }
    else {
      P::max_record = store_max_value(record, P::max_record, key,
                      P::stored_records, P::statement->limit);
    }
  }

  // Operates on an array of keys
  virtual void operator()(const void *key_data, const void *record_data,
                  size_t length) {
    Sequence<Key> keys(key_data, length);
    Sequence<Record> records(record_data, length);
    typename Sequence<Key>::iterator kit = keys.begin();
    typename Sequence<Record>::iterator rit = records.begin();

    if (isset(P::statement->function.flags, UQI_STREAM_KEY)) {
      for (; kit != keys.end(); kit++, rit++) {
        P::max_key = store_max_value(*kit, P::max_key, *rit,
                        P::stored_keys, P::statement->limit);
      }
    }
    else {
      for (; kit != keys.end(); kit++, rit++) {
        P::max_record = store_max_value(*rit, P::max_record, *kit,
                        P::stored_records, P::statement->limit);
      }
    }
  }
};

struct BottomScanVisitorFactory
{
  static ScanVisitor *create(const DbConfig *cfg, SelectStatement *stmt) {
    return (ScanVisitorFactoryHelper::create<BottomScanVisitor>(cfg, stmt));
  }
};

template<typename Key, typename Record>
struct BottomIfScanVisitor : public BottomScanVisitorBase<Key, Record> {
  typedef BottomScanVisitorBase<Key, Record> P;

  BottomIfScanVisitor(const DbConfig *cfg, SelectStatement *stmt)
    : BottomScanVisitorBase<Key, Record>(cfg, stmt), plugin(cfg, stmt) {
  }

  // Operates on a single key
  //
  // TODO first check if the key is > old_maximum, THEN check the predicate
  // (otherwise the predicate is checked for every key, and I think this is
  // more expensive than the other way round)
  virtual void operator()(const void *key_data, uint16_t key_size, 
                  const void *record_data, uint32_t record_size, 
                  size_t duplicate_count) {
    if (plugin.pred(key_data, key_size, record_data, record_size)) {
      Key key(key_data, key_size);
      Record record(record_data, record_size);

      if (isset(P::statement->function.flags, UQI_STREAM_KEY)) {
        P::max_key = store_max_value(key, P::max_key, record,
                        P::stored_keys, P::statement->limit);
      }
      else {
        P::max_record = store_max_value(record, P::max_record, key,
                        P::stored_records, P::statement->limit);
      }
    }
  }

  // Operates on an array of keys and records (both with fixed length)
  //
  // TODO first check if the key is > old_maximum, THEN check the predicate
  // (otherwise the predicate is checked for every key, and I think this is
  // more expensive than the other way round)
  virtual void operator()(const void *key_data, const void *record_data,
                  size_t length) {
    Sequence<Key> keys(key_data, length);
    Sequence<Record> records(record_data, length);
    typename Sequence<Key>::iterator kit = keys.begin();
    typename Sequence<Record>::iterator rit = records.begin();

    if (isset(P::statement->function.flags, UQI_STREAM_KEY)) {
      for (; kit != keys.end(); kit++, rit++) {
        if (plugin.pred(&kit->value, kit->size(), &rit->value, rit->size())) {
          P::max_key = store_max_value(*kit, P::max_key, *rit,
                          P::stored_keys, P::statement->limit);
        }
      }
    }
    else {
      for (; kit != keys.end(); kit++, rit++) {
        if (plugin.pred(&kit->value, kit->size(), &rit->value, rit->size())) {
          P::max_record = store_max_value(*rit, P::max_record, *kit,
                          P::stored_records, P::statement->limit);
        }
      }
    }
  }

  // The predicate plugin
  PluginWrapper plugin;
};

struct BottomIfScanVisitorFactory
{
  static ScanVisitor *create(const DbConfig *cfg, SelectStatement *stmt) {
    return (ScanVisitorFactoryHelper::create<BottomIfScanVisitor>(cfg, stmt));
  }
};

} // namespace upscaledb

