# Modifications copyright (C) 2017, Baidu.com, Inc.
# Copyright 2017 The Apache Software Foundation

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE_2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# where to put generated libraries
set(EXECUTABLE_OUTPUT_PATH "${BUILD_DIR}/test/util")

ADD_BE_TEST(bit_util_test)
ADD_BE_TEST(brpc_stub_cache_test)
ADD_BE_TEST(path_trie_test)
ADD_BE_TEST(count_down_latch_test)
ADD_BE_TEST(lru_cache_util_test)
ADD_BE_TEST(filesystem_util_test)
ADD_BE_TEST(internal_queue_test)
ADD_BE_TEST(cidr_test)
ADD_BE_TEST(new_metrics_test)
ADD_BE_TEST(palo_metrics_test)
ADD_BE_TEST(system_metrics_test)
ADD_BE_TEST(core_local_test)
ADD_BE_TEST(types_test)
ADD_BE_TEST(rpc_channel_test)
