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

#include <gtest/gtest.h>

#include "olap/column_file/byte_buffer.h"
#include "olap/column_file/stream_name.h"
#include "olap/column_file/column_reader.h"
#include "olap/column_file/column_writer.h"
#include "olap/field.h"
#include "olap/olap_define.h"
#include "olap/olap_common.h"
#include "olap/row_cursor.h"
#include "runtime/mem_pool.h"
#include "runtime/string_value.hpp"
#include "runtime/vectorized_row_batch.h"
#include "util/logging.h"

using std::string;

namespace palo {
namespace column_file {

class TestColumn : public testing::Test {
public:
    TestColumn() : 
            _column_writer(NULL),
            _column_reader(NULL),
            _stream_factory(NULL) {
            _offsets.clear();
        _map_in_streams.clear();

        _present_buffers.clear();
        
        _data_buffers.clear();

        _second_buffers.clear();

        _dictionary_buffers.clear();

        _length_buffers.clear();

        _mem_tracker.reset(new MemTracker(-1));
        _mem_pool.reset(new MemPool(_mem_tracker.get()));
    }
    
    virtual ~TestColumn() {
        SAFE_DELETE(_column_writer);
        SAFE_DELETE(_column_reader);
        SAFE_DELETE(_stream_factory);
    }
    
    virtual void SetUp() {
        _offsets.push_back(0);

        _stream_factory = 
                new(std::nothrow) OutStreamFactory(COMPRESS_LZO,
                                                   OLAP_DEFAULT_COLUMN_STREAM_BUFFER_SIZE);
        ASSERT_TRUE(_stream_factory != NULL);
        config::column_dictionary_key_ration_threshold = 30;
        config::column_dictionary_key_size_threshold = 1000;
    }
    
    virtual void TearDown() {
        SAFE_DELETE(_column_writer);

        SAFE_DELETE(_column_reader);

        SAFE_DELETE(_stream_factory);
        
        SAFE_DELETE(_shared_buffer);
        
        _offsets.clear();

        _map_in_streams.clear();

        _present_buffers.clear();
        
        _data_buffers.clear();

        _second_buffers.clear();

        _dictionary_buffers.clear();

        _length_buffers.clear();
    }

    void CreateColumnWriter(const std::vector<FieldInfo> &tablet_schema) {
        _column_writer = ColumnWriter::create(
                0, tablet_schema, _stream_factory, 1024, BLOOM_FILTER_DEFAULT_FPP);
        
        ASSERT_TRUE(_column_writer != NULL);
        ASSERT_EQ(_column_writer->init(), OLAP_SUCCESS);
    }

    void CreateColumnReader(const std::vector<FieldInfo> &tablet_schema) {
        UniqueIdEncodingMap encodings;
        encodings[0] = ColumnEncodingMessage();
        encodings[0].set_kind(ColumnEncodingMessage::DIRECT);
        encodings[0].set_dictionary_size(1);
        CreateColumnReader(tablet_schema, encodings);
    }

