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

suite("test_outer_join_with_order_by") {
    sql """
        drop table if exists outerjoin_A;
    """

    sql """
        drop table if exists outerjoin_B;
    """

    sql """
        create table outerjoin_A ( a int not null )
        ENGINE=OLAP
        DISTRIBUTED BY HASH(a) BUCKETS 1
        PROPERTIES (
        "replication_allocation" = "tag.location.default: 1",
        "in_memory" = "false",
        "storage_format" = "V2"
        );
    """

    sql """
        create table outerjoin_B ( a int not null )
        ENGINE=OLAP
        DISTRIBUTED BY HASH(a) BUCKETS 1
        PROPERTIES (
        "replication_allocation" = "tag.location.default: 1",
        "in_memory" = "false",
        "storage_format" = "V2"
        );
    """

    sql """
        insert into outerjoin_A values( 1 );
    """

    sql """
        insert into outerjoin_B values( 1 );
    """

    qt_select """
        select case when outerjoin_A.a <= outerjoin_A.a then outerjoin_A.a else outerjoin_A.a end as r
        from outerjoin_A right join outerjoin_B on outerjoin_A.a = outerjoin_B.a order by outerjoin_A.a;
    """

    sql """
        drop table if exists outerjoin_A;
    """

    sql """
        drop table if exists outerjoin_B;
    """
}