from dataclasses import dataclass
import datetime
from typing import Sequence

from ydb.library.yql.providers.generic.connector.api.common.data_source_pb2 import EDataSourceKind
from ydb.library.yql.providers.generic.connector.api.service.protos.connector_pb2 import EDateTimeFormat
from ydb.public.api.protos.ydb_value_pb2 import Type

import ydb.library.yql.providers.generic.connector.tests.test_cases.select_positive_common as select_positive_common
import ydb.library.yql.providers.generic.connector.tests.utils.clickhouse as clickhouse
import ydb.library.yql.providers.generic.connector.tests.utils.postgresql as postgresql
from ydb.library.yql.providers.generic.connector.tests.utils.database import Database
from ydb.library.yql.providers.generic.connector.tests.utils.schema import (
    Schema,
    Column,
    ColumnList,
    DataSourceType,
    SelectWhat,
)
from ydb.library.yql.providers.generic.connector.tests.utils.settings import GenericSettings


@dataclass
class TestCase(select_positive_common.TestCase):
    date_time_format: EDateTimeFormat = EDateTimeFormat.YQL_FORMAT

    @property
    def data_out(self) -> Sequence:
        return self.data_out_ if self.data_out_ else self.data_in

    @property
    def generic_settings(self) -> GenericSettings:
        gs = super().generic_settings
        gs.date_time_format = self.date_time_format
        return gs


