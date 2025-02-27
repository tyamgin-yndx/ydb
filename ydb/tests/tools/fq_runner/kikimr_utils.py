import abc
from contextlib import contextmanager

import os
import pytest

from ydb.tests.tools.fq_runner.kikimr_runner import StreamingOverKikimr
from ydb.tests.tools.fq_runner.kikimr_runner import StreamingOverKikimrConfig
from ydb.tests.tools.fq_runner.kikimr_runner import TenantConfig
from ydb.tests.tools.fq_runner.kikimr_runner import TenantType
from ydb.tests.tools.fq_runner.kikimr_runner import YqTenant


class ExtensionPoint(abc.ABC):

    @abc.abstractmethod
    def is_applicable(self, request):
        ExtensionPoint.is_applicable.__annotations__ = {
            'request': pytest.FixtureRequest,
            'return': bool
        }
        pass

    def apply_to_kikimr_conf(self, request, configuration):
        ExtensionPoint.is_applicable.__annotations__ = {
            'request': pytest.FixtureRequest,
            'configuration': StreamingOverKikimrConfig,
            'return': None
        }
        pass

    def apply_to_kikimr(self, request, kikimr):
        ExtensionPoint.is_applicable.__annotations__ = {
            'request': pytest.FixtureRequest,
            'kikimr': StreamingOverKikimr,
            'return': None
        }
        pass


class AddInflightExtension(ExtensionPoint):
    def is_applicable(self, request):
        return (hasattr(request, 'param')
                and isinstance(request.param, dict)
                and "inflight" in request.param)

    def apply_to_kikimr(self, request, kikimr):
        kikimr.inflight = request.param["inflight"]
        kikimr.compute_plane.fq_config['read_actors_factory_config']['s3_read_actor_factory_config'][
            'max_inflight'] = kikimr.inflight
        del request.param["inflight"]


class AddDataInflightExtension(ExtensionPoint):
    def is_applicable(self, request):
        return (hasattr(request, 'param')
                and isinstance(request.param, dict)
                and "data_inflight" in request.param)

    def apply_to_kikimr(self, request, kikimr):
        kikimr.data_inflight = request.param["data_inflight"]
        kikimr.compute_plane.fq_config['read_actors_factory_config']['s3_read_actor_factory_config'][
            'data_inflight'] = kikimr.data_inflight
        del request.param["data_inflight"]


class AddFormatSizeLimitExtension(ExtensionPoint):
    def is_applicable(self, request):
        return (hasattr(request, 'param')
                and isinstance(request.param, dict)
                and len(request.param) != 0)

    def apply_to_kikimr(self, request, kikimr):
        s3 = {}
        s3['format_size_limit'] = []
        for name, limit in request.param.items():
            if name == "":
                s3['file_size_limit'] = limit
            else:
                s3['format_size_limit'].append(
                    {'name': name, 'file_size_limit': limit})
        kikimr.compute_plane.fq_config['gateways']['s3'] = s3  # v1
        kikimr.compute_plane.qs_config['s3'] = s3  # v2