    void CreateColumnReader(
            const std::vector<FieldInfo> &tablet_schema,
            UniqueIdEncodingMap &encodings) {
        UniqueIdToColumnIdMap included;
        included[0] = 0;
        UniqueIdToColumnIdMap segment_included;
        segment_included[0] = 0;

        _column_reader = ColumnReader::create(0,
                                               tablet_schema,
                                               included,
                                               segment_included,
                                               encodings);
        
        ASSERT_TRUE(_column_reader != NULL);

        system("rm ./tmp_file");

        ASSERT_EQ(OLAP_SUCCESS, 
                  helper.open_with_mode("tmp_file", 
                                        O_CREAT | O_EXCL | O_WRONLY, 
                                        S_IRUSR | S_IWUSR));
        std::vector<int> off;
        std::vector<int> length;
        std::vector<int> buffer_size;
        std::vector<StreamName> name;

        std::map<StreamName, OutStream*>::const_iterator it 
            = _stream_factory->streams().begin();
        for (; it != _stream_factory->streams().end(); ++it) {
            StreamName stream_name = it->first;
            OutStream *out_stream = it->second;
            std::vector<ByteBuffer*> *buffers;

            if (out_stream->is_suppressed()) {
                continue;
            }
            
            if (stream_name.kind() == StreamInfoMessage::ROW_INDEX) {
                continue;
            } else if (stream_name.kind() == StreamInfoMessage::PRESENT) {
                buffers = &_present_buffers;
            } else if (stream_name.kind() == StreamInfoMessage::DATA) {
                buffers = &_data_buffers;
            } else if (stream_name.kind() == StreamInfoMessage::SECONDARY) {
                buffers = &_second_buffers;
            } else if (stream_name.kind() == StreamInfoMessage::DICTIONARY_DATA) {
                buffers = &_dictionary_buffers;
            } else if (stream_name.kind() == StreamInfoMessage::LENGTH) {
                buffers = &_length_buffers;
            } else {
                ASSERT_TRUE(false);
            }
            
            ASSERT_TRUE(buffers != NULL);
            off.push_back(helper.tell());
            out_stream->write_to_file(&helper, 0);
            length.push_back(out_stream->get_stream_length());
            buffer_size.push_back(out_stream->get_total_buffer_size());
            name.push_back(stream_name);
        }
        helper.close();

        ASSERT_EQ(OLAP_SUCCESS, helper.open_with_mode("tmp_file", 
                O_RDONLY, S_IRUSR | S_IWUSR)); 

        _shared_buffer = ByteBuffer::create(
                OLAP_DEFAULT_COLUMN_STREAM_BUFFER_SIZE + sizeof(StreamHead));
        ASSERT_TRUE(_shared_buffer != NULL);

        for (int i = 0; i < off.size(); ++i) {
            ReadOnlyFileStream* in_stream = new (std::nothrow) ReadOnlyFileStream(
                    &helper, 
                    &_shared_buffer,
                    off[i], 
                    length[i], 
                    lzo_decompress, 
                    buffer_size[i],
                    &_stats);
            ASSERT_EQ(OLAP_SUCCESS, in_stream->init());

            _map_in_streams[name[i]] = in_stream;
        }

        ASSERT_EQ(_column_reader->init(
                        &_map_in_streams,
                        1024,
                        _mem_pool.get(),
                        &_stats), OLAP_SUCCESS);
    }

    void SetFieldInfo(FieldInfo &field_info,
                      std::string name,
                      FieldType type,
                      FieldAggregationMethod aggregation,
                      uint32_t length,
                      bool is_allow_null,
                      bool is_key) {
        field_info.name = name;
        field_info.type = type;
        field_info.aggregation = aggregation;
        field_info.length = length;
        field_info.is_allow_null = is_allow_null;
        field_info.is_key = is_key;
        field_info.precision = 1000;
        field_info.frac = 10000;
        field_info.unique_id = 0;
        field_info.is_bf_column = false;
    }

    void create_and_save_last_position() {
        ASSERT_EQ(_column_writer->create_row_index_entry(), OLAP_SUCCESS);
    }

    ColumnWriter *_column_writer;

    ColumnReader *_column_reader;
    std::unique_ptr<MemTracker> _mem_tracker;
    std::unique_ptr<MemPool> _mem_pool;
    std::unique_ptr<ColumnVector> _col_vector;

    OutStreamFactory *_stream_factory;

    std::vector<size_t> _offsets;

    std::vector<ByteBuffer*> _present_buffers;

    std::vector<ByteBuffer*> _data_buffers;

    std::vector<ByteBuffer*> _second_buffers;

    std::vector<ByteBuffer*> _dictionary_buffers;

    std::vector<ByteBuffer*> _length_buffers;

    ByteBuffer* _shared_buffer;

    std::map<StreamName, ReadOnlyFileStream *> _map_in_streams;

    FileHandler helper;

    OlapReaderStatistics _stats;
};

TEST_F(TestColumn, VectorizedTinyColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                     std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);

    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);