class Factory:
    _name = 'datetime'

    def _make_test_clickhouse(self) -> TestCase:
        schema = Schema(
            columns=ColumnList(
                Column(
                    name='col_0_id',
                    ydb_type=Type.UINT8,
                    data_source_type=DataSourceType(ch=clickhouse.UInt8()),
                ),
                Column(
                    name='col_1_date',
                    ydb_type=Type.DATE,
                    data_source_type=DataSourceType(ch=clickhouse.Date()),
                ),
                Column(
                    name='col_2_date32',
                    ydb_type=Type.DATE,
                    data_source_type=DataSourceType(ch=clickhouse.Date32()),
                ),
                Column(
                    name='col_3_datetime',
                    ydb_type=Type.DATETIME,
                    data_source_type=DataSourceType(ch=clickhouse.DateTime()),
                ),
                Column(
                    name='col_4_datetime64',
                    ydb_type=Type.TIMESTAMP,
                    data_source_type=DataSourceType(ch=clickhouse.DateTime64()),
                ),
            ),
        )

        '''
        ClickHouse values' bounds:
        Date                    [1970-01-01, 2149-06-06]
        Date32                  [1900-01-01, 2299-12-31]
        Datetime                [1970-01-01 00:00:00, 2106-02-07 06:28:15]
        Datetime64              [1900-01-01 00:00:00, 2299-12-31 23:59:59.99999999]

        YQL datetime bounds:    [1970-01-01 00:00:00, 2106-01-01 00:00:00]
        '''

        data_in = [
            # Value is too early for both CH and YQL
            # In this case ClickHouse behaviour is undefined
            # Clickhouse cuts off only date part of value along ClickHouse bottom bound
            [
                1,
                '1950-01-10',
                '1850-01-10',
                '1950-01-10 12:23:45',
                '1850-01-10 12:23:45.678',
            ],
            # Value is OK for CH, but can be too early for YQL
            [
                2,
                '1970-01-10',
                '1950-01-10',
                '1980-01-10 12:23:45',
                '1950-01-10 12:23:45.678',
            ],
            # Value is OK for both CH and YQL
            [
                3,
                '2004-01-10',
                '2004-01-10',
                '2004-01-10 12:23:45',
                '2004-01-10 12:23:45.678',
            ],
            # Value is OK for CH, but too late for YQL
            [
                4,
                '2110-01-10',
                '2110-01-10',
                '2106-01-10 12:23:45',
                '2110-01-10 12:23:45.678',
            ],
            # Value is too late for both OK for CH
            # In this case ClickHouse behaviour is undefined
            # "Natural" overflow for Datetime
            # Cutting off along ClickHouse top bound for other types
            [
                5,
                '2150-01-10',
                '2300-01-10',
                '2107-01-10 12:23:45',
                '2300-01-10 12:23:45.678',
            ],
        ]

        data_out = [
            [
                1,
                datetime.date(1970, 1, 1),
                None,
                None,
                None,
            ],
            [2, datetime.date(1970, 1, 10), None, datetime.datetime(1980, 1, 10, 12, 23, 45), None],
            [
                3,
                datetime.date(2004, 1, 10),
                datetime.date(2004, 1, 10),
                datetime.datetime(2004, 1, 10, 12, 23, 45),
                datetime.datetime(2004, 1, 10, 12, 23, 45, 678000),
            ],
            [
                4,
                None,
                None,
                None,
                None,
            ],
            [
                5,
                None,
                None,
                datetime.datetime(1970, 12, 4, 5, 55, 29),
                None,
            ],
        ]

        data_source_kind = EDataSourceKind.CLICKHOUSE

        return TestCase(
            name=self._name + '_ch_YQL',
            date_time_format=EDateTimeFormat.YQL_FORMAT,
            data_in=data_in,
            data_out_=data_out,
            select_what=SelectWhat.asterisk(schema.columns),
            select_where=None,
            data_source_kind=data_source_kind,
            schema=schema,
            database=Database.make_for_data_source_kind(data_source_kind),
            pragmas=dict(),
        )

    def _make_test_postgresql(self) -> TestCase:
        schema = Schema(
            columns=ColumnList(
                Column(
                    name='col_0_id',
                    ydb_type=Type.INT32,
                    data_source_type=DataSourceType(pg=postgresql.Int4()),
                ),
                # TODO: timestamp
                Column(
                    name='col_1_datetime64',
                    ydb_type=Type.TIMESTAMP,
                    data_source_type=DataSourceType(pg=postgresql.TimestampWithoutTimeZone()),
                ),
            ),
        )
        data_in = [
            # Date is OK for CH, but too early for YQL
            [
                1,
                datetime.datetime(1950, 5, 27, 12, 23, 45, 678000),
            ],
            # Date is OK for both CH and YQL
            [2, datetime.datetime(1988, 11, 20, 12, 23, 45, 678000)],
            # Date is OK for CH, but too late for YQL
            [
                3,
                datetime.datetime(2108, 1, 1, 12, 23, 45, 678000),
            ],
        ]

        data_out = [
            [
                1,
                None,
            ],
            [
                2,
                # datetime.datetime(1988, 11, 20, 12, 23, 45, 678000).astimezone(ZoneInfo('UTC')).replace(tzinfo=None),
                datetime.datetime(1988, 11, 20, 12, 23, 45, 678000),
            ],
            [
                3,
                None,
            ],
        ]

        data_source_kind = EDataSourceKind.POSTGRESQL

        return TestCase(
            name=self._name + '_pg_YQL',
            date_time_format=EDateTimeFormat.YQL_FORMAT,
            data_in=data_in,
            data_out_=data_out,
            select_what=SelectWhat.asterisk(schema.columns),
            select_where=None,
            data_source_kind=data_source_kind,
            schema=schema,
            database=Database.make_for_data_source_kind(data_source_kind),
            pragmas=dict(),
        )

    def _make_test_string_clickhouse(self) -> TestCase:
        schema = Schema(
            columns=ColumnList(
                Column(
                    name='col_0_id',
                    ydb_type=Type.UINT8,
                    data_source_type=DataSourceType(ch=clickhouse.UInt8()),
                ),
                Column(
                    name='col_1_date',
                    ydb_type=Type.DATE,
                    data_source_type=DataSourceType(ch=clickhouse.Date()),
                ),
                Column(
                    name='col_2_date32',
                    ydb_type=Type.DATE,
                    data_source_type=DataSourceType(ch=clickhouse.Date32()),
                ),
                Column(
                    name='col_3_datetime',
                    ydb_type=Type.DATETIME,
                    data_source_type=DataSourceType(ch=clickhouse.DateTime()),
                ),
                Column(
                    name='col_4_datetime64',
                    ydb_type=Type.TIMESTAMP,
                    data_source_type=DataSourceType(ch=clickhouse.DateTime64()),
                ),
            ),
        )

        data_in = [
            # Value is too early for both CH and YQL
            # In this case ClickHouse behaviour is undefined
            # For Datetime Clickhouse returns bottom bound and
            # cuts off only date part of value along ClickHouse bottom bound for other types
            [
                1,
                '1950-01-10',
                '1850-01-10',
                '1950-01-10 12:23:45',
                '1850-01-10 12:23:45.678',
            ],
            # Value is OK for CH, but can be too early for YQL
            [
                2,
                '1970-01-10',
                '1950-01-10',
                '1980-01-10 12:23:45',
                '1950-01-10 12:23:45.678',
            ],
            # Value is OK for both CH and YQL
            [
                3,
                '2004-01-10',
                '2004-01-10',
                '2004-01-10 12:23:45',
                '2004-01-10 12:23:45.678',
            ],
            # Value is OK for CH, but too late for YQL
            [
                4,
                '2110-01-10',
                '2110-01-10',
                '2106-01-10 12:23:45',
                '2110-01-10 12:23:45.678',
            ],
            # Value is too late for both OK for CH
            # In this case ClickHouse behaviour is undefined
            # "Natural" overflow for Datetime
            # Cutting off along ClickHouse top bound for other types
            [
                5,
                '2150-01-10',
                '2300-01-10',
                '2107-01-10 12:23:45',
                '2300-01-10 12:23:45.678',
            ],
        ]

        data_out = [
            [1, '1970-01-01', '1900-01-01', '1970-01-01T00:00:00Z', '1900-01-01T12:23:45.678Z'],
            [2, '1970-01-10', '1950-01-10', '1980-01-10T12:23:45Z', '1950-01-10T12:23:45.678Z'],
            [3, '2004-01-10', '2004-01-10', '2004-01-10T12:23:45Z', '2004-01-10T12:23:45.678Z'],
            [4, '2110-01-10', '2110-01-10', '2106-01-10T12:23:45Z', '2110-01-10T12:23:45.678Z'],
            [
                5,
                '2149-06-06',
                '2299-12-31',
                '1970-12-04T05:55:29Z',
                '1900-01-01T00:00:00.000Z',  # TODO: strange overflow under bottom bound for datetime64
            ],
        ]

        data_source_kind = EDataSourceKind.CLICKHOUSE

        return TestCase(
            name=self._name + '_ch_string',
            date_time_format=EDateTimeFormat.STRING_FORMAT,
            data_in=data_in,
            data_out_=data_out,
            select_what=SelectWhat.asterisk(schema.columns),
            select_where=None,
            data_source_kind=data_source_kind,
            schema=schema,
            database=Database.make_for_data_source_kind(data_source_kind),
            pragmas=dict(),
        )

    def _make_test_string_postgresql(self) -> TestCase:
        schema = Schema(
            columns=ColumnList(
                Column(
                    name='col_0_id',
                    ydb_type=Type.INT32,
                    data_source_type=DataSourceType(pg=postgresql.Int4()),
                ),
                # TODO: timestamp
                Column(
                    name='col_1_datetime64',
                    ydb_type=Type.TIMESTAMP,
                    data_source_type=DataSourceType(pg=postgresql.TimestampWithoutTimeZone()),
                ),
            ),
        )
        data_in = [
            [
                1,
                datetime.datetime(1950, 5, 27, 12, 23, 45, 678000),
            ],
            [2, datetime.datetime(1988, 11, 20, 12, 23, 45, 678000)],
            [
                3,
                datetime.datetime(2108, 1, 1, 12, 23, 45, 678000),
            ],
        ]

        data_out = [
            [
                1,
                '1950-05-27T12:23:45.678Z',
            ],
            [
                2,
                '1988-11-20T12:23:45.678Z',
            ],
            [
                3,
                '2108-01-01T12:23:45.678Z',
            ],
        ]

        data_source_kind = EDataSourceKind.POSTGRESQL

        return TestCase(
            name=self._name + '_pg_string',
            date_time_format=EDateTimeFormat.STRING_FORMAT,
            data_in=data_in,
            data_out_=data_out,
            select_what=SelectWhat.asterisk(schema.columns),
            select_where=None,
            data_source_kind=data_source_kind,
            schema=schema,
            database=Database.make_for_data_source_kind(data_source_kind),
            pragmas=dict(),
        )

    def make_test_cases(self) -> Sequence[TestCase]:
        return [
            self._make_test_clickhouse(),
            self._make_test_postgresql(),
            self._make_test_string_clickhouse(),
            self._make_test_string_postgresql(),
        ]