class DefaultConfigExtension(ExtensionPoint):

    def __init__(self, s3_url):
        DefaultConfigExtension.__init__.__annotations__ = {
            's3_url': str,
            'return': None
        }
        super().__init__()
        self.s3_url = s3_url

    def is_applicable(self, request):
        return True

    def apply_to_kikimr(self, request, kikimr):
        kikimr.control_plane.fq_config['common']['object_storage_endpoint'] = self.s3_url
        if isinstance(kikimr.compute_plane, YqTenant):
            kikimr.compute_plane.fq_config['common']['object_storage_endpoint'] = self.s3_url
        kikimr.control_plane.fq_config['control_plane_storage']['retry_policy_mapping'] = [
            {
                'status_code': [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
                'policy': {
                    'retry_count': 0
                }
            }
        ]
        kikimr.control_plane.config_generator.yaml_config['metering_config'] = {
            'metering_file_path': 'metering.bill'}

        solomon_endpoint = os.environ.get('SOLOMON_URL')
        if solomon_endpoint is not None:
            kikimr.compute_plane.fq_config['common']['monitoring_endpoint'] = solomon_endpoint


class YQv2Extension(ExtensionPoint):

    def __init__(self, yq_version):
        YQv2Extension.__init__.__annotations__ = {
            'yq_version': str,
            'return': None
        }
        super().__init__()
        self.yq_version = yq_version

    def apply_to_kikimr_conf(self, request, configuration):
        if isinstance(configuration.node_count, dict):
            configuration.node_count["/compute"].tenant_type = TenantType.YDB
            configuration.node_count["/compute"].extra_feature_flags = ['enable_external_data_sources', 'enable_script_execution_operations']
            configuration.node_count["/compute"].extra_grpc_services = ['query_service']
        else:
            configuration.node_count = {
                "/cp": TenantConfig(node_count=1),
                "/compute": TenantConfig(node_count=1,
                                         tenant_type=TenantType.YDB,
                                         extra_feature_flags=[
                                             'enable_external_data_sources',
                                             'enable_script_execution_operations'
                                         ],
                                         extra_grpc_services=['query_service']),
            }

    def is_applicable(self, request):
        return self.yq_version == 'v2'

    def apply_to_kikimr(self, request, kikimr):
        kikimr.control_plane.fq_config['control_plane_storage']['enabled'] = True
        kikimr.control_plane.fq_config['compute'] = {
            'default_compute': 'IN_PLACE',
            'compute_mapping': [
                {
                    'query_type': 'ANALYTICS',
                    'compute': 'YDB',
                    'activation': {
                        'percentage': 100
                    }
                }
            ],
            'ydb': {
                'enable': 'true',
                'control_plane': {
                    'enable': 'true',
                    'single': {
                        'connection': {
                            'endpoint': kikimr.tenants["/compute"].endpoint(),
                            'database': '/local'
                        }
                    }
                }
            }
        }


class ComputeExtension(ExtensionPoint):
    def apply_to_kikimr_conf(self, request, configuration):
        if isinstance(configuration.node_count, dict):
            configuration.node_count["/compute"].node_count = request.param["compute"]
        else:
            configuration.node_count = {
                "/cp": TenantConfig(node_count=1),
                "/compute": TenantConfig(node_count=request.param["compute"]),
            }

    def is_applicable(self, request):
        return (hasattr(request, 'param')
                and isinstance(request.param, dict)
                and "compute" in request.param)

    def apply_to_kikimr(self, request, kikimr):
        kikimr.control_plane.fq_config['control_plane_storage']['mapping'] = {
            "common_tenant_name": ["/compute"]
        }
        del request.param["compute"]


class AuditExtension(ExtensionPoint):
    def is_applicable(self, request):
        return True

    def apply_to_kikimr(self, request, kikimr):
        ua_port = os.environ["UA_RECIPE_PORT"]
        kikimr.control_plane.fq_config['audit']['enabled'] = True
        kikimr.control_plane.fq_config['audit']['uaconfig']['uri'] = "localhost:{}".format(ua_port)


class StatsModeExtension(ExtensionPoint):

    def __init__(self, stats_mode):
        YQv2Extension.__init__.__annotations__ = {
            'stats_mode': str,
            'return': None
        }
        super().__init__()
        self.stats_mode = stats_mode

    def is_applicable(self, request):
        return self.stats_mode != ''

    def apply_to_kikimr(self, request, kikimr):
        kikimr.control_plane.fq_config['control_plane_storage']['stats_mode'] = self.stats_mode
        kikimr.control_plane.fq_config['control_plane_storage']['dump_raw_statistics'] = True


class BindingsModeExtension(ExtensionPoint):

    def __init__(self, bindings_mode, yq_version):
        YQv2Extension.__init__.__annotations__ = {
            'bindings_mode': str,
            'yq_version': str,
            'return': None
        }
        super().__init__()
        self.bindings_mode = bindings_mode
        self.yq_version = yq_version

    def is_applicable(self, request):
        return self.yq_version == 'v2' and self.bindings_mode != ''

    def apply_to_kikimr(self, request, kikimr):
        kikimr.compute_plane.config_generator.yaml_config["table_service_config"]["bindings_mode"] = self.bindings_mode


@contextmanager
def start_kikimr(request, kikimr_extensions):
    start_kikimr.__annotations__ = {
        'request': pytest.FixtureRequest,
        'kikimr_extensions': list[ExtensionPoint],
        'return': bool
    }
    kikimr_configuration = StreamingOverKikimrConfig(cloud_mode=True)

    for extension_point in kikimr_extensions:
        if extension_point.is_applicable(request):
            extension_point.apply_to_kikimr_conf(request, kikimr_configuration)

    kikimr = StreamingOverKikimr(kikimr_configuration)

    for extension_point in kikimr_extensions:
        if extension_point.is_applicable(request):
            extension_point.apply_to_kikimr(request, kikimr)

    kikimr.start_mvp_mock_server()
    try:
        kikimr.start()
        try:
            yield kikimr
        finally:
            kikimr.stop()
    finally:
        kikimr.stop_mvp_mock_server()


yq_v1 = pytest.mark.yq_version('v1')
yq_v2 = pytest.mark.yq_version('v2')
yq_all = pytest.mark.yq_version('v1', 'v2')
yq_stats_full = pytest.mark.stats_mode('STATS_MODE_FULL')