    data++;
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, SeekTinyColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);

    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_TINYINT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_TINYINT, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);
    
    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);
    data++;
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 2);

    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, SkipTinyColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    ASSERT_EQ(_column_reader->skip(2), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, VectorizedTinyColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    write_row.set_not_null(0);
    char value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[1], false);
    value = *reinterpret_cast<char*>(data + 1);
}

TEST_F(TestColumn, TinyColumnIndex) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);

    value = *reinterpret_cast<char*>(data + 1);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, SeekTinyColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_TINYINT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_TINYINT, false);

    PositionProvider position1(&entry1);
    PositionProvider position2(&entry2);
    
    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);
    value = *reinterpret_cast<char*>(data + 1);
    ASSERT_EQ(value, 2);

    ASSERT_EQ(_column_reader->seek(&position2), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, SkipTinyColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("TinyColumn"), 
                 OLAP_FIELD_TYPE_TINYINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 1, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    char value = 1;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, &value, _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    ASSERT_EQ(_column_reader->skip(2), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);    
}

TEST_F(TestColumn, VectorizedShortColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int16_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<int16_t*>(data);
    ASSERT_EQ(value, 1);

    value = *reinterpret_cast<int16_t*>(data + sizeof(int16_t));
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, SeekShortColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int16_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_SMALLINT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_SMALLINT, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);
    
    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);

    value = *reinterpret_cast<char*>(data + sizeof(int16_t)); 
    ASSERT_EQ(value, 2);

    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, SkipShortColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int16_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    ASSERT_EQ(_column_reader->skip(2), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, SeekShortColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int16_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_SMALLINT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_SMALLINT, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);
    
    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 1);

    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, VectorizedShortColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);

    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    int16_t value = 3;
    write_row.set_not_null(0);
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[1], false);

    value = *reinterpret_cast<char*>(data + sizeof(int16_t));
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, SkipShortColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("ShortColumn"), 
                 OLAP_FIELD_TYPE_SMALLINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 2, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int16_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 2;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    ASSERT_EQ(_column_reader->skip(2), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<char*>(data);
    ASSERT_EQ(value, 3);   
}

TEST_F(TestColumn, VectorizedIntColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("IntColumn"), 
                 OLAP_FIELD_TYPE_INT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int32_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<int*>(data);
    ASSERT_EQ(value, 1);

    value = *reinterpret_cast<int*>(data + sizeof(int));
    ASSERT_EQ(value, 3);     
}

TEST_F(TestColumn, VectorizedIntColumnMassWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("IntColumn"), 
                 OLAP_FIELD_TYPE_INT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    for (int32_t i = 0; i < 10000; i++) {
        write_row.set_field_content(0, reinterpret_cast<char *>(&i), _mem_pool.get());
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());

    char* data = NULL; 
    for (int32_t i = 0; i < 10000; ++i) {
        if (i % 1000 == 0) {
            ASSERT_EQ(_column_reader->next_vector(
                _col_vector.get(), 1000, _mem_pool.get()), OLAP_SUCCESS);
            data = reinterpret_cast<char*>(_col_vector->col_data());
        }

        int32_t value = 0;
        value = *reinterpret_cast<int*>(data);
        ASSERT_EQ(value, i);
        data += sizeof(int32_t);
    }
}

TEST_F(TestColumn, VectorizedIntColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("IntColumn"), 
                 OLAP_FIELD_TYPE_INT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int32_t value = -1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());

    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);

    bool* is_null = _col_vector->is_null();
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[0], false);
    value = *reinterpret_cast<int*>(data);
    ASSERT_EQ(value, -1);

    ASSERT_EQ(is_null[1], true);
}

TEST_F(TestColumn, VectorizedLongColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("LongColumnWithoutPresent"), 
                 OLAP_FIELD_TYPE_BIGINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    int64_t value = 1;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<int64_t*>(data);
    ASSERT_EQ(value, 1);

    value = *reinterpret_cast<int64_t*>(data + sizeof(int64_t));
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, VectorizedLongColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("LongColumnWithPresent"), 
                 OLAP_FIELD_TYPE_BIGINT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    int64_t value = 3;
    write_row.set_not_null(0);
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[1], false);

    value = *reinterpret_cast<int64_t*>(data + sizeof(int64_t));
    ASSERT_EQ(value, 3);
}

