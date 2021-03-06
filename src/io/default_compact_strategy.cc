// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "db/dbformat.h"
#include "io/atomic_merge_strategy.h"
#include "io/default_compact_strategy.h"
#include "leveldb/slice.h"

namespace tera {
namespace io {

DefaultCompactStrategy::DefaultCompactStrategy(const TableSchema& schema)
    : m_schema(schema),
      m_raw_key_operator(GetRawKeyOperatorFromSchema(m_schema)) {
    // build index
    for (int32_t i = 0; i < m_schema.column_families_size(); ++i) {
        const std::string name = m_schema.column_families(i).name();
        m_cf_indexs[name] = i;
    }
    m_has_put = false;
    VLOG(11) << "LGCompactStrategy construct";
}

DefaultCompactStrategy::~DefaultCompactStrategy() {}

const char* DefaultCompactStrategy::Name() const {
    return "tera.DefaultCompactStrategy";
}

bool DefaultCompactStrategy::Drop(const leveldb::Slice& tera_key, uint64_t n) {
    leveldb::Slice key, col, qual;
    int64_t ts = -1;
    leveldb::TeraKeyType type;

    if (!m_raw_key_operator->ExtractTeraKey(tera_key, &key, &col, &qual, &ts, &type)) {
        LOG(WARNING) << "invalid tera key: " << tera_key.ToString();
        return true;
    }

    m_cur_type = type;
    m_cur_ts = ts;
    int32_t cf_id = -1;
    if (type != leveldb::TKT_DEL && DropByColumnFamily(col.ToString(), &cf_id)) {
        // drop illegel column family
        return true;
    }

    if (key.compare(m_last_key) != 0) {
        // reach a new row
        m_last_key.assign(key.data(), key.size());
        m_last_col.assign(col.data(), col.size());
        m_last_qual.assign(qual.data(), qual.size());
        m_del_row_ts = m_del_col_ts = m_del_qual_ts = -1;
        m_version_num = 0;
        m_has_put = false;
        switch (type) {
            case leveldb::TKT_DEL:
                m_del_row_ts = ts;
            case leveldb::TKT_DEL_COLUMN:
                m_del_col_ts = ts;
            case leveldb::TKT_DEL_QUALIFIERS:
                m_del_qual_ts = ts;
            default:;
        }
    } else if (m_del_row_ts >= ts) {
        // skip deleted row and the same row_del mark
        return true;
    } else if (col.compare(m_last_col) != 0) {
        // reach a new column family
        m_last_col.assign(col.data(), col.size());
        m_last_qual.assign(qual.data(), qual.size());
        m_del_col_ts = m_del_qual_ts = -1;
        m_version_num = 0;
        m_has_put = false;
        switch (type) {
            case leveldb::TKT_DEL_COLUMN:
                m_del_col_ts = ts;
            case leveldb::TKT_DEL_QUALIFIERS:
                m_del_qual_ts = ts;
            default:;
        }
    } else if (m_del_col_ts > ts) {
        // skip deleted column family
        return true;
    } else if (qual.compare(m_last_qual) != 0) {
        // reach a new qualifier
        m_last_qual.assign(qual.data(), qual.size());
        m_del_qual_ts = -1;
        m_version_num = 0;
        m_has_put = false;
        if (type == leveldb::TKT_DEL_QUALIFIERS) {
            m_del_qual_ts = ts;
        }
    } else if (m_del_qual_ts > ts) {
        // skip deleted qualifier
        return true;
    }

    if (type == leveldb::TKT_VALUE) {
        m_has_put = true;
        if (++m_version_num > m_schema.column_families(cf_id).max_versions()) {
            // drop out-of-range version
            return true;
        }
    }

    if (IsAtomicOP(type) && m_has_put) { // drop ADDs which is later than Put
        return true;
    }
    return false;
}

bool DefaultCompactStrategy::ScanMergedValue(leveldb::Iterator* it, std::string* merged_value) {
    std::string merged_key;
    bool has_merge =  InternalMergeProcess(it, merged_value, &merged_key, true, false);
    return has_merge;
}

bool DefaultCompactStrategy::MergeAtomicOPs(leveldb::Iterator* it,
                                            std::string* merged_value,
                                            std::string* merged_key) {
    bool merge_put_flag = false; // don't merge the last PUT if we have
    return InternalMergeProcess(it, merged_value, merged_key, merge_put_flag, true);
}

bool DefaultCompactStrategy::InternalMergeProcess(leveldb::Iterator* it, std::string* merged_value,
                                                  std::string* merged_key,
                                                  bool merge_put_flag, bool is_internal_key) {
    if (!tera::io::IsAtomicOP(m_cur_type)) {
        return false;
    }
    assert(merged_key);
    assert(merged_value);

    AtomicMergeStrategy atom_merge;
    atom_merge.Init(merged_key, merged_value, it->key(), it->value(), m_cur_type);

    it->Next();
    int64_t last_ts_atomic = m_cur_ts;
    int64_t version_num = 0;

    while (it->Valid()) {
        if (version_num >= 1) {
            break; //avoid accumulate to many versions
        }
        leveldb::Slice itkey = it->key();
        leveldb::Slice key;
        leveldb::Slice col;
        leveldb::Slice qual;
        int64_t ts = -1;
        leveldb::TeraKeyType type;

        if (is_internal_key) {
            leveldb::ParsedInternalKey ikey;
            leveldb::ParseInternalKey(itkey, &ikey);
            if (!m_raw_key_operator->ExtractTeraKey(ikey.user_key, &key, &col, &qual, &ts, &type)) {
                LOG(WARNING) << "invalid internal key for tera: " << itkey.ToString();
                break;
            }
        } else {
            if (!m_raw_key_operator->ExtractTeraKey(itkey, &key, &col, &qual, &ts, &type)) {
                LOG(WARNING) << "invalid tera key: " << itkey.ToString();
                break;
            }
        }

        if (m_last_qual != qual || m_last_col != col || m_last_key != key) {
            break; // out of the current cell
        }

        if (!IsAtomicOP(type) && type != leveldb::TKT_VALUE) {
            break;
        } else if (type == leveldb::TKT_VALUE) {
            if (!merge_put_flag || ++version_num > 1) {
                break;
            }
        }

        if (ts != last_ts_atomic || type ==  leveldb::TKT_VALUE) {
            atom_merge.MergeStep(it->key(), it->value(), type);
        }
        last_ts_atomic = ts;
        it->Next();
    }
    atom_merge.Finish();
    return true;
}

bool DefaultCompactStrategy::ScanDrop(const leveldb::Slice& tera_key, uint64_t n) {
    leveldb::Slice key, col, qual;
    int64_t ts = -1;
    leveldb::TeraKeyType type;

    if (!m_raw_key_operator->ExtractTeraKey(tera_key, &key, &col, &qual, &ts, &type)) {
        LOG(WARNING) << "invalid tera key: " << tera_key.ToString();
        return true;
    }

    m_cur_type = type;
    m_cur_ts = ts;
    int32_t cf_id = -1;
    if (type != leveldb::TKT_DEL && DropByColumnFamily(col.ToString(), &cf_id)) {
        // drop illegel column family
        return true;
    }

    if (key.compare(m_last_key) != 0) {
        // reach a new row
        m_last_key.assign(key.data(), key.size());
        m_last_col.assign(col.data(), col.size());
        m_last_qual.assign(qual.data(), qual.size());
        m_last_type = type;
        m_version_num = 0;
        m_del_row_ts = m_del_col_ts = m_del_qual_ts = -1;
        m_has_put = false;

        switch (type) {
            case leveldb::TKT_DEL:
                m_del_row_ts = ts;
            case leveldb::TKT_DEL_COLUMN:
                m_del_col_ts = ts;
            case leveldb::TKT_DEL_QUALIFIERS:
                m_del_qual_ts = ts;
            default:;
        }
    } else if (m_del_row_ts >= ts) {
        // skip deleted row and the same row_del mark
        return true;
    } else if (col.compare(m_last_col) != 0) {
        // reach a new column family
        m_last_col.assign(col.data(), col.size());
        m_last_qual.assign(qual.data(), qual.size());
        m_last_type = type;
        m_version_num = 0;
        m_del_col_ts = m_del_qual_ts = -1;
        m_has_put = false;
        switch (type) {
            case leveldb::TKT_DEL_COLUMN:
                m_del_col_ts = ts;
            case leveldb::TKT_DEL_QUALIFIERS:
                m_del_qual_ts = ts;
            default:;
        }
    } else if (m_del_col_ts > ts) {
        // skip deleted column family
        return true;
    } else if (qual.compare(m_last_qual) != 0) {
        // reach a new qualifier
        m_last_qual.assign(qual.data(), qual.size());
        m_last_type = type;
        m_version_num = 0;
        m_del_qual_ts = -1;
        m_has_put = false;
        if (type == leveldb::TKT_DEL_QUALIFIERS) {
            m_del_qual_ts = ts;
        }
    } else if (m_del_qual_ts > ts) {
        // skip deleted qualifier
        return true;
    } else if (type == leveldb::TKT_DEL_QUALIFIERS) {
        // reach a delete-all-qualifier mark
        m_del_qual_ts = ts;
    } else if (m_last_type == leveldb::TKT_DEL_QUALIFIER) {
        // skip latest deleted version
        m_last_type = type;
        if (type = leveldb::TKT_VALUE) {
            m_version_num++;
        }
        return true;
    } else {
        m_last_type = type;
    }

    if (type != leveldb::TKT_VALUE && !IsAtomicOP(type)) {
        return true;
    }

    if (type == leveldb::TKT_VALUE) {
        m_has_put = true;
    }

    if (IsAtomicOP(type) && m_has_put) {
        return true;
    }

    CHECK(cf_id >= 0) << "illegel column family";
    if (type == leveldb::TKT_VALUE &&
            ++m_version_num > m_schema.column_families(cf_id).max_versions()) {
        // drop out-of-range version
        return true;
    }
    return false;
}

bool DefaultCompactStrategy::DropByColumnFamily(const std::string& column_family,
                                                int32_t* cf_idx) const {
    std::map<std::string, int32_t>::const_iterator it =
        m_cf_indexs.find(column_family);
    if (it == m_cf_indexs.end()) {
        return true;
    }
    if (cf_idx) {
        *cf_idx = it->second;
    }
    return false;
}

bool DefaultCompactStrategy::DropByLifeTime(int32_t cf_idx, int64_t timestamp) const {
    return false;
}

DefaultCompactStrategyFactory::DefaultCompactStrategyFactory(const TableSchema& schema)
    : m_schema(schema) {}

DefaultCompactStrategy* DefaultCompactStrategyFactory::NewInstance() {
    return new DefaultCompactStrategy(m_schema);
}

} // namespace io
} // namespace tera
