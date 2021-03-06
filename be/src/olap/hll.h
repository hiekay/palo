// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BDG_PALO_BE_SRC_OLAP_HLL_H
#define BDG_PALO_BE_SRC_OLAP_HLL_H

#include <math.h>
#include <stdio.h>
#include <set>
#include <map>

// #include "olap/field_info.h"
#include "olap/olap_common.h"
// #include "olap/olap_define.h"

namespace palo {

const static int HLL_COLUMN_PRECISION = 14;
const static int HLL_EXPLICLIT_INT64_NUM = 160;
const static int HLL_REGISTERS_COUNT = 16384;
// registers (2^14) + 1 (type)
const static int HLL_COLUMN_DEFAULT_LEN = 16385;

struct HllContext {
    bool has_value;
    bool has_sparse_or_full;
    char registers[HLL_REGISTERS_COUNT];
    std::set<uint64_t> hash64_set;
};

// help parse hll set
class HllSetResolver {
public:
    HllSetResolver() : _buf_ref(nullptr),
                       _buf_len(0),
                       _set_type(HLL_DATA_EMPTY),
                       _full_value_position(nullptr),
                       _expliclit_value(nullptr),
                       _expliclit_num(0) {}

    ~HllSetResolver() {}

    typedef uint8_t SetTypeValueType;
    typedef uint8_t ExpliclitLengthValueType;
    typedef int32_t SparseLengthValueType;
    typedef uint16_t SparseIndexType;
    typedef uint8_t SparseValueType;

    // only save pointer
    void init(char* buf, int len){
        this->_buf_ref = buf;
        this->_buf_len = len;
    }

    // hll set type
    HllDataType get_hll_data_type() {
        return _set_type;
    };

    // expliclit value num
    int get_expliclit_count() {
        return (int)_expliclit_num;
    };

    // get expliclit index value 64bit
    uint64_t get_expliclit_value(int index) {
        if (index >= _expliclit_num) {
            return -1;
        }
        return _expliclit_value[index];
    };

    // get expliclit index value 64bit
    char* get_expliclit_value() {
        return (char*)_expliclit_value;
    };

    // get full register value
    char* get_full_value() {
        return _full_value_position;
    };

    // get sparse (index, value) count
    int get_sparse_count() {
        return (int)*_sparse_count;
    };

    // get (index, value) map
    std::map<SparseIndexType, SparseValueType>& get_sparse_map() {
        return _sparse_map;
    };

    // parse set , call after copy() or init()
    void parse();

    // fill registers with set
    void fill_registers(char* registers, int len);

    // fill map with set
    void fill_index_to_value_map(std::map<int, uint8_t>* index_to_value, int len);

    // fill hash map
    void fill_hash64_set(std::set<uint64_t>* hash_set);

private :
    char* _buf_ref;    // set
    int _buf_len;      // set len
    HllDataType _set_type;        //set type
    char* _full_value_position;
    uint64_t* _expliclit_value;
    ExpliclitLengthValueType _expliclit_num;
    std::map<SparseIndexType, SparseValueType> _sparse_map;
    SparseLengthValueType* _sparse_count;
};

// 通过varchar的变长编码方式实现hll集合
// 实现hll列中间计算结果的处理
// empty 空集合
// expliclit 存储64位hash值的集合
// sparse 存储hll非0的register
// full  存储全部的hll register
// empty -> expliclit -> sparse -> full 四种类型的转换方向不可逆
// 第一个字节存放hll集合的类型 0:empty 1:expliclit 2:sparse 3:full
// 已决定后面的数据怎么解析
class HllSetHelper {
public:
    static void set_sparse(char *result, const std::map<int, uint8_t>& index_to_value, int& len);
    static void set_expliclit(char* result, const std::set<uint64_t>& hash_value_set, int& len);
    static void set_full(char* result, const char* registers, const int set_len, int& len);
    static void set_full(char* result, const std::map<int, uint8_t>& index_to_value,
                         const int set_len, int& len);
    static void set_max_register(char *registers,
                                 int registers_len,
                                 const std::set<uint64_t>& hash_set);
    static void fill_set(const char* data, HllContext* context);
    static void init_context(HllContext* context);
};

}  // namespace palo

#endif // BDG_PALO_BE_SRC_OLAP_HLL_H