TEST_F(TestColumn, VectorizedFloatColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("FloatColumnWithoutPresent"), 
                 OLAP_FIELD_TYPE_FLOAT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    float value = 1.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<float*>(data);
    ASSERT_FLOAT_EQ(value, 1.234);

    data += sizeof(float);
    value = *reinterpret_cast<float*>(data);
    ASSERT_FLOAT_EQ(value, 3.234);
}

TEST_F(TestColumn, VectorizedFloatColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("FloatColumnWithPresent"), 
                 OLAP_FIELD_TYPE_FLOAT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    float value = 3.234;
    write_row.set_not_null(0);
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    ASSERT_EQ(is_null[1], false);

    char* data = reinterpret_cast<char*>(_col_vector->col_data()) + sizeof(float);
    value = *reinterpret_cast<float*>(data);
    ASSERT_FLOAT_EQ(value, 3.234);
}

TEST_F(TestColumn, SeekFloatColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("FloatColumnWithPresent"), 
                 OLAP_FIELD_TYPE_FLOAT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    float value = 1.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    value = 3.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_FLOAT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_FLOAT, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);
    
    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<float*>(data);
    ASSERT_FLOAT_EQ(value, 1.234);

    value = *reinterpret_cast<float*>(data + sizeof(float));
    ASSERT_FLOAT_EQ(value, 3.234);    
}

TEST_F(TestColumn, SkipFloatColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("FloatColumnWithPresent"), 
                 OLAP_FIELD_TYPE_FLOAT, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 4, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    float value = 1.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3.234;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    ASSERT_EQ(_column_reader->skip(1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<float*>(data);
    ASSERT_FLOAT_EQ(value, 3.234);    
}

TEST_F(TestColumn, VectorizedDoubleColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DoubleColumnWithoutPresent"), 
                 OLAP_FIELD_TYPE_DOUBLE, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    double value = 1.23456789;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    value = 3.23456789;
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value = *reinterpret_cast<double*>(data);
    ASSERT_DOUBLE_EQ(value, 1.23456789);

    data += sizeof(double);
    value = *reinterpret_cast<double*>(data);
    ASSERT_DOUBLE_EQ(value, 3.23456789); 
}

TEST_F(TestColumn, VectorizedDoubleColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DoubleColumnWithPresent"), 
                 OLAP_FIELD_TYPE_DOUBLE, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    double value = 3.23456789;
    write_row.set_not_null(0);
    write_row.set_field_content(0, reinterpret_cast<char *>(&value), _mem_pool.get());
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[1], false);

    data += sizeof(double);
    value = *reinterpret_cast<double*>(data);
    ASSERT_DOUBLE_EQ(value, 3.23456789); 
}

TEST_F(TestColumn, VectorizedDatetimeColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DatetimeColumnWithoutPresent"), 
                 OLAP_FIELD_TYPE_DATETIME, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("2000-10-10 10:10:10");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), 
        "0&2000-10-10 10:10:10", strlen("0&2000-10-10 10:10:10")) == 0);
}

TEST_F(TestColumn, VectorizedDatetimeColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DatetimeColumnWithoutPresent"), 
                 OLAP_FIELD_TYPE_DATETIME, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 8, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.set_null(0);    
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    std::vector<string> val_string_array;
    val_string_array.push_back("2000-10-10 10:10:10");
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    ASSERT_EQ(is_null[1], false);

    data += sizeof(uint64_t);
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), 
        "0&2000-10-10 10:10:10", strlen("0&2000-10-10 10:10:10")) == 0);

    ASSERT_NE(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
}

TEST_F(TestColumn, VectorizedDateColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DateColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_DATE, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 3, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("2000-10-10");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&2000-10-10", strlen("0&2000-10-10")) == 0);
}

