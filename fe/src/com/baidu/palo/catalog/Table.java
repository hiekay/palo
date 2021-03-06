// Modifications copyright (C) 2017, Baidu.com, Inc.
// Copyright 2017 The Apache Software Foundation

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.baidu.palo.catalog;

import com.baidu.palo.analysis.CreateTableStmt;
import com.baidu.palo.common.InternalException;
import com.baidu.palo.common.io.Text;
import com.baidu.palo.common.io.Writable;
import com.baidu.palo.thrift.TTableDescriptor;

import com.google.common.base.Preconditions;
import com.google.common.collect.Maps;

import org.apache.commons.lang.NotImplementedException;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;

/**
 * Internal representation of table-related metadata. A table contains several partitions.
 */
public class Table extends MetaObject implements Writable {
    public enum TableType {
        MYSQL,
        OLAP,
        SCHEMA,
        INLINE_VIEW,
        VIEW,
        KUDU,
        BROKER
    }

    protected long id;
    protected String name;
    protected TableType type;
    protected List<Column> baseSchema;
    // tree map for case-insensitive lookup
    protected Map<String, Column> nameToColumn;

    public Table(TableType type) {
        this.type = type;
        this.baseSchema = new LinkedList<Column>();
        this.nameToColumn = Maps.newTreeMap(String.CASE_INSENSITIVE_ORDER);
    }

    public Table(long id, String tableName, TableType type, List<Column> baseSchema) {
        this.id = id;
        this.name = tableName;
        this.type = type;
        this.baseSchema = baseSchema;

        this.nameToColumn = Maps.newTreeMap(String.CASE_INSENSITIVE_ORDER);
        if (baseSchema != null) {
            for (Column col : baseSchema) {
                nameToColumn.put(col.getName(), col);
            }
        } else {
            // Only view in with-clause have null base
            Preconditions.checkArgument(type == TableType.VIEW, "Table has no columns");
        }
    }

    public long getId() {
        return id;
    }

    public String getName() {
        return name;
    }

    public TableType getType() {
        return type;
    }

    public int getKeysNum() {
        int keysNum = 0;
        for (Column column : baseSchema) {
            if (column.isKey()) {
                keysNum += 1;
            }
        }
        return keysNum;
    }

    public List<Column> getBaseSchema() {
        return baseSchema;
    }

    public void setNewBaseSchema(List<Column> newSchema) {
        this.baseSchema = newSchema;
        this.nameToColumn.clear();
        for (Column col : baseSchema) {
            nameToColumn.put(col.getName(), col);
        }
    }

    public Column getColumn(String name) {
        return nameToColumn.get(name);
    }

    public TTableDescriptor toThrift() {
        return null;
    }

    public static Table read(DataInput in) throws IOException {
        Table table = null;
        TableType type = TableType.valueOf(Text.readString(in));
        if (type == TableType.OLAP) {
            table = new OlapTable();
            table.readFields(in);
        } else if (type == TableType.MYSQL) {
            table = new MysqlTable();
            table.readFields(in);
        } else if (type == TableType.VIEW) {
            View view = new View();
            view.readFields(in);
            try {
                view.init();
            } catch (InternalException e) {
                throw new IOException(e.getMessage());
            }
            table = view;
        } else if (type == TableType.KUDU) {
            table = new KuduTable();
            table.readFields(in);
        } else if (type == TableType.BROKER) {
            table = new BrokerTable();
            table.readFields(in);
        } else {
            throw new IOException("Unknown table type: " + type.name());
        }

        return table;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        // ATTN: must write type first
        Text.writeString(out, type.name());

        // write last check time
        super.write(out);

        out.writeLong(id);
        Text.writeString(out, name);

        // base schema
        int columnCount = baseSchema.size();
        out.writeInt(columnCount);
        for (Column column : baseSchema) {
            column.write(out);
        }
    }

    @Override
    public void readFields(DataInput in) throws IOException {
        super.readFields(in);

        this.id = in.readLong();
        this.name = Text.readString(in);

        // base schema
        int columnCount = in.readInt();
        for (int i = 0; i < columnCount; i++) {
            Column column = Column.read(in);
            this.baseSchema.add(column);
            this.nameToColumn.put(column.getName(), column);
        }
    }

    public boolean equals(Table table) {
        return true;
    }

    // return if this table is partitioned.
    // For OlapTable ture when is partitioned, or distributed by hash when no partition
    public boolean isPartitioned() {
        return false;
    }

    public Partition getPartition(String partitionName) {
        return null;
    }

    public String getEngine() {
        if (this instanceof OlapTable) {
            return "Palo";
        } else if (this instanceof MysqlTable) {
            return "MySQL";
        } else if (this instanceof SchemaTable) {
            return "MEMORY";
        } else {
            return null;
        }
    }

    public String getMysqlType() {
        if (this instanceof View) {
            return "VIEW";
        }
        return "BASE TABLE";
    }

    public String getComment() {
        if (this instanceof View) {
            return "VIEW";
        }
        return "";
    }

    public CreateTableStmt toCreateTableStmt(String dbName) {
        throw new NotImplementedException();
    }

    @Override
    public int getSignature(int signatureVersion) {
        throw new NotImplementedException();
    }
}