TEST_F(TestColumn, VectorizedDateColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DateColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_DATE, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 3, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
   
    write_row.set_null(0); 
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    std::vector<string> val_string_array;
    val_string_array.push_back("2000-10-10");
    write_row.from_string(val_string_array);
    for (uint32_t i = 0; i < 100; ++i) {
        write_row.set_not_null(0);
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }
    
    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 101, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    for (uint32_t i = 0; i < 100; ++i) {
        data += sizeof(uint24_t);
        ASSERT_EQ(is_null[i+1], false);
        read_row.set_field_content(0, data, _mem_pool.get());
        ASSERT_TRUE(strncmp(read_row.to_string().c_str(), 
            "0&2000-10-10", strlen("0&2000-10-10")) == 0);
    }
}

TEST_F(TestColumn, VectorizedDecimalColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DecimalColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_DECIMAL, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 12, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("1234.5678");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back("5678.1234");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&1234.5678", strlen("0&1234.5678")) == 0);

    data += sizeof(decimal12_t);
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&5678.1234", strlen("0&5678.1234")) == 0);
}

TEST_F(TestColumn, VectorizedDecimalColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DecimalColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_DECIMAL, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 12, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back("5678.1234");
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    data += sizeof(decimal12_t);
    ASSERT_EQ(is_null[1], false);
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&5678.1234", strlen("0&5678.1234")) == 0);
}

TEST_F(TestColumn, SkipDecimalColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DecimalColumnWithPresent"), 
                 OLAP_FIELD_TYPE_DECIMAL, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 12, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("1234.5678");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back("5678.1234");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);

    char read_value[20];
    memset(read_value, 0, 20);
    ASSERT_EQ(_column_reader->skip(1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&5678.1234", strlen("0&5678.1234")) == 0);
}

TEST_F(TestColumn, SeekDecimalColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DecimalColumnWithPresent"), 
                 OLAP_FIELD_TYPE_DECIMAL, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 12, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("1234.5678");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();
    
    val_string_array.clear();
    val_string_array.push_back("5678.1234");
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    create_and_save_last_position();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_FLOAT, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_FLOAT, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);

    char read_value[20];
    memset(read_value, 0, 20);
    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&1234.5678", strlen("0&1234.5678")) == 0);
    
    memset(read_value, 0, 20);
    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), "0&5678.1234", strlen("0&5678.1234")) == 0);
}

TEST_F(TestColumn, VectorizedLargeIntColumnWithoutPresent) {
    // init table schema
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("LargeIntColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_LARGEINT, 
                 OLAP_FIELD_AGGREGATION_SUM, 
                 16, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    // test data
    string value1 = "100000000000000000000000000000000000000";
    string value2 = "-170141183460469231731687303715884105728";

    // write data
    CreateColumnWriter(tablet_schema);
    RowCursor write_row;
    write_row.init(tablet_schema);

    std::vector<string> val_string_array;
    val_string_array.push_back(value1);
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back(value2);
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    value1 = "0&" + value1;
    value2 = "0&" + value2;
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value1.c_str(), value1.size()) == 0);

    read_row.set_field_content(0, data + sizeof(int128_t), _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value2.c_str(), value2.size()) == 0);
}

TEST_F(TestColumn, VectorizedLargeIntColumnWithPresent) {
    // init table schema
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("LargeIntColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_LARGEINT, 
                 OLAP_FIELD_AGGREGATION_SUM, 
                 16, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    // test data
    string value1 = "100000000000000000000000000000000000000";
    string value2 = "-170141183460469231731687303715884105728";

    // write data
    CreateColumnWriter(tablet_schema);
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    std::vector<string> val_string_array;
    val_string_array.push_back(value1);
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back(value2);
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    RowCursor read_row;
    read_row.init(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 3, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);
    ASSERT_EQ(is_null[1], false);
    ASSERT_EQ(is_null[2], false);

    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    value1 = "0&" + value1;
    value2 = "0&" + value2;

    data += sizeof(int128_t);
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value1.c_str(), value1.size()) == 0);

    data += sizeof(int128_t);
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value2.c_str(), value2.size()) == 0);
}

TEST_F(TestColumn, SkipLargeIntColumnWithPresent) {
    // init table schema
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("LargeIntColumnWithPresent"), 
                 OLAP_FIELD_TYPE_LARGEINT, 
                 OLAP_FIELD_AGGREGATION_SUM, 
                 16, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    // test data
    string value1 = "100000000000000000000000000000000000000";
    string value2 = "-170141183460469231731687303715884105728";

    // write data
    CreateColumnWriter(tablet_schema);
    RowCursor write_row;
    write_row.init(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back(value1);
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back(value2);
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    RowCursor read_row;
    read_row.init(tablet_schema);

    value2 = "0&" + value2; 
    ASSERT_EQ(_column_reader->skip(1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    char* data = reinterpret_cast<char*>(_col_vector->col_data());
    read_row.set_field_content(0, data, _mem_pool.get());
    ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value2.c_str(), value2.size()) == 0);
}

// TODO(jiangguoqiang): this test has a problem. Need to fix it.
// TEST_F(TestColumn, SeekLargeIntColumnWithPresent) {
    // // init table schema
    // std::vector<FieldInfo> tablet_schema;
    // FieldInfo field_info;
    // SetFieldInfo(field_info,
                 // std::string("LargeIntColumnWithPresent"), 
                 // OLAP_FIELD_TYPE_LARGEINT, 
                 // OLAP_FIELD_AGGREGATION_SUM, 
                 // 16, 
                 // true,
                 // true);
    // tablet_schema.push_back(field_info);

    // // test data
    // string value1 = "100000000000000000000000000000000000000";
    // string value2 = "-170141183460469231731687303715884105728";
    // string value3 = "65535";

    // // write data
    // CreateColumnWriter(tablet_schema);
    // RowCursor write_row;
    // write_row.init(tablet_schema);
    
    // std::vector<string> val_string_array;
    // val_string_array.push_back(value1);
    // write_row.from_string(val_string_array);
    // ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    // val_string_array.clear();
    // val_string_array.push_back(value2);
    // write_row.from_string(val_string_array);
    // ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    // create_and_save_last_position();
    
    // val_string_array.clear();
    // val_string_array.push_back(value3);
    // write_row.from_string(val_string_array);
    // ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    // create_and_save_last_position();

    // ColumnDataHeaderMessage header;
    // ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // // read data
    // CreateColumnReader(tablet_schema);
    // RowCursor read_row;
    // read_row.init(tablet_schema);
    // PositionEntryReader entry0;
    // entry0._positions = _column_writer->index()->mutable_entry(0)->_positions;
    // entry0._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    // entry0._statistics.init(OLAP_FIELD_TYPE_LARGEINT);

    // PositionEntryReader entry1;
    // entry1._positions = _column_writer->index()->mutable_entry(1)->_positions;
    // entry1._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    // entry1._statistics.init(OLAP_FIELD_TYPE_LARGEINT);

    // PositionProvider position0(&entry0);
    // PositionProvider position1(&entry1);

    // ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    // ASSERT_EQ(_column_reader->next(), OLAP_SUCCESS);
    // ASSERT_EQ(_column_reader->attach(&read_row), OLAP_SUCCESS);
    // ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value1.c_str(), value1.size()) == 0);
    
    // ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    // ASSERT_EQ(_column_reader->next(), OLAP_SUCCESS);
    // ASSERT_EQ(_column_reader->attach(&read_row), OLAP_SUCCESS);
    // ASSERT_TRUE(strncmp(read_row.to_string().c_str(), value3.c_str(), value3.size()) == 0);
// }

TEST_F(TestColumn, VectorizedDirectVarcharColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 10, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("YWJjZGU="); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }
    val_string_array.clear();
    val_string_array.push_back("ZWRjYmE="); //"edcba" base_64_encode is "ZWRjYmE="
    write_row.from_string(val_string_array);
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());

    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 5, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWJjZGU=", value->size) == 0);
    for (uint32_t i = 0; i < 2; i++) {
        value++;
        ASSERT_TRUE(strncmp(value->data, "YWJjZGU=", value->size) == 0);
    }
    for (uint32_t i = 0; i < 2; i++) {
        value++;
        ASSERT_TRUE(strncmp(value->data, "ZWRjYmE=", value->size) == 0);
    }
    ASSERT_NE(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
}

TEST_F(TestColumn, VectorizedDirectVarcharColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 10, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);

    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("YWJjZGU="); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);
    ASSERT_EQ(is_null[1], false);

    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    value++;
    ASSERT_TRUE(strncmp(value->data, "YWJjZGU=", value->size) == 0);
}

TEST_F(TestColumn, SkipDirectVarcharColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 10, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("YWJjZGU="); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back("YWFhYWE="); //"aaaaa" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    char read_value[20];
    memset(read_value, 0, 20);
    ASSERT_EQ(_column_reader->skip(1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWFhYWE=", value->size) == 0);
}

TEST_F(TestColumn, SeekDirectVarcharColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 10, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("YWJjZGU="); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    _column_writer->create_row_index_entry();
    
    val_string_array.clear();
    val_string_array.push_back("YWFhYWE="); //"aaaaa" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    _column_writer->create_row_index_entry();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_VARCHAR, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_VARCHAR, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);

    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWJjZGU=", value->size) == 0);

    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWFhYWE=", value->size) == 0);
}

TEST_F(TestColumn, SeekDirectVarcharColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 10, 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("YWJjZGU="); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    _column_writer->create_row_index_entry();
    
    val_string_array.clear();
    val_string_array.push_back("YWFhYWE="); //"aaaaa" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    _column_writer->create_row_index_entry();

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    PositionEntryReader entry1;
    entry1._positions = _column_writer->index()->mutable_entry(0)->_positions;
    entry1._positions_count = _column_writer->index()->mutable_entry(0)->_positions_count;
    entry1._statistics.init(OLAP_FIELD_TYPE_VARCHAR, false);

    PositionEntryReader entry2;
    entry2._positions = _column_writer->index()->mutable_entry(1)->_positions;
    entry2._positions_count = _column_writer->index()->mutable_entry(1)->_positions_count;
    entry2._statistics.init(OLAP_FIELD_TYPE_VARCHAR, false);

    PositionProvider position0(&entry1);
    PositionProvider position1(&entry2);

    ASSERT_EQ(_column_reader->seek(&position0), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWJjZGU=", value->size) == 0);

    ASSERT_EQ(_column_reader->seek(&position1), OLAP_SUCCESS);
    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
    value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    ASSERT_TRUE(strncmp(value->data, "YWFhYWE=", value->size) == 0);
}

TEST_F(TestColumn, VectorizedStringColumnWithoutPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("VarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_CHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 strlen("abcde"), 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("abcde"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }
    val_string_array.clear();
    val_string_array.push_back("edcba"); //"edcba" base_64_encode is "ZWRjYmE="
    write_row.from_string(val_string_array);
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 5, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());

    ASSERT_TRUE(strncmp(value->data, "abcde", value->size) == 0);
    for (uint32_t i = 0; i < 2; i++) {
        value++;
        ASSERT_TRUE(strncmp(value->data, "abcde", value->size) == 0);  
    }
    for (uint32_t i = 0; i < 2; i++) {
        value++;
        ASSERT_TRUE(strncmp(value->data, "edcba", value->size) == 0);  
    }
    ASSERT_NE(_column_reader->next_vector(
        _col_vector.get(), 1, _mem_pool.get()), OLAP_SUCCESS);
}

TEST_F(TestColumn, VectorizedStringColumnWithPresent) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("VarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_CHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 strlen("abcde"), 
                 true,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    write_row.set_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("abcde"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    write_row.set_not_null(0);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 2, _mem_pool.get()), OLAP_SUCCESS);
    bool* is_null = _col_vector->is_null();
    ASSERT_EQ(is_null[0], true);
    ASSERT_EQ(is_null[1], false);

    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());
    value++;
    ASSERT_TRUE(strncmp(value->data, "abcde", value->size) == 0);
}

TEST_F(TestColumn, VectorizedStringColumnWithoutoutPresent2) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("VarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_CHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 20, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);
    
    std::vector<string> val_string_array;
    val_string_array.push_back("abcde"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    val_string_array.clear();
    val_string_array.push_back("aaaaa"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    val_string_array.clear();
    val_string_array.push_back("bbbbb"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    val_string_array.clear();
    val_string_array.push_back("ccccc"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    val_string_array.clear();
    val_string_array.push_back("ddddd"); //"abcde" base_64_encode is "YWJjZGU="
    write_row.from_string(val_string_array);
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 5, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());

    ASSERT_TRUE(strncmp(value->data, "abcde", value->size) == 0);

    value++;
    ASSERT_TRUE(strncmp(value->data, "aaaaa", value->size) == 0);

    value++;
    ASSERT_TRUE(strncmp(value->data, "bbbbb", value->size) == 0);

    value++;
    ASSERT_TRUE(strncmp(value->data, "ccccc", value->size) == 0);

    value++;
    ASSERT_TRUE(strncmp(value->data, "ddddd", value->size) == 0);
}

TEST_F(TestColumn, VectorizedDirectVarcharColumnWith65533) {
    // write data
    std::vector<FieldInfo> tablet_schema;
    FieldInfo field_info;
    SetFieldInfo(field_info,
                 std::string("DirectVarcharColumnWithoutoutPresent"), 
                 OLAP_FIELD_TYPE_VARCHAR, 
                 OLAP_FIELD_AGGREGATION_REPLACE, 
                 65535, 
                 false,
                 true);
    tablet_schema.push_back(field_info);

    CreateColumnWriter(tablet_schema);
    
    RowCursor write_row;
    write_row.init(tablet_schema);
    write_row.allocate_memory_for_string_type(tablet_schema);

    std::vector<string> val_string_array;
    val_string_array.push_back(std::string(65533, 'a')); 
    ASSERT_EQ(OLAP_SUCCESS, write_row.from_string(val_string_array));
    ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);

    val_string_array.clear();
    val_string_array.push_back("edcba"); //"edcba" base_64_encode is "ZWRjYmE="
    write_row.from_string(val_string_array);
    for (uint32_t i = 0; i < 2; i++) {
        ASSERT_EQ(_column_writer->write(&write_row), OLAP_SUCCESS);
    }   

    ColumnDataHeaderMessage header;
    ASSERT_EQ(_column_writer->finalize(&header), OLAP_SUCCESS);

    // read data
    CreateColumnReader(tablet_schema);
    
    RowCursor read_row;
    read_row.init(tablet_schema);
    read_row.allocate_memory_for_string_type(tablet_schema);

    _col_vector.reset(new ColumnVector());
    ASSERT_EQ(_column_reader->next_vector(
        _col_vector.get(), 3, _mem_pool.get()), OLAP_SUCCESS);
    StringSlice* value = reinterpret_cast<StringSlice*>(_col_vector->col_data());

    for (uint32_t i = 0; i < 65533; i++) {
        ASSERT_TRUE(strncmp(value->data + i, "a", 1) == 0);
    }

    for (uint32_t i = 0; i < 2; i++) {
        value++;
        ASSERT_TRUE(strncmp(value->data, "edcba", value->size) == 0);
    }   
}

}
}

int main(int argc, char** argv) {
    std::string conffile = std::string(getenv("PALO_HOME")) + "/conf/be.conf";
    if (!palo::config::init(conffile.c_str(), false)) {
        fprintf(stderr, "error read config file. \n");
        return -1;
    }
    palo::init_glog("be-test");
    int ret = palo::OLAP_SUCCESS;
    testing::InitGoogleTest(&argc, argv);
    ret = RUN_ALL_TESTS();
    return ret;
}

